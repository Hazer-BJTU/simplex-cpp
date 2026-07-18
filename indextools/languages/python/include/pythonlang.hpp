#pragma once

/**
 * @file pythonlang.hpp
 * @brief Python source code analyzer for the indextools entity extraction framework.
 *
 * PythonLanguage uses tree-sitter's Python grammar to parse source files and
 * extract indexed entities — functions, classes, methods, imports, global
 * variables, lambda expressions, and general identifiers. The extracted
 * entities are intended for downstream AI-based code understanding and
 * navigation (e.g. an LLM consuming the entity list to answer questions about
 * the codebase).
 *
 * The analyzer produces a flat EntityList where hierarchical relationships
 * (e.g. a method belonging to a class) are expressed through parent/child
 * index pairs rather than nested data structures. This makes the list easy
 * to iterate, serialize, and consume by external tools.
 */

#include "lang.hpp"

#include <tree_sitter/api.h>
#include <tree_sitter/tree-sitter-python.h>

namespace indextools {

/**
 * @brief Concrete LangAnalyze for Python source files.
 *
 * Parses Python source via tree-sitter and produces a flat list of
 * PythonLanguage::EntityTag instances, each describing a named code element
 * with its scope chain, source content (dedented), and byte/line positions.
 *
 * ## Extraction categories
 *
 * | Node type(s)                    | Entity type(s)        | Creates scope? |
 * |---------------------------------|-----------------------|----------------|
 * | module                          | module                | yes            |
 * | function_definition             | function_definition, class_method | yes  |
 * | class_definition                | class_definition      | yes            |
 * | import_statement, import_from_* | import_statement      | no             |
 * | assignment, augmented_assignment| global_variables, class_attribute | no |
 * | lambda                          | lambda                | no             |
 * | identifier                      | (line map entry only) | no             |
 *
 * ## Scope tracking: _named_scope and _named_scope_entity_idx
 *
 * These two parallel stacks track the current lexical scope during traversal:
 *
 *   - _named_scope:   stack of (scope_type, scope_name) pairs.
 *                     Each entry describes one enclosing scope (module, class,
 *                     function, etc.). The full stack at any point represents
 *                     the scope chain from outermost to innermost.
 *
 *   - _named_scope_entity_idx:  parallel stack holding the index (into
 *                     _result) of the EntityTag that introduced each scope.
 *                     This lets us establish parent-child links: when a new
 *                     entity is created, its parent is the entity at the top
 *                     of this stack.
 *
 * When an extractor returns `true`, the traversal pushes onto both stacks
 * (the entity introduces a new named scope). When it returns `false`, the
 * stacks are left unchanged (the entity lives in the current scope but
 * does not create a new one for its children).
 *
 * ## Iterative traversal with phase tracking
 *
 * Instead of recursion (which risks stack overflow on deeply nested ASTs),
 * _recursive_extract_entity uses an explicit stack with a two-phase protocol:
 *
 *   - Phase 0 (enter):  process the node (dispatch to the appropriate
 *     extractor), then push a phase-1 frame followed by phase-0 frames for
 *     all children (in reverse order, so they are processed left-to-right).
 *
 *   - Phase 1 (exit):   if this node introduced a named scope, pop the
 *     _named_scope and _named_scope_entity_idx stacks.
 *
 * This manual two-pass approach mimics the natural enter/exit pattern of
 * recursive descent but keeps all state on the heap.
 */
class PythonLanguage: public LangAnalyze {
public:
    /**
     * @brief Python-specific entity descriptor.
     *
     * Each EntityTag represents one code element extracted from the source.
     * It records the element's type, key (a lookup-friendly identifier),
     * fully-qualified scope chain, dedented source content, and positional
     * information in both line and byte coordinates.
     *
     * Hierarchical relationships are stored as indices into the parent
     * EntityList: `parent_entity_idx` points to the enclosing entity,
     * `children_entity_idx` lists entities directly contained within this one.
     */
    struct EntityTag: public LangAnalyze::EntityTag {
        /**
         * @brief Scope chain representation.
         *
         * Each tuple is (scope_type, scope_name). Example for a class method
         * `MyClass.foo()` defined at module level in `example.py`:
         *
         *   [("module", "example.py"),
         *    ("class_definition", "MyClass"),
         *    ("class_method", "foo")]
         *
         * The first element is always the module (outermost scope).
         */
        using NamedScope = std::vector<std::tuple<std::string, std::string>>;

