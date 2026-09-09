# MiniSQL V3 成员 C 任务完成说明

日期：2026-09-09
对应：`outputs/MiniSQL_V3_未完成功能三人分工详细设计任务书.docx`（成员 C 部分）
基线：`outputs/MiniSQL扩展功能实施清单_V1.0.md`、`outputs/V3当前实现状态.md`、`outputs/minisql-backend/docs/*-progress.md`

---

## 一、成员 C 职责与任务分解

成员 C 负责**权限、审计、工作台、Fuzz 和最终验收**，不能绕过后端接口直接修改 A/B 的存储结构。

| 任务 | 编号 | 说明 |
| --- | --- | --- |
| C1 | X24 | 权限、角色与审计闭环（CLI/HTTP 共用鉴权、页式 Catalog、原子 GRANT/REVOKE、权限 UI） |
| C2 | — | 工作台最终收口（多会话、锁等待、取消、权限、审计、备份、资源错误展示） |
| C3 | X27 | DDL/DML Fuzz 与长期回归（状态机、固定种子语料、CI） |
| C4 | X01-X27 | 全量组合验收（feature matrix、最终报告） |

---

## 二、本次完成与修改内容

### C1 · X24 权限与审计

- **新增原子资源接口**（`outputs/minisql-backend/scripts/access-catalog.mjs`）：
  - `createUser` / `dropUser` / `createRole` / `dropRole`
  - `setPassword` / `addRole` / `removeRole`
  - `grant` / `revoke`（支持用户与角色主体、对象级授权、撤销全部授权）
  - 全部为**纯函数**：先克隆并规范化、再校验、最后返回新对象；任何校验失败都抛错且不产生部分变更，满足 X24「逐项 GRANT/REVOKE 原子资源接口」的要求。
  - 继承环、缺省角色、重复用户/角色、空权限集合等非法输入均显式拒绝。
- **新增契约测试**（`outputs/minisql-backend/tests/access-catalog-atomic-contract.mjs`）：27 项检查通过。
- **映射为独立 HTTP 资源端点**（`outputs/minisql-backend/scripts/database-bridge.mjs`）：
  - `POST /api/users`、`DELETE /api/users/:name`、`POST /api/users/:name/password`、`POST /api/users/:name/roles`、`DELETE /api/users/:name/roles/:role`
  - `POST /api/roles`、`DELETE /api/roles/:name`、`POST /api/grants`、`POST /api/revokes`
  - 全部要求 `GRANT` 权限，失败返回 4xx 且不落盘部分状态；CORS 允许 DELETE。
- **启动预热弹性**：引擎 Catalog 预热若因二进制缺失/损坏失败，服务器仍进入降级模式（健康检查报 degraded、引擎路由返回 503），使不依赖引擎的 access/audit/capabilities 管理端点保持可用。
- **新增集成测试**（`outputs/minisql-backend/tests/access-atomic-http.mjs`）：39 项检查通过（不依赖 C++ 引擎）。
- 更新 `outputs/minisql-backend/docs/access-control-progress.md`（本轮实现、验证、限制）。

### C3 · X27 Fuzz 与长期回归

- **新增 DDL/DML 状态机语料持久化**：`outputs/minisql-backend/tests/fuzz/generate-corpus.mjs`
  - 按固定种子生成并落盘：`fixture.sql`、`corpus-select-<seed>.sql`、`corpus-state-<seed>.sql`、`manifest.json`。
  - `manifest.json` 记录种子、样本数、各语料 SHA-256、生成器 SHA-256、fixture SHA-256 与生成时间，满足「固定种子长跑、CI 回归和失败样本重放」的可审计要求。
  - 状态机程序在持久化前用 `validateProgram` 重放校验，确保「构造即合法」。
- **生成固定种子语料**（种子 `20260908 / 42 / 7`，各 100 条 SELECT + 96 步状态机程序），`validateProgram` 全部通过。
- **新增 CI**：`.github/workflows/ci.yml`，含不依赖 C++ 引擎构建的后端成员 C 回归、工作台单元回归与语料复现一致性检查；重型引擎回归以自托管 job 注释形式保留。
- 更新 `outputs/minisql-backend/docs/fuzz-progress.md`（新增 X27 状态机语料章节、运行方式、验证结果与边界）。

### C2 · 工作台收口（权限与审计面板）

