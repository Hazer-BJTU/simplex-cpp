#include <iostream>
#include <string>
#include <system_error>
#include <boost/program_options.hpp>
#include "fileio/atomic_write.hpp"
#include "fileio/base64.hpp"
#include "manager/service.hpp"
#include "versioning/version.hpp"

/** One process handles one trusted request; stdout is never used for logging. */
int main(int argc, char** argv) {
    using Json = nlohmann::json;
    Json response = {{"version", 1}, {"request_id", nullptr}, {"ok", false}};
    try {
        namespace po = boost::program_options;
        po::options_description options("simplex manage options");
        options.add_options()
            ("help,h", "Show management command usage")
            ("version", "Show compiled version")
            ("request", po::value<std::string>(), "Base64-encoded UTF-8 JSON request (one per invocation)");
        po::variables_map arguments;
        try {
            po::store(po::parse_command_line(argc, argv, options), arguments);
            po::notify(arguments);
        } catch (const po::error&) {
            throw manager::Error("invalid_arguments", "invalid manager command arguments");
        }
        if (arguments.count("help")) {
            std::cout << "Usage: simplex manage --request BASE64\n" << options << '\n';
            return std::cout ? 0 : 1;
        }
        if (arguments.count("version")) {
            std::cout << simplex::VERSION_STRING << '\n';
            return std::cout ? 0 : 1;
        }
        if (!arguments.count("request")) {
            throw manager::Error("invalid_arguments", "--request is required");
        }
        Json request;
        try {
            request = Json::parse(fileio::base64_decode(arguments["request"].as<std::string>()));
        } catch (const std::exception&) {
            throw manager::Error("invalid_request", "request must be Base64-encoded UTF-8 JSON");
        }
        if (request.is_object() && request.contains("request_id") && request["request_id"].is_string()) {
            const auto id = request["request_id"].get<std::string>();
            if (id.size() <= 128 && id.find_first_not_of(
                    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") == std::string::npos) {
                response["request_id"] = id;
            }
        }
        response["result"] = manager::execute(request);
        response["ok"] = true;
    } catch (const manager::Error& error) {
        response["error"] = {{"code", error.code}, {"message", error.what()}};
    } catch (const fileio::AtomicWriteError& error) {
        response["error"] = {{"code", "storage_error"},
            {"message", "storage publication failed; inspect before retrying"},
            {"published", error.published()}};
    } catch (const std::system_error& error) {
        response["error"] = {{"code", "storage_error"}, {"message", error.code().message()}};
    } catch (const Json::exception&) {
        response["error"] = {{"code", "invalid_data"}, {"message", "invalid request or stored field type"}};
    } catch (const std::exception&) {
        // Low-level YAML/IO exceptions may contain credentials or raw input.
        response["error"] = {{"code", "storage_error"}, {"message", "management operation failed"}};
    }
    std::string output;
    try {
        output = response.dump();
    } catch (const Json::exception&) {
        // For example, a damaged legacy readable export may contain invalid UTF-8.
        response.erase("result");
        response["ok"] = false;
        response["error"] = {{"code", "invalid_data"}, {"message", "result is not valid JSON text"}};
        output = response.dump();
    }
    std::cout << output << '\n' << std::flush;
    return response.at("ok").get<bool>() && std::cout ? 0 : 1;
}