        // --- Core identity ---

        /// Entity category string, e.g. "function_definition", "class_method", "lambda".
        std::string type;
        /// Lookup key, always lowercased for case-insensitive matching.
        /// For named entities (functions, classes) this is the name;
        /// for lambdas it is derived from context or line number;
        /// for imports it is empty (content is left for AI analysis).
        std::string key;

        // --- Scope ---

        /// Full scope chain from module down to this entity (inclusive).
        /// This is a snapshot taken at entity creation time — it does not
        /// reflect runtime changes to the _named_scope stack.
        NamedScope qualified_name;

        // --- Source content ---

        /// Dedented source lines for this entity. For functions and classes
        /// this is the signature only (not the body), so the AI receives a
        /// compact representation. For imports and variables it is the full
        /// statement text.
        std::vector<std::string> content;

        // --- Position (0-based) ---

        /// Starting line index (0-based) into the source file.
        size_t line_start;
        /// Ending line index (0-based, inclusive) in the source file.
        size_t line_end;
        /// Starting byte offset (0-based) into the raw source string.
        size_t byte_start;
        /// Ending byte offset (0-based, exclusive) into the raw source string.
        size_t byte_end;

        /// Absolute filesystem path of the source file.
        std::filesystem::path absolute_path;

        // --- Hierarchy ---

        /// Index of the parent entity in the EntityList, or std::nullopt for
        /// the module entity (which has no parent).
        std::optional<size_t> parent_entity_idx;
        /// Indices of child entities (those whose parent_entity_idx points here).
        std::vector<size_t> children_entity_idx;

        EntityTag(
            std::string_view type,
            std::string_view key,
            NamedScope qualified_name,
            std::vector<std::string> content,
            size_t line_start,
            size_t line_end,
            size_t byte_start,
            size_t byte_end,
            const std::filesystem::path& absolute_path,
            std::optional<size_t> parent_entity_idx
        );
        ~EntityTag() override = default;
        std::string_view get_key() const noexcept override;
        /**
         * @brief Format the qualified name as a human-readable string.
         *
         * Example output:
         *   "example.py(module).MyClass(class_definition).foo(class_method)"
         *
         * Each scope level is formatted as `name(type)`, joined by dots.
         */
        std::string get_qualified_name_str() const noexcept;
        nlohmann::json to_dict() const noexcept override;
        std::unique_ptr<LangAnalyze::EntityTag> clone() const noexcept override;
    };

    /**
     * @brief Maps lowercased identifier names to the set of line numbers
     *        (0-based) where they appear in the source.
     *
     * This is used by locate_identifier() to support quick positional lookups
     * without re-scanning the source. locate_identifier() expands matched lines
     * with surrounding context and returns a displayable JSON object.
     */
    using LineIndex = std::unordered_map<std::string, std::unordered_set<size_t>>;

private:
    // ========================================================================
    // Analysis state (cleared on each analyze() or reset() call)
    // ========================================================================

    /// Flat list of all extracted entities. The module entity (if present) is
    /// always at index 0; subsequent entities are appended in AST traversal order.
    EntityList _result;

    /// Map from lowercased identifier name → set of line numbers where it occurs.
    /// Built incrementally during traversal by _extract_general_identifier.
    LineIndex _identifier_line_map;

