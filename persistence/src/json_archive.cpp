#include "json_archive.hpp"

#include "fileio/atomic_write.hpp"
#include "load/persistence.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <limits>
#include <ostream>
#include <regex>
#include <set>
#include <stdexcept>
#include <system_error>

namespace persistence::json_archive {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

/** Worker archive names: ordinal, UTC timestamp and the attempt's run identity. */
const std::regex& name_pattern() {
    static const std::regex pattern(
        R"(^[0-9]{20}-[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{6}Z-[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$)");
    return pattern;
}

/** A JSON string member, or nothing when the key is absent or another type. */
std::optional<std::string> string_member(const Json& object, const char* key) {
    const auto entry = object.find(key);
    if (entry == object.end() || !entry->is_string()) return std::nullopt;
    return entry->get<std::string>();
}

/** Directory contents by role, so recognition can demand an exact file set. */
struct Contents {
    bool manifest = false;
    bool payload = false;
    bool legacy = false;
    bool unexpected = false;
};

Contents scan(const fs::path& directory) {
    Contents contents;
    for (const auto& entry : fs::directory_iterator(directory)) {
        const auto name = entry.path().filename().string();
        const bool regular = fs::is_regular_file(entry.symlink_status());
        if (name == manifest_file) {
            contents.manifest = regular;
        } else if (name == state_file) {
            contents.payload = regular;
        } else if (name == legacy_file) {
            contents.legacy = regular;
        } else {
            contents.unexpected = true;
        }
    }
    return contents;
}

/**
 * Validate the publication marker against this directory and store identity.
 *
 * The manifest is the archive's commit record, so every claim it makes about the
 * directory must agree: the identifier names this directory, the recorded
 * session owns this store, and the payload size matches the file on disk.
 * Unreadable or malformed evidence is simply not recognized; retention never
 * deletes a directory whose contents it could not verify.
 */
bool manifest_matches(const fs::path& directory, const std::string& session_id) {
    Json document;
    try {
        std::ifstream input(directory / manifest_file, std::ios::binary);
        if (!input.good()) return false;
        document = Json::parse(input, nullptr, false);
    } catch (const std::exception&) {
        return false;
    }
    if (!document.is_object()) return false;
    const auto format = string_member(document, "format");
    const auto archive = string_member(document, "archive_id");
    const auto owner = string_member(document, "session_id");
    const auto backend = string_member(document, "backend");
    const auto kind = string_member(document, "kind");
    const auto version = document.find("version");
    const auto payload = document.find("payload");
    if (!format || *format != manifest_format) return false;
    if (version == document.end() || !version->is_number_unsigned()) return false;
    if (version->get<std::uint64_t>() != manifest_version) return false;
    if (!archive || *archive != directory.filename().string()) return false;
    if (!owner || *owner != session_id) return false;
    if (!backend || *backend != backend_name(Backend::Json)) return false;
    if (!kind || *kind != "json_state") return false;
    if (payload == document.end() || !payload->is_object()) return false;
    const auto file = string_member(*payload, "file");
    const auto bytes = payload->find("bytes");
    if (!file || *file != state_file) return false;
    if (bytes == payload->end() || !bytes->is_number_unsigned()) return false;
    return bytes->get<std::uint64_t>() == fs::file_size(directory / state_file);
}

/** Normalized archive directory for one reference, or a caller error. */
fs::path archive_directory(
    const fs::path& root, const ArchiveRef& archive, const std::string& session_id) {
    if (archive.backend != Backend::Json || archive.session_id != session_id) {
        throw std::invalid_argument("archive reference belongs to another store");
    }
    if (archive.payload.empty()) {
        throw std::invalid_argument("JSON archive reference requires a payload path");
    }
    const auto payload = fs::absolute(archive.payload).lexically_normal();
    if (payload.filename().string()
            != (archive.kind == ArchiveKind::LegacyReadable ? legacy_file : state_file)
        || payload.parent_path().parent_path() != root) {
        throw std::invalid_argument("archive reference is not a JSON compact archive");
    }
    return payload.parent_path();
}

} // namespace

bool is_archive_name(std::string_view name) {
    return std::regex_match(name.begin(), name.end(), name_pattern());
}

fs::path plan_directory(const fs::path& root, const std::string& run_id) {
    std::uint64_t latest = 0;
    if (fs::exists(root)) {
        for (const auto& entry : fs::directory_iterator(root)) {
            const auto name = entry.path().filename().string();
            if (name.size() < 21 || name[20] != '-') continue;
            std::uint64_t ordinal = 0;
            const auto parsed = std::from_chars(name.data(), name.data() + 20, ordinal);
            if (parsed.ec == std::errc{} && parsed.ptr == name.data() + 20) {
                latest = std::max(latest, ordinal);
            }
        }
    }
    if (latest == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("compact archive sequence exhausted");
    }
    auto ordinal = std::to_string(latest + 1);
    ordinal.insert(0, 20 - ordinal.size(), '0');
    auto time = utc_timestamp();
    std::erase(time, ':');
    const auto name = ordinal + "-" + time + "-" + run_id;
    if (!is_archive_name(name)) {
        throw std::invalid_argument("compact run identity must be a UUID");
    }
    return root / name;
}

