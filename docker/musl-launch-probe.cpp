// Packaging-only probe, compiled with the same musl toolchain as the worker.
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <unistd.h>
#include <nlohmann/json.hpp>
int main(int argc, char** argv)
{
    nlohmann::json result;
    result["pid"] = getpid();
    result["cwd"] = std::filesystem::current_path().string();
    result["arguments"] = nlohmann::json::array();
    for (int i = 1; i < argc; ++i) {
        result["arguments"].push_back(argv[i]);
    }
    for (const auto& [key, name] : {
             std::pair{"library_path", "LD_LIBRARY_PATH"},
             {"cert_file", "SSL_CERT_FILE"}, {"cert_dir", "SSL_CERT_DIR"},
             {"openssl_conf", "OPENSSL_CONF"}}) {
        if (const char* value = std::getenv(name)) {
            result[key] = value;
        } else {
            result[key] = nullptr;
        }
    }
    std::cout << result.dump() << '\n';
    return 37;
}
