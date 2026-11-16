#pragma once

#include <array>
#include <filesystem>
#include <future>
#include <string>

namespace woby {

enum class FileAssociationType { scene, obj };
enum class FileAssociationAction { none, refresh, registerCopy, removeRegistration, openDefaults };

struct FileAssociationRequest {
    FileAssociationAction action = FileAssociationAction::none;
    FileAssociationType type = FileAssociationType::scene;
};

struct FileAssociationStatus {
    std::filesystem::path registeredExecutable;
    bool registeredCurrentCopy = false;
    bool defaultCurrentCopy = false;
    std::string defaultApp = "Not checked";
};

// OS integration state belongs to the runtime, not to a saved scene.
struct FileAssociationsState {
    bool supported = false;
    bool checked = false;
    bool busy = false;
    bool failed = false;
    std::array<FileAssociationStatus, 2> types;
    std::string message;
};

struct FileAssociationsRuntime {
    FileAssociationsState state;
    std::future<FileAssociationsState> work;
};

void startFileAssociationAction(FileAssociationsRuntime& runtime, FileAssociationRequest request);
void pollFileAssociations(FileAssociationsRuntime& runtime);

} // namespace woby
