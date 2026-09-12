#include "scene_file.h"
#include "utf8_path.h"

#include <algorithm>
#include <tuple>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <random>
#include <system_error>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <unistd.h>
#endif

namespace woby {
namespace {

std::string lowercase(std::string value)
{
    for (char& character : value) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }

    return value;
}

bool isWobyPath(const std::filesystem::path& path)
{
    return lowercase(path.extension().string()) == ".woby";
}

std::string trim(std::string_view value)
{
    size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) {
        ++first;
    }

    size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1u]))) {
        --last;
    }

    return std::string(value.substr(first, last - first));
}

std::string stripTomlComment(std::string_view line)
{
    bool inString = false;
    bool escaped = false;
    for (size_t index = 0; index < line.size(); ++index) {
        const char character = line[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (inString && character == '\\') {
            escaped = true;
            continue;
        }
        if (character == '"') {
            inString = !inString;
            continue;
        }
        if (!inString && character == '#') {
            return std::string(line.substr(0, index));
        }
    }

    return std::string(line);
}

std::string escapeTomlString(std::string_view value)
{
    std::string escaped;
    for (char character : value) {
        switch (character) {
        case '\b':
            escaped += "\\b";
            break;
        case '\t':
            escaped += "\\t";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\f':
            escaped += "\\f";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        default:
            escaped += character;
            break;
        }
    }

    return escaped;
}

std::string parseTomlString(std::string_view value)
{
    const std::string text = trim(value);
    if (text.size() < 2u || text.front() != '"' || text.back() != '"') {
        throw std::runtime_error("Expected TOML string value.");
    }

    std::string result;
    bool escaped = false;
    for (size_t index = 1u; index + 1u < text.size(); ++index) {
        const char character = text[index];
        if (escaped) {
            switch (character) {
            case 'b':
                result += '\b';
                break;
            case 't':
                result += '\t';
                break;
            case 'n':
                result += '\n';
                break;
            case 'f':
                result += '\f';
                break;
            case 'r':
                result += '\r';
                break;
            case '"':
                result += '"';
                break;
            case '\\':
                result += '\\';
                break;
            default:
                throw std::runtime_error("Unsupported TOML string escape.");
            }
            escaped = false;
            continue;
        }
        if (character == '\\') {
            escaped = true;
            continue;
        }

        result += character;
    }
    if (escaped) {
        throw std::runtime_error("Unterminated TOML string escape.");
    }

    return result;
}

SceneUpAxis parseSceneUpAxis(std::string_view value)
{
    const std::string text = parseTomlString(value);
    if (text == "z") {
        return SceneUpAxis::z;
    }
    if (text == "y") {
        return SceneUpAxis::y;
    }

    throw std::runtime_error("Expected up axis \"z\" or \"y\".");
}

const char* sceneUpAxisName(SceneUpAxis upAxis)
{
    return upAxis == SceneUpAxis::y ? "y" : "z";
}

SceneNodeKind parseSceneNodeKind(std::string_view value)
{
    const std::string text = parseTomlString(value);
    if (text == "folder") {
        return SceneNodeKind::folder;
    }
    if (text == "file") {
        return SceneNodeKind::file;
    }
    if (text == "group") {
        return SceneNodeKind::group;
    }

    throw std::runtime_error("Expected scene node kind \"folder\", \"file\", or \"group\".");
}

const char* sceneNodeKindName(SceneNodeKind kind)
{
    switch (kind) {
    case SceneNodeKind::folder:
        return "folder";
    case SceneNodeKind::file:
        return "file";
    case SceneNodeKind::group:
        return "group";
    }

    return "folder";
}

float parseTomlFloat(std::string_view value)
{
    const std::string text = trim(value);
    size_t parsedCount = 0;
    const float result = std::stof(text, &parsedCount);
    if (!trim(std::string_view(text).substr(parsedCount)).empty()) {
        throw std::runtime_error("Expected TOML float value.");
    }

    return result;
}

int parseTomlInteger(std::string_view value)
{
    const std::string text = trim(value);
    size_t parsedCount = 0;
    const int result = std::stoi(text, &parsedCount);
    if (!trim(std::string_view(text).substr(parsedCount)).empty()) {
        throw std::runtime_error("Expected TOML integer value.");
    }

    return result;
}

bool parseTomlBool(std::string_view value)
{
    const std::string text = trim(value);
    if (text == "true") {
        return true;
    }
    if (text == "false") {
        return false;
    }

    throw std::runtime_error("Expected TOML boolean value.");
}

std::vector<float> parseTomlFloatArray(std::string_view value)
{
    const std::string text = trim(value);
    if (text.size() < 2u || text.front() != '[' || text.back() != ']') {
        throw std::runtime_error("Expected TOML float array.");
    }

    std::vector<float> values;
    std::string item;
    std::istringstream stream{std::string(text.substr(1u, text.size() - 2u))};
    while (std::getline(stream, item, ',')) {
        const std::string trimmedItem = trim(item);
        if (!trimmedItem.empty()) {
            values.push_back(parseTomlFloat(trimmedItem));
        }
    }

    return values;
}

std::array<float, 3> parseTomlFloat3(std::string_view value)
{
    const auto values = parseTomlFloatArray(value);
    if (values.size() != 3u) {
        throw std::runtime_error("Expected TOML array with 3 floats.");
    }

    return {values[0], values[1], values[2]};
}

std::array<float, 4> parseTomlFloat4(std::string_view value)
{
    const auto values = parseTomlFloatArray(value);
    if (values.size() != 4u) {
        throw std::runtime_error("Expected TOML array with 4 floats.");
    }

    return {values[0], values[1], values[2], values[3]};
}

void writeTomlFloat(std::ostream& stream, float value)
{
    stream << std::setprecision(9) << value;
}

void writeTomlFloat3(std::ostream& stream, const std::array<float, 3>& value)
{
    stream << '[';
    writeTomlFloat(stream, value[0]);
    stream << ", ";
    writeTomlFloat(stream, value[1]);
    stream << ", ";
    writeTomlFloat(stream, value[2]);
    stream << ']';
}

void writeTomlFloat4(std::ostream& stream, const std::array<float, 4>& value)
{
    stream << '[';
    writeTomlFloat(stream, value[0]);
    stream << ", ";
    writeTomlFloat(stream, value[1]);
    stream << ", ";
    writeTomlFloat(stream, value[2]);
    stream << ", ";
    writeTomlFloat(stream, value[3]);
    stream << ']';
}

std::filesystem::path sceneRelativePath(
    const std::filesystem::path& scenePath,
    const std::filesystem::path& modelPath)
{
    const std::filesystem::path basePath = std::filesystem::absolute(scenePath).parent_path().lexically_normal();
    const std::filesystem::path absoluteModelPath = std::filesystem::absolute(modelPath).lexically_normal();
    if (basePath.root_name() != absoluteModelPath.root_name()) {
        throw std::runtime_error("Cannot save model path relative to scene file: " + modelPath.string());
    }

    std::filesystem::path relativePath = absoluteModelPath.lexically_relative(basePath);
    if (relativePath.empty()) {
        relativePath = absoluteModelPath.filename();
    }

    return relativePath;
}

void assignSceneFileValue(SceneFileRecord& record, const std::string& key, std::string_view value)
{
    if (key == "path") {
        record.path = woby::pathFromUtf8(parseTomlString(value));
    } else if (key == "importer_id") {
        record.importerId = parseTomlString(value);
    } else if (key == "visible") {
        record.settings.visible = parseTomlBool(value);
    } else if (key == "scale") {
        record.settings.scale = parseTomlFloat(value);
    } else if (key == "opacity") {
        record.settings.opacity = parseTomlFloat(value);
    } else if (key == "translation") {
        record.settings.translation = parseTomlFloat3(value);
    } else if (key == "rotation_degrees") {
        record.settings.rotationDegrees = parseTomlFloat3(value);
    } else if (key == "vertex_size_scale") {
        record.vertexSizeScale = parseTomlFloat(value);
    }
}

void assignSceneGroupValue(SceneGroupRecord& record, const std::string& key, std::string_view value)
{
    if (key == "name") {
        record.name = parseTomlString(value);
    } else if (key == "visible") {
        record.settings.visible = parseTomlBool(value);
    } else if (key == "analysis_a") {
        record.settings.comparison.a = parseTomlBool(value);
    } else if (key == "analysis_b") {
        record.settings.comparison.b = parseTomlBool(value);
    } else if (key == "show_solid_mesh") {
        record.settings.showSolidMesh = parseTomlBool(value);
    } else if (key == "show_triangles") {
        record.settings.showTriangles = parseTomlBool(value);
    } else if (key == "show_vertices") {
        record.settings.showVertices = parseTomlBool(value);
    } else if (key == "scale") {
        record.settings.scale = parseTomlFloat(value);
    } else if (key == "opacity") {
        record.settings.opacity = parseTomlFloat(value);
    } else if (key == "vertex_size_scale") {
        record.settings.vertexSizeScale = parseTomlFloat(value);
    } else if (key == "translation") {
        record.settings.translation = parseTomlFloat3(value);
    } else if (key == "rotation_degrees") {
        record.settings.rotationDegrees = parseTomlFloat3(value);
    } else if (key == "color") {
        record.settings.color = parseTomlFloat4(value);
    }
}

void assignSceneNodeValue(SceneNodeRecord& record, const std::string& key, std::string_view value)
{
    if (key == "kind") {
        record.kind = parseSceneNodeKind(value);
    } else if (key == "name") {
        record.name = parseTomlString(value);
    } else if (key == "parent") {
        record.parentIndex = parseTomlInteger(value);
    } else if (key == "file_index") {
        record.fileIndex = parseTomlInteger(value);
    } else if (key == "group_index") {
        record.groupIndex = parseTomlInteger(value);
    } else if (key == "visible") {
        record.settings.visible = parseTomlBool(value);
    } else if (key == "scale") {
        record.settings.scale = parseTomlFloat(value);
    } else if (key == "opacity") {
        record.settings.opacity = parseTomlFloat(value);
    } else if (key == "translation") {
        record.settings.translation = parseTomlFloat3(value);
    } else if (key == "rotation_degrees") {
        record.settings.rotationDegrees = parseTomlFloat3(value);
    }
}

void assignCameraValue(SceneCamera& camera, const std::string& key, std::string_view value)
{
    if (key == "target") { camera.target = parseTomlFloat3(value);
    } else if (key == "yaw_radians") { camera.yawRadians = parseTomlFloat(value);
    } else if (key == "pitch_radians") { camera.pitchRadians = parseTomlFloat(value);
    } else if (key == "roll_radians") { camera.rollRadians = parseTomlFloat(value);
    } else if (key == "distance") { camera.distance = parseTomlFloat(value);
    } else if (key == "vertical_fov_degrees") { camera.verticalFovDegrees = parseTomlFloat(value);
    } else if (key == "near_plane") { camera.nearPlane = parseTomlFloat(value);
    }
    // Report invalid numbers at their source line; clamp only after
    // all fields are read so near-plane limits are order-independent.
    (void)normalizedSceneCamera(camera);
}

void assignComparisonValue(SceneComparisonRecord& record, const std::string& key, std::string_view value)
{
    if (key == "name") { record.name = parseTomlString(value);
    } else if (key == "translation") { record.translation = parseTomlFloat3(value);
    } else if (key == "analysis_enabled") {
        record.settings.enabled = parseTomlBool(value);
    } else if (key == "analysis_mode") {
        const auto mode = parseTomlString(value);
        if (mode == "distance") { record.settings.mode = ComparisonMode::distance; }
        else if (mode == "a") { record.settings.mode = ComparisonMode::original; }
        else if (mode == "b") { record.settings.mode = ComparisonMode::repaired; }
        else if (mode == "overlay") { record.settings.mode = ComparisonMode::overlay; }
        else if (mode == "surface_quality") { record.settings.mode = ComparisonMode::surfaceQuality; }
        else { throw std::runtime_error("Unknown analysis mode."); }
    } else if (key == "analysis_distance_on_a") {
        record.settings.distanceOnOriginal = parseTomlBool(value);
    } else if (key == "diagnostic_side") {
        const auto side = parseTomlString(value);
        if (side == "a") { record.settings.diagnosticSide = ComparisonSide::a; }
        else if (side == "b") { record.settings.diagnosticSide = ComparisonSide::b; }
        else { throw std::runtime_error("Unknown diagnostic side."); }
    } else if (key == "diagnostic_category") {
        const auto category = parseTomlString(value);
        if (category == "boundary") { record.settings.diagnosticCategory = DiagnosticCategory::boundary; }
        else if (category == "non_manifold") { record.settings.diagnosticCategory = DiagnosticCategory::nonManifold; }
        else if (category == "winding") { record.settings.diagnosticCategory = DiagnosticCategory::winding; }
        else if (category == "duplicate_points") { record.settings.diagnosticCategory = DiagnosticCategory::duplicatePoints; }
        else if (category == "duplicate_triangles") { record.settings.diagnosticCategory = DiagnosticCategory::duplicateTriangles; }
        else { throw std::runtime_error("Unknown diagnostic category."); }
    } else if (key == "duplicate_points_enabled") { record.settings.duplicates.points = parseTomlBool(value);
    } else if (key == "duplicate_triangles_enabled") { record.settings.duplicates.triangles = parseTomlBool(value);
    } else if (key == "show_duplicate_points") { record.settings.duplicates.showPoints = parseTomlBool(value);
    } else if (key == "show_duplicate_triangles") { record.settings.duplicates.showTriangles = parseTomlBool(value);
    } else if (key == "analysis_tolerance") {
        record.settings.tolerance = parseTomlFloat(value);
    } else if (key == "quality_metric") {
        record.settings.quality.metric = parseSurfaceQualityMetric(parseTomlString(value));
    } else if (key == "quality_on_a") {
        record.settings.quality.onOriginal = parseTomlBool(value);
    } else if (key == "quality_minimum_enabled") {
        record.settings.quality.minimumEnabled = parseTomlBool(value);
    } else if (key == "quality_maximum_enabled") {
        record.settings.quality.maximumEnabled = parseTomlBool(value);
    } else if (key == "quality_minimum_size") {
        record.settings.quality.minimumSize = parseTomlFloat(value);
    } else if (key == "quality_maximum_size") {
        record.settings.quality.maximumSize = parseTomlFloat(value);
    } else if (key == "analysis_color_range") {
        record.settings.colorRange = parseTomlFloat(value);
    } else if (key == "analysis_show_edges") {
        record.settings.showEdges = parseTomlBool(value);
    } else if (key == "analysis_show_boundaries") {
        record.settings.showBoundaries = parseTomlBool(value);
    } else if (key == "analysis_show_non_manifold") {
        record.settings.showNonManifold = parseTomlBool(value);
    }
}

void writeComparisonSettings(std::ostream& stream, const ComparisonSettings& settings)
{
    const auto comparison = normalizedComparisonSettings(settings);
    const char* mode = "distance";
    switch (comparison.mode) {
    case ComparisonMode::distance: break;
    case ComparisonMode::original: mode = "a"; break;
    case ComparisonMode::repaired: mode = "b"; break;
    case ComparisonMode::overlay: mode = "overlay"; break;
    case ComparisonMode::surfaceQuality: mode = "surface_quality"; break;
    }
    stream << "analysis_enabled = " << (comparison.enabled ? "true" : "false") << "\n";
    stream << "analysis_mode = \"" << mode << "\"\n";
    stream << "analysis_distance_on_a = " << (comparison.distanceOnOriginal ? "true" : "false") << "\n";
    stream << "diagnostic_side = \"" << (comparison.diagnosticSide == ComparisonSide::a ? "a" : "b") << "\"\n";
    const char* category = comparison.diagnosticCategory == DiagnosticCategory::boundary ? "boundary"
        : comparison.diagnosticCategory == DiagnosticCategory::nonManifold ? "non_manifold"
        : comparison.diagnosticCategory == DiagnosticCategory::duplicatePoints ? "duplicate_points"
        : comparison.diagnosticCategory == DiagnosticCategory::duplicateTriangles ? "duplicate_triangles" : "winding";
    stream << "diagnostic_category = \"" << category << "\"\n";
    stream << "analysis_tolerance = "; writeTomlFloat(stream, comparison.tolerance); stream << "\n";
    stream << "analysis_color_range = "; writeTomlFloat(stream, comparison.colorRange); stream << "\n";
    stream << "analysis_show_edges = " << (comparison.showEdges ? "true" : "false") << "\n";
    stream << "analysis_show_boundaries = " << (comparison.showBoundaries ? "true" : "false") << "\n";
    stream << "analysis_show_non_manifold = " << (comparison.showNonManifold ? "true" : "false") << "\n";

    stream << "duplicate_points_enabled = " << (comparison.duplicates.points ? "true" : "false") << "\n";
    stream << "duplicate_triangles_enabled = " << (comparison.duplicates.triangles ? "true" : "false") << "\n";
    stream << "show_duplicate_points = " << (comparison.duplicates.showPoints ? "true" : "false") << "\n";
    stream << "show_duplicate_triangles = " << (comparison.duplicates.showTriangles ? "true" : "false") << "\n";

    stream << "quality_metric = \"" << surfaceQualityMetricKey(comparison.quality.metric) << "\"\n";
    stream << "quality_on_a = " << (comparison.quality.onOriginal ? "true" : "false") << "\n";
    stream << "quality_minimum_enabled = " << (comparison.quality.minimumEnabled ? "true" : "false") << "\n";
    stream << "quality_maximum_enabled = " << (comparison.quality.maximumEnabled ? "true" : "false") << "\n";
    stream << "quality_minimum_size = "; writeTomlFloat(stream, comparison.quality.minimumSize); stream << "\n";
    stream << "quality_maximum_size = "; writeTomlFloat(stream, comparison.quality.maximumSize); stream << "\n";
}

void writeCamera(std::ostream& stream, const SceneCamera& value)
{
    const auto camera = normalizedSceneCamera(value);
    stream << "target = ";
    writeTomlFloat3(stream, camera.target);
    stream << "\nyaw_radians = ";
    writeTomlFloat(stream, camera.yawRadians);
    stream << "\npitch_radians = ";
    writeTomlFloat(stream, camera.pitchRadians);
    stream << "\nroll_radians = ";
    writeTomlFloat(stream, camera.rollRadians);
    stream << "\ndistance = ";
    writeTomlFloat(stream, camera.distance);
    stream << "\nvertical_fov_degrees = ";
    writeTomlFloat(stream, camera.verticalFovDegrees);
    stream << "\nnear_plane = ";
    writeTomlFloat(stream, camera.nearPlane);
    stream << "\n";
}

void writeAppearance(std::ostream& stream, const SceneGroupSettings& settings)
{
    stream << "visible = " << (settings.visible ? "true" : "false") << "\n";
    stream << "show_solid_mesh = " << (settings.showSolidMesh ? "true" : "false") << "\n";
    stream << "show_triangles = " << (settings.showTriangles ? "true" : "false") << "\n";
    stream << "show_vertices = " << (settings.showVertices ? "true" : "false") << "\n";
    stream << "scale = ";
    writeTomlFloat(stream, settings.scale);
    stream << "\n";
    stream << "opacity = ";
    writeTomlFloat(stream, settings.opacity);
    stream << "\n";
    stream << "vertex_size_scale = ";
    writeTomlFloat(stream, settings.vertexSizeScale);
    stream << "\n";
    stream << "translation = ";
    writeTomlFloat3(stream, settings.translation);
    stream << "\n";
    stream << "rotation_degrees = ";
    writeTomlFloat3(stream, settings.rotationDegrees);
    stream << "\n";
    stream << "color = ";
    writeTomlFloat4(stream, settings.color);
    stream << "\n";
}

} // namespace

