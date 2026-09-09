# 单列、复合外键与 RESTRICT

日期：2026-09-08。对应 EXT-SQL-011 的部分能力。

## 一、实现范围

1. 列定义支持 REFERENCES parent(column)，表定义支持 FOREIGN KEY(c1,c2,...) REFERENCES parent(p1,p2,...)。父表必须已经存在，或为当前正在创建的表自身；目标列组须按声明顺序完整匹配一个 PRIMARY KEY/UNIQUE。单列目标也可以使用列级 PRIMARY KEY/UNIQUE。复合键的单个分量不能充当外键目标。
2. 两端列数非零且相等，各端不允许重复列；对应列使用相同类型，拒绝隐式字符串与数字转换。项目采用 MATCH SIMPLE 语义：子键任一分量为 NULL 时不要求匹配父行。DEFAULT 与 CAST 产生的非空值同样检查。
3. INSERT/UPDATE 在物理写入前扫描父表，所有非空子键分量必须同时匹配同一条父行，不能由不同父行分别满足。父表 DELETE 和修改被引用键组时执行 RESTRICT；其他字段更新与不改变键组的更新允许。
4. 父表语句的所有候选行检查通过后才开始写入，约束失败不会留下先前候选行的部分变更。RESTRICT 不允许以同条语句中其他行移入相同键值为理由删除或更改正在被引用的原键。
5. 列描述符 v4 保留原 references 对象或 null；表描述符 v3 新增 foreignKeys 数组，并保留旧版表描述符 v1/v2 和无包装旧名称的读取。每个外键保存 columns、table、referencedColumns 三个字段。Catalog 重新加载时校验字段结构及全部引用语义，写入前预编码，超出目录记录长度时拒绝而不是截断。
6. Statement/CREATE 计划的 foreignKeys 表示表级声明，原列级声明仍在 columns.references。Catalog API 的 foreignKeys 为两者合并后的有效外键清单，columns.references 同时保留用于旧客户端兼容；消费者不要再将二者重复合并。执行器统一经 allForeignKeys 处理两种来源。优化器资源计数覆盖外键及两端列组。

## 二、验证

foreign-key-process.mjs 覆盖单列写入、RESTRICT、默认值和元数据。composite-foreign-key-process.mjs 本轮 51 项检查通过，覆盖跨进程重启、错配键组、子表 INSERT/UPDATE、父表 DELETE/UPDATE、NULL 分量、INT64 精确匹配、默认值、单列与复合外键共存、列序、非法定义，以及 AST/计划/API 元数据。旧程序首先在复合外键建表处失败，新程序通过同一回归。

HTTP 回归新增复合外键建表、非法子键、父键更新/删除、目录列组元数据断言。前端类型登记 foreignKeys，不新增独立外键编辑界面；SQL 编辑器继续通过真实执行接口使用该能力。本记录不将接口测试称作浏览器视觉检查，未生成图片。

复合外键落地时的验证记录：复合外键专项 51 项、单列外键 36 项、CHECK 68 项、行/堆/目录 1595 项和 HTTP 回归通过。TypeScript/Vite 生产构建通过，仍保留已有大于 500 kB 包体积提示。自引用新增后的专项证据另见 self-foreign-key-progress.md；两轮证据均不证明多行 VALUES 的事务原子性。测试使用独立数据库文件，不修改用户数据库。

## 三、未完成范围

自引用现已实现，具体候选状态与 RESTRICT 规则及 63 项专项测试见 self-foreign-key-progress.md。约束命名已接入，见 named-constraint-progress.md，当前表描述符写入 v4，保留 v3 读取。显式 ON DELETE/ON UPDATE 子句仍未实现；RESTRICT 是固定项目行为，级联动作不自动加入已承诺范围。索引加速、事务、多进程锁、WAL 和多行 VALUES 撤销仍待完成，不能把 HTTP 服务串行请求当成数据库并发控制。

跨表引用仍要求父表先创建，目录按既有记录顺序加载；后续支持跨表循环引用或目录重排迁移时需要两阶段解析引用。单张表内记录的循环引用已覆盖。测试只在独立数据库运行，不修改用户数据库。
