#include "indextools/utils.hpp"

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <set>
#include <sstream>
#include <system_error>

namespace indextools {

// =============================================================================
// Character-set helpers
// =============================================================================

namespace {

struct Charset {
    std::string h_single;   // single horizontal line
    std::string h_double;   // double horizontal line (for top-level separators)
    std::string v_bar;      // vertical bar
    int h_width;            // total width for horizontal rules
};

Charset make_charset(bool use_ascii, int width) {
    if (use_ascii) {
        return {
            std::string(width, '-'),
            std::string(width, '='),
            "|",
            width
        };
    } else {
        // Unicode box-drawing characters (each is 3 UTF-8 bytes, but visually 1 column)
        // We build strings with the *visual* width in mind.
        // ═ (U+2550), ─ (U+2500), │ (U+2502)
        return {
            std::string(width, '\0'),  // placeholder — built per-use
            std::string(width, '\0'),  // placeholder
            "\xe2\x94\x82",            // │
            width
        };
    }
}

/**
 * @brief Build a horizontal rule string of a given visual width.
 *
 * For ASCII mode, fills with `ch`.
 * For Unicode mode, fills with the UTF-8 encoding of the box-drawing character.
 */
std::string h_rule(char ascii_ch, const std::string& unicode_utf8,
                   int width, bool use_ascii) {
    if (use_ascii) {
        return std::string(width, ascii_ch);
    }
    // Unicode: each char is multi-byte, so we build by appending the UTF-8 sequence
    std::string result;
    result.reserve(width * unicode_utf8.size());
    for (int i = 0; i < width; ++i) {
        result.append(unicode_utf8);
    }
    return result;
}

// UTF-8 encodings of box-drawing characters used in this file:
//   ═  → U+2550  → 0xE2 0x95 0x90
//   ─  → U+2500  → 0xE2 0x94 0x80
//   │  → U+2502  → 0xE2 0x94 0x82
constexpr const char* UTF8_DOUBLE_H = "\xe2\x95\x90";   // ═
constexpr const char* UTF8_SINGLE_H = "\xe2\x94\x80";   // ─
constexpr const char* UTF8_VBAR     = "\xe2\x94\x82";   // │

} // anonymous namespace

// json_to_readable_text — convenience wrapper for backward compatibility
// =============================================================================

std::string json_to_readable_text(const nlohmann::json& json_array, bool use_ascii) {
	// Map the old ASCII flag to the closest new mode.
	// use_ascii=false → human_readable (Unicode box-drawing).
	// use_ascii=true  → ai_readable (compact ASCII, close enough for legacy callers).
	return render_response(json_array,
	                       use_ascii ? RenderMode::ai_readable : RenderMode::human_readable);
}

// =============================================================================
// render_response — schema-aware response rendering
// =============================================================================

namespace {

// ---------------------------------------------------------------------------
// Schema-type detection
// ---------------------------------------------------------------------------

enum class ResponseType {
	DisplayBlock,
	ProcessReport,
	ErrorReport,
	ExternalRef,
	Unknown
};

ResponseType detect_type(const nlohmann::json& obj) {
	if (!obj.is_object()) return ResponseType::Unknown;
	if (obj.contains("text") || obj.contains("content") || obj.contains("sub_entity")) return ResponseType::DisplayBlock;
	if (obj.contains("stdout") || obj.contains("stderr")) return ResponseType::ProcessReport;
	if (obj.contains("detail"))                          return ResponseType::ErrorReport;
	if (obj.contains("path"))                            return ResponseType::ExternalRef;
	return ResponseType::Unknown;
}

// ---------------------------------------------------------------------------
// Value-to-string (unified)
// ---------------------------------------------------------------------------

std::string val_str(const nlohmann::json& v) {
	if (v.is_string())      return v.get<std::string>();
	if (v.is_null())        return "(null)";
	if (v.is_boolean())     return v.get<bool>() ? "true" : "false";
	if (v.is_number())      return v.dump();
	return v.dump();
}

// ---------------------------------------------------------------------------
// Unified meta rendering
// ---------------------------------------------------------------------------

/// Human-readable meta: one line per key-value pair, keys right-aligned.
/// @return the rendered string and updates @p max_key_w for alignment across blocks.
void render_meta_human(std::ostringstream& out,
                       const nlohmann::json& meta,
                       const std::string& indent,
                       int& max_key_w) {
	const auto& names = meta.value("field_name", nlohmann::json::array());
	const auto& contents = meta.value("field_content", nlohmann::json::array());
	const size_t rows = std::min(names.size(), contents.size());

	// First pass: compute key width if not yet determined
	if (max_key_w == 0) {
		for (size_t i = 0; i < rows; ++i) {
			std::string key = names[i].is_string() ? names[i].get<std::string>() : std::string{};
			max_key_w = std::max(max_key_w, static_cast<int>(key.size()));
		}
	}
	if (max_key_w < 4) max_key_w = 4;

	for (size_t i = 0; i < rows; ++i) {
		std::string key = names[i].is_string() ? names[i].get<std::string>() : std::string{};
		out << indent << "  "
		    << std::setw(max_key_w) << std::right << key << " :"
		    << " " << val_str(contents[i]) << "\n";
	}
}

/// AI-readable meta: single line, key: value pairs separated by " | ".
std::string render_meta_ai(const nlohmann::json& meta) {
	const auto& names = meta.value("field_name", nlohmann::json::array());
	const auto& contents = meta.value("field_content", nlohmann::json::array());
	const size_t rows = std::min(names.size(), contents.size());

	std::ostringstream out;
	for (size_t i = 0; i < rows; ++i) {
		if (i > 0) out << " | ";
		std::string key = names[i].is_string() ? names[i].get<std::string>() : std::string{};
		out << key << ": " << val_str(contents[i]);
	}
	return out.str();
}

// ---------------------------------------------------------------------------
// Source-text body rendering (DisplayBlock "text" section)
// ---------------------------------------------------------------------------

/// Line-type → prefix character.
char line_prefix(const nlohmann::json& types, size_t i) {
	if (i >= types.size() || !types[i].is_string()) return ' ';
	const std::string& t = types[i].get_ref<const std::string&>();
	if (t == "add")    return '+';
	if (t == "delete") return '-';
	if (t == "match")  return '*';
	return ' ';
}

/// Human-readable text body: line-numbered with vbar.
void render_text_body_human(std::ostringstream& out,
                            const nlohmann::json& text,
                            const std::string& indent,
                            int rule_width) {
	const auto& content = text.value("line_content", nlohmann::json::array());
	const auto& numbers = text.value("line_number", nlohmann::json::array());
	const auto& types   = text.value("line_type",   nlohmann::json::array());

	if (content.empty()) return;

	// Section header
	out << indent << "  "
	    << h_rule('-', UTF8_SINGLE_H, 3, false)
	    << " Source "
	    << h_rule('-', UTF8_SINGLE_H, std::max(rule_width - 13, 10), false)
	    << "\n";

	// Compute line-number column width
	int num_width = 6;
	for (const auto& n : numbers) {
		if (n.is_number_unsigned() || n.is_number_integer()) {
			size_t val = n.get<size_t>();
			size_t digits = 1;
			while (val >= 10) { val /= 10; ++digits; }
			num_width = std::max(num_width, static_cast<int>(digits));
		}
	}

	const std::string vbar = UTF8_VBAR;

	for (size_t i = 0; i < content.size(); ++i) {
		char prefix = line_prefix(types, i);

		size_t line_num = 0;
		if (i < numbers.size() &&
		    (numbers[i].is_number_unsigned() || numbers[i].is_number_integer())) {
			line_num = numbers[i].get<size_t>();
		}

		std::string line_content;
		if (content[i].is_string()) {
			line_content = content[i].get<std::string>();
		}

		out << indent << "    " << prefix << " "
		    << std::setw(num_width) << std::right << line_num
		    << " " << vbar << " " << line_content << "\n";
	}
	out << "\n";
}

/// AI-readable text body: no line numbers, just prefix + content.
void render_text_body_ai(std::ostringstream& out,
                         const nlohmann::json& text,
                         const std::string& indent) {
	const auto& content = text.value("line_content", nlohmann::json::array());
	const auto& types   = text.value("line_type",   nlohmann::json::array());

	for (size_t i = 0; i < content.size(); ++i) {
		char prefix = line_prefix(types, i);
		std::string line_content;
		if (content[i].is_string()) {
			line_content = content[i].get<std::string>();
		}
		out << indent << prefix << " " << line_content << "\n";
	}
}

// ---------------------------------------------------------------------------
// Whole-content body rendering (DisplayBlock "content" section)
// ---------------------------------------------------------------------------

void render_content_body_human(std::ostringstream& out,
                               const nlohmann::json& entity,
                               const std::string& indent,
                               int rule_width) {
	if (!entity.contains("content") || !entity["content"].is_string()) return;
	const std::string& raw = entity["content"].get_ref<const std::string&>();

	out << indent << "  "
	    << h_rule('-', UTF8_SINGLE_H, 3, false)
	    << " Content "
	    << h_rule('-', UTF8_SINGLE_H, std::max(rule_width - 13, 10), false)
	    << "\n";
	out << indent << "  " << raw << "\n\n";
}

void render_content_body_ai(std::ostringstream& out,
                            const nlohmann::json& entity,
                            const std::string& indent) {
	if (!entity.contains("content") || !entity["content"].is_string()) return;
	out << indent << entity["content"].get_ref<const std::string&>() << "\n";
}

// ---------------------------------------------------------------------------
// Sub-entity rendering (shared by both modes)
// ---------------------------------------------------------------------------

void render_sub_entities(const nlohmann::json& entity,
                         RenderMode mode,
                         int depth,
                         const std::string& label,
                         const std::string& indent,
                         int rule_width,
                         std::ostringstream& out);

// ---------------------------------------------------------------------------
// Per-schema renderers
// ---------------------------------------------------------------------------

void render_display_block(const nlohmann::json& entity,
                          RenderMode mode,
                          int depth,
                          const std::string& label,
                          std::ostringstream& out) {
	const std::string indent(depth * 2, ' ');
	const int rule_width = std::max(78 - static_cast<int>(indent.size()), 20);

	if (mode == RenderMode::human_readable) {
		// Header with double rules
		auto double_rule = [&]() {
			return indent + h_rule('=', UTF8_DOUBLE_H, rule_width, false);
		};
		out << double_rule() << "\n";
		out << indent << "  " << label << "\n";
		out << double_rule() << "\n\n";

		// Meta
		if (entity.contains("meta") && entity["meta"].is_object()) {
			int max_key_w = 0;
			render_meta_human(out, entity["meta"], indent, max_key_w);
			out << "\n";
		}

		// Body: text or content
		if (entity.contains("text") && entity["text"].is_object()) {
			render_text_body_human(out, entity["text"], indent, rule_width);
		} else if (entity.contains("content")) {
			render_content_body_human(out, entity, indent, rule_width);
		}
	} else {
		// AI-readable
		out << indent << "[" << label << "]\n";

		if (entity.contains("meta") && entity["meta"].is_object()) {
			out << indent << render_meta_ai(entity["meta"]) << "\n";
		}

		if (entity.contains("text") && entity["text"].is_object()) {
			render_text_body_ai(out, entity["text"], indent + "  ");
		} else if (entity.contains("content")) {
			render_content_body_ai(out, entity, indent + "  ");
		}
	}

	// Sub-entities
	if (entity.contains("sub_entity") && entity["sub_entity"].is_array()
	    && !entity["sub_entity"].empty()) {
		render_sub_entities(entity, mode, depth, label, indent, rule_width, out);
	}
}

void render_sub_entities(const nlohmann::json& entity,
                         RenderMode mode,
                         int depth,
                         const std::string& label,
                         const std::string& indent,
                         int rule_width,
                         std::ostringstream& out) {
	const auto& children = entity["sub_entity"];

	if (mode == RenderMode::human_readable) {
		out << indent << "  "
		    << h_rule('-', UTF8_SINGLE_H, 3, false)
		    << " Sub-entities (" << children.size() << ") "
		    << h_rule('-', UTF8_SINGLE_H,
		              std::max(rule_width - static_cast<int>(19 + std::to_string(children.size()).size()), 10),
		              false)
		    << "\n\n";
	} else {
		out << indent << "[Sub: " << children.size() << "]\n";
	}

	for (size_t i = 0; i < children.size(); ++i) {
		std::string child_label = label + "." + std::to_string(i + 1);
		render_display_block(children[i], mode, depth + 1, child_label, out);
	}
}

void render_process_report(const nlohmann::json& report,
                           RenderMode mode,
                           size_t index,
                           size_t total,
                           std::ostringstream& out) {
	const std::string label = total > 1
	    ? "Process #" + std::to_string(index + 1) + " of " + std::to_string(total)
	    : "Process #" + std::to_string(index + 1);

	if (mode == RenderMode::human_readable) {
		constexpr int kRuleWidth = 78;
		auto double_rule = [&]() {
			return h_rule('=', UTF8_DOUBLE_H, kRuleWidth, false);
		};
		out << double_rule() << "\n";
		out << "  " << label << "\n";
		out << double_rule() << "\n\n";

		if (report.contains("meta") && report["meta"].is_object()) {
			int max_key_w = 0;
			render_meta_human(out, report["meta"], "", max_key_w);
			out << "\n";
		}

		// stdout
		out << "  "
		    << h_rule('-', UTF8_SINGLE_H, 3, false)
		    << " stdout "
		    << h_rule('-', UTF8_SINGLE_H, 63, false)
		    << "\n";
		if (report.contains("stdout") && report["stdout"].is_string()) {
			out << report["stdout"].get_ref<const std::string&>();
		} else {
			out << "(empty)\n";
		}
		out << "\n";

		// stderr
		out << "  "
		    << h_rule('-', UTF8_SINGLE_H, 3, false)
		    << " stderr "
		    << h_rule('-', UTF8_SINGLE_H, 63, false)
		    << "\n";
		if (report.contains("stderr") && report["stderr"].is_string()) {
			out << report["stderr"].get_ref<const std::string&>();
		} else {
			out << "(empty)\n";
		}
		out << "\n";
	} else {
		out << "[" << label << "]\n";
		if (report.contains("meta") && report["meta"].is_object()) {
			out << render_meta_ai(report["meta"]) << "\n";
		}
		// stdout
		out << "stdout: ";
		if (report.contains("stdout") && report["stdout"].is_string()) {
			out << report["stdout"].get_ref<const std::string&>();
		} else {
			out << "(empty)";
		}
		out << "\n";
		// stderr
		out << "stderr: ";
		if (report.contains("stderr") && report["stderr"].is_string()) {
			out << report["stderr"].get_ref<const std::string&>();
		} else {
			out << "(empty)";
		}
		out << "\n";
	}
}

void render_error_report(const nlohmann::json& error,
                         RenderMode mode,
                         std::ostringstream& out) {
	if (mode == RenderMode::human_readable) {
		constexpr int kRuleWidth = 78;
		auto double_rule = [&]() {
			return h_rule('=', UTF8_DOUBLE_H, kRuleWidth, false);
		};
		out << double_rule() << "\n";
		out << "  Error\n";
		out << double_rule() << "\n\n";

		if (error.contains("meta") && error["meta"].is_object()) {
			int max_key_w = 0;
			render_meta_human(out, error["meta"], "", max_key_w);
			out << "\n";
		}

		if (error.contains("message") && error["message"].is_string()) {
			out << "  " << error["message"].get_ref<const std::string&>() << "\n\n";
		}
	} else {
		out << "[Error]\n";
		if (error.contains("meta") && error["meta"].is_object()) {
			out << render_meta_ai(error["meta"]) << "\n";
		}
		if (error.contains("message") && error["message"].is_string()) {
			out << "Message: " << error["message"].get_ref<const std::string&>() << "\n";
		}
	}
}

void render_external_ref(const nlohmann::json& ref,
                         RenderMode mode,
                         std::ostringstream& out) {
	if (mode == RenderMode::human_readable) {
		constexpr int kRuleWidth = 78;
		auto double_rule = [&]() {
			return h_rule('=', UTF8_DOUBLE_H, kRuleWidth, false);
		};
		out << double_rule() << "\n";
		out << "  External Content\n";
		out << double_rule() << "\n\n";

		if (ref.contains("meta") && ref["meta"].is_object()) {
			int max_key_w = 0;
			render_meta_human(out, ref["meta"], "", max_key_w);
			out << "\n";
		}

		if (ref.contains("message") && ref["message"].is_string()) {
			out << "  " << ref["message"].get_ref<const std::string&>() << "\n\n";
		}
	} else {
		out << "[External Content]\n";
		if (ref.contains("meta") && ref["meta"].is_object()) {
			out << render_meta_ai(ref["meta"]) << "\n";
		}
		if (ref.contains("message") && ref["message"].is_string()) {
			out << "Message: " << ref["message"].get_ref<const std::string&>() << "\n";
		}
	}
}

} // anonymous namespace

// =============================================================================
// render_response — public entry point
// =============================================================================

std::string render_response(const nlohmann::json& response, RenderMode mode) {
	// -------------------------------------------------------------------------
	// Guard
	// -------------------------------------------------------------------------
	if (response.is_null()) {
		return "(null response)\n";
	}
	if (!response.is_array() && !response.is_object()) {
		return "(invalid response)\n";
	}

	// Normalise to array
	std::vector<std::reference_wrapper<const nlohmann::json>> items;
	if (response.is_array()) {
		if (response.empty()) return "(empty)\n";
		for (const auto& item : response) {
			items.emplace_back(item);
		}
	} else {
		items.emplace_back(response);
	}

	std::ostringstream out;

	// -------------------------------------------------------------------------
	// Detect the schema type from the first item (assume homogeneous array)
	// -------------------------------------------------------------------------
	const ResponseType rtype = detect_type(items[0].get());

	switch (rtype) {
	case ResponseType::DisplayBlock: {
		const size_t total = items.size();
		for (size_t i = 0; i < total; ++i) {
			std::string label;
			if (total > 1) {
				label = "Match #" + std::to_string(i + 1) + " of " + std::to_string(total);
			} else {
				label = "Match";
			}
			render_display_block(items[i].get(), mode, 0, label, out);
		}
		break;
	}
	case ResponseType::ProcessReport: {
		const size_t total = items.size();
		for (size_t i = 0; i < total; ++i) {
			render_process_report(items[i].get(), mode, i, total, out);
		}
		break;
	}
	case ResponseType::ErrorReport: {
		for (size_t i = 0; i < items.size(); ++i) {
			render_error_report(items[i].get(), mode, out);
		}
		break;
	}
	case ResponseType::ExternalRef: {
		for (size_t i = 0; i < items.size(); ++i) {
			render_external_ref(items[i].get(), mode, out);
		}
		break;
	}
	case ResponseType::Unknown:
	default: {
		// Fallback: raw JSON dump
		for (size_t i = 0; i < items.size(); ++i) {
			out << items[i].get().dump(2) << "\n";
		}
		break;
	}
	}

	return out.str();
}

// =============================================================================
// Glob Pattern Matching — glob_match
// =============================================================================

bool glob_match(std::string_view pattern, std::string_view name) {
    const char* p = pattern.data();
    const char* p_end = p + pattern.size();
    const char* n = name.data();
    const char* n_end = n + name.size();

    const char* star_p = nullptr;   // position of last '*' in pattern
    const char* star_n = nullptr;   // position in name when '*' was seen

    while (n < n_end) {
        if (p < p_end && *p == '*') {
            // Record star position for backtracking; skip consecutive '*'
            star_p = p++;
            while (p < p_end && *p == '*') ++p;
            star_n = n;
        } else if (p < p_end && *p == '?') {
            ++p;
            ++n;
        } else if (p < p_end && *p == '[') {
            // ---- Character class --------------------------------------------
            const char* class_start = p;  // save for fallback (literal '[')
            const char* cp = p + 1;       // char after '['
            if (cp >= p_end) {
                // Unmatched '[' at end of pattern — treat as literal
                if (*class_start == *n) {
                    ++n;
                    p = class_start + 1;
                } else if (star_p) {
                    p = star_p + 1;
                    n = ++star_n;
                } else {
                    return false;
                }
                continue;
            }

            bool negate = false;
            if (*cp == '!' || *cp == '^') { negate = true; ++cp; }

            bool matched = false;
            char range_start = 0;
            bool in_range = false;

            // Handle ']' as the first character inside the class (literal ']')
            if (cp < p_end && *cp == ']') {
                if (*n == ']') matched = true;
                ++cp;
            }

            while (cp < p_end && *cp != ']') {
                if (in_range) {
                    // completing a range: range_start .. *cp
                    if (static_cast<unsigned char>(*n) >= static_cast<unsigned char>(range_start)
                        && static_cast<unsigned char>(*n) <= static_cast<unsigned char>(*cp)) {
                        matched = true;
                    }
                    in_range = false;
                    ++cp;
                } else if (*cp == '-' && cp > class_start + 1 + (negate ? 1 : 0)
                           && (cp + 1) < p_end && *(cp + 1) != ']') {
                    // '-' as literal: at start of class, or at end before ']'
                    // Here it's in the middle with a char on each side → range
                    range_start = *(cp - 1);
                    ++cp;
                    in_range = true;
                } else {
                    if (*cp == *n) matched = true;
                    ++cp;
                }
            }

            // Handle trailing '-' after a range start (e.g. "[a-]" matches 'a' or '-')
            if (in_range) {
                if (*n == range_start || *n == '-') matched = true;
            }

            // Check for closing ']'
            if (cp < p_end && *cp == ']') {
                // Properly closed class
                ++cp;  // consume ']'
                if (negate ? matched : !matched) {
                    // Mismatch — backtrack to star if available
                    if (star_p) { p = star_p + 1; n = ++star_n; }
                    else return false;
                } else {
                    ++n;
                    p = cp;
                }
            } else {
                // Unmatched '[' (no closing ']' before end of pattern).
                // Treat '[' as a literal character.
                if (*class_start == *n) {
                    ++n;
                    p = class_start + 1;
                } else if (star_p) {
                    p = star_p + 1;
                    n = ++star_n;
                } else {
                    return false;
                }
            }
        } else if (p < p_end && *p == *n) {
            ++p;
            ++n;
        } else if (star_p) {
            // Mismatch — backtrack: star consumes one more char from name.
            // star_p points AT the first '*' (not past it), so star_p + 1
            // may land on a consecutive '*'; skip those explicitly.
            p = star_p + 1;
            while (p < p_end && *p == '*') ++p;
            n = ++star_n;
        } else {
            return false;
        }
    }

    // Consume any remaining '*' in the pattern
    while (p < p_end && *p == '*') ++p;

    return p == p_end;
}

// =============================================================================
// Glob-based File Traversal — glob_find
// =============================================================================

namespace {

/**
 * @brief Split a glob pattern into path segments by '/'.
 *
 * Consecutive '/' are collapsed. A trailing '/' is ignored.
 * Example: "src/**\/*.cpp" → {"src", "**", "*.cpp"}
 */
std::vector<std::string> split_glob_segments(const std::string& pattern) {
    std::vector<std::string> segments;
    if (pattern.empty()) return segments;

    std::string current;
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '/') {
            if (!current.empty()) {
                if (current == "..") {
                    // Collapse ".." by popping the preceding segment, if any.
                    // If there is no preceding segment (or the preceding
                    // segment is also ".."), keep the ".." — it can't be
                    // resolved without knowing the root_path, but it also
                    // can't match any directory entry (directory_iterator
                    // skips ".."), so the pattern will return empty.
                    if (!segments.empty() && segments.back() != "..") {
                        segments.pop_back();
                    } else {
                        segments.push_back(std::move(current));
                    }
                } else if (current != ".") {
                    // Filter out "." (self-referential, no effect)
                    segments.push_back(std::move(current));
                }
                current.clear();
            }
            // Skip consecutive '/'
        } else {
            current += pattern[i];
        }
    }
    if (!current.empty()) {
        if (current == "..") {
            if (!segments.empty() && segments.back() != "..") {
                segments.pop_back();
            } else {
                segments.push_back(std::move(current));
            }
        } else if (current != ".") {
            segments.push_back(std::move(current));
        }
    }
    return segments;
}

