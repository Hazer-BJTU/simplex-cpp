#include "manager/service.hpp"
#include "persistence/selection.hpp"

#include <cerrno>
#include <fstream>
#include <sstream>
#include <limits>
#include "fileio/replace_existing.hpp"
#include <system_error>
#include "fileio/atomic_write.hpp"
#include "fileio/child_path.hpp"
#include "fileio/directory_tree_lock.hpp"
#include "load/persistence.hpp"
#include "yamlconfig/yaml_json.hpp"

namespace manager {
namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;

/** Reject invalid identifiers before deriving any manager metadata filenames. */
std::string identifier(const Json& object, const char* key) {
    if (!object.contains(key) || !object.at(key).is_string()) {
        throw Error("invalid_request", std::string("missing string identifier: ") + key);
    }
    auto value = object.at(key).get<std::string>();
    if (value.empty() || value.size() > 128
        || value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")
            != std::string::npos) {
        throw Error("invalid_request", std::string("invalid identifier: ") + key);
    }
    return value;
}

/** Reject links, including dangling links, rather than following managed paths. */
void ordinary_path(const fs::path& path) {
    fs::path current;
    for (const auto& component : path) {
        current /= component;
        std::error_code error;
        const auto status = fs::symlink_status(current, error);
        if (error && error != std::errc::no_such_file_or_directory) {
            throw std::system_error(error);
        }
        if (fs::is_symlink(status)) {
            throw Error("unsafe_path", "managed paths must not contain symbolic links");
        }
    }
}

/** Stable sibling metadata survives root deletion, preventing lock-inode races. */
struct Paths {
    fs::path root;
    fs::path metadata;
    fs::path record;
    fs::path lock;
    fs::path config;
};

Paths paths(const Json& request) {
    if (!request.contains("directory") || !request.at("directory").is_string()) {
        throw Error("invalid_request", "directory must be an absolute target path");
    }
    const auto value = request.at("directory").get<std::string>();
    fs::path root(value);
    if (value.find('\0') != std::string::npos || !root.is_absolute()
        || root != root.lexically_normal() || root.filename().empty()
        || root.filename() == "." || root.filename() == ".."
        || root.filename() == ".simplex-manager") {
        throw Error("invalid_request", "directory must be a normalized absolute allocation path");
    }
    for (const auto& part : root) {
        if (part == ".." || part == ".simplex-manager") {
            throw Error("invalid_request", "parent traversal and manager metadata paths are not allowed");
        }
    }
    ordinary_path(root);
    // A parent removal must never erase another independently leased allocation.
    // Allocations are flat ownership units even when their parent is operator-selected.
    for (auto parent = root.parent_path(); parent != parent.root_path(); parent = parent.parent_path()) {
        const auto ancestor = parent.parent_path() / ".simplex-manager"
            / (parent.filename().string() + ".json");
        ordinary_path(ancestor);
        if (fs::exists(ancestor)) {
            throw Error("unsafe_path", "allocations cannot be nested inside managed allocations");
        }
    }
    const auto metadata = root.parent_path() / ".simplex-manager";
    ordinary_path(metadata);
    const auto stem = root.filename().string();
    Paths result{root, metadata, metadata / (stem + ".json"),
        metadata / (stem + ".lock"), root / "config" / "config.yaml"};
    ordinary_path(result.record);
    ordinary_path(result.lock);
    return result;
}

std::string read_text(const fs::path& path) {
    ordinary_path(path);
    if (!fs::is_regular_file(path)) {
        throw Error("storage_error", "expected an ordinary file");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw Error("storage_error", "cannot open file");
    }
    std::ostringstream output;
    output << input.rdbuf();
    if (input.bad() || output.bad()) {
        throw Error("storage_error", "cannot read complete file");
    }
    return output.str();
}

Json read_json(const fs::path& path) {
    try {
        return Json::parse(read_text(path));
    } catch (const Json::exception&) {
        throw Error("corrupt_storage", "stored JSON is invalid");
    }
}

/** A single atomic control document publishes source, refresh and startup data. */
void publish(const fs::path& path, const Json& value) {
    ordinary_path(path);
    const auto bytes = value.dump(2) + "\n";
    if (fs::exists(path)) {
        // Preserve explicit operator UID/GID/mode grants during refresh.
        const auto previous = fileio::read_editable_file(path,
            std::numeric_limits<std::size_t>::max() - 1);
        fileio::replace_existing_file(path, previous, bytes);
    } else {
        fileio::atomic_write(path, [&](std::ostream& output) { output << bytes; });
    }
}

std::shared_ptr<fileio::SessionLock> lock(const Paths& p, bool exclusive) {
    try {
        return std::make_shared<fileio::SessionLock>(p.lock,
            exclusive ? fileio::SessionLock::Mode::Exclusive : fileio::SessionLock::Mode::Shared);
    } catch (const std::system_error& error) {
        if (error.code().value() == EWOULDBLOCK || error.code().value() == EAGAIN) {
            throw Error("busy", "allocation is in use");
        }
        throw;
    }
}

Json owned(const Paths& p, const std::string& id) {
    if (!fs::exists(p.record)) {
        throw Error("not_found", "allocation has no manager ownership record");
    }
    auto record = read_json(p.record);
    if (!record.is_object() || record.value("version", 0) != 1
        || record.value("directory", "") != p.root.string()
        || record.value("allocation_id", "") != id) {
        throw Error("ownership_mismatch", "allocation identity does not match");
    }
    const auto status = record.value("status", "");
    if ((status != "preparing" && status != "ready" && status != "removing" && status != "removed")
        || !record.contains("revision") || !record.at("revision").is_number_unsigned()
        || record.at("revision").get<std::uint64_t>() == 0) {
        throw Error("corrupt_storage", "invalid allocation status or revision");
    }
    return record;
}

void require_ready(const Json& record) {
    if (record.value("status", "") != "ready") {
        throw Error("not_ready", "allocation is not ready");
    }
}

/** Match load's portable lexical storage-child contract; reject aliases outside root. */
fs::path child_path(const fs::path& root, const Json& storage, const char* name) {
    fs::path result;
    try {
        const auto value = storage.value(name, std::string(name));
        result = fileio::child_path(root, value);
    } catch (const Json::exception&) {
        throw Error("invalid_configuration", "state and memory must be nonempty relative path strings");
    } catch (const std::invalid_argument&) {
        throw Error("invalid_configuration", "state and memory must be relative paths without parent traversal");
    }
    ordinary_path(result);
    return result;
}

/** Only these scalar connection fields can override an operator's source YAML. */
void refresh_fields(Json& document, const Json& refresh) {
    if (!refresh.is_object()) {
        throw Error("invalid_request", "refresh must be an object");
    }
    for (const auto& [key, value] : refresh.items()) {
        if (!value.is_string() || value.get_ref<const std::string&>().empty()) {
            throw Error("invalid_request", "refresh endpoints must be nonempty strings");
        }
        if (key == "client_endpoint") {
            document["client"]["endpoint"] = value;
        } else if (key == "confirmation_endpoint") {
            if (document.contains("security") && document["security"].contains("confirmation")) {
                document["security"]["confirmation"]["endpoint"] = value;
            }
        } else if (key == "tools_endpoint") {
            if (document.contains("hub_remote_call")) {
                document["hub_remote_call"]["endpoint"] = value;
            }
        } else {
            throw Error("invalid_request", "unsupported refresh field");
        }
    }
}

Json effective(const Paths& p, const Json& record) {
    Json document;
    const auto configuration = record.find("configuration");
    if (configuration == record.end() || !configuration->is_string()) {
        throw Error("corrupt_storage", "allocation record has no configuration document");
    }
    try {
        document = yamlconfig::parse(configuration->get<std::string>());
    } catch (const yamlconfig::YamlConfigError&) {
        throw Error("invalid_configuration", "configuration must be one valid YAML document");
    }
    if (!document.is_object() || document.contains("_simplex_managed")) {
        throw Error("invalid_configuration", "configuration must be an ordinary worker mapping");
    }
    for (const char* name : {"persistence", "client", "security", "hub_remote_call"}) {
        if (document.contains(name) && !document[name].is_object()) {
            throw Error("invalid_configuration", "configuration sections must be mappings");
        }
    }
    auto& storage = document["persistence"];
    if (storage.is_null()) {
        storage = Json::object();
    }
    storage["directory"] = p.root.string();
    child_path(p.root, storage, "state");
    child_path(p.root, storage, "memory");
    const auto refresh = record.find("refresh");
    if (refresh != record.end() && !refresh->is_object()) {
        throw Error("corrupt_storage", "allocation record has an invalid refresh section");
    }
    refresh_fields(document, refresh == record.end() ? Json::object() : *refresh);
    return document;
}

Json reference(const Paths& p, const std::string& id) {
    return {{"_simplex_managed", {{"version", 1}, {"directory", p.root.string()},
        {"allocation_id", id}}}};
}

void verify_reference(const Paths& p, const std::string& id) {
    if (!fs::exists(p.config) || read_json(p.config) != reference(p, id)) {
        throw Error("ownership_mismatch", "managed configuration reference is missing or changed");
    }
}

Json description(const Paths& p, const Json& record) {
    Json result = {{"directory", p.root.string()}, {"allocation_id", record.at("allocation_id")},
        {"status", record.at("status")}, {"revision", record.at("revision")},
        {"config_path", p.config.string()}};
    if (record.at("status") == "ready") {
        result["configuration"] = record.at("configuration");
        result["effective_configuration"] = effective(p, record);
        result["startup"] = record.value("startup", Json(nullptr));
    }
    return result;
}

/** Rebase only configuration-relative paths; prompt paths remain installation-relative. */
Json fork_configuration(const Paths& parent, const Json& startup) {
    auto document = startup.at("configuration");
    const auto rebase = [&](Json& value) {
        if (value.is_string() && !value.get_ref<const std::string&>().empty()) {
            const fs::path path(value.get<std::string>());
            if (path.is_relative()) {
                value = (parent.config.parent_path() / path).lexically_normal().string();
            }
        }
    };
    if (document.contains("worker") && document["worker"].contains("environment")
        && document["worker"]["environment"].contains("workspace")) {
        rebase(document["worker"]["environment"]["workspace"]);
    }
    for (const char* path : {"/plugins/providers", "/plugins/extensions/tools", "/plugins/extensions/loop_hooks"}) {
        const Json::json_pointer pointer(path);
        if (!document.contains(pointer)) continue;
        auto& section = document[pointer];
        if (section.contains("directories") && section["directories"].is_array()) {
            for (auto& entry : section["directories"]) rebase(entry);
        }
        if (section.contains("enable") && section["enable"].is_array()) {
            for (auto& entry : section["enable"]) {
                for (const char* key : {"schema_directory", "config_file"}) {
                    if (entry.is_object() && entry.contains(key)) rebase(entry[key]);
                }
            }
        }
    }
    document["persistence"]["restore"] = "if_present";
    // Child approval remains independently managed by Hub, even if the parent
    // source omitted these optional integrations. Identity URLs are supplied by Hub.
    document["client"]["endpoint"] = "ws://unconfigured.invalid/events";
    document["security"]["confirmation"]["endpoint"] = "ws://unconfigured.invalid/confirmation";
    document["hub_remote_call"]["endpoint"] = "ws://unconfigured.invalid/tools";
    return document;
}

} // namespace

