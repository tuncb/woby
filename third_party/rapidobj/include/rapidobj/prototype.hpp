// Experimental Woby extension to the MIT-licensed vendored RapidOBJ.
// Keeps the existing polygon decoder, index reconciliation and triangulator.
#pragma once
#include "rapidobj.hpp"
#include <deque>
#include <limits>

namespace rapidobj {

enum class StatementKind {
    ordinary, group, materialLibrary, type, degree, parameterVertex,
    curve, parameterCurve, surface, knots, end, trim, hole, unsupported
};

struct ControlReference {
    int64_t position = 0, texcoord = 0, normal = 0;
    bool attributes = false;
};

// Numbers and references are decoded once. Counts mean declarations preceding
// this statement, not final table sizes, including when workers split a body.
struct FreeformStatement {
    StatementKind kind = StatementKind::ordinary;
    size_t line = 0, positions = 0, texcoords = 0, normals = 0;
    std::string name;
    bool rational = false;
    std::vector<double> values;
    std::vector<ControlReference> references;
};
struct PositionWeight { size_t position; double weight; };

struct PrototypeOptions {
    size_t chunk_bytes = 4 * 1024 * 1024; // raw blocks scanned by workers
    size_t read_bytes = 256 * 1024; // individual reads from non-seekable streams
    size_t workers = 0; // zero selects hardware concurrency, capped at 16
    size_t max_statement_bytes = 1024 * 1024;
    bool material_names_only = false; // retain deterministic face IDs without resolving any MTL data
    // Called only on the calling thread. May throw to cancel; outstanding
    // workers are joined before unwinding. No callback or buffer is retained.
    void (*checkpoint)(void*) = nullptr;
    void* user = nullptr;
};
struct PrototypeStats {
    size_t input_bytes = 0, chunks = 0, workers = 0;
    size_t peak_inflight_text_bytes = 0, parsed_chunk_bytes = 0;
    size_t position_copy_bytes_avoided = 0;
};
struct PrototypeResult {
    Result polygons;
    std::vector<FreeformStatement> statements;
    std::vector<PositionWeight> weights; // sorted, only explicitly nonunit weights
    PrototypeStats stats;
};

namespace detail {

inline void PrototypeCheckpoint(const PrototypeOptions& options) {
    if (options.checkpoint) { options.checkpoint(options.user); }
}
inline std::string_view PrototypeToken(std::string_view& text) {
    TrimLeft(text);
    size_t end = 0;
    while (end < text.size() && text[end] != ' ' && text[end] != '\t' && text[end] != '\r') { ++end; }
    const auto token = text.substr(0, end);
    text.remove_prefix(end);
    return token;
}
// Let the numeric decoder find the end once, then validate its delimiter.
inline bool PrototypeNextNumber(std::string_view& text, double& value, bool finite) {
    if (!text.empty() && text.front() == '+') { text.remove_prefix(1); }
    if (text.empty()) { return false; }
    const auto parsed = fast_float::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || (finite && !std::isfinite(value))) { return false; }
    if (parsed.ptr != text.data() + text.size() && *parsed.ptr != ' ' && *parsed.ptr != '\t' && *parsed.ptr != '\r') {
        return false;
    }
    text.remove_prefix(static_cast<size_t>(parsed.ptr - text.data()));
    return true;
}
inline bool PrototypeNumber(std::string_view text, double& value, bool finite = true) {
    if (!text.empty() && text.front() == '+') { text.remove_prefix(1); }
    if (text.empty()) { return false; }
    const auto parsed = fast_float::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()
        && (!finite || std::isfinite(value));
}
inline bool PrototypeInteger(std::string_view text, int64_t& value) {
    if (!text.empty() && text.front() == '+') { text.remove_prefix(1); }
    if (text.empty()) { return false; }
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value != 0;
}
inline bool PrototypeReference(std::string_view text, ControlReference& ref) {
    const auto first = text.find('/');
    if (!PrototypeInteger(text.substr(0, first), ref.position)) { return false; }
    if (first == std::string_view::npos) { return true; }
    ref.attributes = true;
    text.remove_prefix(first + 1);
    const auto second = text.find('/');
    const auto uv = text.substr(0, second);
    if (!uv.empty() && !PrototypeInteger(uv, ref.texcoord)) { return false; }
    if (second == std::string_view::npos) { return !uv.empty(); }
    return PrototypeInteger(text.substr(second + 1), ref.normal);
}
inline bool PrototypeValues(std::string_view text, std::vector<double>& values) {
    for (auto token = PrototypeToken(text); !token.empty(); token = PrototypeToken(text)) {
        double value = 0;
        if (!PrototypeNumber(token, value)) { return false; }
        values.push_back(value);
    }
    return true;
}
inline void PrototypeOrdinary(std::vector<FreeformStatement>& records, size_t line) {
    if (records.empty() || records.back().kind != StatementKind::ordinary) {
        records.push_back({});
        records.back().line = line;
    }
}

struct PrototypeChunk {
    Chunk parsed;
    std::vector<FreeformStatement> statements;
    std::vector<PositionWeight> weights;
};

inline rapidobj_errc PrototypeLine(std::string_view line, size_t line_number,
    PrototypeChunk& out, SharedContext& context) {
    line = line.substr(0, line.find('#'));
    Trim(line);
    if (line.empty()) { return rapidobj_errc::Success; }
    auto text = line;
    const auto key = PrototypeToken(text);
    auto& chunk = out.parsed;
    auto& records = out.statements;

    if (key == "v" || key == "vt" || key == "vn") {
        PrototypeOrdinary(records, line_number);
        std::array<double, 7> values{};
        size_t count = 0;
        for (;;) {
            TrimLeft(text);
            if (text.empty()) { break; }
            if (count == values.size() || !PrototypeNextNumber(text, values[count++], key == "v")) {
                return rapidobj_errc::ParseError;
            }
        }
        if (key == "v") {
            if (count != 3 && count != 4 && count != 6 && count != 7) { return rapidobj_errc::ParseError; }
            chunk.positions.buffer.ensure_enough_room_for(3);
            for (size_t k = 0; k < 3; ++k) { chunk.positions.buffer.push_back(values[k]); }
            if (count == 4 && values[3] != 1) { out.weights.push_back({chunk.positions.count, values[3]}); }
            ++chunk.positions.count;
        } else if (key == "vt") {
            if (count < 1 || count > 3) { return rapidobj_errc::ParseError; }
            chunk.precise_texcoords.ensure_enough_room_for(2);
            chunk.precise_texcoords.push_back(values[0]);
            chunk.precise_texcoords.push_back(values[1]);
            ++chunk.texcoords.count;
        } else {
            if (count != 3) { return rapidobj_errc::ParseError; }
            chunk.precise_normals.ensure_enough_room_for(3);
            for (size_t k = 0; k < 3; ++k) { chunk.precise_normals.push_back(values[k]); }
            ++chunk.normals.count;
        }
        return rapidobj_errc::Success;
    }

    // Ordinary primitives avoid constructing a freeform record or traversing
    // its keyword handlers. Sparse ordinary records still validate body state.
    if (key == "f" || key == "l" || key == "p" || key == "s" || key == "usemtl") {
        if (line.size() > kMaxLineLength) { return rapidobj_errc::LineTooLongError; }
        PrototypeOrdinary(records, line_number);
        return ProcessLine(line, &chunk, &context);
    }

    FreeformStatement record;
    record.line = line_number;
    record.positions = chunk.positions.count;
    record.texcoords = chunk.texcoords.count;
    record.normals = chunk.normals.count;
    if (key == "g" || key == "o" || key == "mtllib") {
        record.kind = key == "mtllib" ? StatementKind::materialLibrary : StatementKind::group;
        Trim(text);
        record.name = text;
        if (key != "mtllib") {
            const auto rc = ProcessLine(line, &chunk, &context);
            if (rc != rapidobj_errc::Success) { return rc; }
        }
    } else if (key == "cstype") {
        record.kind = StatementKind::type;
        auto basis = PrototypeToken(text);
        if (basis == "rat") { record.rational = true; basis = PrototypeToken(text); }
        if (basis.empty() || !PrototypeToken(text).empty()) { return rapidobj_errc::ParseError; }
        record.name = basis;
    } else if (key == "deg" || key == "vp" || key == "parm") {
        record.kind = key == "deg" ? StatementKind::degree
            : key == "vp" ? StatementKind::parameterVertex : StatementKind::knots;
        if (key == "parm") { record.name = PrototypeToken(text); }
        if (!PrototypeValues(text, record.values)) { return rapidobj_errc::ParseError; }
    } else if (key == "curv" || key == "curv2" || key == "surf") {
        record.kind = key == "curv" ? StatementKind::curve
            : key == "curv2" ? StatementKind::parameterCurve : StatementKind::surface;
        const size_t domains = key == "curv" ? 2 : key == "surf" ? 4 : 0;
        for (size_t k = 0; k < domains; ++k) {
            double value = 0;
            if (!PrototypeNumber(PrototypeToken(text), value)) { return rapidobj_errc::ParseError; }
            record.values.push_back(value);
        }
        for (auto token = PrototypeToken(text); !token.empty(); token = PrototypeToken(text)) {
            ControlReference ref;
            if (!PrototypeReference(token, ref)) { return rapidobj_errc::ParseError; }
            record.references.push_back(ref);
        }
    } else if (key == "trim" || key == "hole") {
        record.kind = key == "trim" ? StatementKind::trim : StatementKind::hole;
        for (auto token = PrototypeToken(text); !token.empty(); token = PrototypeToken(text)) {
            double begin = 0, end = 0;
            ControlReference ref;
            if (!PrototypeNumber(token, begin) || !PrototypeNumber(PrototypeToken(text), end)
                || !PrototypeInteger(PrototypeToken(text), ref.position)) { return rapidobj_errc::ParseError; }
            record.values.push_back(begin); record.values.push_back(end);
            record.references.push_back(ref);
        }
    } else if (key == "end") {
        record.kind = StatementKind::end;
        if (!PrototypeToken(text).empty()) { return rapidobj_errc::ParseError; }
    } else if (key == "scrv" || key == "sp" || key == "con" || key == "bmat" || key == "step"
        || key == "ctech" || key == "stech" || key == "bzp" || key == "bsp" || key == "cdc"
        || key == "cdp" || key == "res") {
        record.kind = StatementKind::unsupported;
        record.name = key;
    } else {
        if (line.size() > kMaxLineLength) { return rapidobj_errc::LineTooLongError; }
        return rapidobj_errc::ParseError;
    }
    records.push_back(std::move(record));
    return rapidobj_errc::Success;
}

// A scanner owns only incomplete physical/logical lines. Complete ordinary
// statements are decoded directly from their input block's string_view.
struct PrototypeScanner {
    PrototypeChunk chunk;
    std::string physical, logical;
    size_t physical_lines = 0, logical_start = 1, logical_physical = 0;
};

inline size_t PrototypeContinuation(std::string_view line) {
    if (!line.empty() && line.back() == '\r') { line.remove_suffix(1); }
    size_t end = line.size();
    while (end && (line[end - 1] == ' ' || line[end - 1] == '\t')) { --end; }
    if (end && line[end - 1] == '\\' && line.find('#') == std::string_view::npos) { return end - 1; }
    return std::string_view::npos;
}

inline void PrototypePhysical(PrototypeScanner& scanner, std::string_view line,
    SharedContext& context, const PrototypeOptions& options) {
    ++scanner.physical_lines;
    ++scanner.logical_physical;
    auto fail = [&] {
        scanner.chunk.parsed.error = {make_error_code(rapidobj_errc::LineTooLongError), {}, scanner.logical_start};
    };
    if (line.size() > options.max_statement_bytes) { fail(); return; }
    const auto continuation = PrototypeContinuation(line);
    if (!line.empty() && line.back() == '\r') { line.remove_suffix(1); }
    if (continuation != std::string_view::npos) { line = line.substr(0, continuation); }
    if (line.size() > options.max_statement_bytes - scanner.logical.size()) { fail(); return; }
    if (continuation != std::string_view::npos || scanner.logical_physical > 1) {
        scanner.logical.append(line);
        if (continuation != std::string_view::npos) {
            if (scanner.logical.size() == options.max_statement_bytes) { fail(); }
            else { scanner.logical += ' '; }
            return;
        }
        line = scanner.logical;
    }
    auto trimmed = line;
    TrimLeft(trimmed);
    const auto rc = trimmed.size() > kMaxLineLength && trimmed.front() == '#'
        ? rapidobj_errc::LineTooLongError : PrototypeLine(line, scanner.logical_start, scanner.chunk, context);
    if (rc != rapidobj_errc::Success) {
        scanner.chunk.parsed.error = {make_error_code(rc), std::string(line.substr(0, kMaxLineLength)), scanner.logical_start};
    }
    scanner.logical.clear();
    scanner.logical_physical = 0;
    scanner.logical_start = scanner.physical_lines + 1;
}

inline void PrototypeScan(PrototypeScanner& scanner, std::string_view text, SharedContext& context,
    const PrototypeOptions& options, const std::atomic_bool& stopped) {
    size_t visits = 0;
    while (!text.empty() && !scanner.chunk.parsed.error) {
        if (visits++ % 256 == 0 && stopped.load(std::memory_order_relaxed)) { return; }
        const auto end = text.find('\n');
        const auto line = text.substr(0, end);
        if (line.size() > options.max_statement_bytes - scanner.physical.size()) {
            scanner.chunk.parsed.error = {make_error_code(rapidobj_errc::LineTooLongError), {}, scanner.logical_start};
            return;
        }
        if (end == std::string_view::npos) { scanner.physical.append(line); return; }
        if (scanner.physical.empty()) { PrototypePhysical(scanner, line, context, options); }
        else {
            scanner.physical.append(line);
            PrototypePhysical(scanner, scanner.physical, context, options);
            scanner.physical.clear();
        }
        text.remove_prefix(end + 1);
    }
}

inline void PrototypeFinish(PrototypeScanner& scanner, SharedContext& context, const PrototypeOptions& options) {
    if (scanner.chunk.parsed.error) { return; }
    if (!scanner.physical.empty()) {
        PrototypePhysical(scanner, scanner.physical, context, options);
        scanner.physical.clear();
    }
    if (!scanner.chunk.parsed.error && scanner.logical_physical) {
        scanner.chunk.parsed.error = {make_error_code(rapidobj_errc::ParseError),
            "Unterminated OBJ line continuation", scanner.logical_start};
    }
}

struct PrototypeBlock {
    std::unique_ptr<char, sys::AlignedDeleter> storage;
    std::string_view text;
    size_t storage_bytes = 0, prefix_end = 0, middle_end = 0;
    PrototypeChunk middle;
    std::error_code read_error;
};

inline PrototypeBlock PrototypeAllocateBlock(size_t size) {
    PrototypeBlock block;
    block.storage_bytes = (size + 4095) / 4096 * 4096;
    block.storage.reset(sys::AlignedAllocate(block.storage_bytes, 4096));
    if (!block.storage) { throw std::bad_alloc(); }
    block.text = {block.storage.get(), size};
    return block;
}

inline PrototypeBlock PrototypeParseBlock(PrototypeBlock block, bool first, SharedContext& context,
    const PrototypeOptions& options, const std::atomic_bool& stopped) {
    // The first physical line may start in a preceding block. Keep it and the
    // following complete logical statement for ordered boundary reconciliation.
    // This also handles a '#' before the block that cancels an apparent '\\'.
    if (!first) {
        auto end = block.text.find('\n');
        block.prefix_end = block.text.size();
        size_t visits = 0;
        while (end != std::string_view::npos) {
            if (visits++ % 256 == 0 && stopped.load(std::memory_order_relaxed)) { return block; }
            const auto begin = end + 1;
            end = block.text.find('\n', begin);
            if (end != std::string_view::npos
                && PrototypeContinuation(block.text.substr(begin, end - begin)) == std::string_view::npos) {
                block.prefix_end = end + 1;
                break;
            }
        }
    }
    // Locate the final complete logical statement. Only boundary lines are
    // inspected here; the body is scanned once by its worker, without copying.
    auto end = block.text.rfind('\n');
    block.middle_end = block.prefix_end;
    size_t visits = 0;
    while (end != std::string_view::npos && end >= block.prefix_end) {
        if (visits++ % 256 == 0 && stopped.load(std::memory_order_relaxed)) { return block; }
        const auto previous = end ? block.text.rfind('\n', end - 1) : std::string_view::npos;
        const auto begin = previous == std::string_view::npos ? 0 : previous + 1;
        if (PrototypeContinuation(block.text.substr(begin, end - begin)) == std::string_view::npos) {
            block.middle_end = end + 1;
            break;
        }
        end = previous;
    }
    PrototypeScanner scanner;
    PrototypeScan(scanner, block.text.substr(block.prefix_end, block.middle_end - block.prefix_end), context, options, stopped);
    scanner.chunk.parsed.text.line_count = scanner.physical_lines;
    block.middle = std::move(scanner.chunk);
    return block;
}

inline PrototypeBlock PrototypeReadFileBlock(sys::File& file, size_t offset, size_t size,
    SharedContext& context, const PrototypeOptions& options, const std::atomic_bool& stopped) {
    if (stopped.load(std::memory_order_relaxed)) { return {}; }
    auto block = PrototypeAllocateBlock(size);
    sys::FileReader reader(file);
    PendingRead pending{&reader}; // joins before reader and destination storage are destroyed
    block.read_error = reader.Error();
    if (!block.read_error) { block.read_error = reader.ReadBlock(offset, size, block.storage.get()); }
    if (!block.read_error) {
        const auto read = reader.WaitForResult();
        block.read_error = read.error_code;
        block.text = block.text.substr(0, read.bytes_read);
        if (!block.read_error && read.bytes_read != size) { block.read_error = std::make_error_code(std::io_errc::stream); }
    } else { block.text = {}; }
    return PrototypeParseBlock(std::move(block), offset == 0, context, options, stopped);
}

struct PrototypeInput {
    std::istream* stream = nullptr;
    std::string_view memory;
    sys::File* file = nullptr;
};

inline PrototypeResult PrototypeParse(PrototypeInput input, GeometryBuffers output,
    const MaterialLibrary& material_library, const PrototypeOptions& options) {
    if (!output.positions.empty() || !output.texcoords.empty() || !output.normals.empty()) {
        throw std::invalid_argument("Prototype output buffers must be empty.");
    }
    if (!options.chunk_bytes || !options.read_bytes || !options.max_statement_bytes
        || options.read_bytes > 16 * 1024 * 1024 || options.chunk_bytes > 64 * 1024 * 1024
        || options.max_statement_bytes > 16 * 1024 * 1024 || options.workers > 64) {
        throw std::invalid_argument("Invalid prototype parsing limits.");
    }
    PrototypeResult result;
    const bool names_only = options.material_names_only || std::get_if<std::nullptr_t>(&material_library.Value());
    const auto names_policy = MaterialLibrary::String("");
    auto context = std::make_shared<SharedContext>();
    context->output = &output;
    context->material.library = names_only ? &names_policy : &material_library;
    const auto workers = options.workers ? options.workers
        : std::min(size_t{16}, std::max(size_t{1}, size_t(std::thread::hardware_concurrency())));
    context->thread.concurrency = workers;
    std::vector<Chunk> chunks;
    PrototypeScanner boundary;
    std::atomic_bool stopped = false;
    size_t inflight = 0, positions = 0, texcoords = 0, normals = 0, boundary_line_base = 0;
    // Futures are destroyed first on every exit, before all referenced state.
    struct Pending { std::future<PrototypeBlock> worker; size_t text_bytes; };
    std::deque<Pending> pending;
    const auto collect = [&](PrototypeChunk chunk, size_t line_offset) {
        if (chunk.parsed.error) {
            chunk.parsed.error.line_num += line_offset;
            result.polygons.error = chunk.parsed.error;
            return;
        }
        constexpr auto limit = static_cast<size_t>(std::numeric_limits<int>::max());
        if (chunk.parsed.positions.count > limit - positions || chunk.parsed.texcoords.count > limit - texcoords
            || chunk.parsed.normals.count > limit - normals) { throw std::length_error("OBJ exceeds the supported attribute range."); }
        for (auto& record : chunk.statements) {
            record.line += line_offset;
            record.positions += positions; record.texcoords += texcoords; record.normals += normals;
            result.statements.push_back(std::move(record));
        }
        for (const auto& weight : chunk.weights) { result.weights.push_back({weight.position + positions, weight.weight}); }
        positions += chunk.parsed.positions.count;
        texcoords += chunk.parsed.texcoords.count;
        normals += chunk.parsed.normals.count;
        result.stats.parsed_chunk_bytes += SizeInBytes(chunk.parsed);
        chunks.push_back(std::move(chunk.parsed));
    };
    const auto flush_boundary = [&] {
        boundary.chunk.parsed.text.line_count = boundary.physical_lines - boundary_line_base;
        collect(std::move(boundary.chunk), 0);
        boundary.chunk = {};
        boundary_line_base = boundary.physical_lines;
    };
    const auto collect_block = [&](PrototypeBlock block) {
        ++result.stats.chunks;
        result.stats.input_bytes += block.text.size();
        PrototypeScan(boundary, block.text.substr(0, block.prefix_end), *context, options, stopped);
        if (boundary.chunk.parsed.error) { result.polygons.error = boundary.chunk.parsed.error; return; }
        if (block.middle_end > block.prefix_end) {
            assert(boundary.physical.empty() && boundary.logical.empty() && !boundary.logical_physical);
            flush_boundary();
            const auto line_count = block.middle.parsed.text.line_count;
            collect(std::move(block.middle), boundary.physical_lines);
            if (result.polygons.error) { return; }
            boundary.physical_lines += line_count;
            boundary.logical_start = boundary.physical_lines + 1;
            boundary_line_base = boundary.physical_lines;
        }
        PrototypeScan(boundary, block.text.substr(block.middle_end), *context, options, stopped);
        if (boundary.chunk.parsed.error) { result.polygons.error = boundary.chunk.parsed.error; }
        else if (block.read_error) { result.polygons.error = {block.read_error, {}, boundary.logical_start}; }
    };
    const auto collect_first = [&] {
        while (pending.front().worker.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready) {
            PrototypeCheckpoint(options);
        }
        PrototypeCheckpoint(options);
        collect_block(pending.front().worker.get());
        inflight -= pending.front().text_bytes;
        pending.pop_front();
        PrototypeCheckpoint(options);
    };
    try {
        const auto input_size = input.file ? input.file->size() : input.memory.size();
        size_t offset = 0;
        bool stream_finished = false;
        while (!result.polygons.error && (input.stream ? !stream_finished : offset < input_size)) {
            PrototypeCheckpoint(options);
            if (pending.size() == workers) { collect_first(); }
            if (result.polygons.error) { break; }
            const auto size = input.stream ? options.chunk_bytes : std::min(options.chunk_bytes, input_size - offset);
            PrototypeBlock block;
            if (input.stream) {
                block = PrototypeAllocateBlock(size);
                size_t filled = 0;
                while (filled < size) {
                    PrototypeCheckpoint(options);
                    const auto request = std::min(options.read_bytes, size - filled);
                    input.stream->read(block.storage.get() + filled, static_cast<std::streamsize>(request));
                    filled += static_cast<size_t>(input.stream->gcount());
                    if (input.stream->bad() || (input.stream->fail() && !input.stream->eof())) {
                        block.read_error = std::make_error_code(std::io_errc::stream);
                    }
                    if (input.stream->eof() || block.read_error) { stream_finished = true; break; }
                }
                block.text = block.text.substr(0, filled);
            } else if (!input.file) { block.text = input.memory.substr(offset, size); }
            const auto bytes = input.file ? (size + 4095) / 4096 * 4096
                : block.storage_bytes ? block.storage_bytes : block.text.size();
            const bool sequential = workers == 1 || (offset == 0 && (input.stream ? stream_finished : size == input_size));
            auto parse = [&, start = offset, size, block = std::move(block)]() mutable {
                if (input.file) { return PrototypeReadFileBlock(*input.file, start, size, *context, options, stopped); }
                return PrototypeParseBlock(std::move(block), start == 0, *context, options, stopped);
            };
            if (sequential) {
                result.stats.workers = std::max(result.stats.workers, size_t{1});
                result.stats.peak_inflight_text_bytes = std::max(result.stats.peak_inflight_text_bytes, bytes);
                collect_block(parse());
            } else {
                pending.push_back({std::async(std::launch::async, std::move(parse)), bytes});
                inflight += bytes;
                result.stats.workers = std::max(result.stats.workers, pending.size());
                result.stats.peak_inflight_text_bytes = std::max(result.stats.peak_inflight_text_bytes, inflight);
            }
            offset += size;
        }
        while (!pending.empty() && !result.polygons.error) { collect_first(); }
        if (result.polygons.error) { stopped.store(true, std::memory_order_relaxed); return result; }
        PrototypeFinish(boundary, *context, options);
        flush_boundary();
        if (result.polygons.error) { return result; }
        if (chunks.empty()) { chunks.emplace_back(); }

        // Material dispatch is ordered and runs after parsing. This avoids
        // concurrent mutation of the legacy material-library selection state.
        for (const auto& record : result.statements) {
            if (names_only) { break; }
            if (record.kind != StatementKind::materialLibrary || !context->material.library) { continue; }
            if (context->material.library_name.empty()) {
                context->material.library_name = record.name;
                context->material.parse_result = std::async(std::launch::async, ParseMaterialLibrary, context.get());
            } else if (context->material.library_name != record.name) {
                result.polygons.error = {make_error_code(rapidobj_errc::AmbiguousMaterialLibraryError), record.name, record.line};
                return result;
            }
        }
        if (names_only) {
            ParseMaterialsResult names;
            for (const auto& chunk : chunks) {
                for (const auto& material : chunk.materials.list) {
                    const auto id = static_cast<int>(names.material_map.size());
                    names.material_map.try_emplace(material.name, id);
                }
            }
            std::promise<ParseMaterialsResult> ready;
            context->material.parse_result = ready.get_future();
            ready.set_value(std::move(names));
        }
        PrototypeCheckpoint(options);
        context->thread.concurrency = std::max(size_t{1}, result.stats.workers);
        result.polygons = Merge(chunks, context);
        PrototypeCheckpoint(options);
        if (result.polygons.error) {
            output.positions.clear(); output.texcoords.clear(); output.normals.clear();
        } else { result.stats.position_copy_bytes_avoided = positions * sizeof(std::array<double, 3>); }
        return result;
    } catch (...) {
        stopped.store(true, std::memory_order_relaxed);
        output.positions.clear(); output.texcoords.clear(); output.normals.clear();
        throw;
    }
}
} // namespace detail

