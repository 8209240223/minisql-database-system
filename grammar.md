# MiniSQL Grammar

## 1. 文档范围

本文档描述当前 `minisql-backend` Parser 实际接受的 MiniSQL 语法。

文档只记录已经接入 Parser、语义分析和计划生成的语法，不把尚未支持的 CTE、CROSS JOIN 或窗口函数写成已实现能力。

## 2. 词法约定

```ebnf
identifier        ::= ( letter | "_" ) { letter | digit | "_" } ;
-- 标识符由字母或下划线开头，后续可以包含字母、数字和下划线。

integer_literal   ::= digit { digit } ;
-- 整数常量由十进制数字组成。

decimal_literal   ::= digit { digit } "." digit { digit } ;
-- 小数常量使用十进制小数点表示。

float_literal     ::= digit { digit } "." digit { digit } [ exponent ] ;
-- FLOAT 常量允许使用指数形式。

exponent          ::= ( "e" | "E" ) [ "+" | "-" ] digit { digit } ;
-- 指数部分由 e 或 E、可选符号和十进制数字组成。

string_literal    ::= "'" { character | "''" } "'" ;
-- 字符串使用单引号包围，连续两个单引号表示字符串中的一个单引号。

line_comment      ::= "--" { character } newline ;
-- 双短横线开始的行注释持续到换行。

block_comment     ::= "/*" { character } "*/" ;
-- 块注释从斜杠星号开始，到第一个星号斜杠结束，不支持嵌套。
```

空白和注释不会产生 Token，但会更新 Token 的行号和列号。

## 3. 关键字

以下单词作为关键字识别，不区分大小写。

```text
SELECT DISTINCT FROM AS WHERE GROUP BY HAVING ORDER ASC DESC NULLS FIRST LAST
LIMIT OFFSET CREATE TABLE INDEX UNIQUE INSERT INTO VALUES DEFAULT DELETE UPDATE SET
JOIN INNER LEFT RIGHT FULL OUTER ON AND OR NOT IN IS NULL TRUE FALSE EXISTS CAST
BEGIN TRANSACTION COMMIT ROLLBACK SAVEPOINT RELEASE CHECKPOINT DROP
INT BIGINT FLOAT VARCHAR DECIMAL BOOL DATE
PRIMARY KEY FOREIGN REFERENCES CHECK CONSTRAINT
```

`COUNT`、`SUM`、`AVG`、`MIN` 和 `MAX` 在语法分析中作为聚合函数名处理，其他未知函数名不会被当作通用函数调用接受。

## 4. 类型

```ebnf
type_name         ::= "INT" | "BIGINT" | "FLOAT" | "BOOL" | "DATE" | varchar_type | decimal_type ;
-- 基础类型包括整数、扩展整数、浮点、布尔和日期。

varchar_type      ::= "VARCHAR" [ "(" unsigned_integer ")" ] ;
-- VARCHAR 可以省略长度，也可以使用括号指定长度。

decimal_type      ::= "DECIMAL" "(" unsigned_integer "," unsigned_integer ")" ;
-- DECIMAL 必须给出精度和小数位。

unsigned_integer  ::= digit { digit } ;
-- 类型参数是非负整数。
```

## 5. 顶层语句

```ebnf
program           ::= { statement } ;
-- 一个输入可以包含零条或多条语句。

statement         ::= transaction_stmt
                    | create_table_stmt
                    | create_index_stmt
                    | drop_index_stmt
                    | insert_stmt
                    | select_stmt
                    | update_stmt
                    | delete_stmt
                    | checkpoint_stmt ;
-- 顶层语句由事务、DDL、DML、查询和检查点语句组成。
```

所有普通 SQL 语句都以分号结束。

## 6. 事务语句

```ebnf
transaction_stmt  ::= "BEGIN" [ "TRANSACTION" ] ";"
                    | "COMMIT" [ "TRANSACTION" ] ";"
                    | "ROLLBACK" [ "TRANSACTION" ] ";"
                    | "SAVEPOINT" identifier ";"
                    | "RELEASE" [ "SAVEPOINT" ] identifier ";"
                    | "ROLLBACK" "TO" [ "SAVEPOINT" ] identifier ";" ;
-- 事务语句控制提交、回滚和保存点。

checkpoint_stmt   ::= "CHECKPOINT" ";" ;
-- CHECKPOINT 要求当前事务内容落盘。
```

## 7. 建表和约束

