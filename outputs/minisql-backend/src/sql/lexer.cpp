#include "minisql/sql/lexer.hpp"
#include <unordered_set>
namespace minisql::sql {
namespace {
const std::unordered_set<std::string>& keywords() {
    static const std::unordered_set<std::string> k{"SELECT","FROM","WHERE","CREATE","TABLE","INSERT","INTO","VALUES","DELETE","AND","OR","NOT","INT","BIGINT","FLOAT","VARCHAR","AS","DISTINCT","LIMIT","OFFSET","ORDER","BY","ASC","DESC","UPDATE","SET","JOIN","INNER","ON","LEFT","RIGHT","FULL","OUTER","CROSS","NATURAL","USING","NULL","IS","TRUE","FALSE","NULLS","FIRST","LAST","GROUP","HAVING","CHECKPOINT"};
    return k;
}
bool isKeyword(const std::string& normalized) {
    const auto& k = keywords();
    return k.contains(normalized) || normalized=="BEGIN" || normalized=="COMMIT" || normalized=="ROLLBACK" ||
           normalized=="TRANSACTION" || normalized=="BIGINT" || normalized=="CAST" || normalized=="DEFAULT" ||
           normalized=="PRIMARY" || normalized=="KEY" || normalized=="UNIQUE" || normalized=="REFERENCES" ||
           normalized=="CHECK" || normalized=="FOREIGN" || normalized=="CONSTRAINT";
}

// Shared scanner. When `errors` is nullptr it runs in strict mode (throws on
// the first lexical error, the classic behaviour used by all existing call
// sites). When `errors` is non-null it runs in recovery mode: each lexical
// error is recorded as a MiniSqlError (with an end span) and scanning resumes
// at a stable point so every lexical error in the batch is reported at once.
void scanImpl(const std::string& s, const std::function<void(const Token&)>& consume,
              std::vector<MiniSqlError>* errors) {
    std::size_t i=0, line=1, column=1;
    auto digit=[](char c){return c>='0' && c<='9';};
    auto alpha=[](char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_';};
    auto advance=[&](){
        const auto c=s[i++];
        if(c=='\r'){if(i<s.size()&&s[i]=='\n')++i; ++line; column=1;}
        else if(c=='\n'){++line;column=1;}
        else if((c&0xc0)!=0x80) ++column;
    };
    // Reports a lexical error at [locStart, locEnd]. In strict mode throws;
    // in recovery mode records and returns false so the caller continues.
    auto report=[&](const char* message, const SourceLocation& locStart, const SourceLocation& locEnd){
        if (errors) {
            errors->emplace_back(ErrorCode::Lexical, std::string(message),
                                 SourceLocation{locStart.line, locStart.column, locEnd.line, locEnd.column});
            return false;
        }
        throw MiniSqlError(ErrorCode::Lexical, message, locStart);
    };
    while(i<s.size()) {
        const char c=s[i];
        if(c==' '||c=='\t'||c=='\r'||c=='\n'){advance();continue;}
        const SourceLocation loc{line,column};
        const auto start=i;
        const auto two=s.substr(i,2);
        // If true after recognizing a bad token, the token is dropped (errors
        // recorded) and scanning resumes at the next stable token instead of
        // aborting the whole batch. In strict mode report() throws first.
        bool dropped=false;
        const auto dropToStable=[&](){
            while(i<s.size()){
                const char d=s[i];
                // Space-separated token boundaries and statement/expression
                // delimiters are stable recovery points.
                if(d==' '||d=='\t'||d=='\r'||d=='\n'||d==';'||d==')'||d=='('||d==',')return;
                advance();
            }
        };
        const auto drop=[&](){dropped=true;dropToStable();};
        if(two=="--"){while(i<s.size()&&s[i]!='\n'&&s[i]!='\r')advance();continue;}
        if(two=="/*"){
            advance();advance();
            while(i<s.size()&&s.substr(i,2)!="*/")advance();
            if(i==s.size()){const SourceLocation end{line,column};report("Unterminated block comment",loc,end);drop();}
            advance();advance();continue;
        }
        std::string type;
        if(alpha(c)){
            while(i<s.size()&&(alpha(s[i])||digit(s[i])))advance();
            auto normalized=s.substr(start,i-start);
            for(char& ch:normalized)if(ch>='a'&&ch<='z')ch-=32;
            type=isKeyword(normalized)?"KEYWORD":"IDENTIFIER";
        } else if(digit(c)){
            while(i<s.size()&&digit(s[i]))advance();
            type="INTEGER";
            if(i<s.size()&&s[i]=='.'){
                advance();
                if(i==s.size()||!digit(s[i])){const SourceLocation end{line,column};report("Malformed DECIMAL literal",loc,end);drop();continue;}
                while(i<s.size()&&digit(s[i]))advance();
                type="DECIMAL";
            }
            if(i<s.size()&&(s[i]=='e'||s[i]=='E')){
                advance();
                if(i<s.size()&&(s[i]=='+'||s[i]=='-'))advance();
                if(i==s.size()||!digit(s[i])){const SourceLocation end{line,column};report("Malformed FLOAT literal",loc,end);drop();continue;}
                while(i<s.size()&&digit(s[i]))advance();
                type="FLOAT";
            }
            if(i<s.size()&&(s[i]=='.'||alpha(s[i]))){const SourceLocation end{line,column};report("Unsupported or malformed numeric literal",loc,end);drop();continue;}
        } else if(c=='\''){
            advance();bool closed=false;bool recovered=false;
            while(i<s.size()&&!recovered){
                if(s[i]=='\r'||s[i]=='\n'){
                    const SourceLocation end{line,column};
                    report("Newline in string literal is not supported",loc,end);
                    recovered=true;
                    if(!closed)drop(); // skip malformed string; keep scanning
                    break;
                }
                if(s[i]=='\''){
                    advance();
                    if(i<s.size()&&s[i]=='\''){advance();continue;}
                    closed=true;break;
                }
                advance();
            }
            if(!closed&&!recovered){const SourceLocation end{line,column};report("Unterminated string literal",loc,end);drop();}
            if(recovered){continue;} // already dropped+safely skipped past the newline
            type="STRING";
        } else if(two==">="||two=="<="||two=="!="||two=="=="||two=="<>"){advance();advance();type="OPERATOR";}
        else if(std::string("=<>+-*/").find(c)!=std::string::npos){advance();type="OPERATOR";}
        else if(c=='.' && i+1<s.size() && digit(s[i+1])){const SourceLocation end{line,column};report("Unsupported numeric literal",loc,end);drop();continue;}
        else if(std::string("(),;.").find(c)!=std::string::npos){advance();type="DELIMITER";}
        else{const SourceLocation end{line,column};report("Illegal character",loc,end);advance();drop();continue;}
        if(dropped)continue;
        const SourceLocation endLoc{line,column};
        consume({type,s.substr(start,i-start),loc,endLoc});
    }
    consume({"END","",{line,column},{line,column}});
}
} // namespace

void scanTokens(const std::string& s, const std::function<void(const Token&)>& consume) {
    scanImpl(s, consume, nullptr);
}
std::vector<Token> tokenize(const std::string& s) {
    std::vector<Token> out;
    scanTokens(s, [&](const Token& token) { out.push_back(token); });
    return out;
}
std::vector<Token> tokenizeRecoverable(const std::string& s, std::vector<MiniSqlError>& errors) {
    std::vector<Token> out;
    scanImpl(s, [&](const Token& token) { out.push_back(token); }, &errors);
    return out;
}
} // namespace minisql
