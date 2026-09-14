#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace minisql::sql {

inline constexpr std::uint32_t LR_GRAMMAR_VERSION = 1;
// 文法版本号：文法结构变化时递增，便于判断已经缓存的表是否过期。

struct GrammarProduction {
// 一条文法产生式：左部（非终结符）与右部（符号序列）。
// 产生式 A → X1 X2 ... Xn 在本结构里表示为 lhs="A"、rhs={X1, X2, ..., Xn}。
    std::string lhs;
    // lhs 是产生式左部的非终结符名。
    std::vector<std::string> rhs;
    // rhs 是产生式右部；空右部表示 ε 产生式。
};

struct LrItem {
// LR(0) 项目：形如 A → α · β，表示"已经识别完 α，接下来期待 β"。
    std::size_t production = 0;
    // production 是该项目所属产生式在文法表里的下标。
    std::size_t dot = 0;
    // dot 是圆点位置，表示右部已经匹配到第几个符号。
    bool operator<(const LrItem& other) const {
    // 小于运算：先比产生式下标，再比圆点位置。
        return std::tie(production, dot) < std::tie(other.production, other.dot);
        // 用 std::tie 把两个字段打包后按字典序比较，等价于逐字段比较。
    }
    // 小于运算结束。
    bool operator==(const LrItem& other) const {
    // 相等运算：两个字段都相同才算同一个项目。
        return production == other.production && dot == other.dot;
        // 这是项目集去重、以及判断"两个项目集是否相同"的依据。
    }
    // 相等运算结束。
};

struct LrState {
// LR(0) 的一个状态，本质是"一个项目集 + 它的转移关系"。
    std::vector<LrItem> items;
    // items 是该状态包含的项目集合（闭包已经算完）。
    std::map<std::string, std::size_t> transitions;
    // transitions 记录读入某符号后跳到哪个状态，键是符号名。
};

struct LrTable {
// 完整的 LR(0) 分析表，是 buildCanonicalLr0 的最终产物。
    std::vector<GrammarProduction> productions;
    // productions 是增广后的文法，下标 0 是新增的起始产生式 $accept。
    std::vector<LrState> states;
    // states 按发现顺序编号，动作里引用的状态号就是这里的下标。
    std::map<std::pair<std::size_t, std::string>, std::string> actions;
    // actions 是动作表：键为"状态号 + 向前看符号"，值为动作描述字符串。
};

