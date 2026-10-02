#include "obj_freeform.h"
#include "utf8_path.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace woby {
namespace {
std::vector<std::string> tokens(std::string_view line)
{
    line = line.substr(0, line.find('#'));
    std::istringstream stream{std::string(line)};
    std::vector<std::string> result; std::string token;
    while (stream >> token) { result.push_back(std::move(token)); }
    return result;
}

double number(const std::string& text)
{
    std::string_view token = text;
    if (token.starts_with('+')) { token.remove_prefix(1); }
    double result = 0;
    const auto [end,error] = std::from_chars(token.data(), token.data()+token.size(), result);
    if (error != std::errc{} || end != token.data()+token.size() || !std::isfinite(result)) {
        throw std::runtime_error("Expected a finite number.");
    }
    return result;
}

size_t reference(std::string_view text, size_t count)
{
    if (text.starts_with('+')) { text.remove_prefix(1); }
    int64_t index = 0;
    const auto [end,error] = std::from_chars(text.data(), text.data()+text.size(), index);
    if (error != std::errc{} || end != text.data()+text.size() || index == 0
        || index > static_cast<int64_t>(count) || index < -static_cast<int64_t>(count)) {
        throw std::runtime_error("Invalid freeform control point reference.");
    }
    return static_cast<size_t>(index < 0 ? static_cast<int64_t>(count)+index : index-1);
}

uint32_t degree(const std::string& text)
{
    const double value = number(text);
    if (value < 1 || value > maxFreeformDegree || std::floor(value) != value) {
        throw std::runtime_error("Freeform degree must be between 1 and 8.");
    }
    return static_cast<uint32_t>(value);
}

void bezierKnots(std::vector<double>& parameters, uint32_t p)
{
    if (parameters.size() < 2) { throw std::runtime_error("Bezier geometry requires at least two parameter values."); }
    std::vector<double> knots;
    for (size_t i = 0; i < parameters.size(); ++i) {
        if (i && parameters[i] <= parameters[i-1]) { throw std::runtime_error("Bezier parameter values must strictly increase."); }
        knots.insert(knots.end(), p + ((i == 0 || i+1 == parameters.size()) ? 1u : 0u), parameters[i]);
    }
    parameters = std::move(knots);
}

void finish(FreeformPatch& patch, bool bezier)
{
    if (bezier) {
        bezierKnots(patch.knotsU,patch.degreeU);
        if (patch.surface) { bezierKnots(patch.knotsV,patch.degreeV); }
    }
    if (patch.knotsU.size() <= patch.degreeU+1 || (patch.surface && patch.knotsV.size() <= patch.degreeV+1)) {
        throw std::runtime_error("Freeform geometry is missing its parm knot vectors.");
    }
    patch.countU = static_cast<uint32_t>(patch.knotsU.size() - patch.degreeU - 1);
    patch.countV = patch.surface ? static_cast<uint32_t>(patch.knotsV.size() - patch.degreeV - 1) : 1;
    validateFreeformPatch(patch);
}
} // namespace

bool objFreeformStatement(std::string_view line)
{
    const auto words = tokens(line);
    if (words.empty()) { return false; }
    // RapidOBJ rejects homogeneous vertices before reaching a later cstype.
    // Route those records (and legal short/3D UVs) through the same reader.
    if ((words[0] == "v" && words.size() == 5)
        || (words[0] == "vt" && (words.size() == 2 || words.size() == 4))) { return true; }
    for (const auto keyword : {"cstype","deg","curv","curv2","surf","parm","end","vp","trim","hole",
            "scrv","sp","con","bmat","step","ctech","stech","bzp","bsp","cdc","cdp","res"}) {
        if (words[0] == keyword) { return true; }
    }
    return false;
}

