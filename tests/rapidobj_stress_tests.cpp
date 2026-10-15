#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <rapidobj/prototype.hpp>
#include <fstream>
#include <random>
#include <sstream>

namespace {
struct Fixture {
    std::filesystem::path root;
    Fixture() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (size_t attempt = 0;; ++attempt) {
            root = std::filesystem::absolute(std::filesystem::temp_directory_path())
                / ("woby_parser_stress_" + std::to_string(stamp) + "_" + std::to_string(attempt));
            if (std::filesystem::create_directory(root)) { break; }
        }
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    std::filesystem::path write(const std::string& text) const {
        const auto path = root / "model.obj";
        std::ofstream file(path, std::ios::binary);
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!file) { throw std::runtime_error("Cannot write parser stress fixture"); }
        return path;
    }
};

struct Parsed {
    std::vector<std::array<double, 3>> positions, normals;
    std::vector<std::array<double, 2>> texcoords;
    rapidobj::PrototypeResult result;
    rapidobj::GeometryBuffers buffers() { return {positions, texcoords, normals}; }
};

void same(const Parsed& expected, const Parsed& actual) {
    const auto sameIndices = [](const auto& a, const auto& b) {
        return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const auto& x, const auto& y) {
            return x.position_index == y.position_index && x.texcoord_index == y.texcoord_index
                && x.normal_index == y.normal_index;
        });
    };
    REQUIRE_FALSE(expected.result.polygons.error);
    REQUIRE_FALSE(actual.result.polygons.error);
    CHECK(actual.positions == expected.positions);
    CHECK(actual.texcoords == expected.texcoords);
    CHECK(actual.normals == expected.normals);
    const auto& a = expected.result.polygons.shapes;
    const auto& b = actual.result.polygons.shapes;
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].name == b[i].name);
        CHECK(sameIndices(a[i].mesh.indices, b[i].mesh.indices));
        CHECK(a[i].mesh.num_face_vertices == b[i].mesh.num_face_vertices);
        CHECK(a[i].mesh.material_ids == b[i].mesh.material_ids);
        CHECK(a[i].mesh.smoothing_group_ids == b[i].mesh.smoothing_group_ids);
        CHECK(sameIndices(a[i].lines.indices, b[i].lines.indices));
        CHECK(a[i].lines.num_line_vertices == b[i].lines.num_line_vertices);
        CHECK(sameIndices(a[i].points.indices, b[i].points.indices));
    }
    REQUIRE(expected.result.weights.size() == actual.result.weights.size());
    for (size_t i = 0; i < expected.result.weights.size(); ++i) {
        CHECK(expected.result.weights[i].position == actual.result.weights[i].position);
        CHECK(expected.result.weights[i].weight == actual.result.weights[i].weight);
    }
    // Ordinary markers can split at worker boundaries. Compare every semantic
    // statement, including declaration counts used to resolve freeform indices.
    std::vector<const rapidobj::FreeformStatement*> left, right;
    for (const auto& record : expected.result.statements) {
        if (record.kind != rapidobj::StatementKind::ordinary) { left.push_back(&record); }
    }
    for (const auto& record : actual.result.statements) {
        if (record.kind != rapidobj::StatementKind::ordinary) { right.push_back(&record); }
    }
    REQUIRE(left.size() == right.size());
    for (size_t i = 0; i < left.size(); ++i) {
        const auto& x = *left[i]; const auto& y = *right[i];
        CHECK(x.kind == y.kind); CHECK(x.line == y.line); CHECK(x.name == y.name);
        CHECK(x.positions == y.positions); CHECK(x.texcoords == y.texcoords); CHECK(x.normals == y.normals);
        CHECK(x.rational == y.rational); CHECK(x.values == y.values);
        REQUIRE(x.references.size() == y.references.size());
        for (size_t j = 0; j < x.references.size(); ++j) {
            CHECK(x.references[j].position == y.references[j].position);
            CHECK(x.references[j].texcoord == y.references[j].texcoord);
            CHECK(x.references[j].normal == y.references[j].normal);
            CHECK(x.references[j].attributes == y.references[j].attributes);
        }
    }
}

