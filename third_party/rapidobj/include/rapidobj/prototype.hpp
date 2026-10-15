// Experimental Woby extension to the MIT-licensed vendored RapidOBJ.
// Keeps the existing polygon decoder, index reconciliation and triangulator.
#pragma once
#include "rapidobj.hpp"
#include <deque>
#include <limits>
#include <condition_variable>
#include <bit>
#include <span>

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
    size_t chunk_bytes = 4 * 1024 * 1024; // maximum raw block size; known inputs also balance across workers
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
inline bool PrototypeNumberEnd(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '#';
}

// Decode eight bytes around a decimal point with integer SWAR. The integer
// conversion and division below are the same exact Clinger path as fast_float:
// at most 2^53 in the mantissa and an exactly representable power of ten.
// Exponents, longer decimals and all exceptional forms use the full decoder.
inline bool PrototypeShortNumber(std::string_view& text, double& value) {
    auto digits = text;
    const bool negative = !digits.empty() && digits.front() == '-';
    if (negative) { digits.remove_prefix(1); }
    if (digits.size() < 8) { return false; }
    auto bytes = fast_float::read_u64(digits.data());
    const auto dots = bytes ^ UINT64_C(0x2e2e2e2e2e2e2e2e);
    const auto dot_mask = (dots - UINT64_C(0x0101010101010101)) & ~dots & UINT64_C(0x8080808080808080);
    size_t decimals = 0;
    if (dot_mask) {
        const auto dot = static_cast<unsigned>(std::countr_zero(dot_mask)) / 8;
        const auto prefix = (UINT64_C(1) << (dot * 8)) - 1;
        const auto suffix = dot == 7 ? UINT64_C(0) : ~((UINT64_C(1) << ((dot + 1) * 8)) - 1);
        bytes = (bytes & suffix) | ((bytes & prefix) << 8) | UINT64_C(0x30);
        decimals = 7 - dot;
    }
    if (!fast_float::is_made_of_eight_digits_fast(bytes)) { return false; }
    uint64_t mantissa = fast_float::parse_eight_digits_unrolled(bytes);
    size_t end = 8;
    while (end < digits.size() && digits[end] >= '0' && digits[end] <= '9' && end < 16) {
        mantissa = mantissa * 10 + static_cast<unsigned>(digits[end++] - '0');
        decimals += dot_mask != 0;
    }
    if (mantissa > (UINT64_C(1) << 53) || (end != digits.size() && !PrototypeNumberEnd(digits[end]))) { return false; }
    constexpr double powers[] = {1, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15};
    value = static_cast<double>(mantissa) / powers[decimals];
    if (negative) { value = -value; }
    text.remove_prefix(end + static_cast<size_t>(negative));
    return true;
}

// Let the numeric decoder find the end once, then validate its delimiter.
inline bool PrototypeNextNumber(std::string_view& text, double& value, bool finite) {
    if (!text.empty() && text.front() == '+') { text.remove_prefix(1); }
    if (text.empty()) { return false; }
    if (PrototypeShortNumber(text, value)) { return true; }
    const auto parsed = fast_float::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || (finite && !std::isfinite(value))) { return false; }
    if (parsed.ptr != text.data() + text.size() && *parsed.ptr != ' ' && *parsed.ptr != '\t' && *parsed.ptr != '\r' && *parsed.ptr != '#') {
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
// Short fixed-point decimals cannot overflow or underflow a double. Validate
// these unused color fields without computing a mantissa, exponent or quotient.
// All other forms keep the same numeric decoder and finite-value check.
inline bool PrototypeSkipNumber(std::string_view& text) {
    if (text.size() >= 8 && text[1] == '.' && (text.size() == 8 || PrototypeNumberEnd(text[8]))) {
        // Common exported RGB values contain one digit and six decimal places.
        const auto bytes = fast_float::read_u64(text.data()) ^ UINT64_C(0x1e00);
        if (fast_float::is_made_of_eight_digits_fast(bytes)) { text.remove_prefix(8); return true; }
    }
    size_t end = 0, digits = 0;
    if (!text.empty() && (text[0] == '+' || text[0] == '-')) { ++end; }
    while (end < text.size() && text[end] >= '0' && text[end] <= '9') { ++end; ++digits; }
    if (end < text.size() && text[end] == '.') {
        ++end;
        while (end < text.size() && text[end] >= '0' && text[end] <= '9') { ++end; ++digits; }
    }
    if (digits && digits <= 18 && (end == text.size() || text[end] == ' ' || text[end] == '\t'
        || text[end] == '\r' || text[end] == '#')) {
        text.remove_prefix(end);
        return true;
    }
    double ignored = 0;
    return PrototypeNextNumber(text, ignored, true);
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
        records.emplace_back();
        records.back().line = line;
    }
}

