#include "minisql/common/arithmetic.hpp"
#include <iostream>
#include <string>

int main() {
    std::string op;
    std::int64_t left, right;
    while (std::cin >> op >> left >> right) {
        try {
            const auto value = minisql::arithmetic64(op, left, right, {7, 11});
            std::cout << "OK " << value << '\n';
        } catch (const minisql::MiniSqlError& error) {
            std::cout << "ERR " << static_cast<int>(error.code()) << ' ' << error.what() << '\n';
            if (error.location().line != 7 || error.location().column != 11) return 2;
        }
    }
    return std::cin.eof() ? 0 : 3;
}