inline LrTable buildCanonicalLr0(std::vector<GrammarProduction> grammar) {
// 构造规范 LR(0) 项目集族并生成分析表；参数按值传入，便于函数内直接增广文法。
    if (grammar.empty()) return {};
    // 文法为空说明调用方没给产生式，直接返回空表，避免后面取 front 出错。
    grammar.insert(grammar.begin(), GrammarProduction{"$accept", {grammar.front().lhs}});
    // 增广文法：在最前面插入 $accept → 起始符号，为接受动作提供唯一入口。
    std::set<std::string> nonterminals;
    // 收集所有产生式左部，得到非终结符集合。
    for (const auto& production : grammar) nonterminals.insert(production.lhs);
    // 闭包展开时靠这个集合区分"该继续展开产生式"还是"到此为止"。
    // 逐个左部登记为非终结符。
    const auto closure = [&](std::vector<LrItem> items) {
    // 闭包函数：把项目集补全成真正的闭包（按值接收，函数内可以自由修改）。
        std::set<LrItem> closed(items.begin(), items.end());
        // 用有序集合保存闭包结果：天然去重，且集合内容可以直接比较是否相同。
        bool changed = true;
        // 反复迭代直到不再有新项目加入，这就是闭包运算的不动点写法。
        while (changed) {
        // 只要上一轮有新项目加入就继续。
            changed = false;
            // 先假设本轮不会再变，真的变了再置真。
            const std::vector<LrItem> snapshot(closed.begin(), closed.end());
            // 把当前闭包拷成快照：遍历快照、往原集合里加，避免迭代器失效。
            for (const auto& item : snapshot) {
            // 逐个检查快照里的项目。
                const auto& production = grammar[item.production];
                // 取出该项目所属的产生式。
                if (item.dot >= production.rhs.size()) continue;
                // 圆点已越过右部末尾，说明项目已经规约完，没有符号可展开，跳过。
                const auto& symbol = production.rhs[item.dot];
                // 取出圆点后面的符号，它决定要不要继续展开。
                if (!nonterminals.contains(symbol)) continue;
                // 该符号不是非终结符（是终结符），不需要展开产生式，跳过。
                for (std::size_t index = 0; index < grammar.size(); ++index)
                // 在文法里找所有左部等于该符号的产生式。
                    if (grammar[index].lhs == symbol && closed.insert(LrItem{index, 0}).second) changed = true;
                    // 找到就插入 A → ·γ（圆点在最前）；插入成功说明是新项目，标记本轮有变化。
            }
            // 快照遍历结束。
        }
        // 迭代结束：本轮没有任何新增项目，闭包到达不动点。
        return std::vector<LrItem>(closed.begin(), closed.end());
        // 把集合按有序顺序转成 vector 返回，保证状态内容可以重复比较。
    };
    // 闭包函数定义结束。
    const auto gotoSet = [&](const std::vector<LrItem>& items, const std::string& symbol) {
    // GOTO 函数：计算"从当前项目集读入某符号后"得到的新项目集（已取闭包）。
        std::vector<LrItem> moved;
        // 存放圆点右移一格后的项目。
        for (const auto& item : items) {
        // 逐个检查输入项目。
            const auto& production = grammar[item.production];
            if (item.dot < production.rhs.size() && production.rhs[item.dot] == symbol)
            // 只有当圆点后面的符号恰好等于给定符号时才移动圆点。
                moved.push_back(LrItem{item.production, item.dot + 1});
                // 圆点右移一格（dot + 1），表示这个符号已经被识别。
        }
        // 逐项处理结束。
        return closure(std::move(moved));
        // 对移动后的项目集取闭包再返回；用 move 避免多余拷贝。
    };
    // GOTO 函数定义结束。
    LrTable table;
    // 准备结果分析表。
    table.productions = grammar;
    // 先把增广后的文法存进表里，后面动作里的产生式下标就是它的下标。
    table.states.push_back(LrState{closure({LrItem{0, 0}}), {}});
    // 初始状态：起始项目 $accept → ·S 的闭包，编号为 0。
    for (std::size_t stateIndex = 0; stateIndex < table.states.size(); ++stateIndex) {
    // 主循环按下标推进；states 会在循环中增长，新状态会被后续迭代继续处理。
    // 这里特意不在循环外缓存 states.size()，否则新状态永远处理不到。
        const auto items = table.states[stateIndex].items;
        // 取出当前状态的项目集副本。
        std::set<std::string> symbols;
        // 收集当前状态下所有"圆点后面"的符号，它们就是可能的转移目标。
        for (const auto& item : items) {
        // 逐个项目看圆点后面是什么符号。
            const auto& production = grammar[item.production];
            if (item.dot < production.rhs.size()) symbols.insert(production.rhs[item.dot]);
            // 圆点没到末尾才有后继符号，登记进集合。
        }
        // 符号收集结束。
        for (const auto& symbol : symbols) {
        // 对每个可转移符号计算目标状态。
            const auto targetItems = gotoSet(items, symbol);
            // 计算读入该符号后到达的项目集。
            if (targetItems.empty()) continue;
            // 项目集为空说明没有实际转移，跳过。
            std::size_t target = table.states.size();
            // 先假定需要新建状态，编号就是当前状态数。
            for (std::size_t index = 0; index < table.states.size(); ++index)
            // 在已有状态里查找是否出现过同一个项目集，出现就复用（状态合并）。
                if (table.states[index].items == targetItems) { target = index; break; }
                // 项目集完全相同就复用该下标并跳出查找。
            if (target == table.states.size()) table.states.push_back(LrState{targetItems, {}});
            // 没找到（下标仍等于状态数）才真正追加一个新状态。
            table.states[stateIndex].transitions[symbol] = target;
            // 记录转移关系：当前状态读该符号去往目标状态。
            if (nonterminals.contains(symbol)) table.actions[{stateIndex, symbol}] = "goto " + std::to_string(target);
            // 该符号是非终结符时，这条弧是 GOTO 动作。
            else table.actions[{stateIndex, symbol}] = "shift " + std::to_string(target);
            // 否则是终结符，对应 SHIFT（移进）动作。
        }
        // 转移处理结束。
        for (const auto& item : items) {
        // 再扫描一遍当前状态的项目，处理可以规约的项目。
            const auto& production = grammar[item.production];
            if (item.dot != production.rhs.size()) continue;
            // 圆点不在末尾说明还没识别完，不能规约，跳过。
            table.actions[{stateIndex, "$"}] = item.production == 0 ? "accept" : "reduce " + std::to_string(item.production);
            // 圆点在末尾：产生式 0（$accept）表示接受；其余按该产生式规约。
            // 动作键里的 "$" 是输入结束符，代表向后看符号是句末。
        }
        // 项目扫描结束。
    }
    // 状态遍历结束。
    return table;
    // 返回构造好的规范 LR(0) 分析表。
}

