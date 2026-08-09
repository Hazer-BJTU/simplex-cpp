#pragma once

/**
 * @file extension_framework/extensions.hpp
 * @brief Generic, domain-agnostic *extension* framework: dynamic-library
 *        discovery, loading, verification, and a base context interface.
 *
 * This is the foundation for the project-wide extension system. Where the older
 * `plugin/` framework split the work across several headers (`plugin.hpp` +
 * `registry.hpp` + `dispatcher.hpp` + `manager.hpp`), this header collapses the
 * loading half into one self-contained unit:
 *
 *   - is_likely_dynamic_library()  — platform-aware DSO file predicate.
 *   - get_library_ref()            — open a .so/.dll/.dylib into a shared handle.
 *   - create_object_from_library() — import a factory alias and mint an object.
 *   - ExtensionContext             — the abstract base every extension implements.
 *   - load_modules_directory()     — scan a dir, load every accepted module.
 *   - verify_after_loaded()        — drop failures, log diagnostics, sort by priority.
 *   - load_and_verify_directory()  — one-shot: scan + load + verify, return sorted.
 *   - create_product()             — mint a real Product from a context's library.
 *   - product_factory<>            — cached factory: resolve an alias once, create many.
 *   - ExtensionDispatcher          — directory-driven registry + key router (class).
 *
 * ## Why a single header
 *
 * The loading pipeline is short and fully generic (it knows nothing about
 * languages, commands, or any concrete domain). Keeping it in one header means a
 * new domain picks it up by including one file and supplying two callables: a
 * `Filter` (which files to try) and a `TagGenerator` (which factory alias to
 * import from a given file). `is_likely_dynamic_library` and `same_tag_already`
 * are ready-made implementations of those two policies.
 *
 * ## Lifetime model (the load-bearing invariant)
 *
 * An `ExtensionContext` produced by a dynamic library has its vtable and
 * destructor *inside* that library. The library must therefore stay mapped for
 * the entire lifetime of the context — unmapping it early is undefined behavior.
 *
 * There are two supported ownership shapes, selected by the
 * `bind_library_ref_deleter` template flag on `create_object_from_library`:
 *
 *   - **External ownership (default, `false`)** — the returned object does NOT
 *     pin the library handle; the caller must keep @p library_ref alive
 *     independently for the object's entire lifetime, including its
 *     destruction. Use only when lifetime is managed by an external owner that
 *     outlives every object (mirrors `plugin::PluginManager`). Combining this
 *     mode with `ExtensionContext::bind()` while the object is the handle's
 *     sole owner is a self-unload hazard — see the warning on
 *     `create_object_from_library`.
 *   - **Deleter-pinned (`true`)** — the handle is moved into the returned
 *     `shared_ptr`'s custom deleter, so the library is released only AFTER the
 *     object's deleting-destructor returns. This is the safe mode for a
 *     self-contained object that owns its own library. `load_modules_directory`
 *     uses it (and then `bind()`s the handle onto the context purely so
 *     `verify_after_loaded` can confirm + log the library), so each loaded
 *     context is self-sufficient and safely destructible.
 *
 * @note Why the member `bind()` alone is not enough. Stashing the handle as a
 *       member keeps the library mapped while the object is alive, but on
 *       destruction the member is destroyed *from within* the library's own
 *       deleting-destructor — unmapping the library before that destructor's
 *       `operator delete` epilogue finishes. The deleter-pinned mode releases
 *       the handle from the `shared_ptr` deleter instead, which runs after
 *       `delete p` completes, so the epilogue is safe.
 *
 * ## ABI versioning
 *
 * `ExtensionContext::abi_version()` lets a host reject an extension built
 * against an incompatible header revision. The base interface only mandates that
 * an extension *report* one; each domain owns its expected value and checks it
 * after loading (mirroring the `plugin/` ABI gate).
 *
 * This header depends on Boost.DLL (loading), nlohmann/json (`extras()`), and the
 * project logger (`verify_after_loaded` diagnostics).
 */

#include <cctype>          // std::tolower
#include <cstdint>         // std::uint32_t
#include <filesystem>
#include <format>
#include <functional>      // std::function (ExtensionDispatcher Matcher/Selector)
#include <memory>
#include <optional>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>     // std::is_function_v (resolve_factory_alias)
#include <unordered_map>   // std::unordered_map (ExtensionDispatcher name index)
#include <utility>
#include <vector>

#include <boost/dll.hpp>
#include <boost/dll/import.hpp>
#include <boost/system.hpp>

#include <nlohmann/json.hpp>

#include "logging/logger.hpp"

namespace extension {

// =============================================================================
// Platform-aware dynamic-library predicate
// =============================================================================

/**
 * @brief Best-effort test of whether @p library_path looks like a loadable
 *        dynamic library on the current platform.
 *
 * Inspects the filename extension (case-insensitively) and, on ELF platforms,
 * also accepts versioned suffixes like `libfoo.so.1`. It is deliberately a
 * *guess* by filename only — it never touches the filesystem status and never
 * attempts to actually `dlopen` the file, so it is cheap to call inside a
 * directory scan.
 *
 * The whole body is wrapped in a try/catch and declared `noexcept` because
 * `path::filename()`/`path::extension()` may throw if the path's native encoding
 * cannot be converted to the internal representation; in that pathological case
 * we simply report "not a library" rather than propagating.
 *
 * @param library_path Any path (need not exist on disk).
 * @return true if the filename has a recognized dynamic-library extension.
 */
inline bool is_likely_dynamic_library(const std::filesystem::path& library_path) noexcept {
    try {
        auto filename  = library_path.filename().string();
        auto extension = library_path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return std::tolower(c); });

#if defined(_WIN32)
        return extension == ".dll" || extension == ".ocx";

#elif defined(__APPLE__)
        return extension == ".dylib" || extension == ".so" || extension == ".bundle";

#elif defined(__linux__) || defined(__unix__) || defined(__unix)
        // Unversioned ".so" or a versioned "libfoo.so.<n>" form.
        return extension == ".so" || (filename.find(".so.") != std::string::npos);

#else
        // Unknown platform: accept any of the known extensions to stay useful.
        return extension == ".so"     ||
               extension == ".dll"    ||
               extension == ".dylib"  ||
               extension == ".bundle" ||
               (filename.find(".so.") != std::string::npos);
#endif
    } catch (...) {
        return false;
    }
}

