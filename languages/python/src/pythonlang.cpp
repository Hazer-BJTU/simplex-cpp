/**
 * @file pythonlang.cpp
 * @brief Implementation of PythonLanguage — tree-sitter-based Python entity extraction.
 *
 * This file implements the entity extraction logic for Python source code.
 * The central algorithm is an iterative depth-first traversal of the
 * tree-sitter AST (see _recursive_extract_entity), which dispatches each
 * node to a type-specific extractor method.
 *
 * ## Entity lifecycle during traversal
 *
 * 1. analyze() creates a tree-sitter parser, parses the source, and kicks off
 *    the recursive traversal from the root node.
 * 2. _recursive_extract_entity walks the AST in enter-then-exit order:
 *    - On enter (phase 0): dispatch node to the appropriate _extract_* method.
 *      Each extractor creates an EntityTag, appends it to _result, and
 *      optionally pushes onto the _named_scope / _named_scope_entity_idx stacks.
 *    - On exit (phase 1): if the node introduced a scope, pop the stacks.
 * 3. After traversal, _result contains a flat list of entities with parent/child
 *    index relationships; _identifier_line_map maps identifiers to line numbers.
 *
 * ## Position coordinate systems
 *
 * Tree-sitter uses two coordinate systems:
 *   - Point:   (row, column) — 0-based, where row is the line number.
 *   - Byte:    absolute byte offset into the source string.
 *
 * EntityTag stores both: line_start/line_end for human-friendly display and
 * byte_start/byte_end for precise source slicing. The base class
 * get_dedented_lines() operates on line indices and returns content lines
 * with common leading whitespace removed.
 */

#include "python/pythonlang.hpp"

#include <algorithm>
#include <cstring>   // strcmp, strlen for tree-sitter C API string comparisons
#include <functional>