struct LookaheadItem {
// LR(1) 项目：在 LR(0) 项目基础上多带一个向前看符号。
    std::size_t production = 0;
    // production 是所属产生式下标。
    std::size_t dot = 0;
    // dot 是圆点位置。
    std::string lookahead;
    // lookahead 是向前看终结符：只有当前输入符号等于它时才允许按这条产生式规约。
    bool operator<(const LookaheadItem& other) const {
    // 小于运算：三个字段依次比较，保证项目能放进有序容器。
        return std::tie(production, dot, lookahead) < std::tie(other.production, other.dot, other.lookahead);
        // 先比产生式，再比圆点，最后比向前看符号。
    }
    // 小于运算结束。
    bool operator==(const LookaheadItem& other) const {
    // 相等运算：三个字段全相同才算同一个 LR(1) 项目。
        return production == other.production && dot == other.dot && lookahead == other.lookahead;
        // 三个字段逐一比较。
    }
    // 相等运算结束。
};

struct LookaheadState {
// LR(1) 的一个状态：LR(1) 项目集加转移关系。
    std::vector<LookaheadItem> items;
    // items 是该状态包含的 LR(1) 项目集合。
    std::map<std::string, std::size_t> transitions;
    // transitions 记录读入某符号后转到的状态号。
};

struct LookaheadTable {
// LR(1) 与 LALR 共用的分析表结构。
    std::vector<GrammarProduction> productions;
    // productions 是增广后的文法。
    std::vector<LookaheadState> states;
    // states 是状态集合。
    std::map<std::pair<std::size_t, std::string>, std::string> actions;
    // actions 是动作表，键为"状态号 + 向前看符号"，值为动作描述字符串。
};

