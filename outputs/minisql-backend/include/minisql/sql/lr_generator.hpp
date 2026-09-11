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

struct GrammarProduction {
    std::string lhs;
    std::vector<std::string> rhs;
};

struct LrItem {
    std::size_t production = 0;
    std::size_t dot = 0;
    bool operator<(const LrItem& other) const {
        return std::tie(production, dot) < std::tie(other.production, other.dot);
    }
    bool operator==(const LrItem& other) const {
        return production == other.production && dot == other.dot;
    }
};

struct LrState {
    std::vector<LrItem> items;
    std::map<std::string, std::size_t> transitions;
};

struct LrTable {
    std::vector<GrammarProduction> productions;
    std::vector<LrState> states;
    std::map<std::pair<std::size_t, std::string>, std::string> actions;
};

inline LrTable buildCanonicalLr0(std::vector<GrammarProduction> grammar) {
    if (grammar.empty()) return {};
    grammar.insert(grammar.begin(), GrammarProduction{"$accept", {grammar.front().lhs}});
    std::set<std::string> nonterminals;
    for (const auto& production : grammar) nonterminals.insert(production.lhs);
    const auto closure = [&](std::vector<LrItem> items) {
        std::set<LrItem> closed(items.begin(), items.end());
        bool changed = true;
        while (changed) {
            changed = false;
            const std::vector<LrItem> snapshot(closed.begin(), closed.end());
            for (const auto& item : snapshot) {
                const auto& production = grammar[item.production];
                if (item.dot >= production.rhs.size()) continue;
                const auto& symbol = production.rhs[item.dot];
                if (!nonterminals.contains(symbol)) continue;
                for (std::size_t index = 0; index < grammar.size(); ++index)
                    if (grammar[index].lhs == symbol && closed.insert(LrItem{index, 0}).second) changed = true;
            }
        }
        return std::vector<LrItem>(closed.begin(), closed.end());
    };
    const auto gotoSet = [&](const std::vector<LrItem>& items, const std::string& symbol) {
        std::vector<LrItem> moved;
        for (const auto& item : items) {
            const auto& production = grammar[item.production];
            if (item.dot < production.rhs.size() && production.rhs[item.dot] == symbol)
                moved.push_back(LrItem{item.production, item.dot + 1});
        }
        return closure(std::move(moved));
    };
    LrTable table;
    table.productions = grammar;
    table.states.push_back(LrState{closure({LrItem{0, 0}}), {}});
    for (std::size_t stateIndex = 0; stateIndex < table.states.size(); ++stateIndex) {
        const auto items = table.states[stateIndex].items;
        std::set<std::string> symbols;
        for (const auto& item : items) {
            const auto& production = grammar[item.production];
            if (item.dot < production.rhs.size()) symbols.insert(production.rhs[item.dot]);
        }
        for (const auto& symbol : symbols) {
            const auto targetItems = gotoSet(items, symbol);
            if (targetItems.empty()) continue;
            std::size_t target = table.states.size();
            for (std::size_t index = 0; index < table.states.size(); ++index)
                if (table.states[index].items == targetItems) { target = index; break; }
            if (target == table.states.size()) table.states.push_back(LrState{targetItems, {}});
            table.states[stateIndex].transitions[symbol] = target;
            if (nonterminals.contains(symbol)) table.actions[{stateIndex, symbol}] = "goto " + std::to_string(target);
            else table.actions[{stateIndex, symbol}] = "shift " + std::to_string(target);
        }
        for (const auto& item : items) {
            const auto& production = grammar[item.production];
            if (item.dot != production.rhs.size()) continue;
            table.actions[{stateIndex, "$"}] = item.production == 0 ? "accept" : "reduce " + std::to_string(item.production);
        }
    }
    return table;
}

struct LookaheadItem {
    std::size_t production = 0;
    std::size_t dot = 0;
    std::string lookahead;
    bool operator<(const LookaheadItem& other) const {
        return std::tie(production, dot, lookahead) < std::tie(other.production, other.dot, other.lookahead);
    }
    bool operator==(const LookaheadItem& other) const {
        return production == other.production && dot == other.dot && lookahead == other.lookahead;
    }
};

struct LookaheadState {
    std::vector<LookaheadItem> items;
    std::map<std::string, std::size_t> transitions;
};

struct LookaheadTable {
    std::vector<GrammarProduction> productions;
    std::vector<LookaheadState> states;
    std::map<std::pair<std::size_t, std::string>, std::string> actions;
};