// =============================================================================
// Tag generators (policies for load_modules_directory)
// =============================================================================

/**
 * @brief A `TagGenerator` policy that returns the same factory alias name for
 *        every file.
 *
 * `load_modules_directory` calls the tag generator once per discovered file to
 * learn which exported alias to import from it. When every module in a directory
 * exports its factory under a single well-known name (the common case), this
 * functor returns that name regardless of the file path.
 *
 * The returned `string_view` aliases the functor's own `_context_tag` member, so
 * the functor must outlive any use of the view — which it always does, since the
 * generator is only consulted synchronously during loading.
 */
struct same_tag_always {
    /// The alias name handed back for every path.
    std::string _context_tag;

    /// @return The stored alias name (ignores @p library_path).
    std::string_view operator()([[maybe_unused]] const std::filesystem::path& library_path) const noexcept {
        return _context_tag;
    }
};

// =============================================================================
// Library handle acquisition
// =============================================================================

/**
 * @brief Open a dynamic library and return a shared handle to it.
 *
 * Uses the non-throwing `std::filesystem::exists` overload (with an error_code
 * sink) for the existence check, so a permission error or an unconvertible path
 * is reported as "doesn't exist" instead of propagating a `filesystem_error`.
 * The actual load uses `boost::dll::load_mode::append_decorations`, so a bare
 * stem ("foo") and a fully-decorated name ("libfoo.so") both work.
 *
 * @param target_path Filesystem path to the library; must already exist.
 * @return A `shared_ptr` to the loaded `boost::dll::shared_library`.
 * @throw std::runtime_error if the path is missing or the library cannot be
 *               loaded (the underlying Boost exception text is folded into the
 *               message so callers get one consistent exception type).
 */
inline std::shared_ptr<boost::dll::shared_library> get_library_ref(
    const std::filesystem::path& target_path
) {
    std::error_code exists_ec;
    if (!std::filesystem::exists(target_path, exists_ec)) {
        throw std::runtime_error(
            std::format("target path doesn't exist: {}", target_path.string())
        );
    }

    try {
        auto library_ref = std::make_shared<boost::dll::shared_library>(
            target_path, boost::dll::load_mode::append_decorations
        );

        return library_ref;
    } catch (const std::exception& e) {
        throw std::runtime_error(
            std::format(
                "failed to load library from: {} due to exception: {}",
                target_path.string(),
                e.what()
            )
        );
    }
}

namespace detail {

/**
 * @brief Resolve a factory alias to its raw function pointer, with validation.
 *
 * Shared resolution step for `create_object_from_library` (one-shot) and
 * `product_factory` (cached): checks the handle and the alias, then returns the
 * resolved raw function pointer. Using the raw pointer directly — rather than
 * Boost's `library_function` wrapper that `import_alias` returns — keeps a cached
 * hot path (product_factory::create) down to a single indirect call, with no
 * per-call Boost object or shared_ptr churn.
 *
 * This mirrors exactly what `import_alias` does internally: an alias symbol
 * stores the function's address, and `get<Signature*>(name)` reads that stored
 * pointer. The only thing omitted is wrapping it in a `library_function`.
 *
 * @tparam FactorySignature  A function type, e.g. `std::unique_ptr<T>()`.
 * @param library_ref   A non-null handle to a library exporting @p target_field.
 * @param target_field  The exported alias name (string_view accepted here;
 *                      converted internally because Boost.DLL needs const char*).
 * @return The resolved factory function pointer.
 * @throw std::runtime_error if the handle is null, the alias is missing, or
 *               resolution fails. The owning library's location is included in
 *               the message to aid diagnosis.
 */
template<typename FactorySignature>
FactorySignature* resolve_factory_alias(
    const std::shared_ptr<boost::dll::shared_library>& library_ref,
    std::string_view target_field
) {
    static_assert(std::is_function_v<FactorySignature>,
                  "FactorySignature must be a function type, e.g. std::unique_ptr<T>()");

    if (library_ref == nullptr) {
        throw std::runtime_error("empty library reference (=nullptr)");
    }

    // Boost.DLL's symbol/alias lookup only accepts const char* / const std::string&,
    // so materialize the view exactly once and reuse it below.
    const std::string field{target_field};

    if (!library_ref->has(field)) {
        boost::system::error_code loc_ec;
        throw std::runtime_error(
            std::format(
                "symbol or alias name {} not found in library: {}",
                field,
                library_ref->location(loc_ec).string()
            )
        );
    }

    try {
        // An alias symbol stores the function's address; get<Signature*> returns
        // that stored function pointer (this is precisely what import_alias does
        // to obtain the pointer before wrapping it).
        return library_ref->get<FactorySignature*>(field);
    } catch (const std::exception& e) {
        boost::system::error_code loc_ec;
        throw std::runtime_error(
            std::format(
                "library: {} import error: {}",
                library_ref->location(loc_ec).string(),
                e.what()
            )
        );
    }
}

} // namespace detail

