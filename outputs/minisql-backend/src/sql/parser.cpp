#include "minisql/sql/parser.hpp"
#include <algorithm>
#include <cctype>
#include <charconv>

namespace minisql::sql {
namespace {
class Parser {
public:
    explicit Parser(const std::vector<Token>& tokens): t(tokens) {}
    std::vector<Statement> all(){std::vector<Statement> out; while(i<t.size() && t[i].type!="END"){out.push_back(statement());} return out;}
private:
    const std::vector<Token>& t; std::size_t i=0;
    bool at(const std::string& s){return i<t.size() && t[i].lexeme==s;}
    bool keyword(const std::string& s){if(i>=t.size())return false; auto v=t[i].lexeme; std::transform(v.begin(),v.end(),v.begin(),[](unsigned char c){return static_cast<char>(std::toupper(c));}); return v==s;}
    const Token& take(){if(i>=t.size())throw MiniSqlError(ErrorCode::Syntax,"Unexpected end of input");return t[i++];}
    void expect(const std::string& s){if(!keyword(s)&&!at(s))throw MiniSqlError(ErrorCode::Syntax,"Expected '"+s+"'",i<t.size()?t[i].location:SourceLocation{});++i;}
    std::string identifier(){const auto& x=take();if(x.type!="IDENTIFIER")throw MiniSqlError(ErrorCode::Syntax,"Expected identifier",x.location);return x.lexeme;}
    std::string literal(){if(keyword("DATE")&&i+1<t.size()&&t[i+1].type=="STRING"){++i;return "DATE"+take().lexeme;}if(keyword("NULL")||keyword("TRUE")||keyword("FALSE"))return take().lexeme;std::string sign;if(at("-")||at("+"))sign=take().lexeme;const auto& x=take();if(x.type!="INTEGER"&&x.type!="DECIMAL"&&x.type!="FLOAT"&&(x.type!="STRING"||!sign.empty()))throw MiniSqlError(ErrorCode::Syntax,"Expected literal",x.location);return sign+x.lexeme;}
    std::string typeName() {
        if(keyword("INT")||keyword("BIGINT")||keyword("FLOAT")||keyword("BOOL")||keyword("DATE"))return take().lexeme;
        const bool varchar=keyword("VARCHAR");
        if(!varchar && !keyword("DECIMAL"))throw MiniSqlError(ErrorCode::Syntax,"Expected INT, BIGINT, FLOAT, VARCHAR(n), BOOL, DATE or DECIMAL(p,s) type",t[i].location);
        ++i;if(varchar && !at("("))return "varchar";expect("(");
        const auto parameter=[&](){
            const auto token=take();unsigned value{};
            if(token.type!="INTEGER")throw MiniSqlError(ErrorCode::Syntax,"Expected unsigned type parameter",token.location);
            const auto parsed=std::from_chars(token.lexeme.data(),token.lexeme.data()+token.lexeme.size(),value);
            if(parsed.ec!=std::errc{}||parsed.ptr!=token.lexeme.data()+token.lexeme.size())throw MiniSqlError(ErrorCode::Syntax,"Type parameter is too large",token.location);
            return value;
        };
        const auto precision=parameter();
        if(varchar){expect(")");return "varchar("+std::to_string(precision)+")";}
        expect(",");const auto scale=parameter();expect(")");
        return "decimal("+std::to_string(precision)+","+std::to_string(scale)+")";
    }
    void semicolon(){if(at(";"))++i;else throw MiniSqlError(ErrorCode::Syntax,"Expected ';'",i<t.size()?t[i].location:SourceLocation{});}
    Statement statement(){auto loc=t[i].location;Statement s;if(keyword("BEGIN")||keyword("COMMIT")||keyword("ROLLBACK"))s=transaction();else if(keyword("CREATE"))s=create();else if(keyword("INSERT"))s=insert();else if(keyword("SELECT"))s=select();else if(keyword("DELETE"))s=remove();else if(keyword("UPDATE"))s=update();else if(keyword("CHECKPOINT"))s=checkpointStatement();else if(keyword("DROP"))s=dropIndex();else throw MiniSqlError(ErrorCode::Syntax,"Expected SQL statement or transaction command",loc);s.location=loc;return s;}
    Statement dropIndex(){Statement s{"DropIndex"};expect("DROP");expect("INDEX");s.indexName=identifier();if(keyword("ON")){++i;s.table=identifier();}semicolon();return s;}
    Statement checkpointStatement(){Statement s{"Checkpoint"};expect("CHECKPOINT");semicolon();return s;}
    Statement transaction() {
        Statement s{keyword("BEGIN") ? "Begin" : keyword("COMMIT") ? "Commit" : "Rollback"};
        ++i;if(keyword("TRANSACTION"))++i;
        semicolon();return s;
    }
    Statement update() {
        Statement s{"Update"};expect("UPDATE");s.table=identifier();expect("SET");
        do {auto name=identifier();expect("=");s.assignments.push_back({std::move(name),writeValue()});}
        while(at(",")&&(++i,true));
        if(keyword("WHERE")){++i;s.where=expression();}
        semicolon();return s;
    }
    Statement create(){
        expect("CREATE");
        bool unique = false;
        if (keyword("UNIQUE")) { ++i; unique = true; }
        if (keyword("INDEX")) {
            ++i;
            Statement index{"CreateIndex"};
            index.uniqueIndex = unique;
            index.indexName = identifier();
            expect("ON");index.table = identifier();expect("(");
            do { index.indexColumns.push_back(identifier()); } while (at(",") && (++i, true));
            expect(")");semicolon();
            return index;
        }
        if (unique) throw MiniSqlError(ErrorCode::Syntax, "Expected INDEX after UNIQUE", t[i].location);
        expect("TABLE");
        Statement s{"CreateTable"};s.table=identifier();expect("(");
        std::vector<std::string> explicitNull;
        const auto constraintName = [&]() -> std::string { if(!keyword("CONSTRAINT"))return {}; ++i;return identifier(); };
        const auto recordName = [&](const std::string& label,const std::string& kind,std::size_t index) { if(!label.empty())s.constraintNames.push_back({label,kind,index}); };
        do{
            const auto label=constraintName();
            if(keyword("FOREIGN")){
                recordName(label,"foreignKey",s.foreignKeys.size());
                ++i;expect("KEY");ForeignKey constraint;
                expect("(");do{constraint.columns.push_back(identifier());}while(at(",")&&(++i,true));expect(")");
                expect("REFERENCES");constraint.table=identifier();
                expect("(");do{constraint.referencedColumns.push_back(identifier());}while(at(",")&&(++i,true));expect(")");
                s.foreignKeys.push_back(std::move(constraint));continue;
            }
            if(keyword("CHECK")){
                recordName(label,"check",s.checks.size());
                ++i;expect("(");s.checks.push_back(expression());expect(")");continue;
            }
            if(keyword("PRIMARY")||keyword("UNIQUE")){
                recordName(label,"key",s.keys.size());
                KeyConstraint constraint;
                constraint.primary=keyword("PRIMARY");++i;if(constraint.primary)expect("KEY");
                expect("(");do{constraint.columns.push_back(identifier());}while(at(",")&&(++i,true));expect(")");
                s.keys.push_back(std::move(constraint));continue;
            }
            if(!label.empty())throw MiniSqlError(ErrorCode::Syntax,"Expected table constraint",t[i].location);
            auto name=identifier();
            ColumnDef column{name,typeName()};bool seenNull=false;
            while(keyword("NOT")||keyword("NULL")||keyword("DEFAULT")||keyword("PRIMARY")||keyword("UNIQUE")||keyword("REFERENCES")||keyword("CHECK")||keyword("CONSTRAINT")){
                const auto columnLabel=constraintName();
                if(keyword("CHECK")){
                    recordName(columnLabel,"check",s.checks.size());
                    ++i;expect("(");s.checks.push_back(expression());expect(")");
                }else if(keyword("PRIMARY")){
                    recordName(columnLabel,"primaryKey",s.columns.size());
                    if(column.primaryKey)throw MiniSqlError(ErrorCode::Syntax,"Duplicate PRIMARY KEY",t[i].location);
                    ++i;expect("KEY");column.primaryKey=true;
                }else if(keyword("UNIQUE")){
                    recordName(columnLabel,"unique",s.columns.size());
                    if(column.unique)throw MiniSqlError(ErrorCode::Syntax,"Duplicate UNIQUE",t[i].location);
                    ++i;column.unique=true;
                }else if(keyword("REFERENCES")){
                    recordName(columnLabel,"references",s.columns.size());
                    ++i;auto referencedTable=identifier();expect("(");auto referencedColumn=identifier();expect(")");
                    if(column.references)throw MiniSqlError(ErrorCode::Syntax,"Duplicate REFERENCES",t[i].location);
                    column.references=std::make_pair(std::move(referencedTable),std::move(referencedColumn));
                }else if(keyword("DEFAULT")){
                    if(!columnLabel.empty())throw MiniSqlError(ErrorCode::Syntax,"Named DEFAULT is not supported",t[i].location);
                    if(column.defaultValue)throw MiniSqlError(ErrorCode::Syntax,"Duplicate DEFAULT",t[i].location);
                    ++i;column.defaultValue=literal();
                }else if(keyword("NOT")||keyword("NULL")){
                    if(!columnLabel.empty()&&!keyword("NOT"))throw MiniSqlError(ErrorCode::Syntax,"Named NULL is not a constraint",t[i].location);
                    recordName(columnLabel,"notNull",s.columns.size());
                    if(seenNull)throw MiniSqlError(ErrorCode::Syntax,"Duplicate nullability declaration",t[i].location);
                    seenNull=true;
                    if(keyword("NOT")){++i;expect("NULL");column.nullable=false;}else ++i;
                }else throw MiniSqlError(ErrorCode::Syntax,"Expected column constraint",t[i].location);
            }
            if(seenNull&&column.nullable)explicitNull.push_back(column.name);
            if(column.primaryKey){
                if(seenNull&&column.nullable)throw MiniSqlError(ErrorCode::Syntax,"PRIMARY KEY cannot declare NULL",t[i].location);
                column.nullable=false;
            }
            s.columns.push_back(std::move(column));
        }while(at(",")&&(++i,true));expect(")");semicolon();
        auto normalized=[](std::string name){for(auto& c:name)c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));return name;};
        for(const auto& constraint:s.keys)if(constraint.primary)for(const auto& name:constraint.columns){
            for(const auto& nullable:explicitNull)if(normalized(nullable)==normalized(name))throw MiniSqlError(ErrorCode::Syntax,"PRIMARY KEY cannot declare NULL");
            for(auto& column:s.columns)if(normalized(column.name)==normalized(name))column.nullable=false;
        }
        return s;
    }
    Statement insert(){
        Statement s{"Insert"};expect("INSERT");expect("INTO");s.table=identifier();
        if(keyword("DEFAULT")){++i;expect("VALUES");s.defaultValues=true;semicolon();return s;}
        if(at("(")){++i;do{s.names.push_back(identifier());}while(at(",")&&(++i,true));expect(")");}
        expect("VALUES");expect("(");
        do{
            s.valueExpressions.push_back(writeValue());
        }while(at(",")&&(++i,true));
        expect(")");
        if(at(",")) {
            s.valueRows.push_back(s.valueExpressions);
            while(at(",")) {
                ++i;expect("(");
                std::vector<std::shared_ptr<Expr>> row;
                do { row.push_back(writeValue()); } while(at(",")&&(++i,true));
                expect(")");s.valueRows.push_back(std::move(row));
            }
        }
        semicolon();
        if(std::all_of(s.valueExpressions.begin(),s.valueExpressions.end(),[](const auto& e){return e->kind=="Literal";}))
            for(const auto& e:s.valueExpressions)s.values.push_back(e->value);
        return s;
    }
    std::shared_ptr<Expr> writeValue(){
        if(keyword("DEFAULT")){const auto token=take();return std::make_shared<Expr>(Expr{"Default","DEFAULT",{},{},token.location});}
        return expression();
    }
    std::string tokenText(std::size_t begin, std::size_t end) {
        std::string text;
        for (std::size_t index = begin; index < end; ++index) {
            if (!text.empty()) text += ' ';
            text += t[index].lexeme;
        }
        return text;
    }
    Statement select(bool consumeSemicolon = true){
        Statement s{"Select"};expect("SELECT");
        if(keyword("DISTINCT")){++i;s.distinct=true;}
        do {
            std::shared_ptr<Expr> value;std::string alias;
            if(at("*")){const auto token=take();value=std::make_shared<Expr>(Expr{"Wildcard","*",{},{},token.location});}
            else if(i+2<t.size()&&t[i].type=="IDENTIFIER"&&t[i+1].lexeme=="."&&t[i+2].lexeme=="*"){
                const auto token=take();i+=2;value=std::make_shared<Expr>(Expr{"Wildcard",token.lexeme+".*",{},{},token.location});
            } else value=expression();
            if(keyword("AS")){
                if(value->kind=="Wildcard")throw MiniSqlError(ErrorCode::Syntax,"Wildcard cannot have an alias",t[i].location);
                ++i;alias=identifier();
            }
            if(value->kind=="Identifier"||value->kind=="Wildcard")s.selectList.push_back(value->value);
            s.selectItems.push_back({std::move(value),std::move(alias)});
        } while(at(",")&&(++i,true));
        expect("FROM");s.table=identifier();
        if(keyword("AS")){++i;s.tableAlias=identifier();}
        else if(i<t.size()&&t[i].type=="IDENTIFIER")s.tableAlias=identifier();
        while(keyword("JOIN")||keyword("INNER")||keyword("LEFT")||keyword("RIGHT")||keyword("FULL")) {
            if(s.joins.size()>=32)throw MiniSqlError(ErrorCode::Syntax,"Join count exceeds 32",t[i].location);
            Join join;
            if(keyword("LEFT")){++i;join.left=true;if(keyword("OUTER"))++i;}
            else if(keyword("RIGHT")){++i;join.right=true;if(keyword("OUTER"))++i;}
            else if(keyword("FULL")){++i;join.left=true;join.right=true;if(keyword("OUTER"))++i;}
            else if(keyword("INNER"))++i;
            expect("JOIN");join.table=identifier();
            if(keyword("AS")){++i;join.alias=identifier();}
            else if(i<t.size()&&t[i].type=="IDENTIFIER")join.alias=identifier();
            expect("ON");join.on=expression();s.joins.push_back(std::move(join));
        }
        if(keyword("WHERE")){++i;s.where=expression();}
        if(keyword("GROUP")){
            ++i;expect("BY");
            do {s.groupBy.push_back(expression());} while(at(",")&&(++i,true));
        }
        if(keyword("HAVING")){++i;s.having=expression();}
        if(keyword("ORDER")){
            ++i;expect("BY");
            do {auto value=expression();bool descending=false;
                if(keyword("ASC"))++i;else if(keyword("DESC")){++i;descending=true;}
                std::optional<bool> nullsFirst;
                if(keyword("NULLS")){++i;if(keyword("FIRST")){++i;nullsFirst=true;}else {expect("LAST");nullsFirst=false;}}
                s.orderBy.push_back({std::move(value),descending,nullsFirst});
            } while(at(",")&&(++i,true));
        }
        if(keyword("LIMIT")){++i;s.limit=unsignedCount();}
        if(keyword("OFFSET")){++i;s.offset=unsignedCount();}
        if (consumeSemicolon) semicolon();
        return s;
    }
    std::uint64_t unsignedCount(){
        const auto& token=take();std::uint64_t value=0;
        if(token.type!="INTEGER")throw MiniSqlError(ErrorCode::Syntax,"Expected non-negative integer count",token.location);
        const auto parsed=std::from_chars(token.lexeme.data(),token.lexeme.data()+token.lexeme.size(),value);
        if(parsed.ec!=std::errc{}||parsed.ptr!=token.lexeme.data()+token.lexeme.size())throw MiniSqlError(ErrorCode::Syntax,"Pagination count exceeds UINT64 range",token.location);
        return value;
    }
    Statement remove(){Statement s{"Delete"};expect("DELETE");expect("FROM");s.table=identifier();if(keyword("WHERE")){++i;s.where=expression();}semicolon();return s;}
    std::shared_ptr<Expr> expression(){auto left=conjunction();while(keyword("OR")){++i;left=std::make_shared<Expr>(Expr{"Binary","OR",left,conjunction()});}return left;}
    std::shared_ptr<Expr> conjunction(){auto left=negation();while(keyword("AND")){++i;left=std::make_shared<Expr>(Expr{"Binary","AND",left,negation()});}return left;}
    std::size_t depth=0;
    std::shared_ptr<Expr> negation(){if(keyword("NOT")){if(++depth>256)throw MiniSqlError(ErrorCode::Syntax,"Expression depth exceeded",t[i].location);++i;auto child=negation();--depth;return std::make_shared<Expr>(Expr{"Unary","NOT",child,{}});}return comparison();}
    std::shared_ptr<Expr> comparison(){
        auto left=addition();
        if(keyword("IN")||keyword("NOT")){
            const bool negate=keyword("NOT");const auto op=take();
            if(negate)expect("IN");
            if(++depth>256)throw MiniSqlError(ErrorCode::Syntax,"Expression depth exceeded",op.location);
            expect("(");
            if(keyword("SELECT")){
                const auto start=i;
                (void)select(false);
                const auto text=tokenText(start,i);
                expect(")");--depth;
                auto subquery=std::make_shared<Expr>(Expr{"InSubquery","",left,{},op.location,text});
                return negate?std::make_shared<Expr>(Expr{"Unary","NOT",subquery,{},op.location}):subquery;
            }
            if(at(")"))throw MiniSqlError(ErrorCode::Syntax,"IN requires a non-empty value list",t[i].location);
            std::vector<std::shared_ptr<Expr>> terms;
            do {
                if(terms.size()>=128)throw MiniSqlError(ErrorCode::Syntax,"IN list exceeds 128 values",op.location);
                terms.push_back(std::make_shared<Expr>(Expr{"Binary","=",left,addition(),op.location}));
            } while(at(",")&&(++i,true));
            expect(")");--depth;
            // 平衡 OR 树避免长列表产生线性递归深度。
            while(terms.size()>1){
                std::vector<std::shared_ptr<Expr>> next;
                for(std::size_t n=0;n<terms.size();n+=2)
                    next.push_back(n+1<terms.size()?std::make_shared<Expr>(Expr{"Binary","OR",terms[n],terms[n+1],op.location}):terms[n]);
                terms=std::move(next);
            }
            return negate?std::make_shared<Expr>(Expr{"Unary","NOT",terms.front(),{},op.location}):terms.front();
        }
        if(keyword("IS")){auto op=take();bool negate=keyword("NOT");if(negate)++i;expect("NULL");return std::make_shared<Expr>(Expr{"Unary",negate?"IS NOT NULL":"IS NULL",left,{},op.location});}
        if(at("=")||at("!=")||at("<")||at("<=")||at(">")||at(">=")){auto op=take();auto right=addition();return std::make_shared<Expr>(Expr{"Binary",op.lexeme,left,right,op.location});}
        return left;
    }
    std::shared_ptr<Expr> addition(){auto left=multiplication();while(at("+")||at("-")){auto op=take();left=std::make_shared<Expr>(Expr{"Binary",op.lexeme,left,multiplication(),op.location});}return left;}
    std::shared_ptr<Expr> multiplication(){auto left=unary();while(at("*")||at("/")){auto op=take();left=std::make_shared<Expr>(Expr{"Binary",op.lexeme,left,unary(),op.location});}return left;}
    std::shared_ptr<Expr> unary(){
        if(keyword("EXISTS")){
            const auto token=take();expect("(");
            if(!keyword("SELECT"))throw MiniSqlError(ErrorCode::Syntax,"EXISTS requires a SELECT subquery",t[i].location);
            const auto start=i;(void)select(false);const auto text=tokenText(start,i);expect(")");
            return std::make_shared<Expr>(Expr{"Exists","",nullptr,{},token.location,text});
        }
        if(i+1<t.size()&&t[i+1].lexeme=="("&&
           (keyword("COUNT")||keyword("SUM")||keyword("AVG")||keyword("MIN")||keyword("MAX"))){
            const auto token=take();auto name=token.lexeme;
            std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return static_cast<char>(std::toupper(c));});
            if(++depth>256)throw MiniSqlError(ErrorCode::Syntax,"Expression depth exceeded",token.location);
            expect("(");std::shared_ptr<Expr> argument;
            if(at("*")){
                const auto star=take();
                if(name!="COUNT")throw MiniSqlError(ErrorCode::Syntax,"Only COUNT accepts '*'",star.location);
                argument=std::make_shared<Expr>(Expr{"Wildcard","*",{},{},star.location});
            }else argument=expression();
            expect(")");--depth;
            return std::make_shared<Expr>(Expr{"AggregateExpr",name,argument,{},token.location});
        }
        if(keyword("CAST")){
            const auto token=take();
            if(++depth>256)throw MiniSqlError(ErrorCode::Syntax,"Expression depth exceeded",token.location);
            expect("(");auto child=expression();expect("AS");
            auto target=typeName();expect(")");--depth;
            return std::make_shared<Expr>(Expr{"Cast",target,child,{},token.location});
        }
        if(keyword("DATE")&&i+1<t.size()&&t[i+1].type=="STRING"){const auto loc=t[i].location;return std::make_shared<Expr>(Expr{"Literal",literal(),{},{},loc});}
        if(keyword("NULL")||keyword("TRUE")||keyword("FALSE")){const auto token=take();return std::make_shared<Expr>(Expr{"Literal",token.lexeme,{},{},token.location});}
        if(at("+")||at("-")){
            if(i+1<t.size()&&(t[i+1].type=="INTEGER"||t[i+1].type=="DECIMAL"||t[i+1].type=="FLOAT")){auto loc=t[i].location;return std::make_shared<Expr>(Expr{"Literal",literal(),{},{},loc});}
            auto op=take();
            if(++depth>256)throw MiniSqlError(ErrorCode::Syntax,"Expression depth exceeded",op.location);
            auto child=unary();--depth;
            return std::make_shared<Expr>(Expr{"Unary",op.lexeme,child,{},op.location});
        }
        return primary();
    }
    std::shared_ptr<Expr> primary(){
        if(at("(") && i+1<t.size()){
            auto next=t[i+1].lexeme;for(char& c:next)if(c>='a'&&c<='z')c-=32;
            if(next=="SELECT"){
                const auto token=take();const auto start=i;(void)select(false);const auto text=tokenText(start,i);expect(")");
                return std::make_shared<Expr>(Expr{"ScalarSubquery","",nullptr,{},token.location,text});
            }
        }
        if(at("(")){if(++depth>256)throw MiniSqlError(ErrorCode::Syntax,"Expression depth exceeded",t[i].location);++i;auto e=expression();expect(")");--depth;return e;}auto loc=t[i].location;if(at("-")||at("+"))return std::make_shared<Expr>(Expr{"Literal",literal(),{},{},loc});const auto& x=take();if(x.type=="IDENTIFIER"){auto name=x.lexeme;if(at(".")){++i;name+="."+identifier();}return std::make_shared<Expr>(Expr{"Identifier",name,{},{},x.location});}if(x.type=="INTEGER"||x.type=="DECIMAL"||x.type=="FLOAT"||x.type=="STRING")return std::make_shared<Expr>(Expr{"Literal",x.lexeme,{},{},x.location});throw MiniSqlError(ErrorCode::Syntax,"Expected identifier, literal or '('",x.location);}
};
}
std::vector<Statement> parse(const std::vector<Token>& tokens){return Parser(tokens).all();}
}