std::string corpus(unsigned seed) {
    std::mt19937 random(seed);
    std::string text = "#" + std::string(seed * 31, 'x') + "\n";
    if (seed == 3) { text += "v 1 2 3 #" + std::string(600000, 'x') + "\\\n"; }
    for (size_t i = 0; i < 18000; ++i) {
        const auto value = std::to_string(random() % 100000);
        text += "v " + value + ".125 2.25 -3.5";
        text += i % 127 == 0 ? " .75\r\n" : " #comment\\\n";
        text += "vt .123456789012345 .5\nvn 0 0 1\np -1\n";
        if (i >= 3) {
            switch (random() % 4) {
            case 0: text += "f -3/-3/-3 -2/-2/-2 -1/-1/-1\n"; break;
            case 1: text += "l -3 -2 \\\r\n -1\n"; break;
            case 2: text += "f 1 2 3 4\n"; break;
            default: text += "p -3 -2 -1\n"; break;
            }
        }
        if (i % 2000 == 0) { text += "g group_" + std::to_string(i) + "\ns 3\nusemtl matte\n"; }
        if (i % 1700 == 0) { text += "cstype rat bezier\ndeg 1\ncurv 0 1 -1 -1\nparm u 0 1\nend\n"; }
    }
    text += "p -1"; // Exercise a non-newline-terminated final read.
    return text;
}
} // namespace

TEST_CASE("Face decoding matches the general decoder without partial writes") {
    using namespace rapidobj::detail;
    const std::vector<std::string> references = {
        "1", "1234567", "12345678", "2147483647", "000000000001",
        "1/2", "12345678/87654321", "2//3", "3/2/1",
        "0", "-1", "+1", "2147483648", "9999999999999999999999999",
        "1/0", "1/-1", "1/", "1//", "1///2", "1/a", "1/2/0", "1/2/3/4",
        "-2147483648", "-2147483647", "1/-2/-3", "-00000000001/-2/3"
    };
    for (const auto& a : references) {
        for (const auto& b : references) {
            const auto text = a + "\t" + b + "  3/2/1 ";
            CAPTURE(text);
            Chunk chunk{};
            chunk.positions.count = 10; chunk.texcoords.count = 10; chunk.normals.count = 10;
            const bool accepted = PrototypeSimpleFace(text, chunk);
            if (!accepted) {
                CHECK(chunk.mesh.indices.buffer.size() == 0);
                CHECK(chunk.mesh.indices.flags.size() == 0);
                CHECK(chunk.mesh.faces.buffer.size() == 0);
                continue;
            }
            Buffer<rapidobj::Index> expected;
            Buffer<OffsetFlags> flags;
            const auto [count, error] = ParseFace(text, 10, 10, 10, 3, 255,
                static_cast<OffsetFlags>(ApplyOffset::All), &expected, &flags);
            REQUIRE(error == rapidobj::rapidobj_errc::Success);
            REQUIRE(chunk.mesh.indices.buffer.size() == count);
            for (size_t i = 0; i < count; ++i) {
                CHECK(chunk.mesh.indices.buffer.data()[i].position_index == expected.data()[i].position_index);
                CHECK(chunk.mesh.indices.buffer.data()[i].texcoord_index == expected.data()[i].texcoord_index);
                CHECK(chunk.mesh.indices.buffer.data()[i].normal_index == expected.data()[i].normal_index);
                CHECK(chunk.mesh.indices.flags.data()[i] == flags.data()[i]);
            }
        }
    }
    for (const auto text : {"1 2", "1 2 3 4 5", "1 2 3garbage", "1 2 3/"}) {
        Chunk chunk{};
        CHECK_FALSE(PrototypeSimpleFace(text, chunk));
        CHECK(chunk.mesh.indices.buffer.size() == 0);
    }
}

