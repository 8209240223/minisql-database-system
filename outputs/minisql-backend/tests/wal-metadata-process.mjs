import assert from 'node:assert/strict';
import { existsSync, mkdtempSync, readFileSync, statSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, extname } from 'node:path';
import { openSession } from '../scripts/session-process.mjs';

// X22 记录级 WAL：扩展头的 txId / 逻辑 LSN / prevLsn / 提交 LSN / 记录类型，
// 页记录的 recordLsn / prevRecordLsn 链，Abort（撤销）记录，以及跨日志截断的 LSN 单调性。
const executable = process.env.MINISQL_DATABASE_EXE ?? './build/windows/Release/minisql_database.exe';
const root = mkdtempSync(join(tmpdir(), 'minisql-wal-metadata-'));
const database = join(root, 'db.pages');
const journal = database + '.wal';
const page = 4096;
let checks = 0;

function equal(actual, expected, message) { assert.deepEqual(actual, expected, message); ++checks; }
function ok(value, message) { assert.ok(value, message); ++checks; }
const u32 = (bytes, offset) => bytes.readUInt32LE(offset);
const u64 = (bytes, offset) => Number(bytes.readBigUInt64LE(offset));

// 解析期刊为扩展列表：每个扩展 = 头页 + records×(记录页+数据页) + 终止页。
function extents() {
  if (!existsSync(journal)) return [];
  const bytes = readFileSync(journal);
  const list = [];
  for (let offset = 0; offset + page <= bytes.length;) {
    if (u32(bytes, offset) !== 0x4a44534d) break;
    const records = u64(bytes, offset + 16);
    const entry = {
      offset,
      version: u32(bytes, offset + 8),
      records,
      baseCount: u64(bytes, offset + 48),
      commitSeq: u64(bytes, offset + 56),
      txId: u64(bytes, offset + 64),
      startLsn: u64(bytes, offset + 72),
      prevLsn: u64(bytes, offset + 96),
      recordType: u32(bytes, offset + 104),
      commitLsn: u64(bytes, offset + 112),
      undoNextLsn: u64(bytes, offset + 120),
      extentLsn: u64(bytes, offset + 128),
    };
    if (records > 0) {
      const record = offset + page;
      entry.firstPageId = u64(bytes, record + 8);
      entry.firstRecordLsn = u64(bytes, record + 24);
      entry.firstPrevRecordLsn = u64(bytes, record + 32);
      entry.firstRecordType = u32(bytes, record + 40);
    }
    const terminator = offset + (2 * records + 1) * page;
    if (terminator + page <= bytes.length) {
      entry.terminatorMagic = u32(bytes, terminator);
      entry.terminatorSeq = u64(bytes, terminator + 8);
    }
    list.push(entry);
    offset += 2 * (records + 1) * page;
  }
  return list;
}

