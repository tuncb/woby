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
    size_t chunk_bytes = 4 * 1024 * 1024;
    size_t read_bytes = 256 * 1024;
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
    const auto end = text.find_first_of(" \t\r");
    const auto token = text.substr(0, end);
    text.remove_prefix(end == std::string_view::npos ? text.size() : end);
    return token;
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
struct PrototypeText {
    std::string text;
    size_t first_line = 1, physical_lines = 0, logical_lines = 0;
    // Sparse physical-line accounting; ordinary files need no per-line table.
    std::vector<std::pair<size_t, size_t>> continuations;
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
        for (auto token = PrototypeToken(text); !token.empty(); token = PrototypeToken(text)) {
            if (count == values.size() || !PrototypeNumber(token, values[count++], key == "v")) {
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
        if (key != "f" && key != "l" && key != "p" && key != "s" && key != "usemtl") {
            return rapidobj_errc::ParseError;
        }
        PrototypeOrdinary(records, line_number);
        return ProcessLine(line, &chunk, &context);
    }
    records.push_back(std::move(record));
    return rapidobj_errc::Success;
}

inline PrototypeChunk PrototypeParseChunk(PrototypeText input, const std::shared_ptr<SharedContext>& context) {
    PrototypeChunk out;
    size_t line_number = input.first_line, logical = 0, continuation = 0;
    std::string_view text = input.text;
    while (!text.empty()) {
        const auto end = text.find('\n');
        const auto line = text.substr(0, end);
        out.parsed.text.line_count = line_number - input.first_line + 1;
        // Retain the ordinary reader's physical comment limit as well.
        auto rc = rapidobj_errc::Success;
        auto trimmed = line;
        TrimLeft(trimmed);
        if (trimmed.size() > kMaxLineLength && trimmed.front() == '#') { rc = rapidobj_errc::LineTooLongError; }
        else { rc = PrototypeLine(line, line_number, out, *context); }
        if (rc != rapidobj_errc::Success) {
            out.parsed.error = {make_error_code(rc), std::string(line.substr(0, kMaxLineLength)), line_number};
            break;
        }
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        ++line_number;
        if (continuation < input.continuations.size() && input.continuations[continuation].first == logical) {
            line_number += input.continuations[continuation++].second;
        }
        ++logical;
    }
    out.parsed.text.line_count = input.physical_lines;
    return out;
}

struct PrototypeInput {
    std::istream* stream = nullptr;
    std::string_view memory;
    size_t offset = 0;
    bool eof = false, failed = false;
};
inline size_t PrototypeRead(PrototypeInput& input, std::vector<char>& buffer) {
    if (input.stream) {
        input.stream->read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        input.eof = input.stream->eof();
        input.failed = input.stream->bad() || (input.stream->fail() && !input.eof);
        return static_cast<size_t>(input.stream->gcount());
    }
    const auto size = std::min(buffer.size(), input.memory.size() - input.offset);
    if (size) { memcpy(buffer.data(), input.memory.data() + input.offset, size); }
    input.offset += size;
    input.eof = input.offset == input.memory.size();
    return size;
}

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
    // Declare futures after every object referenced by workers, so exception
    // unwinding joins workers before destroying any referenced state.
    struct Pending { std::future<PrototypeChunk> worker; size_t text_bytes; };
    std::deque<Pending> pending;
    size_t inflight = 0, positions = 0, texcoords = 0, normals = 0;
    const auto collect = [&](PrototypeChunk chunk) {
        if (chunk.parsed.error && (!result.polygons.error
            || chunk.parsed.error.line_num < result.polygons.error.line_num)) { result.polygons.error = chunk.parsed.error; }
        constexpr auto limit = static_cast<size_t>(std::numeric_limits<int>::max());
        if (chunk.parsed.positions.count > limit - positions || chunk.parsed.texcoords.count > limit - texcoords
            || chunk.parsed.normals.count > limit - normals) { throw std::length_error("OBJ exceeds the supported attribute range."); }
        for (auto& record : chunk.statements) {
            record.positions += positions; record.texcoords += texcoords; record.normals += normals;
            result.statements.push_back(std::move(record));
        }
        for (const auto& weight : chunk.weights) { result.weights.push_back({weight.position + positions, weight.weight}); }
        positions += chunk.parsed.positions.count;
        texcoords += chunk.parsed.texcoords.count;
        normals += chunk.parsed.normals.count;
        result.stats.parsed_chunk_bytes += SizeInBytes(chunk.parsed);
        chunks.push_back(std::move(chunk.parsed));
        ++result.stats.chunks;
    };
    const auto collect_first = [&] {
        PrototypeCheckpoint(options);
        collect(pending.front().worker.get());
        inflight -= pending.front().text_bytes;
        pending.pop_front();
        PrototypeCheckpoint(options);
    };
    const auto dispatch = [&](PrototypeText text, bool final) {
        if (text.text.empty()) { return; }
        if (workers == 1 || (final && pending.empty() && chunks.empty())) {
            result.stats.workers = std::max(result.stats.workers, size_t{1});
            result.stats.peak_inflight_text_bytes = std::max(result.stats.peak_inflight_text_bytes, text.text.size());
            collect(PrototypeParseChunk(std::move(text), context));
        } else {
            if (pending.size() == workers) { collect_first(); }
            const auto size = text.text.size();
            pending.push_back({std::async(std::launch::async, PrototypeParseChunk, std::move(text), context), size});
            inflight += size;
            result.stats.workers = std::max(result.stats.workers, pending.size());
            result.stats.peak_inflight_text_bytes = std::max(result.stats.peak_inflight_text_bytes, inflight);
        }
    };

