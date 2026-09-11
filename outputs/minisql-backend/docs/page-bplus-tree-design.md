# MiniSQL B1 / X20：页级 B+ 树索引设计

版本：D0.1（设计阶段）
日期：2026-09-09
归属：成员 B，对应分工任务书 B1（X20，页级 B+ 树索引）
基线：`docs/index-page-storage-progress.md`（当前为“整棵树分页镜像”）

## 1. 背景与目标

### 1.1 现状

- 索引在内存中维护一棵 `BPlusTree`（`include/minisql/storage/bplus_tree.hpp`），节点是 `std::unique_ptr<Node>` 组成的内存树。
- 持久化方式：`BPlusTree::dump/restore` 把整棵树序列化成一份 JSON，`Database::persistIndexPages` 按 3800 字节分块写入页文件中该索引的 owner 页（`IXPAGE` 记录）；启动时 `loadIndexPages` 重组 JSON 并 `restore` 回内存树。
- 查询（IndexScan）完全走内存树：`tree.search` / `tree.range`，内部沿 `Node* children` 指针递归。
- Catalog 的 index 项已暴露 `storage=page-file`、`pageCount`、`height`。

### 1.2 目标差距（任务书 B1 要求）

当前不是“一棵 B+ 节点一页、查询时按子页指针逐页遍历”。具体缺口：

1. 没有层级化、独立的索引页类型（`INDEX_META / INDEX_INTERNAL / INDEX_LEAF`）。
2. 查询不按“根页 → 内节点页 → 叶页”逐页遍历，仍依赖整树内存镜像。
3. 删除低于阈值时的页内借位/合并由整树重写承担，没有页级原地更新。
4. 没有页级结构校验命令（页类型、兄弟指针、键顺序、叶链）。

本设计将其升级为**页级节点持久化**的 B+ 树，复用现有 BufferPool / PageFile，同时保留与当前格式的兼容与回滚路径。

## 2. 总体方案

新增页级引擎 `PageBPlusTree`（新增头文件与源文件，作为新增能力模块），接口语义与现有内存 `BPlusTree` 对齐（`insert/search/range/size/height/validate`），但节点落在页文件里，子指针对应到页（`PageRef`），查询时按页读取。

- 一棵索引 = owner 下一个 `INDEX_META` 元页 + 若干 `INDEX_INTERNAL` / `INDEX_LEAF` 节点页。
- 每个节点独占一个 4096 字节页，页内是单一二进制节点记录（非堆槽多记录）。
- 叶节点带上一节指向右兄弟页，`range` 通过叶链顺序遍历；内节点按下/上边界键找对应的子页指针，逐页下钻。
- 插入发生分裂时递归向父节点页传播；删除低于下界时先向右/左兄弟借位，再合并。
- 索引页变更与堆页变更在事务里进入同一 `beginWriteBatch/commitWriteBatch`，回滚时整批丢弃，不产生孤立索引项。

> 采用增量落地：第一落地点用一个自包含的 `PageBPlusTree` + 契约测试证明页级遍历、分裂、借位/合并、重启恢复；后续再把 `Database` 的索引从内存树切换到页级引擎。

## 3. 索引页格式规范

### 3.1 页类型编号（写入页记录头部）

| 值 | 名称 | 含义 |
| --- | --- | --- |
| `0x50` | `INDEX_META` | 每个索引一个，记录树元信息与根页 |
| `0x41` | `INDEX_INTERNAL` | 内节点页（含根非叶时也是内节点） |
| `0x42` | `INDEX_LEAF` | 叶节点页 |

> 占 1 字节；任何读到的页若首字节不在上述集合，视为损坏，拒绝并报 `ErrorCode::Storage`。

### 3.2 页内节点记录布局（大端独立、固定宽度优先；字符串/复合键按长度编码）

所有页共用 8 字节页头（对齐到 4）：

```
offset  size  字段
0       1     pageType (INDEX_INTERNAL=0x41 / INDEX_LEAF=0x42)
1       1     formatVersion = 1
2       2     keyCount (uint16 LE)
4       4     height (uint32 LE；叶=0，内节点 = 子树层数-1)
8       8     parentPageId (0 = 无父)
16      8     parentGeneration
24      8     leftPageId  (叶链/兄弟借位用；0 = 无)
32      8     leftGen
40      8     rightPageId (0 = 无)
48      8     rightGen
56      ...   payload
```

