# MiniSQL 访问控制与审计进度

## 本轮实现

- 新增 `scripts/access-catalog.mjs`：持久化访问目录（`access.catalog.json`），内置 `admin/reader/writer` 与对应角色；用户、角色、角色继承、用户级/角色级对象授权均可配置。
- **新增原子资源接口（2026-09-09 补）**：`scripts/access-catalog.mjs` 提供 `createUser`、`dropUser`、`createRole`、`dropRole`、`setPassword`、`addRole`、`removeRole`、`grant`、`revoke` 等纯函数原子操作。全部先克隆再校验、最终返回新对象，任何校验失败都不产生部分变更，满足 X24「逐项 GRANT/REVOKE 原子资源接口」的要求。
- **新增独立 HTTP 资源端点（2026-09-09 补）**：`scripts/database-bridge.mjs` 把原子接口映射为 `POST /api/users`、`DELETE /api/users/:name`、`POST /api/users/:name/password`、`POST /api/users/:name/roles`、`DELETE /api/users/:name/roles/:role`、`POST /api/roles`、`DELETE /api/roles/:name`、`POST /api/grants`、`POST /api/revokes`，全部要求 `GRANT` 权限，失败返回 4xx 且不落盘部分状态。CORS 允许 DELETE。
- **启动预热弹性（2026-09-09 补）**：引擎 Catalog 预热若因二进制缺失/损坏失败，服务器仍进入降级模式（健康检查报 degraded、引擎路由返回 503），使不依赖引擎的 access/audit/capabilities 管理端点保持可用。
- **工作台权限与审计面板（2026-09-09 补）**：`minisql-workbench/src/AccessControl.tsx` 新增用户/角色/会话/审计面板；`client.ts` 与 `types.ts` 补齐 access/audit/session 类型与客户端函数；`App.tsx` 顶栏新增「权限与审计」入口。
- 密码不再明文落盘：创建用户时生成随机盐，保存 `sha256-salted` 摘要；`GET /api/access` 与 `GET /api/users` 不返回散列和明文密码。
- HTTP 请求按 `X-MiniSQL-User` / `X-MiniSQL-Password` 建立主体身份，未知用户或密码错误统一返回 403。
- 会话创建时绑定 `requestUser`；后续 execute/compile/diagnostics/catalog/statistics/close/cancel 都要求身份与会话一致，跨会话身份复用返回 403。
- 对象级权限检查：SELECT/INSERT/UPDATE/DELETE/DROP 按 SQL 中解析出的表名检查对应授权，CREATE 检查全局或对象级 CREATE；BEGIN/COMMIT/ROLLBACK/CHECKPOINT 需要 TRANSACTION。
- `catalog` 与 `statistics` 返回前按当前主体的 SELECT 权限过滤表，无权表不会出现在可见元数据中。
- `PUT /api/access` 供具备 GRANT 的主体整体更新访问目录；授权撤销后下一条请求立即生效，不依赖缓存计划。
- 审计增加 `object` 字段，并提供 `user`、`sessionId`、`object`、`from`、`to` 过滤；访问目录响应不暴露密码散列。
- capabilities 暴露 `permissionsModel`、`objectPermissions`、`roleInheritance`、`sessionIdentity`、`passwordHashing`、`auditFiltering`。
- **C++ 引擎入口鉴权（2026-09-10 补）**：新增 `src/security/access_catalog.cpp` 页式目录读取器，真实 `minisql_database.exe` 的 session 和直连 `execute/compile/diagnostics/statistics/catalog` 均在执行前校验用户、密码和对象权限；session 在每个请求前按 `permissionVersion` 热重载权限页，避免权限修改后继续使用旧快照。bridge 关闭共享 worker 时携带最后一次已验证的会话身份。
- **索引检查调用链（2026-09-10 补）**：bridge 将 `table`/`index` 转发到 C++ session，权限 HTTP 回归同步覆盖真实索引检查响应。

## 验证

`node tests/access-control-http.mjs`：42 项检查通过，覆盖角色继承、对象级 SELECT/INSERT/UPDATE/DELETE、索引检查、嵌套子查询对象识别、字符串字面量误报防护、撤权即时生效、错误密码、跨会话身份、诊断不泄露不可见表、catalog/statistics 元数据过滤和审计过滤。

`node tests/access-control-process.mjs`：18 项检查通过，覆盖 C++ session 的正确/错误密码、对象级读写授权、嵌套子查询对象识别、字符串字面量误报防护、关闭握手，以及直连二进制的身份校验。

`node tests/access-catalog-atomic-contract.mjs`：27 项检查通过（2026-09-09 新增），覆盖用户/角色增删、继承环检测、对象级 GRANT/REVOKE、改密、加删角色、重复授权并集、撤销不存在对象幂等与非法输入不产生部分变更。

`node tests/access-atomic-http.mjs`：39 项检查通过（2026-09-09 新增，不依赖 C++ 引擎），覆盖原子 HTTP 端点的建用户/建角色、对象级授权/撤销、角色绑定/解绑、改密、删除用户、缺 GRANT 主体 403、重复创建 400 且状态不变、删除不存在用户 400。

工作台真实浏览器回归：

- `npm.cmd run test:browser` 新增 `c2-resilience-dom.cjs`，在隔离数据库上验证成功查询、语义失败、活动大结果集取消、全量备份、恢复和恢复后查询；测试通过，不生成或读取图片。

既有回归：

- `observability-http.mjs` 42 项通过（reader/writer/unknown 改为独立会话身份）。
- `database-http.mjs` 全套通过。
- `session-http.mjs`、`session-process.mjs`、`cancel-smoke.mjs`、`backup-smoke.mjs` 通过。

## 限制

- 当前访问目录是数据库目录旁的版本化页文件（`access.catalog.pages`），C++ 引擎已经能直接校验并按权限版本热重载，但它尚未作为用户表存入现有 `PersistentCatalog`；旧版 `access.catalog.json` 仅用于迁移读取。权限对象识别已改为跳过注释、字符串、CTE 别名和派生表的词法扫描，但还不是完整 AST 绑定。
- 权限感知 CLI（`scripts/minisql-cli.mjs`）通过 HTTP bridge 强制携带身份；直接调用 `minisql_database.exe` 也已支持 `MINISQL_USER` / `MINISQL_PASSWORD` 身份校验，但这组环境变量只适合作为受控本地入口，不替代后续正式登录协议。
- SQL 表名识别已从正则升级为跳过注释/字符串、识别嵌套 FROM/JOIN/INTO/UPDATE/REFERENCES、排除 CTE 别名和派生表的轻量词法扫描；复杂 SQL 仍需完整 AST 对象绑定，DELETE 中的子查询也可能被保守地要求主表的 DELETE 权限。
- 工作台权限/审计面板已接入（`AccessControl.tsx`），请求会携带当前连接的用户和密码；多会话面板已列出会话、锁等待和持锁状态，并提供活动请求取消。`c2-resilience-dom.cjs` 已纳入基础浏览器 DOM、移动端无溢出、客户端超时、活动请求取消、备份替换、迁移校验成功/失败、损坏备份失败隔离和恢复后查询验收。
