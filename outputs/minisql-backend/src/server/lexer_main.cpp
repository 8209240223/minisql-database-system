#include "minisql/sql/lexer.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <iterator>
int main(){
    using nlohmann::json;
    try{
        const std::string source{std::istreambuf_iterator<char>(std::cin),{}};
        auto tokens=json::array();
        for(const auto& t:minisql::sql::tokenize(source))tokens.push_back({{"type",t.type},{"text",t.lexeme},{"line",t.location.line},{"column",t.location.column}});
        std::cout<<json{{"success",true},{"tokens",tokens},{"columns",json::array()},{"rows",json::array()},{"plan",json::array()},{"statements",0},{"affectedRows",0},{"stages",{{"lexer","passed"},{"parser","notImplemented"},{"semantic","notImplemented"},{"planner","notImplemented"}}}}.dump();
        return 0;
    }catch(const minisql::MiniSqlError& e){std::cout<<e.toJson().dump();return 1;}
}
