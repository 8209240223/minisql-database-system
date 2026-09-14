#pragma once
#include "minisql/common/error.hpp"
#include "minisql/common/decimal_type.hpp"
#include <boost/multiprecision/cpp_int.hpp>
#include <algorithm>
#include <cstdint>
#include <string_view>

namespace minisql {
class ExactDecimal {
// 精确十进制数：内部用“整数系数 + 标度”表示，例如 3.14 存成 314 与 2。
public:
    using Integer = boost::multiprecision::cpp_int;
    // 系数的类型别名，避免到处写长长的模板名。
    static ExactDecimal fromRatio(Integer numerator, Integer denominator, unsigned precision = 38, unsigned scale = 6) {
    // 由分子分母构造，内部做一次带舍入的除法。
        validate(precision, scale);
        // 先检查精度与标度是否合法。
        if (denominator == 0) fail("DECIMAL division by zero");
        // 分母为零直接报错。
        if (denominator < 0) { numerator = -numerator;denominator = -denominator; }
        // 把符号统一挪到分子上，后续只处理正分母。
        return ExactDecimal(round(numerator * power(scale), denominator), precision, scale);
        // 先把分子放大 10 的标度次方，再整除并四舍五入，得到带标度的系数。
    }
    static ExactDecimal fromInteger(std::int64_t value) { return ExactDecimal(Integer(value), 38, 0); }
    // 由整数构造：标度为 0，精度取上限 38。
    static ExactDecimal parse(std::string_view text, unsigned precision = 38, unsigned scale = 6, bool rounding = false) {
    // 解析十进制文本；rounding 决定超出标度时是舍入还是直接拒绝。
        validate(precision, scale);
        // 校验目标精度与标度。
        if (text.empty() || text.size() > 1024) fail("Invalid DECIMAL text length");
        // 文本为空或过长都拒绝，后者是防御性的长度上限。
        std::size_t position = 0;
        // 当前扫描位置。
        const bool negative = text.front() == '-';
        // 记录是否为负数。
        if (text.front() == '+' || text.front() == '-') ++position;
        // 跳过可能存在的正负号。
        Integer units = 0;
        // 累加出来的整数系数，暂时不含标度。
        bool dot = false;
        // 是否已经遇到小数点。
        unsigned fraction = 0, integral = 0;
        // 分别统计小数位数与整数位数。
        for (; position < text.size(); ++position) {
        // 逐字符扫描。
            const char c = text[position];
            // 取当前字符。
            if (c == '.' && !dot && integral > 0) { dot = true;continue; }
            // 只允许一个小数点，且小数点前必须有数字。
            if (c < '0' || c > '9') fail("Invalid DECIMAL text");
            // 其余字符必须是数字。
            units = units * 10 + (c - '0');
            // 把这一位并入系数。
            if (dot) ++fraction; else ++integral;
            // 按是否已过小数点分别计数。
        }
        if (integral == 0 || (dot && fraction == 0)) fail("Invalid DECIMAL text");
        // 整数部分为空，或者有小数点却没有小数位，都判非法。
        if (negative) units = -units;
        // 把符号加到系数上。
        if (fraction > scale) {
        // 文本的小数位比目标标度多，需要缩减。
            const auto divisor = power(fraction - scale);
            // 需要除以 10 的差值次方。
            if (!rounding && units % divisor != 0) fail("DECIMAL scale reduction requires explicit rounding");
            // 不允许舍入时，只有能整除才可接受，否则报错。
            units = rounding ? round(units, divisor) : Integer(units / divisor);
            // 允许舍入就四舍五入，否则直接截断。
        } else units *= power(scale - fraction);
        // 小数位不足时反向放大，使系数与目标标度对齐。
        return ExactDecimal(std::move(units), precision, scale);
        // 构造并返回，构造函数里还会做溢出检查。
    }
    std::string format() const {
    // 把系数还原成人类可读的十进制文本。
        const Integer absolute = units_ < 0 ? -units_ : units_;
        // 先取绝对值，便于按位插入小数点。
        std::string digits = absolute.convert_to<std::string>();
        // 系数转成数字串。
        if (scale_) {
        // 标度为零时不需要小数点。
            if (digits.size() <= scale_) digits.insert(0, scale_ + 1 - digits.size(), '0');
            // 位数不足时左侧补零，保证小数点前至少有一位。
            digits.insert(digits.size() - scale_, 1, '.');
            // 从右往左数第 scale 位之前插入小数点。
        }
        if (units_ < 0) digits.insert(0, 1, '-');
        // 负数补回负号。
        return digits;
        // 返回格式化结果。
    }
    ExactDecimal negated() const { return ExactDecimal(-units_, precision_, scale_); }
    // 取相反数，精度标度不变。
    const Integer& coefficient() const { return units_; }
    // 暴露系数，供外部做进一步计算。
    static ExactDecimal fromCoefficient(Integer units, unsigned precision, unsigned scale) { return ExactDecimal(std::move(units), precision, scale); }
    // 由系数直接构造，用于索引或存储层还原数值。
    ExactDecimal arithmetic(std::string_view op, const ExactDecimal& other, SourceLocation location = {}) const {
    // 四则运算入口：先按规则推导结果标度，再对齐系数计算。
        const auto type = decimalArithmeticType(op, scale_, other.scale_, location);
        // 得到结果的精度与标度。
        try {
            if (op == "*") return ExactDecimal(units_ * other.units_, type.precision, type.scale);
            // 乘法：系数直接相乘，标度是两者之和。
            if (op == "/") {
            // 除法需要先通分再舍入。
                if (other.units_ == 0) fail("DECIMAL division by zero");
                // 除数为零报错。
                Integer numerator = units_ * power(other.scale_ + type.scale);
                // 分子放大，使除法结果带足目标标度。
                Integer denominator = other.units_ * power(scale_);
                // 分母同步放大，保证比值不变。
                if (denominator < 0) { numerator = -numerator;denominator = -denominator; }
                // 统一把符号放在分子上。
                return ExactDecimal(round(numerator, denominator), type.precision, type.scale);
                // 带舍入的整数除法得到系数。
            }
            const Integer left = units_ * power(type.scale - scale_);
            // 加减法：把左操作数放大到公共标度。
            const Integer right = other.units_ * power(type.scale - other.scale_);
            // 右操作数同样对齐到公共标度。
            return ExactDecimal(op == "+" ? Integer(left + right) : Integer(left - right), type.precision, type.scale);
            // 按运算符做加法或减法。
        } catch (const MiniSqlError& error) {
        // 内部抛出的错误在这里补上调用点的位置信息。
            throw MiniSqlError(error.code(), error.what(), location);
            // 保留错误码与消息，只替换位置。
        }
    }
    int compare(const ExactDecimal& other) const {
    // 比较两个十进制数：对齐到较大标度后比较系数。
        const auto scale = std::max(scale_, other.scale_);
        // 取两边较大的标度作为公共标度。
        const Integer left = units_ * power(scale - scale_);
        // 左系数放大。
        const Integer right = other.units_ * power(scale - other.scale_);
        // 右系数放大。
        return left < right ? -1 : left > right ? 1 : 0;
        // 返回负数、零或正数，分别表示小于、等于、大于。
    }
private:
    Integer units_;
    // 内部系数，真实值等于 units_ 除以 10 的 scale_ 次方。
    unsigned precision_, scale_;
    // 精度与标度。
    ExactDecimal(Integer units, unsigned precision, unsigned scale)
        : units_(std::move(units)), precision_(precision), scale_(scale) {
        // 私有构造：所有公开入口最终都走到这里，保证构造出的对象一定合法。
        validate(precision, scale);
        // 校验精度标度。
        const auto limit = power(precision);
        // 精度对应的上限，例如精度 3 表示绝对值必须小于 1000。
        if (units_ >= limit || units_ <= -limit) fail("DECIMAL precision overflow");
        // 超出范围就报精度溢出。
    }
    [[noreturn]] static void fail(const char* message) { throw MiniSqlError(ErrorCode::Execution, message); }
    // 私有失败出口；[[noreturn]] 说明调用后不会返回，编译器可据此优化。
    static void validate(unsigned precision, unsigned scale) {
    // 校验精度与标度的组合是否合法。
        if (precision == 0 || precision > 38 || scale > precision) fail("Invalid DECIMAL precision or scale");
        // 精度必须在 1 到 38 之间，且标度不能超过精度。
    }
    static Integer power(unsigned exponent) {
    // 计算 10 的整数次幂，用于对齐标度。
        Integer result = 1;
        // 从 1 开始。
        while (exponent--) result *= 10;
        // 循环乘十，次数即指数。
        return result;
    }
    static Integer round(const Integer& numerator, const Integer& denominator) {
    // 对除法结果做四舍五入，返回整数商。
        const bool negative = numerator < 0;
        // 先记录符号。
        const Integer absolute = negative ? -numerator : numerator;
        // 转为绝对值参与运算。
        Integer quotient = absolute / denominator;
        // 整数商。
        const Integer remainder = absolute % denominator;
        // 余数用于判断是否需要进位。
        // HALF_UP 对绝对值的中点进位，再恢复符号，负数中点也远离零。
        if (remainder * 2 >= denominator) ++quotient;
        // 余数达到除数的一半就进位，即四舍五入。
        return negative ? Integer(-quotient) : quotient;
        // 恢复符号后返回。
    }
};
} // namespace minisql
