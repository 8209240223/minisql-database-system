#include "minisql/sql/lexer.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <iterator>
int main(){
    // 独立的词法入口：从标准输入读入整段源码，只跑词法分析并输出 token 列表。
    using nlohmann::json;
    // 简写 JSON 命名空间。
    try{
    // 词法错误以异常形式抛出，这里统一捕获后转成 JSON 错误输出。
        const std::string source{std::istreambuf_iterator<char>(std::cin),{}};
        // 把标准输入整个读进来，作为待分析的 SQL 源文本。
        auto tokens=json::array();
        // 准备装 token 的 JSON 数组。
        for(const auto& t:minisql::sql::tokenize(source))tokens.push_back({{"type",t.type},{"text",t.lexeme},{"line",t.location.line},{"column",t.location.column}});
        // 逐个 token 输出四个字段：种别码、词素值、行号、列号。
        std::cout<<json{{"success",true},{"tokens",tokens},{"columns",json::array()},{"rows",json::array()},{"plan",json::array()},{"statements",0},{"affectedRows",0},{"stages",{{"lexer","passed"},{"parser","notImplemented"},{"semantic","notImplemented"},{"planner","notImplemented"}}}}.dump();
        // 输出统一的结果外壳：词法阶段 passed，其余阶段标注 notImplemented。
        // 这样前端可以用同一套协议解析所有阶段的结果，不必为词法单独写分支。
        return 0;
        // 正常结束返回 0。
    }catch(const minisql::MiniSqlError& e){std::cout<<e.toJson().dump();return 1;}
    // 捕获词法错误，把错误对象序列化成 JSON 输出，并用退出码 1 表示失败。
}
// 入口函数结束。