struct PrototypeChunk {
    Chunk parsed;
    std::vector<FreeformStatement> statements;
    std::vector<PositionWeight> weights;
};

inline rapidobj_errc PrototypePosition(std::string_view text, size_t line_number, PrototypeChunk& out) {
    PrototypeOrdinary(out.statements, line_number);
    auto& chunk = out.parsed;
    chunk.precise_positions.ensure_enough_room_for(1);
    auto& position = chunk.precise_positions.append_uninitialized();
    for (auto& value : position) {
        TrimLeft(text);
        if (!PrototypeNextNumber(text, value, true)) { return rapidobj_errc::ParseError; }
    }
    // The common three-coordinate record needs neither a comment prescan nor
    // storage/dispatch for seven potential values. Validate every optional
    // value, but convert only an actual weight that the caller will use.
    std::string_view weight_text;
    size_t extras = 0;
    for (;;) {
        TrimLeft(text);
        if (text.empty() || text.front() == '#') { break; }
        const auto before = text;
        if (extras == 4 || !PrototypeSkipNumber(text)) { return rapidobj_errc::ParseError; }
        if (extras++ == 0) { weight_text = before.substr(0, before.size() - text.size()); }
    }
    if (extras == 2) { return rapidobj_errc::ParseError; }
    if (extras == 1) {
        double weight = 1;
        if (!PrototypeNumber(weight_text, weight)) { return rapidobj_errc::ParseError; }
        if (weight != 1) { out.weights.emplace_back(chunk.positions.count, weight); }
    }
    ++chunk.positions.count;
    return rapidobj_errc::Success;
}

template <bool Normal>
inline rapidobj_errc PrototypeAttribute(std::string_view text, size_t line_number, PrototypeChunk& out) {
    PrototypeOrdinary(out.statements, line_number);
    double first = 0, second = 0, third = 0;
    TrimLeft(text);
    if (!PrototypeNextNumber(text, first, false)) { return rapidobj_errc::ParseError; }
    TrimLeft(text);
    if (Normal || (!text.empty() && text.front() != '#')) {
        if (!PrototypeNextNumber(text, second, false)) { return rapidobj_errc::ParseError; }
        TrimLeft(text);
    }
    if (Normal || (!text.empty() && text.front() != '#')) {
        if (!PrototypeNextNumber(text, third, false)) { return rapidobj_errc::ParseError; }
        TrimLeft(text);
    }
    if (!text.empty() && text.front() != '#') { return rapidobj_errc::ParseError; }
    auto& chunk = out.parsed;
    auto& values = Normal ? chunk.precise_normals : chunk.precise_texcoords;
    values.ensure_enough_room_for(Normal ? 3 : 2);
    values.push_back(first);
    values.push_back(second);
    if constexpr (Normal) { values.push_back(third); ++chunk.normals.count; }
    else { ++chunk.texcoords.count; }
    return rapidobj_errc::Success;
}