```ebnf
create_table_stmt ::= "CREATE" "TABLE" identifier "(" table_element { "," table_element } ")" ";" ;
-- CREATE TABLE 定义一个表及一个或多个列或表级约束。

table_element     ::= column_definition | table_constraint ;
-- 表元素可以是列定义或表级约束。

column_definition ::= identifier type_name { column_constraint } ;
-- 列定义由列名、类型和可选列约束组成。

column_constraint ::= [ "CONSTRAINT" identifier ]
                      ( "NOT" "NULL"
                      | "NULL"
                      | "DEFAULT" literal
                      | "PRIMARY" "KEY"
                      | "UNIQUE"
                      | "REFERENCES" identifier "(" identifier ")"
                      | "CHECK" "(" expression ")" ) ;
-- 列约束包括空值、默认值、主键、唯一、外键和 CHECK。

table_constraint  ::= [ "CONSTRAINT" identifier ]
                      ( "PRIMARY" "KEY" "(" identifier_list ")"
                      | "UNIQUE" "(" identifier_list ")"
                      | "FOREIGN" "KEY" "(" identifier_list ")"
                        "REFERENCES" identifier "(" identifier_list ")"
                      | "CHECK" "(" expression ")" ) ;
-- 表约束支持复合主键、复合唯一键、复合外键和 CHECK。

identifier_list   ::= identifier { "," identifier } ;
-- 标识符列表用逗号分隔。
```

虽然列约束语法允许一般的 `CONSTRAINT name` 前缀，但当前实现不接受 `CONSTRAINT name DEFAULT` 和 `CONSTRAINT name NULL`。

## 8. 索引语句

```ebnf
create_index_stmt ::= "CREATE" [ "UNIQUE" ] "INDEX" identifier
                      "ON" identifier "(" identifier_list ")" ";" ;
-- 创建普通索引或唯一索引。

drop_index_stmt   ::= "DROP" "INDEX" identifier [ "ON" identifier ] ";" ;
-- 删除已存在的索引。
```

## 9. INSERT

```ebnf
insert_stmt       ::= "INSERT" "INTO" identifier
                      [ "(" identifier_list ")" ]
                      ( "DEFAULT" "VALUES"
                      | "VALUES" value_row { "," value_row } ) ";" ;
-- INSERT 可以显式列出目标列，也可以对全部列使用默认值。

value_row         ::= "(" write_value { "," write_value } ")" ;
-- 一个 VALUES 行由括号包围的一个或多个写值组成。

write_value       ::= "DEFAULT" | expression ;
-- 写值可以是 DEFAULT，也可以是普通表达式。
```

当前 Parser 支持多行 `VALUES`，INSERT 的列数、列序和类型由语义分析继续检查。

## 10. UPDATE

```ebnf
update_stmt       ::= "UPDATE" identifier "SET" assignment { "," assignment }
                      [ "WHERE" expression ] ";" ;
-- UPDATE 修改目标表中满足条件的记录。

assignment        ::= identifier "=" write_value ;
-- 赋值语句把目标列设置为 DEFAULT 或表达式。
```

## 11. DELETE

```ebnf
delete_stmt       ::= "DELETE" "FROM" identifier [ "WHERE" expression ] ";" ;
-- DELETE 删除目标表中满足条件的记录，省略 WHERE 时表示整表删除。
```

## 12. SELECT

```ebnf
select_core       ::= "SELECT" [ "DISTINCT" ] select_item { "," select_item }
                      "FROM" table_reference
                      { join_clause }
                      [ "WHERE" expression ]
                      [ "GROUP" "BY" expression { "," expression } ]
                      [ "HAVING" expression ]
                      [ "ORDER" "BY" order_item { "," order_item } ]
                      [ "LIMIT" unsigned_integer ]
                      [ "OFFSET" unsigned_integer ] ;
-- SELECT 核心支持去重、投影、数据源、JOIN、过滤、分组、排序和分页。

select_stmt       ::= select_core ";" ;
-- 顶层 SELECT 语句在核心语法后要求分号。

select_stmt_without_semicolon
                  ::= select_core ;
-- 子查询和派生表复用 SELECT 核心，但不消费外层语句的分号。

select_item       ::= expression [ "AS" identifier ]
                    | "*"
                    | identifier "." "*" ;
-- SELECT 项可以是表达式、普通星号或限定星号，表达式可以带别名。

table_reference   ::= identifier [ [ "AS" ] identifier ]
                    | "(" select_stmt_without_semicolon ")" [ "AS" ] identifier ;
-- 数据源可以是普通表，也可以是带显式别名的派生表。

join_clause       ::= [ "INNER" | "LEFT" [ "OUTER" ] | "RIGHT" [ "OUTER" ] | "FULL" [ "OUTER" ] ]
                      "JOIN" identifier [ [ "AS" ] identifier ]
                      "ON" expression ;
-- JOIN 必须提供 ON 条件，最多支持 32 个 JOIN。

order_item        ::= expression [ "ASC" | "DESC" ]
                      [ "NULLS" ( "FIRST" | "LAST" ) ] ;
-- 排序项可以指定升序、降序和 NULL 的位置。
```

派生表必须提供显式别名。派生表输出列存在重复可推导名称时，Parser 或语义分析会拒绝。

## 13. 表达式