    try {
        PrototypeText text;
        std::vector<char> buffer(options.read_bytes);
        std::string physical, logical;
        size_t physical_line = 0, logical_start = 1, logical_physical = 0;
        Error scanner_error;
        const auto finish_physical = [&] {
            ++physical_line;
            ++logical_physical;
            if (!physical.empty() && physical.back() == '\r') { physical.pop_back(); }
            const auto last = physical.find_last_not_of(" \t");
            const auto comment = physical.find('#');
            const bool continued = last != std::string::npos && physical[last] == '\\'
                && (comment == std::string::npos || last < comment);
            if (continued) { physical.erase(last); }
            if (physical.size() > options.max_statement_bytes - logical.size()) {
                scanner_error = {make_error_code(rapidobj_errc::LineTooLongError), {}, logical_start};
                return;
            }
            logical += physical;
            physical.clear();
            if (continued) {
                if (logical.size() == options.max_statement_bytes) {
                    scanner_error = {make_error_code(rapidobj_errc::LineTooLongError), {}, logical_start};
                } else { logical += ' '; }
                return;
            }
            text.text.append(logical); text.text += '\n';
            if (logical_physical > 1) { text.continuations.push_back({text.logical_lines, logical_physical - 1}); }
            text.physical_lines += logical_physical;
            ++text.logical_lines;
            logical.clear(); logical_physical = 0;
            logical_start = physical_line + 1;
            if (text.text.size() >= options.chunk_bytes) {
                dispatch(std::move(text), false);
                text = {}; text.first_line = logical_start;
            }
        };
        while (!scanner_error && !result.polygons.error && !input.eof && !input.failed) {
            PrototypeCheckpoint(options);
            const auto size = PrototypeRead(input, buffer);
            result.stats.input_bytes += size;
            size_t begin = 0;
            while (begin < size && !scanner_error && !result.polygons.error) {
                const auto* newline = static_cast<const char*>(memchr(buffer.data() + begin, '\n', size - begin));
                const auto end = newline ? static_cast<size_t>(newline - buffer.data()) : size;
                if (end - begin > options.max_statement_bytes - physical.size()) {
                    scanner_error = {make_error_code(rapidobj_errc::LineTooLongError), {}, logical_start};
                    break;
                }
                physical.append(buffer.data() + begin, end - begin);
                if (newline) { finish_physical(); }
                begin = newline ? end + 1 : end;
            }
        }
        if (!scanner_error && !result.polygons.error) {
            if (input.failed) {
                scanner_error = {std::make_error_code(std::io_errc::stream), {}, logical_start};
            } else {
                if (!physical.empty()) { finish_physical(); }
                if (!scanner_error && logical_physical != 0) {
                    scanner_error = {make_error_code(rapidobj_errc::ParseError), "Unterminated OBJ line continuation", logical_start};
                }
            }
        }
        // Include complete statements preceding a scanner error, for earliest
        // diagnostics and consistent material/index accounting.
        dispatch(std::move(text), true);
        while (!pending.empty()) { collect_first(); }
        if (scanner_error && (!result.polygons.error || scanner_error.line_num < result.polygons.error.line_num)) {
            result.polygons.error = std::move(scanner_error);
        }
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
        // Tiny files parsed on the caller must not launch a full merge pool.
        context->thread.concurrency = std::max(size_t{1}, result.stats.workers);
        result.polygons = Merge(chunks, context);
        PrototypeCheckpoint(options);
        if (result.polygons.error) {
            output.positions.clear(); output.texcoords.clear(); output.normals.clear();
        } else { result.stats.position_copy_bytes_avoided = positions * sizeof(std::array<double, 3>); }
        return result;
    } catch (...) {
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
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        PrototypeResult result;
        result.polygons.error = {std::make_error_code(std::io_errc::stream), {}, 0};
        return result;
    }
    return ParseStreamPrototype(input, output,
        MaterialLibrary::SearchPath(std::filesystem::absolute(path).parent_path(), Load::Optional), options);
}
} // namespace rapidobj