/**
 * @brief Import an extension object from an already-opened library via its
 *        exported factory alias.
 *
 * Looks up the @p target_field alias, imports it as a nullary factory returning
 * `std::unique_ptr<ExtensionObject>`, invokes it, and returns the produced
 * object wrapped in a `shared_ptr`.
 *
 * @tparam ExtensionObject            The concrete/abstract object type the
 *                                    factory mints (e.g. `ExtensionContext`).
 * @tparam bind_library_ref_deleter   Ownership mode (see the file header):
 *                                    - `false` (default): the library handle is
 *                                      NOT pinned to the object; the caller must
 *                                      keep @p library_ref alive for as long as
 *                                      the returned object is used AND destroyed.
 *                                      Use this only when lifetime is managed
 *                                      externally (e.g. a long-lived manager owns
 *                                      the handle). Do NOT combine with binding
 *                                      the handle onto the object as a member
 *                                      (`ExtensionContext::bind()`) when the
 *                                      object is the handle's sole owner — see
 *                                      the self-unload warning below.
 *                                    - `true`: @p library_ref is moved into the
 *                                      returned shared_ptr's deleter, so the
 *                                      handle is released only AFTER `delete p`
 *                                      returns. Safe for the object to be the
 *                                      sole owner of its library; use this
 *                                      whenever the object owns its own library
 *                                      (e.g. `load_modules_directory`).
 *
 * @warning Self-unload hazard. An object produced by a dynamic library has its
 *        deleting-destructor (the compiler-generated `delete p` path, including
 *        the final `operator delete`) *inside* that library. If the library
 *        handle is released as a side effect of destroying the object itself —
 *        which is exactly what happens when the handle is stored as a member and
 *        the member is the last reference — then `dlclose` runs from within the
 *        library's own destructor, unmapping the library before its
 *        destructor/operator-delete epilogue finishes, and the program crashes.
 *        The deleter-pinned mode (`true`) avoids this by releasing the handle
 *        from the shared_ptr's deleter, which runs only after `delete p` returns.
 *
 * @param library_ref   A non-null handle to the loaded library.
 * @param target_field  The exported alias name. Passed as `string_view` for
 *                      caller convenience; converted internally to `std::string`
 *                      because Boost.DLL's `has()` / `import_alias()` accept
 *                      only `const char*` / `const std::string&`.
 * @return The extension object (never null on success — a null factory result is
 *         surfaced as a thrown exception by the unique_ptr→shared_ptr move).
 * @throw std::runtime_error if the handle is null, the alias is missing, the
 *               factory throws, or the import fails. The owning library's
 *        location (via its non-throwing `location(ec)`) is included in the
 *        message to aid diagnosis.
 */
template<typename ExtensionObject, bool bind_library_ref_deleter = false>
std::shared_ptr<ExtensionObject> create_object_from_library(
    std::shared_ptr<boost::dll::shared_library> library_ref,
    std::string_view target_field
) {
    // Resolve + validate the factory symbol once: a null handle, a missing alias,
    // or a resolution failure all surface as std::runtime_error carrying the
    // library location (see detail::resolve_factory_alias).
    auto factory_function = detail::resolve_factory_alias<std::unique_ptr<ExtensionObject>()>(
        library_ref, target_field
    );

    try {
        auto new_object = factory_function();

        if constexpr (bind_library_ref_deleter) {
            // Pin the library to this object: the deleter captures the handle and
            // releases it only after `delete p` runs, so the destructor's code
            // (which lives in the library) is still mapped when it executes.
            return std::shared_ptr<ExtensionObject>(
                new_object.release(),
                [object_library_ref = std::move(library_ref)](ExtensionObject* p) {
                    delete p;
                }
            );
        } else {
            // External-ownership mode: the returned object does NOT keep
            // @p library_ref alive. The caller must hold the handle for the
            // object's entire life AND destruction (and must not make the
            // object the handle's sole owner — see the self-unload warning).
            return new_object;
        }
    } catch (const std::exception& e) {
        boost::system::error_code loc_ec;
        throw std::runtime_error(
            std::format(
                "library: {} import error: {}",
                library_ref->location(loc_ec).string(),
                e.what()
            )
        );
    }
}

// =============================================================================
// ExtensionContext — abstract base interface
// =============================================================================

/**
 * @brief Abstract base class every extension implements.
 *
 * An `ExtensionContext` is the long-lived descriptor + identity of one loaded
 * extension module. Beyond reporting its own identity (`name()`, `abi_version()`),
 * it optionally contributes ordering metadata (`priority()`) and free-form
 * diagnostics (`extras()`), and it carries the owning library handle once bound.
 *
 * The library handle is NOT part of the constructor contract: the loader
 * (`load_modules_directory`) constructs the context from the factory, then calls
 * `bind()` to attach the handle. This split lets the same factory signature be
 * used regardless of how the caller wants to manage the handle's lifetime.
 */
class ExtensionContext {
public:
    /// Default priority returned when an extension does not override `priority()`.
    static constexpr long DEFAULT_CONTEXT_PRIORITY = 0l;

private:
    /// Owning library handle, attached by `bind()`. Kept alive for the context's
    /// whole life so its vtable/destructor stay mapped.
    std::shared_ptr<boost::dll::shared_library> _library_ref = nullptr;

public:
    virtual ~ExtensionContext() = default;

    /// ABI version this extension was built against (checked by the host).
    virtual std::uint32_t abi_version() const noexcept = 0;

    /// Human-readable extension name, used for logging and diagnostics.
    virtual std::string_view name() const noexcept = 0;

    /**
     * @brief Attach the owning library handle to this context.
     *
     * After this call the context keeps the library mapped for its own lifetime,
     * satisfying the lifetime invariant in the file header. Must be called with a
     * non-null handle.
     *
     * @param library_ref The handle that produced this context (must not be null).
     * @throw std::runtime_error if @p library_ref is null.
     */
    void bind(std::shared_ptr<boost::dll::shared_library> library_ref) {
        if (library_ref == nullptr) {
            throw std::runtime_error("empty library reference");
        }

        _library_ref = std::move(library_ref);
    }

