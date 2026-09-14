# WAL 与恢复增强（记录级 undo/redo、group commit、模糊检查点、双写缓冲）

日期：2026-09-12。对应 EXT-SYS-002 / EXT-SYS-003 与 X22 剩余项的存储侧增强。
本文档记录本轮实现、验证与边界；默认行为与既有 `.wal` 兼容。

## 一、记录级 WAL（txId / LSN / prevLsn / 记录类型）

### 格式

扩展头升级为 **version 2**，在既有字段（magic、checksum、records、finalCount、identity、
baseCount、commitSeq、txId、startLsn、endLsn、cutoff）之后追加：

| 偏移 | 字段 | 含义 |
| --- | --- | --- |
| 96 | `prevLsn` | 全局 `prevLsn` 链：上一扩展的逻辑 LSN（首个为 0） |
| 104 | `recordType` | 1 = Redo（已提交/预备扩展），3 = Abort（撤销记录） |
| 108 | `flags` | 保留 |
| 112 | `commitLsn` | 提交标记页的逻辑 LSN（未提交为 0） |
| 120 | `undoNextLsn` | 事务内撤销链：同事务上一扩展的逻辑 LSN |
| 128 | `extentLsn` | 本扩展的逻辑 LSN |

页记录页（每页一条）追加：`recordLsn`（24）、`prevRecordLsn`（32，同一页上一条记录的 LSN，
即页级 LSN 链）、`recordType`（40）。检查点记录追加 `walLsn`（64，逻辑 LSN 水位）、
`checkpointBeginLsn`（72）、`checkpointEndLsn`（80）、`archivedBytes`（88）、`archiveSegments`（96）。

**逻辑 LSN 与物理偏移分离**：`startLsn`/`endLsn` 仍是期刊内的字节偏移（用于定位），
`extentLsn`/`recordLsn`/`prevLsn` 是单调递增的逻辑 LSN。检查点截断日志后，`walLsn`
把水位持久化到 `.ckpt`，因此 LSN 跨截断与重启都不回退（`tests/wal-metadata-process.mjs` 断言）。

### 撤销记录（记录级 undo 语义）

`rollbackWriteBatch` 不再让未提交修改在日志里无声消失：批次曾暂存过页时，回滚会追加一条
**Abort 扩展**（`recordType=3`，无页记录，终止页 seq=0，`undoNextLsn` 指向同事务上一 LSN），
随后才把事务链截断。恢复时 Abort 扩展被跳过并继续其后扩展；检查点时随前缀一起回收。

### 恢复

`recoverJournal` 同时接受 v1 与 v2 日志（v1 用 commitSeq 退化为 LSN）。未提交扩展（终止页
seq=0）被丢弃后**继续**扫描其后扩展，因此 Abort 记录不会阻塞后续已提交扩展的重做；
已提交扩展的页级 LSN 链只在提交确认后并入 `pageLsn_`。

## 二、group commit（`MINISQL_GROUP_COMMIT=1`）

- 提交时只把扩展（含提交标记）追加到期刊并 `flush()` 到操作系统，**延迟 fsync 与数据页应用**；
  待同步扩展进入 `pendingExtents_`，其页进入 `pendingPages_` 读覆盖，保证读自己的提交。
- `syncJournalGroup()` 先一次性 fsync 整组日志，再按序 `applyExtent` 把数据页写入主文件。
  崩溃发生在组同步之前时，标记未落盘、数据页也未应用 → 恢复只可能丢弃整组，不会出现
  “有数据无标记”的中间态（`tests/wal-group-commit.mjs` 断言主文件字节不变）。
- 触发点：`MINISQL_GROUP_COMMIT_BYTES`（默认 16 MiB）超过阈值、检查点、快照
  （`copyTo`）、以及 `PageFile` 析构（干净关闭）。
