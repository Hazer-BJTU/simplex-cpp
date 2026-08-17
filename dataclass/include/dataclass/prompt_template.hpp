#pragma once

//
// prompt_template.hpp — Markdown prompt assembly with cache-stable layout
// ========================================================================
//
// PromptTemplate assembles a markdown system prompt from named text
// sections. Its one job is byte stability under the prefix caches LLM
// providers keep (Anthropic, DeepSeek, ...): a cache entry survives exactly
// as long as the prompt's leading bytes stay identical, so content that
// changes must sit as late as possible and everything before it must never
// move. Sections therefore carry a stability tier, and the tier decides
// which mutations the section accepts — the rules are encoded in the API
// shape, not left to review-time discipline:
//
//   Immutable — fixed when added; no mutation path exists afterwards.
//               Identity and fixed instructions go here.
//   Growing   — append() only; existing bytes are never rewritten. Tool
//               listings and other accumulating context go here.
//   Volatile  — rewrite() / erase() allowed; expected at the tail (current
//               date, session summary).
//
// add_section() enforces non-decreasing stability rank (Immutable before
// Growing before Volatile): a layout that would invalidate earlier bytes is
// rejected with std::logic_error rather than silently busting the cache.
//
// Rendering is pure concatenation. Bodies are canonicalised exactly once, as
// they enter the template (trailing whitespace stripped from each line,
// leading/trailing blank lines dropped), and render() never touches stored
// bytes again — appending therefore leaves every earlier byte unchanged,
// which is precisely the property a prefix cache needs.
//
// Tool embedding: PromptTemplate manages text only and knows nothing about
// tools (Invocable lives in model_io.hpp). Render tools to markdown any way
// you like and append() the result, so the embedding style stays in caller
// code:
//
//     tpl.add_section("tools", "Tools", "", SectionStability::Growing);
//     for (const auto& inv : invocables)
//         tpl.append("tools", my_tool_markdown(inv));
//
// Output shape: a section renders as its heading (heading_level '#'s,
// skipped when the title is empty), a blank line, then its body; sections
// are joined by blank lines; the prompt ends with exactly one newline; a
// section with neither title nor body contributes nothing at all.
//
// RenderedPrompt::spans record each section's byte range, so a caller (the
// future model adapter) can tell how far a previous prompt's prefix
// survived a mutation.
//
// Thread safety: no internal locking. Concurrent render() calls are safe
// (all reads); mutations must be externally serialised against reads and
// each other. Iterators point into internal storage and are invalidated by
// ANY mutation — use one only until the next modifying call.
//

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace model_io {

// ---- stability tiers ---------------------------------------------------------

// Cache-layout tier of a section; higher rank = allowed to change more =
// must sit later. See the file header for what each tier accepts.
enum class SectionStability {
    Immutable, // no mutation path after creation.
    Growing,   // append() only; existing bytes never change.
    Volatile,  // rewrite()/erase() allowed; expected at the tail.
};

// ---- records -----------------------------------------------------------------

// One named piece of the prompt. Plain data: no methods, no invariants
// beyond what PromptTemplate enforced when it accepted the section.
struct PromptSection {
    std::string name, title; // name is unique within a template; "" title
                             // renders the body without a heading.
    SectionStability stability = SectionStability::Immutable;
    std::string text;        // canonicalised body, verbatim at render.
};

// A section's byte range in RenderedPrompt::markdown — separators and the
// final newline excluded. Deliberately NOT a serialized record: it is a
// render artifact, not contract data.
struct SectionSpan {
    std::string name;
    SectionStability stability = SectionStability::Immutable;
    std::size_t begin = 0, end = 0;
};

// The render() product: the full markdown plus one span per contributing
// section, in order.
struct RenderedPrompt {
    std::string markdown;
    std::vector<SectionSpan> spans;
};

// ---- the template -------------------------------------------------------------

// Ordered collection of PromptSections with stability-enforced mutation.
// Rule of zero: default-constructible, copyable, movable.
class PromptTemplate {
public:
    // Heading depth for section titles, validated (1..6) at render().
    int heading_level = 2;

    // Add a section at the end. Throws std::logic_error on a duplicate
    // name or when `stability` ranks before the current last section's.
    PromptTemplate& add_section(std::string name, std::string title,
                                std::string text,
                                SectionStability stability
                                = SectionStability::Immutable);

    // Append `text` to a Growing section (blank-line separated). Existing
    // bytes are untouched; an empty `text` is a no-op. Throws on an unknown
    // name or a non-Growing section.
    void append(std::string_view name, std::string text);

    // Replace the body of a Volatile section. Throws on an unknown name or
    // a non-Volatile section.
    void rewrite(std::string_view name, std::string text);

    // Remove a Volatile section. Throws on an unknown name or a
    // non-Volatile section.
    void erase(std::string_view name);

    // ---- read access -------------------------------------------------------

    using const_iterator = std::vector<PromptSection>::const_iterator;

    const_iterator begin() const noexcept { return sections_.begin(); }
    const_iterator end() const noexcept { return sections_.end(); }

    // Iterator to the named section, or end() when absent.
    const_iterator find(std::string_view name) const noexcept;
    bool contains(std::string_view name) const noexcept;
    std::size_t size() const noexcept { return sections_.size(); }

    // Deterministic concatenation of all sections; see the file header for
    // the exact output shape. Throws std::logic_error when heading_level
    // is outside 1..6.
    RenderedPrompt render() const;

private:
    using iterator = std::vector<PromptSection>::iterator;

    // Cache-layout rank of a tier; ordering rule: non-decreasing only.
    static int rank_(SectionStability s) noexcept {
        switch (s) {
        case SectionStability::Immutable: return 0;
        case SectionStability::Growing: return 1;
        case SectionStability::Volatile: return 2;
        }
        return 2; // unreachable, keeps the compiler quiet
    }

    static std::string name_of_(SectionStability s) noexcept;

    // Canonicalise text on entry: strip trailing whitespace per line, drop
    // leading/trailing blank lines. render() then concatenates verbatim.
    static std::string canonicalize_(std::string text);

    // Locate a section or throw; the mutation entry points share this.
    iterator locate_(std::string_view name, const char* who);

    std::vector<PromptSection> sections_;
};

// ---- out-of-line definitions ---------------------------------------------------

inline std::string PromptTemplate::name_of_(SectionStability s) noexcept {
    switch (s) {
    case SectionStability::Immutable: return "immutable";
    case SectionStability::Growing: return "growing";
    case SectionStability::Volatile: return "volatile";
    }
    return "?";
}

inline PromptTemplate& PromptTemplate::add_section(std::string name,
                                                   std::string title,
                                                   std::string text,
                                                   SectionStability stability) {
    if (contains(name))
        throw std::logic_error("PromptTemplate::add_section: duplicate "
                               "section name '" + name + "'");
    if (!sections_.empty()
        && rank_(stability) < rank_(sections_.back().stability))
        throw std::logic_error(
            "PromptTemplate::add_section: stability cannot go backwards "
            "('"
            + name + "' is " + name_of_(stability) + ", last section '"
            + sections_.back().name + "' is "
            + name_of_(sections_.back().stability) + ")");
    PromptSection s;
    s.name = std::move(name);
    s.title = std::move(title);
    s.text = canonicalize_(std::move(text));
    s.stability = stability;
    sections_.push_back(std::move(s));
    return *this;
}

inline void PromptTemplate::append(std::string_view name, std::string text) {
    auto it = locate_(name, "append");
    if (it->stability != SectionStability::Growing)
        throw std::logic_error("PromptTemplate::append: section '" +
                               std::string(name) + "' is " +
                               name_of_(it->stability) + ", not growing");
    auto chunk = canonicalize_(std::move(text));
    if (chunk.empty()) return; // appending nothing keeps bytes identical
    if (!it->text.empty()) it->text += "\n\n";
    it->text += std::move(chunk);
}

inline void PromptTemplate::rewrite(std::string_view name, std::string text) {
    auto it = locate_(name, "rewrite");
    if (it->stability != SectionStability::Volatile)
        throw std::logic_error("PromptTemplate::rewrite: section '" +
                               std::string(name) + "' is " +
                               name_of_(it->stability) + ", not volatile");
    it->text = canonicalize_(std::move(text));
}

inline void PromptTemplate::erase(std::string_view name) {
    auto it = locate_(name, "erase");
    if (it->stability != SectionStability::Volatile)
        throw std::logic_error("PromptTemplate::erase: section '" +
                               std::string(name) + "' is " +
                               name_of_(it->stability) + ", not volatile");
    sections_.erase(it);
}

inline PromptTemplate::const_iterator
PromptTemplate::find(std::string_view name) const noexcept {
    return std::find_if(sections_.begin(), sections_.end(),
                        [&](const PromptSection& s) { return s.name == name; });
}

inline bool PromptTemplate::contains(std::string_view name) const noexcept {
    return find(name) != end();
}

inline RenderedPrompt PromptTemplate::render() const {
    if (heading_level < 1 || heading_level > 6)
        throw std::logic_error(
            "PromptTemplate::render: heading_level must be within 1..6");

    RenderedPrompt out;
    std::string& md = out.markdown;
    const std::string hashes(static_cast<std::size_t>(heading_level), '#');
    for (const auto& s : sections_) {
        std::string part;
        if (!s.title.empty()) part += hashes + " " + s.title;
        if (!s.text.empty()) {
            if (!part.empty()) part += "\n\n";
            part += s.text;
        }
        if (part.empty()) continue; // nothing to contribute, not even a
                                    // separator
        if (!md.empty()) md += "\n\n";
        SectionSpan span;
        span.name = s.name;
        span.stability = s.stability;
        span.begin = md.size();
        md += part;
        span.end = md.size();
        out.spans.push_back(std::move(span));
    }
    if (!md.empty()) md += '\n';
    return out;
}

// Split on '\n', rstrip every line, drop leading/trailing blank lines. The
// result is final: it is stored as-is and never re-canonicalised, so later
// renders of unchanged sections are byte-identical.
inline std::string PromptTemplate::canonicalize_(std::string text) {
    std::vector<std::string_view> lines;
    std::string_view rest = text;
    while (true) {
        const auto nl = rest.find('\n');
        if (nl == std::string_view::npos) {
            lines.push_back(rest);
            break;
        }
        lines.push_back(rest.substr(0, nl));
        rest.remove_prefix(nl + 1);
    }

    std::string out;
    for (auto line : lines) {
        while (!line.empty()
               && (line.back() == ' ' || line.back() == '\t'
                   || line.back() == '\r'))
            line.remove_suffix(1);
        if (out.empty() && line.empty()) continue; // leading blank line
        if (!out.empty()) out += '\n';
        out += line;
    }
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

inline PromptTemplate::iterator
PromptTemplate::locate_(std::string_view name, const char* who) {
    auto it = std::find_if(sections_.begin(), sections_.end(),
                           [&](const PromptSection& s) {
                               return s.name == name;
                           });
    if (it == sections_.end())
        throw std::logic_error(std::string("PromptTemplate::") + who +
                               ": no section named '" + std::string(name) +
                               "'");
    return it;
}

} // namespace model_io
