# 编译器错误码表与多诊断协议（X12）

> 成员 A 交付物（任务书 A5-③）。权威定义见 `include/minisql/common/error.hpp`（`enum class ErrorCode`），
> 诊断装配见 `src/execution/database.cpp::diagnostics()`。

## 一、错误码表

| 码 | 名称 | 诊断 `stage` | 典型来源 |
| --- | --- | --- | --- |
| 0 | Ok | — | 成功占位 |
| 1001 | InvalidArgument | internal | 参数非法（API 边界） |
| 1002 | Configuration | internal | 配置非法/缺失 |
| 2001 | Lexical | lexer | 非法字符、未闭合字符串/注释、非法数字 |
| 2002 | Syntax | parser | 语法错误；表达式深度超限（> `kMaxExpressionDepth = 128`） |
| 2003 | Semantic | semantic | 未绑定列/表、类型不符、未分组列 |
| 3001 | Catalog | semantic | 目录解析失败（未知表/列） |
| 4001 | Storage | internal | 页/文件/序列化错误 |
| 5001 | Execution | internal | 执行期错误（标量子查询多行、不支持的 join 输入等） |
| 5002 | Cancelled | internal | 请求被取消 |
| 6001 | Transaction | internal | 事务状态错误 |
| 7001 | Permission | internal | 权限拒绝 |
| 8001 | Network | internal | 传输层错误 |
| 9001 | NotImplemented | planner | 编译期预留的未实现阶段 |
| 9999 | Internal | internal | 未分类内部错误 |

`stage` 派发规则：`2001 → lexer`；`2002 → parser`；`2003/3001 → semantic`；`9001 → planner`；其余 `→ internal`。

## 二、多诊断协议

单次请求返回的诊断列表（一次编译可含多条独立诊断）：

```json
{
  "success": false,
  "count": 2,
  "diagnostics": [
    {
      "success": false,
      "stage": "parser",
      "code": 2001,
      "message": "Expected expression",
      "line": 1,
      "column": 15,
      "endLine": 1,
      "endColumn": 15,
      "recoverable": true,
      "statementIndex": 0
    }
  ]
}
```

字段语义：

- `endLine` / `endColumn`：源码跨度结束（右开）。缺失时回落 `line` / `column`；完全无位置信息时回落 `0:0-0:0`（编辑器中不可定位，调用方应视作"无跨度"）。
- `statementIndex`：0 基语句序号，标识错误所属语句（多语句输入）。
- `recoverable`：词法/语法错误在 token 级同步后可继续解析后续语句时为 `true`。
- `success`：单条诊断恒为 `false`；顶层 `success` 表示整批是否全部编译成功。

## 三、恢复语义

- **Lexer**：首个词法错误不中断 `scanTokens`，记录诊断后跳到下一个稳定 token；字符串内的 `;` 不切分语句。
- **Parser**：同步集合为 `;`、`)`、语句关键字（SELECT / INSERT / UPDATE / DELETE / CREATE / DROP / …）与查询块边界；到达同步点后可继续收集本语句的后续诊断。
- **Planner 拒收**：AST 含任何 `invalid` 节点即拒绝，不产出可执行计划、不产出空计划或默认节点。
- **表达式深度**：上限 `kMaxExpressionDepth = 128`（递归下降约 8 帧/层，1 MiB 线程栈约 190 层耗尽）；超限返回 `2002 Expression depth exceeded`，且**每条语句重置深度计数**，避免恢复回绕误判后续语句。
- **不执行猜测语句**：恢复得到的"纠错后 SQL"不会提交给执行器；已成功编译的语句也不因后续语句报错而被偷偷执行。
