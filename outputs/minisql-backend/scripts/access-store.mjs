// X24 权限目录的页式持久化。
//
// 权限数据不再以裸 JSON 文件形式保存，而是写入固定 4096 字节页的
// `access.catalog.pages` 文件，与堆表/索引页使用同一页大小，便于后续搬进 C++ 页管理器。
//
// 文件布局：
//   页 0        META 页
//     0..7      magic "MSQLACL\0"
//     8..11     uint32 formatVersion
//     12..15    uint32 pageSize
//     16..19    uint32 pageCount（含 META 页）
//     20..23    uint32 payloadBytes
//     24..27    uint32 permissionVersion（单调递增，永不回退）
//     28..31    uint32 catalogVersion（结构版本，用于迁移）
//     32..39    uint64 updatedAtMs
//     40..71    sha256(payload)
//   页 1..n     DATA 页
//     0..3      magic "ACLD"
//     4..7      uint32 pageId（从 1 开始）
//     8..11     uint32 byteCount（本页有效负载字节数）
//     12..15    uint32 fnv1a32(本页有效负载)
//     16..4095  负载（尾部零填充）
//
// 写入始终先落临时文件再 rename，保证崩溃后要么是旧页面要么是新页面。
// 读取时逐页校验 magic、pageId、byteCount 和页内校验值，再校验整体 sha256；
// 任何一项不符都明确拒绝，不静默返回空权限（空权限会被误当成"全部拒绝"或"默认放行"）。

import { createHash } from 'node:crypto';
// 用 Node 内置的加密库算 SHA-256，避免自己实现或引入第三方依赖。
import { existsSync, mkdirSync, readFileSync, renameSync, statSync, unlinkSync, writeFileSync } from 'node:fs';
// 文件系统操作：判断存在、建目录、读文件、原子改名、取大小、删除、写文件。
import { dirname } from 'node:path';
// 取目录名，用于在写文件前确保父目录存在。

export const PAGE_SIZE = 4096;
// 页大小固定 4096 字节，与 C++ 侧的堆表/索引页保持一致，便于以后搬迁。
export const META_MAGIC = 'MSQLACL\0';
// 元数据页的魔数（8 字节，末尾是真正的 0 字节）。
export const DATA_MAGIC = 0x444c4341; // "ACLD" little-endian
// 数据页魔数：按小端序读出 0x444c4341 正好对应 ASCII 的 "ACLD"。
export const FORMAT_VERSION = 1;
// 文件格式版本；结构变化时递增，读取端据此判断能否识别。
export const META_HEADER_BYTES = 72;
// 元数据页前 72 字节是定长头，其余留空。
export const DATA_HEADER_BYTES = 16;
// 每个数据页前 16 字节是页头。
export const DATA_CAPACITY = PAGE_SIZE - DATA_HEADER_BYTES;
// 数据页正文容量 = 4096 - 16 = 4080 字节。

export class AccessStoreError extends Error {
// 本模块专用的错误类型，便于调用方区分"权限目录文件坏了"和其它错误。
  constructor(message, detail = {}) {
  // 构造时接受错误信息与一份细节对象。
    super(message);
    // 交给基类设置 message。
    this.name = 'AccessStoreError';
    // 固定名字，日志与断言里一眼能认出。
    this.detail = detail;
    // 附带上下文（例如出错的页号、字段值），不塞进 message 以免污染提示。
  }
}

export function fnv1a32(buffer) {
// FNV-1a 32 位哈希，用于页内校验（比 SHA-256 快，代价是强度低，用来发现意外损坏足够）。
  let hash = 0x811c9dc5;
  // FNV 的标准偏移基数。
  for (const byte of buffer) {
  // 逐字节处理。
    hash ^= byte;
    // 先异或。
    hash = Math.imul(hash, 0x01000193) >>> 0;
    // 再乘 32 位 FNV 素数；用 Math.imul 保证按 32 位整数相乘（否则 JS 会按浮点算导致高位丢失），
    // 再右移 0 位转成无符号 32 位。
  }
  return hash >>> 0;
  // 返回无符号 32 位结果。
}

