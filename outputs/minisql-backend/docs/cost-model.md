# 成本模型：公式、默认值与确定性决胜规则（X18）

> 成员 A 交付物（任务书 A5-④）。实现见 `src/execution/database.cpp`
> （`estimate` / `columnSelectivity` / `indexScanSelectivity` / `annotatePlanEstimates`）与 `src/optimizer/optimizer.cpp`。

## 一、行数与成本公式（算子）

记 `L`/`R` = 左右子节点估计行数，`CL`/`CR` = 左右子节点累计成本，`N` = 表行数，`sel` = 过滤选择率。

| 算子 | 估计行数 | 累计成本 |
| --- | --- | --- |
| SeqScan | `N` | `N + 0.1·N` |
| IndexScan | `N × indexScanSelectivity` | `rows + 0.05·rows` |
| Filter | `R × sel(predicate)` | `C + rows` |
| Sort | `R` | `C + R·log2(max(1, R))` |
| Limit | `min(R, limit)` | `C` |
| Distinct | `0.5·R` | `C + 0.5·R` |
| Aggregate | 无分组键 `1`；否则 `min(0.1·R, 1000)` | `C + R` |
| HashJoin | `L × R` | `CL + CR + (L + R)` |
| NestedLoop / Left / Right / Full Join | `0.1 · L × R` | `CL + CR + 0.1·L·R` |

无子节点算子回落 `{0, 1}`；未列出的算子回落 `{R, C + R}`。

## 二、选择率 `columnSelectivity`

对 `Identifier ⋈ Literal` 谓词：

- **布尔字面量**：`NULL` / `false` → 0；`true` → 1；其它非布尔字面量 → 0.25。
- **范围**（`<` `<=` `>` `>=`）+ 数值列 + 存在直方图：按等宽桶累计 `fracLt` / `fracLe`，返回 `fracLt` / `fracLe` / `1-fracLe` / `1-fracLt`。
- **等值**（`=`）：列有 `distinctCount > 0` 时返回 `1/distinct`；字面量为数值且落在 `[min, max]` 之外 → 0。
- **`IS NULL` / `IS NOT NULL`**：用列 `nullRatio` 与其补。
- **逻辑组合**：`AND` 选择率相乘；`OR` 为 `min(1, l + r − l·r)`；`NOT` 为 `1 − sel`。
- **无统计回落**：比较运算 `0.33`；其它 `0.25`。

`indexScanSelectivity`：等值前缀逐列 `1/distinct`（越界归零），范围列走直方图，逐列连乘；合成列谓词与 `columnSelectivity` 同构，保证口径一致。

## 三、缺失统计的默认值

- 无统计表：`estimatedTableRows` 返回空 → `tableRows` 取 `0.0`。
- 无直方图 / 无 distinct：等值回落 `0.33`、其它 `0.25`。
- 计划节点统一标注 `statsSource: "stats-v1"`、`estimatedRows`、`estimatedCost`。
- 统计来源：`statistics()` 优先使用显式 `ANALYZE` 的持久快照（`source: "analyze"`），否则实时扫描（`source: "on-demand-scan"`）；`ANALYZE` 快照在任何写语句后被删除。

## 四、确定性决胜规则

- 候选计划比较后，**同成本必须由固定规则决定**，不依赖容器迭代顺序或随机值。
- **join 选择**：`L + R <= L × R` → HashJoin，否则 NestedLoopJoin；**相等时固定偏好 HashJoin**。旁路谓词下推收窄后可双向重判（可能撤销回 NestedLoopJoin）。
- 相同输入 + 相同统计的重复编译产出**逐字节一致**的 `optimizedPlan`（`deterministic serialization` 回归与 `statistics-costjoin-smoke` 的跨编译一致性断言）。

## 五、EXPLAIN / EXPLAIN ANALYZE

- `EXPLAIN`：只读，返回 `estimatedRows` / `estimatedCost` / `statsSource`，**不执行**目标语句。
- `EXPLAIN ANALYZE`：只读运行 SELECT，返回 `actualRows` / `durationMs` / 页读写（`hits` / `misses` / `diskReads` / `diskWrites`）等。
- 读页与实际页差按 `diskScope: "database-file-pages-including-header"` 记录；**不伪造**固定加速倍数。
