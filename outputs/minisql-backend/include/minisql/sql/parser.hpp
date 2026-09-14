#pragma once
#include "minisql/sql/lexer.hpp"
#include <memory>
#include <variant>
#include <optional>
#include <cstdint>

namespace minisql::sql {
struct Statement;
// 前置声明：Expr 里要保存指向子查询语句的指针，而 Statement 定义在后面。
// X09：Expr 既可以携带原始 SQL 文本（subquerySql，过渡期产物），也可以携带
// 结构化的查询节点（subquery）。向"结构化对象标识"的迁移正在进行中；
// subquerySql 会一直保留到 planner 与执行层（任务 3.3-3.5）真正消费结构化节点为止，之后删除。
// X09: Expr carries either the raw SQL text (subquerySql, interim) or a
// structured query node (subquery). Migration to structured object identity is
// underway; subquerySql stays until planner/execution (3.3-3.5) consume the
// structured node, then it is removed.
struct Expr { std::string kind; std::string value; std::shared_ptr<Expr> left; std::shared_ptr<Expr> right; SourceLocation location{}; std::string subquerySql{}; std::shared_ptr<Statement> subquery{}; };
// 表达式节点，语法树里所有"算得出来的东西"都用它表示。
// kind 是节点种类，如 Literal、Identifier、BinaryExpr、AggregateExpr、Default。
// value 是节点的主要文本，如列名、字面量原文、聚合函数名。
// left / right 是左右子节点，构成二叉树形状的表达式树。
// location 记录该表达式在源语句里的位置，报语义错误时用它定位。
// subquerySql 是子查询的原始文本（过渡形态，迁移完成后删除）。
// subquery 是子查询的结构化语法树，planner/执行层后续按它做对象身份比较。
struct ColumnDef { std::string name; std::string type; bool nullable = true; std::optional<std::string> defaultValue{}; bool primaryKey = false; bool unique = false; std::optional<std::pair<std::string,std::string>> references{}; };
// 建表语句里的一列定义。
// name 是列名；type 是列类型原文（如 INT、VARCHAR(20)、DECIMAL(10,2)）。
// nullable 表示是否允许 NULL，默认允许。
// defaultValue 是 DEFAULT 子句的原文，没有 DEFAULT 时为空。
// primaryKey / unique 是列级的 PRIMARY KEY、UNIQUE 标记。
// references 是列级外键，first 是父表名，second 是父表列名。
struct SelectItem { std::shared_ptr<Expr> expression; std::string alias; };
// 投影列表里的一项。
// expression 是投影表达式；alias 是 AS 后面的别名（没写就是空串）。
struct OrderItem { std::shared_ptr<Expr> expression; bool descending = false; std::optional<bool> nullsFirst{}; };
// ORDER BY 里的一项。
// expression 是排序键；descending 表示是不是降序。
// nullsFirst 记录 NULLS FIRST / NULLS LAST 的显式指定，未写时为空。
struct Assignment { std::string column; std::shared_ptr<Expr> expression; };
// UPDATE 里的一条赋值：column = expression。
struct Join { std::string table; std::string alias; std::shared_ptr<Expr> on; bool left = false; bool right = false; bool cross = false; };
// 一个连接子句。
// table 是被连接的表名；alias 是它的别名。
// on 是连接条件表达式；left / right 标记是左外连接还是右外连接。
// cross 标记这是 CROSS JOIN 或逗号连接：没有 ON 条件，语义是笛卡尔积。
struct KeyConstraint { bool primary = false; std::vector<std::string> columns{}; };
// 表级键约束：primary 为真表示 PRIMARY KEY，否则表示 UNIQUE；
// columns 是构成该键的列名序列，顺序有意义（复合键的前缀可独立走索引）。
struct ForeignKey { std::vector<std::string> columns{}; std::string table{}; std::vector<std::string> referencedColumns{}; };
// 表级外键：本表的 columns 依次引用 table 表的 referencedColumns。
struct ConstraintName { std::string name; std::string kind; std::size_t index; };
// CONSTRAINT 名字绑定：给某类约束的第 index 个目标起名 name。
// kind 取值如 key / check / foreignKey / primaryKey / unique / references / notNull。
struct IndexDef { std::string name; std::vector<std::string> columns; bool unique = false; };
// 一条索引定义：索引名、索引列、是否唯一索引。
// X25: `WITH name [(columns)] AS ( SELECT ... )`. A CTE name is a scope name, not a
// database object: the binder resolves references to it inside the statement scope and
// never emits an access object for it. Recursive CTEs are rejected by the parser.
struct CommonTableExpr { std::string name; std::vector<std::string> columns{}; std::shared_ptr<Statement> query{}; SourceLocation location{}; };
struct Statement {
// 一条语句的完整语法树，整个解析阶段产出的元素都是它。
    std::string kind{};
    // 语句种类，如 CreateTable / Insert / Select / Update / Delete / CreateIndex。
    std::string table{};
    // 本条语句操作的主表名。
    std::vector<ColumnDef> columns{};
    // 建表时的列定义列表。
    std::vector<std::string> names{};
    // 列名列表，INSERT 的目标列、UPDATE 被赋值的列都放在这里。
    std::vector<std::string> values{};
    // 字面量文本形式的值列表（与 valueExpressions 二选一）。
    std::vector<std::string> selectList{};
    // 字符串形式的投影列表，用于兼容把投影按文本保存的老路径。
    std::shared_ptr<Expr> where{};
    // WHERE 条件表达式，没有 WHERE 时为空。
    SourceLocation location{};
    // 本语句在源文本中的起始位置，语义错误按它定位。
    std::vector<SelectItem> selectItems{};
    // 结构化投影列表，是 SELECT 的主表示。
    bool distinct = false;
    // 是否写了 SELECT DISTINCT。
    std::optional<std::uint64_t> limit{};
    // LIMIT 的条数；没写 LIMIT 时为空，区别于写了 LIMIT 0。
    std::uint64_t offset = 0;
    // OFFSET 的偏移量，默认从第 0 条开始。
    std::vector<OrderItem> orderBy{};
    // ORDER BY 排序键列表，按书写顺序依次生效。
    std::vector<Assignment> assignments{};
    // UPDATE 的赋值列表。
    std::string tableAlias{};
    // 主表的别名，未写时为空。
    std::vector<Join> joins{};
    // 本条语句里所有 JOIN 子句，按书写顺序排列。
    // X09：FROM 派生表 `( SELECT ... ) AS alias` —— 结构化子查询节点 + 显式别名。
    // X09: FROM 派生表 `( SELECT ... ) AS alias` —— 结构化子查询节点 + 显式别名。
    std::shared_ptr<Statement> fromSubquery{};
    // FROM 位置写的是子查询时，这里保存内层 SELECT 的语法树；为空表示 FROM 是普通表。
    // X25: WITH 子句引入的公共表表达式，按书写顺序；后一个可以引用前一个。
    std::vector<CommonTableExpr> ctes{};
    std::vector<std::shared_ptr<Expr>> valueExpressions{};
    // 表达式形式的值列表，配合 INSERT 使用，支持 DEFAULT、表达式等非字面量形式。
    bool defaultValues = false;
    // 是否写了 INSERT ... DEFAULT VALUES。
    std::vector<std::vector<std::shared_ptr<Expr>>> valueRows{};
    // 多行 INSERT 的每一行，每个内层 vector 是一行的表达式序列。
    std::vector<KeyConstraint> keys{};
    // 表级 PRIMARY KEY / UNIQUE 约束列表。
    std::vector<std::shared_ptr<Expr>> checks{};
    // CHECK 约束表达式列表（列级与表级合并后按出现顺序存放）。
    std::vector<ForeignKey> foreignKeys{};
    // 表级声明的外键列表。
    std::vector<ConstraintName> constraintNames{};
    // 用户显式起的约束名绑定列表。
    std::vector<std::shared_ptr<Expr>> groupBy{};
    // GROUP BY 分组键表达式列表。
    std::shared_ptr<Expr> having{};
    // HAVING 条件表达式，没有时为空。
    std::string indexName{};
    // CREATE INDEX / DROP INDEX 里的索引名。
    std::string savepointName{};
    // SAVEPOINT / RELEASE / ROLLBACK TO 里的保存点名。
    bool uniqueIndex = false;
    // 是否创建唯一索引。
    std::vector<std::string> indexColumns{};
    // CREATE INDEX 里的索引列名列表。
    std::vector<IndexDef> indexes{};
    // 建表语句里内联声明的索引定义。
    bool invalid = false;
    // 容错解析时，解析失败的语句会被打上这个标记，调用方据此整条拒绝而不中断整批。
};
inline std::string constraintSuffix(const std::vector<ConstraintName>& names, const std::string& kind, std::size_t index) {
// 在约束名绑定表里查"某一类约束的第 index 个目标"有没有起名字。
    for (const auto& binding : names)
    // 逐条比对绑定记录。
        if (binding.kind == kind && binding.index == index) return " [" + binding.name + "]";
        // 类别与下标都吻合就返回形如 " [pk_x]" 的后缀，拼在错误信息后面。
    return {};
    // 没有对应命名，返回空串，错误信息就不追加后缀。
}
inline std::string foreignKeySuffix(const Statement& statement, std::size_t index) {
// 外键既可能写在表级也可能写在列级，这里把两种来源统一编号后再取名字。
    if (index < statement.foreignKeys.size()) return constraintSuffix(statement.constraintNames, "foreignKey", index);
    // 编号落在表级外键范围内，直接按 foreignKey 类查名。
    index -= statement.foreignKeys.size();
    // 否则减去表级外键个数，把编号折算成"列级外键中的第几个"。
    for (std::size_t column = 0; column < statement.columns.size(); ++column)
    // 按列顺序扫描，列级外键的出现顺序就是它们被编号的顺序。
        if (statement.columns[column].references) {
        // 这一列确实声明了列级外键。
            if (index == 0) return constraintSuffix(statement.constraintNames, "references", column);
            // 折算后的编号归零，说明就是它，按 references 类取名字。
            --index;
            // 否则继续往后数。
        }
    return {};
    // 找不到对应约束名，返回空串。
}
inline std::vector<ForeignKey> allForeignKeys(const Statement& statement) {
// 把表级外键与列级外键合并成一个统一列表，供建表校验和元数据持久化使用。
    auto result = statement.foreignKeys;
    // 先放入表级声明的那些外键。
    for (const auto& column : statement.columns)
    // 再扫描每一列。
        if (column.references) result.push_back({{column.name}, column.references->first, {column.references->second}});
        // 列上写了 REFERENCES 就把它转成一条标准外键记录追加进去。
    return result;
    // 返回合并后的列表，顺序为"表级在前、列级在后"。
}
std::vector<Statement> parse(const std::vector<Token>& tokens);
// 严格模式入口：把 token 流解析成语句列表，遇到第一个语法错误就抛异常。
// 恢复模式的解析器：不在第一个语法错误处抛出，而是把所有可恢复的语法错误
// 收集进 errors（带 endLine/endColumn 区间），随后同步到下一条语句的边界，
// 并把出错的 Statement 标记为 invalid，让调用方可以单独拒绝这条语句而不
// 影响整批；合法的语句照常返回。
// Recovery-mode parser: instead of throwing on the first syntax error it
// collects every recoverable syntax error into `errors` (with
// endLine/endColumn spans), synchronizes to the next statement boundary, and
// marks the offending Statement invalid so callers can reject it without
// aborting the whole batch. Valid statements still come back.
std::vector<Statement> parseRecoverable(const std::vector<Token>& tokens, std::vector<MiniSqlError>& errors);
// 容错模式入口：返回全部能解析出的语句，错误集中放在 errors 里。
}