function sha256(buffer) {
// 计算 SHA-256 摘要，返回 32 字节 Buffer。
  return createHash('sha256').update(buffer).digest();
  // update 喂数据、digest 取二进制结果（不给编码就是不转十六进制）。
}

/** 把权限目录编码成页式缓冲区。纯函数，便于契约测试直接断言字节布局。 */
// 纯函数意味着同样的输入必然得到同样的字节，测试可以直接比对缓冲区。
export function encodePages(catalog, { permissionVersion, catalogVersion = 1, updatedAtMs = Date.now() } = {}) {
// 把权限目录对象编码成完整的页式文件缓冲。
  if (!Number.isInteger(permissionVersion) || permissionVersion < 1 || permissionVersion > 0xffffffff)
  // 权限版本必须是 1 到 0xffffffff 之间的整数。
    throw new AccessStoreError('permissionVersion must be a UINT32 >= 1', { permissionVersion });
    // 不满足就拒绝编码，避免写出一个读端会判为损坏的文件。
  const payload = Buffer.from(JSON.stringify(catalog), 'utf8');
  // 把目录序列化成 UTF-8 字节，这就是要分页存放的有效负载。
  const dataPages = Math.max(1, Math.ceil(payload.length / DATA_CAPACITY));
  // 算出需要多少数据页；即使负载为空也至少留一页，保证文件里总有数据页。
  const file = Buffer.alloc((dataPages + 1) * PAGE_SIZE);
  // 一次性分配"1 个元数据页 + N 个数据页"的缓冲区。
  file.write(META_MAGIC, 0, 'latin1');
  // 写魔数。用 latin1 编码保证每个字符恰好落一个字节（不会被当成多字节 UTF-8）。
  file.writeUInt32LE(FORMAT_VERSION, 8);
  // 偏移 8：格式版本。
  file.writeUInt32LE(PAGE_SIZE, 12);
  // 偏移 12：页大小。
  file.writeUInt32LE(dataPages + 1, 16);
  // 偏移 16：总页数（含元数据页）。
  file.writeUInt32LE(payload.length, 20);
  // 偏移 20：有效负载字节数。
  file.writeUInt32LE(permissionVersion, 24);
  // 偏移 24：权限版本。
  file.writeUInt32LE(catalogVersion, 28);
  // 偏移 28：结构版本（用于将来的迁移）。
  file.writeBigUInt64LE(BigInt(updatedAtMs), 32);
  // 偏移 32：写入时间（64 位）。
  sha256(payload).copy(file, 40);
  // 偏移 40：整个负载的 SHA-256，作为最强的完整性凭据。
  for (let index = 0; index < dataPages; ++index) {
  // 逐页写数据。
    const start = index * DATA_CAPACITY;
    // 本页在负载中的起始偏移。
    const slice = payload.subarray(start, Math.min(start + DATA_CAPACITY, payload.length));
    // 切出本页要放的那一段（最后一页可能不足）。
    const base = (index + 1) * PAGE_SIZE;
    // 本页在文件中的起始偏移（第 0 页是元数据页，所以加 1）。
    file.writeUInt32LE(DATA_MAGIC, base);
    // 写数据页魔数。
    file.writeUInt32LE(index + 1, base + 4);
    // 写页号，从 1 开始。
    file.writeUInt32LE(slice.length, base + 8);
    // 写本页实际负载字节数。
    file.writeUInt32LE(fnv1a32(slice), base + 12);
    // 写本页负载的 FNV 校验值。
    slice.copy(file, base + DATA_HEADER_BYTES);
    // 把负载拷到页头之后；缓冲区已清零，尾部自然就是零填充。
  }
  // 数据页写完。
  return file;
  // 返回整份文件内容。
}