    /// Lexical scope stack. Each entry is (scope_type, scope_name).
    /// Pushed when entering a scope-introducing entity (module, function, class),
    /// popped on exit. The full stack at any point represents the current scope
    /// chain from outermost to innermost.
    EntityTag::NamedScope _named_scope;

    /// Parallel to _named_scope: stores the _result index of the entity that
    /// introduced each scope level. Used to resolve parent_entity_idx for
    /// newly created entities and to populate children_entity_idx on the parent.
    std::vector<size_t> _named_scope_entity_idx;

public:
    PythonLanguage() = default;
    ~PythonLanguage() override = default;
    PythonLanguage(const PythonLanguage&) = default;
    PythonLanguage(PythonLanguage&&) noexcept = default;
    PythonLanguage& operator = (const PythonLanguage&) = default;
    PythonLanguage& operator = (PythonLanguage&&) noexcept = default;

private:
    // ========================================================================
    // Core traversal
    // ========================================================================

    /**
     * @brief Iterative depth-first AST traversal with enter/exit phase tracking.
     *
     * This is the central dispatch loop. It walks the tree-sitter AST using an
     * explicit stack (avoiding recursion) and dispatches each node to the
     * appropriate _extract_* method based on its tree-sitter node type.
     *
     * ## Phase protocol
     *
     * Each stack frame has a `phase` field:
     *   - phase 0 (enter): process the node via the type dispatch table,
     *     then push a phase-1 frame followed by all children (reversed so
     *     they are visited left-to-right).
     *   - phase 1 (exit):  if the node's extractor returned true (meaning it
     *     introduced a named scope), pop _named_scope and
     *     _named_scope_entity_idx to restore the parent scope.
     *
     * ## Why iterative instead of recursive?
     *
     * Tree-sitter ASTs can be arbitrarily deep (e.g. deeply nested lambdas or
     * expressions). A recursive implementation would risk stack overflow on
     * pathological inputs. The explicit stack keeps all traversal state on the
     * heap, bounded only by available memory.
     *
     * @param node The root AST node to traverse (typically the tree root).
     */
    void _recursive_extract_entity(TSNode node) noexcept;

    /**
     * @brief Extract the full source text spanned by a tree-sitter node.
     *
     * Uses byte offsets from tree-sitter (ts_node_start_byte / ts_node_end_byte)
     * to slice the original source string. This is O(1) in the substring length
     * for std::string (copy-on-write or small-string-optimized, depending on STL).
     *
     * @param node The tree-sitter node to extract content from.
     * @return The substring of the source covered by the node, or empty string
     *         if the node is null.
     */
    std::string _get_full_node_content(TSNode node) const noexcept;

public:
    // ========================================================================
    // Public interface (overrides from LangAnalyze)
    // ========================================================================

    /**
     * @brief Parse the loaded source and populate the entity list.
     *
     * Creates a fresh tree-sitter parser, sets the Python grammar, and parses
     * the source. On success, clears previous results and runs the recursive
     * entity extraction starting from the root AST node. On parse failure
     * (tree == nullptr), clears all state and returns early — the caller can
     * inspect the empty result() to detect failure.
     *
     * @return this pointer for method chaining.
     */
    PythonLanguage* analyze() noexcept override;