TEST_CASE("Face sizing counts references across vector boundaries and inline comments") {
    using namespace rapidobj::detail;
    std::mt19937 random(471);
    constexpr std::string_view alphabet = "0123456789/-+ \t\r#xyz";
    for (size_t i = 0; i < 10000; ++i) {
        std::string text(random() % 300, ' ');
        for (auto& ch : text) { ch = alphabet[random() % alphabet.size()]; }
        const auto offset = static_cast<size_t>(random() % (text.size() + 1));
        const auto input = text.substr(offset);
        std::istringstream words(input.substr(0, input.find('#')));
        size_t expected = 0;
        std::string word;
        while (words >> word) { ++expected; }
        CHECK(PrototypeFaceWords(std::string_view(text).substr(offset)) == expected);
    }
    for (size_t count : {size_t{3}, size_t{4}, size_t{5}, size_t{255}}) {
        std::string face;
        for (size_t i = 0; i < count; ++i) { face += "\t -1234567/12345678/-9 "; }
        CHECK(PrototypeFaceWords(face + "# ignore 7 more words here") == count);
    }
}

TEST_CASE("Uniform relative index merge validates all flag combinations") {
    using namespace rapidobj;
    using namespace rapidobj::detail;
    for (OffsetFlags mask = 0; mask < 8; ++mask) {
        std::array<Index, 25> input, output;
        const Index value{mask & ApplyOffset::Position ? -1 : 7, -1, -1};
        input.fill(value); output.fill({-9, -9, -9});
        std::array<OffsetFlags, 25> flags;
        flags.fill(mask);
        const auto copy = [&] { return CopyIndices(output.data(), input.data(), flags.data(), 24,
            {20, 30, 40}, {100, 80, 60}).Execute(); };
        REQUIRE(copy() == rapidobj_errc::Success);
        for (size_t i = 0; i < 24; ++i) {
            CHECK(output[i].position_index == (mask & ApplyOffset::Position ? 19 : 7));
            CHECK(output[i].texcoord_index == (mask & ApplyOffset::Texcoord ? 29 : -1));
            CHECK(output[i].normal_index == (mask & ApplyOffset::Normal ? 39 : -1));
        }
        for (size_t i = 0; i < 24; ++i) {
            input[i].position_index = mask & ApplyOffset::Position ? -21 : -1;
            CHECK(copy() == rapidobj_errc::IndexOutOfBoundsError); input[i] = value;
            input[i].texcoord_index = mask & ApplyOffset::Texcoord ? -31 : -2;
            CHECK(copy() == rapidobj_errc::IndexOutOfBoundsError); input[i] = value;
            input[i].normal_index = mask & ApplyOffset::Normal ? -41 : -2;
            CHECK(copy() == rapidobj_errc::IndexOutOfBoundsError); input[i] = value;
        }
        CHECK(output.back().position_index == -9);
    }
}

TEST_CASE("Polygon limit diagnostics leave room for the extra reference") {
    using namespace rapidobj::detail;
    Buffer<rapidobj::Index> indices;
    Buffer<OffsetFlags> flags;
    indices.reserve(255); flags.reserve(255);
    std::string text;
    for (size_t i = 0; i < 256; ++i) { text += "1/1/1 "; }
    const auto [count, error] = ParseFace(text, 1, 1, 1, 3, 255,
        static_cast<OffsetFlags>(ApplyOffset::All), &indices, &flags);
    CHECK(count == 0);
    CHECK(error == rapidobj::rapidobj_errc::TooManyIndicesError);
}

