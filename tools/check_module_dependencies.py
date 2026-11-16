"""Check named-module imports and textual project dependencies with Clang."""

from pathlib import Path
import json
import re
import tempfile

from check_systems import clean_arguments, compiler_owner, load_commands, module_name, run_process


def source_system(inventory, source):
    owner = inventory.files[source]
    for system in inventory.systems.values():
        if owner in {module_name(system.name), module_name(system.name, False)}:
            return system
    return None


def allowed_dependencies(inventory, source):
    system = source_system(inventory, source)
    if system is None:
        return set(inventory.legacy_dependencies)
    allowed = set(system.api_dependencies)
    if not source.is_relative_to(system.api):
        allowed.update(system.implementation_dependencies)
    return allowed


def validate_imports(inventory, source, rule):
    system = source_system(inventory, source)
    expected = compiler_owner(inventory, source)
    for provided in rule.get("provides", []):
        if not system or provided["logical-name"] != expected:
            raise ValueError(f"{source}: module ownership mismatch: {provided['logical-name']}")
    if source.suffix == ".cppm" and not rule.get("provides"):
        raise ValueError(f"{source}: module interface or partition declaration is missing")
    owners = {item.module: item.name for item in inventory.systems.values() if item.module}
    for dependency in rule.get("requires", []):
        name = dependency["logical-name"]
        primary = name.split(":", 1)[0]
        if system and primary == system.module:
            # Public interfaces must not import implementation partitions.
            if ":" in name and source.is_relative_to(system.api):
                partitions = dict(system.partitions)
                target = partitions.get(name.split(":", 1)[1])
                if target is None or not target.is_relative_to(system.api):
                    raise ValueError(f"{source}: API imports internal partition {name}")
            continue
        if ":" in name or primary not in owners or owners[primary] not in allowed_dependencies(inventory, source):
            raise ValueError(f"{source}: undeclared module dependency {name}")


def depfile_paths(text, directory, resolved=None):
    # The scanner uses a fixed target without a drive letter. Decode Make
    # escapes, but preserve ordinary Windows directory separators.
    body = text.replace("\\\n", "").partition(":")[2]
    resolved = {} if resolved is None else resolved
    for token in re.findall(r"(?:\\[ \t#\\:]|[^\s])+", body):
        token = re.sub(r"\\([ \t#\\:])", r"\1", token).replace("$$", "$")
        candidate = directory / token
        if candidate not in resolved:
            resolved[candidate] = candidate.resolve()
        yield resolved[candidate]


def validate_headers(inventory, source, dependencies):
    own = source_system(inventory, source)
    allowed = allowed_dependencies(inventory, source)
    for path in dependencies:
        if path == source or path not in inventory.files:
            continue
        target = source_system(inventory, path)
        if target is None:
            if own and "legacy" not in allowed:
                raise ValueError(f"{source}: undeclared header dependency on legacy: {path}")
            continue
        if path.suffix == ".cppm":
            raise ValueError(f"{source}: use import instead of including module source {path}")
        if own == target and not (source.is_relative_to(own.api) and path.is_relative_to(own.internal)):
            continue
        if path.is_relative_to(target.internal):
            raise ValueError(f"{source}: foreign internal header {path}")
        if target.name not in allowed:
            raise ValueError(f"{source}: undeclared header dependency on {target.name}: {path}")


def check_modules(inventory, output, configuration, llvm_root, jobs):
    records = load_commands(inventory, output / configuration)
    scanner = llvm_root / "bin" / ("clang-scan-deps.exe" if (llvm_root / "bin/clang-scan-deps.exe").exists() else "clang-scan-deps")
    with tempfile.TemporaryDirectory(prefix="woby module checks ") as directory:
        temporary = Path(directory).resolve()
        scan_records, objc_records, outputs = [], [], {}
        for index, record in enumerate(records):
            source = Path(record["file"]).resolve()
            # Objective-C++ stays behind header adapters in this trial.
            if source.suffix == ".mm":
                objc_records.append(dict(record, arguments=clean_arguments(record)
                                         + ["-Qunused-arguments", "-c", str(source)]))
                continue
            obj = temporary / f"{index}.obj"
            dep = temporary / f"{index}.d"
            args = clean_arguments(record)
            cl = Path(args[0]).stem == "clang-cl" or "--driver-mode=cl" in args
            flags = ["-MD", "-MF", str(dep), "-MT", "woby_dependency"]
            if cl:
                flags = ["/clang:" + flag for flag in flags]
            args += ["-Qunused-arguments", "-c", str(source), "-o", str(obj), *flags]
            scan_records.append(dict(record, arguments=args, output=str(obj)))
            outputs[str(obj)] = (source, dep, Path(record["directory"]))
        database = temporary / "commands.json"
        database.write_text(json.dumps(scan_records), encoding="utf-8")
        print(f"Scanning module and header dependencies for {len(scan_records)} C++ compilations.", flush=True)
        code, result, diagnostics = run_process([str(scanner), "-format=p1689", "-j", str(jobs),
                                               "-compilation-database=" + str(database)], inventory.root, timeout=900)
        if code:
            raise ValueError(diagnostics or result)
        if diagnostics:
            raise ValueError(f"Module scan must be warning-free:\n{diagnostics}")
        seen, resolved = set(), {}
        for rule in json.loads(result)["rules"]:
            key = rule["primary-output"]
            if key not in outputs or key in seen:
                raise ValueError(f"Unexpected or duplicate module scan output: {key}")
            seen.add(key)
            source, dep, base = outputs[key]
            validate_imports(inventory, source, rule)
            validate_headers(inventory, source, depfile_paths(dep.read_text(encoding="utf-8"), base, resolved))
        if seen != set(outputs):
            raise ValueError("Module scan did not cover all configured C++ sources")
        if objc_records:
            database.write_text(json.dumps(objc_records), encoding="utf-8")
            code, result, diagnostics = run_process([str(scanner), "-format=experimental-full", "-j", str(jobs),
                                                   "-compilation-database=" + str(database)], inventory.root, timeout=900)
            if code or diagnostics:
                raise ValueError(diagnostics or result)
            objc_seen = set()
            for unit in json.loads(result)["translation-units"]:
                for command in unit["commands"]:
                    source = Path(command["input-file"]).resolve()
                    objc_seen.add(source)
                    validate_headers(inventory, source, [Path(path).resolve() for path in command["file-deps"]])
            if objc_seen != {Path(record["file"]).resolve() for record in objc_records}:
                raise ValueError("Header scan did not cover all configured Objective-C++ sources")
        print(f"Module boundaries: {len(seen)} source compilations passed.")
    return 0
