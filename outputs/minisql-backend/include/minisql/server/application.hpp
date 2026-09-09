#pragma once
#include "minisql/common/config.hpp"
#include <iosfwd>

namespace minisql {
int runApplication(int argc, const char* const* argv, std::istream& input,
                   std::ostream& output, std::ostream& error,
                   const Environment& environment = processEnvironment);
} // namespace minisql