TEST_CASE("Index merge validates each triplet lane and preserves relative references") {
    using namespace rapidobj;
    using namespace rapidobj::detail;
    for (size_t size = 0; size <= 35; ++size) {
        for (size_t start = 0; start < 8; ++start) {
            std::vector<Index> input(size + start, {7, -1, -1});
            std::vector<Index> output(size + start + 1, {-9, -9, -9});
            std::vector<OffsetFlags> flags(size + start);
            const auto copy = [&] {
                return CopyIndices(output.data() + start, input.data() + start, flags.data() + start,
                    size, {20, 30, 40}, {100, 80, 60}).Execute();
            };
            REQUIRE(copy() == rapidobj_errc::Success);
            CHECK(output.back().position_index == -9);
            for (size_t i = start; i < start + size; ++i) {
                CAPTURE(size); CAPTURE(start); CAPTURE(i);
                CHECK(output[i].position_index == 7);
                CHECK(output[i].texcoord_index == -1);
                CHECK(output[i].normal_index == -1);
                for (const auto bad : {Index{-1, -1, -1}, Index{100, -1, -1},
                    Index{7, -2, -1}, Index{7, 80, -1}, Index{7, -1, -2}, Index{7, -1, 60},
                    Index{std::numeric_limits<int>::max(), -1, -1}}) {
                    input[i] = bad;
                    CHECK(copy() == rapidobj_errc::IndexOutOfBoundsError);
                }
                input[i] = {-1, -1, -1};
                flags[i] = static_cast<OffsetFlags>(ApplyOffset::All);
                REQUIRE(copy() == rapidobj_errc::Success);
                CHECK(output[i].position_index == 19);
                CHECK(output[i].texcoord_index == 29);
                CHECK(output[i].normal_index == 39);
                input[i] = {7, -1, -1}; flags[i] = 0;
            }
        }
    }
    std::array<Index, 8> input{}, output{};
    std::array<OffsetFlags, 8> flags{};
    input.fill({0, -1, -1});
    CHECK(CopyIndices(output.data(), input.data(), flags.data(), 8, {}, {1, 0, 0}).Execute() == rapidobj_errc::Success);
    CHECK(CopyIndices(output.data(), input.data(), flags.data(), 8, {}, {}).Execute() == rapidobj_errc::IndexOutOfBoundsError);
}

TEST_CASE("Unused color validation preserves numeric acceptance and weight precision") {
    using namespace rapidobj::detail;
    std::vector<std::string> values = {
        "0", "-0", "+0", ".5", "1.", "-.00000000000000001", "123456789012345678",
        "1e300", "1e-300", "1e309", "-1e309", "1e-999", "nan", "inf", "+-1",
        ".", "+", "-", "1.2.3", "1e", "1x", "1,2", "1\r", "00000000000000000000000000001"
    };
    std::mt19937 random(87);
    for (size_t i = 0; i < 2000; ++i) {
        values.push_back(std::to_string(random()) + '.' + std::to_string(random()));
    }
    for (const auto& value : values) {
        for (const auto suffix : {"", " ", "\t", "#comment"}) {
            const auto token = value + suffix;
            CAPTURE(token);
            std::string_view actual = token, expected = token;
            double decoded = 0;
            const bool valid = PrototypeNextNumber(expected, decoded, true);
            CHECK(PrototypeSkipNumber(actual) == valid);
            if (valid) { CHECK(actual == expected); }
        }
    }
    for (const auto& value : values) {
        std::string_view token = value;
        double decoded = 0;
        const bool valid = PrototypeNextNumber(token, decoded, true) && token.empty();
        if (!valid) { continue; }
        PrototypeChunk chunk;
        REQUIRE(PrototypePosition("1 2 3 " + value, 1, chunk) == rapidobj::rapidobj_errc::Success);
        if (decoded != 1) {
            REQUIRE(chunk.weights.size() == 1);
            CHECK(chunk.weights[0].weight == decoded);
        } else { CHECK(chunk.weights.empty()); }
    }
}

