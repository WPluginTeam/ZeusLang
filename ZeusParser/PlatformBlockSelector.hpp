#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>
#include "TreeParser.hpp"

namespace ZeusParser {

inline std::string currentHostPlatform() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "darwin";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

inline bool matchesPlatform(const std::string& platformName, const std::string& targetPlatform) {
    if (platformName == targetPlatform) return true;
    if (platformName == "win32" && targetPlatform == "windows") return true;
    if (platformName == "windows" && targetPlatform == "win32") return true;
    if ((platformName == "macos" || platformName == "darwin") &&
        (targetPlatform == "darwin" || targetPlatform == "macos")) return true;
    if (platformName == "posix" && (targetPlatform == "linux" || targetPlatform == "darwin" || targetPlatform == "macos")) return true;
    return false;
}

inline void selectPlatformBlocks(
    std::vector<TreeParser::NodeAndBlock>& nodes,
    const std::string& targetPlatform = currentHostPlatform()
) {
    for (auto it = nodes.begin(); it != nodes.end();) {
        if (!std::holds_alternative<std::unique_ptr<TreeParser::CodeBlock>>(*it)) {
            ++it;
            continue;
        }
        auto& block = std::get<std::unique_ptr<TreeParser::CodeBlock>>(*it);
        if (!block) {
            ++it;
            continue;
        }
        if (block->type == CodeBlockType::USING_PLATFORM) {
            if (!matchesPlatform(block->platformName, targetPlatform)) {
                it = nodes.erase(it);
                continue;
            }
            selectPlatformBlocks(block->nodes, targetPlatform);
            auto children = std::move(block->nodes);
            const size_t count = children.size();
            it = nodes.erase(it);
            it = nodes.insert(it, std::make_move_iterator(children.begin()),
                                  std::make_move_iterator(children.end()));
            it += count;
            continue;
        }

        selectPlatformBlocks(block->nodes, targetPlatform);
        ++it;
    }
}

} // namespace ZeusParser
