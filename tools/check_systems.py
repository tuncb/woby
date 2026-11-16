"""Generate Clang system boundaries and check APIs and declaration references."""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import tomllib


HEADERS = {".h", ".hpp", ".hh", ".hxx", ".inl", ".inc"}
SOURCES = {".cpp", ".cc", ".cxx", ".mm", ".cppm"}
LEGACY = "WobyLegacy"


@dataclass(frozen=True)
class System:
    name: str
    root: Path
    api: Path
    internal: Path
    api_dependencies: tuple[str, ...]
    implementation_dependencies: tuple[str, ...]
    tests: tuple[Path, ...]
    module: str = ""
    partitions: tuple[tuple[str, Path], ...] = ()


@dataclass
class Inventory:
    root: Path
    systems: dict[str, System]
    legacy_dependencies: tuple[str, ...]
    files: dict[Path, str]
    roots: tuple[Path, ...]


def module_name(name: str, api: bool = True) -> str:
    return LEGACY if name == "legacy" else "Woby_" + name + ("_Api" if api else "_Impl")


def project_path(root: Path, value: str) -> Path:
    if not isinstance(value, str) or not value or Path(value).is_absolute():
        raise ValueError(f"Expected a relative project path, got {value!r}")
    path = (root / value).resolve()
    if not path.is_relative_to(root) or path == root:
        raise ValueError(f"Path must stay below the project root: {value}")
    if not path.exists():
        raise ValueError(f"Project path does not exist: {value}")
    return path


def load_inventory(root: Path) -> Inventory:
    root = root.resolve()
    with (root / "systems.toml").open("rb") as stream:
        config = tomllib.load(stream)
    if config.get("version") != 1:
        raise ValueError("systems.toml must use version = 1")
    systems = {}
    for name, spec in config.get("systems", {}).items():
        if name == "legacy" or not re.fullmatch(r"[a-z][a-z0-9_]*", name):
            raise ValueError(f"Invalid system name: {name}")
        base = project_path(root, spec["root"])
        api = project_path(base, spec["api"])
        internal = project_path(base, spec["internal"])
        if api.is_relative_to(internal) or internal.is_relative_to(api):
            raise ValueError(f"API and internal directories overlap: {name}")
        if not all(path.is_dir() for path in (base, api, internal)):
            raise ValueError(f"System paths must be directories: {name}")
        systems[name] = System(name, base, api, internal,
                               tuple(spec.get("api_dependencies", [])),
                               tuple(spec.get("implementation_dependencies", [])),
                               tuple(project_path(root, value) for value in spec.get("tests", [])),
                               spec.get("module", ""),
                               tuple((part, project_path(base, path)) for part, path in spec.get("partitions", {}).items()))
    if not systems:
        raise ValueError("Declare at least one system")
    legacy = config["legacy"]
    names = set()
    for system in systems.values():
        if system.module:
            if not re.fullmatch(r"[A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*", system.module) or system.module in names:
                raise ValueError(f"Invalid or duplicate module name: {system.module}")
            names.add(system.module)
        elif system.partitions:
            raise ValueError(f"Partitions require a named module: {system.name}")
        paths = set()
        for name, path in system.partitions:
            if (not re.fullmatch(r"[A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*", name)
                    or path in paths or path.suffix != ".cppm"
                    or not (path.is_relative_to(system.api) or path.is_relative_to(system.internal))):
                raise ValueError(f"Invalid module partition: {system.name}:{name}")
            paths.add(path)
    dependencies = tuple(legacy.get("dependencies", []))
    valid = set(systems) | {"legacy"}
    for name, values in [("legacy", dependencies)] + [
            (system.name, system.api_dependencies + system.implementation_dependencies)
            for system in systems.values()]:
        if set(values) - valid or name in values:
            raise ValueError(f"Unknown or self dependency for {name}: {values}")
    # API cycles cannot be fixed by including more headers. Implementation edges
    # point at APIs, so mutually dependent implementations need not form a cycle.
    graph = {module_name(name): [module_name(dep) for dep in system.api_dependencies]
             for name, system in systems.items()}
    graph[LEGACY] = [module_name(dep) for dep in dependencies]
    visited, active = set(), []

    def visit(node):
        if node in active:
            raise ValueError("API dependency cycle: " + " -> ".join(active + [node]))
        if node in visited:
            return
        active.append(node)
        for target in graph[node]:
            visit(target)
        active.pop()
        visited.add(node)

    for node in graph:
        visit(node)
    roots = [project_path(root, value) for value in legacy["roots"]]
    test_owners = {}
    for system in systems.values():
        for other in systems.values():
            if system != other and system.root.is_relative_to(other.root):
                raise ValueError(f"System roots overlap: {system.name}, {other.name}")
        for test in system.tests:
            if test.suffix not in SOURCES or test in test_owners or not test.is_file():
                raise ValueError(f"Invalid or duplicate test owner: {test}")
            test_owners[test] = module_name(system.name, False)
    files = {}
    for directory in roots + [system.root for system in systems.values()]:
        for path in sorted(directory.rglob("*")):
            if not path.is_file() or path.suffix not in HEADERS | SOURCES:
                continue
            resolved = path.resolve()
            if not resolved.is_relative_to(root) or resolved != path:
                raise ValueError(f"Source links are not supported in checked roots: {path}")
            owner = LEGACY
            for system in systems.values():
                if path.is_relative_to(system.api):
                    if path.suffix not in HEADERS and not (system.module and path.suffix == ".cppm"):
                        raise ValueError(f"API directories must contain headers, not source files: {path}")
                    owner = module_name(system.name)
                elif path.is_relative_to(system.internal):
                    owner = module_name(system.name, False)
                elif path.is_relative_to(system.root):
                    raise ValueError(f"Unclassified system file: {path}")
            if path in test_owners:
                if owner != LEGACY:
                    raise ValueError(f"Test ownership overlaps a system directory: {path}")
                owner = test_owners[path]
            files[path] = owner
    if set(test_owners) - set(files):
        raise ValueError("System test files must be inside a checked legacy root")
    return Inventory(root, systems, dependencies, files, tuple(sorted(set(roots + [
        system.root for system in systems.values()]))))


