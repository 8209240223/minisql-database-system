#include "minisql/sql/parser.hpp"
#include <algorithm>
#include <cctype>
#include <charconv>

namespace minisql::sql {
namespace {
// 传来的 errors（而不是抛出 MiniSqlError），再抛出 Recovered{} 让语句级处理器
// 能够同步到下一条语句并继续解析。
// Internal control-flow marker used only in recovery mode. When the parser
// encounters a syntax error it records the diagnostic into the caller's
// errors vector (instead of throwing MiniSqlError) and throws Recovered{} so
// the statement-level handler can synchronize and continue.
struct Recovered {};
// 这个类型没有成员，只是一个"跳到语句边界"的信号。

// 第十七章 REQ-CORE-001：批量语句上限 10000。超限按资源预算诊断，
// 消息含 "budget exceeded" 以便 HTTP 适配层映射到 413。
constexpr std::size_t kMaxStatements = 10000;
const char* kStatementBudgetMessage = "Statement budget exceeded: batch input exceeds 10000 statements";

class Parser {
// 递归下降解析器：每个 SQL 语法成分对应一个成员函数，自顶向下往下推。
public:
    explicit Parser(const std::vector<Token>& tokens): t(tokens) {}
    // 构造时只保存 token 流引用，不做任何扫描，避免多余复制。
    // 严格解析：遇到第一个错误就抛 MiniSqlError（历史一直沿用的行为）。
    // Strict parse: throws MiniSqlError on the first error (classic behaviour).
    std::vector<Statement> all(){
    // 把整条 token 流解析成语句列表。
        std::vector<Statement> out;
        // 结果容器，按语句出现顺序存放。
        while(i<t.size() && t[i].type!="END"){
            if(out.size()>=kMaxStatements)
                throw MiniSqlError(ErrorCode::Execution, kStatementBudgetMessage,
                                   i<t.size()?t[i].location:SourceLocation{});
            out.push_back(statement());
        }
        return out;
        // 返回全部语句。
    }
    // 打开恢复模式：语法错误写进 errors（永不抛出），并在下一条语句边界恢复解析。
    // Enables recovery mode: syntax errors are pushed into `errors` (never
    // thrown) and scanning resumes at the next statement boundary.
    void setRecoverable(std::vector<MiniSqlError>& errors){ recover_ = true; errors_ = &errors; }
    // 打开开关并记住错误收集容器；后续 fail() 会据此改走"记录"分支。
    // 容错解析：每个可恢复错误记录到 errors_ 并在下一条语句边界恢复解析，
    // 出错的那条语句被丢弃；合法语句照常产出。
    // Recovery parse: each recoverable error is recorded into errors_ and
    // scanning resumes at the next statement boundary; the offending statement
    // is dropped. Valid statements are still produced.
    std::vector<Statement> allRecoverable(){
    // 容错模式的解析主循环。
        std::vector<Statement> out;
        // 结果容器。
        while(i<t.size() && t[i].type!="END"){
        // 同样循环到结束标记为止。
            if(out.size()>=kMaxStatements){
                errors_->emplace_back(ErrorCode::Execution, std::string(kStatementBudgetMessage),
                                      i<t.size()?t[i].location:SourceLocation{});
                break;
            }
            try {
            // 用异常机制做"整条语句回退"：出错就抛出 Recovered 跳到这里。
                auto st = statement();
                // 尝试解析一条完整语句。
                if (inError_) st.invalid = true;
                // 解析过程中发生过错误（阶段内同步过），给这条语句打上 invalid 标记。
                out.push_back(st);
                // 无论好坏都先收进结果，调用方按 invalid 决定是否执行。
            } catch (const Recovered&) {
            // 捕获"需要跳到语句边界"的信号。
                // 同步到下一条语句的边界。
                // Synchronize to the next statement boundary.
                inError_ = false;
                // 清掉错误标记，准备处理下一条语句。
                while(i<t.size() && t[i].type!="END" && t[i].lexeme!=";") ++i;
                // 一路跳过 token 直到分号或输入结束，把出错语句的残骸丢干净。
                if(i<t.size() && t[i].lexeme==";") ++i;
                // 若停在分号上，连分号一起跳过，正好落在下一条语句开头。
            }
        }
        return out;
        // 返回所有成功解析（或已标记无效）的语句。
    }
private:
// 以下是解析器的内部实现细节，不对外暴露。
    const std::vector<Token>& t;
    // 待解析的 token 流，用引用保存以免复制整张表。
    std::size_t i=0;
    // 当前读取位置在 token 流中的下标，解析过程中持续推进。
    bool recover_ = false;
    // 是否处于恢复模式（由 setRecoverable 打开）。
    std::vector<MiniSqlError>* errors_ = nullptr;
    // 恢复模式下收集诊断的容器；严格模式下为空。
    bool inError_ = false;
    // 本条语句解析过程中是否已经记录过错误。

    bool at(const std::string& s){return i<t.size() && t[i].lexeme==s;}
    // 判断当前 token 的词素是否恰好等于给定字符串（用于匹配运算符、逗号、分号等符号）。
    bool keyword(const std::string& s){if(i>=t.size())return false; auto v=t[i].lexeme; std::transform(v.begin(),v.end(),v.begin(),[](unsigned char c){return static_cast<char>(std::toupper(c));}); return v==s;}
    // 判断当前 token 是不是给定的关键字：先越界保护，再把词素整体转大写，
    // 最后与传入的大写关键字比较，从而实现大小写不敏感的关键字匹配。
    std::string actualToken() const { return i<t.size() && t[i].type!="END" ? t[i].lexeme : std::string{}; }
    // 取当前 token 的文本用于报错；已经读到结尾时返回空串，避免越界。
    // 统一的错误出口。严格模式下与经典解析器一样抛出 MiniSqlError；
    // 恢复模式下记录诊断并抛出 Recovered{}，一路展开到最近的同步点。

    // Central error outlet. In strict mode it throws MiniSqlError exactly as
    // the classic parser did. In recovery mode it records the diagnostic and
    // throws Recovered{} to unwind to the nearest sync point.
    [[noreturn]] void fail(ErrorCode code, const std::string& message, const SourceLocation& loc, const SourceLocation& end = {},
                           std::string actual = {}, std::vector<std::string> expected = {}){
        // [[noreturn]] 告诉编译器这个函数一旦调用就不会返回，消除"缺少返回值"的告警。
        // 参数分别是要抛的错误码、错误信息、起点位置、可选的终点位置、
        // 可选的"实际看到的内容"和"本应出现的内容列表"，后两者用于生成更友好的语法诊断。
        if (recover_ && errors_) {
        // 恢复模式且已有错误容器。
            const SourceLocation span = (end.line || end.column)
                ? SourceLocation{loc.line, loc.column, end.line, end.column}
                : loc;
            // 如果给了有效终点，就把错误表示成一个区间；否则退化成单点位置。
            errors_->emplace_back(code, message, span, std::string{}, std::move(actual), std::move(expected));
            // 在容器尾部构造诊断对象，把消息、区间、实际值与期望值一并带上。
            inError_ = true;
            // 标记本条语句已出错，稍后会被打上 invalid。
            throw Recovered{};
            // 抛出信号，让调用链展开到语句级 try/catch 去做同步。
        }
        throw MiniSqlError(code, message, loc, std::string{}, std::move(actual), std::move(expected));
        // 严格模式：直接抛出真正的异常，行为与历史版本完全一致。
    }

