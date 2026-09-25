#include "load/configuration.hpp"
#include "yamlconfig/yaml_json.hpp"

#include <stdexcept>

namespace load {

model_io::PromptTemplate read_system_prompt(const std::filesystem::path& file) {
    try {
        const auto document = yamlconfig::load_file(file);
        if (!document.is_object()) {
            throw std::invalid_argument("expected a prompt mapping");
        }
        model_io::PromptTemplate prompt;
        if (const auto heading = document.find("heading_level"); heading != document.end()) {
            if (!heading->is_number_integer() || *heading < 1 || *heading > 6) {
                throw std::invalid_argument("heading_level must be an integer in 1..6");
            }
            prompt.heading_level = heading->get<int>();
        }
        const auto& sections = document.at("sections");
        if (!sections.is_array()) {
            throw std::invalid_argument("sections must be a list");
        }
        for (const auto& section : sections) {
            if (!section.is_object()) {
                throw std::invalid_argument("each section must be a mapping");
            }
            const auto name = section.at("name").get<std::string>();
            if (name.empty() || name.starts_with("skill.") || name == "environment.runtime") {
                throw std::invalid_argument("section names must be nonempty; skill.* and environment.runtime are reserved");
            }
            const auto title = section.value("title", std::string{});
            const auto text = section.at("text").get<std::string>();
            const auto tier = section.value("stability", std::string("immutable"));
            model_io::SectionStability stability;
            if (tier == "immutable") {
                stability = model_io::SectionStability::Immutable;
            } else if (tier == "growing") {
                stability = model_io::SectionStability::Growing;
            } else if (tier == "volatile") {
                stability = model_io::SectionStability::Volatile;
            } else {
                throw std::invalid_argument("stability must be immutable, growing or volatile");
            }
            prompt.add_section(name, title, text, stability);
        }
        (void)prompt.render();
        return prompt;
    } catch (const std::exception& error) {
        throw std::invalid_argument("system prompt '" + file.string() + "': " + error.what());
    }
}

} // namespace load
