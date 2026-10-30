#define BOOST_TEST_MODULE TextEditSmallStack
#include <boost/test/unit_test.hpp>

#include "textedit/edit.hpp"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {
struct Scratch {
    std::filesystem::path root;
    Scratch()
    {
        auto pattern = (std::filesystem::temp_directory_path() / "simplex-edit-stack-XXXXXX").string();
        const auto directory = ::mkdtemp(pattern.data());
        if (!directory) throw std::runtime_error("cannot create edit stack directory");
        root = directory;
    }
    ~Scratch()
    {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

struct Edit {
    std::filesystem::path path;
    std::string replacement;
    textedit::EditResult result;
    std::exception_ptr failure;
};

/** No Boost.Test assertion runs on the small-stack thread. */
void* replace_on_thread(void* argument) noexcept
{
    auto& edit = *static_cast<Edit*>(argument);
    try {
        edit.result = textedit::str_replace_file(edit.path, "UNIQUE", edit.replacement);
    } catch (...) {
        edit.failure = std::current_exception();
    }
    return nullptr;
}

/** Match musl's small default pthread stack even on glibc CI hosts. */
void run_on_small_stack(Edit& edit)
{
    pthread_attr_t attributes;
    BOOST_REQUIRE_EQUAL(::pthread_attr_init(&attributes), 0);
    struct Attributes {
        pthread_attr_t& value;
        ~Attributes() { ::pthread_attr_destroy(&value); }
    } cleanup{attributes};
    BOOST_REQUIRE_EQUAL(::pthread_attr_setstacksize(&attributes, 128 * 1024), 0);
    pthread_t thread;
    BOOST_REQUIRE_EQUAL(::pthread_create(&thread, &attributes, replace_on_thread, &edit), 0);
    BOOST_REQUIRE_EQUAL(::pthread_join(thread, nullptr), 0);
    if (edit.failure) std::rethrow_exception(edit.failure);
}
}

BOOST_AUTO_TEST_CASE(exact_edits_fit_small_worker_thread_stacks)
{
    Scratch scratch;
    const auto path = scratch.root / "source";
    // Exercise empty and multi-chunk prefix/suffix verification, removal and
    // multi-chunk candidate skipping. None of these require file-sized memory.
    for (const std::size_t prefix_size : {std::size_t{0}, std::size_t{192 * 1024}}) {
        for (const std::size_t suffix_size : {std::size_t{0}, std::size_t{192 * 1024}}) {
            for (const std::size_t replacement_size : {std::size_t{0}, std::size_t{192 * 1024}}) {
                const std::string prefix(prefix_size, 'a');
                const std::string suffix(suffix_size, 'b');
                Edit edit{path, std::string(replacement_size, 'c'), {}, {}};
                {
                    std::ofstream output(path, std::ios::binary | std::ios::trunc);
                    output << prefix << "UNIQUE" << suffix;
                    BOOST_REQUIRE(output.good());
                }
                run_on_small_stack(edit);
                BOOST_CHECK(edit.result.status == textedit::EditStatus::Modified);
                std::ifstream input(path, std::ios::binary);
                const std::string actual{std::istreambuf_iterator<char>(input), {}};
                BOOST_TEST(actual == prefix + edit.replacement + suffix);
                BOOST_TEST(std::distance(std::filesystem::directory_iterator(scratch.root),
                                          std::filesystem::directory_iterator{}) == 1);
            }
        }
    }
}