`INDEX_META` 页（独立布局，其余字段不用于节点算法）：

```
offset  size  字段
0       1     pageType = INDEX_META
1       1     formatVersion = 1
2       2     maxKeys (uint16 LE)
4       4     unique (uint32 0/1)
8       8     size (行数)
16      8     rootPageId
24      8     rootGeneration
```

#### payload（`INDEX_LEAF`）

逐项紧凑拼接：

```
keyCount 组：每组 = keyCountValues(byte) + 各 value 编码
keyCount 组：每组 = RowRef(28 字节：pageId8 + pageGen8 + slot4 + slotGen8)
```

#### payload（`INDEX_INTERNAL`）

```
keyCount 组：每组 = keyCountValues(byte) + 各 value 编码
(keyCount+1) 个 child 指针：每组 = PageRef(16 字节：pageId8 + generation8)
```

内节点键 `k[i]` 是分隔键：`child[i] 子树所有键 < k[i] <= child[i+1] 子树所有键`（与现有内存树 `upper_bound` 下钻一致）。

### 3.3 value 编码（二叉）

复用 `heap.hpp` 的 `Value` 变体（顺序：int32, string, monostate, int64, bool, double）：

| type 字节 | 负载 |
| --- | --- |
| `0x69` 'i' | int32，4 字节 LE |
| `0x6c` 'l' | int64，8 字节 LE |
| `0x64` 'd' | double，8 字节 LE |
| `0x62` 'b' | bool，1 字节 |
| `0x73` 's' | string：uint32 len + 原始 UTF-8 |
| `0x6e` 'n' | NULL（monostate） |

比较逻辑与 `bplus_tree.cpp` 的 `compareScalar`/`IndexKey::compare` 完全一致（保证顺序兼容）。

### 3.4 页校验

- 页内字段长度必须落在页容量内；解析越界返回损坏。
- `BufferPool.get` 返回的 `SlottedPage` 自带代次/校验（`page.hpp`），页级校验命令额外核对节点内 keyCount、height、兄弟指针环形与叶链完整性。

## 4. 算法

### 4.1 根页定位

读 `INDEX_META` → `rootPageId/rootGeneration`。任意操作起始于根页。

### 4.2 IndexScan（根 → 叶逐页下钻）

1. 读根页。若非叶：用 `upper_bound` 定位应下钻的 child 指针，读取子页，重复至叶。
2. 三点查：在叶页内 `lower_bound` 起点；单点 `search` 在该叶内线性找等值并沿叶链继续（处理重复键）。
3. 范围查 `range(lower,upper)`：定位起始叶，从该叶沿 `rightPageId` 叶链条按序收集，直到键超过上界或叶链结束。返回顺序即键序。

### 4.3 插入与分裂（递归向父传播）

叶子先按 `lower_bound` 插入；若 `keyCount > maxKeys`，取中位分裂成左右两叶，用 right 的首键作为分隔键，把分隔键 + 右页指针交给父。父（内节点）插入后若超 `maxKeys` 同样分裂并向上一级传播：

- 内节点分裂：左保留 `columns[0..mid-1]`（mid 个键 + mid+1 个子指针），`columns[mid]` 上提为分隔键，右侧平移到新页。根树分裂时新建根内节点页并更新 `INDEX_META`.
- 唯一索引冲突：插入前对插入键做叶页 `search`，命中即返回 false（与现有内存树 `unique` 语义一致）。

### 4.4 删除、借位与合并（页级原地更新）

叶子删除后若 `keyCount` 低于下界 `floor((maxKeys+1)/2)`：

1. 右兄弟可借：把右兄弟首个条目移到本叶末尾，用右兄弟新的首键更新父分隔键。
2. 左兄弟可借：把左兄弟末尾条目移到本叶开头，用本叶新的首键更新父分隔键。
3. 不可借：与兄弟合并（小页并入兄弟），删除空页并回收，从父节点页移除对应分隔键 + 子指针；若父因此低于下界，递归重复借位/合并。仅剩子节点时收缩根（`height` 减一，更新 `INDEX_META`）。