```ebnf
expression        ::= or_expression ;
-- 表达式从最低优先级的 OR 表达式开始解析。

or_expression     ::= and_expression { "OR" and_expression } ;
-- OR 按从左到右结合。

and_expression    ::= negation { "AND" negation } ;
-- AND 的优先级高于 OR。

negation          ::= "NOT" negation | comparison ;
-- NOT 作用于后续完整比较表达式，因此 NOT a = 1 解析为 NOT (a = 1)。

comparison        ::= addition
                      [ ( "=" | "!=" | "<" | "<=" | ">" | ">=" ) addition
                      | [ "NOT" ] "IN" "(" in_operand ")"
                      | "IS" [ "NOT" ] "NULL" ] ;
-- 比较运算包括等值、范围和 IN，IS NULL 用于空值判断。

in_operand        ::= expression { "," expression } | select_stmt_without_semicolon ;
-- IN 的右侧可以是值列表或 SELECT 子查询。

addition          ::= multiplication { ( "+" | "-" ) multiplication } ;
-- 加减运算按从左到右结合。

multiplication    ::= unary { ( "*" | "/" ) unary } ;
-- 乘除运算的优先级高于加减。

unary             ::= [ "+" | "-" ] primary
                    | aggregate_expression
                    | "EXISTS" "(" select_stmt_without_semicolon ")"
                    | "CAST" "(" expression "AS" type_name ")" ;
-- 一元运算、聚合、EXISTS 和 CAST 位于表达式的高优先级层。

aggregate_expression
                  ::= ( "COUNT" | "SUM" | "AVG" | "MIN" | "MAX" )
                      "(" ( "*" | expression ) ")" ;
-- 只有 COUNT 接受星号，其他聚合函数必须使用表达式参数。

primary           ::= literal
                    | identifier [ "." identifier ]
                    | "(" expression ")"
                    | scalar_subquery ;
-- 基本表达式包括常量、标识符、括号表达式和标量子查询。

scalar_subquery   ::= "(" select_stmt_without_semicolon ")" ;
-- 标量子查询必须返回单个值，实际行数由语义和执行阶段检查。

literal           ::= integer_literal
                    | decimal_literal
                    | float_literal
                    | string_literal
                    | "NULL"
                    | "TRUE"
                    | "FALSE"
                    | "DATE" string_literal ;
-- 字面量包括数字、字符串、三值布尔和 DATE 字符串。
```

## 14. 优先级

从高到低为：

```text
括号、常量、标识符、聚合、CAST、EXISTS、标量子查询
一元正负号
乘法和除法
加法和减法
比较、IN、IS NULL
NOT
AND
OR
```

## 15. 当前边界

当前 Parser 不接受的语法包括：

- 通用函数调用，例如 `unknown(v)`。
- CTE，例如 `WITH ... AS (...)`。
- `CROSS JOIN`。
- 窗口函数。
- 带引号的标识符。
- 连续比较表达式，例如 `a = b = c`。
- 没有别名的派生表。

不支持的语法必须返回语法或语义错误，不得生成可执行计划。

## 16. 错误恢复

严格模式在第一个错误处停止。

恢复模式会记录错误，并同步到下一个真实语句边界。恢复模式不能在字符串内部或注释内部把分号当作语句结束符。

## 17. Parser 实现方案与文法对应

当前实现采用递归下降 Parser。

```text
all / allRecoverable  -> program
statement             -> statement
select                -> select_core
expression            -> expression
conjunction           -> and_expression
negation              -> negation
comparison            -> comparison
addition              -> addition
multiplication        -> multiplication
unary                 -> unary
primary               -> primary
```

左递归通过循环消除。例如 `or_expression` 和 `and_expression` 不在文法中直接递归调用自身，而是在循环中反复处理同层运算符。

`NOT` 通过 `negation ::= "NOT" negation | comparison` 处理，使 `NOT a = 1` 构造成 `NOT (a = 1)`。

`IN` 值列表在 Parser 中构造成等值比较的平衡 OR 树，避免长列表产生线性递归深度。

恢复模式使用语句边界同步。字符串、行注释和块注释都由 Lexer 处理，Parser 不会把其中的分号当作语句结束符。

## 18. 课件冲突项的最终处理

| 冲突项 | 当前处理 |
| --- | --- |
| NOT 优先级文字与表达式文法冲突 | 以表达式文法为准，`NOT` 作用于完整比较表达式 |
| 双等号 `==` | 不接受，等于比较只使用单等号 `=` |
| 小数常量没有基础数据类型 | 当前实现支持 DECIMAL 和 FLOAT |
| 基础配置只要求四类 SQL | 当前扩展支持 UPDATE、JOIN、GROUP BY、ORDER BY 和子查询 |
| 课件计划清单没有 Delete 节点 | 当前实现生成 Delete 计划节点 |
| 是否允许多行 VALUES | 当前实现支持多行 VALUES |
| 表名和列名是否大小写不敏感 | 当前实现按大小写不敏感匹配 |
| INSERT 是否允许省略列清单 | 当前实现允许省略列清单并按 Schema 顺序映射 |
