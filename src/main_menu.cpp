#include "main_menu.h"

#include "annotation_ui.h"
#include "ui_operations.h"
#include "utf8_path.h"

#include <utility>

namespace woby {
MainMenuResult drawMainMenu(UiState& state, AnnotationInteraction& annotation,
    const MainMenuAvailability& availability, const RecentFiles& recentFiles)
{
    MainMenuResult result;
    const bool ready = !availability.fileActionsDisabled;
    const bool captureReady = ready && !availability.capturePending;
    const auto command = [&](const char* label, const char* shortcut, MainMenuCommand action, bool enabled) {
        if (ImGui::MenuItem(label, shortcut, false, enabled)) {
            result.command = action;
            result.popupPosition = ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y);
        }
    };
    const auto recentMenu = [&](const char* label, RecentFileKind kind) {
        if (!ImGui::BeginMenu(label, ready && recentFileCount(recentFiles, kind) > 0)) { return; }
        for (const auto& entry : recentFiles.entries) {
            if (entry.kind != kind) { continue; }
            const auto path = pathToUtf8(entry.path);
            const auto name = pathToUtf8(entry.path.filename());
            const auto folder = pathToUtf8(entry.path.parent_path());
            ImGui::PushID(path.c_str());
            if (ImGui::MenuItem(name.c_str(), folder.c_str())) {
                result.command = kind == RecentFileKind::scene ? MainMenuCommand::openRecentScene : MainMenuCommand::addRecentModel;
                result.recentPath = entry.path;
            }
            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s\nRight-click to remove this entry.", path.c_str()); }
            if (ImGui::BeginPopupContextItem("recent-file-options")) {
                if (ImGui::MenuItem("Remove from history")) {
                    result.command = kind == RecentFileKind::scene ? MainMenuCommand::removeRecentScene : MainMenuCommand::removeRecentModel;
                    result.recentPath = entry.path;
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndMenu();
    };
    if (!ImGui::BeginMainMenuBar()) { return result; }
    if (ImGui::BeginMenu("File")) {
        command("New scene", nullptr, MainMenuCommand::newScene, captureReady);
        command("Open scene...", "Ctrl+O", MainMenuCommand::openScene, ready);
        recentMenu("Recent scenes", RecentFileKind::scene);
        ImGui::Separator();
        command("Save", "Ctrl+S", MainMenuCommand::saveScene, ready);
        command("Save as...", "Ctrl+Shift+S", MainMenuCommand::saveSceneAs, ready);
        ImGui::Separator();
        command("Add models...", nullptr, MainMenuCommand::addModels, ready);
        command("Add model folder...", nullptr, MainMenuCommand::addModelFolder, ready);
        command("From clipboard", nullptr, MainMenuCommand::fromClipboard, ready);
        recentMenu("Recent models", RecentFileKind::model);
        command("Clear recent history", nullptr, MainMenuCommand::clearRecentFiles, ready && !recentFiles.entries.empty());
        ImGui::Separator();
        command("Export PNG...", nullptr, MainMenuCommand::exportPng, captureReady);
        ImGui::Separator();
        command("Exit", nullptr, MainMenuCommand::exit, captureReady);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        command("Undo", "Ctrl+Z", MainMenuCommand::undo, captureReady && availability.canUndo);
        command("Redo", "Ctrl+Y / Ctrl+Shift+Z", MainMenuCommand::redo, captureReady && availability.canRedo);
        ImGui::Separator();
        command("Settings...", nullptr, MainMenuCommand::settings, ready);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Scene pane", "Ctrl+B", state.viewerPaneVisible)) {
            setViewerPaneVisible(state, !state.viewerPaneVisible);
        }
        if (ImGui::MenuItem("Properties pane", nullptr, state.propertiesPaneVisible)) {
            setPropertiesPaneVisible(state, !state.propertiesPaneVisible);
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("Camera")) {
            for (const auto& [label, view] : {
                    std::pair{"Top", CameraView::top}, {"Bottom", CameraView::bottom},
                    {"Front", CameraView::front}, {"Back", CameraView::back},
                    {"Left", CameraView::left}, {"Right", CameraView::right}}) {
                if (ImGui::MenuItem(label)) { setCameraView(state, view); }
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Fit All", "R", false, !state.files.empty() || !state.comparisons.empty())) {
            fitCameraToScene(state);
        }
        if (ImGui::MenuItem("Fit Selection", nullptr, false, selectedSceneBounds(state).has_value())) {
            fitCameraToSelection(state);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Adaptive points while navigating", nullptr, state.adaptivePoints, scenePostLoadReady(state))) {
            setAdaptivePoints(state, !state.adaptivePoints);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Opaque points refine to full detail when still. Picking and measurements use original points.");
        if (ImGui::MenuItem("X-ray triangle edges", nullptr, state.triangleEdgeXray)) {
            setTriangleEdgeXray(state, !state.triangleEdgeXray);
        }
        if (ImGui::MenuItem("Ground grid", nullptr, state.showGrid)) { toggleShowGrid(state); }
        if (ImGui::MenuItem("Origin axes", nullptr, state.showOrigin)) { toggleShowOrigin(state); }
        if (ImGui::MenuItem("Show dimensions", nullptr, state.showDimensions)) {
            setShowDimensions(state, !state.showDimensions);
        }
        if (ImGui::BeginMenu("Scene up axis")) {
            if (ImGui::MenuItem("Y up", nullptr, state.upAxis == SceneUpAxis::y)) { setSceneUpAxis(state, SceneUpAxis::y); }
            if (ImGui::MenuItem("Z up", nullptr, state.upAxis == SceneUpAxis::z)) { setSceneUpAxis(state, SceneUpAxis::z); }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Tools")) {
        if (ImGui::MenuItem("Text annotation", nullptr, annotation.textTool, !annotation.dragging)) {
            const bool active = annotation.textTool; cancelAnnotationPointer(annotation); annotation.textTool = !active;
        }
        if (ImGui::MenuItem("Cross section", nullptr, annotation.sectionTool, ready && !state.files.empty() && !annotation.dragging)) {
            const bool active = annotation.sectionTool; cancelAnnotationPointer(annotation); annotation.sectionTool = !active;
        }
        for (const auto shape : {AnnotationShape::line, AnnotationShape::rectangle}) {
            const bool active = annotation.tool == shape;
            if (ImGui::MenuItem(shape == AnnotationShape::line ? "Surface line" : "Surface rectangle",
                    nullptr, active, ready && !state.files.empty() && !annotation.dragging)) {
                cancelAnnotationPointer(annotation);
                if (!active) { annotation.tool = shape; }
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        command("Check for updates...", nullptr, MainMenuCommand::checkUpdates, ready && !availability.updateBusy);
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
    return result;
}
} // namespace woby