def compiler_owner(inventory: Inventory, path: Path) -> str:
    owner = inventory.files[path]
    for system in inventory.systems.values():
        if system.module and owner in {module_name(system.name), module_name(system.name, False)}:
            for partition, source in system.partitions:
                if source == path:
                    return system.module + ":" + partition
            return system.module
    return owner


def path_pattern(path: Path, directory: bool = False) -> str:
    # SourceManager can retain either slash spelling and Windows filename case.
    result = ""
    for char in path.as_posix():
        if char == "/":
            result += r"[/\\]"
        elif os.name == "nt" and char.isascii() and char.isalpha():
            result += "[" + char.lower() + char.upper() + "]"
        else:
            result += re.escape(char)
    return "^" + result + (r"[/\\]" if directory else "$")


def tidy_config(inventory: Inventory, aliases: dict[Path, set[Path]] | None = None) -> dict:
    aliases = aliases or {}

    def patterns(paths):
        return "(" + "|".join(path_pattern(spelling) for path in sorted(paths)
                                for spelling in sorted({path} | aliases.get(path, set()))) + ")"

    def quoted(value):
        # The dynamic matcher parser keeps regex backslashes verbatim. JSON
        # escaping belongs only to the outer config file, not to Query strings.
        if '"' in value:
            raise ValueError("Clang matcher paths cannot contain double quotation marks")
        return '"' + value + '"'

    checks = []
    for system in inventory.systems.values():
        private_files = {path for path in inventory.files if path.is_relative_to(system.internal)}
        if not private_files:
            continue
        internal = quoted(patterns(private_files))
        allowed = quoted(patterns(private_files | set(system.tests)))
        declaration = f"namedDecl(isExpansionInFileMatching({internal}))"
        # Implicit AST nodes can have no source location. Check only uses that
        # have a location in this project, not synthesized library nodes.
        outside = (f"isExpansionInFileMatching({quoted(path_pattern(inventory.root, True))}), "
                   f"unless(isExpansionInFileMatching({allowed}))")
        matchers = {
            "declaration": f"declRefExpr(to({declaration}), {outside})",
            "member": f"memberExpr(member({declaration}), {outside})",
            "type": f"typeLoc(loc(qualType(hasDeclaration({declaration}))), {outside})",
        }
        for kind, matcher in matchers.items():
            checks.append({"Name": f"woby-{system.name}-{kind}",
                           "Query": f'match {matcher}.bind("use")',
                           "Diagnostic": [{"BindName": "use", "Level": "Warning",
                                           "Message": f"Use the {system.name} API; this declaration is internal."}]})
    return {"Checks": "-*,custom-woby-*", "WarningsAsErrors": "custom-woby-*",
            "HeaderFilterRegex": path_pattern(inventory.root, True), "CustomChecks": checks}


