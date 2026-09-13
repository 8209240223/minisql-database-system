# AST / Plan / Schema 版本迁移说明（X13）

> 成员 A 交付物（任务书 A5-②）。权威常量与读端闸门见 `include/minisql/sql/serialization.hpp`；
> 计划文档读写见 `src/sql/planner.cpp`。

## 一、版本常量

| 常量 | 值 | 作用 |
| --- | --- | --- |
| `AST_SCHEMA_VERSION` | 1（主） | AST 文档 `schemaVersion` |
| `AST_SCHEMA_MINOR` | 0 | 本二进制写出的最新 AST 次版本 |
| `AST_NODE_VERSION` | 1（主） | 语句/表达式节点契约 `nodeVersion` |
| `PLAN_SCHEMA_VERSION` | 1（主） | 逻辑计划文档 `schemaVersion` |
| `PLAN_SCHEMA_MINOR` | 0 | 本二进制写出的最新计划次版本 |
| `PLAN_VERSION` | 1（主） | 计划节点契约 `planVersion` |
| `PRODUCER_VERSION` | 1 | 写出方二进制版本 |
| `CATALOG_SCHEMA_VERSION` | 6 | 最新表/列描述符版本；列类型使用稳定数值 `typeId` |

## 二、读端闸门（统一规则）

`versionReadable(documentMajor, documentMinor, currentMajor, currentMinor)`：

- 主版本**不等** → 拒绝（`Unsupported AST schema version` / `Invalid serialized logical plan`）。
- 主版本相等且 `documentMinor <= currentMinor` → 接受（次版本向后兼容读；缺省的可选字段回落默认）。
- 主版本相等但 `documentMinor > currentMinor` → 拒绝（读不出更新次版本引入的字段形状）。

配套辅助：

- `readVersionField(node, field, out)`：接受 JSON **有符号/无符号**整数，拒绝缺失/错型/负值/越界。
- `optionalVersionReadable(node, field, major, minor)`：字段缺省或为 `null` 时视为最旧可读形态（接受）；存在则须过闸门。

## 三、文档外形

AST：

```json
{"schemaVersion": 1, "schemaMinor": 0, "nodeVersion": 1, "producerVersion": 1, "statements": []}
```

Plan：

```json
{"schemaVersion": 1, "schemaMinor": 0, "planVersion": 1, "producerVersion": 1, "planKind": "logical", "plans": []}
```

- 旧文档 `{"schemaVersion": 1, "statements": [...]}`（无 `schemaMinor` / `nodeVersion`）**仍可读**：次版本按 0、节点版本视为兼容。
- 写入函数：`serializeAstDocument(statements)`（AST）、`serializePlanDocument(plans)`（Plan）；读取函数：`deserializeAst`、`deserializePlans`。

## 四、迁移策略

- **加字段（向后兼容）**：提升 `*_SCHEMA_MINOR`。旧读者因 `minor > currentMinor` 拒绝更新的文档；新读者对旧文档按缺省回落读取。
- **改语义 / 删字段（不兼容）**：提升主版本。旧读者一律拒绝；新读者拒绝旧主版本，须提供显式迁移路径。
- **Catalog 迁移**：`CATALOG_SCHEMA_VERSION` 由 `persistent_catalog` 管理，提供 `migrationPlan(fromVersion)` 与中断恢复（`pendingMigration` / `recovered`）。迁移**不得**静默改变列类型、NULL 语义、约束或索引定义；失败后原库保持可打开。
- **跨组接入**：`compile` 响应当前仍输出 `{"schemaVersion": 1}`；把 `nodeVersion` / `planVersion` 接入响应属**接口变更**，须与 B/C 同步前端 `types.ts` 后进行（任务书 8.1 接口冻结）。本提交只完成编解码契约，不动响应结构。

## 五、验证

`tests/planner_contract.cpp`：版本化往返、未知主版拒绝、更高次版拒绝、`nodeVersion`/`planVersion` 未知主版拒绝、同主版次版兼容读。