namespace indextools {

// ============================================================================
// EntityTag: constructor and member functions
// ============================================================================

PythonLanguage::EntityTag::EntityTag(
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
): type(type), key(key), qualified_name(std::move(qualified_name)),
   content(std::move(content)), line_start(line_start), line_end(line_end),
   byte_start(byte_start), byte_end(byte_end), absolute_path(absolute_path),
   parent_entity_idx(parent_entity_idx), children_entity_idx() {}

std::string_view PythonLanguage::EntityTag::get_key() const noexcept {
    return key;
}

std::string PythonLanguage::EntityTag::get_qualified_name_str() const noexcept {
    std::string name_str;
    for (size_t i = 0; i < qualified_name.size(); ++i) {
        const auto& [type, name] = qualified_name[i];
        if (i > 0) {
            // Separate scope levels with dots for readability.
            // Example: "example.py(module).MyClass(class_definition)"
            name_str.append(" > ");
        }
        // Format: name(type), e.g. "foo(function_definition)"
        name_str.append(name).append("(").append(type).append(")");
    }
    return name_str;
}

/**
 * @brief Convert current EntityTag object to nlohmann json dictionary format
 * @return nlohmann::json Structured json data containing meta info and text content
 * @noexcept This function guarantees no exceptions will be thrown
 *
 * Data structure description:
 * The returned json dict has two top-level keys: "meta" and "text",
 * both store general displayable information in column-based layout.
 *
 * 1. "meta" section: stores static tag metadata as key-value pairs
 *    - field_name: Fixed header column names: ["File", "Type", "Target", "Lines"]
 *    - field_content: Corresponding value array matching field_name order:
 *        absolute file path, entity type, qualified entity name, line range string
 *
 * 2. "text" section: Stores line-by-line source content with line metadata
 *    - line_content: Raw source text content of this entity tag
 *    - line_number: Array of sequential line numbers starting from line_start
 *    - line_type: Array marking category of each corresponding line
 *      Supported line_type values:
 *      "base"  - normal base content line
 *      "add"    - newly added line
 *      "delete" - deleted line
 *      "match"  - matched reference line
 */
nlohmann::json PythonLanguage::EntityTag::to_dict() const noexcept {
    // Build via the shared schema builders (see schema.hpp) so the entity's
    // display-block shape stays identical to every other producer.
    nlohmann::json meta = schema::MetaBuilder()
        .field("File", absolute_path.string())
        .field("Type", type)
        .field("Target", get_qualified_name_str())
        .field("Lines", schema::range_str(line_start, line_end))
        .build();

    // One text line per content line; all lines default to "base" type.
    schema::TextBody body;
    for (size_t i = 0; i < content.size(); i++) {
        body.line(content[i], line_start + i, schema::line_type::base);
    }

    return schema::text_block(std::move(meta), body.build());
}

std::unique_ptr<LangAnalyze::EntityTag> PythonLanguage::EntityTag::clone() const noexcept {
    // Create a shallow copy of all scalar and string members,
    // a deep copy of the content vector, and a shallow copy of
    // children_entity_idx (which is just a list of indices).
    auto cloned_entity = std::make_unique<PythonLanguage::EntityTag>(
        type, key, qualified_name,
        content, line_start, line_end,
        byte_start, byte_end, absolute_path,
        parent_entity_idx
    );
    cloned_entity->children_entity_idx = children_entity_idx;
    return cloned_entity;
}

// ============================================================================
// analyze() — top-level entry point
// ============================================================================

PythonLanguage* PythonLanguage::analyze() noexcept {
    // Create a fresh parser for each analysis call. While this has a small
    // allocation cost, it ensures no state leaks between calls and avoids
    // lifetime management complexity. The parser is tiny compared to the
    // tree and source data.
    TSParser* parser = ts_parser_new();
    if (parser == nullptr) {
        // Allocation failure — ts_parser_new() calls calloc() internally
        // and may return null under memory pressure.
        _result.clear();
        _identifier_line_map.clear();
        _named_scope.clear();
        _named_scope_entity_idx.clear();
        return this;
    }

    // ts_parser_set_language returns false on ABI incompatibility between
    // the tree-sitter core library and the Python grammar. Treat this as
    // a hard failure to avoid silent misanalysis.
    if (!ts_parser_set_language(parser, tree_sitter_python())) {
        ts_parser_delete(parser);
        _result.clear();
        _identifier_line_map.clear();
        _named_scope.clear();
        _named_scope_entity_idx.clear();
        return this;
    }

    // Parse the source. We use _lines.source().size() (not strlen) because:
    //   (1) The source may contain embedded null bytes, and strlen would
    //       stop at the first null.
    //   (2) string_view::size() is O(1) vs strlen's O(n).
    //
    // The cast to uint32_t is required by tree-sitter's API. We assert that
    // the source fits in 32 bits; a static_assert is not possible (size is
    // runtime), but this guard catches the edge case explicitly rather than
    // silently truncating.
    auto src = _lines.source();
    TSTree* tree = ts_parser_parse_string(parser, NULL, src.data(), static_cast<uint32_t>(src.size()));

    // If parsing fails (e.g. severely malformed syntax that tree-sitter
    // cannot recover from), clear all state and return. The caller can
    // detect failure by checking if result() is empty.
    if (tree == nullptr) {
        ts_parser_delete(parser);
        _result.clear();
        _identifier_line_map.clear();
        _named_scope.clear();
        _named_scope_entity_idx.clear();
        return this;
    }
    TSNode root_node = ts_tree_root_node(tree);

    // Clear previous analysis results before populating new ones.
    // This is done after successful parse so that a failed parse
    // leaves previous results intact (though currently both paths clear).
    _result.clear();
    _identifier_line_map.clear();
    _named_scope.clear();
    _named_scope_entity_idx.clear();

    // Kick off the recursive extraction from the AST root.
    // The root node of a Python file is always a "module" node.
    _recursive_extract_entity(root_node);

    // Clean up tree-sitter resources. The tree must be deleted before
    // the parser because it may reference parser-internal data.
    ts_tree_delete(tree);
    ts_parser_delete(parser);
    return this;
}

// ============================================================================
// reset() — clear all analysis state for reuse
// ============================================================================

PythonLanguage* PythonLanguage::reset() noexcept {
    // Call the base class reset first to clear source, lines, and path.
    LangAnalyze::reset();
    _result.clear();
    _identifier_line_map.clear();
    _named_scope.clear();
    _named_scope_entity_idx.clear();
    return this;
}

// ============================================================================
// result() — access the entity list
// ============================================================================

const PythonLanguage::EntityList& PythonLanguage::result() const noexcept {
    return _result;
}

// ============================================================================
// _recursive_extract_entity — the core traversal engine
// ============================================================================

void PythonLanguage::_recursive_extract_entity(TSNode node) noexcept {
    if (ts_node_is_null(node)) {
        return;
    }

    /**
     * Stack frame for the iterative traversal.
     *
     * Each frame corresponds to one AST node. The `phase` field implements
     * a manual enter/exit protocol:
     *
     *   phase 0:  The node has not been processed yet. When popped, we
     *             dispatch it to the appropriate extractor, then push a
     *             phase-1 frame (for later cleanup) followed by phase-0
     *             frames for all children (in reverse order so they are
     *             visited left-to-right since we use a LIFO stack).
     *
     *   phase 1:  The node and all its descendants have been processed.
     *             If this node's extractor returned true (is_named_scope),
     *             we pop the _named_scope and _named_scope_entity_idx stacks
     *             to restore the enclosing scope.
     *
     * `is_named_scope` is meaningful only for phase-1 frames; it stores
     * the return value of the extractor called during phase 0.
     */
    struct Frame {
        TSNode node;
        int phase;
        bool is_named_scope;
    };

    std::vector<Frame> stack;
    stack.push_back({node, 0, false});

    while (!stack.empty()) {
        Frame frame = stack.back();
        stack.pop_back();

        if (ts_node_is_null(frame.node)) {
            continue;
        }

        // --- Phase 1: exit ---
        // All children have been processed. Clean up scope if needed.
        if (frame.phase == 1) {
            if (frame.is_named_scope && !_named_scope.empty()) {
                _named_scope.pop_back();
                _named_scope_entity_idx.pop_back();
            }
            continue;
        }

        // --- Phase 0: enter ---
        // Process this node and schedule children.

        bool is_named_scope = false;
        size_t children_cnt = ts_node_child_count(frame.node);
        std::string node_type(ts_node_type(frame.node));

        // Dispatch: select the appropriate extractor based on the tree-sitter
        // node type string. These are the tree-sitter Python grammar node
        // types; see https://github.com/tree-sitter/tree-sitter-python for
        // the full grammar definition.
        //
        // Note: the conditions are mutually exclusive (a node has exactly one
        // type), so the order matters only for performance. We check the most
        // common types first.
        //
        // The `else if` chain (not standalone `if`s) is intentional: it
        // prevents accidental double-dispatch if conditions are reordered
        // and makes the mutual-exclusion property explicit.

        if (node_type == "module") {
            is_named_scope = _extract_module(frame.node);
        } else if (node_type == "function_definition" || node_type == "class_definition") {
            is_named_scope = _extract_class_function_definition(frame.node);
        } else if (node_type == "import_statement" || node_type == "import_from_statement") {
            is_named_scope = _extract_import_statement(frame.node);
        } else if ((node_type == "assignment" || node_type == "augmented_assignment") && !_named_scope.empty()) {
            // Only extract assignments at module or class scope.
            // Local variables inside functions are deliberately skipped —
            // they are not meaningful for top-level code navigation.
            //
            // The _named_scope.empty() check is a safety guard: if we
            // somehow encounter an assignment before the module node has
            // been processed, we skip it rather than accessing .back() on
            // an empty vector (which would be UB).
            const auto& scope_type = std::get<0>(_named_scope.back());
            if (scope_type == "module" || scope_type == "class_definition") {
                is_named_scope = _extract_global_variables(frame.node);
            }
        } else if (node_type == "lambda") {
            is_named_scope = _extract_lambda(frame.node);
        } else if (node_type == "identifier") {
            is_named_scope = _extract_general_identifier(frame.node);
        }

        // Push the exit frame first (it will be processed after all children),
        // then push all children in reverse order so they are visited
        // left-to-right (since we pop from the back).
        stack.push_back({frame.node, 1, is_named_scope});
        for (size_t i = children_cnt; i > 0; --i) {
            stack.push_back({ts_node_child(frame.node, i - 1), 0, false});
        }
    }

    return;
}

// ============================================================================
// _get_full_node_content — extract source text for a tree-sitter node
// ============================================================================

std::string PythonLanguage::_get_full_node_content(TSNode node) const noexcept {
    if (ts_node_is_null(node)) {
        return {};
    }

    // Tree-sitter provides byte offsets relative to the parse input string.
    // Since we passed the source data to ts_parser_parse_string, these
    // offsets are directly usable as indices into the source buffer.
    uint32_t byte_start = ts_node_start_byte(node);
    uint32_t byte_end = ts_node_end_byte(node);
    auto src = _lines.source();
    return std::string(src.substr(byte_start, byte_end - byte_start));
}

// ============================================================================
// locate_identifier — look up identifier occurrences and return JSON with context
// ============================================================================

nlohmann::json PythonLanguage::locate_identifier(std::string_view identifier) const noexcept {
    size_t num_lines = _lines.size();
    if (num_lines == 0) {
        return nlohmann::json::array();
    }

    // ==========================================================================
    // 1.  Lowercase the query key to match the lowercased keys stored in
    //     _identifier_line_map by _extract_general_identifier.
    // ==========================================================================
    std::string lowered(identifier);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    auto it = _identifier_line_map.find(lowered);

    // Not found — return an empty JSON array so callers can iterate without
    // special-casing the "not found" path.
    if (it == _identifier_line_map.end()) {
        return nlohmann::json::array();
    }

    // ==========================================================================
    // 2.  Delegate to the base-class builder, which expands the matched lines
    //     with context and emits the standard "meta" + "text" block. This is
    //     the same path FallbackLanguage::locate_identifier() takes; the "File"
    //     / "Matches" / "Total Length" fields and match/base tagging are all
    //     handled there, keeping the output byte-identical across analyzers.
    // ==========================================================================
    return _build_identifier_lookup_json("Identifier", lowered, it->second);
}

nlohmann::json PythonLanguage::locate_entity(std::string_view entity_key) const noexcept {
    // ==========================================================================
    // 1.  Lowercase the query key.
    //     Entity keys are stored in their original casing; we lowercase both
    //     sides of the comparison to achieve case-insensitive matching.
    // ==========================================================================
    std::string lowered_key(entity_key);
    std::transform(lowered_key.begin(), lowered_key.end(), lowered_key.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // ==========================================================================
    // 2.  Scan _result for all entities whose key matches the lowered query.
    //     Each match is serialized together with its descendant tree.
    // ==========================================================================
    nlohmann::json result = nlohmann::json::array();
    for (size_t i = 0; i < _result.size(); ++i) {
        // Lowercase the entity key for case-insensitive comparison
        std::string entity_key_lower(_result[i]->get_key());
        if (!entity_key_lower.empty() && entity_key_lower == lowered_key) {
            result.push_back(_serialize_with_children(i));
        }
    }

    return result;
}

// ============================================================================
// get_full_structure — serialize the entire entity tree from the module root
// ============================================================================

nlohmann::json PythonLanguage::get_full_structure() const noexcept {
    nlohmann::json result = nlohmann::json::array();
    for (size_t i = 0; i < _result.size(); ++i) {
        const auto* py_entity = static_cast<const PythonLanguage::EntityTag*>(_result[i].get());
        if (py_entity->type == "module") {
            result.push_back(_serialize_with_children(i));
            break;
        }
    }
    return result;
}

const PythonLanguage::LineIndex& PythonLanguage::get_identifier_line_map() const noexcept {
    return _identifier_line_map;
}

// ============================================================================
// _serialize_with_children — recursively serialize entity with its descendants
// ============================================================================

nlohmann::json PythonLanguage::_serialize_with_children(size_t idx) const noexcept {
    // Bounds check — should never fail with well-formed indices, but guard
    // against corruption to stay noexcept-safe.
    if (idx >= _result.size()) {
        return nlohmann::json::object();
    }

    // Base representation: meta + text from to_dict()
    nlohmann::json j = _result[idx]->to_dict();

    // Build the sub_entity array by recursing into each child.
    // children_entity_idx lives on the PythonLanguage::EntityTag subclass,
    // so we must static_cast to reach it.
    nlohmann::json sub_entity = nlohmann::json::array();
    const auto* py_entity = static_cast<const PythonLanguage::EntityTag*>(_result[idx].get());
    for (size_t child_idx : py_entity->children_entity_idx) {
        sub_entity.push_back(_serialize_with_children(child_idx));
    }
    j["sub_entity"] = std::move(sub_entity);
    return j;
}

// ============================================================================
// _extract_decorator — find decorator position for decorated definitions
// ============================================================================

std::optional<std::tuple<size_t, size_t>> PythonLanguage::_extract_decorator(TSNode node) noexcept {
    if (ts_node_is_null(node)) {
        return std::nullopt;
    }

    // In tree-sitter-python, a decorated function/class has the AST structure:
    //
    //   decorated_definition
    //     ├── decorator          ← e.g. "@staticmethod"
    //     ├── decorator          ← e.g. "@abstractmethod" (if multiple)
    //     └── function_definition (or class_definition)
    //
    // The function/class node IS a child of decorated_definition, not a
    // sibling. So we check the parent node type, then scan the parent's
    // children for the first decorator.
    //
    // We return the position of the FIRST decorator (not the last), because
    // we want to extend the entity's start backward to include all decorators.

    auto parent_node = ts_node_parent(node);
    if (ts_node_is_null(parent_node) || strcmp(ts_node_type(parent_node), "decorated_definition") != 0) {
        return std::nullopt;
    }

    for (size_t i = 0; i < ts_node_child_count(parent_node); ++i) {
        auto sibling_node = ts_node_child(parent_node, i);
        if (strcmp(ts_node_type(sibling_node), "decorator") == 0) {
            // Return the position of the first decorator found.
            // We use .row (not .column) because EntityTag stores line-level
            // positions, and byte_start because byte_end is adjusted
            // automatically by the caller using the original node's byte_end.
            return std::tuple<size_t, size_t>{
                ts_node_start_point(sibling_node).row,
                ts_node_start_byte(sibling_node)
            };
        }
    }

    return std::nullopt;
}

// ============================================================================
// _extract_module — extract the top-level module entity
// ============================================================================

bool PythonLanguage::_extract_module(TSNode node) noexcept {
    uint32_t line_start = ts_node_start_point(node).row;
    uint32_t line_end = ts_node_end_point(node).row;
    uint32_t byte_start = ts_node_start_byte(node);
    uint32_t byte_end = ts_node_end_byte(node);

    // Push the module onto the scope stack BEFORE creating the entity,
    // because the entity's qualified_name should include itself.
    // The module key is the filename (without directory), e.g. "example.py".
    // When load() is used without a prior open() call, _absolute_path is
    // empty — fall back to "<memory>" to indicate an in-memory source.
    std::string module_key = _absolute_path.empty()
        ? std::string{"<memory>"}
        : _absolute_path.filename().string();
    _named_scope.emplace_back("module", module_key);

    // The module entity:
    //   - type = "module"
    //   - key = filename (or "<memory>" for in-memory sources)
    //   - content = empty (the module's body is covered by child entities)
    //   - parent_entity_idx = nullopt (the module is the root)
    auto new_entity = std::make_unique<EntityTag>(
        "module",
        module_key,
        _named_scope,
        std::vector<std::string>{},
        line_start,
        line_end,
        byte_start,
        byte_end,
        _absolute_path,
        std::nullopt
    );
    _result.push_back(std::move(new_entity));
    _named_scope_entity_idx.push_back(_result.size() - 1);
    return true;
}

// ============================================================================
// _extract_class_function_definition — extract function/class/method entities
// ============================================================================

bool PythonLanguage::_extract_class_function_definition(TSNode node) noexcept {
    uint32_t line_start = ts_node_start_point(node).row;
    uint32_t line_end = ts_node_end_point(node).row;
    uint32_t byte_start = ts_node_start_byte(node);
    uint32_t byte_end = ts_node_end_byte(node);

    // Resolve the parent entity (the entity at the top of the scope stack).
    // This will be null for module-level definitions, or a class_definition
    // entity for methods defined inside a class.
    std::optional<size_t> parent_entity_idx;
    EntityTag* parent_entity = nullptr;
    if (_named_scope_entity_idx.size() > 0) {
        parent_entity_idx = _named_scope_entity_idx.back();
        // static_cast is safe here: _result only ever contains EntityTag
        // instances (created by our own extractors), so the downcast cannot
        // fail at runtime.
        parent_entity = static_cast<EntityTag*>(_result[parent_entity_idx.value()].get());
    }

    // Determine the entity type. The tree-sitter node type is either
    // "function_definition" or "class_definition". We override to
    // "class_method" when a function is defined directly inside a class body.
    std::string type(ts_node_type(node));
    if (type == "function_definition" && parent_entity && parent_entity->type == "class_definition") {
        type = "class_method";
    }

    // Extract the function/class name from the "name" field.
    // tree-sitter-python uses named fields: the "name" field of a
    // function_definition is the identifier token after `def`.
    auto function_name_node = ts_node_child_by_field_name(node, "name", strlen("name"));
    std::string function_name;
    if (!ts_node_is_null(function_name_node)) {
        function_name = _get_full_node_content(function_name_node);
    } else {
        // Fallback for malformed code where tree-sitter cannot identify
        // the name (e.g. `def 123():` which is a syntax error but may
        // still parse as a function_definition with a missing name).
        function_name = "<unknown>";
    }

    // If the function/class has decorators, adjust the start position to
    // include them. This ensures the entity span covers @decorator lines.
    auto decorator_pos = _extract_decorator(node);
    if (decorator_pos.has_value()) {
        auto [new_line_start, new_byte_start] = decorator_pos.value();
        line_start = new_line_start;
        byte_start = new_byte_start;
    }

    // Extract only the signature lines as the entity content.
    //
    // For a function/class definition, tree-sitter separates the signature
    // from the body via the "body" field. We extract lines from the entity
    // start to the line just before the body begins. This gives us the
    // signature (def line + parameter lines + return type annotation) without
    // the implementation body.
    //
    // Example:
    //   @staticmethod
    //   def foo(a: int,
    //           b: str) -> bool:    ← content ends here
    //       return True              ← body starts here (not included)
    uint32_t signature_line_start = line_start, signature_line_end = line_start;
    auto body_node = ts_node_child_by_field_name(node, "body", strlen("body"));
    if (!ts_node_is_null(body_node)) {
        signature_line_end = ts_node_start_point(body_node).row;
    }
    auto dedented_content = get_dedented_lines(signature_line_start, signature_line_end - 1);

    // Push this entity's scope BEFORE creating the entity, so the
    // qualified_name includes itself.
    _named_scope.emplace_back(type, function_name);

    // Lowercase the key for case-insensitive lookups.
    // We create a copy (not in-place modification of function_name) because
    // function_name is still needed with original casing in _named_scope.
    std::string key(function_name);
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });

    auto new_entity = std::make_unique<EntityTag>(
        type,
        key,
        _named_scope,
        std::move(dedented_content),
        line_start,
        line_end,
        byte_start,
        byte_end,
        _absolute_path,
        parent_entity_idx
    );
    _result.push_back(std::move(new_entity));
    size_t entity_idx = _result.size() - 1;
    _named_scope_entity_idx.push_back(entity_idx);

    // Wire up parent-child relationship.
    if (parent_entity) {
        parent_entity->children_entity_idx.push_back(entity_idx);
    }
    return true;
}

// ============================================================================
// _extract_import_statement — extract import entities
// ============================================================================

bool PythonLanguage::_extract_import_statement(TSNode node) noexcept {
    uint32_t line_start = ts_node_start_point(node).row;
    uint32_t line_end = ts_node_end_point(node).row;
    uint32_t byte_start = ts_node_start_byte(node);
    uint32_t byte_end = ts_node_end_byte(node);

    std::optional<size_t> parent_entity_idx;
    EntityTag* parent_entity = nullptr;
    if (_named_scope_entity_idx.size() > 0) {
        parent_entity_idx = _named_scope_entity_idx.back();
        parent_entity = static_cast<EntityTag*>(_result[parent_entity_idx.value()].get());
    }

    std::string type = "import_statement";
    auto dedented_content = get_dedented_lines(line_start, line_end);

    // Build a snapshot of the current scope. We copy _named_scope (not
    // reference it) because imports do not push onto _named_scope and we
    // want the qualified_name to include a synthetic "import_statement" entry.
    // This snapshot captures the scope chain at the moment of import.
    //
    // The key is deliberately empty — import content is included verbatim
    // in the content field for downstream AI to parse (module name, aliases,
    // relative imports, etc.). AI-based import analysis proved more robust
    // than manually walking the tree-sitter import node structure, which
    // varies significantly across import forms.
    auto named_scope = _named_scope;
    named_scope.emplace_back("import_statement", "");

    auto new_entity = std::make_unique<EntityTag>(
        type,
        std::string{},      // empty key — AI analyzes the content
        std::move(named_scope),
        std::move(dedented_content),
        line_start,
        line_end,
        byte_start,
        byte_end,
        _absolute_path,
        parent_entity_idx
    );
    _result.push_back(std::move(new_entity));
    size_t entity_idx = _result.size() - 1;

    if (parent_entity) {
        parent_entity->children_entity_idx.push_back(entity_idx);
    }
    return false;  // imports do NOT introduce a named scope
}

