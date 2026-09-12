# MiniSQL 页级 B+ 树进度（X20 最小实现）

## 本轮交付（插入成树、删除路径、页级结构校验、HTTP/工作台接入）

- 新增/验证同上（页级 `PageBPlusTree`：插入成树、删除借位/合并/根收缩、`inspect()` 结构校验）。
- **Database 索引主路径切到页级引擎**：
  - `RuntimeIndex` 增加 `pageFile`/`pageTree`，用 `search()`/`range()`/`height()`/`size()`/`validate()` 包装二选一引擎；executor 的 IndexScan、唯一校验、catalog 的 height/pageCount 均改走统一接口。
  - `PageBPlusTree::search` 对齐为 `const`，与内存 `BPlusTree` 一致，可在只读上下文调用。
  - `rebuildIndexes`：页级引擎时清旧页后从堆全量建树（`create()` + 逐行 `insert`），索引页与堆页同属一个 `PageFile`/WAL，落在 `run(plan)` 的 `beginWriteBatch`…`commitWriteBatch` 批次内，随批次原子提交/回滚——即"同一 WAL 批次联动"。
  - 引擎开关：`MINISQL_INDEX_ENGINE`（未设或非 `memory`）= 页级 `page-file` 主路径；设为 `memory` 走原内存树 + `loadIndexPages`/`persistIndexPages`（后者仍保留作兼容入口）。
  - 页级路径因每次重建总与堆一致，无需指纹判断陈旧；`MINISQL_REBUILD_INDEXES` 对页级引擎恒等重建。
- `minisql_page_bplus_tree_contract` 加入 CTest（`tests/page_bplus_tree_contract.cpp`）：空树/多级分裂/点查/范围/复合键/唯一拒绝/**删除借位合并根收缩**/**删除后重启恢复**/**`inspect()` 结构校验**/**陈旧代次拒绝**。隔离环境验证 496 项全绿。

## 本轮追加（inspect() 接入 HTTP/工作台）

- 后端会话协议（`database_main.cpp`）新增 `indexInspect` 操作，字段白名单放行 `table`/`index`，经 `Database::indexInspect` 返回页级结构 JSON。
- `session-process.mjs` 的 `request()` 透传 `table`/`index` 上下文。
- bridge 新增只读会话路由 `POST /api/sessions/:id/index-inspect`，带表级 `READ` 权限与审计；`runSessionOperation` 支持透传上下文。
- 工作台 Web（`client.ts`/`App.tsx`/`IndexInspect.tsx`）：侧栏表展开区列出索引，点击即触发检查；新增 `Inspect` 输出面板展示树高/节点/叶链/校验标记/结构问题及页明细。

## 合并后验证（2026-09-10）

- Windows Release 主工程构建通过：`cmake --preset windows -DBUILD_TESTING=OFF`、`cmake --build --preset windows-release --parallel`。
- `node tests/index-smoke.mjs`：29 项索引检查、41 项索引持久化检查、44 项重启/持久化检查通过。
- `node tests/database-http.mjs`：真实 HTTP 执行链路通过；`node tests/session-process.mjs`：51 项 session 协议检查通过。
- 工作台 `npm.cmd run build` 与 `npm.cmd run test:browser` 通过，页级索引检查入口与现有连接、权限、设置、拖动和移动端 DOM 回归无冲突。
- 新增 `tests/index-scale-smoke.mjs`：1000 行 15 项（height=2, pages=33）、10000 行 33 项（height=3, pages=323）通过，覆盖批量插入、CREATE INDEX、点查、范围扫描、行数和 catalog 索引元数据。
- 新增 `tests/index-performance-curve.mjs`：1000/5000/10000 行耗时约 0.46s / 0.80s / 3.41s，树高 2/3/3，页数 33/162/323，并输出 `tests/artifacts/index-performance-curve.json`。

## 尚未闭合

- X20 仍保留“部分实现”：需要继续补齐页级索引与所有 SQL 扩展的全量组合验收，以及更大规模数据下的性能/资源曲线。
- 旧 JSON 镜像格式仍保留为兼容回退路径；`MINISQL_INDEX_ENGINE=memory` 可用于对照验证，但不应把兼容路径当作页级主路径的验证替代。

