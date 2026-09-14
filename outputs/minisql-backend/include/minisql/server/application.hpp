#pragma once
#include "minisql/common/config.hpp"
#include <iosfwd>

namespace minisql {
int runApplication(int argc, const char* const* argv, std::istream& input,
// 主入口：接收命令行参数以及三个流（标准输入、标准输出、标准错误）。
                   std::ostream& output, std::ostream& error,
                   // （承接上一行）把流作为参数传入，便于测试时注入内存流，而不是真的读写控制台。
                   const Environment& environment = processEnvironment);
                   // environment 默认取进程环境，测试时可以注入一份假环境。
} // namespace minisql