TEST_CASE("Short decimal decoding is bit exact against the full number decoder") {
    using namespace rapidobj::detail;
    std::mt19937_64 random(0x849327);
    for (size_t i = 0; i < 100000; ++i) {
        auto text = std::to_string(random() % UINT64_C(10000000000000000));
        if (i % 3 != 0) { text.insert(static_cast<size_t>(random() % (text.size() + 1)), "."); }
        if (i % 2) { text.insert(0, "-"); }
        const auto numeric_end = text.size();
        if (i % 7 == 0) { text += "e-13"; }
        text += i % 4 ? "\tmore" : "#comment";
        CAPTURE(text);
        double expected = 0, actual = 0;
        const auto reference = fast_float::from_chars(text.data(), text.data() + text.size(), expected);
        REQUIRE(reference.ec == std::errc{});
        std::string_view remaining = text;
        REQUIRE(PrototypeNextNumber(remaining, actual, false));
        CHECK(std::bit_cast<uint64_t>(actual) == std::bit_cast<uint64_t>(expected));
        CHECK(remaining.data() == reference.ptr);
        CHECK(remaining.data() >= text.data() + numeric_end);
    }
    for (const auto text : {"-0.000000", "9007199254740992", "9007199254740993", "1.234567890123456789",
        "000000000000000000.001", "0000000/1", "000000..0", "0000000.1e309", "0000000.1e-999"}) {
        std::string_view remaining = text;
        double actual = 0, expected = 0;
        const auto reference = fast_float::from_chars(remaining.data(), remaining.data() + remaining.size(), expected);
        const bool valid = reference.ec == std::errc{} && std::isfinite(expected)
            && (reference.ptr == remaining.data() + remaining.size() || PrototypeNumberEnd(*reference.ptr));
        CHECK(PrototypeNextNumber(remaining, actual, true) == valid);
        if (valid) { CHECK(std::bit_cast<uint64_t>(actual) == std::bit_cast<uint64_t>(expected)); }
    }
}

TEST_CASE("Seeded parser stress preserves geometry across worker counts and native fallbacks") {
    const Fixture fixture;
    for (unsigned seed = 1; seed <= 4; ++seed) {
        CAPTURE(seed);
        const auto text = corpus(seed);
        REQUIRE(text.size() > 1024 * 1024);
        const auto path = fixture.write(text);
        rapidobj::PrototypeOptions options;
        options.workers = 1; options.material_names_only = true;
        Parsed reference;
        reference.result = rapidobj::ParseMemoryPrototype(text, reference.buffers(), rapidobj::MaterialLibrary::Ignore(), options);
        for (size_t workers : {size_t{1}, size_t{2}, size_t{4}, size_t{16}}) {
            for (size_t bytes : {size_t{256 * 1024}, size_t{4 * 1024 * 1024}}) {
                CAPTURE(workers); CAPTURE(bytes);
                options.workers = workers; options.chunk_bytes = bytes;
                Parsed file;
                file.result = rapidobj::ParseFilePrototype(path, file.buffers(), options);
                same(reference, file);
                CHECK(file.result.stats.input_bytes == text.size());
            }
        }
    }
}

TEST_CASE("Bounded parser workers preserve order through queue reuse and long boundaries") {
    const Fixture fixture;
    std::string text;
    const auto part = corpus(2);
    for (size_t i = 0; i < 5; ++i) { text += part + '\n'; }
    // A logical line spans more than one input block. Its comment cancels the
    // apparent continuation and must not turn its embedded text into records.
    text.insert(part.size(), "\nv 1 2 3 #" + std::string(600000, 'x') + "\\\n");
    const auto path = fixture.write(text);
    Parsed expected;
    rapidobj::PrototypeOptions options;
    options.workers = 1; options.material_names_only = true;
    expected.result = rapidobj::ParseMemoryPrototype(text, expected.buffers(), rapidobj::MaterialLibrary::Ignore(), options);
    options.chunk_bytes = 256 * 1024;
    for (const size_t workers : {size_t{2}, size_t{4}}) {
        options.workers = workers;
        Parsed actual;
        actual.result = rapidobj::ParseFilePrototype(path, actual.buffers(), options);
        same(expected, actual);
        CHECK(actual.result.stats.chunks > workers * 4);
        CHECK(actual.result.stats.workers == workers);
        CHECK(actual.result.stats.peak_inflight_text_bytes <= (3 * workers + 1) * options.chunk_bytes);
    }
    const auto error_line = static_cast<size_t>(std::count(text.begin(), text.end(), '\n')) + 1;
    fixture.write(text + "v broken\nv also_broken\n");
    Parsed invalid;
    invalid.result = rapidobj::ParseFilePrototype(path, invalid.buffers(), options);
    REQUIRE(invalid.result.polygons.error);
    CHECK(invalid.result.polygons.error.line_num == error_line);
    CHECK(invalid.positions.empty());
}

