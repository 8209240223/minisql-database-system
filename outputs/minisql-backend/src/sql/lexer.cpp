#include "minisql/sql/lexer.hpp"
#include <cstdio>
#include <unordered_set>
namespace minisql::sql {
void scanTokens(const std::string& s, const std::function<void(const Token&)>& consume) {
    const std::unordered_set<std::string> keywords{"SELECT","FROM","WHERE","CREATE","TABLE","INSERT","INTO","VALUES","DELETE","AND","OR","NOT","INT","BIGINT","FLOAT","VARCHAR","AS","DISTINCT","LIMIT","OFFSET","ORDER","BY","ASC","DESC","UPDATE","SET","JOIN","INNER","ON","LEFT","RIGHT","FULL","OUTER","CROSS","NATURAL","USING","NULL","IS","TRUE","FALSE","NULLS","FIRST","LAST","GROUP","HAVING","CHECKPOINT"};
    std::size_t i=0, line=1, column=1;
    auto digit=[](char c){return c>='0' && c<='9';};
    auto alpha=[](char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_';};
    auto advance=[&](){
        const auto c=static_cast<unsigned char>(s[i++]);
        if(c=='\r'){if(i<s.size()&&s[i]=='\n')++i; ++line; column=1;}
        else if(c=='\n'){++line;column=1;}
        else if((c&0xc0)!=0x80) ++column;
    };
    while(i<s.size()) {
        const char c=s[i];
        if(c==' '||c=='\t'||c=='\r'||c=='\n'){advance();continue;}
        const SourceLocation loc{line,column};
        auto describeChar = [](unsigned char ch) -> std::string {
            if (ch >= 0x20 && ch < 0x7F) return {"'", static_cast<char>(ch), "'"};
            char buf[8]; std::snprintf(buf, sizeof(buf), "\\x%02X", ch); return buf;
        };
        auto describeContext = [&]() -> std::string {
            if (i >= s.size()) return "reached end of input";
            std::string peek = s.substr(i, 8);
            for (auto& ch : peek) if ((static_cast<unsigned char>(ch) < 0x20 || ch == 0x7F)) ch = '.';
            return "'" + peek + "'";
        };
        auto fail = [&](const std::string& message){throw MiniSqlError(ErrorCode::Lexical,message,loc);};
        const auto start=i;
        const auto two=s.substr(i,2);
        if(two=="--"){while(i<s.size()&&s[i]!='\n'&&s[i]!='\r')advance();continue;}
        if(two=="/*"){
            advance();advance();
            while(i<s.size()&&s.substr(i,2)!="*/")advance();
            if(i==s.size())fail("Unterminated block comment: reached end of input, expected '*/'");
            advance();advance();continue;
        }
        std::string type;
        if(alpha(c)){
            while(i<s.size()&&(alpha(s[i])||digit(s[i])))advance();
            auto normalized=s.substr(start,i-start);
            for(char& ch:normalized)if(ch>='a'&&ch<='z')ch-=32;
            type=(keywords.contains(normalized)||normalized=="BEGIN"||normalized=="COMMIT"||normalized=="ROLLBACK"||normalized=="TRANSACTION"||normalized=="BIGINT"||normalized=="CAST"||normalized=="DEFAULT"||normalized=="PRIMARY"||normalized=="KEY"||normalized=="UNIQUE"||normalized=="REFERENCES"||normalized=="CHECK"||normalized=="FOREIGN"||normalized=="CONSTRAINT")?"KEYWORD":"IDENTIFIER";
        } else if(digit(c)){
            while(i<s.size()&&digit(s[i]))advance();
            type="INTEGER";
            if(i<s.size()&&s[i]=='.'){
                advance();
                if(i==s.size()||!digit(s[i]))fail("Malformed DECIMAL literal: expected digits after '.' but found "+(i>=s.size()?std::string("end of input"):describeChar(static_cast<unsigned char>(s[i]))));
                while(i<s.size()&&digit(s[i]))advance();
                type="DECIMAL";
            }
            if(i<s.size()&&(s[i]=='e'||s[i]=='E')){
                advance();
                if(i<s.size()&&(s[i]=='+'||s[i]=='-'))advance();
                if(i==s.size()||!digit(s[i]))fail("Malformed FLOAT literal: expected digits after exponent but found "+(i>=s.size()?std::string("end of input"):describeChar(static_cast<unsigned char>(s[i]))));
                while(i<s.size()&&digit(s[i]))advance();
                type="FLOAT";
            }
            if(i<s.size()&&(s[i]=='.'||alpha(s[i])))fail("Unsupported numeric literal: '"+s.substr(start,i-start)+"' followed by "+describeChar(static_cast<unsigned char>(s[i]))+" (expected digits or operator)");
        } else if(c=='\''){
            advance();bool closed=false;
            while(i<s.size()){
                if(s[i]=='\r'||s[i]=='\n')fail("Newline in string literal is not supported; use '\\n' escape instead");
                if(s[i]=='\''){advance();if(i<s.size()&&s[i]=='\''){advance();continue;}closed=true;break;}
                advance();
            }
            if(!closed)fail("Unterminated string literal: reached end of input, expected closing '\\''");
            type="STRING";
        } else if(two=="=="||two=="<>"){fail("Unsupported comparison operator '"+two+"' (did you mean '=' or '!=' ?);}
        else if(two==">="||two=="<="||two=="!="){advance();advance();type="OPERATOR";}
        else if(std::string("=<>+-*/").find(c)!=std::string::npos){advance();type="OPERATOR";}
        else if(c=='.' && i+1<s.size() && digit(s[i+1]))fail("Unsupported numeric literal starting with '.': use '0.' prefix (found "+describeContext()+")");
        else if(std::string("(),;.").find(c)!=std::string::npos){advance();type="DELIMITER";}
        else fail("Illegal character "+describeChar(static_cast<unsigned char>(c))+" at "+describeContext());
        consume({type,s.substr(start,i-start),loc});
    }
    consume({"END","",{line,column}});
}
std::vector<Token> tokenize(const std::string& s) {
    std::vector<Token> out;
    scanTokens(s, [&](const Token& token) { out.push_back(token); });
    return out;
}
}