// ============================================================================
// _extract_global_variables — extract module-level and class-level assignments
// ============================================================================

bool PythonLanguage::_extract_global_variables(TSNode node) noexcept {
    uint32_t line_start = ts_node_start_point(node).row;
    uint32_t line_end = ts_node_end_point(node).row;
    uint32_t byte_start = ts_node_start_byte(node);
    uint32_t byte_end = ts_node_end_byte(node);

    std::optional<size_t> parent_entity_idx;
    EntityTag* parent_entity = nullptr;
    if (_named_scope_entity_idx.size() > 0) {
        parent_entity_idx = _named_scope_entity_idx.back();
        parent_entity = static_cast<EntityTag*>(_result[parent_entity_idx.value()].get());
    }

    // Determine entity type based on the enclosing scope.
    //
    //   module scope:  type = "global_variables"
    //     x = 1              → global variable
    //     x: int = 1         → global variable (tree-sitter parses as
    //                          regular assignment in current grammar)
    //
    //   class scope:   type = "class_attribute"
    //     class Foo:
    //         x = 1          → class attribute
    //
    // Local variables inside functions are excluded by the scope gate
    // in _recursive_extract_entity (scope must be "module" or
    // "class_definition").
    std::string type;
    if (!_named_scope.empty() && std::get<0>(_named_scope.back()) == "class_definition") {
        type = "class_attribute";
    } else {
        type = "global_variables";
    }
    auto dedented_content = get_dedented_lines(line_start, line_end);

    // Extract the left-hand side of the assignment as the key.
    // tree-sitter-python uses the field name "left" for the assignment target.
    // This captures:
    //   - simple names:       x = 1           → key = "x"
    //   - attribute access:   obj.attr = 1    → key = "obj.attr"
    //   - subscript:          arr[0] = 1      → key = "arr[0]"
    //   - pattern:            a, b = 1, 2     → key = "a, b"
    //
    // The full left-hand text is used as-is; no lowercasing is applied here
    // (unlike function/class names) because variable keys may include
    // non-identifier syntax like dots and brackets.
    auto lvalue_node = ts_node_child_by_field_name(node, "left", strlen("left"));
    if (ts_node_is_null(lvalue_node)) {
        return false;
    }
    std::string key = _get_full_node_content(lvalue_node);

    // Snapshot the current scope (same pattern as imports — this entity
    // does not create a new scope, so we copy).
    auto named_scope = _named_scope;
    named_scope.emplace_back("global_variables", "");

    auto new_entity = std::make_unique<EntityTag>(
        type,
        key,
        std::move(named_scope),
        std::move(dedented_content),
        line_start,
        line_end,
        byte_start,
        byte_end,
        _absolute_path,
        parent_entity_idx
    );
    _result.push_back(std::move(new_entity));
    size_t entity_idx = _result.size() - 1;

    if (parent_entity) {
        parent_entity->children_entity_idx.push_back(entity_idx);
    }
    return false;  // assignments do NOT introduce a named scope
}