TEST_CASE("Parser stress preserves first-error diagnostics at native boundaries") {
    const Fixture fixture;
    for (size_t edge : {size_t{256 * 1024}, size_t{512 * 1024}, size_t{1024 * 1024}}) {
        for (size_t shift = 0; shift < 4; ++shift) {
            std::string text;
            while (text.size() + 8 < edge - shift) { text += "v 1 2 3\n"; }
            const auto padding = edge - shift - text.size();
            text += padding == 1 ? "\n" : '#' + std::string(padding - 2, 'x') + '\n';
            const auto errorLine = static_cast<size_t>(std::count(text.begin(), text.end(), '\n')) + 1;
            text += "vn 0 broken 1\n";
            while (text.size() < 2 * 1024 * 1024) { text += "v 4 5 6\n"; }
            text += "v another_error\n";
            const auto path = fixture.write(text);
            for (size_t workers : {size_t{2}, size_t{4}, size_t{16}}) {
                CAPTURE(edge); CAPTURE(shift); CAPTURE(workers);
                Parsed parsed;
                rapidobj::PrototypeOptions options; options.workers = workers;
                parsed.result = rapidobj::ParseFilePrototype(path, parsed.buffers(), options);
                REQUIRE(parsed.result.polygons.error);
                CHECK(parsed.result.polygons.error.line_num == errorLine);
                CHECK(parsed.result.polygons.error.code == rapidobj::make_error_code(rapidobj::rapidobj_errc::ParseError));
                CHECK(parsed.positions.empty()); CHECK(parsed.normals.empty()); CHECK(parsed.texcoords.empty());
            }
        }
    }
}

TEST_CASE("Repeated cancellation joins workers before caller storage reuse") {
    const Fixture fixture;
    const auto text = corpus(7);
    for (size_t cancelAt = 1; cancelAt <= 12; ++cancelAt) {
        const auto path = fixture.write(text);
        Parsed parsed;
        struct Checkpoint { size_t calls = 0, limit; std::thread::id caller = std::this_thread::get_id(); } checkpoint{0, cancelAt};
        rapidobj::PrototypeOptions options; options.workers = 4; options.material_names_only = true;
        options.user = &checkpoint;
        options.checkpoint = [](void* data) {
            auto& state = *static_cast<Checkpoint*>(data);
            if (std::this_thread::get_id() != state.caller) { throw std::runtime_error("wrong checkpoint thread"); }
            if (++state.calls == state.limit) { throw std::runtime_error("cancelled"); }
        };
        CHECK_THROWS_WITH(rapidobj::ParseFilePrototype(path, parsed.buffers(), options), "cancelled");
        CHECK(parsed.positions.empty()); CHECK(parsed.normals.empty()); CHECK(parsed.texcoords.empty());
        CHECK(std::filesystem::remove(path));
        options.checkpoint = nullptr;
        parsed.result = rapidobj::ParseMemoryPrototype("v 1 2 3\np 1\n", parsed.buffers(), rapidobj::MaterialLibrary::Ignore(), options);
        REQUIRE_FALSE(parsed.result.polygons.error);
        REQUIRE(parsed.positions.size() == 1);
        CHECK(parsed.positions[0] == std::array<double, 3>{1, 2, 3});
    }
}
