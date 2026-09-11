export function calculateVirtualRange(options: {
  rowCount: number;
  rowHeight: number;
  viewportHeight: number;
  scrollTop: number;
  overscan?: number;
}) {
  const rowHeight = Math.max(1, options.rowHeight);
  const overscan = Math.max(0, options.overscan ?? 8);
  const rowCount = Math.max(0, options.rowCount);
  const start = Math.max(0, Math.floor(Math.max(0, options.scrollTop) / rowHeight) - overscan);
  const visible = Math.ceil(Math.max(0, options.viewportHeight) / rowHeight) + overscan * 2;
  return { start, end: Math.min(rowCount, start + Math.max(1, visible)) };
}
