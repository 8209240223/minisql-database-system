#include "minisql/catalog/persistent_catalog.hpp"
#include "minisql/sql/serialization.hpp"
#include <chrono>
#include <bit>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace minisql;
using namespace minisql::storage;
int checks = 0;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
    ++checks;
}
template<class F> void rejects(F action, const char* message) {
    bool rejected = false;
    try { action(); } catch (const MiniSqlError&) { rejected = true; }
    require(rejected, message);
}
int main() {
    const RowSchema floatSchema{ColumnType::Float};
    for (const double value : {0.0, -0.0, 1.25, -123.5, std::numeric_limits<double>::max(),
            std::numeric_limits<double>::lowest(), std::numeric_limits<double>::min(), std::numeric_limits<double>::denorm_min()}) {
        const auto bytes = encodeRow({value}, floatSchema);
        const auto decoded = std::get<double>(decodeRow(bytes, floatSchema).front());
        require(std::bit_cast<std::uint64_t>(decoded) == std::bit_cast<std::uint64_t>(value), "FLOAT bit-exact codec round trip");
    }
    require(std::holds_alternative<std::monostate>(decodeRow(encodeRow({std::monostate{}}, floatSchema), floatSchema)[0]), "FLOAT NULL round trip");
    for (const auto invalid : {std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
        rejects([&] { encodeRow({invalid}, floatSchema); }, "FLOAT non-finite encoding rejected");
    auto damagedFloat = encodeRow({0.0}, floatSchema);
    damagedFloat[damagedFloat.size()-1] = 0x7f;
    damagedFloat[damagedFloat.size()-2] = 0xf0;
    rejects([&] { decodeRow(damagedFloat, floatSchema); }, "FLOAT non-finite disk payload rejected");
    damagedFloat.pop_back();
    rejects([&] { decodeRow(damagedFloat, floatSchema); }, "FLOAT truncated payload rejected");
    rejects([&] { encodeRow({std::int32_t(1)}, floatSchema); }, "FLOAT does not silently accept integer storage values");
    const auto named = sql::parse(sql::tokenize("CREATE TABLE named(id INT CONSTRAINT pk PRIMARY KEY,score INT CONSTRAINT positive CHECK(score>=0));"))[0];
    {
        catalog::Catalog view;view.create(named);
        require(view.find("named")->constraintNames.size() == 2, "named constraint registry retained");
    }
    for (const auto& binding : std::vector<sql::ConstraintName>{{"bad", "check", 99}, {"bad", "unknown", 0},
             {"bad", "primaryKey", 1}, {"bad", "unique", 1}, {"bad", "notNull", 1}, {"bad", "references", 1},
             {"bad", "foreignKey", 0}, {"bad", "key", 0}, {"", "check", 0}, {"bad name", "check", 0}}) {
        auto damaged = named;damaged.constraintNames = {binding};
        rejects([&] { catalog::Catalog view;view.create(damaged); }, "invalid named constraint target rejected");
    }
    {
        auto damaged = named;damaged.constraintNames.push_back({"PK", "check", 0});
        rejects([&] { catalog::Catalog view;view.create(damaged); }, "constraint names case-insensitively unique");
        damaged = named;damaged.constraintNames.push_back({"another", "primaryKey", 0});
        rejects([&] { catalog::Catalog view;view.create(damaged); }, "constraint cannot have two names");
    }
    const nlohmann::json literalNode{{"kind", "Literal"}, {"value", "'O''Brien'"}, {"line", 1}, {"column", 2}};
    require(sql::serializeExpression(sql::deserializeExpression(literalNode)) == literalNode, "CHECK string AST round trip");
    auto malformed = std::vector<nlohmann::json>{nullptr, 12, nlohmann::json::array()};
    for (const auto* field : {"kind", "value", "line", "column"}) {
        auto node = literalNode; node.erase(field); malformed.push_back(node);
    }
    for (const auto& value : {"'unterminated", "1 2", "1;DELETE", "1/*comment*/", "name", "--1"}) {
        auto node = literalNode; node["value"] = value; malformed.push_back(node);
    }
    for (const auto& value : {nlohmann::json(-1), nlohmann::json(1.5), nlohmann::json("1"), nlohmann::json(nullptr)}) {
        auto node = literalNode; node["line"] = value; malformed.push_back(node);
    }
    auto node = literalNode; node["value"] = 1; malformed.push_back(node);
    node = literalNode; node["kind"] = "Unknown"; malformed.push_back(node);
    node = literalNode; node["columnId"] = 0; malformed.push_back(node);
    node = literalNode; node["left"] = literalNode; malformed.push_back(node);
    node = literalNode; node["kind"] = "Binary"; node["value"] = "="; node["left"] = literalNode; malformed.push_back(node);
    node["right"] = nullptr; malformed.push_back(node);
    node["right"] = literalNode; node["value"] = "INVALID"; malformed.push_back(node);
    node["value"] = "and"; malformed.push_back(node);
    node = literalNode; node["kind"] = "Cast"; node["value"] = "TIME"; node["left"] = literalNode; malformed.push_back(node);
    node = literalNode; node["kind"] = "Identifier"; node["value"] = "a.b.c"; malformed.push_back(node);
    for (const auto& damaged : malformed) {
        bool storageError = false;
        try { sql::deserializeExpression(damaged); }
        catch (const MiniSqlError& error) { storageError = error.code() == ErrorCode::Storage; }
        require(storageError, "malformed CHECK AST returns Storage error");
    }
    node = literalNode;
    for (int depth = 0; depth < 257; ++depth)
        node = {{"kind", "Unary"}, {"value", "NOT"}, {"line", 0}, {"column", 0}, {"left", std::move(node)}};
    rejects([&] { sql::deserializeExpression(node); }, "CHECK AST depth bounded");
    const RowSchema schema{ColumnType::Int, ColumnType::Varchar};
    const Row edge{std::numeric_limits<std::int32_t>::min(), std::string("a\0b", 3)};
    require(decodeRow(encodeRow(edge, schema), schema) == edge, "binary row round trip");
    require(encodeRow(edge, schema)[0] == 1, "non-null rows retain v1 format");
    const RowSchema boundedSchema{ColumnSchema::varchar(2)};
    for(const auto& text : {std::string(""),std::string("ab"),std::string("\xe4\xb8\xad\xe6\x96\x87"),std::string("\xf0\x9f\x98\x80")+"a",std::string("e\xcc\x81"),std::string("\0a",2)}) {
        const auto bytes=encodeRow({text},boundedSchema);
        require(bytes[0]==4 && bytes[9]==6 && bytes[13]==2 && bytes.size()==21+text.size(),"VARCHAR(n) v4 type and length descriptor");
        require(decodeRow(bytes,boundedSchema)==Row{text},"bounded UTF-8 strings round trip by code points");
        for(std::size_t size=0;size<bytes.size();++size)
            rejects([&] { decodeRow(std::span(bytes).first(size),boundedSchema); },"v4 truncation rejected");
    }
    require(utf8Length("\xf0\x9f\x98\x80")==1 && utf8Length("e\xcc\x81")==2,"code points not UTF-16 units or grapheme clusters");
    for(const auto& text : {std::string("abc"),std::string("\xf0\x9f\x98\x80")+"ab",std::string("\xc0\xaf"),std::string("\xed\xa0\x80"),std::string("\xf4\x90\x80\x80"),std::string("\xe4\xb8")})
        rejects([&] { encodeRow({text},boundedSchema); },"overlength or invalid UTF-8 bounded value rejected");
    require(decodeRow(encodeRow({std::monostate{}},boundedSchema),boundedSchema)==Row{std::monostate{}},"bounded NULL round trip");
    rejects([&] { encodeRow({std::monostate{}},{ColumnSchema::varchar(0)}); },"NULL cannot bypass zero length schema");
    const auto boundBytes=encodeRow({std::string("ab")},boundedSchema);
    rejects([&] { decodeRow(boundBytes,{ColumnSchema::varchar(3)}); },"declared string bound mismatch rejected");
    auto boundDamage=boundBytes;boundDamage[0]=3;
    rejects([&] { decodeRow(boundDamage,boundedSchema); },"v4 cannot pretend to be v3");
    boundDamage=boundBytes;boundDamage[13]=1;
    rejects([&] { decodeRow(boundDamage,{ColumnSchema::varchar(1)}); },"overlength persisted payload rejected even with matching smaller descriptor");
    boundDamage=boundBytes;boundDamage[21]=0xff;
    rejects([&] { decodeRow(boundDamage,boundedSchema); },"invalid persisted UTF-8 rejected");
    const RowSchema mixedBound{ColumnSchema::varchar(3),ColumnType::Date,ColumnType::Bool,ColumnSchema{4,2},ColumnType::Varchar};
    const Row mixedBoundRow{std::string("abc"),std::string("2024-02-29"),true,std::string("1.20"),std::string("x")};
    require(decodeRow(encodeRow(mixedBoundRow,mixedBound),mixedBound)==mixedBoundRow,"all prior types retain encoding within v4");
    const RowSchema maxBound{ColumnSchema::varchar(UINT32_MAX)};
    const auto maximumBound=encodeRow({std::string("x")},maxBound);
    for(std::size_t i=13;i<17;++i) require(maximumBound[i]==255,"full 32-bit VARCHAR bound encoded without narrowing");
    require(decodeRow(maximumBound,maxBound)==Row{std::string("x")},"maximum declared VARCHAR bound round trip");
    const RowSchema dateSchema{ColumnType::Date};
    for (const auto* date : {"0001-01-01","9999-12-31","1969-12-31","1970-01-01","1970-01-02","1900-02-28","2000-02-29"}) {
        const auto bytes=encodeRow({std::string(date)},dateSchema);
        require(bytes.size()==17 && bytes[0]==3 && bytes[9]==5,"DATE has stable type descriptor and four byte day count");
        require(decodeRow(bytes,dateSchema)==Row{std::string(date)},"DATE boundary round trip");
        for (std::size_t size=0;size<bytes.size();++size)
            rejects([&] { decodeRow(std::span(bytes).first(size),dateSchema); },"DATE truncation rejected");
    }
    require(parseIsoDate("1970-01-01")==0 && parseIsoDate("1969-12-31")==-1 && parseIsoDate("1970-01-02")==1,"DATE epoch and signed days");
    const auto startDay=parseIsoDate("1600-01-01"),endDay=parseIsoDate("2000-01-01");
    bool fullCycle=true;
    for(auto day=startDay;day<endDay;++day) fullCycle=fullCycle && parseIsoDate(formatIsoDate(day))==day;
    require(fullCycle && endDay-startDay==146097,"DATE complete 400 year Gregorian cycle round trip");
    for(const auto* date : {"0000-01-01","10000-01-01","1900-02-29","2023-02-29","2024-04-31","2024-00-01","2024-13-01","2024-01-00","2024-01-32","2024-1-01","2024/01/01","2024-01-01 "})
        rejects([&] { encodeRow({std::string(date)},dateSchema); },"invalid calendar row rejected");
    require(decodeRow(encodeRow({std::monostate{}},dateSchema),dateSchema)==Row{std::monostate{}},"DATE NULL round trip");
    auto invalidDate=encodeRow({std::string("1970-01-01")},dateSchema);
    for(std::size_t i=13;i<17;++i) require(invalidDate[i]==0,"epoch encoded as zero little endian days");
    invalidDate[16]=127;
    rejects([&] { decodeRow(invalidDate,dateSchema); },"out of range stored day rejected before calendar conversion");
    invalidDate=encodeRow({std::string("1970-01-01")},dateSchema);invalidDate[10]=1;
    rejects([&] { decodeRow(invalidDate,dateSchema); },"DATE precision descriptor rejected");
    rejects([&] { encodeRow({std::int32_t(0)},dateSchema); },"DATE does not implicitly accept numeric epoch");
    rejects([&] { decodeRow(encodeRow({std::string("1970-01-01")},dateSchema),{ColumnType::Varchar}); },"DATE cannot be decoded as VARCHAR");
    const RowSchema boolSchema{ColumnType::Bool};
    for (const bool value : {false,true}) {
        const auto bytes = encodeRow({value},boolSchema);
        require(bytes.size() == 14 && bytes[0] == 3 && bytes[9] == 4 && bytes[13] == value, "BOOL has stable v3 descriptor and one byte payload");
        require(decodeRow(bytes,boolSchema) == Row{value}, "BOOL round trip retains variant type");
        for (std::size_t size=0;size<bytes.size();++size)
            rejects([&] { decodeRow(std::span(bytes).first(size),boolSchema); }, "truncated BOOL row rejected");
        for (unsigned invalid=2;invalid<256;++invalid) {
            auto damaged=bytes;damaged.back()=static_cast<std::uint8_t>(invalid);
            rejects([&] { decodeRow(damaged,boolSchema); }, "noncanonical BOOL byte rejected");
        }
    }
    require(decodeRow(encodeRow({std::monostate{}},boolSchema),boolSchema) == Row{std::monostate{}}, "BOOL NULL round trip");
    rejects([&] { encodeRow({std::int32_t(1)},boolSchema); }, "BOOL does not store integer one");
    rejects([&] { encodeRow({std::string("true")},boolSchema); }, "BOOL does not store string truth");
    rejects([&] { encodeRow({true},{ColumnType::Int}); }, "INT does not store BOOL");
    auto invalidBool=encodeRow({true},boolSchema);invalidBool[10]=1;
    rejects([&] { decodeRow(invalidBool,boolSchema); }, "BOOL has no precision parameter");
    const RowSchema mixedBool{ColumnType::Bool,ColumnSchema{5,2},ColumnType::Int,ColumnType::Varchar};
    const Row mixedBoolRow{false,std::string("1.20"),std::int32_t(1),std::string("x")};
    require(decodeRow(encodeRow(mixedBoolRow,mixedBool),mixedBool)==mixedBoolRow,"BOOL mixed with DECIMAL and legacy fields");
    const RowSchema decimalSchema{ColumnSchema{38,6}};
    for (const std::string value : {"0.000000","-1.000000","1.234567","99999999999999999999999999999999.999999","-99999999999999999999999999999999.999999"}) {
        const auto bytes = encodeRow({value}, decimalSchema);
        require(bytes.size() == 29 && bytes[0] == 3, "DECIMAL row uses v3 descriptors and 16-byte coefficient");
        require(decodeRow(bytes, decimalSchema) == Row{value}, "DECIMAL boundary round trip");
        for (std::size_t length = 0; length < bytes.size(); ++length) {
            const std::span<const std::uint8_t> truncated(bytes.data(), length);
            rejects([&] { decodeRow(truncated, decimalSchema); }, "truncated DECIMAL row rejected");
        }
    }
    const auto coefficient = encodeRow({std::string("0.000001")}, decimalSchema);
    require(coefficient[9] == 3 && coefficient[10] == 38 && coefficient[11] == 6 && coefficient[12] == 0 && coefficient[13] == 1, "stable DECIMAL type and little endian coefficient");
    for (std::size_t i = 14; i < coefficient.size(); ++i) require(coefficient[i] == 0, "DECIMAL coefficient high bytes zero");
    const auto negative = encodeRow({std::string("-0.000001")}, decimalSchema);
    for (std::size_t i = 13; i < negative.size(); ++i) require(negative[i] == 255, "DECIMAL negative coefficient is two's complement");
    require(decodeRow(encodeRow({std::monostate{}},decimalSchema),decimalSchema) == Row{std::monostate{}}, "DECIMAL NULL round trip");
    rejects([&] { decodeRow(coefficient,{ColumnSchema{38,5}}); }, "DECIMAL scale mismatch rejected");
    rejects([&] { decodeRow(coefficient,{ColumnSchema{37,6}}); }, "DECIMAL precision mismatch rejected");
    rejects([&] { decodeRow(coefficient,{ColumnType::Varchar}); }, "DECIMAL cannot be read as VARCHAR");
    rejects([&] { encodeRow({std::string("1.001")},{ColumnSchema{3,2}}); }, "codec cannot silently reduce decimal scale");
    rejects([&] { encodeRow({std::string("10.00")},{ColumnSchema{3,2}}); }, "codec rejects precision overflow");
    rejects([&] { encodeRow({std::monostate{}},{ColumnSchema{0,0}}); }, "NULL cannot bypass decimal schema validation");
    auto damagedDecimal = coefficient;damagedDecimal[12] = 1;
    rejects([&] { decodeRow(damagedDecimal,decimalSchema); }, "descriptor reserved bits validated");
    damagedDecimal = coefficient;damagedDecimal[8] = 128;
    rejects([&] { decodeRow(damagedDecimal,decimalSchema); }, "DECIMAL NULL padding validated");
    damagedDecimal = coefficient;damagedDecimal[28] = 127;
    rejects([&] { decodeRow(damagedDecimal,decimalSchema); }, "stored DECIMAL coefficient overflow rejected");
    const RowSchema mixedDecimal{ColumnType::Int,ColumnSchema{4,2},ColumnType::Varchar};
    const Row mixedRow{std::int32_t(7),std::string("1.20"),std::string("text")};
    require(decodeRow(encodeRow(mixedRow,mixedDecimal),mixedDecimal) == mixedRow, "mixed legacy and decimal field round trip");
    const RowSchema bigSchema{ColumnType::Bigint};
    for (const auto value : {std::numeric_limits<std::int64_t>::min(), std::int64_t(-1), std::int64_t(0), std::int64_t(9007199254740993LL), std::numeric_limits<std::int64_t>::max()}) {
        const auto encoded = encodeRow({value}, bigSchema);
        require(encoded.size() == 16, "BIGINT payload is eight bytes");
        require(std::get<std::int64_t>(decodeRow(encoded, bigSchema)[0]) == value, "BIGINT boundary round trip");
        for (std::size_t size = 8; size < 16; ++size) {
            auto truncated = encoded;truncated.resize(size);
            rejects([&] { decodeRow(truncated, bigSchema); }, "truncated BIGINT rejected");
        }
    }
    const auto endian = encodeRow({std::int64_t(0x0102030405060708LL)}, bigSchema);
    require(std::vector<std::uint8_t>(endian.begin() + 8, endian.end()) == std::vector<std::uint8_t>({8,7,6,5,4,3,2,1}), "BIGINT little endian format");
    require(decodeRow(encodeRow({std::monostate{}}, bigSchema), bigSchema) == Row{std::monostate{}}, "nullable BIGINT encoding");
    rejects([&] { encodeRow({std::int64_t(1)}, {ColumnType::Int}); }, "no implicit narrowing in row codec");
    rejects([&] { encodeRow({std::int32_t(1)}, bigSchema); }, "promotion must precede row encoding");
    RowSchema wideSchema(10, ColumnType::Int);
    Row nullable(10, std::int32_t(7));nullable[0] = std::monostate{};nullable[9] = std::monostate{};
    auto nullableBytes = encodeRow(nullable, wideSchema);
    require(nullableBytes[0] == 2 && nullableBytes[8] == 1 && nullableBytes[9] == 2, "v2 multi-byte NULL bitmap");
    require(decodeRow(nullableBytes, wideSchema) == nullable, "NULL bitmap round trip");
    nullableBytes[9] |= 128;
    rejects([&] { decodeRow(nullableBytes, wideSchema); }, "invalid bitmap padding rejected");
    nullableBytes = encodeRow(nullable, wideSchema);nullableBytes.resize(9);
    rejects([&] { decodeRow(nullableBytes, wideSchema); }, "truncated bitmap rejected");
    nullableBytes = encodeRow(nullable, wideSchema);nullableBytes[0] = 3;
    rejects([&] { decodeRow(nullableBytes, wideSchema); }, "unknown row version rejected");
    rejects([&] { encodeRow({std::string("wrong"), std::int32_t(1)}, schema); }, "wrong types");
    rejects([&] { encodeRow({std::int32_t(1)}, schema); }, "wrong count");
    rejects([&] { encodeRow({std::int32_t(1), std::string(4100, 'x')}, schema); }, "oversized row");
    rejects([&] { encodeRow({std::int32_t(1), std::string("\xc0\x80", 2)}, schema); }, "invalid UTF8");
    auto bytes = encodeRow(edge, schema);
    bytes.pop_back();
    rejects([&] { decodeRow(bytes, schema); }, "truncated row");
    bytes = encodeRow(edge, schema);
    bytes.push_back(0);
    rejects([&] { decodeRow(bytes, schema); }, "trailing bytes");
    const auto directory = std::filesystem::path("tests/artifacts") /
        ("heap-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    const auto bigPath = directory / "bigint.pages";
    std::vector<RowRef> bigRefs;
    {
        auto file = std::make_shared<PageFile>(bigPath);BufferPool buffer(file, 2, ReplacementPolicy::LRU);HeapStore heap(file, buffer);
        for (std::int64_t i = 0; i < 1000; ++i) bigRefs.push_back(heap.insert(2, bigSchema, {std::numeric_limits<std::int64_t>::max() - i}));
        require(file->pagesFor(2).size() > 2, "BIGINT rows exceed buffer capacity");
        heap.flush();
    }
    {
        auto file = std::make_shared<PageFile>(bigPath);BufferPool buffer(file, 2, ReplacementPolicy::FIFO);HeapStore heap(file, buffer);
        for (std::size_t i = 0; i < bigRefs.size(); ++i)
            require(std::get<std::int64_t>(heap.read(2, bigSchema, bigRefs[i])[0]) == std::numeric_limits<std::int64_t>::max() - static_cast<std::int64_t>(i), "BIGINT value retained after reopen");
        bigRefs[0] = heap.replace(2, bigSchema, bigRefs[0], {std::monostate{}});
        bigRefs[1] = heap.replace(2, bigSchema, bigRefs[1], {std::numeric_limits<std::int64_t>::min()});heap.flush();
    }
    {
        auto file = std::make_shared<PageFile>(bigPath);BufferPool buffer(file, 2, ReplacementPolicy::LRU);HeapStore heap(file, buffer);
        require(std::holds_alternative<std::monostate>(heap.read(2, bigSchema, bigRefs[0])[0]), "BIGINT NULL replacement persists");
        require(std::get<std::int64_t>(heap.read(2, bigSchema, bigRefs[1])[0]) == std::numeric_limits<std::int64_t>::min(), "BIGINT minimum replacement persists");
    }
    for (std::size_t index = 0; index < malformed.size(); ++index) {
        const auto corruptPath = directory / ("invalid-check-" + std::to_string(index) + ".pages");
        {
            auto file = std::make_shared<PageFile>(corruptPath);BufferPool buffer(file, 2, ReplacementPolicy::LRU);HeapStore heap(file, buffer);
            heap.insert(1, {ColumnType::Int, ColumnType::Int, ColumnType::Varchar, ColumnType::Varchar},
                {std::int32_t(2), std::int32_t(0), std::string("id"), std::string("int")});
            const auto descriptor = nlohmann::json{{"version", 2}, {"name", "damaged"}, {"keys", nlohmann::json::array()},
                {"checks", nlohmann::json::array({malformed[index]})}}.dump();
            heap.insert(0, {ColumnType::Int, ColumnType::Varchar, ColumnType::Int}, {std::int32_t(2), descriptor, std::int32_t(1)});
            heap.flush();
        }
        auto file = std::make_shared<PageFile>(corruptPath);BufferPool buffer(file, 2, ReplacementPolicy::LRU);HeapStore heap(file, buffer);
        bool storageError = false;
        try { catalog::PersistentCatalog damaged(heap); }
        catch (const MiniSqlError& error) { storageError = error.code() == ErrorCode::Storage; }
        require(storageError, "reopened catalog rejects malformed CHECK with valid page checksums");
    }
    const auto typeIdPath = directory / "type-id.pages";
    {
        auto file = std::make_shared<PageFile>(typeIdPath);BufferPool buffer(file, 2, ReplacementPolicy::LRU);HeapStore heap(file, buffer);
        catalog::PersistentCatalog catalog(heap);
        catalog.create(sql::parse(sql::tokenize("CREATE TABLE typed(i INT,v VARCHAR(12),d DECIMAL(8,2));"))[0]);
        std::size_t seen = 0;
        heap.scan(1, {ColumnType::Int, ColumnType::Int, ColumnType::Varchar, ColumnType::Varchar}, [&](RowRef, const Row& row) {
            if (std::get<std::int32_t>(row[0]) != 2) return;
            const auto ordinal = std::get<std::int32_t>(row[1]);
            const auto descriptor = nlohmann::json::parse(std::get<std::string>(row[3]));
            require(descriptor.at("version") == 5, "new catalog column descriptor version");
            require(descriptor.contains("typeId") && descriptor.at("typeId").is_number_unsigned(), "catalog type identity is numeric");
            require(!descriptor.contains("type"), "new catalog descriptor omits legacy type string");
            if (ordinal == 0) require(descriptor.at("typeId") == 1 && descriptor.at("typeParameters").is_null(), "INT stable type id");
            if (ordinal == 1) require(descriptor.at("typeId") == 6 && descriptor.at("typeParameters").at("length") == 12, "VARCHAR stable type id and length");
            if (ordinal == 2) require(descriptor.at("typeId") == 7 && descriptor.at("typeParameters") == nlohmann::json{{"precision", 8}, {"scale", 2}}, "DECIMAL stable type id and parameters");
            ++seen;
        });
        require(seen == 3, "all type-id descriptors inspected");
    }
    const auto unknownTypeIdPath = directory / "unknown-type-id.pages";
    {
        auto file = std::make_shared<PageFile>(unknownTypeIdPath);BufferPool buffer(file, 2, ReplacementPolicy::LRU);HeapStore heap(file, buffer);
        const auto column = nlohmann::json{{"version", 5}, {"typeId", 999}, {"typeParameters", nullptr}, {"nullable", true},
            {"defaultValue", nullptr}, {"primaryKey", false}, {"unique", false}, {"references", nullptr}}.dump();
        heap.insert(1, {ColumnType::Int, ColumnType::Int, ColumnType::Varchar, ColumnType::Varchar},
            {std::int32_t(2), std::int32_t(0), std::string("id"), column});
        const auto table = nlohmann::json{{"version", 5}, {"name", "unknown_type"}, {"keys", nlohmann::json::array()},
            {"checks", nlohmann::json::array()}, {"foreignKeys", nlohmann::json::array()}, {"constraintNames", nlohmann::json::array()},
            {"indexes", nlohmann::json::array()}}.dump();
        heap.insert(0, {ColumnType::Int, ColumnType::Varchar, ColumnType::Int}, {std::int32_t(2), table, std::int32_t(1)});
        heap.flush();
    }
    {
        auto file = std::make_shared<PageFile>(unknownTypeIdPath);BufferPool buffer(file, 2, ReplacementPolicy::LRU);HeapStore heap(file, buffer);
        bool storageError = false;
        try { catalog::PersistentCatalog damaged(heap); }
        catch (const MiniSqlError& error) { storageError = error.code() == ErrorCode::Storage; }
        require(storageError, "unknown persisted type id rejected as corruption");
    }
    const auto legacyPath = directory / "legacy.pages";
    {
        auto file = std::make_shared<PageFile>(legacyPath);BufferPool buffer(file, 2, ReplacementPolicy::LRU);HeapStore heap(file, buffer);
        heap.insert(1, {ColumnType::Int, ColumnType::Int, ColumnType::Varchar, ColumnType::Varchar}, {std::int32_t(2), std::int32_t(0), std::string("id"), std::string("int")});
        heap.insert(0, {ColumnType::Int, ColumnType::Varchar, ColumnType::Int}, {std::int32_t(2), std::string("legacy"), std::int32_t(1)});
        heap.insert(2, {ColumnType::Int}, {std::int32_t(42)});heap.flush();
        heap.insert(1, {ColumnType::Int, ColumnType::Int, ColumnType::Varchar, ColumnType::Varchar},
            {std::int32_t(3), std::int32_t(0), std::string("id"), std::string(R"({"version":1,"type":"int","nullable":false})")});
        heap.insert(0, {ColumnType::Int, ColumnType::Varchar, ColumnType::Int}, {std::int32_t(3), std::string("version_one"), std::int32_t(1)});heap.flush();
    }
    {
        auto file = std::make_shared<PageFile>(legacyPath);BufferPool buffer(file, 2, ReplacementPolicy::LRU);HeapStore heap(file, buffer);
        catalog::PersistentCatalog catalog(heap);
        require(catalog.view().find("legacy")->columns[0].nullable, "legacy implicit column accepts nullable extension");
        require(!catalog.view().find("version_one")->columns[0].nullable, "version one descriptor preserves NOT NULL");
        require(!catalog.view().find("version_one")->columns[0].defaultValue, "version one descriptor has no default");
        heap.scan(2, {ColumnType::Int}, [&](RowRef, const Row& row) { require(std::get<std::int32_t>(row[0]) == 42, "legacy v1 data read"); });
        catalog.create(sql::parse(sql::tokenize("CREATE TABLE strict_table(id INT NOT NULL);"))[0]);
    }
    {
        auto file = std::make_shared<PageFile>(legacyPath);BufferPool buffer(file, 2, ReplacementPolicy::LRU);HeapStore heap(file, buffer);
        catalog::PersistentCatalog catalog(heap);
        require(!catalog.view().find("strict_table")->columns[0].nullable, "NOT NULL catalog descriptor reopened");
    }
    const auto path = directory / "database.pages";
    std::int32_t tableId = 0;
    std::vector<RowRef> refs;
    auto definition = sql::parse(sql::tokenize("CREATE TABLE student(id INT, name VARCHAR);"))[0];
    {
        auto file = std::make_shared<PageFile>(path);
        BufferPool buffer(file, 2, ReplacementPolicy::LRU);
        HeapStore heap(file, buffer);
        catalog::PersistentCatalog catalog(heap);
        tableId = catalog.create(definition);
        require(tableId >= 2 && catalog.view().find("STUDENT") != nullptr, "catalog create visible");
        const auto allocated = file->allocatedPages();
        rejects([&] { catalog.create(definition); }, "duplicate table rejected");
        require(file->allocatedPages() == allocated, "duplicate no page effect");
        for (std::int32_t i = 0; i < 300; ++i)
            refs.push_back(heap.insert(tableId, schema, {i, std::string(80, char('a' + i % 26))}));
        require(file->pagesFor(tableId).size() > 4, "data exceeds buffer twice");
        std::int32_t count = 0;
        heap.scan(tableId, schema, [&](RowRef, const Row& row) {
            auto id = std::get<std::int32_t>(row[0]);
            require(std::get<std::string>(row[1]) == std::string(80, char('a' + id % 26)), "scanned row payload");
            ++count;
        });
        require(count == 300, "all pages scanned");
        for (std::size_t i = 0; i < refs.size(); i += 2) heap.erase(tableId, refs[i]);
        heap.flush();
        require(!file->pagesFor(0).empty() && !file->pagesFor(1).empty(), "catalog stored in record pages");
        rejects([&] { heap.read(tableId + 1, schema, refs[1]); }, "cross table read rejected");
    }
    {
        auto file = std::make_shared<PageFile>(path);
        BufferPool buffer(file, 2, ReplacementPolicy::FIFO);
        HeapStore heap(file, buffer);
        catalog::PersistentCatalog catalog(heap);
        require(catalog.tables().size() == 1 && catalog.tables()[0].id == tableId, "catalog identity reopened");
        const auto* table = catalog.view().find("student");
        require(table && table->columns.size() == 2 && table->columns[1].type == "varchar", "catalog schema reopened");
        std::int32_t count = 0;
        heap.scan(tableId, schema, [&](RowRef, const Row& row) {
            require(std::get<std::int32_t>(row[0]) % 2 == 1, "deletions persisted");
            ++count;
        });
        require(count == 150, "remaining rows after reopen");
        for (std::size_t i = 1; i < refs.size(); i += 2) heap.erase(tableId, refs[i]);
        heap.flush();
        require(file->pagesFor(tableId).empty(), "empty table pages reclaimed");
        require(catalog.view().find("student") != nullptr, "delete all preserves schema");
        const auto id = heap.insert(tableId, schema, {std::int32_t(99), std::string("new")});
        rejects([&] { heap.read(tableId, schema, refs[1]); }, "old row ref after page reuse");
        require(std::get<std::int32_t>(heap.read(tableId, schema, id)[0]) == 99, "insert after delete all");
        heap.flush();
    }
    std::cout << checks << " row/heap/catalog checks passed\nEvidence: " << directory.string() << '\n';
}