- 默认关闭：未设该变量时逐次提交仍然 fsync 标记并立即应用，既有崩溃点语义（`prepared` /
  `published` / `applied-page` / `data-synced` / `checkpointed`）完全不变。

## 三、模糊检查点与日志归档 / 安全回收

- `CheckpointOptions` 增加 `fuzzy` 与 `archive`，并可用 `MINISQL_FUZZY_CHECKPOINT=1` /
  `MINISQL_ARCHIVE_WAL=1` 全局启用。
- **模糊检查点**：记录 `checkpointBeginLsn` / `checkpointEndLsn` 与截止位置
  `walCutoffBytes = walBytes()`，**不截断日志、不强制刷出缓存**；恢复从截止位置之后重做。
  提交时数据页已落盘，因此该截止位置对恢复是安全的。
- **安全回收**：非模糊检查点回收日志前缀（`cutoff==0` 表示无更早恢复起点，整段可回收；
  否则只回收检查点覆盖的前缀），可用 `archive` 先归档到 `.wal.archive.N` 并追加
  `.wal.archive.log` 清单。回收范围限定为“已提交、已落盘、且不被检查点之后的恢复所需”，
  未提交尾在回滚时已截断，活动事务的批次不允许检查点。
- 统计：`wal.archiveSegments` / `wal.archivedBytes` / `wal.fuzzyCheckpoint` /
  `checkpointRecord.checkpointBeginLsn`。

## 四、torn-page 防护（双写缓冲，`MINISQL_DOUBLEWRITE=1`）

- 新增 `<db>.dwb`：1 个头页 + 32 个槽位（每槽一页）。`applyExtent` 在写主文件前先把整
  扩展的页落入槽位并 fsync（`doublewrite-staged` 崩溃点），主文件同步成功后再释放槽位。
- 打开数据库时先 `recoverDoubleWrite()`（在日志重做之前），用仍有效的槽位修复未完成的页写，
  必要时扩展文件长度，然后同步主文件。
- 这是重做日志不可用时的最后一道防线：即使 `.wal` 丢失，已暂存页仍可修复
  （`tests/doublewrite-torn-page.mjs` 的 A/B 对照：启用时恢复出已提交行，禁用时丢失）。
- 默认关闭；启用时统计 `wal.doubleWrite = true`。

## 五、验证

| 测试 | 结果 |
| --- | --- |
| `node tests/wal-metadata-process.mjs` | 49 项：扩展/记录 LSN、txId、prevLsn 链、Abort 记录、跨截断单调性 |
| `node tests/wal-group-commit.mjs` | 29 项：group commit 延迟应用与阈值同步、模糊检查点、归档与安全回收 |
| `node tests/doublewrite-torn-page.mjs` | 16 项：槽位暂存、日志不可用时的修复、槽位释放、正常路径不变 |
| `node tests/x22-fault-injection.mjs` | 30 项（既有崩溃点语义保持不变） |
| `node tests/background-checkpoint-fault-injection.mjs` | 39 项 |
| `node tests/auto-checkpoint-smoke.mjs` | 62 项 |
| `node tests/checkpoint-smoke.mjs` / `decimal-journal-process.mjs` | 6 / 32 项 |
| `minisql_write_batch_contract` / `minisql_storage_contract` / `minisql_database_contract` / `minisql_buffer_contract` | 24 / 31 / 81 / 40 项 |

## 六、边界

- group commit 的“组”按字节阈值与进程生命周期界定；本实现不提供多线程并发提交的组内合并。
- 断电语义只能通过进程级故障注入与文件字节对比验证；本轮未做真实硬件断电或网络文件系统验证。
- 模糊检查点在本引擎中不减少日志体积（提交已应用数据页），其价值是避免检查点停顿与立即截断；
  空间回收由后续的归档/安全回收完成。
- 双写缓冲按扩展暂存，单扩展超过 32 页时退化为直接写主文件（不影响既有路径）。
- POSIX 分支未在本机验证。