    /// @return The bound library handle (may be null before `bind()` is called).
    std::shared_ptr<boost::dll::shared_library> get_library_ref() const noexcept {
        return _library_ref;
    }

    /**
     * @brief Ordering weight used by `verify_after_loaded` to sort extensions.
     *
     * Higher sorts first. A dedicated extension should return a high value; a
     * broad catch-all/fallback should return a low (possibly negative) value so
     * it wins only when nothing more specific is present. Overriding is optional.
     *
     * @return The extension's priority (default `DEFAULT_CONTEXT_PRIORITY`).
     */
    virtual long priority() const noexcept {
        return DEFAULT_CONTEXT_PRIORITY;
    }

    /**
     * @brief Free-form diagnostic metadata for logging / introspection.
     *
     * Default returns a JSON `null`. Override to surface extension-specific info
     * (version strings, capabilities, feature flags). Returning by value keeps
     * the interface simple; keep overrides cheap.
     *
     * @return A JSON value (default `null`).
     */
    virtual nlohmann::json extras() const {
        return {};
    }
};

// =============================================================================
// Directory loading
// =============================================================================

/**
 * @brief Scan a directory and load every accepted module as an ExtensionContext.
 *
 * Two-phase: first collect all candidate files (applying @p filter, optionally
 * recursing), then attempt to load each one. Loading is per-file resilient: a
 * single bad module is recorded as an error and a null slot, never aborting the
 * rest of the scan.
 *
 * The returned context vector and the @p errors vector are *parallel*: index `i`
 * of the result corresponds to index `i` of @p errors. A null result slot is
 * always paired with an error message; a non-null slot is paired with
 * `std::nullopt`. Callers typically hand both vectors straight to
 * `verify_after_loaded()`.
 *
 * Every successfully loaded context has its library handle bound via
 * `ExtensionContext::bind()`, so the returned contexts are self-sufficient —
 * their libraries stay mapped until the contexts are destroyed.
 *
 * @tparam Filter        Callable `bool(const std::filesystem::path&)` selecting
 *                       which files to attempt (`is_likely_dynamic_library` is a
 *                       ready-made choice).
 * @tparam TagGenerator  Callable returning the alias name to import from a given
 *                       path (`same_tag_always` is a ready-made choice).
 *
 * @param directory_path  Directory to scan (must exist and be a directory).
 * @param filter          File-acceptance predicate.
 * @param tag_generator   Per-file alias-name generator.
 * @param errors[out]     Appended to, one entry per discovered module.
 * @param recursive       Recurse into subdirectories if true.
 * @return One slot per discovered module (null on per-file failure).
 * @throw std::runtime_error if the directory itself cannot be scanned (a bad
 *               directory aborts the whole call; per-file failures do not).
 */
template<typename Filter, typename TagGenerator>
std::vector<std::shared_ptr<ExtensionContext>> load_modules_directory(
    const std::filesystem::path& directory_path,
    Filter&& filter,
    TagGenerator&& tag_generator,
    std::vector<std::optional<std::string>>& errors,
    bool recursive = false
) {
    std::vector<std::filesystem::path> module_paths;
    std::vector<std::shared_ptr<ExtensionContext>> loaded;

    // ---- Phase 1: discover candidate files (no loading yet) -----------------
    // skip_permission_denied keeps a single unreadable subdir from aborting a
    // recursive walk; the per-file try/catch below handles individual load errors.
    try {
        if (recursive) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(
                     directory_path,
                     std::filesystem::directory_options::skip_permission_denied)) {
                if (std::filesystem::is_regular_file(entry.status()) && filter(entry.path())) {
                    module_paths.push_back(entry.path());
                }
            }
        } else {
            for (const auto& entry : std::filesystem::directory_iterator(
                     directory_path,
                     std::filesystem::directory_options::skip_permission_denied)) {
                if (std::filesystem::is_regular_file(entry.status()) && filter(entry.path())) {
                    module_paths.push_back(entry.path());
                }
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        throw std::runtime_error(
            std::format("failed to scan modules due to filesystem error: {}", e.what())
        );
    } catch (const std::exception& e) {
        throw std::runtime_error(
            std::format("failed to scan modules due to exception: {}", e.what())
        );
    }

    // ---- Phase 2: load each candidate, recording per-file results -----------
    for (const auto& module_path : module_paths) {
        try {
            auto context_tag = tag_generator(module_path);
            auto library_ref = get_library_ref(module_path);
            // Deleter-pinned mode (true): the library handle is captured in the
            // returned shared_ptr's deleter, so it is released only AFTER the
            // context's deleting-destructor returns. This is mandatory here:
            // the context owns its library (via bind() below), and the
            // deleting-destructor's code lives in that same library — releasing
            // the handle as a *member* of the context (the false mode + bind)
            // would dlclose() the library from inside its own destructor, crashing
            // the destructor's operator-delete epilogue in unmapped code.
            auto context_ref = create_object_from_library<ExtensionContext, true>(library_ref, context_tag);
            context_ref->bind(library_ref);

            loaded.push_back(std::move(context_ref));
            errors.push_back(std::nullopt);
        } catch (const std::exception& e) {
            // A bad module must not poison its neighbors: record a null slot and
            // the failure text, and keep going.
            loaded.push_back(nullptr);
            errors.push_back(e.what());
        }
    }

    return loaded;
}

/**
 * @brief Convenience overload of `load_modules_directory` that discards errors.
 *
 * Use when per-file diagnostics are not needed; internally it simply drops the
 * parallel error vector.
 */
template<typename Filter, typename TagGenerator>
std::vector<std::shared_ptr<ExtensionContext>> load_modules_directory(
    const std::filesystem::path& directory_path,
    Filter&& filter,
    TagGenerator&& tag_generator,
    bool recursive = false
) {
    std::vector<std::optional<std::string>> errors;
    return load_modules_directory(
        directory_path,
        std::forward<Filter>(filter),
        std::forward<TagGenerator>(tag_generator),
        errors,
        recursive
    );
}

