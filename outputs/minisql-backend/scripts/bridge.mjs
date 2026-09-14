// Compatibility entry point retained for callers that still launch scripts/bridge.mjs.
// The real database bridge owns HTTP, execution, persistence, Catalog and authorization.
console.warn('scripts/bridge.mjs is deprecated; forwarding to scripts/database-bridge.mjs');
await import('./database-bridge.mjs');
