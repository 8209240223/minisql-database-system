#include "minisql/sql/lexer.hpp"
#include <unordered_set>
namespace minisql::sql {
namespace {
const std::unordered_set<std::string>& keywords() {
// 返回主关键字表；用函数包一层是为了借助函数内 static 的线程安全惰性初始化。
    static const std::unordered_set<std::string> k{"SELECT","FROM","WHERE","CREATE","TABLE","INSERT","INTO","VALUES","DELETE","AND","OR","NOT","INT","BIGINT","FLOAT","VARCHAR","AS","DISTINCT","LIMIT","OFFSET","ORDER","BY","ASC","DESC","UPDATE","SET","JOIN","INNER","ON","LEFT","RIGHT","FULL","OUTER","CROSS","NATURAL","USING","NULL","IS","TRUE","FALSE","NULLS","FIRST","LAST","GROUP","HAVING","CHECKPOINT"};
    // 首次调用时构造这一份集合，之后所有调用共享它，不会重复建表。
    // 表中每个字符串都已是大写，所以调用方传入前必须先把名字转成大写。
    return k;
    // 返回常量引用，调用方拿到的是同一份表而不是副本，省去复制开销。
}
bool isKeyword(const std::string& normalized) {
// 判断一个大写名字是不是关键字，供识别标识符时区分 KEYWORD 与 IDENTIFIER。
    const auto& k = keywords();
    // 先取到主关键字表。
    return k.contains(normalized) || normalized=="BEGIN" || normalized=="COMMIT" || normalized=="ROLLBACK" ||
           normalized=="TRANSACTION" || normalized=="BIGINT" || normalized=="CAST" || normalized=="DEFAULT" ||
           normalized=="PRIMARY" || normalized=="KEY" || normalized=="UNIQUE" || normalized=="REFERENCES" ||
           normalized=="CHECK" || normalized=="FOREIGN" || normalized=="CONSTRAINT";
    // 先在主表里查；查不到再用后半段补判事务控制（BEGIN/COMMIT/ROLLBACK/TRANSACTION）
    // 与约束相关（PRIMARY/KEY/UNIQUE/REFERENCES/CHECK/FOREIGN/CONSTRAINT）以及 CAST/DEFAULT/BIGINT。
    // 拆成两组的目的是让主表保持紧凑，新增少量关键字时只改这一行即可。
}

// 共用扫描器。errors 为空指针时运行在严格模式：遇到第一个词法错误就按经典行为
// 抛出异常，现有所有调用点都走这条路。errors 非空时运行在恢复模式：每条词法
// 错误都记录成一条 MiniSqlError（带结束区间），随后在稳定位置继续扫描，从而
// 把整批语句里的词法错误一次性报全；合法 token 照常产出，后续语句仍能解析。
void scanImpl(const std::string& s, const std::function<void(const Token&)>& consume,
              std::vector<MiniSqlError>* errors) {
    std::size_t i=0, line=1, column=1;
    // i 是源串下标游标；line/column 是当前字符所在的行号与列号，都从 1 开始计数。
    auto digit=[](char c){return c>='0' && c<='9';};
    // 判断一个字符是不是十进制数字，数字字面量扫描要用。
    auto alpha=[](char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_';};
    // 判断一个字符能不能作为标识符的组成字符：字母或下划线。
    auto advance=[&](){
    // 前进一个字符，并在前进过程中维护行列号，这是全文唯一的"读下一字符"入口。
        const auto c=s[i++];
        // 取出当前字符，同时把下标游标往后推一格。
        if(c=='\r'){if(i<s.size()&&s[i]=='\n')++i; ++line; column=1;}
        // 遇到回车算换行：若后面紧跟换行符则把 \r\n 当成一个换行整体吃掉；
        // 行号加一，列号回到 1，表示新行从第一列开始。
        else if(c=='\n'){++line;column=1;}
        // 单独出现的换行符同样算换行，行号加一、列号归 1。
        else if((c&0xc0)!=0x80) ++column;
        // 普通字符列号加一；UTF-8 续字节（高两位为 10）不加列号，
        // 这样"中"这类三字节汉字整体只占一列，用户看到的位置与显示一致。
    };
    // 报告一个词法错误，出错区间是 [locStart, locEnd]。严格模式下抛出异常；
    // 恢复模式下记录进 errors 并返回 false，让调用方继续往下扫。
    auto report=[&](const char* message, const SourceLocation& locStart, const SourceLocation& locEnd){
        if (errors) {
        // 恢复模式：errors 不为空。
            errors->emplace_back(ErrorCode::Lexical, std::string(message),
                                 SourceLocation{locStart.line, locStart.column, locEnd.line, locEnd.column});
            // 直接在列表尾部构造一条词法错误，把起止行列号一起带上。
            return false;
            // 返回 false 表示"这次不中断，你接着扫"。
        }
        throw MiniSqlError(ErrorCode::Lexical, message, locStart);
        // 严格模式：立刻抛出，位置取错误起始点，行为与历史版本一致。
    };
    while(i<s.size()) {
    // 主循环：只要还有字符没读完就继续切 token。
        const char c=s[i];
        // 看一眼当前字符，用它来决定走哪个识别分支。
        if(c==' '||c=='\t'||c=='\r'||c=='\n'){advance();continue;}
        // 空白字符（空格、制表符、回车、换行）直接跳过，不产生 token；
        // 注意这里仍要调用 advance 才能让行列号正确推进。
        const SourceLocation loc{line,column};
        // 记下本 token 的起始行列号，最终写进 Token::location。
        const auto start=i;
        // 记下本 token 在源串里的起始下标，结束时用它切出词素值。
        const auto two=s.substr(i,2);
        // 预取当前位置往后两个字符，用来判断 -- 、/* 、>= 这类多字符记号。
        // 识别到坏 token 后该标志为真，表示这个 token 已被丢弃（错误已记录），
        // 扫描改从下一个稳定记号继续，而不是让整批语句全部作废。
        bool dropped=false;
        // dropped 为真时主循环末尾会 continue，跳过后面的产出逻辑。
        const auto dropToStable=[&](){
        // 把游标推到下一个"稳定恢复点"，用于坏 token 之后的重新同步。
            while(i<s.size()){
            // 一直往后看，直到遇到边界或到文件末尾。
                const char d=s[i];
                // 取当前字符判断是不是边界。
                // 空白分隔的 token 边界，以及语句/表达式分隔符，都是稳定恢复点。
                if(d==' '||d=='\t'||d=='\r'||d=='\n'||d==';'||d==')'||d=='('||d==',')return;
                // 遇到这些字符就停下，说明已经停在一个新 token 的自然起点上。
                advance();
                // 否则继续前进，把坏 token 的剩余部分吃掉。
            }
        };
        const auto drop=[&](){dropped=true;dropToStable();};
        // 组合动作：先标记丢弃，再把游标推到下一个稳定点。
        if(two=="--"){while(i<s.size()&&s[i]!='\n'&&s[i]!='\r')advance();continue;}
        // 行注释：-- 之后的字符一直吃到行尾（不含换行符本身），整体不产生 token。
        if(two=="/*"){
        // 块注释：从 /* 开始。
            advance();advance();
            // 吃掉开头的斜杠和星号两个字符。
            while(i<s.size()&&s.substr(i,2)!="*/")advance();
            // 一路扫描直到遇到配对的 */，或者扫到源串末尾。
            if(i==s.size()){const SourceLocation end{line,column};report("Unterminated block comment",loc,end);drop();}
            // 扫到末尾还没闭合，说明块注释没写完：按词法错误上报并丢弃；
            // drop 在严格模式下不会被执行到，因为 report 已经抛出异常。
            advance();advance();continue;
            // 吃掉闭合的 */ 两个字符，然后进入下一轮循环继续找 token。
        }
        std::string type;
        // 下面开始正式识别 token，这里准备存放它的种别码。
        if(alpha(c)){
        // 分支一：以字母或下划线开头，可能的关键字或标识符。
            while(i<s.size()&&(alpha(s[i])||digit(s[i])))advance();
            // 贪心吃下后续所有字母、数字、下划线，得到完整的名字。
            auto normalized=s.substr(start,i-start);
            // 切出原始词素，保留用户书写的大小写。
            for(char& ch:normalized)if(ch>='a'&&ch<='z')ch-=32;
            // 把副本里的小写字母统一减 32 变成大写，实现大小写不敏感比较。
            type=isKeyword(normalized)?"KEYWORD":"IDENTIFIER";
            // 转大写后能在关键字表里查到的算 KEYWORD，否则算普通 IDENTIFIER。
        } else if(digit(c)){
        // 分支二：以数字开头，进入数字字面量识别。
            while(i<s.size()&&digit(s[i]))advance();
            // 先吃掉整数部分的所有数字。
            type="INTEGER";
            // 默认按整数处理，后面若出现小数点或指数再升级类型。
            if(i<s.size()&&s[i]=='.'){
            // 遇到小数点，说明这是小数。
                advance();
                // 吃掉小数点。
                if(i==s.size()||!digit(s[i])){const SourceLocation end{line,column};report("Malformed DECIMAL literal",loc,end);drop();continue;}
                // 小数点后必须紧跟数字；3. 或 3.x 这种写法按词法错误上报并跳过。
                while(i<s.size()&&digit(s[i]))advance();
                // 吃掉小数部分的全部数字。
                type="DECIMAL";
                // 种别码升级成 DECIMAL，表示带标度的定点数。
            }
            if(i<s.size()&&(s[i]=='e'||s[i]=='E')){
            // 遇到 e/E，说明这是科学计数法。
                advance();
                // 吃掉指数标记字符。
                if(i<s.size()&&(s[i]=='+'||s[i]=='-'))advance();
                // 指数可以带一个正负号，有就吃掉。
                if(i==s.size()||!digit(s[i])){const SourceLocation end{line,column};report("Malformed FLOAT literal",loc,end);drop();continue;}
                // 指数部分必须至少有一位数字，否则是残缺写法，报错并跳过。
                while(i<s.size()&&digit(s[i]))advance();
                // 吃掉指数的全部数字位。
                type="FLOAT";
                // 带指数的按 FLOAT 处理，因为它需要浮点精度来表示。
            }
            if(i<s.size()&&(s[i]=='.'||alpha(s[i]))){const SourceLocation end{line,column};report("Unsupported or malformed numeric literal",loc,end);drop();continue;}
            // 若数字后面还紧跟着点或字母（如 1.2.3、12abc），说明这不是一个合法数字：
            // 上报"数字字面量非法"并丢弃，避免后面把 12abc 切成 12 和 abc 两个 token。
        } else if(c=='\''){
        // 分支三：单引号开头，进入字符串字面量识别。
            advance();bool closed=false;bool recovered=false;
            // 吃掉起始引号；closed 表示是否读到配对的结束引号；
            // recovered 表示是否已经按"字符串里换行"的错误路径处理过。
            while(i<s.size()&&!recovered){
            // 一直读，直到成功闭合或走入错误恢复路径。
                if(s[i]=='\r'||s[i]=='\n'){
                // 字符串里出现换行。
                    const SourceLocation end{line,column};
                    // 记下出错时的结束位置。
                    report("Newline in string literal is not supported",loc,end);
                    // 上报"不支持字符串内换行"，严格模式在此抛出。
                    recovered=true;
                    // 标记已走恢复路径，循环随即结束。
                    if(!closed)drop(); // 跳过坏字符串，但保持继续扫描
                    // 尚未闭合时把这个坏 token 丢掉并重新同步到下一个稳定点。
                    break;
                    // 跳出字符串扫描循环。
                }
                if(s[i]=='\''){
                // 遇到单引号，可能是结束引号，也可能是转义。
                    advance();
                    // 先吃掉这个引号。
                    if(i<s.size()&&s[i]=='\''){advance();continue;}
                    // 若紧跟第二个引号，则按 SQL 惯例把 '' 解释成一个真正的单引号字符，
                    // 吃掉后继续扫描字符串内容，不算结束。
                    closed=true;break;
                    // 否则这就是结束引号，标记闭合并跳出。
                }
                advance();
                // 普通字符直接吃掉，继续看下一个。
            }
            if(!closed&&!recovered){const SourceLocation end{line,column};report("Unterminated string literal",loc,end);drop();}
            // 一路读到文件末尾都没闭合，属于未闭合字符串：上报并丢弃该 token。
            if(recovered){continue;} // 已经丢弃并安全跳过换行，直接进入下一轮
            // 若走的是换行错误路径，这里直接继续，避免把半截字符串当合法 token 产出。
            type="STRING";
            // 正常闭合，种别码为 STRING。
        } else if(two==">="||two=="<="||two=="!="||two=="=="||two=="<>"){advance();advance();type="OPERATOR";}
        // 分支四：双字符运算符优先判断，>=、<=、!=、==、<> 必须整体作为一个 token，
        // 否则会被拆成两个单字符运算符，导致比较语义出错。
        else if(std::string("=<>+-*/").find(c)!=std::string::npos){advance();type="OPERATOR";}
        // 分支五：单字符运算符 = < > + - * /。
        else if(c=='.' && i+1<s.size() && digit(s[i+1])){const SourceLocation end{line,column};report("Unsupported numeric literal",loc,end);drop();continue;}
        // 分支六：点号后面紧跟数字（如 .5）在本方言里不支持，明确报错而不是当成点分隔符。
        else if(std::string("(),;.").find(c)!=std::string::npos){advance();type="DELIMITER";}
        // 分支七：括号、逗号、分号、点号属分隔符，只起分隔作用不参与运算。
        else{const SourceLocation end{line,column};report("Illegal character",loc,end);advance();drop();continue;}
        // 分支八：以上都不匹配，说明出现了方言不认识的字符（如 # 或 @）：
        // 按"非法字符"上报，吃掉它并重新同步，保证后面的合法 token 不被连带丢弃。
        if(dropped)continue;
        // 如果本次识别过程中已判定为坏 token，就不产出，直接进入下一轮。
        const SourceLocation endLoc{line,column};
        // 记录 token 结束位置：此时游标已停在 token 之后。
        consume({type,s.substr(start,i-start),loc,endLoc});
        // 产出这个 token：种别码 + 词素值 + 起始位置 + 结束位置，交给回调。
    }
    consume({"END","",{line,column},{line,column}});
    // 全部字符扫完后补一个 END 结束标记，方便上层解析循环用它判断"输入结束"。
}
} // namespace

void scanTokens(const std::string& s, const std::function<void(const Token&)>& consume) {
// 流式扫描的公开入口。
    scanImpl(s, consume, nullptr);
    // 第三个参数传 nullptr，即走严格模式：遇到第一个词法错误直接抛异常。
}
std::vector<Token> tokenize(const std::string& s) {
// 严格模式的批量入口，返回整段源码的 token 列表。
    std::vector<Token> out;
    // 准备结果容器。
    scanTokens(s, [&](const Token& token) { out.push_back(token); });
    // 把流式接口的回调接成一个"往容器里塞"的动作，复用同一套扫描逻辑。
    return out;
    // 返回收集到的 token 表（末尾包含 END）。
}
std::vector<Token> tokenizeRecoverable(const std::string& s, std::vector<MiniSqlError>& errors) {
// 容错模式的批量入口：不中断，把所有词法错误一次性收集进 errors。
    std::vector<Token> out;
    // 准备结果容器。
    scanImpl(s, [&](const Token& token) { out.push_back(token); }, &errors);
    // 第三个参数传 &errors，扫描器切换到恢复模式，错误只记录不抛出。
    return out;
    // 返回合法 token 表，调用方再配合 errors 生成完整诊断。
}
} // namespace minisql