删除同时清理堆行引用所在页计数，保证 `pageCount` 准确。

### 4.5 事务/WAL 一致性

索引页修改必须与堆页同一写批次（`PageFile::beginWriteBatch/commitWriteBatch`）。事务回滚时 `rollbackWriteBatch` 丢弃整批新页与修改，不产生孤立索引项。页级实现沿用现有 PageFile 的整页 WAL + 代次校验，保证崩溃后要么整体生效要么整体回滚。

## 5. 接口面（新增）

```cpp
class PageBPlusTree {
public:
    PageBPlusTree(std::shared_ptr<PageFile> file, BufferPool& buffer,
                  std::uint64_t owner, std::size_t maxKeys, bool unique);
    bool create();                      // 分配 INDEX_META 页
    bool exists();                      // 元页已存在
    bool insert(IndexKey key, RowRef row);
    std::vector<RowRef> search(const IndexKey& key);
    std::vector<RowRef> range(const std::optional<IndexKey>& lower, bool lowerInclusive,
                              const std::optional<IndexKey>& upper, bool upperInclusive);
    std::size_t size() const;
    std::size_t height() const;         // 读元页/根页推导
    std::size_t pageCount() const;      // 该 owner 的页数量
    bool validate();                    // 页类型/兄弟指针/键序/叶链
private:
    std::shared_ptr<PageFile> file_;
    BufferPool& buffer_;
    std::uint64_t owner_;
    std::size_t maxKeys_;
    bool unique_;
    std::optional<PageRef> meta_;
};
```

`IndexKey::compare` 直接复用现有 `bplus_tree.cpp` 的 `IndexKey::compare`。

## 6. 兼容、迁移与回滚

| 场景 | 策略 |
| --- | --- |
| 旧 JSON 快照页（当前 `IXPAGE` 分块） | 保留 `Database::loadIndexPages`（内存树 + JSON）路径作为回滚/兼容入口；页级引擎与旧格式二选一，由 `MINISQL_INDEX_ENGINE` 或格式版本选择。 |
| 旧索引无法作为页级树读取 | 检测到旧格式时触发 `MINISQL_REBUILD_INDEXES` 从堆表重建，重建后写新页级格式。 |
| 页级引擎发现损坏页 | 明确拒绝（不得静默返回错误结果），提供从堆表重建入口。 |
| 回滚 | 页级引擎未合入主路径前，现有内存树 + 44 项 `index-smoke` 全量保持在绿色；页级作为新增能力逐步替换。 |

## 7. 校验命令与校验点（任务书 B1 验证）

- 页级结构校验输出：页类型、height、keyCount、兄弟指针、叶链长度、根页可达。
- 契约测试覆盖：根分裂/多级树、重复键拒绝、复合键与前缀范围、删除触发借位与合并、重启（重建 BufferPool 后重新加载）后叶链与根页可恢复、索引与全扫一致、损坏页类型明确拒绝。

## 8. 主要改动文件

| 文件 | 内容 |
| --- | --- |
| `include/minisql/storage/page_bplus_tree.hpp`（新增） | `PageBPlusTree` 接口与页类型常量 |
| `src/storage/page_bplus_tree.cpp`（新增） | 节点序列化、分裂、借位/合并、页级遍历 |
| `include/minisql/storage/bplus_tree.hpp`（不改，复用 `IndexKey::compare`） | — |
| `CMakeLists.txt` | `minisql_storage` 追加新源文件 |
| `tests/page_bplus_tree_contract.cpp`（新增） | 页级契约测试，加入 CTest |
| `tests/index-smoke.mjs`（后续） | 切换主路径后补充页级断言 |

## 9. 完成判据（对任务书 B1）

- 页级契约测试通过（分裂、借位/合并、重启、损坏拒绝）。
- IndexScan 逐页遍历、`storage=page-file`、`pageCount/height` 与 `database-http.mjs` 回归全绿。
- 保持既有 `bplus_tree_contract.exe` 全绿（公开 API 不变）。