// ============================================================================
// _extract_lambda — extract lambda expression entities
// ============================================================================

bool PythonLanguage::_extract_lambda(TSNode node) noexcept {
    uint32_t line_start = ts_node_start_point(node).row;
    uint32_t line_end = ts_node_end_point(node).row;
    uint32_t byte_start = ts_node_start_byte(node);
    uint32_t byte_end = ts_node_end_byte(node);

    std::optional<size_t> parent_entity_idx;
    EntityTag* parent_entity = nullptr;
    if (_named_scope_entity_idx.size() > 0) {
        parent_entity_idx = _named_scope_entity_idx.back();
        parent_entity = static_cast<EntityTag*>(_result[parent_entity_idx.value()].get());
    }

    // === Key derivation ===
    //
    // Strategy 1: If the lambda is the right-hand side of an assignment
    // whose left-hand side is a simple identifier, use that identifier.
    //   Example:  square = lambda x: x * x    → key = "square"
    //
    // We check: (a) parent is an `assignment` node, (b) its `left` field
    // is an `identifier` (not a pattern, subscript, or attribute). This
    // ensures we only use clean, meaningful names.
    std::string key;
    TSNode parent = ts_node_parent(node);
    if (!ts_node_is_null(parent) && strcmp(ts_node_type(parent), "assignment") == 0) {
        TSNode left = ts_node_child_by_field_name(parent, "left", strlen("left"));
        if (!ts_node_is_null(left) && strcmp(ts_node_type(left), "identifier") == 0) {
            key = _get_full_node_content(left);
            std::transform(key.begin(), key.end(), key.begin(),
                          [](unsigned char c) { return std::tolower(c); });
        }
    }

    // Strategy 2 (fallback): Use line-number-based key.
    //   Example:  map(lambda x: x * 2, items) → key = "lambda_5"
    //
    // Line numbers are 1-indexed for human readability (line 1 is the
    // first line of the file). This covers lambdas that appear in call
    // arguments, list/dict literals, default parameter values, and other
    // non-assignment contexts.
    if (key.empty()) {
        key = "lambda_" + std::to_string(line_start + 1);
    }

    std::string type = "lambda";
    auto dedented_content = get_dedented_lines(line_start, line_end);

    // Build a snapshot of the current scope with a synthetic "lambda" entry.
    //
    // Important design decision: lambda does NOT push onto _named_scope
    // (returns false). This means:
    //   - The lambda is a child of the enclosing function/class/module in
    //     the entity hierarchy, not a scope boundary.
    //   - Identifiers inside the lambda body are attributed to the enclosing
    //     scope, not to the lambda itself.
    //   - Nested lambdas each become independent entities under the same
    //     parent, not nested under each other.
    //
    // This reflects Python's semantics: `lambda` is an expression that
    // creates a function object, not a named scope like `def`.
    auto named_scope = _named_scope;
    named_scope.emplace_back("lambda", key);

    auto new_entity = std::make_unique<EntityTag>(
        type,
        key,
        std::move(named_scope),
        std::move(dedented_content),
        line_start,
        line_end,
        byte_start,
        byte_end,
        _absolute_path,
        parent_entity_idx
    );
    _result.push_back(std::move(new_entity));
    size_t entity_idx = _result.size() - 1;

    if (parent_entity) {
        parent_entity->children_entity_idx.push_back(entity_idx);
    }
    return false;  // lambdas do NOT introduce a named scope
}

// ============================================================================
// _extract_general_identifier — record identifier line positions
// ============================================================================

bool PythonLanguage::_extract_general_identifier(TSNode node) noexcept {
    uint32_t line_start = ts_node_start_point(node).row;

    // Lowercase the identifier for case-insensitive lookups.
    // Python is case-sensitive, but for code navigation purposes
    // (e.g. "find all occurrences of `myVar`"), case-insensitive
    // matching is more practical.
    std::string key = _get_full_node_content(node);
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });

    // Insert this line number into the set for this identifier.
    // Using unordered_set ensures duplicate line entries are collapsed
    // (e.g. multiple references to `x` on the same line produce one entry).
    auto it = _identifier_line_map.find(key);
    if (it != _identifier_line_map.end()) {
        it->second.insert(line_start);
    } else {
        _identifier_line_map[key] = { line_start };
    }
    return false;  // identifiers do NOT introduce a named scope
}

}
