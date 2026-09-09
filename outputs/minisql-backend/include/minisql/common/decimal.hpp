#pragma once
#include "minisql/common/error.hpp"
#include "minisql/common/decimal_type.hpp"
#include <boost/multiprecision/cpp_int.hpp>
#include <algorithm>
#include <cstdint>
#include <string_view>

namespace minisql {
class ExactDecimal {
public:
    using Integer = boost::multiprecision::cpp_int;
    static ExactDecimal fromRatio(Integer numerator, Integer denominator, unsigned precision = 38, unsigned scale = 6) {
        validate(precision, scale);
        if (denominator == 0) fail("DECIMAL division by zero");
        if (denominator < 0) { numerator = -numerator;denominator = -denominator; }
        return ExactDecimal(round(numerator * power(scale), denominator), precision, scale);
    }
    static ExactDecimal fromInteger(std::int64_t value) { return ExactDecimal(Integer(value), 38, 0); }
    static ExactDecimal parse(std::string_view text, unsigned precision = 38, unsigned scale = 6, bool rounding = false) {
        validate(precision, scale);
        if (text.empty() || text.size() > 1024) fail("Invalid DECIMAL text length");
        std::size_t position = 0;
        const bool negative = text.front() == '-';
        if (text.front() == '+' || text.front() == '-') ++position;
        Integer units = 0;
        bool dot = false;
        unsigned fraction = 0, integral = 0;
        for (; position < text.size(); ++position) {
            const char c = text[position];
            if (c == '.' && !dot && integral > 0) { dot = true;continue; }
            if (c < '0' || c > '9') fail("Invalid DECIMAL text");
            units = units * 10 + (c - '0');
            if (dot) ++fraction; else ++integral;
        }
        if (integral == 0 || (dot && fraction == 0)) fail("Invalid DECIMAL text");
        if (negative) units = -units;
        if (fraction > scale) {
            const auto divisor = power(fraction - scale);
            if (!rounding && units % divisor != 0) fail("DECIMAL scale reduction requires explicit rounding");
            units = rounding ? round(units, divisor) : Integer(units / divisor);
        } else units *= power(scale - fraction);
        return ExactDecimal(std::move(units), precision, scale);
    }
    std::string format() const {
        const Integer absolute = units_ < 0 ? -units_ : units_;
        std::string digits = absolute.convert_to<std::string>();
        if (scale_) {
            if (digits.size() <= scale_) digits.insert(0, scale_ + 1 - digits.size(), '0');
            digits.insert(digits.size() - scale_, 1, '.');
        }
        if (units_ < 0) digits.insert(0, 1, '-');
        return digits;
    }
    ExactDecimal negated() const { return ExactDecimal(-units_, precision_, scale_); }
    const Integer& coefficient() const { return units_; }
    static ExactDecimal fromCoefficient(Integer units, unsigned precision, unsigned scale) { return ExactDecimal(std::move(units), precision, scale); }
    ExactDecimal arithmetic(std::string_view op, const ExactDecimal& other, SourceLocation location = {}) const {
        const auto type = decimalArithmeticType(op, scale_, other.scale_, location);
        try {
            if (op == "*") return ExactDecimal(units_ * other.units_, type.precision, type.scale);
            if (op == "/") {
                if (other.units_ == 0) fail("DECIMAL division by zero");
                Integer numerator = units_ * power(other.scale_ + type.scale);
                Integer denominator = other.units_ * power(scale_);
                if (denominator < 0) { numerator = -numerator;denominator = -denominator; }
                return ExactDecimal(round(numerator, denominator), type.precision, type.scale);
            }
            const Integer left = units_ * power(type.scale - scale_);
            const Integer right = other.units_ * power(type.scale - other.scale_);
            return ExactDecimal(op == "+" ? Integer(left + right) : Integer(left - right), type.precision, type.scale);
        } catch (const MiniSqlError& error) {
            throw MiniSqlError(error.code(), error.what(), location);
        }
    }
    int compare(const ExactDecimal& other) const {
        const auto scale = std::max(scale_, other.scale_);
        const Integer left = units_ * power(scale - scale_);
        const Integer right = other.units_ * power(scale - other.scale_);
        return left < right ? -1 : left > right ? 1 : 0;
    }
private:
    Integer units_;
    unsigned precision_, scale_;
    ExactDecimal(Integer units, unsigned precision, unsigned scale)
        : units_(std::move(units)), precision_(precision), scale_(scale) {
        validate(precision, scale);
        const auto limit = power(precision);
        if (units_ >= limit || units_ <= -limit) fail("DECIMAL precision overflow");
    }
    [[noreturn]] static void fail(const char* message) { throw MiniSqlError(ErrorCode::Execution, message); }
    static void validate(unsigned precision, unsigned scale) {
        if (precision == 0 || precision > 38 || scale > precision) fail("Invalid DECIMAL precision or scale");
    }
    static Integer power(unsigned exponent) {
        Integer result = 1;
        while (exponent--) result *= 10;
        return result;
    }
    static Integer round(const Integer& numerator, const Integer& denominator) {
        const bool negative = numerator < 0;
        const Integer absolute = negative ? -numerator : numerator;
        Integer quotient = absolute / denominator;
        const Integer remainder = absolute % denominator;
        // HALF_UP 对绝对值的中点进位，再恢复符号，负数中点也远离零。
        if (remainder * 2 >= denominator) ++quotient;
        return negative ? Integer(-quotient) : quotient;
    }
};
}
