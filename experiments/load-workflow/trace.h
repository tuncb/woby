#pragma once
#include "model_mesh.h"
#include "scene_mesh_preparation.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <string>

// Only generated experiment sources include this file. No hooks are compiled
// into the normal viewer, and timing/ownership controls never enter UiState.
namespace woby::workflow {
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;
struct Phase { std::string name, path; Clock::time_point start; };
void setPath(std::string path);
const std::string& variant();
Phase begin(std::string name);
void end(const Phase& phase, Json data = Json::object());
void next(Phase& phase, std::string name, Json data = Json::object());
void mark(std::string name, Json data = Json::object());
void frameSubmitted(bool hasFiles);
Json meshInfo(const Mesh& mesh);
Json preparationInfo(const SceneMeshPreparation& prepared);
Json annotationInfo(const MeshAnnotationCache& cache);
} // namespace woby::workflow
