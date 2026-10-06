"""Stage timers in benchmark-only header copies; no per-vertex instrumentation."""
import sys
from pathlib import Path


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f'Parser profiling anchor drift: {old[:100]!r}')
    return text.replace(old, new, 1)


def generate(root, output):
    output.mkdir(parents=True, exist_ok=True)
    common='''#include <chrono>
namespace parser_profile {
using Clock = std::chrono::steady_clock;
inline double scan_ms = 0, merge_ms = 0, allocate_ms = 0, copy_ms = 0;
inline size_t workers = 0, chunks = 0;
inline unsigned worker_limit = 0;
inline std::atomic<int64_t> input_ns = 0, decode_ns = 0;
inline int64_t nanoseconds(Clock::time_point begin) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-begin).count();
}
inline double elapsed(Clock::time_point begin) {
    return std::chrono::duration<double, std::milli>(Clock::now()-begin).count();
}
}
'''
    text=(root/'third_party/rapidobj/include/rapidobj/rapidobj.hpp').read_text(encoding='utf-8')
    # The public legacy API has no worker option. Override only this benchmark
    # copy so worker-count comparisons give both parsers the same budget.
    text=replace(text, '    auto num_threads           = std::thread::hardware_concurrency();',
        '    auto num_threads           = parser_profile::worker_limit ? parser_profile::worker_limit : std::thread::hardware_concurrency();')
    text=replace(text, '    // allocate attribute arrays',
        '    const auto allocation_started = parser_profile::Clock::now();\n    // allocate attribute arrays')
    text=replace(text, '    // compute tasks to construct attribute arrays',
        '    parser_profile::allocate_ms = parser_profile::elapsed(allocation_started);\n    // compute tasks to construct attribute arrays')
    text=replace(text, '    // perform merge\n', '    const auto copy_started = parser_profile::Clock::now();\n    // perform merge\n')
    text=replace(text, '    if (context->merging.error != rapidobj_errc::Success)',
        '    parser_profile::copy_ms = parser_profile::elapsed(copy_started);\n    if (context->merging.error != rapidobj_errc::Success)')
    first=text.index('inline Result ParseFile(const std::filesystem::path& filepath')
    last=text.index('inline Result ParseStream(std::istream& is', first)
    part=text[first:last]
    part=replace(part, '    // std::cout << DumpDebug(*context);', '''    parser_profile::scan_ms = std::chrono::duration<double, std::milli>(context->debug.parse.total_time).count();
    parser_profile::merge_ms = std::chrono::duration<double, std::milli>(context->debug.merge.total_time).count();
    parser_profile::workers = context->thread.concurrency;
    parser_profile::chunks = chunks.size();
    for (size_t i = 0; i < context->debug.io.submit_time.size(); ++i) {
        const auto io = context->debug.io.submit_time[i] + context->debug.io.wait_time[i];
        parser_profile::input_ns.fetch_add(io.count(), std::memory_order_relaxed);
        parser_profile::decode_ns.fetch_add((context->debug.parse.time[i]-io).count(), std::memory_order_relaxed);
    }''')
    text=text[:first]+part+text[last:]
    text=text.replace('namespace rapidobj {', common+'\nnamespace rapidobj {',1)
    (output/'rapidobj.hpp').write_text(text,encoding='utf-8')
    text=(root/'third_party/rapidobj/include/rapidobj/prototype.hpp').read_text(encoding='utf-8')
    text=replace(text, '    // The first physical line may start',
        '    const auto decode_started = parser_profile::Clock::now();\n    // The first physical line may start')
    text=replace(text, '    block.middle = std::move(scanner.chunk);',
        '    block.middle = std::move(scanner.chunk);\n    parser_profile::decode_ns.fetch_add(parser_profile::nanoseconds(decode_started), std::memory_order_relaxed);')
    text=replace(text, '    if (stopped.load(std::memory_order_relaxed)) { return {}; }',
        '    if (stopped.load(std::memory_order_relaxed)) { return {}; }\n    const auto input_started = parser_profile::Clock::now();')
    text=replace(text, '    return PrototypeParseBlock(std::move(block), offset == 0, context, options, stopped);',
        '    parser_profile::input_ns.fetch_add(parser_profile::nanoseconds(input_started), std::memory_order_relaxed);\n    return PrototypeParseBlock(std::move(block), offset == 0, context, options, stopped);')
    text=replace(text, '    // Range profiling starts here in the benchmark-only generated header.',
        '    const auto range_started = parser_profile::Clock::now();')
    text=replace(text, '    // Range profiling ends here in the benchmark-only generated header.', '''    const auto range_io = reader.SubmitTime() + reader.WaitTime();
    parser_profile::input_ns.fetch_add(range_io.count(), std::memory_order_relaxed);
    parser_profile::decode_ns.fetch_add(parser_profile::nanoseconds(range_started)-range_io.count(), std::memory_order_relaxed);''')
    text=replace(text, '    PrototypeResult result;\n    const bool names_only',
        '    const auto scan_started = parser_profile::Clock::now();\n    PrototypeResult result;\n    const bool names_only')
    text=replace(text, '        result.polygons = Merge(chunks, context);', '''        parser_profile::scan_ms = parser_profile::elapsed(scan_started);
        const auto merge_started = parser_profile::Clock::now();
        result.polygons = Merge(chunks, context);
        parser_profile::merge_ms = parser_profile::elapsed(merge_started);
        parser_profile::workers = result.stats.workers;
        parser_profile::chunks = result.stats.chunks;''')
    (output/'prototype.hpp').write_text(text,encoding='utf-8')


if __name__=='__main__':
    generate(Path(sys.argv[1]),Path(sys.argv[2]))