let session = await openSession(executable, database);
try {
  equal((await session.request('execute',
    'CREATE TABLE t(id INT PRIMARY KEY, v INT); INSERT INTO t VALUES(1,10); INSERT INTO t VALUES(2,20);')).success,
    true, 'seed three committed statements');

  const committed = extents();
  equal(committed.length, 3, 'one extent per committed statement');
  const [first, second, third] = committed;
  equal(first.version, 2, 'journal extent header version 2');
  equal(first.recordType, 1, 'committed extent is a redo record');
  equal(first.firstRecordType, 1, 'page record type is redo');
  equal(first.extentLsn, 1, 'logical LSN starts at 1');
  equal(first.prevLsn, 0, 'first extent has no predecessor');
  equal(first.txId, 1, 'first write batch gets transaction id 1');
  equal(second.txId, 2, 'transaction ids are allocated per write batch');
  equal(third.txId, 3, 'transaction id advances');
  equal(second.extentLsn, 2, 'logical LSN advances per commit');
  equal(second.prevLsn, first.extentLsn, 'prevLsn chains to the previous extent');
  equal(third.extentLsn, 3, 'third logical LSN');
  equal(third.prevLsn, second.extentLsn, 'global chain continues');
  equal(first.commitLsn, first.startLsn + (2 * first.records + 1) * page, 'commitLsn points at the commit marker page');
  equal(first.terminatorMagic, 0x434d544d, 'terminator page present');
  equal(first.terminatorSeq, first.commitSeq, 'commit marker carries the commit sequence');
  equal(first.firstPageId, 0, 'file header page is logged first');
  equal(first.firstRecordLsn, first.startLsn + page, 'record LSN equals its physical offset');
  ok(third.firstRecordLsn > first.firstRecordLsn, 'record LSNs advance across extents');

  // 页级链：同一页在后续扩展里再次被记录时，prevRecordLsn 指向它上一条记录。
  const repeated = committed.find(extent => extent.firstPrevRecordLsn > 0);
  ok(repeated, 'page-level prevRecordLsn chain is populated');
  ok(repeated.firstPrevRecordLsn < repeated.firstRecordLsn, 'page chain points backwards');

  const before = await session.request('statistics');
  equal(before.wal.lastExtentLsn, third.extentLsn, 'statistics expose the chain tail');
  equal(before.wal.committedSequence, third.commitSeq, 'statistics expose the commit sequence');
  ok(before.wal.nextLsn > third.extentLsn, 'statistics expose the next LSN');
  ok(before.wal.trackedPages >= 1, 'statistics expose page-level LSN tracking');
  ok(before.wal.recordedPages >= 3, 'statistics expose recorded page count');

  // 回滚写成一条 Abort 记录（记录级撤销），而不是让未提交修改无声消失。
  equal((await session.request('execute', 'BEGIN; INSERT INTO t VALUES(3,30); ROLLBACK;')).success, true,
    'rollback transaction');
  const afterRollback = extents();
  const abort = afterRollback.at(-1);
  equal(abort.recordType, 3, 'rollback appends an abort record');
  equal(abort.records, 0, 'abort record carries no page records');
  equal(abort.terminatorSeq, 0, 'abort record is terminated as uncommitted');
  equal(abort.undoNextLsn, abort.prevLsn, 'undo chain points at the transaction predecessor');
  equal(abort.prevLsn, third.extentLsn, 'abort extent chains onto the previous extent');
  equal(abort.extentLsn, third.extentLsn + 1, 'abort record consumes the next logical LSN');
  equal(abort.commitSeq, third.commitSeq, 'abort does not advance the commit sequence');
  const aborted = await session.request('statistics');
  equal(aborted.wal.abortedExtents, 1, 'statistics count aborted extents');
  equal(aborted.wal.lastExtentLsn, abort.extentLsn, 'statistics track the abort extent');
  equal((await session.request('execute', 'SELECT id FROM t ORDER BY id;')).results[0].rows, [[1], [2]],
    'rollback leaves no rows');

  // 检查点截断日志，但逻辑 LSN 水位与 prevLsn 链尾必须跨截断保持单调。
  equal((await session.request('execute', 'CHECKPOINT;')).success, true, 'checkpoint');
  ok(!existsSync(journal) || statSync(journal).size === 0, 'checkpoint truncates the journal');
  equal((await session.request('execute', 'INSERT INTO t VALUES(4,40);')).success, true, 'insert after checkpoint');
  const afterCheckpoint = extents();
  equal(afterCheckpoint.length, 1, 'one extent after the checkpoint');
  ok(afterCheckpoint[0].extentLsn > abort.extentLsn, 'logical LSN stays monotonic across truncation');
  equal(afterCheckpoint[0].prevLsn, abort.extentLsn, 'chain tail survives truncation');
} finally {
  await session.close().catch(() => {});
}

// 重启后恢复重放并继续单调递增。
session = await openSession(executable, database);
try {
  const reopened = await session.request('statistics');
  equal(reopened.wal.lastExtentLsn, 5, 'recovery restores the LSN watermark');
  equal((await session.request('execute', 'INSERT INTO t VALUES(5,50);')).success, true, 'insert after reopen');
  const advanced = await session.request('statistics');
  equal(advanced.wal.lastExtentLsn, 6, 'LSN keeps advancing after reopen');
  ok(advanced.wal.trackedPages >= 1, 'page LSN tracking continues after reopen');
  equal((await session.request('execute', 'SELECT id FROM t ORDER BY id;')).results[0].rows,
    [[1], [2], [4], [5]], 'recovered rows are correct');
} finally {
  await session.close();
}
console.log(`${checks} WAL metadata checks passed: lsn/txId/prevLsn chains, abort records and monotonicity`);
