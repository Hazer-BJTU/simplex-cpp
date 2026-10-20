#pragma once

#include "tools/intrinsic/tool_base.hpp"
#include "persistence/archive_query.hpp"
#include <boost/asio/any_io_executor.hpp>

namespace tools::intrinsic::archive_memory {

/** Shared immutable dependencies and host-only limits for read-only queries. */
class QueryTool : public DeclaredTool {
public:
    void write_attributes(model_io::InvokeQuery& query) const override;

protected:
    QueryTool(const char* declaration,
              std::shared_ptr<const persistence::ArchiveReader> reader,
              boost::asio::any_io_executor storage_executor,
              bool search);

    /// Validate names and text before dispatch; SQL also validates source ranges.
    static void validate_fields(const model_io::InvokeQuery& query,
                                std::initializer_list<std::string_view> allowed);
    static void validate_text(const std::string& value, const char* name);
    [[noreturn]] static void query_failed(const persistence::ArchiveQueryError& error);

    std::shared_ptr<const persistence::ArchiveReader> reader_;
    boost::asio::any_io_executor storage_executor_;
    persistence::ArchiveQueryOptions options_;
    std::size_t max_output_bytes_ = 32768;
    std::size_t snippet_bytes_ = 512;
};

/** Literal search; result locators remain valid only while their archive exists. */
class SearchTool final : public QueryTool {
public:
    SearchTool(std::shared_ptr<const persistence::ArchiveReader> reader,
               boost::asio::any_io_executor storage_executor);
    void ensure_arguments(model_io::InvokeQuery& query) const override;
    boost::asio::awaitable<model_io::Content> invoke(const model_io::InvokeQuery& query) override;
};

/** Bounded original-source reads, with display replacement and byte pagination. */
class FetchTool final : public QueryTool {
public:
    FetchTool(std::shared_ptr<const persistence::ArchiveReader> reader,
              boost::asio::any_io_executor storage_executor);
    void ensure_arguments(model_io::InvokeQuery& query) const override;
    boost::asio::awaitable<model_io::Content> invoke(const model_io::InvokeQuery& query) override;
};

} // namespace tools::intrinsic::archive_memory