/**
 * @brief Recursive implementation of glob_find.
 *
 * @param current_path    The filesystem path being visited.
 * @param segments        All glob pattern segments.
 * @param seg_idx         Index of the current pattern segment to match.
 * @param results         Output vector for matched file paths.
 * @param visited_dirs    Set of canonical paths to detect symlink cycles.
 */
void glob_find_recursive(const std::filesystem::path& current_path,
                         const std::vector<std::string>& segments,
                         size_t seg_idx,
                         std::vector<std::filesystem::path>& results,
                         std::set<std::string>& visited_dirs) {
    namespace fs = std::filesystem;

    if (seg_idx >= segments.size()) {
        // All pattern segments consumed — if this is a regular file, record it
        std::error_code ec;
        if (fs::is_regular_file(current_path, ec) && !ec) {
            results.push_back(fs::absolute(current_path));
        }
        return;
    }

    const std::string& current_seg = segments[seg_idx];
    const bool is_last = (seg_idx + 1 == segments.size());

    if (current_seg == "**") {
        // ---- ** segment: matches zero or more directory levels --------------

        // (a) ** matches zero directories: skip this segment, try matching
        //     remaining segments from current_path
        glob_find_recursive(current_path, segments, seg_idx + 1,
                           results, visited_dirs);

        // (b) ** matches 1+ directory levels: iterate entries
        std::error_code ec;
        if (!fs::is_directory(current_path, ec) || ec) return;

        // Track visited canonical paths to detect symlink cycles
        std::error_code canon_ec;
        std::string canon = fs::canonical(current_path, canon_ec).string();
        if (canon_ec) return;  // can't resolve — skip

        if (!visited_dirs.insert(canon).second) {
            return;  // already visited — cycle detected
        }

        std::error_code dir_ec;
        fs::directory_iterator it;
        try {
            it = fs::directory_iterator(current_path, dir_ec);
        } catch (const fs::filesystem_error&) {
            return;
        }
        if (dir_ec) return;

        for (const auto& entry : it) {
            const auto& entry_name = entry.path().filename().string();
            if (entry_name == "." || entry_name == "..") continue;

            std::error_code entry_ec;

            if (entry.is_directory(entry_ec) && !entry_ec) {
                // Directory: ** consumes this level and stays active
                // (the zero-dir path (a) in the recursive call will try
                //  matching the remaining segments)
                glob_find_recursive(entry.path(), segments, seg_idx,
                                   results, visited_dirs);
            }

            // When ** is the last segment, any regular file at any depth matches.
            // Directories are handled above (they recurse); files need to be
            // collected here because (a) only checks current_path itself, not
            // the individual entries.
            if (is_last) {
                if (entry.is_regular_file(entry_ec) && !entry_ec) {
                    results.push_back(fs::absolute(entry.path()));
                }
            }
        }
    } else {
        // ---- Regular segment: match against directory/file names ------------

        std::error_code ec;
        if (!fs::is_directory(current_path, ec) || ec) return;

        std::error_code dir_ec;
        fs::directory_iterator it;
        try {
            it = fs::directory_iterator(current_path, dir_ec);
        } catch (const fs::filesystem_error&) {
            return;
        }
        if (dir_ec) return;

        for (const auto& entry : it) {
            const auto& entry_name = entry.path().filename().string();
            if (entry_name == "." || entry_name == "..") continue;

            // Pruning: skip entries whose name doesn't match the current segment
            if (!glob_match(current_seg, entry_name)) continue;

            std::error_code entry_ec;
            if (is_last) {
                // Last segment — must be a regular file
                if (entry.is_regular_file(entry_ec) && !entry_ec) {
                    results.push_back(fs::absolute(entry.path()));
                }
            } else {
                // Intermediate segment — must be a directory to recurse into
                if (entry.is_directory(entry_ec) && !entry_ec) {
                    glob_find_recursive(entry.path(), segments, seg_idx + 1,
                                       results, visited_dirs);
                }
            }
        }
    }
}

} // anonymous namespace

std::vector<std::filesystem::path> glob_find(const std::filesystem::path& root_path,
                                             const std::string& glob_pattern) {
    namespace fs = std::filesystem;

    // Empty pattern → empty result
    if (glob_pattern.empty()) return {};

    // Parse the glob pattern into segments
    std::vector<std::string> segments = split_glob_segments(glob_pattern);
    if (segments.empty()) return {};

    // Validate root_path
    std::error_code ec;
    if (!fs::is_directory(root_path, ec) || ec) return {};

    std::vector<fs::path> results;
    std::set<std::string> visited_dirs;

    // Start traversal
    glob_find_recursive(root_path, segments, 0, results, visited_dirs);

    // Sort for deterministic output. std::filesystem::path has a total ordering
    // (operator<) that compares the native representation lexicographically.
    std::sort(results.begin(), results.end());

    return results;
}

} // namespace indextools