inline rapidobj_errc PrototypeGeneralLine(std::string_view line, size_t line_number,
    PrototypeChunk& out, SharedContext& context) {
    line = line.substr(0, line.find('#'));
    Trim(line);
    if (line.empty()) { return rapidobj_errc::Success; }
    auto text = line;
    const auto key = PrototypeToken(text);
    auto& chunk = out.parsed;
    auto& records = out.statements;

    if (key == "v") { return PrototypePosition(text, line_number, out); }
    if (key == "vt" || key == "vn") {
        return key == "vn" ? PrototypeAttribute<true>(text, line_number, out)
            : PrototypeAttribute<false>(text, line_number, out);
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

inline bool PrototypePositiveIndex(std::string_view& text, int& index) {
    const auto* p = text.data();
    const auto* end = p + text.size();
    uint64_t value = 0;
    if (end - p >= 8) {
        const auto bytes = fast_float::read_u64(p);
        const auto invalid = ((bytes + UINT64_C(0x4646464646464646)) | (bytes - UINT64_C(0x3030303030303030)))
            & UINT64_C(0x8080808080808080);
        const auto count = static_cast<unsigned>(std::countr_zero(invalid)) / 8;
        if (!count) { return false; }
        const auto padded = count == 8 ? bytes
            : (bytes << ((8 - count) * 8)) | (UINT64_C(0x3030303030303030) >> (count * 8));
        value = fast_float::parse_eight_digits_unrolled(padded);
        p += count;
    }
    while (p != end && *p >= '0' && *p <= '9') {
        value = value * 10 + static_cast<unsigned>(*p++ - '0');
        if (value > static_cast<uint64_t>(std::numeric_limits<int>::max())) { return false; }
    }
    if (!value) { return false; }
    index = static_cast<int>(value - 1);
    text.remove_prefix(static_cast<size_t>(p - text.data()));
    return true;
}

inline bool PrototypeFaceIndex(std::string_view& text, size_t count, int& index,
    OffsetFlags& flags, ApplyOffset flag) {
    const bool relative = !text.empty() && text.front() == '-';
    if (relative) { text.remove_prefix(1); }
    if (!PrototypePositiveIndex(text, index)) { return false; }
    if (relative) {
        index = static_cast<int>(count) - index - 1;
        flags |= static_cast<OffsetFlags>(flag);
    }
    return true;
}

// Decode triangle/quad references once, including relative UVs and normals.
// Commit only a fully recognized face. Polygons and invalid forms keep the
// general decoder and its original diagnostic behavior.
inline bool PrototypeSimpleFace(std::string_view text, Chunk& chunk) {
    std::array<Index, 4> corners;
    std::array<OffsetFlags, 4> flags{};
    size_t count = 0;
    for (auto& corner : corners) {
        TrimLeft(text);
        if (text.empty() || text.front() == '#') { break; }
        corner = {-1, -1, -1};
        if (!PrototypeFaceIndex(text, chunk.positions.count, corner.position_index, flags[count], ApplyOffset::Position)) { return false; }
        if (!text.empty() && text.front() == '/') {
            text.remove_prefix(1);
            if (text.empty()) { return false; }
            if (text.front() != '/' && !PrototypeFaceIndex(text, chunk.texcoords.count, corner.texcoord_index,
                flags[count], ApplyOffset::Texcoord)) { return false; }
            if (!text.empty() && text.front() == '/') {
                text.remove_prefix(1);
                if (!PrototypeFaceIndex(text, chunk.normals.count, corner.normal_index, flags[count], ApplyOffset::Normal)) { return false; }
            }
        }
        if (!text.empty() && !PrototypeNumberEnd(text.front())) { return false; }
        ++count;
    }
    TrimLeft(text);
    if ((!text.empty() && text.front() != '#') || count < 3) { return false; }
    auto& indices = chunk.mesh.indices;
    indices.buffer.ensure_enough_room_for(count);
    indices.flags.ensure_enough_room_for(count);
    for (size_t i = 0; i < count; ++i) {
        indices.buffer.push_back(corners[i]);
        indices.flags.push_back(flags[i]);
    }
    chunk.mesh.faces.buffer.ensure_enough_room_for(1);
    chunk.mesh.faces.buffer.push_back(static_cast<unsigned char>(count));
    ++chunk.mesh.faces.count;
    return true;
}

inline rapidobj_errc PrototypeLine(std::string_view line, size_t line_number,
    PrototypeChunk& out, SharedContext& context) {
    TrimLeft(line);
    if (line.size() >= 2 && (line[1] == ' ' || line[1] == '\t')) {
        const auto kind = line.front();
        if (kind == 'v') { return PrototypePosition(line.substr(2), line_number, out); }
        if (kind == 'p') {
            auto text = line.substr(2);
            TrimLeft(text);
            int value = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            if (parsed.ec == std::errc{}) {
                auto remainder = text.substr(static_cast<size_t>(parsed.ptr - text.data()));
                TrimLeft(remainder);
                if (remainder.empty() || remainder.front() == '#') {
                    if (static_cast<size_t>(parsed.ptr - line.data()) > kMaxLineLength) { return rapidobj_errc::LineTooLongError; }
                    if (!value) { return rapidobj_errc::IndexOutOfBoundsError; }
                    PrototypeOrdinary(out.statements, line_number);
                    auto& chunk = out.parsed;
                    auto& indices = chunk.points.indices;
                    const auto negative = value < 0;
                    value = negative ? value + static_cast<int>(chunk.positions.count) : value - 1;
                    indices.buffer.ensure_enough_room_for(1);
                    indices.flags.ensure_enough_room_for(1);
                    indices.buffer.push_back({value, -1, -1});
                    indices.flags.push_back(static_cast<OffsetFlags>(negative ? ApplyOffset::Position : ApplyOffset::None));
                    return rapidobj_errc::Success;
                }
            }
        }
        if (kind == 'f' || kind == 'l' || kind == 'p') {
            if (kind == 'f' && line.size() <= kMaxLineLength && PrototypeSimpleFace(line.substr(2), out.parsed)) {
                PrototypeOrdinary(out.statements, line_number);
                return rapidobj_errc::Success;
            }
            line = line.substr(0, line.find('#'));
            TrimRight(line);
            if (line.size() > kMaxLineLength) { return rapidobj_errc::LineTooLongError; }
            PrototypeOrdinary(out.statements, line_number);
            return ProcessLine(line, &out.parsed, &context);
        }
    }
    if (line.size() >= 3 && line[0] == 'v' && (line[2] == ' ' || line[2] == '\t')) {
        if (line[1] == 't') { return PrototypeAttribute<false>(line.substr(3), line_number, out); }
        if (line[1] == 'n') { return PrototypeAttribute<true>(line.substr(3), line_number, out); }
    }
    return PrototypeGeneralLine(line, line_number, out, context);
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

// With no partial statement, keep reconstruction state outside the common loop.
// Only an incomplete line or actual continuation needs the slower state machine.
inline bool PrototypeCompleteLines(PrototypeScanner& scanner, std::string_view& text, SharedContext& context,
    const PrototypeOptions& options, const std::atomic_bool& stopped, std::span<const uint32_t> line_ends = {}) {
    const auto* begin = text.data();
    size_t line_index = 0;
    auto line_number = scanner.physical_lines;
    size_t visits = 0;
    bool done = false;
    while (!text.empty()) {
        if (visits++ % 256 == 0 && stopped.load(std::memory_order_relaxed)) { done = true; break; }
        const auto end = line_index < line_ends.size()
            ? line_ends[line_index++] - static_cast<size_t>(text.data() - begin) : text.find('\n');
        auto line = text.substr(0, end);
        if (end == std::string_view::npos || line.size() > options.max_statement_bytes) { break; }
        if (!line.empty() && line.back() == '\r') { line.remove_suffix(1); }
        if (!line.empty() && (line.back() == '\\' || line.back() == ' ' || line.back() == '\t')
            && PrototypeContinuation(line) != std::string_view::npos) { break; }
        if (line.size() > kMaxLineLength) {
            auto trimmed = line;
            TrimLeft(trimmed);
            if (!trimmed.empty() && trimmed.front() == '#') { break; }
        }
        ++line_number;
        const auto rc = PrototypeLine(line, line_number, scanner.chunk, context);
        text.remove_prefix(end + 1);
        if (rc != rapidobj_errc::Success) {
            scanner.chunk.parsed.error = {make_error_code(rc), std::string(line.substr(0, kMaxLineLength)), line_number};
            done = true;
            break;
        }
    }
    scanner.physical_lines = line_number;
    scanner.logical_start = line_number + 1;
    return done || text.empty();
}

inline void PrototypeScan(PrototypeScanner& scanner, std::string_view text, SharedContext& context,
    const PrototypeOptions& options, const std::atomic_bool& stopped) {
    size_t visits = 0;
    while (!text.empty() && !scanner.chunk.parsed.error) {
        if (visits++ % 256 == 0 && stopped.load(std::memory_order_relaxed)) { return; }
        if (scanner.physical.empty() && !scanner.logical_physical
            && PrototypeCompleteLines(scanner, text, context, options, stopped)) { return; }
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
    size_t input_bytes = 0;
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

// A bounded sizing pass avoids geometric growth and copying while decoding.
// Count only ordinary physical records. Continuations and less common syntax
// still use the checked growable buffers; these estimates never set their size.
inline size_t PrototypeFaceWords(std::string_view text) {
    size_t words = 0;
    bool previous_space = true;
#if defined(__AVX2__)
    const auto count = [&](uint32_t spaces, uint32_t comments, unsigned width) {
        const auto valid = width == 32 ? UINT32_MAX : (uint32_t{1} << width) - 1;
        auto starts = (~spaces & ((spaces << 1) | static_cast<uint32_t>(previous_space))) & valid;
        comments &= valid;
        if (comments) { starts &= (uint32_t{1} << std::countr_zero(comments)) - 1; }
        words += static_cast<size_t>(std::popcount(starts));
        previous_space = ((spaces >> (width - 1)) & 1) != 0;
        return comments == 0;
    };
    while (text.size() >= 32) {
        const auto bytes = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(text.data()));
        const auto spaces = _mm256_or_si256(_mm256_cmpeq_epi8(bytes, _mm256_set1_epi8(' ')),
            _mm256_or_si256(_mm256_cmpeq_epi8(bytes, _mm256_set1_epi8('\t')), _mm256_cmpeq_epi8(bytes, _mm256_set1_epi8('\r'))));
        if (!count(static_cast<uint32_t>(_mm256_movemask_epi8(spaces)),
            static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(bytes, _mm256_set1_epi8('#')))), 32)) { return words; }
        text.remove_prefix(32);
    }
    for (const unsigned width : {16u, 8u}) {
        if (text.size() < width) { continue; }
        const auto bytes = width == 16 ? _mm_loadu_si128(reinterpret_cast<const __m128i*>(text.data()))
            : _mm_loadl_epi64(reinterpret_cast<const __m128i*>(text.data()));
        const auto spaces = _mm_or_si128(_mm_cmpeq_epi8(bytes, _mm_set1_epi8(' ')),
            _mm_or_si128(_mm_cmpeq_epi8(bytes, _mm_set1_epi8('\t')), _mm_cmpeq_epi8(bytes, _mm_set1_epi8('\r'))));
        if (!count(static_cast<uint32_t>(_mm_movemask_epi8(spaces)),
            static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(bytes, _mm_set1_epi8('#')))), width)) { return words; }
        text.remove_prefix(width);
    }
#endif
    for (const char ch : text) {
        if (ch == '#') { break; }
        const bool space = ch == ' ' || ch == '\t' || ch == '\r';
        words += !space && previous_space;
        previous_space = space;
    }
    return words;
}

