# 页写入暂存与回滚

日期：2026-09-08。服务于 EXT-SYS-002、EXT-SYS-003 及多行写入原子性，是存储前置机制，不是完整事务实现。

## 一、已实现的机制

PageFile 在写入批次开始时保存页数量、活动页及代次、所有者映射和空闲列表。批次内 writeRaw 不覆盖原文件，而按页编号更新内存写集合；readRaw 优先读取集合中的候选页。新页分配、旧页释放、空闲页复用及文件头更新均走这一入口，不因缓存淘汰或 flush 提前落盘。

BufferPool.beginWriteBatch 先确保没有固定页并刷新批次前的脏页，再开启暂存。rollbackWriteBatch 同样要求所有页解除固定，恢复 PageFile 的原目录后丢弃缓存，防止之后 flush 把已回滚的候选页重新写出。嵌套批次和无活动批次回滚明确报事务错误。

新增 stagedPageReads、stagedPageWrites，区分暂存集合访问与文件访问；未开启批次时沿用现有统计行为。统计不会因回滚自动清零，仍反映发生过的访问。

批次产生的内部 PageRef/RowRef 在回滚后必须丢弃，不允许作为已提交标识交付调用方；现阶段没有承诺跨回滚和后续重新分配后的标识永不复用。集成层必须通过唯一的 BufferPool 管理批次，不能直接回滚 PageFile 后继续使用旧缓存。

## 二、验证

write_batch_contract 通过 24 项断言，覆盖单页缓存、多页淘汰、变长行更新、新页和复用页、释放与恢复、固定页拒绝、嵌套拒绝、读自己的暂存修改、回滚后的原记录与目录、后续 flush 不恢复脏页，以及对象关闭后重新打开。

测试直接读取页文件并按完整字节比较，不仅比较查询结果。write-batch-process.mjs 额外启动子进程，在暂存十条大记录且发生淘汰后直接以状态 73 退出，不运行析构；主进程验证文件字节未变，再用另一新进程确认只存在原数据。

原有 storage_contract 31 项和 buffer_contract 35 项重新构建后通过。未开启写入批次时，页持久化、缓存替换、固定页保护及 I/O 错误行为继续由这些用例覆盖。

构建入口增加 batch-test、storage-test、buffer-test，CMake 注册对应的新批次契约测试。当前验证使用本机构建脚本；没有把 CMake 配置存在视为 CMake 构建已通过。

## 三、提交与 SQL 接入更新

1. 已实现 commitWriteBatch、带校验与数据库身份的整页重做日志、日志先行同步、发布提交点、启动恢复及日志截断同步。详见 journal-progress.md。
2. 已实现数据库旁路锁文件的原生独占句柄；第二个协作进程不能同时打开同一数据库，进程退出后锁自动释放。硬链接数据库被拒绝；不承诺防止外部非协作程序直接改写数据文件。
3. SQL 执行器已为 CREATE TABLE、INSERT、UPDATE、DELETE 自动开启并提交页写入批次。提交点前失败回滚页和缓存，并重新加载持久化目录；提交点后失败禁用当前实例，要求重新打开恢复。
4. 默认多语句脚本仍按语句提交。BEGIN/COMMIT/ROLLBACK 已接入显式事务，事务内语句共用批次，见 transaction-progress.md；HTTP 跨请求会话仍未开放。事务外 SELECT 不开启写入批次，多行 VALUES 共用单条 INSERT 的批次。
5. 内存写集合限制为 16384 页，尚无溢写与统一内存预算；不声明支持任意大小事务。进程故障注入不等于真实硬件断电验证。

database_contract 新增 SQL 提交故障与目录恢复检查，连同原有场景共 47 项通过，覆盖单帧缓存、四种写语句回滚、回滚后表名复用、提交点后拒绝查询/编译/目录访问、重新打开恢复数据与建表约束。

完整项目目标不变，以上前置机制通过不等于 X11、X21 或 X22 验收通过。

## 四、实现参考

Windows 文件独占与同步实现使用 CreateFileW、FlushFileBuffers 和 MoveFileExW；实现位于 src/storage/file_io.cpp。参考 [CreateFile 官方文档](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilea) 和 [MoveFileEx 官方接口说明](https://github.com/MicrosoftDocs/sdk-api/blob/docs/sdk-api-src/content/winbase/nf-winbase-movefileexw.md)。POSIX 分支尚未在本机验证。
