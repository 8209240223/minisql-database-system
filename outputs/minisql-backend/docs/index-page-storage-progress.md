# MiniSQL 索引页落盘进度

## 本轮实现

- B+ 树新增 `dump` / `restore` 字符串接口，原有 JSON sidecar 的 `save` / `load` 保留为兼容入口。
- Database 不再把索引镜像写入 `db.pages.idx.*.json`，改为将 B+ 树快照按 3800 字节分块写入页文件中的独立 owner 页。
- 每个索引页只保存一块带序号、总页数和长度的 `IXPAGE` 记录；启动时按序号重组并调用 `restore`，校验失败则从堆表扫描重建。
- 提供 `MINISQL_REBUILD_INDEXES=1` 恢复入口，可强制忽略页镜像并从堆表重建，重建后再次写回页文件。
- CREATE INDEX、INSERT/UPDATE/DELETE 后的重建、DROP INDEX 和事务回滚都走页文件分配/释放；DROP INDEX 不再删除 sidecar 文件。
- Catalog 的 index 项新增 `storage=page-file`、`pageCount` 和 `height`，可观察根分裂和页数。
- capabilities 暴露 `indexPageStorage`。

## 验证

`node tests/index-smoke.mjs`：44 项检查通过，新增覆盖：

- 400 行索引数据使树超过根页容量，`height>=2`，页式索引 `pageCount>=1`。
- 分裂后点查与范围查有序正确，删除数据后索引/全扫一致。
- 重启后从页文件恢复，仍能命中 IndexScan，页镜像与树高度保留。
- 强制重建后索引恢复并再次写回页文件。
- 不再产生任何 `.idx.*.json` sidecar。

`bplus_tree_contract.exe` 113 项、`database-http.mjs` 全套和 `session-http.mjs` 28 项通过。

## 限制

- 当前是“整棵树分页镜像”，不是每棵 B+ 节点一页并在查询时按子页指针逐页遍历。
- 删除低于阈值时的页内借位/合并仍由内存树分裂逻辑和整树重写承担，尚未实现页级原地更新。
- 索引页仍沿用 PageFile 普通页，没有单独索引页类型编号头；若需求要求独立页类型，需要在页头元数据中继续扩展。
- `MINISQL_REBUILD_INDEXES` 是维护入口，不等同于自动从任意磁盘损坏中恢复。