    /**
     * @brief Reset all analysis state.
     *
     * Calls LangAnalyze::reset() (clearing source, lines, and path) and
     * additionally clears the entity list, identifier map, and scope stacks.
     * The object can be reused for a new analysis after this call.
     *
     * @return this pointer for method chaining.
     */
    PythonLanguage* reset() noexcept override;
    const EntityList& result() const noexcept override;
    /// Locates an identifier in the source and returns a displayable JSON array
    /// with metadata and matching lines with surrounding context.
    /// The number of context lines is controlled by set_context_lines() (default 2).
    nlohmann::json locate_identifier(std::string_view identifier) const noexcept override;
    nlohmann::json locate_entity(std::string_view entity_key) const noexcept override;
    /**
     * @brief Returns the full entity tree starting from the module root.
     *
     * Locates the top-level module entity in the analysis result and serializes
     * it together with its entire descendant subtree via _serialize_with_children().
     * Each entity carries "meta", "text", and "sub_entity" sections.
     *
     * @return A JSON array with one element (the module entity and its full tree),
     *         or an empty array if no module entity was extracted.
     */
    nlohmann::json get_full_structure() const noexcept override;
    /// Expose the full identifier → lines map (useful for debugging and IDE integration).
    const LineIndex& get_identifier_line_map() const noexcept;

private:
    // ========================================================================
    // Entity extractors
    //
    // Each _extract_* method is called when the traversal encounters a node
    // of the corresponding tree-sitter type. Return value convention:
    //
    //   - true  → this entity introduces a new named scope.
    //             The traversal will push onto _named_scope and
    //             _named_scope_entity_idx, then pop them on exit (phase 1).
    //
    //   - false → this entity does NOT introduce a named scope.
    //             It still may create an entity in _result, but the scope
    //             stacks are not modified.
    //
    // Implementation note: most extractors follow the same pattern —
    //   (1) read node positions from tree-sitter
    //   (2) resolve parent entity from _named_scope_entity_idx
    //   (3) build the EntityTag with the current scope snapshot
    //   (4) append to _result and wire up parent-child links
    // ========================================================================

    /**
     * @brief Recursively serialize an entity together with its entire descendant
     *        subtree.
     *
     * The base representation for each entity comes from to_dict(), which
     * provides "meta" and "text" sections. This helper adds a third section:
     *
     *   "sub_entity" : [ ... ]     // ordered list of child entities, each
     *                               // recursively serialized the same way
     *
     * The recursion naturally terminates at leaf entities (those with an empty
     * children_entity_idx), which receive an empty "sub_entity" array.
     *
     * @param idx  Index into the _result entity list.
     * @return A JSON object with "meta", "text", and "sub_entity" keys,
     *         or an empty object if @p idx is out of bounds.
     */
    nlohmann::json _serialize_with_children(size_t idx) const noexcept;

    /**
     * @brief Extract decorator position information for a decorated definition.
     *
     * Tree-sitter wraps decorated functions/classes in a `decorated_definition`
     * parent node. The decorator nodes appear as siblings before the actual
     * definition node. This method walks the parent's children to find the
     * first decorator and returns its position, so the entity's line_start
     * and byte_start can be adjusted to include decorators.
     *
     * @param node The function_definition or class_definition node.
     * @return (line, byte_start) of the first decorator if the parent is a
     *         decorated_definition with at least one decorator; std::nullopt
     *         otherwise (node is not decorated, or is null).
     */
    std::optional<std::tuple<size_t, size_t>> _extract_decorator(TSNode node) noexcept;

    /**
     * @brief Extract the module entity (root of the scope chain).
     *
     * The module is always the first entity in _result (index 0). It uses the
     * source filename (without directory) as both key and name. The module has
     * no parent (parent_entity_idx = nullopt) and empty content (the module
     * body is covered by its child entities).
     *
     * @return true (module introduces a named scope).
     */
    bool _extract_module(TSNode node) noexcept;

    /**
     * @brief Extract a function_definition or class_definition entity.
     *
     * Handles three entity types:
     *   - function_definition at module level    → type = "function_definition"
     *   - function_definition inside a class     → type = "class_method"
     *   - class_definition                       → type = "class_definition"
     *
     * The type is determined at runtime by inspecting the parent entity's type:
     * if the parent is a class_definition and the current node is a
     * function_definition, the entity type is changed to "class_method".
     *
     * Content extraction: only the signature lines are included (from the
     * entity start to the beginning of the body block), not the full function
     * body. This keeps the content compact for AI analysis — the AI sees the
     * API surface without implementation noise.
     *
     * Decorator handling: if the node is wrapped in a decorated_definition,
     * line_start and byte_start are adjusted backward to include decorator
     * lines (e.g. "@staticmethod\n").
     *
     * Key lowercasing: the function/class name is lowercased to enable
     * case-insensitive lookups.
     *
     * @return true (functions and classes introduce named scopes).
     */
    bool _extract_class_function_definition(TSNode node) noexcept;

