#pragma once

#include "tools/intrinsic/toolset_base.hpp"
#include "persistence/archive_query.hpp"
#include <boost/asio/any_io_executor.hpp>

namespace tools::intrinsic {

/**
 * Read retained SQLite archives for one host-selected session. The worker only
 * registers this set when persistence is enabled and the selected backend is
 * SQLite, including auto selection. No path or session selector is model-facing.
 *
 * The reader is shared/immutable; the executor is borrowed and must remain alive
 * until every invocation has completed. Use a storage executor, not the network
 * executor: each joined operation performs synchronous, timeout-bounded SQL.
 * Construction loads declarations/skill but does not open or migrate a database.
 */
class ArchiveMemoryToolSet final : public IntrinsicToolSet {
public:
    ArchiveMemoryToolSet(
        std::shared_ptr<const persistence::ArchiveReader> reader,
        boost::asio::any_io_executor storage_executor);

    std::string_view name() const noexcept override;
};

} // namespace tools::intrinsic