/** 解码并逐页校验。任何结构问题都抛 AccessStoreError，绝不返回"看起来能用"的部分结果。 */
export function decodePages(file) {
// 解码页式文件并逐层校验，返回目录对象与各项元数据。
  if (!Buffer.isBuffer(file)) throw new AccessStoreError('Access catalog pages must be a Buffer');
  // 入参必须是 Buffer；传字符串或 Uint8Array 都拒绝，避免隐式转换带来歧义。
  if (file.length < PAGE_SIZE * 2 || file.length % PAGE_SIZE !== 0)
  // 至少要有"元数据页 + 一个数据页"，且总长度必须是页大小的整数倍。
    throw new AccessStoreError('Access catalog file is not a whole number of pages', { bytes: file.length });
    // 不满足说明文件被截断或根本不是本格式；细节里带上实际字节数便于排查。
  if (file.subarray(0, 8).toString('latin1') !== META_MAGIC)
  // 校验元数据页魔数。
    throw new AccessStoreError('Access catalog magic mismatch');
    // 不符合就不是权限目录文件。
  const formatVersion = file.readUInt32LE(8);
  // 读格式版本。
  if (formatVersion !== FORMAT_VERSION)
  // 版本必须完全一致（这里不做"向后兼容读取"）。
    throw new AccessStoreError(`Unsupported access catalog format version ${formatVersion}`, { formatVersion });
    // 不认识就明确拒绝，避免按错误布局解读数据。
  const pageSize = file.readUInt32LE(12);
  // 读页大小。
  if (pageSize !== PAGE_SIZE) throw new AccessStoreError(`Unsupported access catalog page size ${pageSize}`, { pageSize });
  // 页大小必须等于本模块约定的 4096。
  const pageCount = file.readUInt32LE(16);
  // 读记录的总页数。
  if (pageCount !== file.length / PAGE_SIZE)
  // 必须与文件实际长度吻合。
    throw new AccessStoreError('Access catalog page count disagrees with file size', { pageCount, bytes: file.length });
    // 不一致说明文件被改过或写入中断过。
  const payloadBytes = file.readUInt32LE(20);
  // 读有效负载字节数。
  const permissionVersion = file.readUInt32LE(24);
  // 读权限版本。
  const catalogVersion = file.readUInt32LE(28);
  // 读结构版本。
  const updatedAtMs = Number(file.readBigUInt64LE(32));
  // 读写入时间；用 BigInt 读再转 Number（毫秒时间戳在安全整数范围内）。
  const expectedDigest = file.subarray(40, 72);
  // 取出元数据页里记录的 32 字节摘要。
  const dataPages = pageCount - 1;
  // 数据页数量 = 总页数减掉元数据页。
  if (payloadBytes > dataPages * DATA_CAPACITY)
  // 记录的负载长度不能超过所有数据页的容量之和。
    throw new AccessStoreError('Access catalog payload does not fit the recorded pages', { payloadBytes, dataPages });
    // 超出说明元数据自相矛盾。
  const chunks = [];
  // 各数据页的正文。
  let seen = 0;
  // 实际累计到的字节数，用于与元数据里的长度对账。
  for (let index = 0; index < dataPages; ++index) {
  // 逐页校验。
    const base = (index + 1) * PAGE_SIZE;
    // 本页起始偏移。
    if (file.readUInt32LE(base) !== DATA_MAGIC)
    // 校验数据页魔数。
      throw new AccessStoreError(`Access catalog data page ${index + 1} magic mismatch`, { page: index + 1 });
      // 不是数据页就报错并指出页号。
    const pageId = file.readUInt32LE(base + 4);
    // 读页头里记录的页号。
    if (pageId !== index + 1)
    // 页号必须与它的物理位置一致。
      throw new AccessStoreError(`Access catalog data page id mismatch at page ${index + 1}`, { page: index + 1, pageId });
      // 不一致说明页被搬运或错位。
    const byteCount = file.readUInt32LE(base + 8);
    // 读本页有效字节数。
    if (byteCount > DATA_CAPACITY)
    // 不能超过单页容量。
      throw new AccessStoreError(`Access catalog data page ${pageId} declares ${byteCount} bytes`, { page: pageId, byteCount });
      // 超出说明这一页被破坏。
    const slice = file.subarray(base + DATA_HEADER_BYTES, base + DATA_HEADER_BYTES + byteCount);
    // 切出本页正文。
    if (file.readUInt32LE(base + 12) !== fnv1a32(slice))
    // 现场重算 FNV 并与页头里的值比对。
      throw new AccessStoreError(`Access catalog data page ${pageId} checksum mismatch`, { page: pageId });
      // 不一致说明页内容损坏。
    chunks.push(Buffer.from(slice));
    // 复制一份收进结果（subarray 是共享内存的视图，复制可以避免后续误改原文件缓冲）。
    seen += byteCount;
    // 累加实际字节数。
  }
  // 逐页校验结束。
  if (seen !== payloadBytes)
  // 各页字节数之和必须等于元数据里记录的长度。
    throw new AccessStoreError('Access catalog page bytes disagree with the META payload length', { seen, payloadBytes });
    // 不等说明有页缺失或多算，属于损坏。
  const payload = Buffer.concat(chunks);
  // 拼出完整负载。
  if (!sha256(payload).equals(expectedDigest))
  // 用常数时间的 equals 比较整体 SHA-256。
    throw new AccessStoreError('Access catalog payload digest mismatch');
    // 不一致说明负载被篡改过（FNV 只能发现意外损坏，SHA 用于发现刻意修改）。
  let catalog;
  try {
  // 解析可能失败。
    catalog = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(payload));
    // 用严格 UTF-8 解码（遇到非法字节直接抛错）再解析 JSON。
  } catch (error) {
  // 解码或解析失败。
    throw new AccessStoreError('Access catalog payload is not valid UTF-8 JSON', { cause: String(error) });
    // 统一报成"不是合法 UTF-8 JSON"，并把底层原因放在 detail 里备查。
  }
  return { catalog, permissionVersion, catalogVersion, updatedAtMs, pageCount, payloadBytes, formatVersion };
  // 返回解析出的目录与全部元数据，调用方可以据此判断是否需要重新加载。
}