inline LookaheadTable buildCanonicalLr1(std::vector<GrammarProduction> grammar) {
// 构造规范 LR(1) 项目集族；参数按值传入以便函数内增广文法。
    if (grammar.empty()) return {};
    // 文法为空直接返回空表。
    grammar.insert(grammar.begin(), GrammarProduction{"$accept", {grammar.front().lhs}});
    // 增广文法：插入 $accept → 起始符号。
    std::set<std::string> nonterminals;
    // 收集非终结符集合。
    for (const auto& production : grammar) nonterminals.insert(production.lhs);
    // 逐个左部登记。
    std::map<std::string, std::set<std::string>> first;
    // FIRST 集合表：first[X] 保存可以从 X 推导出的首个终结符集合。
    std::set<std::string> nullable;
    // 可空非终结符集合：能推导出 ε 的符号记在这里。
    for (const auto& production : grammar) if (production.rhs.empty()) nullable.insert(production.lhs);
    // 空右部产生式说明左部可以直接推导 ε，登记为可空。
    bool changed = true;
    // 反复迭代直到 FIRST 与可空集都不再变化。
    while (changed) {
    // 循环条件：还有新信息加入就继续。
        changed = false;
        // 先假设本轮无变化。
        for (const auto& production : grammar) {
        // 逐条产生式分析，尝试把右部首符号能推出的终结符并入左部 FIRST。
            bool allNullable = true;
            // 先假设这条产生式右部整体都可空。
            for (const auto& symbol : production.rhs) {
            // 从左到右扫描右部符号。
                if (!nonterminals.contains(symbol)) { changed |= first[production.lhs].insert(symbol).second; allNullable = false; break; }
                // 遇到终结符：把它加入左部 FIRST 后即可停止，且右部不再可能整体为空。
                for (const auto& token : first[symbol]) changed |= first[production.lhs].insert(token).second;
                // 遇到非终结符：把它的 FIRST 全部并入左部 FIRST。
                if (!nullable.contains(symbol)) { allNullable = false; break; }
                // 若该非终结符不可空，则右部不可能整体为空，停止扫描。
            }
            // 右部扫描结束。
            if (allNullable) changed |= nullable.insert(production.lhs).second;
            // 右部全可空说明左部也能推出 ε，登记为可空。
        }
        // 产生式处理结束。
    }
    // 迭代结束（FIRST 与可空集到达不动点）。
    const auto firstOfSuffix = [&](const GrammarProduction& production, std::size_t begin, const std::string& lookahead) {
    // 计算"右部某个位置之后那段符号串"的 FIRST 集合，用于 LR(1) 闭包传递向前看符号。
        std::set<std::string> result;
        // 结果集合。
        bool allNullable = true;
        // 先假设后半段全部可空。
        for (std::size_t index = begin; index < production.rhs.size(); ++index) {
        // 从 begin 位置依次向后扫描。
            const auto& symbol = production.rhs[index];
            // 取当前位置的符号。
            if (!nonterminals.contains(symbol)) { result.insert(symbol); allNullable = false; break; }
            // 遇到终结符：加入结果并停止（后面不可能再补向前看符号）。
            result.insert(first[symbol].begin(), first[symbol].end());
            // 遇到非终结符：把它的 FIRST 全部并入结果。
            if (!nullable.contains(symbol)) { allNullable = false; break; }
            // 若该非终结符不可空，则后面不可能全可空，停止扫描。
        }
        if (allNullable) result.insert(lookahead);
        // 后半段全可空时，向前看符号本身也能被推出，加入结果。
        return result;
        // 返回计算结果。
    };
    // firstOfSuffix 定义结束。
    const auto closure = [&](std::vector<LookaheadItem> items) {
    // LR(1) 闭包函数：与 LR(0) 闭包的区别是必须把向前看符号正确传递下去。
        std::set<LookaheadItem> closed(items.begin(), items.end());
        // 用有序集合保存闭包结果，天然去重。
        std::queue<LookaheadItem> pending;
        // 用工作队列做增量推进：只有新加入的项目才需要继续展开。
        for (const auto& item : closed) pending.push(item);
        // 初始把已有项目全部入队。
        while (!pending.empty()) {
        // 队列非空就继续处理。
            const auto item = pending.front(); pending.pop();
            // 取出队首项目并出队。
            const auto& production = grammar[item.production];
            if (item.dot >= production.rhs.size()) continue;
            // 圆点到末尾，没有符号可展开，跳过。
            const auto& symbol = production.rhs[item.dot];
            // 取圆点后面的符号。
            if (!nonterminals.contains(symbol)) continue;
            // 不是非终结符就不需要展开，跳过。
            const auto lookaheads = firstOfSuffix(production, item.dot + 1, item.lookahead);
            // 计算该位置往后能推出的向前看符号集合（后半段全可空时包含原向前看符号）。
            for (std::size_t index = 0; index < grammar.size(); ++index) {
            // 在文法里找所有左部等于该符号的产生式。
                if (grammar[index].lhs != symbol) continue;
                // 左部不对就跳过。
                for (const auto& lookahead : lookaheads) {
                // 对每个可能的向前看符号分别生成新项目。
                    LookaheadItem next{index, 0, lookahead};
                    // 新项目：产生式 index、圆点在最前、向前看为 lookahead。
                    if (closed.insert(next).second) pending.push(next);
                    // 插入成功说明是新项目，入队以便继续展开。
                }
                // 向前看符号遍历结束。
            }
            // 产生式遍历结束。
        }
        // 队列处理结束（闭包到达不动点）。
        return std::vector<LookaheadItem>(closed.begin(), closed.end());
        // 把集合按有序顺序转成 vector 返回。
    };
    // 闭包函数定义结束。
    const auto gotoSet = [&](const std::vector<LookaheadItem>& items, const std::string& symbol) {
    // GOTO 函数：读入某符号后把圆点右移一格，再取闭包。
        std::vector<LookaheadItem> moved;
        // 存放移动后的项目。
        for (const auto& item : items) {
        // 逐个检查输入项目。
            const auto& production = grammar[item.production];
            if (item.dot < production.rhs.size() && production.rhs[item.dot] == symbol)
            // 圆点后面的符号恰好等于给定符号时才移动。
                moved.push_back(LookaheadItem{item.production, item.dot + 1, item.lookahead});
                // 圆点右移一格，向前看符号保持不变。
        }
        return closure(std::move(moved));
        // 对移动后的项目集取闭包后返回。
    };
    // GOTO 函数定义结束。
    LookaheadTable table;
    // 准备结果分析表。
    table.productions = grammar;
    // 存入增广后的文法。
    table.states.push_back(LookaheadState{closure({LookaheadItem{0, 0, "$"}}), {}});
    // 初始状态：$accept → ·S, $ 的闭包。
    for (std::size_t stateIndex = 0; stateIndex < table.states.size(); ++stateIndex) {
    // 主循环按下标推进，新状态会在后续迭代中被处理。
        const auto items = table.states[stateIndex].items;
        // 取出当前状态的项目集副本。
        std::set<std::string> symbols;
        // 收集所有可见的转移符号。
        for (const auto& item : items) {
        // 逐个项目看圆点后面的符号。
            const auto& production = grammar[item.production];
            if (item.dot < production.rhs.size()) symbols.insert(production.rhs[item.dot]);
            // 圆点未到末尾才有后继符号，登记进集合。
        }
        // 收集结束。
        for (const auto& symbol : symbols) {
        // 对每个符号计算目标状态。
            const auto targetItems = gotoSet(items, symbol);
            // 计算读入该符号后的项目集。
            if (targetItems.empty()) continue;
            // 空项目集说明没有转移，跳过。
            std::size_t target = table.states.size();
            // 先假定要新建状态。
            for (std::size_t index = 0; index < table.states.size(); ++index)
            // 在已有状态里查找相同项目集以复用编号。
                if (table.states[index].items == targetItems) { target = index; break; }
                // 找到就复用并跳出。
            if (target == table.states.size()) table.states.push_back(LookaheadState{targetItems, {}});
            // 未找到才真正追加新状态。
            table.states[stateIndex].transitions[symbol] = target;
            // 记录转移关系。
            table.actions[{stateIndex, symbol}] = (nonterminals.contains(symbol) ? "goto " : "shift ") + std::to_string(target);
            // 非终结符对应 GOTO，终结符对应 SHIFT。
        }
        // 转移处理结束。
        for (const auto& item : items) {
        // 扫描可规约项目。
            const auto& production = grammar[item.production];
            if (item.dot != production.rhs.size()) continue;
            // 圆点不在末尾则不能规约，跳过。
            table.actions[{stateIndex, item.lookahead}] = item.production == 0 ? "accept" : "reduce " + std::to_string(item.production);
            // 产生式 0 表示接受，其余按该产生式规约；向前看符号来自项目自身。
        }
        // 项目扫描结束。
    }
    // 状态遍历结束。
    return table;
    // 返回规范 LR(1) 分析表。
}

