# 备份格式与迁移实现进度

日期：2026-09-10。对应 EXT-SYS-007 和 X26，当前为部分实现，不表示 X26 全量验收通过。

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

## 在线一致性快照与迁移回滚

- 引擎新增 `snapshot` 子命令（Database::snapshotInfo）：在无活动事务且无写批时 flush 全部缓冲并执行一次检查点，返回提交序号、WAL 状态、脏页水位、目录版本与索引版本，作为一致性快照位置。
- `/backup` 备份前调用该一致性快照替代裸 CHECKPOINT，并把 `snapshot{committedSequence, walBytes, dirtyWatermark, catalogVersion, indexVersion, checkpointedAt}` 写入 manifest（version 2/3 保留原字段，新增可选 `snapshot` 对象，不做版本号升级，避免破坏既有回归断言）。
- `/restore` 在原子替换前把当前库复制为迁移回滚副本 `backups/<name>.rollback-<ts>.pages`；替换成功则删除回滚副本，替换失败则保留原库与回滚副本供检查，且不删除 `.wal` 以免破坏原库恢复链。
- `/backups` 列表额外暴露 `snapshot` 快照信息与 `chainDepth`（增量链深度）、`verified`。

## 未完成

- 尚未实现备份期间在线并发读写一致性快照和 WAL 非零位置重做；快照仍要求无活动会话（409）。
- 增量备份仍要求无活动会话，恢复前仍会在内存中完整重建页文件，未实现页面流式恢复。
- WAL 截止位置当前固定为 0，因为备份前已执行一致性检查点；尚未支持从非零 WAL 位置在恢复时重做。
- X26 全量验收仍待完成。