## 回归命令

```powershell
cd outputs\minisql-backend
.\bin\page_bplus_tree_contract.exe   # 页级契约
.\bin\bplus_tree_contract.exe        # 既有公开 API 契约回归
node tests\index-smoke.mjs            # 既有 44 项索引回归（确保不受影响）
```

## 兼容与回滚

- `PageBPlusTree` 已接入 `Database` 索引主路径；原 `BPlusTree`/JSON 镜像仍保留为兼容回退实现，既有索引测试保持绿色。
- 设计见 `docs/page-bplus-tree-design.md`。旧 JSON 快照 `loadIndexPages` 保留为兼容入口。

## 索引事务化补充（2026-09-12）

### 一、同一 undo/WAL 单元

INSERT/UPDATE/DELETE 先经 `validateUniqueIndexes` 校验，再同批执行 `insertIndexEntries`/`eraseIndexEntries`，
堆页与索引页同属一个 `PageFile`，落在 `runNode` 的 `beginWriteBatch`…`commitWriteBatch` 批次内。
提交前失败由 `rollbackBatch`（回滚页批次 + `catalog_.reload` + `reloadIndexRuntimes`）恢复，
`tests/index-transaction-process.mjs` 覆盖 DML 提交后一致、事务回滚后索引条目恢复、以及唯一冲突后实例仍可用。

### 二、唯一索引建造三阶段（build → validate → publish）

`Database::buildIndexEntries` 把这两个阶段抽成统一实现：

1. **build**：页级引擎先 `clearIndexPages(owner)` 再 `create()` 并从堆表全量插入；内存引擎 `BPlusTree::reset()` 后重建。
2. **validate**：`validate()` 校验树结构、比对条目数与已插入条数、统计唯一键重复数；内存引擎额外检查每行可索引。
3. **publish**：只有 `problems` 为空才登记运行时；页级引擎的节点页已落在 owner 下，内存引擎用 `persistMemoryIndexes` 写回快照。

`CREATE INDEX` 与 `initializeIndexes` 的重建路径都走这三阶段；唯一索引遇到重复键在 validate 阶段被拒，
不写目录、不发布运行时，随写批次回滚。失败诊断沿用 `UNIQUE index contains duplicate keys`。

### 三、索引一致性检查与在线重建

- `Database::indexVerify(table, index)`：结构校验（页类型/兄弟/叶链/根可达）＋堆↔索引双向交叉检查
  （每行条目可达、每条目指向存活且键一致的堆行、条目数与可索引行数一致），返回 `consistent` 与问题列表。
- `Database::indexRebuild(table, index)`：对单个索引执行 build → validate → publish，事务内随事务提交/回滚，
  事务外自成写批次；发布即替换运行实例并释放旧页。
- 会话协议新增 `indexVerify` / `indexRebuild`；bridge 路由 `POST /api/sessions/:id/index-verify`（READ）
  与 `/index-rebuild`（UPDATE）；能力报告新增 `indexVerify` / `indexRebuild` / `indexConsistencyCheck` /
  `uniqueIndexBuildPhases`。

### 四、验证

- `node tests\index-transaction-process.mjs`：84 项（页级与内存引擎各一遍），覆盖重复键拒绝后不发布、
  DML/回滚一致性、在线重建、事务内重建回滚、唯一约束经索引生效、重启后一致。
- `node tests\index-smoke.mjs` 47 项、`node tests\incremental-index-process.mjs` 68 项、
  `minisql_bplus_tree_contract` 113 项、`minisql_page_bplus_tree_contract` 496 项保持通过。

### 五、边界

- 在线重建在 `mu_` 串行化下进行，不提供与并发写的细粒度并行；重建期间读写互斥但不中断实例。
- `indexVerify` 是全量交叉检查，代价与表行数成正比，适合维护/诊断入口，不是每次查询的开销。
- 一致性检查能发现结构损坏、缺失/悬挂/错键条目；发现损坏后的修复手段是 `indexRebuild`，不是自动静默重建。

