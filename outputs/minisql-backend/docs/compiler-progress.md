# 编译到逻辑计划阶段记录

日期：2026-09-08。对应需求基线：MiniSQL V3。本文为已运行的局部测试记录，不代表全部扩展完成。

## 本次实现

1. 编译入口接入 Catalog 语义检查和逻辑计划生成，保留仅解析模式用于隔离 Parser 测试。
2. 语义检查包括大小写不敏感的表列查找、重复表/列、INSERT 数量/重复目标/完整列覆盖/类型、INT32 范围、条件列与 BOOL 类型。
3. compilePlans 复制正式 Catalog，按顺序验证语句；前面的 CREATE 对后面可见，但成功或失败均不修改调用方 Catalog。
4. 生成 CreateTable、Insert、SeqScan、Filter、Project、Delete。INSERT 值按表结构重排并解码字符串；Project 保留列顺序和重复列；Delete 输入保留行定位标记。
5. 计划 JSON 保存父子节点、语句序号、深度、类型、列序号和绑定条件；尚未执行的阶段明确标记未实现。
6. HTTP 适配层增加能力查询，返回引擎身份、阶段能力和空目录副本模式；中文输入输出不再直接逐 Buffer 转字符串拼接。

## 已验证

| 检查 | 实际结果 |
| --- | --- |
| g++ C++20 独立编译器构建 | 退出码 0 |
| tests/parser-regression.mjs | 12 项通过 |
| tests/planner-regression.mjs | 25 项通过 |
| tests/planner_contract.cpp 独立构建及运行 | Catalog 成功/失败路径隔离通过 |
| tests/bridge-regression.mjs | 7 项场景通过，临时服务使用系统分配端口并在结束时退出 |

构建使用本机 g++，头文件来自工作区提取的 nlohmann/json；完整 vcpkg/CMake 构建未在本轮验证。CMake 已登记 planner 库和快照隔离测试，但不据此声称完整 CTest 通过。

## 仍未完成

1. 执行器、数据页、缓存池、持久化目录和重启验证；HTTP execute 仍返回 501。
2. 优化器、27 组完整扩展与 DataGrip 工作台完整联调。
3. 当前列绑定使用表内列序号，不是持久化 columnId；tableId、schemaVersion、旧计划失效检查仍需实现。
4. 表/INSERT 等错误仍可能定位到语句起点；所有 AST 节点的完整源码范围和多错误恢复尚未补齐。
5. JSON 是输出接口，尚未实现反序列化、未知版本拒绝与完整往返测试。
6. HTTP 目录当前为空，仅支持同批 CREATE 为后续编译提供 Schema；不应把这种演示编译模式当成可用的持久数据库会话。
7. 旧后台服务若仍运行，不会自动重新加载桥接源码；本轮联调使用新启动的隔离进程，不声称旧端口已更新。

## 后续依赖

先实现 Page/File/Buffer 与页式 Catalog，再接四类执行算子和重启测试；扩展按 V3 的类型、表达式、查询、索引、恢复及并发依赖推进。临时通过局部测试不能降低 V3 的最终目标。