inline void PrototypeReserve(std::string_view text, Chunk& chunk, const std::atomic_bool& stopped,
    std::vector<uint32_t>& line_ends) {
    const auto bytes = text.size();
    line_ends.reserve(bytes / 32);
    size_t positions = 0, texcoords = 0, normals = 0, faces = 0, corners = 0, visits = 0;
    while (!text.empty()) {
        if (visits++ % 256 == 0 && stopped.load(std::memory_order_relaxed)) { return; }
        const auto end = text.find('\n');
        if (end != std::string_view::npos) { line_ends.push_back(static_cast<uint32_t>(bytes - text.size() + end)); }
        auto line = text.substr(0, end);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        TrimLeft(line);
        if (line.size() >= 2 && (line[1] == ' ' || line[1] == '\t')) {
            if (line[0] == 'v') { ++positions; }
            else if (line[0] == 'f') {
                ++faces;
                corners += std::min(size_t{kMaxVerticesInFace + 1}, PrototypeFaceWords(line.substr(2)));
            }
        } else if (line.size() >= 3 && line[0] == 'v' && (line[2] == ' ' || line[2] == '\t')) {
            texcoords += line[1] == 't';
            normals += line[1] == 'n';
        }
    }
    chunk.precise_positions.reserve(positions);
    chunk.precise_texcoords.reserve(texcoords * 2);
    chunk.precise_normals.reserve(normals * 3);
    chunk.mesh.faces.buffer.reserve(faces);
    // The general decoder reserves its diagnostic lookahead before each face.
    // Keep that margin so relative references and polygons do not force a
    // whole-buffer growth copy near the end of an exactly sized block.
    const auto index_capacity = corners ? corners + kMaxVerticesInFace + 1 : 0;
    chunk.mesh.indices.buffer.reserve(index_capacity);
    chunk.mesh.indices.flags.reserve(index_capacity);
}

