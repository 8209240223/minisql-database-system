#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>
#include <nlohmann/json_fwd.hpp>

namespace minisql {

enum class ErrorCode {
// 错误码枚举；class 说明是强类型枚举，不会隐式转换成整数。
    Ok = 0, InvalidArgument = 1001, Configuration = 1002,
    // 0 表示成功；1001 是调用参数不合法，1002 是配置文件有问题。
    Lexical = 2001, Syntax = 2002, Semantic = 2003,
    // 2001、2002、2003 分别对应编译器三阶段：词法、语法、语义。
    Catalog = 3001, Storage = 4001, Execution = 5001, Cancelled = 5002,
    // 3001 目录错误，4001 存储错误，5001 执行错误，5002 用户主动取消。
    Transaction = 6001, Permission = 7001, Network = 8001,
    // 6001 事务错误，7001 权限错误，8001 网络错误。
    NotImplemented = 9001, Internal = 9999
    // 9001 表示功能未实现，9999 是兜底的内部错误。
};
// 千位即类别：2xxx 编译、3xxx 目录、4xxx 存储、5xxx 执行，6xxx 及以后是事务与外围设施。

struct SourceLocation {
// 记录错误在源码中的位置，供前端定位与高亮。
    std::size_t line{0};   // 1-based; 0 means location unavailable.
    // 行号从 1 开始计数；0 表示拿不到位置信息。
    std::size_t column{0};
    // 列号同样从 1 开始计数。
    std::size_t endLine{0};    // Exclusive end of the source span; 0 = fall back to line.
    // 错误片段跨多行时的结束行；0 表示退化成单行错误。
    std::size_t endColumn{0};  // One past the last column of the source span; 0 = fall back to column.
    // 结束列是“最后一列的下一列”，也就是左闭右开区间。
};

std::string errorType(ErrorCode code);
// 把错误码翻译成字符串名，例如 2001 转成 LexicalError；实现在 error.cpp。

class MiniSqlError : public std::runtime_error {
// 继承标准库的 runtime_error，因此能被 catch (const std::exception&) 接住。
public:
    MiniSqlError(ErrorCode code, std::string message,
                 SourceLocation location = {}, std::string suggestion = {},
                 std::string actual = {}, std::vector<std::string> expected = {});
    // 构造函数；后四个参数都有默认值，调用方只给错误码和消息也能编译通过。
    ErrorCode code() const noexcept { return code_; }
    // 取错误码；noexcept 表示这个取值函数保证不抛异常。
    SourceLocation location() const noexcept { return location_; }
    // 取位置信息。
    const std::string& suggestion() const noexcept { return suggestion_; }
    // 取修复建议，例如 Did you mean: age?
    const std::string& actual() const noexcept { return actual_; }
    // 取实际读到的符号，对应诊断输出里的 actual 字段。
    const std::vector<std::string>& expected() const noexcept { return expected_; }
    // 取期望符号列表，对应诊断输出里的 expected 字段。
    nlohmann::json toJson() const;
    // 把整个错误打包成 JSON；实现在 error.cpp。
private:
    ErrorCode code_;
    // 错误码，决定输出 JSON 里的 type 与 code。
    SourceLocation location_;
    // 位置，决定输出 JSON 里的 line 与 column。
    std::string suggestion_;
    // 修复建议。
    std::string actual_;
    // 实际读到的符号。
    std::vector<std::string> expected_;
    // 期望符号列表。
};

class Status {
// 轻量级结果：只有错误码和消息，不带位置，用在不需要定位的场合。
public:
    Status() = default;
    // 默认构造：错误码是 Ok，表示成功。
    Status(ErrorCode code, std::string message);
    // 带错误码与消息的构造；实现在 error.cpp，内部会校验状态自洽。
    bool ok() const noexcept { return code_ == ErrorCode::Ok; }
    // 判断是否成功。
    ErrorCode code() const noexcept { return code_; }
    // 取错误码。
    const std::string& message() const noexcept { return message_; }
    // 取消息文本。
    void throwIfError() const;
    // 如果处于失败状态，就升级成 MiniSqlError 抛出去。
private:
    ErrorCode code_{ErrorCode::Ok};
    // 成员默认值：默认就是成功。
    std::string message_;
    // 失败时携带的说明文本。
};

template <typename T>
class Result {
// Result<T>：要么装一个 T 类型的值，要么装一个失败的 Status，二者只能有其一。
public:
    explicit Result(T value) : state_(std::move(value)) {}
    // 成功路径：把值直接装进 variant。
    explicit Result(Status error) : state_(std::move(error)) {
        if (std::get<Status>(state_).ok()) {
            // 禁止装入“成功”的 Status，否则状态自相矛盾。
            throw std::invalid_argument("An error Result cannot contain OK status");
        }
    }
    // 失败路径：装入 Status，并做一次自洽性校验。
    bool ok() const noexcept { return std::holds_alternative<T>(state_); }
    // 判断当前装的是值还是错误。
    const T& value() const {
        if (!ok()) { std::get<Status>(state_).throwIfError(); }
        // 如果装的是错误，这一行会抛出异常，因此下一行必定拿得到值。
        return std::get<T>(state_);
    }
    // 只读取值。
    T& value() {
        if (!ok()) { std::get<Status>(state_).throwIfError(); }
        // 同上：先抛错再取值，保证不会读到不存在的值。
        return std::get<T>(state_);
    }
    // 可写取值。
    Status status() const { return ok() ? Status{} : std::get<Status>(state_); }
    // 成功时返回默认的成功状态，失败时返回内部的错误。
private:
    std::variant<T, Status> state_;
    // 二选一容器：同一时刻只保存其中一种。
};

} // namespace minisql
