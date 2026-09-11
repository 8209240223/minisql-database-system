#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>

namespace minisql::execution {

class RowStream {
public:
    virtual ~RowStream() = default;
    virtual bool next(nlohmann::json& row) = 0;
    virtual void cancel() = 0;
    virtual void close() = 0;
    virtual nlohmann::json resourceUsage() const = 0;
};

} // namespace minisql::execution
