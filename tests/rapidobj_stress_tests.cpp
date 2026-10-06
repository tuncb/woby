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
