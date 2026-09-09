# 真实执行阶段记录

日期：2026-09-08。对应 V3 第六章及第 17.6 节，不代表全部扩展已经完成。

## 新增能力

1. Database 将 Lexer、Parser、语义检查、逻辑计划、PersistentCatalog、HeapStore、BufferPool 与 PageFile 接通。
2. CreateTable 实际登记目录表；Insert 使用计划中已按 Schema 重排的值；Project/Filter/SeqScan 读取真实记录；Delete 收集匹配 RowRef 后执行删除，避免在固定页的扫描回调内释放页。
3. 比较和 NOT/AND/OR 使用绑定列序号求值，AND/OR 短路；输出保持投影顺序及重复列名。无匹配行时仍返回列名。
4. compile 使用真实 Catalog 的副本，不发布编译建表。execute 返回逐语句结果与已完成数量，语法/语义错误不撤销先前提交。
5. 新增 minisql_database 命令行程序，参数为数据库页文件和 execute、compile 或 catalog，SQL 通过标准输入传入，标准输出为 JSON。此入口不替代已有主程序，后续需统一 CLI 和 HTTP 生命周期。

## 已验证

严格 g++ C++20 构建开启 Wall/Wextra/Werror，编译器退出码 0。

tests/database_contract.cpp：22 项检查通过，覆盖四类 SQL、INSERT 列映射、带分号及转义引号字符串、布尔条件、重复投影列、空结果结构、类型错误、先成功后语义失败的提交状态、真实目录编译、关闭重开及删除持久性。

tests/database-process.mjs：分别启动独立进程建表插入、查询、读取目录、只编译、删除及再次查询，用于验证进程结束后的页式持久化，而不是仅重建同一进程中的对象。

## 未完成和限制

1. HTTP bridge 仍指向旧编译入口，execute 仍返回未实现；前端尚不能调用本次 Database 实现。不要把 CLI 测试作为 HTTP/前端已完成的证据。
2. 已改为共用 scanTokens 流式词法回调，在真实分号处逐句执行；尾部词法、语法或语义错误保留前面已提交结果。compile 仍整批检查、无表数据副作用。
3. 查询结果和待删除 RowRef 目前在内存收集，无结果预算、分页、取消及外部溢写；单线程，无事务、锁和 WAL。
4. 计划暂不包含稳定 tableId/schemaVersion 的执行前失效检查；当前只执行刚生成的计划，未开放外部计划执行。
5. 无优化器及 27 组完整扩展；错误源码精度、资源上限和统一阶段输出仍待补齐。
6. 命令行 compile/catalog 在不存在路径上会初始化空数据库文件；编译不会创建 SQL 表，但严格的无文件副作用只读打开模式仍需实现。
7. 已登记 CMake 目标，本轮未运行完整 vcpkg/CMake/CTest 构建。正常文件刷新不代表断电原子恢复。

## 批次边界修复验证

执行回归增至 29 项并通过，新增末尾非法字符、未闭合注释、缺失末尾分号、跨行字符串拒绝、绝对行列位置及先前提交内容核对。词法/语法 12 项、语义/计划 25 项重新通过。新扫描入口与 tokenize 共用一套字符串、注释和位置规则，不使用按字符串分号切分的备用实现。