inline LookaheadTable buildLalr(std::vector<GrammarProduction> grammar) {
// 构造 LALR(1) 分析表：先建规范 LR(1)，再按项目核心合并同心状态。
    auto canonical = buildCanonicalLr1(std::move(grammar));
    // 先求出规范 LR(1) 表；move 交出文法所有权以避免复制。
    if (canonical.states.empty()) return canonical;
    // 规范表为空说明文法为空，直接返回空表。
    const auto coreKey = [&](const LookaheadState& state) {
    // 计算状态核心的字符串键：核心只由"产生式 + 圆点位置"决定，忽略向前看符号。
        std::string value;
        // 键缓冲。
        for (const auto& item : state.items) value += std::to_string(item.production) + ":" + std::to_string(item.dot) + ";";
        // 逐个核心项目按固定格式拼接，得到可比较的字符串键。
        return value;
        // 返回核心键。
    };
    // coreKey 定义结束。
    std::map<std::string, std::size_t> mergedByCore;
    // 核心键到合并后状态号的映射。
    std::vector<std::size_t> canonicalToMerged(canonical.states.size());
    // 记录规范状态到合并后状态的对应关系。
    LookaheadTable result;
    // 合并结果表。
    result.productions = canonical.productions;
    // 产生式与规范表保持一致。
    for (std::size_t index = 0; index < canonical.states.size(); ++index) {
    // 逐个规范状态分配合并后的编号。
        const auto key = coreKey(canonical.states[index]);
        // 计算该状态的核心键。
        auto [found, inserted] = mergedByCore.emplace(key, result.states.size());
        // 键已存在则复用编号，否则新建一个合并状态。
        if (inserted) result.states.push_back({});
        // 新建时才追加空状态占位。
        canonicalToMerged[index] = found->second;
        // 记录映射关系。
    }
    // 映射分配结束。
    for (std::size_t index = 0; index < canonical.states.size(); ++index) {
    // 第二遍：把同心状态的项目合并到一起。
        auto& target = result.states[canonicalToMerged[index]];
        // 取出目标合并状态。
        target.items.insert(target.items.end(), canonical.states[index].items.begin(), canonical.states[index].items.end());
        // 把该规范状态的项目整体追加进目标状态。
    }
    // 项目合并结束。
    for (auto& state : result.states) {
    // 合并后要清理重复项目。
        std::sort(state.items.begin(), state.items.end());
        // 先排序，让相同项目彼此相邻。
        state.items.erase(std::unique(state.items.begin(), state.items.end()), state.items.end());
        // 再用 unique 去重并把尾部垃圾删掉。
    }
    // 去重结束。
    for (std::size_t index = 0; index < canonical.states.size(); ++index) {
    // 第三遍：重建转移关系，把目标状态号翻译成合并后的编号。
        auto& target = result.states[canonicalToMerged[index]];
        // 取出目标合并状态。
        for (const auto& [symbol, destination] : canonical.states[index].transitions)
        // 遍历该规范状态的每条转移。
            target.transitions[symbol] = canonicalToMerged[destination];
            // 目标状态号换成合并后的编号后写回。
    }
    // 转移重建结束。
    for (std::size_t stateIndex = 0; stateIndex < result.states.size(); ++stateIndex) {
    // 第四遍：由合并后的状态生成动作表。
        for (const auto& [symbol, target] : result.states[stateIndex].transitions)
        // 先写转移动作。
            result.actions[{stateIndex, symbol}] = "goto-or-shift " + std::to_string(target);
            // 合并后不再区分 GOTO 与 SHIFT，统一记成 goto-or-shift，交给上层按符号类型解释。
        for (const auto& item : result.states[stateIndex].items) {
        // 再写规约与接受动作。
            const auto& production = result.productions[item.production];
            if (item.dot == production.rhs.size())
            // 圆点在末尾的是可规约项目。
                result.actions[{stateIndex, item.lookahead}] = item.production == 0 ? "accept" : "reduce " + std::to_string(item.production);
                // 产生式 0 是接受，其余按该产生式规约（向前看符号来自项目本身）。
            else
            // 圆点不在末尾说明还可以继续移进。
                result.actions[{stateIndex, production.rhs[item.dot]}] = "shift " + std::to_string(result.states[stateIndex].transitions.at(production.rhs[item.dot]));
                // 按下一个符号查转移表得到目标状态，记成 shift 动作。
        }
        // 项目处理结束。
    }
    // 状态处理结束。
    return result;
    // 返回 LALR 分析表。
}

} // namespace minisql::sql
