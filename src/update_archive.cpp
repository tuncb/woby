#include "update.h"
#include "utf8_path.h"

#include <archive.h>
#include <archive_entry.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <set>
#include <stdexcept>

namespace woby {
void extractUpdateArchive(const std::filesystem::path& source, const std::filesystem::path& destination,
    const std::string& platform)
{
    if (std::filesystem::exists(destination)) { throw std::runtime_error("Archive destination must be new."); }
    std::filesystem::create_directories(destination);
    std::unique_ptr<archive, decltype(&archive_read_free)> reader(archive_read_new(), archive_read_free);
    if (!reader) { throw std::runtime_error("Cannot initialize archive reader."); }
    archive_read_support_format_zip(reader.get());
    archive_read_support_format_tar(reader.get());
    archive_read_support_filter_gzip(reader.get());
    const auto check = [&](int result) {
        if (result != ARCHIVE_OK) {
            const char* error = archive_error_string(reader.get());
            throw std::runtime_error(std::string("Cannot extract update: ") + (error ? error : "invalid archive"));
        }
    };
#ifdef _WIN32
    check(archive_read_open_filename_w(reader.get(), source.c_str(), 65536));
#else
    check(archive_read_open_filename(reader.get(), source.c_str(), 65536));
#endif
    const auto prefix = "woby-" + platform;
    std::set<std::string> names;
    uint64_t total = 0;
    size_t entryCount = 0;
    archive_entry* entry = nullptr;
    for (;;) {
        const auto result = archive_read_next_header(reader.get(), &entry);
        if (result == ARCHIVE_EOF) { break; }
        check(result);
        if (++entryCount > 20000) { throw std::runtime_error("Archive has too many entries."); }
        const auto* raw = archive_entry_pathname_utf8(entry);
        if (!raw) { throw std::runtime_error("Archive has an invalid filename."); }
        std::string name(raw);
        const auto type = archive_entry_filetype(entry);
        if (archive_entry_symlink(entry) || archive_entry_hardlink(entry)
            || (type != AE_IFREG && type != AE_IFDIR) || archive_entry_is_encrypted(entry) == 1) {
            throw std::runtime_error("Archive contains a link, special file, or encrypted entry.");
        }
        if (type == AE_IFDIR && name.ends_with('/')) { name.pop_back(); }
        if (name == prefix && type == AE_IFDIR) { continue; }
        if (!name.starts_with(prefix + "/")) { throw std::runtime_error("Unexpected archive root."); }
        name.erase(0, prefix.size() + 1);
        requireUpdatePath(destination, name);
        std::string folded = name;
        std::transform(folded.begin(), folded.end(), folded.begin(), [](char ch) {
            return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : ch;
        });
        if (!names.insert(folded).second || folded == ".woby-update" || folded.starts_with(".woby-update/")) {
            throw std::runtime_error("Archive contains a duplicate or reserved path.");
        }
        const auto path = destination / pathFromUtf8(name);
        if (type == AE_IFDIR) { std::filesystem::create_directories(path); continue; }
        const auto size = archive_entry_size(entry);
        if (size < 0 || size > 512ll * 1024 * 1024) { throw std::runtime_error("Invalid archive file size."); }
        total += static_cast<uint64_t>(size);
        if (total > 2ull * 1024 * 1024 * 1024) { throw std::runtime_error("Unpacked archive is too large."); }
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        std::array<char, 65536> buffer{};
        uint64_t written = 0;
        for (;;) {
            const auto count = archive_read_data(reader.get(), buffer.data(), buffer.size());
            if (count < 0) { throw std::runtime_error("Cannot read archive contents."); }
            if (count == 0) { break; }
            written += static_cast<uint64_t>(count);
            if (written > static_cast<uint64_t>(size)) { throw std::runtime_error("Archive file exceeds declared size."); }
            output.write(buffer.data(), static_cast<std::streamsize>(count));
            if (!output) { throw std::runtime_error("Cannot write extracted update."); }
        }
        output.close();
        if (!output || written != static_cast<uint64_t>(size)) { throw std::runtime_error("Incomplete archive file."); }
#ifndef _WIN32
        auto permissions = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write
            | std::filesystem::perms::group_read | std::filesystem::perms::others_read;
        if (archive_entry_perm(entry) & 0100) {
            permissions |= std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec;
        }
        std::filesystem::permissions(path, permissions);
#endif
    }
    check(archive_read_close(reader.get()));
    const auto manifest = readPackageManifest(destination);
    validateUpdatePackage(destination, manifest);
    std::set<std::string> expected{packageManifestName};
    for (const auto& file : manifest.files) { expected.insert(file.path); }
    for (const auto& file : std::filesystem::recursive_directory_iterator(destination)) {
        if (file.is_regular_file() && !expected.contains(file.path().lexically_relative(destination).generic_string())) {
            throw std::runtime_error("Archive contains an unlisted file.");
        }
    }
}
} // namespace woby
