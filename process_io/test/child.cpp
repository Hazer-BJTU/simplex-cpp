#include <array>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

// Controlled local fixture: no shell initialization or optional interpreter.
int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    const std::string mode = argv[1];
    if (mode == "echo") {
        std::array<char, 4096> buffer;
        while (std::cin) {
            std::cin.read(buffer.data(), buffer.size());
            std::cout.write(buffer.data(), std::cin.gcount());
            std::cout.flush();
        }
        std::cerr << "done";
    } else if (mode == "burst") {
        const std::string block(4096, 'x');
        for (int i = 0; i < 128; ++i) {
            std::cout << block;
            std::cerr << block;
        }
    } else if (mode == "exit") {
        std::cout << "tail\n";
        std::cerr << "error\n";
    } else if (mode == "no-read" || mode == "close-input") {
        if (mode == "close-input") ::close(STDIN_FILENO);
        std::cout << "ready" << std::flush;
        std::this_thread::sleep_for(std::chrono::seconds(30));
    } else {
        return 3;
    }
}
