# 持久化格式兼容表

日期：2026-09-10。对应任务书 B5 交付物 5「持久化格式兼容表」，与 EXT-SYS-001/003/007 的页级索引、WAL/检查点和备份迁移格式配套。本文档只说明各持久化工件（文件）的字节布局与版本兼容关系，不重复各专项设计中的算法细节。

## 1. 数据库主文件 `<db>.pages`

堆／目录／索引／页级状态都落在该文件，固定页大小 4096 字节，小端字节序。

### 1.1 文件头（固定 40 字节）

| 偏移 | 长度 | 字段 | 说明 |
|---|---|---|---|
| 0 | 4 | `fileMagic` | `0x4644534d`（"MDSD"）；不符即拒绝加载 |
| 4 | 4 | 保留 | 0 |
| 8 | 4 | `version` | `1` 或 `2`；不支持其他值 |
| 12 | 4 | 页大小 | 恒为 4096 |
| 16 | 8 | 页数 `count` | 活动页数 |
| 24 | 8 | `identity[0]`（version 2） | 文件身份，v1 未写入 |
| 32 | 8 | `identity[1]`（version 2） | 文件身份 |

> v1 到 v2 兼容：加载 v1 时只读 header[0:16]，随后按需用 `writeHeader()` 迁移为 v2（补全身份字段），保存时 `version` 取 `1/2`。

### 1.2 页记录 `SlottedPage`

每页序列化为 `{ pageId, owner, generation }` 头加负载；内嵌校验与代次，读到时校验 `id/generation/owner`，不符报 `STORAGE_CORRUPTION`。

## 2. 页类型编号（页记录头首字节）

| 字节 | 类型 | 说明 |
|---|---|---|
| `0x00` | 堆数据页 | 通用槽页（非索引） |
| `0x50` | `INDEX_META` | 索引元页（每索引一个，记根页与树元信息） |
| `0x41` | `INDEX_INTERNAL` | 内节点页（含根为内节点时） |
| `0x42` | `INDEX_LEAF` | 叶节点页（height=0，带右兄弟指针） |

> 任何读到的页首字节不在上述集合即视为损坏，拒绝并报 `ErrorCode::Storage`。

### 2.1 索引两种持久化格式兼容

| 格式 | 载体 | 读写方式 | 兼容/回滚 |
|---|---|---|---|
| 旧 JSON 快照 | 分块 `IXPAGE` owner 页 | `dump/restore` 整树 JSON，按 3800B 分块 | `MINISQL_INDEX_ENGINE` 或格式版本选择回落路径 |
| 页级节点（新） | `INDEX_META/INTERNAL/LEAF` 页 | 按页逐层遍历，子指针即 `PageRef` | 检测到旧格式时 `MINISQL_REBUILD_INDEXES` 从堆重建 |

索引元数据仅按 owner 页关联，依赖 `pageId + generation` 定位；删除/合并会修正父页 `PageRef` 并回收旧页。

## 3. WAL `<db>.wal`

整页重做（redo）日志，追加模式，只增不截断直到检查点。

| 头部字段 | 说明 |
|---|---|
| magic | `0x4a44534d`（"JDSM"）|
| 提交序号 / LSN | 单调递增；与检查点 `.ckpt` 对齐保证跨重启 LSN 单调 |
| 事务 id | 偏移 64 |
| 起始 LSN / 结束 LSN | 结束 LSN 偏移 80；恢复起点据此判定 |
| 记录 magic | `0x5244534d`（record）与 `0x434d544d`（commit marker）区分提交/未提交/半条 |

恢复按 `.ckpt` 的 `walCutoffBytes` 跳过前缀，只重放其后已提交的扩展。提交序号在每个 `commitWriteBatch` 递增，并在恢复后写回 `.ckpt`。

## 4. 检查点边车 `<db>.ckpt`

| magic | `0x4d595043`（"MYPC"）|
|---|---|
| `walCutoffBytes` | 检查点吸收的 WAL 截止字节（未检查点剩余日志位置）|
| `dirtyWatermark` | 已落盘到主文件的页数上限（脏页水位/恢复起点）|
| `catalogVersion` | 目录版本 |
| `indexVersion` | 索引版本 |
| `committedSequence` | 检查点时的提交序号（LSN 语义）|
| `timestampMs` | 检查点时间戳 |

加载时校验 magic／字段；损坏（不完整字段、magic 不符、顺序异常）的 `.ckpt` 必须被拒绝。

## 5. 备份工件与版本

### 5.1 全量备份 `<name>.pages` + `<name>.pages.json`

manifest version 2：

| 字段 | 说明 |
|---|---|
| `version` | `2` |
| `name`/`createdAt`/`bytes` | 标识与大小 |
| `sha256` | 文件整 SHA-256，恢复前校验 |
| `pageFormatVersion` | 对应 `.pages` 文件头 version（1/2）|
| `walBytes` | 快照 WAL 字节（一致性快照后为 0）|
| `snapshot` | 可选对象：`{committedSequence, walBytes, dirtyWatermark, catalogVersion, indexVersion, checkpointedAt}` |

version 1 旧 manifest 自动迁移为 version 2（校验 SHA-256 后补 `pageFormatVersion`/`walBytes`）。version ≠ 2 或 `pageFormatVersion` 与文件不符、`walBytes != 0` 时拒绝（422）。

### 5.2 增量备份 `<name>.delta` + manifest

manifest version 3 = version 2 字段 + `kind=incremental`、`base`（指向 `.pages` 或 `.delta`）。文件头 64 字节 `"MISQLDLT"`（版本 1、页大小 4096、base 页数、最终页数、记录数），后随变长页记录 `{ index8, pageData4096 }`（只保存相对 base 变化/新增页）。

链式兼容：`base1.pages → inc1.delta → inc2.delta`，恢复时递归校验整条链，任一生效于版本/页数/SHA-256 不符即 422 且不覆盖数据库。`/backups` 暴露 `chainDepth`。

### 5.3 恢复与回滚

- 恢复先整条链内存重建 → 同目录临时文件原子替换 → 替换前复制原库为 `backups/<name>.rollback-<ts>.pages`；替换失败保留原库与回滚副本且不删除 `.wal`。
- 成功后删除回滚副本。

## 6. 兼容矩阵

| 工件 | 读取 | 写入 | 拒绝条件 |
|---|---|---|---|
| `.pages` v1 | 兼容，读 header[0:16] | 迁移 v2 后写 | magic 不符 / 版本 ∉ {1,2} / identity 全 0 |
| `.pages` v2 | 校验 identity 非 0 | 支持 | `pageFormatVersion` 缺失不匹配 |
| 索引 JSON 快照 | 旧格式回滚入口 | `REBUILD_INDEXES` 后写页级 | 旧索引被页级读取 |
| 索引页级 | 逐页下钻 | 分裂/借位/合并/根收缩 | 页类型不在 {0x41,0x42,0x50} |
| `.wal` | 跳过 cutoff 前缀后重放 | 追加，不截断 | magic 不符 / 半条日志不提交 |
| `.ckpt` | 恢复起点 | 每次检查点覆写 | 损坏 / magic 不符 / 字段异常 |
| daomin v2 manifest | 校验后使用 | 迁移自 v1 | version ∉ {2,3} / sha256 失配 / `walBytes != 0` |
| daomin v3 delta | 链上先验 base | 相对 base 写变化页 | 链上任一环节损坏 |

## 7. 已知未覆盖

- 书开关备份仍为「一致性快照（空闲）」且 `walBytes=0`；从非零 WAL 截止位置恢复重做尚未实现。
- 恢复先整链内存重建，页面级（按页流式）恢复未实现。