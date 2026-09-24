#pragma once
#include <iostream>
#include <string_view>
#include <nlohmann/json.hpp>

namespace core_example {
/** Escape terminal controls, including in peer-provided labels. */
inline void safe(std::ostream& output, std::string_view text) {
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char value : text) {
        if (value == '\n') output << "\n  ";
        else if (value >= 0x20 && value != 0x7f) output << static_cast<char>(value);
        else output << "\\x" << hex[value >> 4] << hex[value & 15];
    }
}
inline void block(std::string_view title, std::string_view text) {
    std::cout << "\n=== ";
    safe(std::cout, title);
    std::cout << " ===\n  ";
    safe(std::cout, text);
    std::cout << "\n=== end ===\n" << std::flush;
}
/** Complete blocks only. Process result prose already separates stdout/stderr. */
inline void display(const nlohmann::json& event) {
    const auto name = event.at("event").get<std::string>();
    const auto& data = event.at("data");
    if (name == "model_response") {
        if (data.contains("reasoning")) block("Reasoning", data["reasoning"].value("raw", ""));
        if (data.contains("content"))
            for (const auto& part : data["content"]) block("Assistant", part.value("raw", ""));
    } else if (name == "tool_results") {
        for (const auto& item : data) {
            const auto& result = item.at("invoke_return");
            block("Tool result " + result.at("query").value("name", ""),
                result.at("output").value("raw", ""));
        }
    } else {
        block(name, data.dump(2));
    }
}
} // namespace core_example
