#include "fileio/child_path.hpp"
#include <stdexcept>

namespace fileio {

std::filesystem::path child_path(
    const std::filesystem::path& root, std::string_view relative) {
    const std::filesystem::path child(relative);
    if (relative.empty() || relative.find('\0') != std::string_view::npos
        || child.is_absolute() || child.has_root_name()) {
        throw std::invalid_argument("persistence child must be a nonempty relative directory");
    }
    for (const auto& component : child) {
        if (component == "..") {
            throw std::invalid_argument("persistence child must not contain parent traversal");
        }
    }
    return (root / child).lexically_normal();
}

} // namespace fileio
