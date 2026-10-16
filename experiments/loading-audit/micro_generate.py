"""Count rare buffer growth operations in a separate generated parser target."""
from pathlib import Path
import sys


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError("Allocation audit anchor drift: " + old[:100])
    return text.replace(old, new, 1)


def generate(root, source, output):
    output.mkdir(parents=True, exist_ok=True)
    text = (source / "rapidobj.hpp").read_text(encoding="utf-8")
    text = '#include "parser_micro.hpp"\n' + text
    text = replace(text, "Buffer(size_t size) noexcept : m_size(size), m_room(0), m_data(new T[size]) {}",
        "Buffer(size_t size) noexcept : m_size(size), m_room(0), m_data(new T[size]) { parser_micro::growth<T>(0, size, false); }")
    text = replace(text, "            m_room = cap - m_size;",
        "            parser_micro::growth<T>(m_size, cap, static_cast<bool>(src));\n            m_room = cap - m_size;")
    (output / "rapidobj.hpp").write_text(text, encoding="utf-8")
    text = (source / "prototype.hpp").read_text(encoding="utf-8")
    if "    const auto range_started = parser_profile::Clock::now();" in text:
        text = replace(text, "    const auto range_started = parser_profile::Clock::now();",
            "    const auto range_started = parser_profile::Clock::now();\n    const auto micro_cpu_start = parser_micro::cpu_ticks();")
        text = replace(text, "    result.middle = std::move(scanner.chunk);", """    const auto micro_io = reader.SubmitTime() + reader.WaitTime();
    parser_micro::range_record(begin, size, range_started, micro_cpu_start,
        std::chrono::duration<double, std::milli>(micro_io).count(), scanner.chunk.parsed.positions.count,
        scanner.chunk.parsed.texcoords.count, scanner.chunk.parsed.mesh.faces.count,
        scanner.chunk.parsed.mesh.indices.buffer.size());
    result.middle = std::move(scanner.chunk);""")
    else:
        # Persistent workers process several blocks. Reset their local counters
        # for each block so the range records do not count earlier blocks again.
        text = replace(text, "    const auto input_started = parser_profile::Clock::now();",
            "    const auto input_started = parser_profile::Clock::now();\n"
            "    parser_micro::local = {};\n    const auto micro_cpu_start = parser_micro::cpu_ticks();")
        text = replace(text, "    return PrototypeParseBlock(std::move(block), offset == 0, context, options, stopped);", """    const auto micro_io = reader.SubmitTime() + reader.WaitTime();
    block = PrototypeParseBlock(std::move(block), offset == 0, context, options, stopped);
    parser_micro::range_record(offset, size, input_started, micro_cpu_start,
        std::chrono::duration<double, std::milli>(micro_io).count(), block.middle.parsed.positions.count,
        block.middle.parsed.texcoords.count, block.middle.parsed.mesh.faces.count,
        block.middle.parsed.mesh.indices.buffer.size());
    return block;""")
    (output / "prototype.hpp").write_text(text, encoding="utf-8")
    text = (root / "experiments/parser-throughput/benchmark.cpp").read_text(encoding="utf-8")
    text = replace(text, "        std::cout << output.dump() << '\\n';",
        "        output[\"allocation_audit\"] = parser_micro::snapshot();\n        std::cout << output.dump() << '\\n';")
    (output / "benchmark.cpp").write_text(text, encoding="utf-8")


if __name__ == "__main__":
    generate(*(Path(arg).resolve() for arg in sys.argv[1:]))
