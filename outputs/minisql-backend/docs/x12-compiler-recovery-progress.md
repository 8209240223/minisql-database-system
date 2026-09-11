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

- `tests/diagnostics-smoke.mjs`：**31 项**（多错误 + 合法语句混合、字符串内分号、invalid AST 拒收、位置逐字符一致；含 EOF 位置断言）。

## 边界

- **EOF 处错误已带真实位置**（本轮修正，§6.23-b）：诊断路径不把 END token 入队，此前解析器耗尽 token 流会回落 `SourceLocation{}` → `0:0`。现 `Parser::eofLocation()` 取最后一个 token 的 `endLocation`（其右边界＝输入末尾），`take()/expect()/semicolon()` 三处回落点改用它。示例：`SELECT ` → `1:7`；`... WHERE (id > 1` → `1:31`；多行 `WHERE (\n` → `3:8`。
- 仍会回落 `0:0` 的仅剩**无对应 token 位置**的语义报错（如 `PRIMARY KEY cannot declare NULL`）；调用方应按"无跨度"处理。
- 错误码表与协议全文见 [compiler-error-codes.md](compiler-error-codes.md)。