/** 只读 META 页，用于在不解析全部权限数据的情况下比较权限版本。 */
export function readHeader(pagesFile) {
// 只读元数据页：在不解码全部权限数据的前提下比较版本号。
  const meta = Buffer.alloc(META_HEADER_BYTES);
  // 准备 72 字节的元数据头缓冲。
  const handle = readFileSync(pagesFile);
  // 把整个文件读进来（简单直接；调用方只在需要比较版本时用这个函数）。
  handle.copy(meta, 0, 0, Math.min(META_HEADER_BYTES, handle.length));
  // 只把前 72 字节拷进来；用 min 防止文件比 72 字节还短时越界。
  if (meta.subarray(0, 8).toString('latin1') !== META_MAGIC) throw new AccessStoreError('Access catalog magic mismatch');
  // 魔数不符直接拒绝，避免把别的文件当成权限目录。
  return {
  // 返回各项元数据。
    formatVersion: meta.readUInt32LE(8),
    // 格式版本。
    pageSize: meta.readUInt32LE(12),
    // 页大小。
    pageCount: meta.readUInt32LE(16),
    // 总页数。
    payloadBytes: meta.readUInt32LE(20),
    // 有效负载字节数。
    permissionVersion: meta.readUInt32LE(24),
    // 权限版本（调用方最关心的字段）。
    catalogVersion: meta.readUInt32LE(28),
    // 结构版本。
    updatedAtMs: Number(meta.readBigUInt64LE(32)),
    // 写入时间。
  };
}

/** 由 access 文件路径推导页式文件路径；`.json` 后缀被替换为 `.pages`。 */
// 例如 access.json → access.pages；其它后缀则直接在末尾追加 .pages。
export function pagesPathFor(accessPath) {
// 推导页式文件路径。
  return String(accessPath).replace(/\.json$/i, '') + '.pages';
  // 去掉结尾的 .json（大小写不敏感），再补上 .pages。
}