def write_changed(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text(encoding="utf-8") != content:
        path.write_text(content, encoding="utf-8", newline="\n")


def generate(inventory: Inventory, output: Path) -> None:
    modules = {LEGACY: [module_name(dep) for dep in inventory.legacy_dependencies]}
    for system in inventory.systems.values():
        modules[module_name(system.name)] = [module_name(dep) for dep in system.api_dependencies]
        modules[module_name(system.name, False)] = [module_name(system.name)] + [
            module_name(dep) for dep in system.implementation_dependencies]
    lines = []
    for name, dependencies in modules.items():
        lines.append(f"module {name} {{")
        for path, owner in sorted(inventory.files.items()):
            # Implementation sources also have owners when included as text.
            if owner == name and (path.suffix in HEADERS or name.endswith("_Impl")):
                private = "private " if name.endswith("_Impl") else ""
                lines.append(f"  {private}textual header {json.dumps(path.as_posix(), ensure_ascii=False)}")
        lines.extend(f"  use {dependency}" for dependency in sorted(set(dependencies)))
        lines.append("}")
    write_changed(output / "systems.modulemap", "\n".join(lines) + "\n")
    write_changed(output / "clang-tidy.json", json.dumps(tidy_config(inventory), indent=2) + "\n")
    data = {"sources": {path.as_posix(): compiler_owner(inventory, path) for path in sorted(inventory.files)
                        if path.suffix in SOURCES},
            "roots": [path.as_posix() for path in inventory.roots]}
    write_changed(output / "inventory.json", json.dumps(data, indent=2) + "\n")


def clean_arguments(record: dict, *, api: bool = False) -> list[str]:
    """Remove outputs and capture flags; preserve compiler and platform semantics."""
    arguments = record["arguments"]
    if not isinstance(arguments, list) or not arguments or not all(isinstance(arg, str) for arg in arguments):
        raise ValueError("Compilation records must contain an arguments array")
    source = (Path(record["directory"]) / record["file"]).resolve()
    cl = "--driver-mode=cl" in arguments or Path(arguments[0]).stem == "clang-cl"
    result = [arguments[0]]
    i = 1
    while i < len(arguments):
        arg = arguments[i]
        value = arg.removeprefix("/clang:")
        if value in {"-MJ", "-gen-cdb-fragment-path", "--gen-cdb-fragment-path",
                     "-o", "-MF", "-MT", "-MQ", "-serialize-diagnostics"}:
            i += 2
            continue
        if value.startswith("-fmodule-output="):
            i += 1
            continue
        if value.startswith(("-MJ", "-gen-cdb-fragment-path=", "--gen-cdb-fragment-path=")):
            i += 1
            continue
        if arg in {"/Fo", "/Fd", "/Fa", "/Fp", "/Yu", "/Yc"}:
            i += 2
            continue
        if arg.startswith(("/Fo", "/Fd", "/Fa", "/Fp", "/Yu", "/Yc")):
            i += 1
            continue
        if value in {"-c", "/c", "-MD", "-MMD", "-MP"}:
            i += 1
            continue
        # CMake's Ninja PCH uses forwarded cc1 arguments on Clang.
        if value == "-Xclang" and i + 1 < len(arguments):
            forwarded = arguments[i + 1].removeprefix("/clang:")
            if forwarded == "-include-pch" or (api and forwarded in {"-include", "-imacros"}):
                i += 4 if i + 2 < len(arguments) and arguments[i + 2].removeprefix("/clang:") == "-Xclang" else 3
                continue
            if forwarded == "-emit-pch":
                i += 2
                continue
            result.extend(arguments[i:i + 2])
            i += 2
            continue
        if value == "-include-pch":
            i += 2
            continue
        if api and (value in {"-include", "-imacros"} or arg == "/FI"):
            i += 2
            continue
        if api and (arg.startswith("/FI") or value.startswith(("-fmodule-name=", "-fmodule-map-file="))):
            i += 1
            continue
        # clang-cl fragments contain normalized GNU spellings as well as
        # forwarded flags. Replay those through the clang-cl driver explicitly.
        if cl and arg == "-include":
            result.append("/FI" + arguments[i + 1])
            i += 2
            continue
        if cl and arg == "/TP" and "-xc++" in arguments:
            i += 1
            continue
        if not arg.startswith("-") and (Path(record["directory"]) / arg).resolve() == source:
            i += 1
            continue
        result.append("/clang:" + arg if cl and arg.startswith("-f") else arg)
        i += 1
    return result


def read_fragments(path: Path) -> list[dict]:
    text = path.read_text(encoding="utf-8").strip().removesuffix(",")
    # Clang's fragment writer uses C byte escapes for non-ASCII bytes. Convert
    # those escapes without changing escaped backslashes in literal filenames.
    pieces, byte_escapes = [], False
    index = 0
    while index < len(text):
        if text[index:index + 2] == "\\\\":
            pieces.append("\\\\")
            index += 2
        elif text[index:index + 2] == "\\x" and re.fullmatch(r"[0-9a-fA-F]{2}", text[index + 2:index + 4]):
            pieces.append("\\u00" + text[index + 2:index + 4])
            byte_escapes = True
            index += 4
        else:
            pieces.append(text[index])
            index += 1
    # A clang-cl PCH invocation writes both the PCH and object records into
    # one fragment. Each record ends with a comma, as with single-source files.
    records = json.loads("[" + "".join(pieces) + "]")

    def decode(value):
        if isinstance(value, str) and byte_escapes:
            try:
                raw = value.encode("latin1")
            except UnicodeEncodeError:
                return value
            try:
                return raw.decode("utf-8")
            except UnicodeDecodeError:
                return raw.decode("mbcs") if os.name == "nt" else value
        if isinstance(value, list):
            return [decode(item) for item in value]
        if isinstance(value, dict):
            return {key: decode(item) for key, item in value.items()}
        return value

    return decode(records)


def load_commands(inventory: Inventory, directory: Path) -> list[dict]:
    targets_file = directory / "targets.txt"
    if not targets_file.exists():
        raise ValueError(f"Configure CMake first; missing {targets_file}")
    records = []
    for target in targets_file.read_text(encoding="utf-8").splitlines():
        expected = {Path(line).resolve() for line in (directory / f"sources-{target}.txt").read_text(
            encoding="utf-8").splitlines() if line}
        latest = {}
        for path in (directory / "commands" / target).glob("*.json"):
            stamp = path.stat().st_mtime_ns
            for record in read_fragments(path):
                source = (Path(record["directory"]) / record["file"]).resolve()
                if source not in expected:
                    continue  # Includes PCHs and sources removed from the target.
                if source not in latest or stamp > latest[source][0]:
                    latest[source] = (stamp, record)
        missing = expected - set(latest)
        if missing:
            raise ValueError(f"Build {target} in this configuration first; missing commands for " +
                             ", ".join(str(path) for path in sorted(missing)))
        for source, (_, record) in sorted(latest.items()):
            if source not in inventory.files:
                raise ValueError(f"Reconfigure CMake; source ownership changed: {source}")
            record["file"] = str(source)
            arguments = clean_arguments(record)
            owners = {arg.removeprefix("/clang:").removeprefix("-fmodule-name=") for arg in arguments
                      if arg.removeprefix("/clang:").startswith("-fmodule-name=")}
            if owners != {compiler_owner(inventory, source)}:
                raise ValueError(f"Reconfigure and build {target}; source ownership changed: {source}")
            record["arguments"] = arguments + [str(source)]
            records.append(record)
    if not records:
        raise ValueError("No checked compilation commands were found")
    return records


def run_process(arguments: list[str], directory: Path, timeout: int = 180) -> tuple[int, str, str]:
    try:
        result = subprocess.run(arguments, cwd=directory, text=True, encoding="utf-8", errors="replace",
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
        return result.returncode, result.stdout, result.stderr
    except subprocess.TimeoutExpired as error:
        return 1, "", f"Command timed out: {arguments[0]} ({directory})\n{error}"


def run_command(arguments: list[str], directory: Path) -> tuple[int, str]:
    code, output, errors = run_process(arguments, directory)
    return code, output + errors


def scan_inputs(inventory: Inventory, records: list[dict], scanner: Path, temporary: Path,
                output: Path, jobs: int) -> tuple[set[str], str]:
    """Resolve actual Clang header spellings before building path-based matchers.

    SourceManager retains paths such as api/../internal/file.h. A regex against
    only the canonical directory would silently miss declarations in that file.
    The dependency scanner gives us those spellings and selects the TUs which
    can reference internal declarations. It still scans every configured source.
    """
    database = temporary / "scan_commands.json"
    # Scanning does not generate code, so optimization options can be unused.
    scan_records = [dict(record, arguments=clean_arguments(record) + ["-Qunused-arguments", "-c", record["file"]])
                    for record in records]
    database.write_text(json.dumps(scan_records), encoding="utf-8")
    code, text, diagnostics = run_process([str(scanner), "-compilation-database=" + str(database),
                                          "-format=experimental-full", "-j", str(jobs)], inventory.root)
    if code:
        return set(), diagnostics or text or f"Clang dependency scan failed with exit code {code}."
    if diagnostics:
        print(diagnostics, file=sys.stderr, end="", flush=True)
    data = json.loads(text)
    aliases = {}
    affected = set()
    scanned = set()
    resolved_paths = {}
    for unit in data["translation-units"]:
        for command in unit["commands"]:
            source = Path(command["input-file"]).resolve()
            if source not in inventory.files:
                raise ValueError(f"Dependency scan returned an unowned source: {source}")
            scanned.add(source)
            for spelling in command["file-deps"]:
                path = Path(spelling)
                if spelling not in resolved_paths:
                    resolved_paths[spelling] = path.resolve()
                resolved = resolved_paths[spelling]
                if resolved in inventory.files:
                    aliases.setdefault(resolved, set()).add(path)
                    if inventory.files[resolved].endswith("_Impl"):
                        affected.add(str(source))
    expected = {Path(record["file"]).resolve() for record in records}
    if scanned != expected:
        raise ValueError("Dependency scan did not cover the configured source files")
    write_changed(output / "clang-tidy.json", json.dumps(tidy_config(inventory, aliases), indent=2) + "\n")
    return affected, ""


def boundary_flags(module: str, module_map: Path, cl: bool) -> list[str]:
    prefix = "/clang:" if cl else ""
    return [prefix + "-fmodules-decluse", prefix + "-Werror=private-header",
            prefix + "-fmodule-name=" + module, prefix + "-fmodule-map-file=" + str(module_map)]


def api_checks(inventory: Inventory, records: list[dict], output: Path, temporary: Path) -> list[tuple[list[str], Path]]:
    commands = []
    for system in inventory.systems.values():
        candidates = [record for record in records
                      if inventory.files[Path(record["file"]).resolve()] == module_name(system.name, False)]
        if not candidates:
            raise ValueError(f"No implementation command is available to check the {system.name} API")
        record = candidates[0]
        for index, header in enumerate(sorted(path for path in inventory.files if path.is_relative_to(system.api))):
            source = temporary / f"{system.name}-{index}.cpp"
            source.write_text(f"#include {json.dumps(header.as_posix(), ensure_ascii=False)}\n", encoding="utf-8")
            arguments = clean_arguments(record, api=True)
            cl = any(arg == "--driver-mode=cl" for arg in arguments) or Path(arguments[0]).stem == "clang-cl"
            arguments += boundary_flags(module_name(system.name), output / "systems.modulemap", cl)
            arguments += ["-Qunused-arguments"]
            arguments += (["/Zs", "/WX", "/Y-"] if cl else ["-fsyntax-only", "-Werror"])
            commands.append((arguments + [str(source)], Path(record["directory"])))
    return commands


def check(inventory: Inventory, output: Path, configuration: str, llvm_root: Path, jobs: int) -> int:
    generate(inventory, output)
    if any(system.module for system in inventory.systems.values()):
        from check_module_dependencies import check_modules
        return check_modules(inventory, output, configuration, llvm_root, jobs)
    directory = output / configuration
    records = load_commands(inventory, directory)
    tidy = llvm_root / "bin" / ("clang-tidy.exe" if os.name == "nt" else "clang-tidy")
    scanner = llvm_root / "bin" / ("clang-scan-deps.exe" if os.name == "nt" else "clang-scan-deps")
    for tool in (tidy, scanner):
        if not tool.is_file():
            raise ValueError(f"Install the analysis tools from the pinned LLVM distribution: {tool}")
    # Temporary files remain outside the source tree. All paths in the database
    # retain their original absolute bases, including when this is another drive.
    with tempfile.TemporaryDirectory(prefix="woby system checks ") as name:
        temporary = Path(name).resolve()
        (temporary / "compile_commands.json").write_text(json.dumps(records), encoding="utf-8")
        print(f"Scanning {len(records)} compilation commands with Clang.", flush=True)
        affected, error = scan_inputs(inventory, records, scanner, temporary, output, jobs)
        if error:
            print(error)
            return 1
        tasks = api_checks(inventory, records, output, temporary)
        for source in sorted(affected):
            tasks.append(([str(tidy), "--experimental-custom-checks", "--config-file=" + str(output / "clang-tidy.json"),
                           "--extra-arg=-Qunused-arguments", "-p", str(temporary), source], inventory.root))
        failed = 0
        print(f"Checking {len(tasks)} API headers and source files with Clang.", flush=True)
        with ThreadPoolExecutor(max_workers=jobs) as executor:
            futures = {executor.submit(run_command, arguments, cwd): arguments[-1] for arguments, cwd in tasks}
            for index, future in enumerate(as_completed(futures), 1):
                code, text = future.result()
                if code:
                    failed += 1
                    print(f"Failed: {futures[future]}", flush=True)
                    print(text, flush=True)
                if index % 25 == 0:
                    print(f"Checked {index}/{len(tasks)}.", flush=True)
        print(f"System checks: {len(tasks) - failed} passed, {failed} failed.")
        return 1 if failed else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["generate", "check"])
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--build", type=Path, required=True, help="CMake build directory")
    parser.add_argument("--config", default="Release")
    parser.add_argument("--llvm-root", type=Path, default=Path(os.environ.get("WOBY_LLVM_ROOT", "C:/Program Files/LLVM")))
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if not re.fullmatch(r"[A-Za-z0-9_-]+", args.config):
        parser.error("--config must be a configuration name")
    try:
        inventory = load_inventory(args.root)
        output = args.build.resolve() / "systems"
        if args.command == "generate":
            generate(inventory, output)
            return 0
        return check(inventory, output, args.config, args.llvm_root.resolve(), args.jobs)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"System check error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
