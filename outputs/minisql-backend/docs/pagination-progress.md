# 分页实施记录

日期：2026-09-08。对应 EXT-SQL-005 的 LIMIT/OFFSET 子范围。

## 实现

SELECT 末尾支持 LIMIT 非负整数、LIMIT 非负整数 OFFSET 非负整数，以及独立 OFFSET 非负整数。参数使用 UINT64 并检查越界，拒绝负数、字符串、小数、重复子句和 OFFSET 后再写 LIMIT。

计划增加 Limit 节点，在 Project 或 Distinct 之上执行。LIMIT 0 保留列结构而不访问子计划；其他情况目前先取得子计划结果，再按偏移和数量截取，计算时使用剩余数量避免 offset 加 limit 溢出。

AST 与计划 JSON 中计数以十进制字符串输出，避免浏览器 Number 无法精确表示 UINT64 大值。未设置 limit 使用 null；该协议约定需在未来前端集成中保留。

## 验证

optimizer_contract 联合测试增至 87 项，通过新增的 15 项分页检查：条数、临近末尾、仅 OFFSET、LIMIT 0、极大参数、DISTINCT 后分页、计划层级及七类非法参数。

独立编译器重建后 25 项语义/计划回归通过。此测试范围不证明排序或流式性能已实现。

## 尚未完成

ORDER BY、NULL 排序、稳定分页所需的显式排序、外部排序、流式限量读取和取消均未接入。当前 LIMIT 不限制扫描和结果构建的内存占用，除零条快速路径外仍先物化子结果。EXT-SQL-005 保持部分实现，完整目标不变。