export function writeStore(pagesFile, catalog, options) {
// 把权限目录写入页式文件，采用"先写临时文件再改名"的原子写法。
  mkdirSync(dirname(pagesFile), { recursive: true });
  // 确保父目录存在（recursive 相当于 mkdir -p）。
  const buffer = encodePages(catalog, options);
  // 编码成完整文件内容。
  const temporary = pagesFile + '.tmp';
  // 临时文件名固定加 .tmp，便于崩溃后清理。
  writeFileSync(temporary, buffer);
  // 先把新内容完整写到临时文件。
  renameSync(temporary, pagesFile);
  // 再用改名替换正式文件。改名在同一文件系统内是原子的，
  // 因此任何时刻读到的要么是旧的完整文件，要么是新的完整文件，不会读到写了一半的内容。
  return buffer.length;
  // 返回写入字节数，便于调用方记录与断言。
}

export function readStore(pagesFile) {
// 读并解码页式文件（校验逻辑都在 decodePages 里）。
  return decodePages(readFileSync(pagesFile));
  // 读文件后直接交给解码器。
}

/**
 * 打开权限存储，必要时从旧的 JSON 文件迁移。
 * 优先用页式文件；没有页式文件但有旧 JSON 时才做一次性迁移。
 * 迁移只发生一次：写出页式文件后保留 `.json` 作为 `.json.migrated` 备份，方便回滚。
 * 两者都不存在时用 defaults() 生成的默认目录，并立即落盘。
 */
export function openStore(accessPath, { defaults, normalize }) {
// accessPath 是旧的 JSON 路径；defaults/normalize 由调用方注入，保持本模块与具体权限模型解耦。
  const pagesFile = pagesPathFor(accessPath);
  // 推出页式文件路径。
  if (existsSync(pagesFile)) {
  // 情况一：页式文件已存在，直接用它。
    const state = readStore(pagesFile);
    // 读出并校验。
    return { pagesFile, migrated: false, source: 'pages', ...state, catalog: normalize(state.catalog, state.catalog) };
    // 标记来源是 pages、未发生迁移；并用 normalize 把目录规范成当前结构
    // （第二个参数传自己，表示"没有旧版本可参照"）。
  }
  // 情况一结束。
  if (existsSync(accessPath) && statSync(accessPath).isFile()) {
  // 情况二：有旧的 JSON 文件（且确实是文件而不是目录）。
    const parsed = JSON.parse(readFileSync(accessPath, 'utf8'));
    // 按 UTF-8 读并解析。
    const catalog = normalize(parsed, parsed);
    // 规范化目录结构。
    const permissionVersion = Number.isInteger(parsed.permissionVersion) && parsed.permissionVersion > 0 ? parsed.permissionVersion : 1;
    // 沿用旧文件里的权限版本；没有或非法就从头开始记为 1。
    writeStore(pagesFile, catalog, { permissionVersion });
    // 先把页式文件写出来（这一步成功才算迁移成功）。
    try { renameSync(accessPath, accessPath + '.migrated'); }
    // 再把旧 JSON 改名成 .migrated 作为备份（保留而不是删除，便于回滚）。
    catch { /* 备份失败不影响已经落盘的页式目录。 */ }
    // 备份失败也不回滚：页式文件已经写好，功能不受影响。
    return { pagesFile, migrated: true, source: 'json', catalog, permissionVersion, catalogVersion: 1, updatedAtMs: Date.now() };
    // 标记来源是 json 且确实发生了迁移。
  }
  // 情况二结束。
  const catalog = normalize(defaults(), undefined);
  // 情况三：什么都还没有，用调用方给的默认目录初始化。
  writeStore(pagesFile, catalog, { permissionVersion: 1 });
  // 落盘。
  return { pagesFile, migrated: false, source: 'defaults', catalog, permissionVersion: 1, catalogVersion: 1, updatedAtMs: Date.now() };
  // 标记来源是 defaults。
}

export function removeStore(accessPath) {
// 删除页式文件（主要用于测试清理）。
  const pagesFile = pagesPathFor(accessPath);
  // 推出页式文件路径。
  for (const file of [pagesFile, pagesFile + '.tmp']) {
  // 正式文件与可能残留的临时文件都要删。
    try { unlinkSync(file); } catch { /* 文件可能本来就不存在。 */ }
    // 文件不存在不算错误，直接忽略。
  }
  // 删除结束。
}
