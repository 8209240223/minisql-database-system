# X13 版本化与 Schema 迁移 —— progress

> 成员 A 交付物（任务书 A5-①）。设计目标见任务书 A2；执行记录见 `task-A-development-plan.md` §6.3、§6.20。

## 状态：编解码契约已完成；响应接入待跨组同步

## 已实现

- **AST / Plan 版本化**：文档写入 `schemaVersion` / `schemaMinor` / `nodeVersion`（AST）或 `planVersion`（Plan）/ `producerVersion`。
- **读端闸门**：未知主版本拒绝；同主版 `minor <= current` 兼容读；更高次版本拒绝。详见 [ast-plan-schema-versioning.md](ast-plan-schema-versioning.md)。
- **稳定对象身份**：列以 `columnId` 标识；表/列/约束结构不依赖数组下标。
- **Catalog 迁移契约**：`CATALOG_SCHEMA_VERSION = 6`；新列描述符使用稳定数值 `typeId` 与参数对象，旧 v1-v4 描述符继续兼容读取，未知 ID 按损坏拒绝；`migrationPlan(fromVersion)` 暴露可检查的迁移步骤和中断恢复。
- **未知节点拒绝**：新 AST 节点交给旧 Planner 时明确报错，不产出空计划或默认节点。

## 验证

- `tests/planner_contract.cpp`：AST/Plan 版本化往返、未知主版拒绝、更高次版拒绝、`nodeVersion`/`planVersion` 未知主版拒绝、兼容读、writer 字段存在。
- `catalog_migration_contract`（ctest）：迁移链、中断恢复、未知版本拒绝、非正版本判 corruption。
- `tests/backup-smoke.mjs`：迁移链与失败回滚后库可打开。

## 边界

- `compile` 响应当前仍输出 `{"schemaVersion": 1}`；把 `nodeVersion` / `planVersion` 接入响应是**跨组接口变更**，须与 B/C 同步前端 `types.ts` 后进行（任务书 8.1 接口冻结）。