    const Token& take(){
    // 取出当前 token 并把游标前移一格，是所有读取动作的统一入口。
        if(i>=t.size()) { fail(ErrorCode::Syntax, "Unexpected end of input", SourceLocation{}); }
        // 已经越过末尾说明语句被截断，按语法错误上报。
        return t[i++];
        // 返回当前 token 的引用，同时把下标加一。
    }
    void expect(const std::string& s){
    // 期待当前位置出现某个符号或关键字，出现就消费掉，否则报错。
        if(!keyword(s)&&!at(s)){
        // 既不是关键字形式也不是符号形式，说明对不上。
            fail(ErrorCode::Syntax, "Expected '"+s+"'", i<t.size()?t[i].location:SourceLocation{}, {}, actualToken(), {s});
            // 报"期望某记号"的语法错误，并把实际内容与期望内容一起交出去。
        }
        ++i;
        // 匹配成功后消费掉这个 token。
    }
    std::string identifier(){
    // 读一个标识符（表名、列名、别名、约束名都走这里）。
        const auto& x=take();
        // 先取出当前 token。
        if(x.type!="IDENTIFIER") fail(ErrorCode::Syntax, "Expected identifier", x.location, x.endLocation, x.lexeme, {"IDENTIFIER"});
        // 种别码不是 IDENTIFIER 就报错，期望列表里只有 IDENTIFIER。
        return x.lexeme;
        // 返回标识符原文（保留用户书写的大小写）。
    }
    std::string literal(){
    // 读一个字面量，返回它的文本形式（带符号、带 DATE 前缀）。
        if(keyword("DATE")&&i+1<t.size()&&t[i+1].type=="STRING"){++i;return "DATE"+take().lexeme;}
        // 形态一：DATE 关键字紧跟字符串，先跳过 DATE 再取字符串，
        // 拼成 "DATE'2024-01-01'" 这样的规范文本，便于后续统一解析日期。
        if(keyword("NULL")||keyword("TRUE")||keyword("FALSE"))return take().lexeme;
        // 形态二：NULL/TRUE/FALSE 本身就是完整字面量，直接取原文返回。
        std::string sign;if(at("-")||at("+"))sign=take().lexeme;
        // 形态三：可能带正负号，先把符号读下来存着。
        const auto& x=take();
        // 再读实际的值 token。
        if(x.type!="INTEGER"&&x.type!="DECIMAL"&&x.type!="FLOAT"&&(x.type!="STRING"||!sign.empty()))
            fail(ErrorCode::Syntax, "Expected literal", x.location, x.endLocation, x.lexeme,
                 {"INTEGER", "DECIMAL", "FLOAT", "STRING", "NULL", "TRUE", "FALSE", "DATE"});
        // 合法组合只有两种：数字字面量（可带符号），或者不带符号的字符串。
        // 注意 "-'abc'" 这类"符号加字符串"被显式排除，因为它没有意义。
        return sign+x.lexeme;
        // 把符号和值拼回一个完整字面量文本返回。
    }
    std::string typeName() {
    // 读一个列类型名，并把它规范化成后续统一使用的写法。
        if(keyword("INT")||keyword("BIGINT")||keyword("FLOAT")||keyword("BOOL")||keyword("DATE"))return take().lexeme;
        // 无参数的简单类型直接取原文返回（INT/BIGINT/FLOAT/BOOL/DATE）。
        const bool varchar=keyword("VARCHAR");
        // 判断是不是 VARCHAR，因为它需要括号里的长度参数。
        if(!varchar && !keyword("DECIMAL")) fail(ErrorCode::Syntax, "Expected data type", t[i].location, {}, actualToken(),
            {"INT", "BIGINT", "FLOAT", "VARCHAR", "BOOL", "DATE", "DECIMAL"});
        // 既不是 VARCHAR 也不是 DECIMAL 就报"期望数据类型"，并把支持的类型全部列出来。
        ++i;if(varchar && !at("("))return "varchar";expect("(");
        // 消费掉类型关键字；VARCHAR 后面没写括号时按无长度写法处理，直接返回 varchar；
        // 否则到这里一定是 VARCHAR(...) 或 DECIMAL(...)，紧接着吃掉左括号。
        const auto parameter=[&](){
        // 局部工具：读取一个无符号整数参数（VARCHAR 的长度、DECIMAL 的精度或标度）。
            const auto token=take();unsigned value{};
            // 取出 token，value 用来接收解析结果。
            if(token.type!="INTEGER") fail(ErrorCode::Syntax, "Expected unsigned type parameter", token.location);
            // 参数必须是整数 token，其它形式（负数、小数、标识符）都非法。
            const auto parsed=std::from_chars(token.lexeme.data(),token.lexeme.data()+token.lexeme.size(),value);
            // 用 from_chars 做一次不抛异常的文本转整数。
            if(parsed.ec!=std::errc{}||parsed.ptr!=token.lexeme.data()+token.lexeme.size()) fail(ErrorCode::Syntax, "Type parameter is too large", token.location);
            // 出现溢出，或者没有把整个文本都消费掉，都说明这个参数超出可表示范围。
            return value;
            // 返回解析出来的无符号数值。
        };
        const auto precision=parameter();
        // 第一个参数：VARCHAR 是长度，DECIMAL 是精度。
        if(varchar){expect(")");return "varchar("+std::to_string(precision)+")";}
        // VARCHAR 形态：吃掉右括号，把类型规范成 varchar(N) 返回。
        expect(",");const auto scale=parameter();expect(")");
        // DECIMAL 形态：还要再读一个逗号和第二个参数（标度），最后吃右括号。
        return "decimal("+std::to_string(precision)+","+std::to_string(scale)+")";
        // 统一规范成 decimal(P,S) 的形式返回，后续类型判断都按这个文本比对。
    }
    void semicolon(){if(at(";"))++i;else fail(ErrorCode::Syntax, "Expected ';'", i<t.size()?t[i].location:SourceLocation{}, {}, actualToken(), {";"});}
    // 语句结束符处理：有分号就吃掉；没有也报错，保持"每条语句必须以分号结尾"的约定。
    Statement statement(){auto loc=t[i].location;Statement s;if(keyword("BEGIN")||keyword("COMMIT")||keyword("ROLLBACK")||keyword("SAVEPOINT")||keyword("RELEASE"))s=transaction();else if(keyword("CREATE"))s=create();else if(keyword("INSERT"))s=insert();else if(keyword("SELECT"))s=select();else if(keyword("DELETE"))s=remove();else if(keyword("UPDATE"))s=update();else if(keyword("CHECKPOINT"))s=checkpointStatement();else if(keyword("DROP"))s=dropIndex();else if(keyword("WITH"))s=withQuery();else fail(ErrorCode::Syntax, "Expected SQL statement or transaction command", loc, {}, actualToken(), {"BEGIN", "COMMIT", "ROLLBACK", "SAVEPOINT", "RELEASE", "CREATE", "INSERT", "SELECT", "DELETE", "UPDATE", "CHECKPOINT", "DROP", "WITH"});s.location=loc;return s;}
    // X25: 非递归 WITH。CTE 名是作用域名，绑定器据此把引用解析到内部查询，
    // 因此权限层不会把 CTE 别名误当成数据库对象，也不需要任何文本扫描兜底。
    Statement withQuery(){
        expect("WITH");
        if(keyword("RECURSIVE")) fail(ErrorCode::NotImplemented, "Recursive common table expressions are not supported",
            t[i].location, {}, actualToken(), {"IDENTIFIER"});
        std::vector<CommonTableExpr> ctes;
        do {
            if(ctes.size()>=32) fail(ErrorCode::Syntax, "Common table expression count exceeds 32", t[i].location);
            CommonTableExpr cte;
            cte.location=i<t.size()?t[i].location:SourceLocation{};
            cte.name=identifier();
            for(const auto& existing: ctes)
                if(equalNames(existing.name,cte.name))
                    fail(ErrorCode::Semantic, "Duplicate common table expression name", cte.location, {}, cte.name, {});
            if(at("(")){
                ++i;
                do { cte.columns.push_back(identifier()); } while(at(",")&&(++i,true));
                expect(")");
            }
            expect("AS");expect("(");
            auto query=select(false);
            expect(")");
            if(!cte.columns.empty()&&!query.selectItems.empty()&&query.selectList.size()==query.selectItems.size()&&
               cte.columns.size()!=query.selectItems.size())
                fail(ErrorCode::Semantic, "Common table expression column count does not match its query", cte.location);
            cte.query=std::make_shared<Statement>(std::move(query));
            ctes.push_back(std::move(cte));
        } while(at(",")&&(++i,true));
        if(!keyword("SELECT")) fail(ErrorCode::Syntax, "Expected SELECT after WITH",
            i<t.size()?t[i].location:SourceLocation{}, {}, actualToken(), {"SELECT"});
        auto s=select();
        s.ctes=std::move(ctes);
        return s;
    }
    static bool equalNames(std::string left,std::string right){
        const auto fold=[](std::string& value){std::transform(value.begin(),value.end(),value.begin(),
            [](unsigned char c){return static_cast<char>(std::tolower(c));});};
        fold(left);fold(right);return left==right;
    }
    Statement dropIndex(){Statement s{"DropIndex"};expect("DROP");expect("INDEX");s.indexName=identifier();if(keyword("ON")){++i;s.table=identifier();}semicolon();return s;}
    // 解析 DROP INDEX 名称 [ON 表名]；ON 子句可选，给定了就限制只在指定表里找索引。
    Statement checkpointStatement(){Statement s{"Checkpoint"};expect("CHECKPOINT");semicolon();return s;}
    // 解析 CHECKPOINT; 这条手动触发检查点的语句。
    Statement transaction() {
    // 解析事务控制语句，返回带对应 kind 的语句节点。
        if (keyword("SAVEPOINT")) {
        // 形态一：SAVEPOINT 名字;
            Statement s{"Savepoint"};++i;s.savepointName=identifier();semicolon();return s;
            // 消费 SAVEPOINT，读保存点名，再吃掉分号返回。
        }
        if (keyword("RELEASE")) {
        // 形态二：RELEASE [SAVEPOINT] 名字;
            Statement s{"ReleaseSavepoint"};++i;
            // 消费 RELEASE，并准备返回释放保存点的语句。
            if(keyword("SAVEPOINT"))++i;
            // 中间的 SAVEPOINT 关键字是可选的，写了就跳过。
            s.savepointName=identifier();semicolon();return s;
            // 读保存点名并吃掉分号。
        }
        if (keyword("ROLLBACK")) {
        // 形态三：ROLLBACK 或 ROLLBACK TO [SAVEPOINT] 名字;
            ++i;
            // 先消费 ROLLBACK。
            if (keyword("TO")) { ++i;if(keyword("SAVEPOINT"))++i;Statement s{"RollbackTo"};s.savepointName=identifier();semicolon();return s; }
            // 带 TO 的是"回滚到某个保存点"：跳过可选的 SAVEPOINT，读名字后返回 RollbackTo。
            if(keyword("TRANSACTION"))++i;semicolon();return Statement{"Rollback"};
            // 不带 TO 的是整体回滚：跳过可选的 TRANSACTION 关键字后返回 Rollback。
        }
        Statement s{keyword("BEGIN") ? "Begin" : "Commit"};
        // 剩下的只有 BEGIN 和 COMMIT 两种，按当前关键字决定 kind。
        ++i;if(keyword("TRANSACTION"))++i;
        // 消费该关键字，并跳过可选的 TRANSACTION 修饰词。
        semicolon();return s;
        // 吃掉分号后返回语句。
    }
    Statement update() {
    // 解析 UPDATE 表名 SET 列=值[, 列=值...] [WHERE 条件];
        Statement s{"Update"};expect("UPDATE");
        if(at("(")) derivedTarget(s); else s.table=identifier();
        expect("SET");
        do {auto name=identifier();expect("=");s.assignments.push_back({std::move(name),writeValue()});}
        while(at(",")&&(++i,true));
        // 循环读取赋值项：读列名、吃等号、再读赋值表达式（writeValue 支持 DEFAULT）；
        // 只要后面跟着逗号就消费掉逗号继续下一项。
        if(keyword("WHERE")){++i;s.where=expression();}
        // WHERE 是可选的，写了就消费关键字并解析条件表达式。
        semicolon();return s;
        // 吃掉分号返回。
    }
    Statement create(){
    // 解析 CREATE TABLE 与 CREATE [UNIQUE] INDEX 两种建对象语句。
        expect("CREATE");
        // 先消费 CREATE。
        bool unique = false;
        // 记录是否写了 UNIQUE 前缀。
        if (keyword("UNIQUE")) { ++i; unique = true; }
        // 有 UNIQUE 就消费掉并置位，稍后用于唯一索引。
        if (keyword("INDEX")) {
        // 分支一：CREATE [UNIQUE] INDEX 索引名 ON 表名(列,...);
            ++i;
            // 消费 INDEX。
            Statement index{"CreateIndex"};
            // 构造建索引语句节点。
            index.uniqueIndex = unique;
            // 带上是否唯一的信息。
            index.indexName = identifier();
            // 读索引名。
            expect("ON");index.table = identifier();expect("(");
            // 消费 ON 与目标表名，并吃掉左括号。
            do { index.indexColumns.push_back(identifier()); } while (at(",") && (++i, true));
            // 逐个读索引列，遇到逗号就继续。
            expect(")");semicolon();
            // 吃右括号和分号。
            return index;
            // 返回建索引语句。
        }
        if (unique) fail(ErrorCode::Syntax, "Expected INDEX after UNIQUE", t[i].location);
        // 写了 UNIQUE 但后面不是 INDEX，说明这个前缀用错了位置。
        expect("TABLE");
        // 剩下的只可能是 CREATE TABLE。
        Statement s{"CreateTable"};s.table=identifier();expect("(");
        // 读表名并吃掉列定义列表的左括号。
        std::vector<std::string> explicitNull;
        // 记录"显式写了 NULL"的列名，稍后用于检测主键与 NULL 的冲突。
        const auto constraintName = [&]() -> std::string { if(!keyword("CONSTRAINT"))return {}; ++i;return identifier(); };
        // 局部工具：如果当前位置是 CONSTRAINT，就读出后面的约束名；否则返回空串。
        const auto recordName = [&](const std::string& label,const std::string& kind,std::size_t index) { if(!label.empty())s.constraintNames.push_back({label,kind,index}); };
        // 局部工具：把"约束名 + 类别 + 第几个"登记到语句的命名表里；
        // 没有名字（label 为空）时什么也不做。
        do{
        // 循环解析括号里的每一个成员：可能是表级约束，也可能是一列定义。
            const auto label=constraintName();
            // 每个成员前面都可能带一个可选 CONSTRAINT 名字。
            if(keyword("FOREIGN")){
            // 表级外键：FOREIGN KEY(本表列) REFERENCES 父表(父列)。
                recordName(label,"foreignKey",s.foreignKeys.size());
                // 先登记约束名，序号是"已解析外键的个数"，正好是这条在列表里的位置。
                ++i;expect("KEY");ForeignKey constraint;
                // 消费 FOREIGN、KEY，并准备外键结构。
                expect("(");do{constraint.columns.push_back(identifier());}while(at(",")&&(++i,true));expect(")");
                // 读本表侧的列清单。
                expect("REFERENCES");constraint.table=identifier();
                // 消费 REFERENCES 并读父表名。
                expect("(");do{constraint.referencedColumns.push_back(identifier());}while(at(",")&&(++i,true));expect(")");
                // 读父表侧被引用的列清单。
                s.foreignKeys.push_back(std::move(constraint));continue;
                // 收进语句并跳过后面的列定义分支。
            }
            if(keyword("CHECK")){
            // 表级 CHECK 约束。
                recordName(label,"check",s.checks.size());
                // 登记约束名，序号等于已有 CHECK 的个数。
                ++i;expect("(");s.checks.push_back(expression());expect(")");continue;
                // 消费 CHECK 与括号，把括号里的条件表达式解析后收进列表。
            }
            if(keyword("PRIMARY")||keyword("UNIQUE")){
            // 表级主键或唯一键。
                recordName(label,"key",s.keys.size());
                // 登记约束名，序号等于已有键约束的个数。
                KeyConstraint constraint;
                // 准备键约束结构。
                constraint.primary=keyword("PRIMARY");++i;if(constraint.primary)expect("KEY");
                // 判断是主键还是唯一键，消费关键字（主键还要再吃掉 KEY）。
                expect("(");do{constraint.columns.push_back(identifier());}while(at(",")&&(++i,true));expect(")");
                // 读构成这个键的列清单，顺序被保留。
                s.keys.push_back(std::move(constraint));continue;
                // 收进语句并继续下一个成员。
            }
            if(!label.empty()) fail(ErrorCode::Syntax, "Expected table constraint", t[i].location);
            // 写了 CONSTRAINT 名字，后面却不是任何一种表级约束，属于语法错误。
            auto name=identifier();
            // 到这里说明这是普通列定义，先读列名。
            ColumnDef column{name,typeName()};bool seenNull=false;
            // 读列类型构造列定义；seenNull 记录本列是否写过可空性声明。
            while(keyword("NOT")||keyword("NULL")||keyword("DEFAULT")||keyword("PRIMARY")||keyword("UNIQUE")||keyword("REFERENCES")||keyword("CHECK")||keyword("CONSTRAINT")){
            // 列定义后面可以跟一串列级约束，只要下一个关键字仍是约束开头就继续循环。
                const auto columnLabel=constraintName();
                // 每条列级约束前也可能带 CONSTRAINT 名字。
                if(keyword("CHECK")){
                // 列级 CHECK。
                    recordName(columnLabel,"check",s.checks.size());
                    // 列级 CHECK 与表级 CHECK 共用同一个列表，用同一个序号体系。
                    ++i;expect("(");s.checks.push_back(expression());expect(")");
                    // 解析括号里的条件并收进列表。
                }else if(keyword("PRIMARY")){
                // 列级主键。
                    recordName(columnLabel,"primaryKey",s.columns.size());
                    // 序号取"当前列的个数"，因为这一列还没入列表，正好是它未来的下标。
                    if(column.primaryKey) fail(ErrorCode::Syntax, "Duplicate PRIMARY KEY", t[i].location);
                    // 同一列上重复声明主键属于错误。
                    ++i;expect("KEY");column.primaryKey=true;
                    // 消费 PRIMARY KEY 并置位。
                }else if(keyword("UNIQUE")){
                // 列级唯一约束。
                    recordName(columnLabel,"unique",s.columns.size());
                    // 记录命名与目标列下标。
                    if(column.unique) fail(ErrorCode::Syntax, "Duplicate UNIQUE", t[i].location);
                    // 重复声明唯一约束属于错误。
                    ++i;column.unique=true;
                    // 消费关键字并置位。
                }else if(keyword("REFERENCES")){
                // 列级外键。
                    recordName(columnLabel,"references",s.columns.size());
                    // 记录命名与目标列下标。
                    ++i;auto referencedTable=identifier();expect("(");auto referencedColumn=identifier();expect(")");
                    // 消费 REFERENCES，读父表名，再读括号里的父列名。
                    if(column.references) fail(ErrorCode::Syntax, "Duplicate REFERENCES", t[i].location);
                    // 同一列重复声明外键属于错误。
                    column.references=std::make_pair(std::move(referencedTable),std::move(referencedColumn));
                    // 把父表名与父列名存成一对外键信息。
                }else if(keyword("DEFAULT")){
                // 列默认值。
                    if(!columnLabel.empty()) fail(ErrorCode::Syntax, "Named DEFAULT is not supported", t[i].location);
                    // DEFAULT 不是约束，不允许起名字。
                    if(column.defaultValue) fail(ErrorCode::Syntax, "Duplicate DEFAULT", t[i].location);
                    // 重复写 DEFAULT 属于错误。
                    ++i;column.defaultValue=literal();
                    // 消费关键字并读一个字面量作为默认值。
                }else if(keyword("NOT")||keyword("NULL")){
                // 可空性声明：NOT NULL 或 NULL。
                    if(!columnLabel.empty()&&!keyword("NOT")) fail(ErrorCode::Syntax, "Named NULL is not a constraint", t[i].location);
                    // 只有 NOT NULL 才算约束可以命名，单独一个 NULL 不能起名字。
                    recordName(columnLabel,"notNull",s.columns.size());
                    // 给非空约束登记名字。
                    if(seenNull) fail(ErrorCode::Syntax, "Duplicate nullability declaration", t[i].location);
                    // 同一列写两次可空性属于错误。
                    seenNull=true;
                    // 标记已经出现过可空性声明。
                    if(keyword("NOT")){++i;expect("NULL");column.nullable=false;}else ++i;
                    // NOT NULL 走两步消费并置为不可空；单独 NULL 只消费一个 token。
                }else fail(ErrorCode::Syntax, "Expected column constraint", t[i].location);
                // 理论上走不到这里，保留兜底分支。
            }
            if(seenNull&&column.nullable)explicitNull.push_back(column.name);
            // 显式写了 NULL 的列名记下来，后面要检查它是不是主键的一部分。
            if(column.primaryKey){
            // 本列被声明为主键。
                if(seenNull&&column.nullable) fail(ErrorCode::Syntax, "PRIMARY KEY cannot declare NULL", t[i].location);
                // 主键不允许显式声明可空。
                column.nullable=false;
                // 主键一律强制非空。
            }
            s.columns.push_back(std::move(column));
            // 把这列加入语句的列列表。
        }while(at(",")&&(++i,true));expect(")");semicolon();
        // 有逗号就继续解析下一个成员；最后吃掉右括号与分号。
        auto normalized=[](std::string name){for(auto& c:name)c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));return name;};
        // 局部工具：把名字整体转大写，做大小写不敏感的列名比较。
        for(const auto& constraint:s.keys)if(constraint.primary)for(const auto& name:constraint.columns){
        // 遍历所有主键约束，逐个检查它引用的列。
            for(const auto& nullable:explicitNull)if(normalized(nullable)==normalized(name)) fail(ErrorCode::Syntax, "PRIMARY KEY cannot declare NULL", SourceLocation{});
            // 如果某个主键列被显式写成了 NULL，报"主键不能声明可空"。
            for(auto& column:s.columns)if(normalized(column.name)==normalized(name))column.nullable=false;
            // 把主键涉及的列统统改成非空，保证元数据一致。
        }
        return s;
        // 返回解析好的建表语句。
    }
    Statement insert(){
    // 解析 INSERT INTO 表名 [(列,...)] VALUES (...)[,(...)...]; 以及 DEFAULT VALUES 写法。
        Statement s{"Insert"};expect("INSERT");expect("INTO");s.table=identifier();
        // 依次消费 INSERT、INTO 并读目标表名。
        if(keyword("DEFAULT")){++i;expect("VALUES");s.defaultValues=true;semicolon();return s;}
        // 分支一：INSERT INTO t DEFAULT VALUES; 只需置位并返回，无需列清单。
        if(at("(")){++i;do{s.names.push_back(identifier());}while(at(",")&&(++i,true));expect(")");}
        // 分支二：写了列清单，逐个读列名，遇到逗号继续。
        expect("VALUES");expect("(");
        // 消费 VALUES 与第一个左括号。
        do{
        // 读第一行的每一个值。
            s.valueExpressions.push_back(writeValue());
            // writeValue 既能读普通表达式，也能识别 DEFAULT 关键字。
        }while(at(",")&&(++i,true));
        expect(")");
        // 有逗号说明这一行还有值，继续读；最后吃掉右括号。
        if(at(",")) {
        // 后面还跟着逗号，说明这是多行插入。
            s.valueRows.push_back(s.valueExpressions);
            // 先把已经读好的第一行原样存进多行列表。
            while(at(",")) {
            // 逐行处理剩下的每一行。
                ++i;expect("(");
                // 消费行间逗号和每行的左括号。
                std::vector<std::shared_ptr<Expr>> row;
                // 准备装这一行的表达式。
                do { row.push_back(writeValue()); } while(at(",")&&(++i,true));
                // 逐个读值。
                expect(")");s.valueRows.push_back(std::move(row));
                // 吃掉右括号，把这一行收进多行列表。
            }
        }
        semicolon();
        // 吃掉语句结尾分号。
        if(std::all_of(s.valueExpressions.begin(),s.valueExpressions.end(),[](const auto& e){return e->kind=="Literal";}))
            for(const auto& e:s.valueExpressions)s.values.push_back(e->value);
        // 兼容路径：如果这一行的值全是纯字面量，就额外把它们的文本抄一份到 values，
        // 让只认字面量文本的老代码也能工作；只要有一个不是字面量就不抄。
        return s;
        // 返回插入语句。
    }
    std::shared_ptr<Expr> writeValue(){
    // 读一个"可写值"：普通表达式，或者 DEFAULT 关键字。
        if(keyword("DEFAULT")){const auto token=take();return std::make_shared<Expr>(Expr{"Default","DEFAULT",{},{},token.location});}
        // 写成 DEFAULT 时专门造一个 Default 节点，而不是当普通标识符处理。
        return expression();
        // 其它情况一律交给通用表达式解析。
    }
    std::string tokenText(std::size_t begin, std::size_t end) {
    // 把 [begin, end) 区间的 token 重新拼成一段文本。
        std::string text;
        // 结果缓冲。
        for (std::size_t index = begin; index < end; ++index) {
        // 逐 token 拼接。
            if (!text.empty()) text += ' ';
            // 除第一个之外，每个 token 前面补一个空格当分隔，保持可读性。
            text += t[index].lexeme;
            // 追加该 token 的词素。
        }
        return text;
        // 返回拼接后的文本，用来给子查询保存一份原始 SQL 记录。
    }
    bool syncClause(){
    // 子句级同步：出错后尽量落在下一个 SELECT 子句或语句边界上继续解析。
        if(!recover_) return false;
        // 严格模式不做同步，直接返回。
        inError_ = false;
        // 已经完成一次同步，清掉错误标记，让后续解析继续收集新错误。
        while(i<t.size()){
        // 一直往后找同步点。
            const auto& tok = t[i];
            // 看当前 token。
            if (tok.type=="END" || tok.lexeme==";") return true;
            // 遇到输入结束或分号，说明已经到下一条语句边界，告诉调用方"整条语句作废"。
            // 停在下一个 SELECT 子句/子句关键字上。
            // Stop at the next SELECT clause / sub-clause keyword.
            if (tok.type=="KEYWORD" && (tok.lexeme=="WHERE"||tok.lexeme=="GROUP"||tok.lexeme=="HAVING"||
                tok.lexeme=="ORDER"||tok.lexeme=="LIMIT"||tok.lexeme=="OFFSET"||tok.lexeme=="JOIN"||
                tok.lexeme=="INNER"||tok.lexeme=="LEFT"||tok.lexeme=="RIGHT"||tok.lexeme=="FULL")) return false;
            // 这些关键字各自开启一个新子句，停在这里就能继续解析剩余子句，
            // 从而做到"只丢坏掉的那一个子句"而不是整条 SELECT。
            ++i;
            // 否则继续往后跳过。
        }
        return true;
        // 扫到末尾也算同步完成。
    }
    Statement select(bool consumeSemicolon = true){
    // 解析 SELECT 语句；参数控制要不要吃掉分号（子查询场景不带分号）。
        Statement s{"Select"};expect("SELECT");
        // 消费 SELECT 关键字。
        if(keyword("DISTINCT")){++i;s.distinct=true;}
        // DISTINCT 可选，写了就置位。
        do {
        // 逐个解析投影项。
            std::shared_ptr<Expr> value;std::string alias;
            // 本项的表达式的别名。
            if(at("*")){const auto token=take();value=std::make_shared<Expr>(Expr{"Wildcard","*",{},{},token.location});}
            // 形态一：裸星号，建一个通配节点。
            else if(i+2<t.size()&&t[i].type=="IDENTIFIER"&&t[i+1].lexeme=="."&&t[i+2].lexeme=="*"){
            // 形态二：表名.*，用三个 token 的固定模式识别。
                const auto token=take();i+=2;value=std::make_shared<Expr>(Expr{"Wildcard",token.lexeme+".*",{},{},token.location});
                // 取表名 token，跳过点和星号，把词素拼成"表名.*"。
            } else value=expression();
            // 其余情况交给通用表达式解析。
            if(keyword("AS")){
            // 写了 AS 别名。
                if(value->kind=="Wildcard") fail(ErrorCode::Syntax, "Wildcard cannot have an alias", t[i].location);
                // 通配符不允许起别名，否则展开后含义不清。
                ++i;alias=identifier();
                // 消费 AS 并读别名。
            }
            if(value->kind=="Identifier"||value->kind=="Wildcard")s.selectList.push_back(value->value);
            // 兼容路径：普通列或通配符的文本同时抄一份进 selectList，供老代码使用。
            s.selectItems.push_back({std::move(value),std::move(alias)});
            // 把结构化投影项收进列表（表达式 + 别名）。
        } while(at(",")&&(++i,true));
        // 有逗号就继续解析下一个投影项。
        expect("FROM");
        // 投影列表结束后必须出现 FROM。
        if(at("(")) {
        // 分支一：FROM 后面是左括号，说明是派生表（子查询）。
            // X09：派生表 `FROM ( SELECT ... ) [AS] alias`，必须有显式别名。
            // X09: 派生表 `FROM ( SELECT ... ) [AS] alias`，必须有显式别名。
            // 第十七章：派生表嵌套必须计数，否则 `FROM (SELECT ... (SELECT ...))` 可以无界递归。
            if(++depth>256) fail(ErrorCode::Syntax, "Query nesting depth exceeded", t[i].location);
            ++i;
            // 吃掉左括号。
            auto derived = select(false);
            // 递归解析里面的 SELECT；不消费分号，因为后面还有外层内容。
            expect(")");
            // 吃掉右括号。
            std::string alias;
            // 准备别名。
            if(keyword("AS")){++i;alias=identifier();}
            // 带 AS 的显式别名。
            else if(i<t.size()&&t[i].type=="IDENTIFIER")alias=identifier();
            // 不带 AS 时的隐式别名（直接跟在右括号后面）。
            if(alias.empty()) fail(ErrorCode::Semantic, "A derived table must have an explicit alias", t[i].location);
            // 没写别名就直接拒绝：派生表没有名字，外层没法引用它。
            // 派生表输出列名不得歧义（重复 → 语义歧义错误）。
            // 派生表输出列名不得歧义（重复 → 语义歧义错误）。
            if(derived.selectList.size()==derived.selectItems.size()) {
            // 只有当兼容用的 selectList 与结构化投影项一一对应时才做静态判重。
                std::vector<std::string> names;
                // 收集内层投影的输出列名。
                bool opaque=false;
                // 标记是否存在"静态看不出来"的投影项。
                for(const auto& item: derived.selectItems){
                // 逐个看内层投影项。
                    std::string name;
                    // 本项的输出列名。
                    if(!item.alias.empty()){name=item.alias;}
                    // 有别名就用别名，它是最终输出列名。
                    else if(item.expression&&item.expression->kind=="Identifier"){name=item.expression->value;}
                    // 否则若投影是普通列，列名就是输出名。
                    else { opaque=true; break; } // wildcard / 表达式：静态无法判重，跳过。
                    // 其它形态（通配符展开、计算表达式）静态判断不了，直接放弃判重。
                    names.push_back(name);
                    // 记下这一列的名字。
                }
                if(!opaque){
                // 只有所有列名都清楚时才检查。
                    auto sorted=names;std::sort(sorted.begin(),sorted.end());
                    // 复制一份并排序，让相同名字在排序后彼此相邻。
                    if(std::adjacent_find(sorted.begin(),sorted.end())!=sorted.end())
                        fail(ErrorCode::Semantic, "Derived table output column name is ambiguous", t[i].location);
                    // 排序后存在相邻相等元素，说明有重名列，派生表按名字引用会歧义，拒绝。
                }
            }
            if(!derived.selectList.empty())s.selectList=derived.selectList;
            // 把内层的投影文本透传给外层，保持兼容路径可用。
            s.fromSubquery=std::make_shared<Statement>(std::move(derived));
            // 把内层 SELECT 的语法树挂到外层语句上。
            s.tableAlias=alias;
            // 别名就是派生表对外使用的名字。
            s.table=alias; // 3.3 planner 以 Scope 链消费 fromSubquery；此处占位保持既有表路径兼容。
            // 占位写法：planner 已经改走 Scope 链消费 fromSubquery，这里把别名叫表名
            // 只是为了让仍按"表名"取数的老路径不至于拿到空串。
            --depth;
        } else {
        // 分支二：FROM 后面是普通表。
            s.table=identifier();
            // 读表名。
            if(keyword("AS")){++i;s.tableAlias=identifier();}
            // 带 AS 的别名。
            else if(i<t.size()&&t[i].type=="IDENTIFIER")s.tableAlias=identifier();
            // 不带 AS 的隐式别名。
        }
        while(keyword("JOIN")||keyword("INNER")||keyword("LEFT")||keyword("RIGHT")||keyword("FULL")) {
        // 循环解析所有 JOIN 子句。
            if(s.joins.size()>=32) fail(ErrorCode::Syntax, "Join count exceeds 32", t[i].location);
            // 连接个数设上限 32，避免计划规模失控。
            Join join;
            // 准备一个连接子句。
            if(keyword("LEFT")){++i;join.left=true;if(keyword("OUTER"))++i;}
            // 左外连接，OUTER 可选。
            else if(keyword("RIGHT")){++i;join.right=true;if(keyword("OUTER"))++i;}
            // 右外连接，OUTER 可选。
            else if(keyword("FULL")){++i;join.left=true;join.right=true;if(keyword("OUTER"))++i;}
            // 全外连接同时置左、右两个标记，因为它两侧都可能补 NULL。
            else if(keyword("INNER"))++i;
            // 内连接，只需跳过 INNER。
            expect("JOIN");join.table=identifier();
            // 消费 JOIN 并读被连接的表名。
            if(keyword("AS")){++i;join.alias=identifier();}
            // 带 AS 的右表别名。
            else if(i<t.size()&&t[i].type=="IDENTIFIER")join.alias=identifier();
            // 隐式别名。
            expect("ON");join.on=expression();s.joins.push_back(std::move(join));
            // 必须带 ON 条件，解析后把整个连接子句收进列表。
        }
        bool clauseRecovered = false;
        // 记录本条 SELECT 是否发生过子句级错误恢复。
        const auto clauseGuard = [&](const auto& body){
        // 局部工具：把每个子句的解析包一层"出错就同步"的保护。
            if (!recover_) { body(); return; }
            // 严格模式直接执行，异常一路抛给上层。
            try { body(); } catch (const Recovered&) { clauseRecovered = true; syncClause(); }
            // 恢复模式下捕获子句错误，打到下一个子句或语句边界继续解析。
        };
        if(keyword("WHERE")){++i;clauseGuard([&]{s.where=expression();});}
        // WHERE 子句：消费关键字后在保护下解析条件。
        if(keyword("GROUP")){
        // GROUP BY 子句。
            ++i;expect("BY");
            // 消费 GROUP 与 BY。
            do {clauseGuard([&]{s.groupBy.push_back(expression());});} while(at(",")&&(++i,true));
            // 逐个解析分组键，逗号分隔；每一项都单独受保护，坏一个不影响其余。
        }
        if(keyword("HAVING")){++i;clauseGuard([&]{s.having=expression();});}
        // HAVING 子句。
        if(keyword("ORDER")){
        // ORDER BY 子句。
            ++i;expect("BY");
            // 消费 ORDER 与 BY。
            do {auto value=expression();bool descending=false;
            // 解析一个排序键表达式，并准备升降序标志。
                if(keyword("ASC"))++i;else if(keyword("DESC")){++i;descending=true;}
                // ASC 显式升序；DESC 置为降序；都不写则默认升序。
                std::optional<bool> nullsFirst;
                // 记录 NULLS FIRST/LAST 的显式指定。
                if(keyword("NULLS")){++i;if(keyword("FIRST")){++i;nullsFirst=true;}else {expect("LAST");nullsFirst=false;}}
                // 写了 NULLS 就必须跟 FIRST 或 LAST，对应 NULL 排在最前或最后。
                s.orderBy.push_back({std::move(value),descending,nullsFirst});
                // 把这一项排序键收进列表。
            } while(at(",")&&(++i,true));
            // 逗号分隔，继续读下一个排序键。
        }
        if(keyword("LIMIT")){++i;clauseGuard([&]{s.limit=unsignedCount();});}
        // LIMIT 子句，条数必须是非负整数。
        if(keyword("OFFSET")){++i;clauseGuard([&]{s.offset=unsignedCount();});}
        // OFFSET 子句，偏移量同样是非负整数。
        if (clauseRecovered) s.invalid = true;
        // 只要发生过子句级恢复，这条 SELECT 就被标记为无效，调用方不要执行它。
        if (consumeSemicolon) semicolon();
        // 顶层语句需要吃掉分号；作为子查询解析时不吃。
        return s;
        // 返回 SELECT 语句。
    }
    std::uint64_t unsignedCount(){
    // 读一个无符号整数，用于 LIMIT/OFFSET 的条数。
        const auto& token=take();std::uint64_t value=0;
        // 取出 token 并准备接收解析结果。
        if(token.type!="INTEGER") fail(ErrorCode::Syntax, "Expected non-negative integer count", token.location, token.endLocation, token.lexeme, {"INTEGER"});
        // 不是整数 token（负数会被词法切成符号加数字）就报错。
        const auto parsed=std::from_chars(token.lexeme.data(),token.lexeme.data()+token.lexeme.size(),value);
        // 文本转无符号整数。
        if(parsed.ec!=std::errc{}||parsed.ptr!=token.lexeme.data()+token.lexeme.size()) fail(ErrorCode::Syntax, "Pagination count exceeds UINT64 range", token.location);
        // 溢出或没读完整个文本都说明数值超范围。
        return value;
        // 返回解析出的数值。
    }
    void derivedTarget(Statement& s) {
        expect("(");
        auto derived=select(false);
        expect(")");
        std::string alias;
        if(keyword("AS")){++i;alias=identifier();}
        else if(i<t.size()&&t[i].type=="IDENTIFIER")alias=identifier();
        if(alias.empty()) fail(ErrorCode::Semantic, "A derived table must have an explicit alias", t[i].location);
        s.fromSubquery=std::make_shared<Statement>(std::move(derived));
        s.tableAlias=alias;s.table=alias;
    }
    Statement remove(){Statement s{"Delete"};expect("DELETE");expect("FROM");if(at("("))derivedTarget(s);else s.table=identifier();if(keyword("WHERE")){++i;s.where=expression();}semicolon();return s;}
    std::shared_ptr<Expr> expression(){auto left=conjunction();while(keyword("OR")){++i;left=std::make_shared<Expr>(Expr{"Binary","OR",left,conjunction()});}return left;}
    // 表达式的最外层：先解析一个合取式，再不断看后面有没有 OR；
    // 每读到一个 OR 就把左边已解析结果和右边新的合取式拼成 Binary 节点。
    // 因为 OR 是左结合的，left 始终代表"已经拼好的整个左侧"。
    std::shared_ptr<Expr> conjunction(){auto left=negation();while(keyword("AND")){++i;left=std::make_shared<Expr>(Expr{"Binary","AND",left,negation()});}return left;}
    // 合取层：逻辑与。先解析一个否定式，再循环吃掉 AND 与右侧否定式，
    // 拼成 Binary 节点。AND 优先级高于 OR，所以它被放在 OR 的下层递归。
    std::size_t depth=0;
    // 当前表达式的嵌套深度，用来防止超长表达式把递归栈打爆。
    std::shared_ptr<Expr> negation(){if(keyword("NOT")){if(++depth>256) fail(ErrorCode::Syntax, "Expression depth exceeded", t[i].location);++i;auto child=negation();--depth;return std::make_shared<Expr>(Expr{"Unary","NOT",child,{}});}return comparison();}
    // 否定层：如果读到 NOT，就先加深深度计数并检查是否超过 256 层，超了就报错；
    // 然后消费 NOT，递归解析它作用的对象，再把深度计数还原，返回一个 Unary 节点。
    // 没有 NOT 时直接下沉到比较层。NOT 可以连续出现（NOT NOT x），所以递归的是自己。
    std::shared_ptr<Expr> comparison(){
    // 比较层：处理 IN、IS NULL 以及 = != < <= > >= 这些比较运算。
        auto left=addition();
        // 先解析左边的算术表达式。
        if(keyword("IN")||keyword("NOT")){
        // 后面可能是 IN(...) 或 NOT IN(...)。
            const bool negate=keyword("NOT");const auto op=take();
            // 记下是不是取反形式，并消费掉 NOT 或 IN 这个 token（位置信息留着报错用）。
            if(negate)expect("IN");
            // 如果是 NOT，则后面必须紧跟 IN，否则这个 NOT 用在了不支持的位置。
            if(++depth>256) fail(ErrorCode::Syntax, "Expression depth exceeded", op.location);
            // 进入嵌套结构，深度加一并做上限检查。
            expect("(");
            // 无论哪种形态，IN 后面都是左括号。
            if(keyword("SELECT")){
            // 形态一：IN (SELECT ...) 子查询。
                const auto start=i;
                // 记下子查询起始 token 下标。
                auto query=select(false);
                // 递归解析内层 SELECT，不消费分号。
                const auto text=tokenText(start,i);
                // 把这段子查询的 token 重新拼成文本，保存一份原始写法。
                expect(")");--depth;
                // 吃掉右括号，深度还原。
                auto subquery=std::make_shared<Expr>(Expr{"InSubquery","",left,{},op.location,text});
                // 构造 IN 子查询节点，左子树是待比较的表达式，text 记录原文。
                subquery->subquery=std::make_shared<Statement>(std::move(query));
                // 把结构化子查询语法树挂到节点上，供 planner 做对象身份分析。
                return negate?std::make_shared<Expr>(Expr{"Unary","NOT",subquery,{},op.location}):subquery;
                // NOT IN 时在外面再包一层 Unary NOT，否则直接返回 IN 节点。
            }
            if(at(")")) fail(ErrorCode::Syntax, "IN requires a non-empty value list", t[i].location);
            // 形态二：值列表。空的 () 没有意义，直接报错。
            std::vector<std::shared_ptr<Expr>> terms;
            // 准备装"左值 = 每个列表项"这些等值比较节点。
            do {
            // 逐个读列表项。
                if(terms.size()>=128) fail(ErrorCode::Syntax, "IN list exceeds 128 values", op.location);
                // 列表长度上限 128，防止生成过大的表达式树。
                terms.push_back(std::make_shared<Expr>(Expr{"Binary","=",left,addition(),op.location}));
                // 每读一项就生成一个"左值 = 该项"的等值比较节点收起来。
            } while(at(",")&&(++i,true));
            expect(")");--depth;
            // 逗号分隔；最后吃掉右括号并还原深度。
            // 平衡 OR 树避免长列表产生线性递归深度。
            // 平衡 OR 树避免长列表产生线性递归深度。
            while(terms.size()>1){
            // 反复两两合并，直到只剩一个节点，形成接近平衡的 OR 树。
                std::vector<std::shared_ptr<Expr>> next;
                // 本轮合并后的结果。
                for(std::size_t n=0;n<terms.size();n+=2)
                // 每次取两个相邻节点合并。
                    next.push_back(n+1<terms.size()?std::make_shared<Expr>(Expr{"Binary","OR",terms[n],terms[n+1],op.location}):terms[n]);
                    // 成对时拼成 OR 节点；落单时直接搬到下一轮。
                terms=std::move(next);
                // 用本轮结果替换原列表，继续下一轮。
            }
            return negate?std::make_shared<Expr>(Expr{"Unary","NOT",terms.front(),{},op.location}):terms.front();
            // NOT IN 时把整棵 OR 树取反；否则直接返回 OR 树。
        }
        if(keyword("IS")){auto op=take();bool negate=keyword("NOT");if(negate)++i;expect("NULL");return std::make_shared<Expr>(Expr{"Unary",negate?"IS NOT NULL":"IS NULL",left,{},op.location});}
        // IS NULL / IS NOT NULL：消费 IS，看有没有 NOT，再要求必须有 NULL，
        // 最后按是否取反生成 Unary 节点，节点种类名直接写成可读的 IS NULL / IS NOT NULL。
        // 方言归一：`==` 等价 `=`，`<>` 等价 `!=`。词素保持源码原文，语义统一按规范算子处理。
        // 方言归一：`==` 等价 `=`，`<>` 等价 `!=`。词素保持源码原文，语义统一按规范算子处理。
        if(at("=")||at("==")||at("!=")||at("<>")||at("<")||at("<=")||at(">")||at(">=")){
        // 八种比较运算符任意一种都进入比较分支。
            auto op=take();auto right=addition();
            // 取运算符，再解析右边的算术表达式。
            const auto normalized=op.lexeme=="=="?std::string("="):op.lexeme=="<>"?std::string("!="):op.lexeme;
            // 归一化：== 统一记成 =，<> 统一记成 !=，其余保持原样。
            // 这样后续语义分析与执行层只需认识一套规范运算符。
            return std::make_shared<Expr>(Expr{"Binary",normalized,left,right,op.location});
            // 生成 Binary 节点返回，位置取运算符所在位置。
        }
        return left;
        // 没有比较运算符，说明这个"比较表达式"其实就是左边的算术表达式，直接返回。
    }
    std::shared_ptr<Expr> addition(){auto left=multiplication();while(at("+")||at("-")){auto op=take();left=std::make_shared<Expr>(Expr{"Binary",op.lexeme,left,multiplication(),op.location});}return left;}
    // 加减层：先解析一个乘法层结果，再循环处理 + 与 -；
    // 每轮把左边的已有结果与右侧新的乘法层结果拼成 Binary 节点，实现左结合。
    std::shared_ptr<Expr> multiplication(){auto left=unary();while(at("*")||at("/")){auto op=take();left=std::make_shared<Expr>(Expr{"Binary",op.lexeme,left,unary(),op.location});}return left;}
    // 乘除层：结构同上，但继承的是 unary（一元层），优先级高于加减。
    std::shared_ptr<Expr> unary(){
    // 一元层：处理 EXISTS、聚合函数、CAST、日期与布尔字面量、正负号、以及括号。
        if(keyword("EXISTS")){
        // 形态一：EXISTS (SELECT ...)。
            const auto token=take();expect("(");
            // 取 EXISTS 的 token（位置信息要留给节点），并吃掉左括号。
            if(!keyword("SELECT")) fail(ErrorCode::Syntax, "EXISTS requires a SELECT subquery", t[i].location);
            // EXISTS 后面必须直接跟 SELECT，否则语义不成立。
            const auto start=i;auto query=select(false);const auto text=tokenText(start,i);expect(")");
            // 解析内层 SELECT，拼出它的原文，最后吃掉右括号。
            auto node=std::make_shared<Expr>(Expr{"Exists","",nullptr,{},token.location,text});
            // 构造 EXISTS 节点，左子节点为空（它没有操作数，只有一个子查询）。
            node->subquery=std::make_shared<Statement>(std::move(query));
            // 挂上结构化的子查询语法树。
            return node;
            // 返回 EXISTS 节点。
        }
        if(i+1<t.size()&&t[i+1].lexeme=="("&&
           (keyword("COUNT")||keyword("SUM")||keyword("AVG")||keyword("MIN")||keyword("MAX"))){
        // 形态二：聚合函数，靠"名字后面紧跟左括号"这一点与普通函数式写法区分。
            const auto token=take();auto name=token.lexeme;
            // 取函数名 token 与它的原文。
            std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return static_cast<char>(std::toupper(c));});
            // 把函数名统一转大写，方便后面与 COUNT 等规范名比较。
            if(++depth>256) fail(ErrorCode::Syntax, "Expression depth exceeded", token.location);
            // 进入嵌套结构，检查深度上限。
            expect("(");std::shared_ptr<Expr> argument;
            // 吃掉左括号，准备接收参数。
            if(at("*")){
            // 参数写成星号的形态（COUNT(*)）。
                const auto star=take();
                // 取星号 token。
                if(name!="COUNT") fail(ErrorCode::Syntax, "Only COUNT accepts '*'", star.location);
                // 只有 COUNT(*) 合法，SUM(*)/AVG(*) 之类没有意义。
                argument=std::make_shared<Expr>(Expr{"Wildcard","*",{},{},star.location});
                // 用通配节点表示这个参数。
            }else argument=expression();
            // 否则按普通表达式解析参数。
            expect(")");--depth;
            // 吃掉右括号并还原深度。
            return std::make_shared<Expr>(Expr{"AggregateExpr",name,argument,{},token.location});
            // 返回聚合节点：种类名是 AggregateExpr，value 是大写的函数名，左子节点是参数。
        }
        if(keyword("CAST")){
        // 形态三：CAST(表达式 AS 类型)。
            const auto token=take();
            // 取 CAST 的位置信息。
            if(++depth>256) fail(ErrorCode::Syntax, "Expression depth exceeded", token.location);
            // 深度检查。
            expect("(");auto child=expression();expect("AS");
            // 吃掉左括号，解析被转换的表达式，再吃掉 AS。
            auto target=typeName();expect(")");--depth;
            // 解析目标类型名，吃掉右括号并还原深度。
            return std::make_shared<Expr>(Expr{"Cast",target,child,{},token.location});
            // 返回转换节点：value 是目标类型文本，左子节点是源表达式。
        }
        if(keyword("DATE")&&i+1<t.size()&&t[i+1].type=="STRING"){const auto loc=t[i].location;return std::make_shared<Expr>(Expr{"Literal",literal(),{},{},loc});}
        // 形态四：日期字面量 DATE'2024-01-01'，交给 literal() 拼成规范文本后当字面量节点。
        if(keyword("NULL")||keyword("TRUE")||keyword("FALSE")){const auto token=take();return std::make_shared<Expr>(Expr{"Literal",token.lexeme,{},{},token.location});}
        // 形态五：NULL/TRUE/FALSE 三个关键字本身就是字面量，直接取原文建字面量节点。
        if(at("+")||at("-")){
        // 形态六：前面出现正负号。
            if(i+1<t.size()&&(t[i+1].type=="INTEGER"||t[i+1].type=="DECIMAL"||t[i+1].type=="FLOAT")){auto loc=t[i].location;return std::make_shared<Expr>(Expr{"Literal",literal(),{},{},loc});}
            // 若后面紧跟数字，说明这是带符号的数字字面量（如 -3），
            // 整体按一个字面量处理，而不是当成取负运算，避免语义层多算一步。
            auto op=take();
            // 否则它是作用在表达式上的一元运算符。
            if(++depth>256) fail(ErrorCode::Syntax, "Expression depth exceeded", op.location);
            // 深度检查。
            auto child=unary();--depth;
            // 递归解析被作用的表达式，再还原深度。
            return std::make_shared<Expr>(Expr{"Unary",op.lexeme,child,{},op.location});
            // 返回一元运算节点，value 就是 + 或 -。
        }
        return primary();
        // 走到这里说明是最基本的成分（括号、列名、字面量），下沉到 primary 层。
    }
    std::shared_ptr<Expr> primary(){
    // 最基本的表达式成分：括号表达式、标量子查询、列引用、字面量。
        if(at("(") && i+1<t.size()){
        // 左括号后面还有内容，需要判断它是"括号表达式"还是"标量子查询"。
            auto next=t[i+1].lexeme;for(char& c:next)if(c>='a'&&c<='z')c-=32;
            // 复制下一个 token 的词素并转大写，用于关键字比较。
            if(next=="SELECT"){
            // 括号里是 SELECT，说明这是标量子查询。
                // 第十七章：标量子查询同样计入嵌套深度，防止 `(SELECT (SELECT ...))` 无界递归。
                if(++depth>256) fail(ErrorCode::Syntax, "Query nesting depth exceeded", t[i].location);
                const auto token=take();const auto start=i;auto query=select(false);const auto text=tokenText(start,i);expect(")");--depth;
                auto node=std::make_shared<Expr>(Expr{"ScalarSubquery","",nullptr,{},token.location,text});
                // 构造标量子查询节点。
                node->subquery=std::make_shared<Statement>(std::move(query));
                // 挂上结构化子查询语法树。
                return node;
                // 返回该节点。
            }
        }
        if(at("(")){if(++depth>256) fail(ErrorCode::Syntax, "Expression depth exceeded", t[i].location);++i;auto e=expression();expect(")");--depth;return e;}auto loc=t[i].location;if(at("-")||at("+"))return std::make_shared<Expr>(Expr{"Literal",literal(),{},{},loc});const auto& x=take();if(x.type=="IDENTIFIER"){auto name=x.lexeme;if(at(".")){++i;name+="."+identifier();}return std::make_shared<Expr>(Expr{"Identifier",name,{},{},x.location});}if(x.type=="INTEGER"||x.type=="DECIMAL"||x.type=="FLOAT"||x.type=="STRING")return std::make_shared<Expr>(Expr{"Literal",x.lexeme,{},{},x.location}); fail(ErrorCode::Syntax, "Expected identifier, literal or '('", x.location, x.endLocation, x.lexeme, {"IDENTIFIER", "CONST", "("});
    // 普通括号：深度加一并检查上限，吃掉左括号，递归解析里面完整表达式，
    // 再吃掉右括号，最后还原深度并返回内部表达式（括号本身不产生节点）。
    // 先把当前位置记下来，供下面几种情况复用。
    // 情况 A：还残留正负号（例如从更外层透传下来），整体当一个字面量处理。
    // 情况 B：取一个 token，若是 IDENTIFIER，就构造列引用节点；
    //         若后面紧跟点号，就把"表名.列名"拼成一个完整名字。
    // 情况 C：若是数字或字符串 token，构造字面量节点。
    // 情况 D：以上都不是，说明这里既不是列名也不是字面量也不是左括号，报语法错误。
    }
};
}
// 严格入口（普通执行与计划生成都走这里）。与经典解析器一样，第一个错误就抛出。
// Strict entry point (used by normal execution/planning). Throws on the first
// error just like the classic parser.
std::vector<Statement> parse(const std::vector<Token>& tokens){return Parser(tokens).all();}
// 直接构造解析器并跑全量解析，不做任何容错处理。
// 容错入口：收集语法错误并继续解析。
// Recovery entry point: collects syntax errors and keeps parsing.
std::vector<Statement> parseRecoverable(const std::vector<Token>& tokens, std::vector<MiniSqlError>& errors){
    Parser parser(tokens);
    // 构造解析器。
    parser.setRecoverable(errors);
    // 打开恢复模式，把错误收集容器交给它。
    return parser.allRecoverable();
    // 跑容错解析，返回尽可能多的合法语句。
}
}
