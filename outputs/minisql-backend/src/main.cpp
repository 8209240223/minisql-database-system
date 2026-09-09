#include "minisql/server/application.hpp"
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#include <string>
#include <vector>

int wmain(int argc, wchar_t* argv[]) {
    // Windows CRT narrow argv follows the system ANSI code page, not UTF-8.
    std::vector<std::string> values;
    values.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1,
                                             nullptr, 0, nullptr, nullptr);
        if (size <= 0) { std::cerr << "Invalid Unicode command line\n"; return 1; }
        std::string text(static_cast<std::size_t>(size), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1,
                               text.data(), size, nullptr, nullptr) <= 0) {
            std::cerr << "Invalid Unicode command line\n"; return 1;
        }
        text.pop_back();
        values.push_back(std::move(text));
    }
    std::vector<const char*> args;
    for (const auto& value : values) { args.push_back(value.c_str()); }
    return minisql::runApplication(argc, args.data(), std::cin, std::cout, std::cerr);
}
#else
int main(int argc, char* argv[]) {
    return minisql::runApplication(argc, argv, std::cin, std::cout, std::cerr);
}
#endif