- **安装前端依赖**：`outputs/minisql-workbench` 执行 `npm install` 成功，依赖补全后 `test:safety`(22 项)/`test:csv`(21 项) 由失败转通过。
- **新增权限与审计面板**（`outputs/minisql-workbench/src/AccessControl.tsx`）：用户/角色/会话/审计四个标签页，支持建/删用户与角色、对象级授权/撤销、改密、绑定/解绑角色、会话列表、审计过滤。
- **客户端与类型**：`client.ts` 补齐 access/audit/session 客户端函数与原子端点调用；`types.ts` 新增 `AccessState`/`AccessUser`/`AccessRole`/`AuditEntry`/`SessionEntry` 类型。
- **入口**：`App.tsx` 顶栏新增「权限与审计」按钮并渲染面板；`styles.css` 新增面板样式（含深色适配）。
- 构建 `tsc -b && vite build` 通过；全部工作台单元/契约测试通过。

### C4 · X01-X27 全量验收（进度）

- 同步更新 `outputs/MiniSQL扩展功能实施清单_V1.0.md` 中 `EXT-SYS-005(X24)` 与 `EXT-QA-001(X27)` 状态行，反映本次原子接口、HTTP 端点、工作台面板与状态机语料/CI 进展。
- 各模块 progress 文档（成员 C 主责：`access-control-progress.md`、`fuzz-progress.md`、`workbench-*-progress.md`）已维护为当前实现与限制的对应说明。

---

## 三、验证结果（可复跑）

以下为本次在本地、无 C++ 引擎构建前提下运行的成员 C 纯 Node 契约测试，全部通过：

| 测试 | 说明 | 结果 |
| --- | --- | --- |
| `node tests/access-store-contract.mjs` | X24 页式权限目录：页布局、多页负载、损坏拒绝、原子写、JSON 迁移 | 48 项通过 |
| `node tests/access-catalog-atomic-contract.mjs` | X24 原子权限接口（本次新增） | 27 项通过 |
| `node tests/access-atomic-http.mjs` | X24 原子 HTTP 资源端点（本次新增，不依赖引擎） | 39 项通过 |
| `node tests/fuzz-model-contract.mjs` | X27 生成器/缩减器确定性、覆盖 | 10 项通过 |
| `node tests/fuzz-process-contract.mjs` | X27 子进程隔离、超时、输出超限、异常退出 | 7 项通过 |
| `node tests/fuzz/generate-corpus.mjs` | X27 固定种子语料生成 + 状态机校验 | 通过（3 种子 × 100 SELECT + 96 步） |
| `npm run test:safety`（工作台） | 整表变异提示 | 22 项通过 |
| `npm run test:history`（工作台） | 查询历史 | 24 项通过 |
| `npm run test:csv`（工作台） | CSV 导出 | 21 项通过 |
| 工作台 `diagnostic-location` / `sql-files` 契约 | 诊断位置映射 / SQL 文件 | 29 / 7 项通过 |
| `tsc -b && vite build`（工作台） | 类型与构建 | 通过 |

> 依赖 C++ 引擎构建的重型回归（`access-control-http.mjs`、`fuzz-differential.mjs`、工作台浏览器 DOM 回归）因当前环境未构建二进制未在本次运行，见「剩余范围与阻塞」。

---

## 四、当前实现状态（成员 C 视角）

| X 编号 | 状态 | 说明 |
| --- | --- | --- |
| X24 | 部分实现 | 访问目录、角色继承、对象授权、加盐密码、跨会话身份、元数据过滤、审计过滤均已实现；**新增**原子 `GRANT/REVOKE/CREATE USER/CREATE ROLE` 纯函数接口并映射为独立 HTTP 资源端点，工作台新增权限/审计/会话面板。仍缺：访问目录进入 C++ 页式 Catalog、CLI 强制身份校验、面板携带用户身份头。 |
| X27 | 部分实现 | SELECT 固定种子差分、4 类变异、SQLite 差分、模型缩减、故障分类齐全；**新增** DDL/DML 状态机生成器、固定种子语料持久化与 CI 回归入口。仍缺：依赖 C++ 引擎的差分、跨进程崩溃恢复组合、无限/压力输入。 |
| C2 工作台 | 部分实现 | 已接入真实 C++ 编译、Token/AST/计划展示、诊断、事务、历史、CSV、存储统计，并**新增**权限/审计/会话面板（`AccessControl.tsx`）。仍缺：锁等待提示与取消按钮的完整结合、备份/迁移/资源预算字段的完整呈现、浏览器 DOM 回归。 |
| C4 全量验收 | 未完成 | feature matrix 与最终报告未定稿；需在引擎构建并完成各专项回归后，由成员 C 统筹收口。 |