// File and stream input deliberately use the same scanner and grammar.
// Search paths must be absolute. The caller specifies material resolution;
// an in-memory OBJ never implicitly consults the current working directory.
inline void ValidatePrototypeMaterials(const MaterialLibrary& materials) {
    if (const auto* paths = std::get_if<std::vector<std::filesystem::path>>(&materials.Value())) {
        if (paths->empty() || std::any_of(paths->begin(), paths->end(), [](const auto& p) { return p.is_relative(); })) {
            throw std::invalid_argument("Prototype material paths must be absolute and nonempty.");
        }
    } else if (std::get_if<std::monostate>(&materials.Value())) {
        throw std::invalid_argument("Prototype requires an explicit material policy.");
    }
}
inline PrototypeResult ParseStreamPrototype(std::istream& input, GeometryBuffers output,
    const MaterialLibrary& materials, const PrototypeOptions& options = {}) {
    ValidatePrototypeMaterials(materials);
    return detail::PrototypeParse({&input, {}}, output, materials, options);
}
inline PrototypeResult ParseMemoryPrototype(std::string_view input, GeometryBuffers output,
    const MaterialLibrary& materials, const PrototypeOptions& options = {}) {
    ValidatePrototypeMaterials(materials);
    return detail::PrototypeParse({nullptr, input}, output, materials, options);
}
inline PrototypeResult ParseFilePrototype(const std::filesystem::path& path, GeometryBuffers output,
    const PrototypeOptions& options = {}) {
    // Buffered native handles allow arbitrary block boundaries and reuse the
    // vendored positioned/overlapped reader on worker threads.
    detail::sys::File input(path, true);
    if (!input) {
        PrototypeResult result;
        result.polygons.error = {std::make_error_code(std::io_errc::stream), {}, 0};
        return result;
    }
    return detail::PrototypeParse({nullptr, {}, &input}, output,
        MaterialLibrary::SearchPath(std::filesystem::absolute(path).parent_path(), Load::Optional), options);
}
} // namespace rapidobj
