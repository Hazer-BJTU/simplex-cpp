#include "tools/intrinsic/archive_memory/toolset.hpp"
#include "tools/intrinsic/archive_memory/schemas.hpp"
#include "tools.hpp"

namespace tools::intrinsic {

ArchiveMemoryToolSet::ArchiveMemoryToolSet(
    std::shared_ptr<const persistence::ArchiveReader> reader,
    boost::asio::any_io_executor storage_executor)
{
    if (!reader) {
        throw std::invalid_argument("archive memory requires a session reader");
    }
    register_tools({
        std::make_shared<archive_memory::SearchTool>(reader, storage_executor),
        std::make_shared<archive_memory::FetchTool>(reader, storage_executor)
    });
    declare_capability_group("archive_memory", {"memory_search", "memory_fetch"});
    load_skill(archive_memory::schema_directory() / "skill.yaml");
}

std::string_view ArchiveMemoryToolSet::name() const noexcept
{
    return "archive_memory";
}

} // namespace tools::intrinsic
