#pragma once

#include "core.hpp"
#include "../../ZeusRuntimeLib/ZeusGC.hpp"

#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ZeusBuiltins {

namespace RegexBuiltinDetail {

using Value = ZeusType::UniversalVariant;

struct MatchRecord {
    std::vector<std::string> groups;
    std::vector<bool> matched;
    std::vector<int64_t> starts;
    std::vector<int64_t> ends;
};

struct RegexCacheEntry {
    std::shared_ptr<const std::regex> expression;
    std::list<std::string>::iterator position;
};

inline std::mutex& cacheMutex() {
    static std::mutex value;
    return value;
}

inline std::unordered_map<std::string, RegexCacheEntry>& cache() {
    static std::unordered_map<std::string, RegexCacheEntry> value;
    return value;
}

inline std::list<std::string>& cacheOrder() {
    static std::list<std::string> value;
    return value;
}

inline const std::string& requireString(
    const Value& value,
    const char* functionName,
    const char* argumentName
) {
    if (const auto* text = std::get_if<std::string>(&value)) return *text;
    if (const auto* boxed = std::get_if<LexerType::Value>(&value)) {
        if (const auto* text = std::get_if<std::string>(&boxed->data)) return *text;
    }
    throw std::runtime_error(std::string(functionName) + ": " + argumentName +
        " must be a string");
}

inline int64_t requireInteger(
    const Value& value,
    const char* functionName,
    const char* argumentName
) {
    const ZeusType::BigInteger* integer = std::get_if<ZeusType::BigInteger>(&value);
    if (!integer) {
        if (const auto* boxed = std::get_if<LexerType::Value>(&value)) {
            integer = std::get_if<ZeusType::BigInteger>(&boxed->data);
        }
    }
    if (!integer) {
        throw std::runtime_error(std::string(functionName) + ": " + argumentName +
            " must be an integer");
    }
    return ZeusType::bigIntegerToInt(*integer);
}

inline std::string cacheKey(const std::string& pattern, int64_t flags) {
    return std::to_string(flags) + ":" + std::to_string(pattern.size()) + ":" + pattern;
}

inline std::string dotAllPattern(const std::string& pattern) {
    std::string transformed;
    transformed.reserve(pattern.size());
    bool escaped = false;
    bool inCharacterClass = false;
    for (const char current : pattern) {
        if (escaped) {
            transformed.push_back(current);
            escaped = false;
            continue;
        }
        if (current == '\\') {
            transformed.push_back(current);
            escaped = true;
            continue;
        }
        if (current == '[' && !inCharacterClass) {
            inCharacterClass = true;
            transformed.push_back(current);
            continue;
        }
        if (current == ']' && inCharacterClass) {
            inCharacterClass = false;
            transformed.push_back(current);
            continue;
        }
        if (current == '.' && !inCharacterClass) {
            transformed += "[\\s\\S]";
        } else {
            transformed.push_back(current);
        }
    }
    return transformed;
}

inline std::shared_ptr<const std::regex> compile(
    const std::string& pattern,
    int64_t flags
) {
    constexpr int64_t ignoreCase = 2;
    constexpr int64_t dotAll = 16;
    constexpr int64_t supportedFlags = ignoreCase | dotAll;
    if ((flags & ~supportedFlags) != 0) {
        throw std::runtime_error(
            "re.compile: only IGNORECASE and DOTALL flags are supported");
    }

    const std::string key = cacheKey(pattern, flags);
    {
        std::lock_guard<std::mutex> lock(cacheMutex());
        auto& entries = cache();
        const auto found = entries.find(key);
        if (found != entries.end()) {
            cacheOrder().splice(cacheOrder().begin(), cacheOrder(), found->second.position);
            return found->second.expression;
        }
    }

    auto syntax = std::regex_constants::ECMAScript;
    if ((flags & ignoreCase) != 0) syntax |= std::regex_constants::icase;
    const std::string source = (flags & dotAll) != 0
        ? dotAllPattern(pattern)
        : pattern;

    std::shared_ptr<const std::regex> expression;
    try {
        expression = std::make_shared<const std::regex>(source, syntax);
    } catch (const std::regex_error& error) {
        throw std::runtime_error(std::string("re.error: ") + error.what());
    }

    {
        std::lock_guard<std::mutex> lock(cacheMutex());
        auto& entries = cache();
        const auto raced = entries.find(key);
        if (raced != entries.end()) {
            cacheOrder().splice(cacheOrder().begin(), cacheOrder(), raced->second.position);
            return raced->second.expression;
        }
        constexpr size_t cacheLimit = 128;
        auto& order = cacheOrder();
        if (entries.size() >= cacheLimit) {
            entries.erase(order.back());
            order.pop_back();
        }
        order.push_front(key);
        entries.emplace(key, RegexCacheEntry{expression, order.begin()});
    }
    return expression;
}

inline MatchRecord recordMatch(const std::smatch& match) {
    MatchRecord record;
    record.groups.reserve(match.size());
    record.matched.reserve(match.size());
    record.starts.reserve(match.size());
    record.ends.reserve(match.size());
    for (size_t index = 0; index < match.size(); ++index) {
        const bool matched = match[index].matched;
        record.matched.push_back(matched);
        record.groups.push_back(matched ? match[index].str() : std::string());
        if (matched) {
            record.starts.push_back(static_cast<int64_t>(match.position(index)));
            record.ends.push_back(record.starts.back() +
                static_cast<int64_t>(match.length(index)));
        } else {
            record.starts.push_back(-1);
            record.ends.push_back(-1);
        }
    }
    return record;
}

inline std::vector<MatchRecord> findMatches(
    const std::string& pattern,
    int64_t flags,
    const std::string& text,
    int64_t mode
) {
    const auto expression = compile(pattern, flags);
    std::vector<MatchRecord> result;
    if (mode == 2) {
        std::smatch match;
        if (std::regex_match(text, match, *expression)) {
            result.push_back(recordMatch(match));
        }
        return result;
    }

    if (mode == 0 || mode == 1) {
        std::smatch match;
        auto matchFlags = std::regex_constants::match_default;
        if (mode == 1) matchFlags |= std::regex_constants::match_continuous;
        if (std::regex_search(text, match, *expression, matchFlags)) {
            result.push_back(recordMatch(match));
        }
        return result;
    }

    if (mode != 3) throw std::runtime_error("re: invalid internal match mode");
    for (std::sregex_iterator current(text.begin(), text.end(), *expression), end;
         current != end; ++current) {
        result.push_back(recordMatch(*current));
    }
    return result;
}

inline Value array(std::vector<Value> items) {
    ZeusType::Array result;
    result.impl = ZeusGC::makeArrayData();
    result.impl->data = std::move(items);
    return ZeusType::Variant(std::move(result));
}

inline Value integer(int64_t value) {
    return ZeusType::BigInteger(value);
}

inline Value nullValue() {
    return ZeusType::Variant(ZeusType::Null());
}

inline Value groupValue(const MatchRecord& match, size_t index) {
    if (!match.matched[index]) return nullValue();
    return match.groups[index];
}

inline Value toValue(const MatchRecord& match) {
    std::vector<Value> groups;
    groups.reserve(match.groups.size());
    for (size_t index = 0; index < match.groups.size(); ++index) {
        groups.push_back(array({
            groupValue(match, index),
            integer(match.starts[index]),
            integer(match.ends[index])
        }));
    }
    return array({
        integer(match.starts[0]),
        integer(match.ends[0]),
        array(std::move(groups))
    });
}

inline std::string expandReplacement(
    const std::string& replacement,
    const MatchRecord& match
) {
    std::string result;
    result.reserve(replacement.size());
    for (size_t index = 0; index < replacement.size(); ++index) {
        if (replacement[index] != '\\' || index + 1 >= replacement.size()) {
            result.push_back(replacement[index]);
            continue;
        }
        const char next = replacement[index + 1];
        if (next == '\\') {
            result.push_back('\\');
            ++index;
            continue;
        }
        size_t groupStart = index + 1;
        size_t digitsStart = groupStart;
        size_t digitsEnd = digitsStart;
        if (next == 'g' && digitsStart + 1 < replacement.size() &&
            replacement[digitsStart + 1] == '<') {
            digitsStart += 2;
            digitsEnd = digitsStart;
            while (digitsEnd < replacement.size() && replacement[digitsEnd] >= '0' &&
                   replacement[digitsEnd] <= '9') ++digitsEnd;
            if (digitsEnd == digitsStart || digitsEnd >= replacement.size() ||
                replacement[digitsEnd] != '>') {
                throw std::runtime_error("re.sub: invalid \\g<group> replacement");
            }
            index = digitsEnd;
        } else if (next >= '0' && next <= '9') {
            digitsEnd = digitsStart;
            while (digitsEnd < replacement.size() && replacement[digitsEnd] >= '0' &&
                   replacement[digitsEnd] <= '9') ++digitsEnd;
            index = digitsEnd - 1;
        } else {
            result.push_back('\\');
            continue;
        }

        size_t groupIndex = 0;
        for (size_t digit = digitsStart; digit < digitsEnd; ++digit) {
            groupIndex = groupIndex * 10 +
                static_cast<size_t>(replacement[digit] - '0');
        }
        if (groupIndex >= match.groups.size()) {
            throw std::runtime_error("re.sub: replacement refers to a missing group");
        }
        if (match.matched[groupIndex]) result += match.groups[groupIndex];
    }
    return result;
}

inline std::vector<MatchRecord> findAllForOperation(
    const std::string& pattern,
    int64_t flags,
    const std::string& text
) {
    return findMatches(pattern, flags, text, 3);
}

inline Value builtinCompile(const std::vector<Value>& args) {
    if (args.size() != 2) throw std::runtime_error("__re_compile expects 2 arguments");
    const auto& pattern = requireString(args[0], "__re_compile", "pattern");
    const int64_t flags = requireInteger(args[1], "__re_compile", "flags");
    return integer(static_cast<int64_t>(compile(pattern, flags)->mark_count()));
}

inline Value builtinMatches(const std::vector<Value>& args) {
    if (args.size() != 4) throw std::runtime_error("__re_matches expects 4 arguments");
    const auto& pattern = requireString(args[0], "__re_matches", "pattern");
    const int64_t flags = requireInteger(args[1], "__re_matches", "flags");
    const auto& text = requireString(args[2], "__re_matches", "string");
    const int64_t mode = requireInteger(args[3], "__re_matches", "mode");
    const auto matches = findMatches(pattern, flags, text, mode);
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (mode != 3) {
        return matches.empty() ? nullValue() : toValue(matches.front());
    }
    std::vector<Value> result;
    result.reserve(matches.size());
    for (const auto& match : matches) result.push_back(toValue(match));
    return array(std::move(result));
}

inline Value builtinMatchAt(const std::vector<Value>& args) {
    if (args.size() != 2) throw std::runtime_error("__re_match_at expects 2 arguments");
    const auto* records = getArrayPtr(args[0]);
    if (!records || !records->impl) {
        throw std::runtime_error("__re_match_at: records must be an Array");
    }
    const int64_t index = requireInteger(args[1], "__re_match_at", "index");
    if (index < 0 || static_cast<size_t>(index) >= records->impl->data.size()) {
        throw std::runtime_error("__re_match_at: index is out of range");
    }
    const auto* record = getArrayPtr(records->impl->data[static_cast<size_t>(index)]);
    if (!record) throw std::runtime_error("__re_match_at: invalid match record");
    return records->impl->data[static_cast<size_t>(index)];
}

inline Value builtinFindall(const std::vector<Value>& args) {
    if (args.size() != 3) throw std::runtime_error("__re_findall expects 3 arguments");
    const auto& pattern = requireString(args[0], "__re_findall", "pattern");
    const int64_t flags = requireInteger(args[1], "__re_findall", "flags");
    const auto& text = requireString(args[2], "__re_findall", "string");
    const auto matches = findAllForOperation(pattern, flags, text);
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    std::vector<Value> result;
    result.reserve(matches.size());
    for (const auto& match : matches) {
        if (match.groups.size() == 1) {
            result.emplace_back(match.groups[0]);
        } else if (match.groups.size() == 2) {
            result.emplace_back(match.matched[1] ? match.groups[1] : std::string());
        } else {
            std::vector<Value> groups;
            groups.reserve(match.groups.size() - 1);
            for (size_t index = 1; index < match.groups.size(); ++index) {
                groups.emplace_back(match.matched[index] ? match.groups[index] : std::string());
            }
            result.push_back(array(std::move(groups)));
        }
    }
    return array(std::move(result));
}

inline Value builtinSplit(const std::vector<Value>& args) {
    if (args.size() != 4) throw std::runtime_error("__re_split expects 4 arguments");
    const auto& pattern = requireString(args[0], "__re_split", "pattern");
    const int64_t flags = requireInteger(args[1], "__re_split", "flags");
    const auto& text = requireString(args[2], "__re_split", "string");
    const int64_t maxSplit = requireInteger(args[3], "__re_split", "maxsplit");
    const auto matches = findAllForOperation(pattern, flags, text);
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    std::vector<Value> result;
    size_t previousEnd = 0;
    int64_t splits = 0;
    for (const auto& match : matches) {
        if (maxSplit > 0 && splits >= maxSplit) break;
        const size_t start = static_cast<size_t>(match.starts[0]);
        const size_t end = static_cast<size_t>(match.ends[0]);
        result.emplace_back(text.substr(previousEnd, start - previousEnd));
        for (size_t group = 1; group < match.groups.size(); ++group) {
            result.push_back(match.matched[group]
                ? Value(match.groups[group])
                : nullValue());
        }
        previousEnd = end;
        ++splits;
    }
    result.emplace_back(text.substr(previousEnd));
    return array(std::move(result));
}

struct SubstitutionResult {
    std::string text;
    int64_t count = 0;
};

inline SubstitutionResult substitute(
    const std::string& pattern,
    int64_t flags,
    const std::string& replacement,
    const std::string& text,
    int64_t count
) {
    const auto matches = findAllForOperation(pattern, flags, text);
    std::string result;
    size_t previousEnd = 0;
    int64_t replacements = 0;
    for (const auto& match : matches) {
        if (count > 0 && replacements >= count) break;
        const size_t start = static_cast<size_t>(match.starts[0]);
        const size_t end = static_cast<size_t>(match.ends[0]);
        if (start < previousEnd) continue;
        result.append(text, previousEnd, start - previousEnd);
        result += expandReplacement(replacement, match);
        previousEnd = end;
        ++replacements;
    }
    result.append(text, previousEnd, std::string::npos);
    return SubstitutionResult{std::move(result), replacements};
}

inline Value builtinSub(const std::vector<Value>& args) {
    if (args.size() != 5) throw std::runtime_error("__re_sub expects 5 arguments");
    const auto& pattern = requireString(args[0], "__re_sub", "pattern");
    const int64_t flags = requireInteger(args[1], "__re_sub", "flags");
    const auto& replacement = requireString(args[2], "__re_sub", "repl");
    const auto& text = requireString(args[3], "__re_sub", "string");
    const int64_t count = requireInteger(args[4], "__re_sub", "count");
    return substitute(pattern, flags, replacement, text, count).text;
}

inline Value builtinSubn(const std::vector<Value>& args) {
    if (args.size() != 5) throw std::runtime_error("__re_subn expects 5 arguments");
    const auto& pattern = requireString(args[0], "__re_subn", "pattern");
    const int64_t flags = requireInteger(args[1], "__re_subn", "flags");
    const auto& replacement = requireString(args[2], "__re_subn", "repl");
    const auto& text = requireString(args[3], "__re_subn", "string");
    const int64_t count = requireInteger(args[4], "__re_subn", "count");
    const auto result = substitute(pattern, flags, replacement, text, count);
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    return array({Value(result.text), integer(result.count)});
}

inline Value builtinEscape(const std::vector<Value>& args) {
    if (args.size() != 1) throw std::runtime_error("__re_escape expects 1 argument");
    const auto& text = requireString(args[0], "__re_escape", "pattern");
    std::string result;
    result.reserve(text.size() * 2);
    constexpr std::string_view special = R"(\.^$|()[]{}*+?-)";
    for (const char current : text) {
        if (special.find(current) != std::string_view::npos) result.push_back('\\');
        result.push_back(current);
    }
    return result;
}

} // namespace RegexBuiltinDetail

} // namespace ZeusBuiltins
