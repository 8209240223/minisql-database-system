#include "minisql/common/decimal.hpp"
#include <iostream>
#include <stdexcept>

int main() {
    using D = minisql::ExactDecimal;
    unsigned checks = 0;
    const auto require = [&](bool condition) { if (!condition) throw std::runtime_error("Exact DECIMAL contract failed at " + std::to_string(checks)); ++checks; };
    require(D::fromRatio(1,3).format() == "0.333333");
    require(D::fromRatio(2,3).format() == "0.666667");
    require(D::fromRatio(-2,3).format() == "-0.666667");
    require(D::fromRatio(1,-3).format() == "-0.333333");
    require(D::fromRatio(1,2000000).format() == "0.000001");
    require(D::fromRatio(-1,2000000).format() == "-0.000001");
    require(D::fromRatio(-1,3000000).format() == "0.000000");
    require(D::fromRatio(D::Integer("9223372036854775807")*2,2).format() == "9223372036854775807.000000");
    require(D::fromRatio(D::Integer("-9223372036854775808")*2,2).format() == "-9223372036854775808.000000");
    require(D::fromRatio(-1,2,38,0).format() == "-1");
    require(D::parse("00012.3400").format() == "12.340000");
    require(D::parse("-0.000000").format() == "0.000000");
    require(D::parse("+1").format() == "1.000000");
    require(D::parse("1.0000000").format() == "1.000000");
    require(D::parse("9.9999995",38,6,true).format() == "10.000000");
    require(D::parse("-1.2345675",38,6,true).format() == "-1.234568");
    require(D::parse("99999999999999999999999999999999.999999").format() == "99999999999999999999999999999999.999999");
    require(D::parse("0.00000000000000000000000000000000000001",38,38).format() == "0.00000000000000000000000000000000000001");
    require(D::parse("2").compare(D::parse("10")) < 0);
    require(D::parse("-10").compare(D::parse("-2")) < 0);
    require(D::parse("1.00",38,2).compare(D::fromInteger(1)) == 0);
    require(D::parse("-2").negated().format() == "2.000000");
    require(D::parse("1.25",3,2).arithmetic("+",D::parse("0.125",3,3)).format() == "1.375");
    require(D::parse("1.25",3,2).arithmetic("-",D::parse("2",1,0)).format() == "-0.75");
    require(D::parse("1.25",3,2).arithmetic("*",D::parse("0.125",3,3)).format() == "0.15625");
    require(D::parse("1",1,0).arithmetic("/",D::parse("3",1,0)).format() == "0.333333");
    require(D::parse("-2",1,0).arithmetic("/",D::parse("3",1,0)).format() == "-0.666667");
    require(D::parse("1.2",2,1).arithmetic("/",D::parse("0.03",2,2)).format() == "40.000000");
    require(D::parse("1.00000001",9,8).arithmetic("/",D::parse("-2",1,0)).format() == "-0.50000001");
    require(D::parse("-0.00000001",8,8).arithmetic("/",D::parse("3",1,0)).format() == "0.00000000");
    require(D::parse("99999999999999999999999999999999999998",38,0).arithmetic("+",D::fromInteger(1)).format() == "99999999999999999999999999999999999999");
    require(minisql::decimalType("decimal(38,12)")->scale == 12);
    for (const auto type : {"decimal(39,1)","decimal(1,2)","decimal(0,0)","decimal(1,)","decimal(1,0)x","varchar"}) require(!minisql::decimalType(type));
    for (const std::string op : {"+","*","/"}) {
        bool failed = false;
        try {
            const auto maximum = D::parse("99999999999999999999999999999999999999",38,0);
            (void)maximum.arithmetic(op,D::fromInteger(op == "/" ? 0 : 2),{2,7});
        } catch (const minisql::MiniSqlError& error) {
            failed = error.code() == minisql::ErrorCode::Execution && error.location().line == 2 && error.location().column == 7;
        }
        require(failed);
    }
    bool scaleOverflow = false;
    try { (void)minisql::decimalArithmeticType("*",20,19,{3,8}); }
    catch (const minisql::MiniSqlError& error) { scaleOverflow = error.code() == minisql::ErrorCode::Semantic && error.location().column == 8; }
    require(scaleOverflow);
    for (const auto& text : {"", "+", "-", ".5", "1.", "1e3", "1.2.3", " 1", "1 ", "NaN", "1.0000001", "100000000000000000000000000000000"}) {
        bool failed = false;
        try { (void)D::parse(text); } catch (const minisql::MiniSqlError& error) { failed = error.code() == minisql::ErrorCode::Execution; }
        require(failed);
    }
    for (auto value : {0u,39u}) {
        bool failed = false;
        try { (void)D::parse("1",value,0); } catch (const minisql::MiniSqlError&) { failed = true; }
        require(failed);
    }
    bool zero = false;
    try { (void)D::fromRatio(1,0); } catch (const minisql::MiniSqlError&) { zero = true; }
    require(zero);
    bool scale = false;
    try { (void)D::parse("0",2,3); } catch (const minisql::MiniSqlError&) { scale = true; }
    require(scale);
    bool overflow = false;
    try { (void)D::parse("9.95",2,1,true); } catch (const minisql::MiniSqlError&) { overflow = true; }
    require(overflow);
    std::cout << checks << " exact decimal checks passed\n";
}
