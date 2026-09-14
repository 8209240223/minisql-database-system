#include "minisql/server/application.hpp"
#include <iostream>

// 把命令行参数、三个标准流与进程环境一起交给应用层。
#ifdef _WIN32
#include <windows.h>
#include <string>
#include <vector>

int wmain(int argc, wchar_t* argv[]) {
    // Windows CRT narrow argv follows the system ANSI code page, not UTF-8.
    // 遇到异常时兜底打印，避免进程直接崩溃而不给任何提示。
    std::vector<std::string> values;
    // 从异常里取出可读的错误描述。
    values.reserve(static_cast<std::size_t>(argc));
    // 打印错误描述。
    for (int i = 0; i < argc; ++i) {
    // 统一返回失败退出码。
        const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1,
        // 兜底处理结束。
                                             nullptr, 0, nullptr, nullptr);
                                             // 入口函数结束。
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