    /**
     * @brief Extract an import_statement or import_from_statement entity.
     *
     * Imports are stored with an empty key — the raw import text is preserved
     * in the content field for downstream AI analysis to interpret. This
     * decision was made because tree-sitter's import node structure varies
     * significantly across forms (simple import, from-import, aliased import,
     * multi-name import) and AI-based analysis is more robust than manual
     * AST-walking for this task.
     *
     * The qualified_name is a snapshot of _named_scope plus a synthetic
     * ("import_statement", "") entry. The snapshot is taken (not referenced)
     * because imports do not push onto _named_scope.
     *
     * @return false (imports do not introduce named scopes).
     */
    bool _extract_import_statement(TSNode node) noexcept;

    /**
     * @brief Extract a module-level or class-level assignment entity.
     *
     * Handles both plain `assignment` and `augmented_assignment` (+=, -=, etc.)
     * nodes. The entity type depends on scope:
     *   - At module scope → type = "global_variables"
     *   - At class scope   → type = "class_attribute"
     *
     * The key is derived from the `left` field of the assignment node, which
     * tree-sitter parses as the assignment target. This captures simple names,
     * attribute accesses (obj.attr), subscripts (arr[0]), and pattern
     * assignments (a, b = ...) — the full left-hand-side text becomes the key.
     *
     * ## Important: scope gate
     *
     * This extractor is only called when _named_scope is non-empty AND the
     * current scope is "module" or "class_definition". This prevents local
     * variables inside functions from being extracted as global entities.
     *
     * @return false (assignments do not introduce named scopes).
     */
    bool _extract_global_variables(TSNode node) noexcept;

    /**
     * @brief Extract a lambda expression entity.
     *
     * Lambda expressions are extracted as independent entities (Plan A:
     * standalone entity attached to the enclosing scope's entity, not as a
     * sub-entity of an assignment).
     *
     * ## Key derivation strategy
     *
     * Two-tier approach for generating a meaningful key:
     *
     *   1. Assignment context: if the lambda's parent node is an `assignment`
     *      and its `left` field is a simple `identifier`, use that identifier
     *      (lowercased) as the key. Example:
     *        square = lambda x: x * x     → key = "square"
     *
     *   2. Fallback: use "lambda_" + line number (1-indexed). This covers
     *      lambdas that appear in call arguments, list literals, or other
     *      non-assignment contexts. Example:
     *        map(lambda x: x * 2, items)  → key = "lambda_5"
     *
     * ## Scope behavior
     *
     * Lambdas do NOT introduce a named scope — they return false. The
     * qualified_name in the entity is a snapshot of the current _named_scope
     * with a synthetic ("lambda", key) entry appended. Since lambda does not
     * push onto _named_scope, nested identifiers inside the lambda body are
     * attributed to the enclosing function/class/module, not the lambda itself.
     * This reflects Python's semantics: lambdas are expressions, not scope
     * boundaries.
     *
     * @return false (lambdas do not introduce named scopes).
     */
    bool _extract_lambda(TSNode node) noexcept;

    /**
     * @brief Record an identifier occurrence in the line map.
     *
     * Every `identifier` node encountered during traversal is lowercased and
     * recorded in _identifier_line_map, mapping the name to the set of line
     * numbers where it appears. This is the simplest extractor: it creates no
     * entity, just updates the map.
     *
     * The line map powers locate_identifier(), which supports use cases like
     * "find all references to variable X" or "jump to definition of Y".
     *
     * @return false (identifiers do not introduce named scopes).
     */
    bool _extract_general_identifier(TSNode node) noexcept;
};

}