ObjFreeformInput readObjFreeform(const std::filesystem::path& path, const ModelLoadProgressCallback& progress)
{
    std::ifstream file(path);
    if (!file) { throw std::runtime_error("Cannot read OBJ freeform geometry: " + pathToUtf8(path)); }
    ObjFreeformInput result;
    std::vector<std::array<double,4>> positions;
    std::vector<std::array<double,2>> texcoords;
    std::vector<Coordinate> normals;
    size_t lineNumber = 0, statementLine = 0;
    std::string currentName, type;
    bool rational = false;
    uint32_t degreeU = 0, degreeV = 0;
    std::optional<FreeformPatch> active;
    std::string physical;
    try {
        while (std::getline(file,physical)) {
            statementLine = ++lineNumber;
            std::string logical = physical;
            size_t physicalLines = 1;
            while (true) {
                const auto last = logical.find_last_not_of(" \t\r");
                if (last == std::string::npos || logical[last] != '\\') { break; }
                logical.erase(last);
                if (!std::getline(file,physical)) { throw std::runtime_error("Unterminated OBJ line continuation."); }
                logical += " " + physical; ++lineNumber; ++physicalLines;
                if (logical.size() > 1024*1024) { throw std::runtime_error("OBJ freeform statement exceeds 1 MiB."); }
            }
            if (logical.size() > 1024*1024) { throw std::runtime_error("OBJ freeform statement exceeds 1 MiB."); }
            if (lineNumber % 1024 == 0) { reportModelLoadProgress(progress,ModelLoadStage::reading); }
            const auto words = tokens(logical);
            bool keep = true;
            if (!words.empty()) {
                const auto& key = words[0];
                if (key == "trim" || key == "hole" || key == "curv2" || key == "scrv" || key == "sp" || key == "con") {
                    throw std::runtime_error("Unsupported OBJ freeform statement '" + key + "': trimming and surface connectivity are not supported.");
                }
                if (active && key != "parm" && key != "end") { throw std::runtime_error("Expected parm or end in a freeform body."); }
                if (key == "v") {
                    if (words.size() != 4 && words.size() != 5 && words.size() != 7 && words.size() != 8) { throw std::runtime_error("Invalid OBJ vertex."); }
                    positions.push_back({number(words[1]),number(words[2]),number(words[3]),words.size() == 5 ? number(words[4]) : 1});
                    if (positions.size() > maxFreeformVertices) { throw std::runtime_error("Freeform OBJ control table exceeds the vertex limit."); }
                    logical = "v " + words[1] + " " + words[2] + " " + words[3];
                } else if (key == "vt") {
                    if (words.size() < 2 || words.size() > 4) { throw std::runtime_error("Invalid texture vertex."); }
                    texcoords.push_back({number(words[1]),words.size() > 2 ? number(words[2]) : 0});
                    if (words.size() == 4) { (void)number(words[3]); }
                    logical = "vt " + words[1] + " " + (words.size() > 2 ? words[2] : "0");
                } else if (key == "vn") {
                    if (words.size() != 4) { throw std::runtime_error("Invalid normal vertex."); }
                    for (size_t i = 1; i < 4; ++i) { (void)number(words[i]); }
                    normals.push_back({number(words[1]), number(words[2]), number(words[3])});
                } else if (key == "vp") {
                    if (words.size() < 2 || words.size() > 4) { throw std::runtime_error("Invalid parameter vertex."); }
                    for (size_t i = 1; i < words.size(); ++i) { (void)number(words[i]); }
                    keep = false;
                } else if (key == "g" || key == "o") {
                    currentName.clear();
                    for (size_t i = 1; i < words.size(); ++i) { if (i > 1) { currentName += ' '; } currentName += words[i]; }
                } else if (key == "cstype") {
                    rational = words.size() == 3 && words[1] == "rat";
                    if (words.size() != (rational ? 3u : 2u)) { throw std::runtime_error("Invalid cstype statement."); }
                    type = words.back();
                    if (type != "bezier" && type != "bspline") { throw std::runtime_error("Unsupported OBJ freeform basis '" + type + "'. Use bezier or bspline."); }
                    keep = false;
                } else if (key == "deg") {
                    if (words.size() != 2 && words.size() != 3) { throw std::runtime_error("Invalid deg statement."); }
                    degreeU = degree(words[1]); degreeV = words.size() == 3 ? degree(words[2]) : 0; keep = false;
                } else if (key == "curv" || key == "surf") {
                    const bool surface = key == "surf";
                    const size_t first = surface ? 5 : 3;
                    if (type.empty() || degreeU == 0 || (surface && degreeV == 0) || words.size() <= first) {
                        throw std::runtime_error("Freeform geometry requires cstype, deg, parameter ranges and control points.");
                    }
                    active.emplace(); auto& patch = *active;
                    patch.name = currentName; patch.surface = surface;
                    patch.degreeU = degreeU; patch.degreeV = surface ? degreeV : 0;
                    patch.domainU = {number(words[1]),number(words[2])};
                    if (surface) { patch.domainV = {number(words[3]),number(words[4])}; }
                    for (size_t i = first; i < words.size(); ++i) {
                        const auto& token = words[i]; const auto slash = token.find('/');
                        auto point = positions.at(reference(std::string_view(token).substr(0,slash),positions.size()));
                        if (!rational) { point[3] = 1; }
                        patch.controls.push_back(point);
                        if (slash != std::string::npos) {
                            if (!surface) { throw std::runtime_error("OBJ curves require position-only control references."); }
                            const auto second = token.find('/',slash+1);
                            const auto uv = std::string_view(token).substr(slash+1,second == std::string::npos ? second : second-slash-1);
                            if (!uv.empty()) { patch.texcoords.push_back(texcoords.at(reference(uv,texcoords.size()))); }
                            if (second != std::string::npos) { patch.normals.push_back(normals.at(reference(std::string_view(token).substr(second+1),normals.size()))); }
                        }
                    }
                    keep = false;
                } else if (key == "parm") {
                    if (!active || words.size() < 4 || (words[1] != "u" && words[1] != "v") || (words[1] == "v" && !active->surface)) {
                        throw std::runtime_error("Invalid parm statement.");
                    }
                    auto& values = words[1] == "u" ? active->knotsU : active->knotsV;
                    values.clear();
                    for (size_t i = 2; i < words.size(); ++i) { values.push_back(number(words[i])); }
                    keep = false;
                } else if (key == "end") {
                    if (!active || words.size() != 1) { throw std::runtime_error("Unexpected freeform end statement."); }
                    finish(*active,type == "bezier");
                    result.patches.push_back(std::move(*active)); active.reset(); keep = false;
                } else if (objFreeformStatement(logical)) {
                    throw std::runtime_error("Unsupported OBJ freeform statement '" + key + "'.");
                }
            }
            if (keep) { result.polygonText += logical; }
            result.polygonText.append(physicalLines,'\n');
        }
        if (file.bad()) { throw std::runtime_error("Failed while reading OBJ."); }
        if (active) { throw std::runtime_error("Freeform geometry is missing its end statement."); }
    } catch (const std::exception& error) {
        throw std::runtime_error("Failed to load OBJ freeform geometry: " + pathToUtf8(path) + " (line " + std::to_string(statementLine) + "): " + error.what());
    }
    return result;
}
} // namespace woby
