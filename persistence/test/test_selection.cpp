#define BOOST_TEST_MODULE PersistenceSelection
#include <boost/test/unit_test.hpp>
#include "persistence/selection.hpp"
#include <fstream>
#include <unistd.h>

struct Scratch {
    std::filesystem::path root = std::filesystem::temp_directory_path()
        / ("simplex_selection_" + std::to_string(getpid()));
    Scratch() { std::filesystem::remove_all(root); }
    ~Scratch() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

BOOST_FIXTURE_TEST_CASE(selection_matrix_preserves_existing_history, Scratch) {
    using persistence::Backend;
    using persistence::select_backend;
    BOOST_CHECK(select_backend(root) == Backend::Sqlite);
    BOOST_CHECK(select_backend(root, "json") == Backend::Json);
    BOOST_TEST(!std::filesystem::exists(root));
    std::filesystem::create_directories(root);
    std::ofstream(root / "state.json") << "invalid data still counts as existing history";
    BOOST_CHECK(select_backend(root) == Backend::Json);
    BOOST_CHECK_THROW(select_backend(root, "sqlite"), std::runtime_error);
    std::ofstream(root / "state.sqlite3") << "invalid database";
    BOOST_CHECK_THROW(select_backend(root), std::runtime_error);
    // Both stores present: no explicit choice can pick one without abandoning
    // the other history, so selection is refused for every setting.
    BOOST_CHECK_THROW(select_backend(root, "sqlite"), std::runtime_error);
    BOOST_CHECK_THROW(select_backend(root, "json"), std::runtime_error);
    std::filesystem::remove(root / "state.json");
    BOOST_CHECK(select_backend(root) == Backend::Sqlite);
    BOOST_CHECK_THROW(select_backend(root, "json"), std::runtime_error);
    BOOST_CHECK_THROW(persistence::inspect_sqlite_current(root), std::exception);
    BOOST_CHECK_THROW(select_backend(root, "unknown"), std::invalid_argument);
}

BOOST_FIXTURE_TEST_CASE(inspection_preserves_complete_current_state_and_revision, Scratch) {
    BOOST_TEST(!persistence::inspect_sqlite_current(root));
    BOOST_TEST(!std::filesystem::exists(root));
    model_io::AgentInputState state;
    state.meta.session_id = "owner";
    state.extras = {{"payload", std::string(1024 * 1024, 'x')},
        {"counter", UINT64_MAX}, {"binary", std::string("a\0b", 3)}};
    auto store = persistence::make_sqlite_store("owner", root);
    const auto receipt = store->checkpoint(state, persistence::Boundary::Shutdown);
    const auto current = persistence::inspect_sqlite_current(root);
    BOOST_REQUIRE(current);
    BOOST_TEST(nlohmann::json(current->state) == nlohmann::json(state));
    BOOST_TEST(*current->revision == *receipt.revision);
    BOOST_TEST(current->operation_id == receipt.operation_id);
}

BOOST_FIXTURE_TEST_CASE(stopped_store_directory_copy_preserves_state_and_identity, Scratch) {
    model_io::AgentInputState state;
    state.meta.session_id = "backup-owner";
    state.extras = {{"marker", "complete backup"}};
    {
        auto store = persistence::make_sqlite_store("backup-owner", root / "original");
        store->checkpoint(state, persistence::Boundary::Shutdown);
    }
    // All connections are closed before copying the directory, as documented.
    // This is not a recipe for copying the main file of a live WAL database.
    std::filesystem::copy(root / "original", root / "backup",
        std::filesystem::copy_options::recursive);
    const auto backup = persistence::inspect_sqlite_current(root / "backup");
    BOOST_REQUIRE(backup);
    BOOST_TEST(nlohmann::json(backup->state) == nlohmann::json(state));
}
