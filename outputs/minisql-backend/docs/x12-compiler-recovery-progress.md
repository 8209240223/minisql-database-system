# X12 编译器错误恢复 —— progress

> 成员 A 交付物（任务书 A5-①）。设计目标见任务书 A1；执行记录见 `task-A-development-plan.md` §6.2。

## 状态：已完成

## 已实现

- **Token 级位置**：`Token` / `SourceLocation` 携带 `line` / `column` / `endLine` / `endColumn`（右开跨度），缺失时回落。
- **Lexer 恢复**：非法字符、未闭合字符串/注释、非法数字各记一条 `2001` 诊断后跳到稳定 token，不再遇错即抛；字符串内的 `;` 不切分语句。
- **Parser 多诊断 + 同步集合**：`;`、`)`、语句关键字（SELECT / INSERT / UPDATE / DELETE / CREATE / DROP / …）与查询块边界；单语句可产出多条独立诊断。
- **invalid 标记 + Planner 拒收**：错误处生成带 `invalid` 的节点/Statement；AST 含 invalid 即拒，不产出可执行计划。
- **多诊断协议**：`diagnostics()` 输出 `success` / `stage` / `code` / `message` / `line` / `column` / `endLine` / `endColumn` / `recoverable` / `statementIndex`；服务端透传，工作台按 `statementIndex` 定位。
- **深度防护**：表达式深度上限 `kMaxExpressionDepth = 128`，超限返回 `2002`；每语句重置深度计数。

## 验证

- `tests/diagnostics-smoke.mjs`：22 项（多错误 + 合法语句混合、字符串内分号、invalid AST 拒收、位置逐字符一致）。

## 边界

- 无位置信息的诊断回落 `0:0-0:0`（编辑器中不可定位）；调用方应按"无跨度"处理。
- 错误码表与协议全文见 [compiler-error-codes.md](compiler-error-codes.md)。
