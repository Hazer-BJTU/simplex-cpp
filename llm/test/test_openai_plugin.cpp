#define BOOST_TEST_MODULE openai_llm_plugin
#include <boost/test/unit_test.hpp>

#include <boost/asio/io_context.hpp>

#include <filesystem>

#include "llm/models.hpp"

#ifndef OPENAI_PLUGIN_DIR
#error OPENAI_PLUGIN_DIR must name the built provider-plugin directory
#endif

BOOST_AUTO_TEST_CASE(dispatcher_loads_and_mints_openai_responses_model) {
    llm::LLMDispatcher dispatcher;
    BOOST_CHECK_EQUAL(
        dispatcher.load_models(std::filesystem::path(OPENAI_PLUGIN_DIR)), 1u);

    boost::asio::io_context io;
    auto model = dispatcher.create_model(
        "openai", io.get_executor(), nlohmann::json{{"model", "test-model"}});
    BOOST_REQUIRE(model);
    BOOST_CHECK(model->model_type() == llm::LLMModelType::Conversation);
    BOOST_CHECK(!dispatcher.create_model(
        "unknown", io.get_executor(), nlohmann::json{{"model", "test-model"}}));
}