Error::Error(std::string code_value, std::string message)
    : std::runtime_error(std::move(message)), code(std::move(code_value)) {}
Error::~Error() = default;

Json execute(const Json& input) {
    auto request = input; // clean_fork derives configuration on the target, never on Hub.
    if (!request.is_object() || request.value("version", 0) != 1) {
        throw Error("invalid_request", "expected management protocol version 1");
    }
    identifier(request, "request_id");
    const auto id = identifier(request, "allocation_id");
    const auto operation = identifier(request, "operation");
    if (operation != "prepare" && operation != "inspect" && operation != "capture_startup"
        && operation != "read_state" && operation != "remove" && operation != "clean_fork") {
        throw Error("unsupported_operation", "unsupported management operation");
    }
    const bool preparing = operation == "prepare" || operation == "clean_fork";
    auto p = paths(request);
    const bool mutation = operation != "inspect" && operation != "read_state";
    std::unique_ptr<fileio::DirectoryTreeLock> namespace_lease;
    if (mutation) {
        try {
            namespace_lease = std::make_unique<fileio::DirectoryTreeLock>(p.root.parent_path());
        } catch (const std::system_error& error) {
            if (error.code().value() == EWOULDBLOCK || error.code().value() == EAGAIN) {
                throw Error("busy", "allocation namespace is in use");
            }
            throw;
        }
        // The initial lookup is advisory. Recheck ancestry under the namespace
        // fence and retain it through publication/deletion, before taking the
        // per-allocation lock. Parent and child roots otherwise use different locks.
        p = paths(request);
    }
    if (!fs::exists(p.metadata)) {
        if (!preparing) {
            throw Error("not_found", "allocation has no manager metadata");
        }
        fs::create_directories(p.metadata);
        fs::permissions(p.metadata, fs::perms::owner_all);
    }
    auto lease = lock(p, mutation);
    ordinary_path(p.root);

    if (operation == "clean_fork") {
        if (!request.contains("source") || !request.at("source").is_object()) {
            throw Error("invalid_request", "clean_fork requires a captured source allocation");
        }
        const auto& source = request.at("source");
        const auto parent_id = identifier(source, "allocation_id");
        const auto lifecycle = identifier(source, "lifecycle_id");
        const auto parent = paths(source);
        if (parent.root == p.root || parent_id == id || !source.contains("revision")
            || !source.at("revision").is_number_integer() || source.at("revision") <= 0) {
            throw Error("invalid_request", "clean_fork requires a distinct allocation and captured revision");
        }
        if (fs::exists(p.record)) {
            const auto previous = owned(p, id);
            if (!previous.contains("fork_source") || previous.at("fork_source") != source) {
                throw Error("conflict", "allocation belongs to a different fork intent");
            }
            // A lost reply can be retried after the parent has stopped or retired.
            // Reuse the committed source, never the parent's newer configuration.
            if (previous.at("status") == "removed" || previous.at("status") == "removing") {
                throw Error("retired", "retired allocation paths cannot be reused");
            }
            request["configuration"] = previous.at("configuration");
        } else {
            if (!fs::exists(parent.record)) throw Error("not_found", "source allocation is missing");
            auto parent_lease = lock(parent, false); // Compatible with a running worker.
            const auto parent_record = owned(parent, parent_id);
            require_ready(parent_record);
            verify_reference(parent, parent_id);
            if (!parent_record.contains("startup")) throw Error("conflict", "source startup is not captured");
            const auto& startup = parent_record.at("startup");
            if (startup.at("lifecycle_id") != lifecycle || startup.at("revision") != source.at("revision")) {
                throw Error("conflict", "source lifecycle or revision changed");
            }
            request["configuration"] = fork_configuration(parent, startup).dump();
        }
    }

    if (preparing && !fs::exists(p.record)) {
        if (fs::exists(p.root)) {
            throw Error("unmanaged_directory", "existing directories require explicit migration; refusing adoption");
        }
        if (!request.contains("configuration") || !request.at("configuration").is_string()) {
            throw Error("invalid_request", "prepare requires YAML configuration text");
        }
        Json initial = {{"version", 1}, {"directory", p.root.string()}, {"allocation_id", id},
            {"status", "preparing"}, {"revision", 1}, {"configuration", request.at("configuration")},
            {"refresh", request.value("refresh", Json::object())}};
        if (operation == "clean_fork") initial["fork_source"] = request.at("source");
        effective(p, initial); // Validate before claiming or creating the allocation.
        publish(p.record, initial);
    }

    auto record = owned(p, id);
    if (preparing) {
        if (record.at("status") == "removed" || record.at("status") == "removing") {
            throw Error("retired", "retired allocation paths cannot be reused");
        }
        if (request.contains("configuration") && request.at("configuration") != record.at("configuration")) {
            throw Error("conflict", "allocation already has a different source configuration");
        }
        if (record.at("status") == "preparing") {
            // The durable ownership record permits retry after interrupted creation.
            ordinary_path(p.config);
            fs::create_directories(p.config.parent_path());
            fs::permissions(p.root, fs::perms::owner_all);
            fs::permissions(p.config.parent_path(), fs::perms::owner_all);
            if (fs::exists(p.config)) {
                verify_reference(p, id);
            } else {
                publish(p.config, reference(p, id));
            }
            record["status"] = "ready";
            publish(p.record, record);
        }
        require_ready(record);
        verify_reference(p, id);
        if (request.contains("refresh") && request.at("refresh") != record.at("refresh")) {
            if (!request.contains("expected_revision") || request.at("expected_revision") != record.at("revision")) {
                throw Error("conflict", "refresh requires the current expected_revision");
            }
            auto next = record;
            next["refresh"] = request.at("refresh");
            next["revision"] = record.at("revision").get<std::uint64_t>() + 1;
            effective(p, next);
            publish(p.record, next);
            record = std::move(next);
        }
        return description(p, record);
    }
    if (operation == "inspect") {
        if (record.at("status") == "ready") {
            verify_reference(p, id);
        }
        return description(p, record);
    }
    if (operation == "remove") {
        if (record.at("status") == "removed") {
            if (fs::exists(p.root)) {
                throw Error("ownership_mismatch", "retired path has been occupied again");
            }
            return description(p, record);
        }
        if (record.at("status") == "ready") {
            verify_reference(p, id);
        }
        // Cover an old worker using the legacy lock as well as managed leases.
        std::unique_ptr<fileio::SessionLock> worker;
        if (fs::exists(p.root)) {
            ordinary_path(p.root / "session.lock");
            try {
                worker = std::make_unique<fileio::SessionLock>(p.root / "session.lock");
            } catch (const std::system_error& error) {
                if (error.code().value() == EWOULDBLOCK || error.code().value() == EAGAIN) {
                    throw Error("busy", "worker still owns the session");
                }
                throw;
            }
        }
        record["status"] = "removing";
        publish(p.record, record);
        fs::remove_all(p.root); // Does not follow interior symlinks.
        // Retain only a small tombstone; retired allocations must not retain secrets.
        const auto fork_source = record.value("fork_source", Json(nullptr));
        record = {{"version", 1}, {"directory", p.root.string()}, {"allocation_id", id},
            {"status", "removed"}, {"revision", record.at("revision")}};
        if (!fork_source.is_null()) record["fork_source"] = fork_source;
        publish(p.record, record);
        return description(p, record);
    }
    require_ready(record);
    verify_reference(p, id);
    if (operation == "capture_startup") {
        const auto lifecycle = identifier(request, "lifecycle_id");
        if (!request.contains("expected_revision") || request.at("expected_revision") != record.at("revision")) {
            throw Error("conflict", "startup capture requires the current expected_revision");
        }
        Json startup = {{"lifecycle_id", lifecycle}, {"revision", record.at("revision")},
            {"configuration", effective(p, record)}};
        if (record.contains("startup") && record["startup"]["lifecycle_id"] == lifecycle
            && record["startup"] != startup) {
            throw Error("conflict", "lifecycle already has a different startup configuration");
        }
        record["startup"] = std::move(startup);
        publish(p.record, record);
        return description(p, record);
    }

    const auto document = effective(p, record);
    const auto& storage = document.at("persistence");
    const auto state_directory = child_path(p.root, storage, "state");
    ordinary_path(state_directory / "state.sqlite3");
    ordinary_path(state_directory / "state.sqlite3-wal");
    ordinary_path(state_directory / "state.sqlite3-shm");
    ordinary_path(state_directory / "state.json");
    std::string selection;
    try {
        selection = storage.value("backend",
            storage.contains("format") ? std::string("json") : std::string("auto"));
        if ((storage.contains("backend") && selection != "json" && selection != "sqlite")
            || storage.value("format", std::string("json")) != "json"
            || (storage.contains("format") && selection == "sqlite")) {
            throw Error("invalid_configuration", "conflicting or unsupported persistence format");
        }
    } catch (const Json::exception&) {
        throw Error("invalid_configuration", "persistence backend and format must be strings");
    }
    persistence::Backend backend;
    try {
        backend = persistence::select_backend(state_directory, selection);
    } catch (const std::runtime_error& error) {
        throw Error("invalid_configuration", error.what());
    }
    if (backend == persistence::Backend::Sqlite) {
        std::optional<persistence::StoredState> current;
        try {
            current = persistence::inspect_sqlite_current(state_directory);
        } catch (const std::exception& error) {
            throw Error("corrupt_storage",
                std::string("SQLite state cannot be read: ") + error.what());
        }
        Json result = {{"backend", "sqlite"}, {"state", nullptr}, {"readable", nullptr}};
        if (current) {
            result["state"] = current->state;
            result["revision"] = current->revision ? Json(*current->revision) : Json();
            result["operation_id"] = current->operation_id;
        }
        return result;
    }
    const auto state_path = state_directory / "state.json";
    ordinary_path(state_path);
    Json result = {{"backend", "json"}, {"state", nullptr}, {"readable", nullptr}};
    if (fs::exists(state_path)) {
        // Validate using the existing codec, but return the original JSON so
        // unknown compatible fields also survive inspection without clipping.
        auto state = read_json(state_path);
        try {
            (void)load::decode_state(state);
        } catch (const std::exception& error) {
            throw Error("corrupt_storage",
                std::string("stored state cannot be decoded: ") + error.what());
        }
        result["state"] = std::move(state);
    }
    const auto readable_path = state_directory / "readable.md";
    ordinary_path(readable_path);
    if (fs::exists(readable_path)) {
        result["readable"] = read_text(readable_path);
    }
    return result;
}

std::optional<Configuration> resolve_configuration(const fs::path& file, const Json& document) {
    if (!document.is_object() || !document.contains("_simplex_managed")) {
        return std::nullopt;
    }
    if (document.size() != 1) {
        throw Error("invalid_configuration", "managed references cannot contain inline overrides");
    }
    const auto& ref = document.at("_simplex_managed");
    if (!ref.is_object() || ref.value("version", 0) != 1) {
        throw Error("invalid_configuration", "unsupported managed configuration reference");
    }
    const auto id = identifier(ref, "allocation_id");
    const auto p = paths(ref);
    if (fs::absolute(file).lexically_normal() != p.config) {
        throw Error("ownership_mismatch", "managed reference was moved outside its allocation");
    }
    if (!fs::exists(p.record)) {
        throw Error("not_found", "managed allocation is missing");
    }
    auto lease = lock(p, false);
    const auto record = owned(p, id);
    require_ready(record);
    verify_reference(p, id);
    return Configuration{effective(p, record), std::move(lease)};
}

} // namespace manager
