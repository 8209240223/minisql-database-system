#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>

namespace minisql::execution {

class RowStream {
// RowStream 是所有算子的统一读取接口：上层只负责一行一行地取，不关心下面是扫描、连接还是聚合。
public:
// 公开接口，供执行层内部的各个算子实现类继承。
    virtual ~RowStream() = default;
    // 虚析构：通过基类指针删除派生算子时，必须能正确释放派生类持有的资源。
    virtual bool next(nlohmann::json& row) = 0;
    // 取下一行：取到返回 true 并把该行写进 row；已经取完返回 false。
    virtual void cancel() = 0;
    // 请求取消：排序、连接这类长跑算子据此尽快停下并释放资源。
    virtual void close() = 0;
    // 关闭并释放资源；执行结束后必须调用，保证文件句柄与临时文件被回收。
    virtual nlohmann::json resourceUsage() const = 0;
    // 汇报本算子占用的资源（行数、内存、溢出到磁盘的字节数等），供可观测性与结果预算控制使用。
};

} // namespace minisql::execution