// =============================================================================
// Post-load verification + ordering
// =============================================================================

/**
 * @brief Filter out failed loads, log diagnostics, and order survivors by priority.
 *
 * Walks the parallel `loaded`/`errors` vectors produced by
 * `load_modules_directory`. For each slot:
 *   - if null (load failed) → log the paired error (or a generic fallback) and
 *     drop it;
 *   - if the context has no bound library handle → log a warning and drop it
 *     (a context without its library is unsafe to use);
 *   - otherwise → log a success line and keep it.
 *
 * The survivors are then sorted highest-`priority()`-first. A *stable* sort is
 * used so extensions with equal priority retain their discovery order, giving
 * deterministic ordering (matching the sibling `plugin/` dispatcher's semantics).
 *
 * @param loaded  The raw result vector (taken by value; nulls allowed).
 * @param errors  The parallel error vector. Bounds-checked defensively: if it is
 *                shorter than `loaded`, missing entries use a generic message
 *                rather than reading out of bounds.
 * @return The verified, priority-sorted contexts (no nulls).
 */
inline std::vector<std::shared_ptr<ExtensionContext>> verify_after_loaded(
    std::vector<std::shared_ptr<ExtensionContext>> loaded,
    const std::vector<std::optional<std::string>>& errors
) {
    std::vector<std::shared_ptr<ExtensionContext>> verified_loaded;
    for (size_t i = 0; i < loaded.size(); ++i) {
        if (loaded[i] == nullptr) {
            // Defensive bounds check: the contract is that `errors` is parallel
            // to `loaded`, but do not trust it blindly — never read out of bounds.
            std::string reason = "unknown extension loading failure";
            if (i < errors.size()) {
                reason = errors[i].value_or(reason);
            }
            logging::Logger::warning(reason);
            continue;
        }

        auto library_ref = loaded[i]->get_library_ref();
        if (library_ref == nullptr) {
            std::string msg = std::format(
                "extension context: {} will be ignored due to lacking of library reference",
                loaded[i]->name()
            );
            logging::Logger::warning(msg);
            continue;
        }

        boost::system::error_code loc_ec;
        std::string msg = std::format(
            "extension context: {} from library: {} loaded successfully",
            loaded[i]->name(),
            library_ref->location(loc_ec).string()
        );
        logging::Logger::info(msg);
        verified_loaded.push_back(std::move(loaded[i]));
    }

    // Stable sort: equal-priority extensions keep discovery order. Descending so
    // the highest-priority extension comes first.
    using ExtPtr = std::shared_ptr<ExtensionContext>;
    std::stable_sort(
        verified_loaded.begin(),
        verified_loaded.end(),
        [](const ExtPtr& x, const ExtPtr& y) -> bool {
            return x->priority() > y->priority();
        }
    );

    return verified_loaded;
}

// =============================================================================
// One-shot load + verify (composes load_modules_directory + verify_after_loaded)
// =============================================================================

/**
 * @brief Load every module in a directory and return the verified, sorted set.
 *
 * This is the high-level convenience entry point most hosts actually want: it
 * composes `load_modules_directory` and `verify_after_loaded` so that one call
 * turns a directory into a ready-to-use, ordered context list. Concretely it:
 *
 *   1. scans @p directory_path for accepted files (applying @p filter, optionally
 *      recursing) and loads each one as a deleter-pinned, self-sufficient context;
 *   2. drops any module that failed to load or was left without a bound library
 *      handle, logging a diagnostic for each; and
 *   3. returns the survivors ordered highest-`priority()`-first (stable, so equal
 *      priorities keep discovery order).
 *
 * The returned list contains only usable contexts — no nulls, no unbound
 * contexts. Per-file load failures do NOT abort the call: they are appended to
 * @p errors (parallel to the raw load result) and silently dropped from the
 * returned list, exactly as if the two underlying functions had been called by
 * hand. The only condition that throws is a failure to scan @p directory_path
 * itself.
 *
 * @tparam Filter        Callable `bool(const std::filesystem::path&)` selecting
 *                       which files to attempt (`is_likely_dynamic_library` is a
 *                       ready-made choice).
 * @tparam TagGenerator  Callable returning the alias name to import from a given
 *                       path (`same_tag_always` is a ready-made choice).
 *
 * @param directory_path  Directory to scan (must exist and be a directory).
 * @param filter          File-acceptance predicate.
 * @param tag_generator   Per-file alias-name generator.
 * @param errors[out]     Appended to, one entry per discovered module (null slot
 *                        on the raw load = error message here; usable context =
 *                        `std::nullopt`).
 * @param recursive       Recurse into subdirectories if true.
 * @return The verified, priority-sorted contexts (no nulls, no unbound contexts).
 * @throw std::runtime_error if @p directory_path itself cannot be scanned (a bad
 *               directory aborts the whole call; per-file failures do not).
 *
 * @see load_modules_directory  for the raw load step.
 * @see verify_after_loaded      for the filter + sort step.
 */
template<typename Filter, typename TagGenerator>
std::vector<std::shared_ptr<ExtensionContext>> load_and_verify_directory(
    const std::filesystem::path& directory_path,
    Filter&& filter,
    TagGenerator&& tag_generator,
    std::vector<std::optional<std::string>>& errors,
    bool recursive = false
) {
    // Load first: produces a parallel (context, error) slot per discovered module,
    // with null context slots paired to error messages. The contexts are already
    // deleter-pinned + bound, so they are safe to hold for the caller's lifetime.
    auto loaded = load_modules_directory(
        directory_path,
        std::forward<Filter>(filter),
        std::forward<TagGenerator>(tag_generator),
        errors,
        recursive
    );

    // Then filter out the nulls / unbound contexts, log diagnostics, and stable-
    // sort survivors by priority. `loaded` is moved in (verify takes it by value)
    // since the raw vector is not needed beyond this point.
    return verify_after_loaded(std::move(loaded), errors);
}

