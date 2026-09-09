# MiniSQL 访问控制与审计进度

## 本轮实现

- 新增 `scripts/access-catalog.mjs`：持久化访问目录（`access.catalog.json`），内置 `admin/reader/writer` 与对应角色；用户、角色、角色继承、用户级/角色级对象授权均可配置。
- 密码不再明文落盘：创建用户时生成随机盐，保存 `sha256-salted` 摘要；`GET /api/access` 与 `GET /api/users` 不返回散列和明文密码。
- HTTP 请求按 `X-MiniSQL-User` / `X-MiniSQL-Password` 建立主体身份，未知用户或密码错误统一返回 403。
- 会话创建时绑定 `requestUser`；后续 execute/compile/diagnostics/catalog/statistics/close/cancel 都要求身份与会话一致，跨会话身份复用返回 403。
- 对象级权限检查：SELECT/INSERT/UPDATE/DELETE/DROP 按 SQL 中解析出的表名检查对应授权，CREATE 检查全局或对象级 CREATE；BEGIN/COMMIT/ROLLBACK/CHECKPOINT 需要 TRANSACTION。
- `catalog` 与 `statistics` 返回前按当前主体的 SELECT 权限过滤表，无权表不会出现在可见元数据中。
- `PUT /api/access` 供具备 GRANT 的主体整体更新访问目录；授权撤销后下一条请求立即生效，不依赖缓存计划。
- 审计增加 `object` 字段，并提供 `user`、`sessionId`、`object`、`from`、`to` 过滤；访问目录响应不暴露密码散列。
- capabilities 暴露 `permissionsModel`、`objectPermissions`、`roleInheritance`、`sessionIdentity`、`passwordHashing`、`auditFiltering`。

## 验证

`node tests/access-control-http.mjs`：37 项检查通过，覆盖角色继承、对象级 SELECT/INSERT/UPDATE/DELETE、撤权即时生效、错误密码、跨会话身份、诊断不泄露不可见表、catalog/statistics 元数据过滤和审计过滤。

既有回归：

- `observability-http.mjs` 42 项通过（reader/writer/unknown 改为独立会话身份）。
- `database-http.mjs` 全套通过。
- `session-http.mjs`、`session-process.mjs`、`cancel-smoke.mjs`、`backup-smoke.mjs` 通过。

## 限制

- 当前访问目录是数据库目录旁的版本化 JSON 文件，尚未作为用户表存入页式 Catalog。
- CLI 直接调用 `minisql_database.exe` 时仍未强制身份校验；目前权限检查集中在 HTTP bridge，面向用户的入口是工作台 HTTP API。
- 前端还没有用户/角色/对象授权管理界面，也没有按当前身份展示会话列表。
- SQL 表名识别是轻量正则扫描，尚未覆盖派生表、CTE 和复杂子查询的精确对象身份；DELETE 中的子查询也可能被保守地要求主表的 DELETE 权限。
- 管理接口采用整体 `PUT /api/access`，尚未提供逐项 GRANT/REVOKE 的原子资源接口。