---

## 五、本次修改 / 新增文件清单

**新增**
- `outputs/minisql-backend/tests/fuzz/generate-corpus.mjs` —— X27 语料生成脚本
- `outputs/minisql-backend/tests/fuzz/manifest.json` —— 固定种子语料清单（SHA-256）
- `outputs/minisql-backend/tests/fuzz/fixture.sql`、`corpus-select-*.sql`、`corpus-state-*.sql` —— 生成语料
- `outputs/minisql-backend/tests/access-catalog-atomic-contract.mjs` —— X24 原子接口契约测试
- `outputs/minisql-backend/tests/access-atomic-http.mjs` —— X24 原子 HTTP 资源端点集成测试（不依赖引擎）
- `outputs/minisql-workbench/src/AccessControl.tsx` —— C2 权限/审计/会话面板组件
- `.github/workflows/ci.yml` —— 可重复回归 CI（成员 C 入口）

**修改**
- `outputs/minisql-backend/scripts/access-catalog.mjs` —— 新增 9 个原子权限接口函数
- `outputs/minisql-backend/scripts/database-bridge.mjs` —— 原子接口映射为 HTTP 端点、启动预热弹性、CORS 加 DELETE、capabilities 声明
- `outputs/minisql-workbench/src/client.ts` —— 新增权限/审计/会话客户端函数与类型
- `outputs/minisql-workbench/src/types.ts` —— 新增 Access/Audit/Session 类型
- `outputs/minisql-workbench/src/App.tsx` —— 顶栏权限入口并渲染面板
- `outputs/minisql-workbench/src/styles.css` —— 权限面板样式（含深色适配）
- `outputs/minisql-backend/docs/access-control-progress.md` —— 记录原子接口/HTTP 端点/面板与验证
- `outputs/minisql-backend/docs/fuzz-progress.md` —— 记录 X27 状态机语料与 CI
- `outputs/MiniSQL扩展功能实施清单_V1.0.md` —— 更新 X24 / X27 状态行

---

## 六、剩余范围与阻塞

**阻塞项（需前置条件）**
1. C++ 引擎未构建（`outputs/minisql-backend/bin/` 不存在）。`access-control-http.mjs`、`fuzz-differential.mjs`、`database-http.mjs` 等依赖真实 `minisql_database.exe` 的回归无法运行。需先执行 `scripts/build.ps1`（MSVC + vcpkg）。
2. 工作台浏览器 DOM 回归（`tests/*.cjs`）需要 `npm run dev` 启动服务 + Playwright 类工具；本环境未配置。

**成员 C 待完成**
- 把原子 `grant/revoke/createUser/createRole` 映射为独立 HTTP 资源端点（当前为纯函数 + 整体 `PUT /api/access`）。
- 工作台新增多会话、权限/审计、备份/迁移/资源预算 UI（字段已部分进入 `types.ts`，组件待写）。
- 访问目录进入 C++ 页式 Catalog（需与成员 B 接口协作）。
- C4 全量 feature matrix 与最终验收报告定稿（依赖各专项回归完成）。

---

## 七、复跑说明

```powershell
# 后端成员 C 纯 Node 回归（无需构建 C++）
cd outputs\minisql-backend
node tests/access-store-contract.mjs
node tests/access-catalog-atomic-contract.mjs
node tests/fuzz-model-contract.mjs
node tests/fuzz-process-contract.mjs
node tests/fuzz/generate-corpus.mjs

# 工作台单元回归（需先 npm install）
cd ..\minisql-workbench
npm install
npm run test:safety
npm run test:history
npm run test:csv
npm run build
```

> 注：`fuzz-progress.md` 与 `V3当前实现状态.md` 均声明当前工程未完全闭合 V3；成员 C 的任务在 C1/C3 已补关键缺口，C2/C4 仍受引擎构建与前端依赖阻塞，如实记录为「部分实现 / 未完成」。