/**
 * @brief Convenience overload of `load_and_verify_directory` that discards errors.
 *
 * Use when per-file diagnostics are not needed: returns only the usable,
 * priority-sorted contexts. Internally it discards the parallel error vector, so
 * the caller gives up visibility into which modules (if any) failed to load.
 */
template<typename Filter, typename TagGenerator>
std::vector<std::shared_ptr<ExtensionContext>> load_and_verify_directory(
    const std::filesystem::path& directory_path,
    Filter&& filter,
    TagGenerator&& tag_generator,
    bool recursive = false
) {
    std::vector<std::optional<std::string>> errors;
    return load_and_verify_directory(
        directory_path,
        std::forward<Filter>(filter),
        std::forward<TagGenerator>(tag_generator),
        errors,
        recursive
    );
}

// =============================================================================
// Product creation
// =============================================================================

/**
 * @brief Create a real `Product` object from a loaded context's bound library.
 *
 * Thin wrapper over `create_object_from_library`: it pulls the bound library
 * handle off @p context and imports @p factory_alias as a
 * `std::unique_ptr<Product>()` factory, minting a new `Product`. Use this to turn
 * a dispatched context into the actual service/object it provides — the creation
 * path a concrete `Plugin::create()` used to own is now just this one call.
 *
 * The default deleter-pinned mode (`bind_library_ref_deleter = true`) is safe:
 * the `Product`'s `shared_ptr` deleter captures the handle, so the library stays
 * mapped through the `Product`'s deleting-destructor — the same lifetime
 * invariant documented on `create_object_from_library`, and the same reason
 * `load_modules_directory` uses that mode. The context independently keeps its
 * *own* bound reference, so context and product may outlive each other freely.
 *
 * @tparam Product                  The concrete product type the factory mints.
 * @tparam bind_library_ref_deleter Ownership mode (default `true` = self-contained
 *                                  and safe; `false` = external ownership, caller
 *                                  must keep the context/library alive).
 * @param context        A loaded context whose bound library exports @p factory_alias.
 * @param factory_alias  The exported factory alias that mints the `Product`.
 * @return The created `Product` (never null on success).
 * @throw std::runtime_error if @p context is null, has no bound library handle,
 *               the alias is missing, or the factory/import fails (all forwarded
 *               from the null-context check here or from `create_object_from_library`).
 *
 * @see create_object_from_library  for the underlying import + lifetime semantics.
 * @see ExtensionDispatcher::dispatch  for how to obtain @p context for a given key.
 */
template<typename Product, bool bind_library_ref_deleter = true>
std::shared_ptr<Product> create_product(
    const std::shared_ptr<ExtensionContext>& context,
    std::string_view factory_alias
) {
    if (context == nullptr) {
        throw std::runtime_error("cannot create product from a null context");
    }
    // A verified context always has a bound handle; create_object_from_library
    // re-checks for null and surfaces a clear error if it is somehow absent.
    return create_object_from_library<Product, bind_library_ref_deleter>(
        context->get_library_ref(), factory_alias
    );
}

/**
 * @brief Cached factory handle: resolve a Product's factory alias ONCE, then mint
 *        many Products by invoking the cached pointer.
 *
 * `create_product` / `create_object_from_library` redo the full symbol resolution
 * (the `has()` lookup + the alias dereference) on *every* call — fine for a
 * one-shot, but on a hot path that mints many Products from one library that is
 * wasted work. `product_factory` pays that cost once, in the constructor, and
 * stores the resolved raw function pointer; `create()` then costs nothing but a
 * single indirect call.
 *
 * The factory holds its own reference to the library (the handle it resolved
 * from), keeping it mapped for the factory's lifetime. Created Products follow
 * the same two ownership modes as `create_object_from_library`, selected by
 * @p bind_library_ref_deleter:
 *
 *   - **`true` (default)** — each Product is deleter-pinned: its shared_ptr
 *     deleter captures the library ref, so it is safely destructible *even after
 *     the factory is destroyed*. Self-contained, matches `create_product`'s
 *     default. Costs one control-block allocation per Product (unavoidable with a
 *     custom deleter).
 *   - **`false`** — Products are plain shared_ptrs with no deleter (no per-Product
 *     allocation). They rely on the factory (its held library ref) outliving
 *     every one of them — the cheaper, external-ownership mode. Use only when the
 *     factory's lifetime strictly encloses all Products'.
 *
 * Typical hot-path usage:
 * @code
 *   product_factory<MyService> factory{ctx->get_library_ref(), "create_service"};
 *   for (...) {
 *       auto svc = factory.create();   // no symbol resolution, just a call
 *   }
 * @endcode
 *
 * @tparam Product                  The concrete product type the factory mints.
 * @tparam bind_library_ref_deleter Ownership mode for created Products (default
 *                                  `true` = self-contained / safe).
 */
template<typename Product, bool bind_library_ref_deleter = true>
class product_factory {
public:
    /// The factory's function type: nullary, returns `unique_ptr<Product>`.
    using factory_signature = std::unique_ptr<Product>();
    /// Resolved raw factory function pointer, cached at construction.
    using factory_pointer = factory_signature*;

    /**
     * @brief Resolve the factory alias from a loaded library handle.
     *
     * @param library_ref  A non-null handle to a library exporting @p alias.
     * @param alias        The exported factory alias (e.g. "create_my_product").
     * @throw std::runtime_error if the handle is null, the alias is missing, or
     *               resolution fails (forwarded from detail::resolve_factory_alias,
     *               message includes the library location).
     */
    product_factory(std::shared_ptr<boost::dll::shared_library> library_ref,
                    std::string_view alias)
        : _library_ref(std::move(library_ref)) {
        _factory = detail::resolve_factory_alias<factory_signature>(_library_ref, alias);
    }

