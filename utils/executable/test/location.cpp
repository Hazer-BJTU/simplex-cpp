#include "executable/location.hpp"

#include <iostream>

int main(int argc, char** argv)
{
    if (argc != 2) {
        return 2;
    }
    const auto expected = std::filesystem::canonical(argv[1]);
    // Discovery must not depend on the current working directory, including
    // when an explicitly invoked musl loader is the kernel's executable.
    std::filesystem::current_path(std::filesystem::temp_directory_path());
    auto error = std::make_error_code(std::errc::io_error);
    const auto actual = executable::location(error);
    if (error || actual != expected) {
        std::cerr << "expected " << expected << ", found " << actual
                  << ": " << error.message() << '\n';
        return 1;
    }
}
