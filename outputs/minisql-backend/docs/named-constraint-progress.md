# 约束命名实施记录

日期：2026-09-08，对应 EXT-SQL-011。

## 一、语法与名称范围

支持在列级 PRIMARY KEY、UNIQUE、CHECK、REFERENCES、NOT NULL 前，以及表级 PRIMARY KEY、UNIQUE、CHECK、FOREIGN KEY 前添加 CONSTRAINT name。名称采用当前未加引号标识符规则；CONSTRAINT 为保留关键字。

名称在同一表内不区分大小写且唯一，不同表可以复用。每个实际约束最多一个名称；多个 CHECK 可以分别命名。不为未命名约束自动生成名称。命名 DEFAULT 或 NULL 声明不在本项目语法范围内，明确报错。

## 二、元数据契约

Statement、CREATE 计划、Catalog 表对象及目录 API 新增 constraintNames 数组。每项固定包含 name、kind、index；index 从零开始，不是全局约束编号。

| kind | index 指向 |
| --- | --- |
| key | keys 中的表级主键或唯一键 |
| check | checks 中的表达式，列级和表级统一排列 |
| foreignKey | 表级 foreignKeys 数组 |
| primaryKey | columns 中带 PRIMARY KEY 的列 |
| unique | columns 中带 UNIQUE 的列 |
| references | columns 中带 REFERENCES 的列 |
| notNull | columns 中不可空的列 |

目录 API 的 foreignKeys 包含表级外键及兼容列级外键；其中表级项排在前面。constraintNames 的 foreignKey 索引只指表级项，references 索引仍指 columns，不能把两种索引混用。

表描述符写入版本升级为 v4，新增 constraintNames，读取保留 v1/v2/v3 及原始表名格式兼容；列描述符仍为 v4。旧表得到空名称数组，不自动改名。重载检查字段类型、非负整数索引、目标范围、目标约束是否存在、名称唯一性及一对一绑定；非法元数据拒绝加载。

## 三、错误诊断

具名约束违反时在原错误文本后附加方括号名称，例如 [pk_child]；错误码与错误阶段保持原语义。支持唯一键/主键重复、CHECK 失败、NULL 及省略必填列、子表外键失败、父表 RESTRICT，以及自引用最终状态失败。父表 RESTRICT 报的是子表外键名称，不是父键名称。

一条 SQL 同时违反多个约束时报告检查顺序中首先发现的一项，不承诺列出全部违规项。列同时具有具名 NOT NULL 和主键时，NULL 诊断优先使用显式 NOT NULL 名称。类型错误不自动归因于唯一键或 CHECK。

## 四、验证与边界

named-constraint-process.mjs 先在旧程序的具名主键建表处失败，修复后通过 50 项。覆盖列级及表级语法、重复名称、跨表同名、NOT NULL、CHECK、单列/复合外键、父表 RESTRICT、自引用、AST/计划一致性、目录 API 和跨进程重启。目录 API 同时返回原始 checks 定义，保证 check 名称索引有可定位的目标。

heap_catalog_contract 重建后通过 1608 项，其中新增 13 项名称映射检查，覆盖保留合法映射、非法名称、越界或不存在的约束目标、大小写重复和单个约束重复命名。

关联验证：自引用专项 63 项、HTTP 回归及 TypeScript/Vite 构建通过。HTTP 覆盖具名主键失败名称、具名 CHECK 失败名称和目录名称映射；前端仍有原有大于 500 kB 包体积提示。未执行浏览器视觉检查。

现有工作台通过真实 SQL 接口使用该语法，前端类型已增加名称映射，不新增独立约束编辑器。ALTER TABLE、重命名/删除约束、多行 VALUES 原子性、事务和 WAL 仍待完成。该功能不等于 EXT-SQL-011 整体完成；未生成图片。