    // The state is a shared_ptr + a raw pointer, so default copy/move are correct
    // (copies share the resolved pointer and the library ref).
    product_factory(const product_factory&) = default;
    product_factory(product_factory&&) noexcept = default;
    product_factory& operator=(const product_factory&) = default;
    product_factory& operator=(product_factory&&) noexcept = default;

    /**
     * @brief Mint one Product by invoking the cached factory pointer.
     *
     * No symbol resolution happens here — only a single indirect call through the
     * pointer cached at construction.
     *
     * @return The created Product (self-contained in the default mode).
     */
    std::shared_ptr<Product> create() const {
        auto new_object = _factory();   // unique_ptr<Product> via the cached pointer
        if constexpr (bind_library_ref_deleter) {
            // Deleter-pinned: capture the library ref so the Product is safely
            // destructible even if this factory is gone first.
            return std::shared_ptr<Product>(
                new_object.release(),
                [object_library_ref = _library_ref](Product* p) { delete p; }
            );
        } else {
            // External ownership: the Product does not pin the library; this
            // factory (its _library_ref) must outlive every created Product.
            return new_object;
        }
    }

    /// The library handle this factory resolved from (kept mapped while alive).
    const std::shared_ptr<boost::dll::shared_library>& library_ref() const noexcept {
        return _library_ref;
    }

private:
    /// Keeps the owning library mapped; the factory pointer's code lives in it.
    std::shared_ptr<boost::dll::shared_library> _library_ref;
    /// Resolved once in the constructor; invoked per create().
    factory_pointer _factory = nullptr;
};

// =============================================================================
// ExtensionDispatcher — directory-driven context registry + key router
// =============================================================================
//
// A stateful counterpart to the free functions above: it owns the verified
// ExtensionContext list produced by importing one or more directories, maintains
// a name -> context index for O(1) exact-name lookup, and routes a request key to
// the best matching context. The Matcher and Selector are bound at run time via
// `std::function` members (set_matcher / set_selector), NOT template parameters,
// so `ExtensionDispatcher` itself is a single non-templated type — you reconfigure
// its routing without changing its type.
//
// ## Two independent customization levers
//
//   1. Composition (no subclass needed). set_matcher() / set_selector() swap the
//      matching and selection rules at run time. The defaults match by name()
//      and prefer higher priority(), so the dispatcher works as a name-keyed
//      router straight out of the box.
//
//   2. Inheritance. load_directory() delegates to a protected virtual
//      import_directory(); the default implementation calls
//      load_and_verify_directory(), but a subclass overrides import_directory()
//      to change how a directory becomes contexts (multiple sources, a custom
//      verifier, in-memory assembly, etc.) while reusing the indexing +
//      dispatch machinery. The class has a virtual destructor, so it is designed
//      to be derived from.
//
// The framework still knows nothing about your domain: a Matcher typically
// downcasts the base shared_ptr (`std::dynamic_pointer_cast<YourContext>`) to read
// category-specific state — the concrete context *is* the payload, so no separate
// per-entry datum is needed.
//
// ## Lifetime
//
// Entries are shared_ptr<ExtensionContext>, each self-sufficient via the
// deleter-pinned bind from load_and_verify_directory. The dispatcher may be
// copied, moved, and destroyed freely; clear() or destruction releases the
// contexts (and, when the last reference drops, their libraries).

/**
 * @brief Stateful directory-driven context registry + key router.
 *
 * Holds a verified context list + a name index, and routes a key to the best
 * matching context. Populate it with load_directory() (or add() for in-memory
 * contexts), configure matching/selection with set_matcher()/set_selector(), and
 * query with dispatch() / find().
 */
class ExtensionDispatcher {
public:
    /// Owned context pointer type.
    using ContextPtr = std::shared_ptr<ExtensionContext>;

    /// "Does this context claim @p key?" Default matches `ctx->name() == key`.
    using Matcher = std::function<bool(const ContextPtr&, std::string_view)>;

    /// "Is @p a strictly better than @p b?" Default = higher `priority()`.
    using Selector = std::function<bool(const ContextPtr&, const ContextPtr&)>;

    /// Directory-scan file predicate (`is_likely_dynamic_library` is the norm).
    using Filter = std::function<bool(const std::filesystem::path&)>;

    /// Per-file factory-alias generator (`same_tag_always` is the norm).
    using TagGenerator = std::function<std::string_view(const std::filesystem::path&)>;

    ExtensionDispatcher() = default;
    virtual ~ExtensionDispatcher() = default;

    // The state is all shared_ptr / std::function / value types, so the default
    // copy/move are correct (copying shares the contexts via refcount).
    ExtensionDispatcher(const ExtensionDispatcher&) = default;
    ExtensionDispatcher(ExtensionDispatcher&&) noexcept = default;
    ExtensionDispatcher& operator=(const ExtensionDispatcher&) = default;
    ExtensionDispatcher& operator=(ExtensionDispatcher&&) noexcept = default;

    // ---- manual registration -------------------------------------------------

    /**
     * @brief Register a single context directly (no directory needed).
     *
     * Appends @p context to the list and indexes it by `name()`. Useful for
     * in-memory contexts or tests. A later context under an already-indexed name
     * does NOT overwrite the first (the index keeps the earliest); it is still
     * appended to the list and reachable via dispatch().
     *
     * @return true if added; false if @p context was null (nothing changes).
     */
    bool add(ContextPtr context) {
        if (!context) {
            return false;
        }
        // emplace leaves an existing name untouched; the context is still pushed
        // below so dispatch() can still reach it by key.
        _by_name.emplace(std::string(context->name()), context);
        _contexts.push_back(std::move(context));
        return true;
    }

    // ---- directory import ----------------------------------------------------

