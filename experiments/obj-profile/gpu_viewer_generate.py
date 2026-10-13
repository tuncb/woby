"""Generate a viewer copy with short inactive pacing and upload-poll logs.

Upload bytes are backend ticket progress since the previous poll. Wall time is
the poll duration, rather than the former synchronous upload-copy duration.
"""
from pathlib import Path
import sys
from generate import replace, section


def generate(root, output):
    output.mkdir(parents=True, exist_ok=True)
    text = (root / "src/frame_pacing.cpp").read_text(encoding="utf-8")
    text = replace(text, "uint64_t{50'000'000}", "uint64_t{1'000'000}")
    (output / "frame_pacing.cpp").write_text(text, encoding="utf-8")
    text = (root / "src/main.cpp").read_text(encoding="utf-8")
    text = replace(text, "    std::vector<std::string> uploadErrors;",
                   "    std::vector<std::string> uploadErrors;\n    std::vector<uint64_t> researchUploadedBytes;")
    text = replace(text, "    finalize.uploadErrors.resize(finalize.files.size());",
                   "    finalize.uploadErrors.resize(finalize.files.size());\n    finalize.researchUploadedBytes.resize(finalize.files.size());")
    text = section(text, "std::optional<std::string> processGpuFinalizeStep(", "void openSceneOrRequestDirtyWarning(", [
        ("            if (!woby::pollGpuMeshUpload(*upload)) { return {}; }", '''            const auto uploadStart = woby::PerformanceClock::now();
            const bool researchComplete = woby::pollGpuMeshUpload(*upload);
            const auto researchStats = woby::graphics::bufferUploadStats(upload->ticket);
            auto& researchPreviousBytes = finalize.researchUploadedBytes[finalize.nextFileIndex];
            const auto frameBytes = researchStats.uploadedBytes >= researchPreviousBytes
                ? researchStats.uploadedBytes - researchPreviousBytes : uint64_t{0};
            researchPreviousBytes = researchStats.uploadedBytes;
            spdlog::info("research upload_frame bytes={} wall_ms={}", frameBytes, elapsedMilliseconds(uploadStart));
            if (!researchComplete) { return {}; }'''),
    ])
    (output / "main.cpp").write_text(text, encoding="utf-8")


if __name__ == "__main__":
    generate(*map(Path, sys.argv[1:]))
