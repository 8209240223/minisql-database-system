export function calculateVirtualRange(options: {
// 虚拟滚动参数：总行数、行高、视口高度、滚动位置与 overscan。
  rowCount: number;
  // 结果总行数。
  rowHeight: number;
  // 每行像素高度。
  viewportHeight: number;
  // 视口像素高度。
  scrollTop: number;
  // 当前滚动偏移。
  overscan?: number;
  // 上下额外预渲染行数，默认 8。
}) {
// 根据总行数、行高、视口高度和滚动位置计算需要渲染的行区间。
  const rowHeight = Math.max(1, options.rowHeight);
// 行高至少为 1，避免除零。
  const overscan = Math.max(0, options.overscan ?? 8);
// 预渲染的额外行数，默认上下各留 8 行。
  const rowCount = Math.max(0, options.rowCount);
// 总行数不能为负。
  const start = Math.max(0, Math.floor(Math.max(0, options.scrollTop) / rowHeight) - overscan);
// 起始行 = 当前滚动行减去上侧 overscan。
  const visible = Math.ceil(Math.max(0, options.viewportHeight) / rowHeight) + overscan * 2;
// 可见行数 = 视口行数加上下 overscan。
  return { start, end: Math.min(rowCount, start + Math.max(1, visible)) };
// 返回闭区间起止；结束行不超过总行数。
}