std::filesystem::path sceneAbsolutePath(
    const std::filesystem::path& scenePath,
    const std::filesystem::path& storedPath)
{
    if (storedPath.is_absolute()) {
        return storedPath.lexically_normal();
    }

    return (std::filesystem::absolute(scenePath).parent_path() / storedPath).lexically_normal();
}

std::filesystem::path sceneSavePathWithExtension(std::filesystem::path path)
{
    if (!isWobyPath(path)) {
        path += ".woby";
    }

    return path;
}

bool sceneContentEqual(const SceneDocument& a, const SceneDocument& b)
{
    // The live camera is excluded so ordinary navigation stays transient.
    // Named checkpoints, including their cameras, are editable document content.
    return std::tie(a.annotations, a.views, a.comparisons, a.comparison, a.masterVertexPointSize, a.showOrigin,
               a.showGrid, a.showDimensions, a.upAxis, a.files, a.nodes)
        == std::tie(b.annotations, b.views, b.comparisons, b.comparison, b.masterVertexPointSize, b.showOrigin,
               b.showGrid, b.showDimensions, b.upAxis, b.files, b.nodes);
}

SceneDocument readSceneDocument(const std::filesystem::path& scenePath)
{
    std::ifstream stream(scenePath);
    if (!stream) {
        throw std::runtime_error("Failed to open scene file: " + scenePath.string());
    }

    enum class Section {
        root,
        annotation,
        annotationSegment,
        view,
        viewCamera,
        viewObject,
        viewPart,
        camera,
        file,
        group,
        node,
        comparison,
        comparisonA,
        comparisonB,
    };

    SceneDocument document;
    Section section = Section::root;
    bool viewCameraSeen = false;
    size_t lineNumber = 0;

    std::string line;
    while (std::getline(stream, line)) {
        ++lineNumber;
        const std::string text = trim(stripTomlComment(line));
        if (text.empty()) {
            continue;
        }

        try {
            if (text == "[[annotations]]") {
                document.annotations.emplace_back(); section = Section::annotation; continue;
            }
            if (text == "[[annotations.segments]]") {
                if (document.annotations.empty()) { throw std::runtime_error("Annotation segment appeared before annotation."); }
                if (document.annotations.back().geometry.segments.size() >= 1000000) { throw std::runtime_error("Too many annotation segments."); }
                document.annotations.back().geometry.segments.emplace_back(); section = Section::annotationSegment; continue;
            }
            if (text == "[[views]]") {
                document.views.emplace_back();
                viewCameraSeen = false;
                section = Section::view;
                continue;
            }
            if (text == "[views.camera]" || text == "[[views.objects]]" || text == "[[views.parts]]") {
                if (document.views.empty()) { throw std::runtime_error("View child appeared before view."); }
                if (text == "[views.camera]") {
                    if (viewCameraSeen) { throw std::runtime_error("Duplicate view camera table."); }
                    viewCameraSeen = true;
                    section = Section::viewCamera;
                }
                else if (text == "[[views.parts]]") {
                    document.views.back().parts.emplace_back();
                    section = Section::viewPart;
                }
                else {
                    document.views.back().objects.emplace_back();
                    section = Section::viewObject;
                }
                continue;
            }
            if (text == "[camera]") {
                if (document.camera) { throw std::runtime_error("Duplicate camera table."); }
                document.camera.emplace();
                section = Section::camera;
                continue;
            }
            if (text == "[[analyses]]") {
                document.comparisons.emplace_back();
                section = Section::comparison;
                continue;
            }
            if (text == "[[analyses.a]]" || text == "[[analyses.b]]") {
                if (document.comparisons.empty()) { throw std::runtime_error("Analysis input appeared before analysis."); }
                const bool a = text == "[[analyses.a]]";
                (a ? document.comparisons.back().a : document.comparisons.back().b).emplace_back();
                section = a ? Section::comparisonA : Section::comparisonB;
                continue;
            }
            if (text == "[[files]]") {
                document.files.emplace_back();
                section = Section::file;
                continue;
            }
            if (text == "[[files.groups]]") {
                if (document.files.empty()) {
                    throw std::runtime_error("Group table appeared before any file table.");
                }
                document.files.back().groups.emplace_back();
                section = Section::group;
                continue;
            }
            if (text == "[[nodes]]") {
                document.nodes.emplace_back();
                section = Section::node;
                continue;
            }

            const size_t separator = text.find('=');
            if (separator == std::string::npos) {
                throw std::runtime_error("Expected TOML key/value pair.");
            }

            const std::string key = trim(std::string_view(text).substr(0, separator));
            const std::string value = trim(std::string_view(text).substr(separator + 1u));
            if (section == Section::root) {
                if (key == "version") {
                    const int version = parseTomlInteger(value);
                    if (version != 2 && version != 3 && version != 4 && version != 5 && version != 6 && version != 7 && version != 8) {
                        throw std::runtime_error("Unsupported scene version.");
                    }
                } else if (key == "master_vertex_point_size") {
                    document.masterVertexPointSize = parseTomlFloat(value);
                } else if (key == "show_origin") {
                    document.showOrigin = parseTomlBool(value);
                } else if (key == "show_grid") {
                    document.showGrid = parseTomlBool(value);
                } else if (key == "show_dimensions") {
                    document.showDimensions = parseTomlBool(value);
                } else if (key == "up_axis") {
                    document.upAxis = parseSceneUpAxis(value);
                } else if (key == "analysis_enabled") {
                    document.comparison.enabled = parseTomlBool(value);
                } else if (key == "analysis_mode") {
                    const auto mode = parseTomlString(value);
                    if (mode == "distance") { document.comparison.mode = ComparisonMode::distance; }
                    else if (mode == "a") { document.comparison.mode = ComparisonMode::original; }
                    else if (mode == "b") { document.comparison.mode = ComparisonMode::repaired; }
                    else if (mode == "overlay") { document.comparison.mode = ComparisonMode::overlay; }
                    else { throw std::runtime_error("Unknown analysis mode."); }
                } else if (key == "analysis_distance_on_a") {
                    document.comparison.distanceOnOriginal = parseTomlBool(value);
                } else if (key == "analysis_tolerance") {
                    document.comparison.tolerance = parseTomlFloat(value);
                } else if (key == "analysis_color_range") {
                    document.comparison.colorRange = parseTomlFloat(value);
                } else if (key == "analysis_show_edges") {
                    document.comparison.showEdges = parseTomlBool(value);
                } else if (key == "analysis_show_boundaries") {
                    document.comparison.showBoundaries = parseTomlBool(value);
                } else if (key == "analysis_show_non_manifold") {
                    document.comparison.showNonManifold = parseTomlBool(value);
                }
            } else if (section == Section::annotation) {
                auto& item = document.annotations.back();
                if (key == "name") { item.settings.name = parseTomlString(value); }
                else if (key == "note") { item.settings.note = parseTomlString(value); }
                else if (key == "target_name") { item.targetName = parseTomlString(value); }
                else if (key == "file_index") { item.fileIndex = parseTomlInteger(value); }
                else if (key == "group_index") { item.groupIndex = parseTomlInteger(value); }
                else if (key == "visible") { item.settings.visible = parseTomlBool(value); }
                else if (key == "locked") { item.settings.locked = parseTomlBool(value); }
                else if (key == "width") { item.settings.width = parseTomlFloat(value); }
                else if (key == "color") { item.settings.color = parseTomlFloat4(value); }
                else if (key == "fingerprint") { item.geometry.fingerprint = parseTomlString(value); }
                else if (key == "homogeneous_depth") { item.geometry.homogeneousDepth = parseTomlBool(value); }
                else if (key == "shape") {
                    const auto shape = parseTomlString(value);
                    if (shape == "line") { item.geometry.shape = AnnotationShape::line; }
                    else if (shape == "rectangle") { item.geometry.shape = AnnotationShape::rectangle; }
                    else { throw std::runtime_error("Unknown annotation shape."); }
                } else if (key == "projector") {
                    const auto values = parseTomlFloatArray(value);
                    if (values.size() != 16) { throw std::runtime_error("Expected 16 annotation projector values."); }
                    std::copy(values.begin(), values.end(), item.geometry.projector.begin());
                } else if (key == "start" || key == "end") {
                    const auto values = parseTomlFloatArray(value);
                    if (values.size() != 2) { throw std::runtime_error("Expected 2 annotation control coordinates."); }
                    auto& control = key == "start" ? item.geometry.start : item.geometry.end;
                    control = {values[0], values[1]};
                }
            } else if (section == Section::annotationSegment) {
                auto& segment = document.annotations.back().geometry.segments.back();
                if (key == "triangle") {
                    const int triangle = parseTomlInteger(value);
                    if (triangle < 0) { throw std::runtime_error("Invalid annotation triangle."); }
                    segment.triangle = static_cast<uint32_t>(triangle);
                } else if (key == "a") { segment.a = parseTomlFloat3(value); }
                else if (key == "b") { segment.b = parseTomlFloat3(value); }
            } else if (section == Section::view) {
                auto& view = document.views.back();
                if (key == "name") { view.name = parseTomlString(value); }
                else if (key == "show_origin") { view.scene.showOrigin = parseTomlBool(value); }
                else if (key == "show_grid") { view.scene.showGrid = parseTomlBool(value); }
                else if (key == "show_dimensions") { view.scene.showDimensions = parseTomlBool(value); }
                else if (key == "up_axis") { view.scene.upAxis = parseSceneUpAxis(value); }
                else if (key == "master_vertex_point_size") { view.scene.masterVertexPointSize = parseTomlFloat(value); }
            } else if (section == Section::viewCamera) {
                assignCameraValue(document.views.back().scene.camera, key, value);
            } else if (section == Section::viewPart) {
                auto& part = document.views.back().parts.back();
                if (key == "analysis_index") { part.comparisonIndex = parseTomlInteger(value); }
                else if (key == "file_index") { part.fileIndex = parseTomlInteger(value); }
                else if (key == "group_index") { part.groupIndex = parseTomlInteger(value); }
                else if (key == "enabled") { part.enabled = parseTomlBool(value); }
                else if (key == "side") {
                    const auto side = parseTomlString(value);
                    if (side == "a") { part.side = ComparisonSide::a; }
                    else if (side == "b") { part.side = ComparisonSide::b; }
                    else { throw std::runtime_error("Unknown view analysis side."); }
                }
            } else if (section == Section::viewObject) {
                auto& object = document.views.back().objects.back();
                if (key == "kind") {
                    const auto kind = parseTomlString(value);
                    if (kind == "folder") { object.kind = ViewObjectKind::folder; }
                    else if (kind == "file") { object.kind = ViewObjectKind::file; }
                    else if (kind == "group") { object.kind = ViewObjectKind::group; }
                    else if (kind == "analysis") { object.kind = ViewObjectKind::comparison; }
                    else if (kind == "annotation") { object.kind = ViewObjectKind::annotation; }
                    else { throw std::runtime_error("Unknown view object kind."); }
                } else if (key == "index") { object.index = parseTomlInteger(value); }
                else if (key == "group_index") { object.groupIndex = parseTomlInteger(value); }
                else if (key == "selection_order") { object.settings.selectionOrder = parseTomlInteger(value); }
                else if (key.starts_with("analysis_") || key.starts_with("quality_")) {
                    SceneComparisonRecord comparison;
                    comparison.settings = object.settings.comparison;
                    assignComparisonValue(comparison, key, value);
                    object.settings.comparison = comparison.settings;
                } else {
                    SceneGroupRecord group;
                    group.settings = object.settings.appearance;
                    assignSceneGroupValue(group, key, value);
                    object.settings.appearance = group.settings;
                }
            } else if (section == Section::camera) {
                assignCameraValue(*document.camera, key, value);
            } else if (section == Section::comparison) {
                assignComparisonValue(document.comparisons.back(), key, value);
            } else if (section == Section::comparisonA || section == Section::comparisonB) {
                auto& comparison = document.comparisons.back();
                auto& part = (section == Section::comparisonA ? comparison.a : comparison.b).back();
                if (key == "file_index") { part.fileIndex = parseTomlInteger(value); }
                else if (key == "group_index") { part.groupIndex = parseTomlInteger(value); }
                else if (key == "name") { part.name = parseTomlString(value); }
                else if (key == "enabled") { part.enabled = parseTomlBool(value); }
            } else if (section == Section::file) {
                assignSceneFileValue(document.files.back(), key, value);
            } else if (section == Section::group) {
                assignSceneGroupValue(document.files.back().groups.back(), key, value);
            } else {
                assignSceneNodeValue(document.nodes.back(), key, value);
            }
        } catch (const std::exception& exception) {
            throw std::runtime_error(
                scenePath.string()
                + ":"
                + std::to_string(lineNumber)
                + ": "
                + exception.what());
        }
    }

    if (document.camera) { document.camera = normalizedSceneCamera(*document.camera); }
    for (auto& view : document.views) { view.scene.camera = normalizedSceneCamera(view.scene.camera); }

    for (const auto& file : document.files) {
        if (file.path.empty()) {
            throw std::runtime_error("Scene contains a file entry without a path.");
        }
    }

    for (size_t nodeIndex = 0; nodeIndex < document.nodes.size(); ++nodeIndex) {
        const auto& node = document.nodes[nodeIndex];
        if (node.parentIndex >= static_cast<int>(nodeIndex)) {
            throw std::runtime_error("Scene node parent must appear before its child.");
        }
        if (node.kind == SceneNodeKind::file
            && (node.fileIndex < 0 || node.fileIndex >= static_cast<int>(document.files.size()))) {
            throw std::runtime_error("Scene file node references an invalid file index.");
        }
        if (node.kind == SceneNodeKind::group) {
            if (node.fileIndex < 0 || node.fileIndex >= static_cast<int>(document.files.size())) {
                throw std::runtime_error("Scene group node references an invalid file index.");
            }

            const auto& file = document.files[static_cast<size_t>(node.fileIndex)];
            if (node.groupIndex < 0 || node.groupIndex >= static_cast<int>(file.groups.size())) {
                throw std::runtime_error("Scene group node references an invalid group index.");
            }
        }
    }

    for (auto& item : document.annotations) {
        validateAnnotationGeometry(item.geometry);
        item.settings = normalizedAnnotationSettings(std::move(item.settings));
        if (item.fileIndex == -1 && item.groupIndex == -1) { continue; }
        if (item.fileIndex < 0 || static_cast<size_t>(item.fileIndex) >= document.files.size()
            || item.groupIndex < 0 || static_cast<size_t>(item.groupIndex) >= document.files[static_cast<size_t>(item.fileIndex)].groups.size()) {
            throw std::runtime_error("Annotation references an invalid source part.");
        }
    }
    // Migrate v2-v4 membership into one object, then discard legacy input fields.
    SceneComparisonRecord legacy;
    legacy.name = "Analysis 1";
    legacy.settings = normalizedComparisonSettings(document.comparison);
    for (size_t f = 0; f < document.files.size(); ++f) {
        auto& file = document.files[f];
        for (size_t g = 0; g < file.groups.size(); ++g) {
            auto& group = file.groups[g];
            const SceneComparisonPartRecord part{static_cast<int>(f), static_cast<int>(g),
                pathToUtf8(file.path.filename()) + " / " + group.name};
            if (group.settings.comparison.a) { legacy.a.push_back(part); }
            if (group.settings.comparison.b) { legacy.b.push_back(part); }
            group.settings.comparison = {};
        }
    }
    if (document.comparisons.empty() && (!legacy.a.empty() || !legacy.b.empty()
        || document.comparison != ComparisonSettings{})) {
        document.comparisons.push_back(std::move(legacy));
    }
    document.comparison = {};
    for (auto& comparison : document.comparisons) {
        if (comparison.name.empty()) { comparison.name = "Analysis"; }
        comparison.settings = normalizedComparisonSettings(comparison.settings);
        if (comparison.translation && !std::all_of(comparison.translation->begin(), comparison.translation->end(), [](float v) { return std::isfinite(v); })) {
            comparison.translation = std::array<float, 3>{};
        }
        for (auto* members : {&comparison.a, &comparison.b}) {
            for (const auto& part : *members) {
                if (part.fileIndex == -1 && part.groupIndex == -1) { continue; }
                if (part.fileIndex < 0 || static_cast<size_t>(part.fileIndex) >= document.files.size()
                    || part.groupIndex < 0 || static_cast<size_t>(part.groupIndex)
                        >= document.files[static_cast<size_t>(part.fileIndex)].groups.size()) {
                    throw std::runtime_error("Analysis references an invalid source part.");
                }
            }
            std::sort(members->begin(), members->end(), [](const auto& a, const auto& b) {
                return std::tie(a.fileIndex, a.groupIndex, a.name) < std::tie(b.fileIndex, b.groupIndex, b.name);
            });
            members->erase(std::unique(members->begin(), members->end(), [](const auto& a, const auto& b) {
                return a.fileIndex == b.fileIndex && a.groupIndex == b.groupIndex
                    && (a.fileIndex >= 0 || a.name == b.name);
            }), members->end());
        }
    }
    return document;
}