    /**
     * @brief Import a directory's modules, verify, and append to the registry.
     *
     * Calls the protected virtual import_directory() (which defaults to
     * load_and_verify_directory()), then appends the surviving contexts and
     * indexes each by `name()`. Override import_directory() in a subclass to
     * change the import/verify step without touching indexing.
     *
     * Safe to call multiple times to merge directories. Per-file load failures
     * are surfaced through @p errors and dropped from the result (they never
     * reach the registry); only a directory-scan failure throws.
     *
     * @return The number of contexts added this call.
     * @throw std::runtime_error if the directory itself cannot be scanned
     *               (forwarded from load_and_verify_directory / import_directory).
     */
    std::size_t load_directory(
        const std::filesystem::path& directory,
        Filter filter,
        TagGenerator tag_generator,
        std::vector<std::optional<std::string>>& errors,
        bool recursive = false
    ) {
        auto imported = import_directory(
            directory, std::move(filter), std::move(tag_generator), errors, recursive
        );
        for (auto& ctx : imported) {
            if (!ctx) {
                continue;                 // defensive: verify_after_loaded yields none
            }
            _by_name.emplace(std::string(ctx->name()), ctx); // first name wins
            _contexts.push_back(std::move(ctx));
        }
        return imported.size();
    }

    /// Convenience overload of load_directory() that discards per-file errors.
    std::size_t load_directory(
        const std::filesystem::path& directory,
        Filter filter,
        TagGenerator tag_generator,
        bool recursive = false
    ) {
        std::vector<std::optional<std::string>> errors;
        return load_directory(
            std::move(directory), std::move(filter), std::move(tag_generator), errors, recursive
        );
    }

    // ---- name index ----------------------------------------------------------

    /// O(1) lookup of the context first registered under @p name, or null.
    ContextPtr find(std::string_view name) const {
        const auto it = _by_name.find(std::string(name));
        return it == _by_name.end() ? nullptr : it->second;
    }

    /// Whether any context is registered under @p name.
    bool contains(std::string_view name) const noexcept {
        return _by_name.find(std::string(name)) != _by_name.end();
    }

    // ---- context list --------------------------------------------------------

    /// Read-only view of the verified, ordered context list.
    const std::vector<ContextPtr>& contexts() const noexcept { return _contexts; }
    /// Number of registered contexts.
    std::size_t size() const noexcept { return _contexts.size(); }
    /// Whether the registry is empty.
    bool empty() const noexcept { return _contexts.empty(); }

    /// Drop every context and clear the name index.
    void clear() noexcept {
        _contexts.clear();
        _by_name.clear();
    }

    // ---- dispatch ------------------------------------------------------------

    /**
     * @brief Route @p key to the best matching context, or null if none claim it.
     *
     * Iterates the context list, keeps those for which the bound Matcher returns
     * true, and returns the one that beats the rest under the bound Selector.
     * Stable among equals: the Selector is a strict comparator, so a tie keeps
     * the first-seen context. A null result means "no context claims this key" —
     * a normal outcome, not an error.
     *
     * @return The best matching context, or a null shared_ptr.
     */
    ContextPtr dispatch(std::string_view key) const {
        return dispatch(key, _matcher);
    }

    /**
     * @brief One-off dispatch with an explicit matcher, ignoring the bound one.
     *
     * Use when a single lookup needs a different matching rule without disturbing
     * the configured Matcher.
     */
    ContextPtr dispatch(std::string_view key, const Matcher& matcher) const {
        ContextPtr best;
        for (const auto& ctx : _contexts) {
            if (!ctx) {
                continue;                 // defensive: the list holds no nulls
            }
            if (!matcher(ctx, key)) {
                continue;                 // not a candidate for this key
            }
            // Strict comparator: on a tie _selector returns false both ways, so
            // `best` (the earlier-seen context) is retained — stable selection.
            if (!best || _selector(ctx, best)) {
                best = ctx;
            }
        }
        return best;
    }

    // ---- matcher / selector binding -----------------------------------------

    /// Bind the Matcher used by the one-argument dispatch().
    void set_matcher(Matcher matcher) { _matcher = std::move(matcher); }
    /// Bind the Selector used by dispatch() to pick among matches.
    void set_selector(Selector selector) { _selector = std::move(selector); }
    /// The currently bound Matcher.
    const Matcher& matcher() const noexcept { return _matcher; }
    /// The currently bound Selector.
    const Selector& selector() const noexcept { return _selector; }

protected:
    /**
     * @brief Virtual hook: produce the verified context list for a directory.
     *
     * The default forwards to load_and_verify_directory(). Override to change
     * how a directory becomes contexts (merge several sources, apply a custom
     * verifier, build in memory, ...) while reusing load_directory()'s indexing.
     * Called only from load_directory(), i.e. after construction — so the virtual
     * dispatch reaches the most-derived override.
     *
     * @return The verified contexts to append (nulls are tolerated and skipped).
     */
    virtual std::vector<ContextPtr> import_directory(
        const std::filesystem::path& directory,
        Filter filter,
        TagGenerator tag_generator,
        std::vector<std::optional<std::string>>& errors,
        bool recursive
    ) {
        return load_and_verify_directory(
            directory, std::move(filter), std::move(tag_generator), errors, recursive
        );
    }

    /// The registry: verified contexts in load/add order.
    std::vector<ContextPtr> _contexts;
    /// name() -> context index (first registration of a name wins).
    std::unordered_map<std::string, ContextPtr> _by_name;
    /// Bound matcher (default: match by name()).
    Matcher _matcher = [](const ContextPtr& c, std::string_view key) {
        return c->name() == key;
    };
    /// Bound selector (default: higher priority() first).
    Selector _selector = [](const ContextPtr& a, const ContextPtr& b) {
        return a->priority() > b->priority();
    };
};

} // namespace extension
