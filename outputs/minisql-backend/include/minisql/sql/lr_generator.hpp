#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
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

} // namespace minisql::sql