void writeSceneDocument(const std::filesystem::path& scenePath, const SceneDocument& document, bool overwrite)
{
    // Serialize before touching the destination. Relative paths use its final location.
    std::ostringstream stream;
    stream.exceptions(std::ios::badbit | std::ios::failbit);

    stream << "# woby scene\n";
    stream << "version = 8\n";
    stream << "master_vertex_point_size = ";
    writeTomlFloat(stream, document.masterVertexPointSize);
    stream << "\n";
    stream << "show_origin = " << (document.showOrigin ? "true" : "false") << "\n";
    stream << "show_grid = " << (document.showGrid ? "true" : "false") << "\n";
    stream << "show_dimensions = " << (document.showDimensions ? "true" : "false") << "\n";
    stream << "up_axis = \"" << sceneUpAxisName(document.upAxis) << "\"\n\n";

    if (document.camera) {
        stream << "[camera]\n";
        writeCamera(stream, *document.camera);
    }

    for (const auto& file : document.files) {
        const std::filesystem::path relativeModelPath = sceneRelativePath(scenePath, file.path);
        stream << "\n[[files]]\n";
        const auto encodedPath = relativeModelPath.generic_u8string();
        const std::string pathText(reinterpret_cast<const char*>(encodedPath.data()), encodedPath.size());
        stream << "path = \"" << escapeTomlString(pathText) << "\"\n";
        if (!file.importerId.empty()) {
            stream << "importer_id = \"" << escapeTomlString(file.importerId) << "\"\n";
        }
        stream << "visible = " << (file.settings.visible ? "true" : "false") << "\n";
        stream << "scale = ";
        writeTomlFloat(stream, file.settings.scale);
        stream << "\n";
        stream << "opacity = ";
        writeTomlFloat(stream, file.settings.opacity);
        stream << "\n";
        stream << "translation = ";
        writeTomlFloat3(stream, file.settings.translation);
        stream << "\n";
        stream << "rotation_degrees = ";
        writeTomlFloat3(stream, file.settings.rotationDegrees);
        stream << "\n";
        stream << "vertex_size_scale = ";
        writeTomlFloat(stream, file.vertexSizeScale);
        stream << "\n";

        for (const auto& group : file.groups) {
            stream << "\n[[files.groups]]\n";
            stream << "name = \"" << escapeTomlString(group.name) << "\"\n";
            writeAppearance(stream, group.settings);
        }
    }

    for (const auto& node : document.nodes) {
        stream << "\n[[nodes]]\n";
        stream << "kind = \"" << sceneNodeKindName(node.kind) << "\"\n";
        stream << "name = \"" << escapeTomlString(node.name) << "\"\n";
        stream << "parent = " << node.parentIndex << "\n";
        stream << "file_index = " << node.fileIndex << "\n";
        stream << "group_index = " << node.groupIndex << "\n";
        stream << "visible = " << (node.settings.visible ? "true" : "false") << "\n";
        stream << "scale = ";
        writeTomlFloat(stream, node.settings.scale);
        stream << "\n";
        stream << "opacity = ";
        writeTomlFloat(stream, node.settings.opacity);
        stream << "\n";
        stream << "translation = ";
        writeTomlFloat3(stream, node.settings.translation);
        stream << "\n";
        stream << "rotation_degrees = ";
        writeTomlFloat3(stream, node.settings.rotationDegrees);
        stream << "\n";
    }
    for (const auto& item : document.annotations) {
        validateAnnotationGeometry(item.geometry);
        stream << "\n[[annotations]]\n";
        stream << "name = \"" << escapeTomlString(item.settings.name) << "\"\n";
        stream << "note = \"" << escapeTomlString(item.settings.note) << "\"\n";
        stream << "target_name = \"" << escapeTomlString(item.targetName) << "\"\n";
        stream << "file_index = " << item.fileIndex << "\ngroup_index = " << item.groupIndex << "\n";
        stream << "visible = " << (item.settings.visible ? "true" : "false") << "\n";
        stream << "locked = " << (item.settings.locked ? "true" : "false") << "\n";
        stream << "width = "; writeTomlFloat(stream, item.settings.width); stream << "\n";
        stream << "color = "; writeTomlFloat4(stream, item.settings.color); stream << "\n";
        stream << "shape = \"" << (item.geometry.shape == AnnotationShape::line ? "line" : "rectangle") << "\"\n";
        stream << "fingerprint = \"" << escapeTomlString(item.geometry.fingerprint) << "\"\n";
        stream << "homogeneous_depth = " << (item.geometry.homogeneousDepth ? "true" : "false") << "\n";
        const auto array = [&](const char* key, const auto& values) {
            stream << key << " = [";
            for (size_t i = 0; i < values.size(); ++i) { if (i) { stream << ", "; } writeTomlFloat(stream, values[i]); }
            stream << "]\n";
        };
        array("projector", item.geometry.projector); array("start", item.geometry.start); array("end", item.geometry.end);
        for (const auto& segment : item.geometry.segments) {
            stream << "\n[[annotations.segments]]\ntriangle = " << segment.triangle << "\n";
            array("a", segment.a); array("b", segment.b);
        }
    }
    for (const auto& record : document.comparisons) {
        stream << "\n[[analyses]]\n";
        stream << "name = \"" << escapeTomlString(record.name) << "\"\n";
        if (record.translation) {
            stream << "translation = "; writeTomlFloat3(stream, *record.translation); stream << "\n";
        }
        writeComparisonSettings(stream, record.settings);

        for (const auto side : {ComparisonSide::a, ComparisonSide::b}) {
            for (const auto& part : side == ComparisonSide::a ? record.a : record.b) {
                stream << (side == ComparisonSide::a ? "\n[[analyses.a]]\n" : "\n[[analyses.b]]\n");
                stream << "file_index = " << part.fileIndex << "\n";
                stream << "group_index = " << part.groupIndex << "\n";
                stream << "name = \"" << escapeTomlString(part.name) << "\"\n";
                stream << "enabled = " << (part.enabled ? "true" : "false") << "\n";
            }
        }
    }
    for (const auto& view : document.views) {
        stream << "\n[[views]]\nname = \"" << escapeTomlString(view.name) << "\"\n";
        stream << "show_origin = " << (view.scene.showOrigin ? "true" : "false") << "\n";
        stream << "show_grid = " << (view.scene.showGrid ? "true" : "false") << "\n";
        stream << "show_dimensions = " << (view.scene.showDimensions ? "true" : "false") << "\n";
        stream << "up_axis = \"" << sceneUpAxisName(view.scene.upAxis) << "\"\n";
        stream << "master_vertex_point_size = "; writeTomlFloat(stream, view.scene.masterVertexPointSize);
        stream << "\n[views.camera]\n";
        writeCamera(stream, view.scene.camera);
        for (const auto& object : view.objects) {
            const char* kind = "group";
            switch (object.kind) {
            case ViewObjectKind::folder: kind = "folder"; break;
            case ViewObjectKind::file: kind = "file"; break;
            case ViewObjectKind::group: break;
            case ViewObjectKind::comparison: kind = "analysis"; break;
            case ViewObjectKind::annotation: kind = "annotation"; break;
            }
            stream << "\n[[views.objects]]\nkind = \"" << kind << "\"\n";
            stream << "index = " << object.index << "\ngroup_index = " << object.groupIndex << "\n";
            stream << "selection_order = " << object.settings.selectionOrder << "\n";
            writeAppearance(stream, object.settings.appearance);
            if (object.kind == ViewObjectKind::comparison) {
                writeComparisonSettings(stream, object.settings.comparison);
            }
        }
        for (const auto& part : view.parts) {
            stream << "\n[[views.parts]]\nanalysis_index = " << part.comparisonIndex << "\n";
            stream << "file_index = " << part.fileIndex << "\ngroup_index = " << part.groupIndex << "\n";
            stream << "side = \"" << (part.side == ComparisonSide::a ? "a" : "b") << "\"\n";
            stream << "enabled = " << (part.enabled ? "true" : "false") << "\n";
        }
    }
    const auto contents = stream.str();
    // An exclusively created sibling directory owns the temporary file. Both the
    // file and destination are on the same filesystem, including on Windows.
    std::filesystem::path temporaryDirectory;
    std::random_device random;
    for (int attempt = 0; attempt < 32; ++attempt) {
        auto candidate = scenePath.parent_path() / (".woby-save-" + std::to_string(random())
            + "-" + std::to_string(random()));
        if (std::filesystem::create_directory(candidate)) {
            temporaryDirectory = std::move(candidate);
            break;
        }
    }
    if (temporaryDirectory.empty()) {
        throw std::runtime_error("Cannot create a temporary scene file.");
    }
    const auto temporaryPath = temporaryDirectory / "scene.tmp";
    try {
        std::ofstream output;
        output.exceptions(std::ios::badbit | std::ios::failbit);
        output.open(temporaryPath, std::ios::binary | std::ios::trunc);
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.flush();
        output.close();
#ifdef _WIN32
        if (!MoveFileExW(temporaryPath.c_str(), scenePath.c_str(), overwrite ? MOVEFILE_REPLACE_EXISTING : 0)) {
            const auto code = GetLastError();
            const auto error = code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS
                ? std::make_error_code(std::errc::file_exists)
                : std::error_code(static_cast<int>(code), std::system_category());
            throw std::filesystem::filesystem_error("Cannot install saved scene", scenePath, error);
        }
#else
        if (overwrite) {
            std::filesystem::rename(temporaryPath, scenePath);
        } else if (::link(temporaryPath.c_str(), scenePath.c_str()) != 0) {
            throw std::filesystem::filesystem_error("Cannot install saved scene", scenePath,
                std::error_code(errno, std::generic_category()));
        }
#endif
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporaryPath, ignored);
        std::filesystem::remove(temporaryDirectory, ignored);
        throw;
    }
    // Cleanup failure cannot turn an already committed save into a failed save.
    std::error_code ignored;
    std::filesystem::remove(temporaryPath, ignored);
    std::filesystem::remove(temporaryDirectory, ignored);
}

} // namespace woby