inline LookaheadTable buildCanonicalLr1(std::vector<GrammarProduction> grammar) {
    if (grammar.empty()) return {};
    grammar.insert(grammar.begin(), GrammarProduction{"$accept", {grammar.front().lhs}});
    std::set<std::string> nonterminals;
    for (const auto& production : grammar) nonterminals.insert(production.lhs);
    std::map<std::string, std::set<std::string>> first;
    std::set<std::string> nullable;
    for (const auto& production : grammar) if (production.rhs.empty()) nullable.insert(production.lhs);
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& production : grammar) {
            bool allNullable = true;
            for (const auto& symbol : production.rhs) {
                if (!nonterminals.contains(symbol)) { changed |= first[production.lhs].insert(symbol).second; allNullable = false; break; }
                for (const auto& token : first[symbol]) changed |= first[production.lhs].insert(token).second;
                if (!nullable.contains(symbol)) { allNullable = false; break; }
            }
            if (allNullable) changed |= nullable.insert(production.lhs).second;
        }
    }
    const auto firstOfSuffix = [&](const GrammarProduction& production, std::size_t begin, const std::string& lookahead) {
        std::set<std::string> result;
        bool allNullable = true;
        for (std::size_t index = begin; index < production.rhs.size(); ++index) {
            const auto& symbol = production.rhs[index];
            if (!nonterminals.contains(symbol)) { result.insert(symbol); allNullable = false; break; }
            result.insert(first[symbol].begin(), first[symbol].end());
            if (!nullable.contains(symbol)) { allNullable = false; break; }
        }
        if (allNullable) result.insert(lookahead);
        return result;
    };
    const auto closure = [&](std::vector<LookaheadItem> items) {
        std::set<LookaheadItem> closed(items.begin(), items.end());
        std::queue<LookaheadItem> pending;
        for (const auto& item : closed) pending.push(item);
        while (!pending.empty()) {
            const auto item = pending.front(); pending.pop();
            const auto& production = grammar[item.production];
            if (item.dot >= production.rhs.size()) continue;
            const auto& symbol = production.rhs[item.dot];
            if (!nonterminals.contains(symbol)) continue;
            const auto lookaheads = firstOfSuffix(production, item.dot + 1, item.lookahead);
            for (std::size_t index = 0; index < grammar.size(); ++index) {
                if (grammar[index].lhs != symbol) continue;
                for (const auto& lookahead : lookaheads) {
                    LookaheadItem next{index, 0, lookahead};
                    if (closed.insert(next).second) pending.push(next);
                }
            }
        }
        return std::vector<LookaheadItem>(closed.begin(), closed.end());
    };
    const auto gotoSet = [&](const std::vector<LookaheadItem>& items, const std::string& symbol) {
        std::vector<LookaheadItem> moved;
        for (const auto& item : items) {
            const auto& production = grammar[item.production];
            if (item.dot < production.rhs.size() && production.rhs[item.dot] == symbol)
                moved.push_back(LookaheadItem{item.production, item.dot + 1, item.lookahead});
        }
        return closure(std::move(moved));
    };
    LookaheadTable table;
    table.productions = grammar;
    table.states.push_back(LookaheadState{closure({LookaheadItem{0, 0, "$"}}), {}});
    for (std::size_t stateIndex = 0; stateIndex < table.states.size(); ++stateIndex) {
        const auto items = table.states[stateIndex].items;
        std::set<std::string> symbols;
        for (const auto& item : items) {
            const auto& production = grammar[item.production];
            if (item.dot < production.rhs.size()) symbols.insert(production.rhs[item.dot]);
        }
        for (const auto& symbol : symbols) {
            const auto targetItems = gotoSet(items, symbol);
            if (targetItems.empty()) continue;
            std::size_t target = table.states.size();
            for (std::size_t index = 0; index < table.states.size(); ++index)
                if (table.states[index].items == targetItems) { target = index; break; }
            if (target == table.states.size()) table.states.push_back(LookaheadState{targetItems, {}});
            table.states[stateIndex].transitions[symbol] = target;
            table.actions[{stateIndex, symbol}] = (nonterminals.contains(symbol) ? "goto " : "shift ") + std::to_string(target);
        }
        for (const auto& item : items) {
            const auto& production = grammar[item.production];
            if (item.dot != production.rhs.size()) continue;
            table.actions[{stateIndex, item.lookahead}] = item.production == 0 ? "accept" : "reduce " + std::to_string(item.production);
        }
    }
    return table;
}

inline LookaheadTable buildLalr(std::vector<GrammarProduction> grammar) {
    auto canonical = buildCanonicalLr1(std::move(grammar));
    if (canonical.states.empty()) return canonical;
    const auto coreKey = [&](const LookaheadState& state) {
        std::string value;
        for (const auto& item : state.items) value += std::to_string(item.production) + ":" + std::to_string(item.dot) + ";";
        return value;
    };
    std::map<std::string, std::size_t> mergedByCore;
    std::vector<std::size_t> canonicalToMerged(canonical.states.size());
    LookaheadTable result;
    result.productions = canonical.productions;
    for (std::size_t index = 0; index < canonical.states.size(); ++index) {
        const auto key = coreKey(canonical.states[index]);
        auto [found, inserted] = mergedByCore.emplace(key, result.states.size());
        if (inserted) result.states.push_back({});
        canonicalToMerged[index] = found->second;
    }
    for (std::size_t index = 0; index < canonical.states.size(); ++index) {
        auto& target = result.states[canonicalToMerged[index]];
        target.items.insert(target.items.end(), canonical.states[index].items.begin(), canonical.states[index].items.end());
    }
    for (auto& state : result.states) {
        std::sort(state.items.begin(), state.items.end());
        state.items.erase(std::unique(state.items.begin(), state.items.end()), state.items.end());
    }
    for (std::size_t index = 0; index < canonical.states.size(); ++index) {
        auto& target = result.states[canonicalToMerged[index]];
        for (const auto& [symbol, destination] : canonical.states[index].transitions)
            target.transitions[symbol] = canonicalToMerged[destination];
    }
    for (std::size_t stateIndex = 0; stateIndex < result.states.size(); ++stateIndex) {
        for (const auto& [symbol, target] : result.states[stateIndex].transitions)
            result.actions[{stateIndex, symbol}] = "goto-or-shift " + std::to_string(target);
        for (const auto& item : result.states[stateIndex].items) {
            const auto& production = result.productions[item.production];
            if (item.dot == production.rhs.size())
                result.actions[{stateIndex, item.lookahead}] = item.production == 0 ? "accept" : "reduce " + std::to_string(item.production);
            else
                result.actions[{stateIndex, production.rhs[item.dot]}] = "shift " + std::to_string(result.states[stateIndex].transitions.at(production.rhs[item.dot]));
        }
    }
    return result;
}

} // namespace minisql::sql
