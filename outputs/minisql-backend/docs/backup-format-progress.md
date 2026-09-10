# 备份格式与迁移实现进度

日期：2026-09-09。对应 EXT-SYS-007 和 X26，当前为部分实现，不表示 X26 全量验收通过。

## 已实现

- 备份 manifest 升级为 version 2，记录页文件格式版本、WAL 截止位置、字节数、创建时间和 SHA-256。
- 备份前仍要求无活动会话，并执行 CHECKPOINT，确保复制文件不包含未落盘 WAL。
- 恢复先校验 SHA-256，再校验页格式版本、manifest 版本和 WAL 截止位置。
- 接受 version 1 旧 manifest，并根据备份页文件自动迁移为 version 2。
- 未知 manifest 版本或页格式版本明确返回 422，不执行覆盖恢复。
- 备份列表返回 manifestVersion、pageFormatVersion 和 walBytes。
- HTTP capabilities 新增 backupManifestVersion 和 backupMigration。

## 增量备份与迁移链增量

- 新增离线按页增量备份：以某个全量或增量备份为 base，CHECKPOINT 后逐 4096 页比较，仅保存变化页和新增页。
- 增量文件为 `name.delta`，manifest 升级为 version 3，记录 `kind=incremental`、`base`、页数、SHA-256 和页格式版本。
- 支持链式创建与恢复：`base1.pages -> inc1.delta -> inc2.delta`，恢复时递归校验整条链，任何一环校验失败都返回 422 且不覆盖数据库。
- 恢复改为先在内存中重建完整页文件，再通过同目录临时文件原子替换，避免写坏数据库后再调用恢复。
- 修复 PageFile CHECKPOINT 对不存在 WAL 的边界缺陷：恢复删 WAL 后再次 CHECKPOINT 不再报 9999，而是创建空 WAL 并完成截断。
- capabilities 暴露 `backupIncremental`、`backupChain`、`backupManifestVersions`。

## 验证

- `node tests/backup-smoke.mjs`：42 项检查通过。
- 覆盖 v2 manifest 字段、未知版本拒绝、v1 到 v2 迁移、恢复后数据一致、备份列表和活动会话拒绝。
- 覆盖全量 base1、增量 inc1、二次链 inc2、恢复、列表 kind/base、链上 manifest 损坏拒绝。
- Release 构建和 HTTP 回归通过。

## 在线快照增量

- 新增在线一致性快照入口 `POST /api/backup`（`mode=online`）：
  - 复用 C++ `Database::createSnapshot` 在数据库级锁内复制页文件、WAL 和 `.ckpt`。
  - 备份 manifest 升级为 version 4，记录 `walBytes`、`walCutoffBytes`、`committedSequence`、`catalogVersion` 和 `indexVersion`。
  - 会话在线时也允许快照；快照完成后 bridge 会关闭无会话共享引擎，避免旧锁阻塞后续请求。
  - 恢复链支持 version 4 manifest 和非零 WAL 截止位置，并在重建整条链后原子替换页文件和 sidecar。

## 在线快照验证

- `node tests/backup-online-smoke.mjs`：19 项通过，覆盖在线快照、manifest 版本 4、WAL 侧车校验、快照后插入并恢复、活动会话下快照和恢复后数据一致。

## 未完成

- 尚未实现备份期间在线并发读写一致性快照和 WAL 非零位置重做。
- 增量备份仍要求无活动会话，恢复前仍会在内存中完整重建页文件，未实现页面流式恢复。
- 尚未实现自动迁移失败回滚点：校验失败发生在写库前，因此不会破坏当前库，但没有保留临时回滚目录。
- WAL 截止位置当前固定为 0，因为备份前已执行 CHECKPOINT；尚未支持从非零 WAL 位置重做。
- X26 全量验收仍待完成。