void publish(
    const fs::path& directory, const std::string& session_id,
    const model_io::AgentInputState& original) {
    const auto payload = directory / state_file;
    load::save_state(payload, original, load::StateFormat::Json);
    const Json manifest = {
        {"format", manifest_format},
        {"version", manifest_version},
        {"archive_id", directory.filename().string()},
        {"session_id", session_id},
        {"backend", backend_name(Backend::Json)},
        {"kind", "json_state"},
        {"payload", {{"file", state_file}, {"bytes", fs::file_size(payload)}}},
    };
    // Publication point: the manifest is published atomically and last, so the
    // archive becomes recognizable only when both files are complete.
    fileio::atomic_write(directory / manifest_file, [&manifest](std::ostream& output) {
        output << manifest.dump(2) << '\n';
    });
}

std::optional<Recognized> recognize(
    const fs::path& directory, const std::string& session_id) {
    if (!is_archive_name(directory.filename().string())) return std::nullopt;
    if (!fs::is_directory(fs::symlink_status(directory))) return std::nullopt;
    const auto contents = scan(directory);
    if (contents.unexpected) return std::nullopt;
    if (contents.legacy) {
        // Pre-#187 evidence: write-only Markdown with no manifest. Reported so a
        // historical reference keeps its format, but never restorable or deleted.
        if (contents.manifest || contents.payload) return std::nullopt;
        return Recognized{ArchiveKind::LegacyReadable, directory / legacy_file};
    }
    if (!contents.manifest || !contents.payload) return std::nullopt;
    if (!manifest_matches(directory, session_id)) return std::nullopt;
    return Recognized{ArchiveKind::JsonState, directory / state_file};
}

std::optional<ArchiveRef> reference_at(
    const fs::path& root, const fs::path& path, const std::string& session_id) {
    if (path.empty()) return std::nullopt;
    const auto normalized = fs::absolute(path).lexically_normal();
    // Only a direct child of this root can be one of this store's archives; the
    // recorded form is usually the payload file inside that child.
    auto directory = normalized.parent_path() == root
        ? normalized : normalized.parent_path();
    if (directory.parent_path() != root) return std::nullopt;
    const auto recognized = recognize(directory, session_id);
    if (!recognized) return std::nullopt;
    return ArchiveRef{session_id, directory.filename().string(), Backend::Json,
        recognized->kind, recognized->payload};
}

CleanupResult prune(
    const fs::path& root, const std::string& session_id, const ArchiveRef& current,
    const std::vector<ArchiveRef>& referenced, const load::MemoryRetention& policy) {
    CleanupResult result;
    if (policy.max_archives == 0) {
        return result;
    }
    if (!fs::is_directory(fs::symlink_status(root))) {
        throw std::invalid_argument("memory cleanup requires an ordinary archive root");
    }
    struct Candidate {
        fs::path directory;
        std::uintmax_t bytes;
    };
    std::vector<Candidate> candidates;
    for (const auto& entry : fs::directory_iterator(root)) {
        const auto recognized = recognize(entry.path(), session_id);
        if (!recognized || recognized->kind != ArchiveKind::JsonState) continue;
        candidates.push_back({entry.path(), fs::file_size(recognized->payload)});
    }
    const auto current_directory = archive_directory(root, current, session_id);
    const auto protected_entry = std::find_if(candidates.begin(), candidates.end(),
        [&](const auto& archive) { return archive.directory == current_directory; });
    if (protected_entry == candidates.end()) {
        throw std::runtime_error(
            "current memory archive is missing or has unexpected contents");
    }
    std::set<fs::path> protected_paths{current_directory};
    for (const auto& archive : referenced) {
        protected_paths.insert(archive_directory(root, archive, session_id));
    }
    const auto protected_archive = [&](const auto& archive) {
        return protected_paths.contains(archive.directory);
    };
    std::size_t retained = std::count_if(candidates.begin(), candidates.end(), protected_archive);
    std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
        return left.directory.filename() > right.directory.filename();
    });
    for (const auto& archive : candidates) {
        if (protected_archive(archive)) continue;
        if (retained >= policy.max_archives) {
            if (fs::remove(archive.directory / state_file)) {
                result.removed_bytes += archive.bytes;
                ++result.removed_archives;
            }
            // Remove the marker before the directory so an interrupted deletion
            // leaves an unrecognizable directory rather than a restorable copy.
            fs::remove(archive.directory / manifest_file);
            // Refuse to recursively delete contents introduced since the scan.
            fs::remove(archive.directory);
        } else {
            ++retained;
        }
    }
    return result;
}

} // namespace persistence::json_archive
