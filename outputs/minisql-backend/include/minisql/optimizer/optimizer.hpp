#pragma once
#include "minisql/sql/planner.hpp"
#include <unordered_map>

namespace minisql::optimizer {
struct Options {
// 优化开关集合：每一项对应一条或一组规则，默认全部打开。
    bool constantComparison = true;
    // 常量比较折叠：把常量之间的比较直接算成 true 或 false。
    bool booleanSimplification = true;
    // 布尔表达式化简：处理双重否定、吸收律这类恒等变换。
    bool removeTrueFilter = true;
    // 删除恒真过滤：WHERE true 这类过滤条件直接去掉。
    bool removeFalseFilter = true;
    // 删除恒假过滤：WHERE false 直接改写成"不返回任何行"。
    bool predicatePushdown = true;
    // 谓词下推：把过滤条件尽量推到扫描或连接下方，减少中间结果。
    bool hashJoin = true;
    // 哈希连接：把等值连接改写成哈希连接，避免嵌套循环。
    bool pruneColumns = true;
    // 列裁剪：只读取真正用到的列，减少 I/O。
    bool constantArithmetic = true;
    // 常量算术折叠：把常量参与的算术表达式提前算出来。
    bool decorrelateSubquery = true;
    // 子查询去关联：把相关子查询改写成可独立执行的连接或聚合形式。
    double defaultTableRows = 1000.0;
    std::size_t memoryBudgetBytes = 64 * 1024 * 1024;
    std::unordered_map<std::string, double> tableRows{};
    std::size_t maxIterations = 16;
    // 优化主循环的最大迭代轮数，防止规则互相激发造成死循环。
    std::size_t maxNodes = 65536;
    // 计划节点规模上限，超过就停止优化，避免计划无限膨胀。
    std::vector<std::string> disabledRules{};
    // 显式禁用某些规则的名单，按规则名匹配。
};
// 选项结构结束。
struct Result {
// 优化结果：既给出改写后的计划，也给出可观测的改写记录。
    std::vector<sql::LogicalPlan> plans;
    // plans 是优化后的计划列表。
    nlohmann::json changes = nlohmann::json::array();
    // changes 记录每一处改写（规则名、位置、前后差异）。
    std::size_t iterations = 0;
    // iterations 记录实际跑了几轮优化。
    bool converged = false;
    // converged 表示是否在迭代上限内达到不动点。
    nlohmann::json diagnostics = nlohmann::json::array();
    // diagnostics 是优化过程产生的诊断信息。
};
// 结果结构结束。
nlohmann::json ruleDescriptors();
// 返回所有可用优化规则的描述（规则名、说明、默认是否开启），供 CLI 展示。
Result optimize(const std::vector<sql::LogicalPlan>& plans, Options options = {});
// 优化主入口：输入逻辑计划与选项，返回优化结果。
}
