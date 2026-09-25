#pragma once
#include <cstdio>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include <sstream>

// 调试系统（频道版）
// 用法：
//  1. 编译期：定义 ZEUS_DEBUG 以默认开启全部调试；可再定义 ZEUS_DEBUG_CHANNEL_MASK=0x.. 设定默认频道掩码
//  2. 源文件顶部（包含本头前）可定义 ZEUS_DEBUG_FILE_CHANNEL=ZeusDebug::CH_IR 等，默认 CH_MISC
//  3. 运行期：ZeusDebug::setEnabled(true/false) 总开关；ZeusDebug::enableChannels(mask) / setChannels(mask)
//     以及 ZeusDebug::parseAndSetChannels("IR,VM,PARSE")
//  4. 输出宏 ZEUS_DEBUG_PRINT / ZEUS_DEBUG_EXEC 会按文件频道过滤
//  5. 频道枚举：IR, VM, PARSE, COMPILE, BUILTIN, MISC

namespace ZeusDebug {
    enum Channel : uint32_t {
        CH_IR            = 1u << 0,
        CH_VM            = 1u << 1,
        CH_PARSE         = 1u << 2,
        CH_COMPILE       = 1u << 3,
        CH_BUILTIN       = 1u << 4,
        CH_MISC          = 1u << 5,
        CH_MEM           = 1u << 6,  // 内存调试通道
        CH_PackageLoader = 1u << 7,  // 包加载器调试通道
        CH_ALL           = 0xFFFFFFFFu
    };

    inline std::atomic<bool>& _flag() {
        static std::atomic<bool> g{false};
#ifdef ZEUS_DEBUG
        if(!g.load(std::memory_order_relaxed)) g.store(true, std::memory_order_relaxed);
#endif
        return g;
    }

    inline std::atomic<uint32_t>& _mask() {
#ifdef ZEUS_DEBUG_CHANNEL_MASK
        static std::atomic<uint32_t> m{ (uint32_t)ZEUS_DEBUG_CHANNEL_MASK };
#else
        static std::atomic<uint32_t> m{ CH_ALL };
#endif
        return m;
    }

    inline bool enabled() { return _flag().load(std::memory_order_relaxed); }
    inline void setEnabled(bool v) { _flag().store(v, std::memory_order_relaxed); }

    inline uint32_t channels() { return _mask().load(std::memory_order_relaxed); }
    inline void setChannels(uint32_t m) { _mask().store(m, std::memory_order_relaxed); }
    inline void enableChannels(uint32_t m) { _mask().fetch_or(m, std::memory_order_relaxed); }
    inline void disableChannels(uint32_t m) { _mask().fetch_and(~m, std::memory_order_relaxed); }

    inline bool channelEnabled(uint32_t ch) {
        return (channels() & ch) != 0u;
    }

    inline uint32_t channelFromName(const std::string& name) {
        if (name == "IR") return CH_IR;
        if (name == "VM") return CH_VM;
        if (name == "PARSE") return CH_PARSE;
        if (name == "COMPILE") return CH_COMPILE;
        if (name == "BUILTIN") return CH_BUILTIN;
        if (name == "MISC") return CH_MISC;
        if (name == "MEM") return CH_MEM;
        if (name == "PackageLoader") return CH_PackageLoader;
        if (name == "ALL") return CH_ALL;
        return 0;
    }

    inline void parseAndSetChannels(const std::string& csv) {
        // 逗号/空格分隔，构建掩码
        std::stringstream ss(csv);
        std::string item; uint32_t m=0;
        while (std::getline(ss, item, ',')) {
            // 去除空格
            size_t b=0; while (b<item.size() && (item[b]==' '||item[b]=='\t')) b++;
            size_t e=item.size(); while (e> b && (item[e-1]==' '||item[e-1]=='\t')) e--;
            if (e<=b) continue;
            std::string trimmed=item.substr(b,e-b);
            uint32_t c = channelFromName(trimmed);
            if (c) m |= c;
        }
        if (m==0) m = CH_ALL; // 空或无效 -> ALL
        setChannels(m);
    }
}

#ifndef ZEUS_DEBUG_FILE_CHANNEL
#define ZEUS_DEBUG_FILE_CHANNEL ::ZeusDebug::CH_MISC
#endif

#define ZEUS_DEBUG_PRINT(fmt, ...) do { \
    if (::ZeusDebug::enabled() && ::ZeusDebug::channelEnabled(ZEUS_DEBUG_FILE_CHANNEL)) { std::fprintf(stderr, fmt "\n", ##__VA_ARGS__); } \
} while(0)

#define ZEUS_DEBUG_EXEC(code) do { \
    if (::ZeusDebug::enabled() && ::ZeusDebug::channelEnabled(ZEUS_DEBUG_FILE_CHANNEL)) { code } \
} while(0)