inline PrototypeBlock PrototypeParseBlock(PrototypeBlock block, bool first, SharedContext& context,
    const PrototypeOptions& options, const std::atomic_bool& stopped) {
    block.input_bytes = block.text.size();
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
    auto middle = block.text.substr(block.prefix_end, block.middle_end - block.prefix_end);
    if (block.text.size() >= kBlockSize) {
        std::vector<uint32_t> line_ends;
        PrototypeReserve(middle, scanner.chunk.parsed, stopped, line_ends);
        PrototypeCompleteLines(scanner, middle, context, options, stopped, line_ends);
    }
    PrototypeScan(scanner, middle, context, options, stopped);
    scanner.chunk.parsed.text.line_count = scanner.physical_lines;
    block.middle = std::move(scanner.chunk);
    return block;
}

// Retain only boundary text after a pool worker decodes a block. Thus input
// storage stays bounded by the worker count, even when collection is ordered.
inline void PrototypeKeepEdges(PrototypeBlock& block) {
    if (block.middle_end == block.prefix_end) { return; }
    const auto suffix_size = block.text.size() - block.middle_end;
    const auto marker = block.middle_end > block.prefix_end ? size_t{1} : size_t{0};
    const auto bytes = block.prefix_end + marker + suffix_size;
    if (!bytes) { block.storage.reset(); block.storage_bytes = 0; block.text = {}; return; }
    auto edges = PrototypeAllocateBlock(bytes);
    std::memcpy(edges.storage.get(), block.text.data(), block.prefix_end);
    if (marker) { edges.storage.get()[block.prefix_end] = '\0'; }
    std::memcpy(edges.storage.get() + block.prefix_end + marker, block.text.data() + block.middle_end, suffix_size);
    block.storage = std::move(edges.storage);
    block.storage_bytes = edges.storage_bytes;
    block.text = {block.storage.get(), bytes};
    block.middle_end = block.prefix_end + marker;
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

inline size_t PrototypeBlockBytes(size_t input_size, size_t workers, size_t maximum) {
    // Keep tiny inputs on the caller, but do not leave most cores idle on a
    // medium file just because it fits in two or three maximum-sized blocks.
    // The maximum remains authoritative for explicit boundary/limit settings.
    if (workers == 1 || input_size <= kSingleThreadCutoff) { return maximum; }
    const auto per_worker = input_size / workers + (input_size % workers != 0);
    const auto aligned = per_worker / 4096 * 4096 + (per_worker % 4096 != 0 ? 4096 : 0);
    return std::min(maximum, std::max(size_t{kBlockSize}, aligned));
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
        result.stats.input_bytes += block.input_bytes;
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
        auto block = pending.front().worker.get();
        collect_block(std::move(block));
        inflight -= pending.front().text_bytes;
        pending.pop_front();
        PrototypeCheckpoint(options);
    };
    try {
        const auto input_size = input.file ? input.file->size() : input.memory.size();
        const auto block_bytes = input.stream ? options.chunk_bytes : PrototypeBlockBytes(input_size, workers, options.chunk_bytes);
        size_t offset = 0;
        bool stream_finished = false;
        if (input.file && input_size > kSingleThreadCutoff && workers > 1) {
            const auto block_count = (input_size - 1) / block_bytes + 1;
            const auto pool_size = std::min(workers, block_count);
            const auto window = std::min(2 * pool_size, block_count);
            std::vector<PrototypeBlock> blocks(window);
            std::vector<bool> ready(window);
            std::atomic_size_t next_block = 0;
            size_t collected = 0;
            std::mutex mutex;
            std::condition_variable changed;
            std::exception_ptr failure;
            const auto stop_pool = [&] {
                { std::lock_guard lock(mutex); stopped.store(true, std::memory_order_relaxed); }
                changed.notify_all();
            };
            std::vector<std::future<void>> pool;
            pool.reserve(pool_size);
            try {
                for (size_t i = 0; i < pool_size; ++i) {
                    PrototypeCheckpoint(options);
                    pool.push_back(std::async(std::launch::async, [&] {
                        try {
                            while (!stopped.load(std::memory_order_relaxed)) {
                                const auto index = next_block.fetch_add(1, std::memory_order_relaxed);
                                if (index >= block_count) { break; }
                                {
                                    std::unique_lock lock(mutex);
                                    changed.wait(lock, [&] { return index < collected + window || stopped.load(std::memory_order_relaxed); });
                                }
                                if (stopped.load(std::memory_order_relaxed)) { break; }
                                const auto begin = index * block_bytes;
                                auto block = PrototypeReadFileBlock(*input.file, begin,
                                    std::min(block_bytes, input_size - begin), *context, options, stopped);
                                PrototypeKeepEdges(block);
                                {
                                    std::lock_guard lock(mutex);
                                    blocks[index % window] = std::move(block);
                                    ready[index % window] = true;
                                }
                                changed.notify_all();
                            }
                        } catch (...) {
                            {
                                std::lock_guard lock(mutex);
                                if (!failure) { failure = std::current_exception(); }
                                stopped.store(true, std::memory_order_relaxed);
                            }
                            changed.notify_all();
                        }
                    }));
                }
                for (size_t index = 0; index < block_count; ++index) {
                    PrototypeBlock block;
                    for (;;) {
                        PrototypeCheckpoint(options);
                        std::unique_lock lock(mutex);
                        changed.wait_for(lock, std::chrono::milliseconds(10), [&] { return ready[index % window] || failure; });
                        if (failure) { std::rethrow_exception(failure); }
                        if (!ready[index % window]) { continue; }
                        block = std::move(blocks[index % window]);
                        ready[index % window] = false;
                        ++collected;
                        break;
                    }
                    changed.notify_all();
                    collect_block(std::move(block));
                    if (result.polygons.error) { stop_pool(); break; }
                }
                for (auto& worker : pool) { worker.get(); PrototypeCheckpoint(options); }
            } catch (...) {
                stop_pool();
                // All workers must join before blocks and next_block are freed.
                for (auto& worker : pool) { if (worker.valid()) { worker.wait(); } }
                throw;
            }
            result.stats.workers = pool_size;
            // Bound input storage, including a worker's temporary edge copy
            // and the block held by the collector. The scanner's reconstructed
            // statement has its separate max_statement_bytes limit.
            result.stats.peak_inflight_text_bytes = (pool_size + window + 1) * ((block_bytes + 4095) / 4096 * 4096);
            offset = input_size;
        }
        while (!result.polygons.error && (input.stream ? !stream_finished : offset < input_size)) {
            PrototypeCheckpoint(options);
            if (pending.size() == workers) { collect_first(); }
            if (result.polygons.error) { break; }
            const auto size = input.stream ? block_bytes : std::min(block_bytes, input_size - offset);
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
    const auto materials = MaterialLibrary::SearchPath(std::filesystem::absolute(path).parent_path(), Load::Optional);
    return detail::PrototypeParse({nullptr, {}, &input}, output, materials, options);
}
} // namespace rapidobj
