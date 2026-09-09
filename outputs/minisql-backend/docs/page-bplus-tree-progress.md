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

## 未含（仅剩全量回归）

- 全量回归：本仓库本地沙箱无法完成 vcpkg 依赖安装（`vcpkg install` 引导挂起、0 下载缓存），亦无 node 可编译前端 TS，故 **execution/server 及前端改动未在本环境编译/回归**。请在有 vcpkg + node 环境跑：
  - `cmake --preset windows && cmake --build --preset windows-debug && ctest --preset windows-debug`
  - `cd outputs\minisql-workbench && npm install && npm run build && npm run preview`（联调 bridge 端口）
  - 及 `tests/index-smoke.mjs` 与 HTTP 会话链路。若页级默认引发回归，可设 `MINISQL_INDEX_ENGINE=memory` 回退旧内存引擎对比。

## 回归命令

```powershell
cd outputs\minisql-backend
.\bin\page_bplus_tree_contract.exe   # 页级契约
.\bin\bplus_tree_contract.exe        # 既有公开 API 契约回归
node tests\index-smoke.mjs            # 既有 44 项索引回归（确保不受影响）
```

## 兼容与回滚

- `PageBPlusTree` 为新增自包含模块，不改动现有 `BPlusTree`/`Database` 主路径，既有索引测试保持绿色。
- 设计见 `docs/page-bplus-tree-design.md`。旧 JSON 快照 `loadIndexPages` 保留为兼容入口。