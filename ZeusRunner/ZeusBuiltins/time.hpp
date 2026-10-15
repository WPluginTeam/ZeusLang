#pragma once

// ZeusBuiltins Time Functions
// 时间相关函数：time, clock, localtime, strftime 等

#include "core.hpp"
#include "../../ZeusRuntimeLib/ZeusGC.hpp"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace ZeusBuiltins {

    // =====================================================================
    // 时间函数实现
    // =====================================================================

    // __time_time(): 返回当前时间戳（秒，浮点数）
    inline ZeusType::UniversalVariant builtin_time_time(const std::vector<ZeusType::UniversalVariant>& args) {
        auto now = std::chrono::system_clock::now();
        auto duration = now.time_since_epoch();
        auto seconds = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count() / 1000.0;

        return ZeusType::UniversalVariant(ZeusType::BigDecimal(std::to_string(seconds)));
    }

    // __time_time_ms(): 返回当前时间戳（毫秒，整数）
    inline ZeusType::UniversalVariant builtin_time_time_ms(const std::vector<ZeusType::UniversalVariant>& args) {
        auto now = std::chrono::system_clock::now();
        auto duration = now.time_since_epoch();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();

        return ZeusType::UniversalVariant(ZeusType::BigInteger(ms));
    }

    // __time_time_ns(): 返回当前时间戳（纳秒，整数）
    inline ZeusType::UniversalVariant builtin_time_time_ns(const std::vector<ZeusType::UniversalVariant>& args) {
        auto now = std::chrono::system_clock::now();
        auto duration = now.time_since_epoch();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();

        return ZeusType::UniversalVariant(ZeusType::BigInteger(ns));
    }

    // __time_clock(): 返回程序运行时间（秒，浮点数）
    inline ZeusType::UniversalVariant builtin_time_clock(const std::vector<ZeusType::UniversalVariant>& args) {
        static auto start = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(now - start).count();
        double seconds = duration / 1000000.0;

        return ZeusType::UniversalVariant(ZeusType::BigDecimal(std::to_string(seconds)));
    }

    // __time_perf_counter(): 高精度性能计数器（秒，浮点数）
    inline ZeusType::UniversalVariant builtin_time_perf_counter(const std::vector<ZeusType::UniversalVariant>& args) {
        auto now = std::chrono::high_resolution_clock::now();
        auto duration = now.time_since_epoch();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
        double seconds = ns / 1000000000.0;

        return ZeusType::UniversalVariant(ZeusType::BigDecimal(std::to_string(seconds)));
    }

    // __time_perf_counter_ns(): 高精度性能计数器（纳秒，整数）
    inline ZeusType::UniversalVariant builtin_time_perf_counter_ns(const std::vector<ZeusType::UniversalVariant>& args) {
        auto now = std::chrono::high_resolution_clock::now();
        auto duration = now.time_since_epoch();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();

        return ZeusType::UniversalVariant(ZeusType::BigInteger(ns));
    }

    // __time_localtime(timestamp?): 返回本地时间结构（字典）
    // 如果不提供 timestamp，使用当前时间
    inline ZeusType::UniversalVariant builtin_time_localtime(const std::vector<ZeusType::UniversalVariant>& args) {
        std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
        std::time_t timestamp;

        if (args.empty()) {
            timestamp = std::time(nullptr);
        } else {
            // 从参数获取时间戳
            const auto& v = args[0];

            if (std::holds_alternative<ZeusType::BigInteger>(v)) {
                timestamp = static_cast<std::time_t>(std::get<ZeusType::BigInteger>(v).toInt64());
            } else if (std::holds_alternative<ZeusType::BigDecimal>(v)) {
                timestamp = static_cast<std::time_t>(std::get<ZeusType::BigDecimal>(v).toDouble());
            } else if (std::holds_alternative<LexerType::Value>(v)) {
                const auto& val = std::get<LexerType::Value>(v);

                if (std::holds_alternative<ZeusType::BigInteger>(val.data)) {
                    timestamp = static_cast<std::time_t>(std::get<ZeusType::BigInteger>(val.data).toInt64());
                } else if (std::holds_alternative<ZeusType::BigDecimal>(val.data)) {
                    timestamp = static_cast<std::time_t>(std::get<ZeusType::BigDecimal>(val.data).toDouble());
                } else {
                    throw std::runtime_error("__time_localtime: invalid timestamp type");
                }
            } else {
                throw std::runtime_error("__time_localtime: invalid timestamp type");
            }
        }

        std::tm* tm = std::localtime(&timestamp);

        if (!tm) {
            throw std::runtime_error("__time_localtime: failed to convert timestamp");
        }

        // 创建时间结构字典
        ZeusType::Object obj;
        obj.impl = ZeusGC::makeObjectData();
        
        auto setField = [&](const std::string& key, int64_t value) {
            obj.impl->setValue(
                key, ZeusType::UniversalVariant(ZeusType::BigInteger(value)));
        };

        setField("tm_year", tm->tm_year + 1900);  // 年份（完整年份）
        setField("tm_mon", tm->tm_mon + 1);        // 月份（1-12）
        setField("tm_mday", tm->tm_mday);          // 日（1-31）
        setField("tm_hour", tm->tm_hour);          // 时（0-23）
        setField("tm_min", tm->tm_min);            // 分（0-59）
        setField("tm_sec", tm->tm_sec);            // 秒（0-59）
        setField("tm_wday", tm->tm_wday);          // 星期（0=周日，6=周六）
        setField("tm_yday", tm->tm_yday + 1);      // 年内天数（1-366）
        setField("tm_isdst", tm->tm_isdst);        // 夏令时标志

        return ZeusType::UniversalVariant(ZeusType::Variant(obj));
    }

    // __time_gmtime(timestamp?): 返回 UTC 时间结构（字典）
    inline ZeusType::UniversalVariant builtin_time_gmtime(const std::vector<ZeusType::UniversalVariant>& args) {
        std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
        std::time_t timestamp;

        if (args.empty()) {
            timestamp = std::time(nullptr);
        } else {
            const auto& v = args[0];

            if (std::holds_alternative<ZeusType::BigInteger>(v)) {
                timestamp = static_cast<std::time_t>(std::get<ZeusType::BigInteger>(v).toInt64());
            } else if (std::holds_alternative<ZeusType::BigDecimal>(v)) {
                timestamp = static_cast<std::time_t>(std::get<ZeusType::BigDecimal>(v).toDouble());
            } else if (std::holds_alternative<LexerType::Value>(v)) {
                const auto& val = std::get<LexerType::Value>(v);

                if (std::holds_alternative<ZeusType::BigInteger>(val.data)) {
                    timestamp = static_cast<std::time_t>(std::get<ZeusType::BigInteger>(val.data).toInt64());
                } else if (std::holds_alternative<ZeusType::BigDecimal>(val.data)) {
                    timestamp = static_cast<std::time_t>(std::get<ZeusType::BigDecimal>(val.data).toDouble());
                } else {
                    throw std::runtime_error("__time_gmtime: invalid timestamp type");
                }
            } else {
                throw std::runtime_error("__time_gmtime: invalid timestamp type");
            }
        }

        std::tm* tm = std::gmtime(&timestamp);

        if (!tm) {
            throw std::runtime_error("__time_gmtime: failed to convert timestamp");
        }

        ZeusType::Object obj;
        obj.impl = ZeusGC::makeObjectData();
        
        auto setField = [&](const std::string& key, int64_t value) {
            obj.impl->setValue(
                key, ZeusType::UniversalVariant(ZeusType::BigInteger(value)));
        };

        setField("tm_year", tm->tm_year + 1900);
        setField("tm_mon", tm->tm_mon + 1);
        setField("tm_mday", tm->tm_mday);
        setField("tm_hour", tm->tm_hour);
        setField("tm_min", tm->tm_min);
        setField("tm_sec", tm->tm_sec);
        setField("tm_wday", tm->tm_wday);
        setField("tm_yday", tm->tm_yday + 1);
        setField("tm_isdst", tm->tm_isdst);

        return ZeusType::UniversalVariant(ZeusType::Variant(obj));
    }

    // __time_mktime(tm_dict): 将时间结构转换为时间戳
    inline ZeusType::UniversalVariant builtin_time_mktime(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.empty()) {
            throw std::runtime_error("__time_mktime: requires 1 argument");
        }

        const auto& v = args[0];

        // 从 Variant 中提取 Object
        if (!std::holds_alternative<ZeusType::Variant>(v)) {
            throw std::runtime_error("__time_mktime: argument must be a time struct dict");
        }

        const auto& var = std::get<ZeusType::Variant>(v);

        if (!std::holds_alternative<ZeusType::Object>(var)) {
            throw std::runtime_error("__time_mktime: argument must be a time struct dict");
        }

        const auto& obj = std::get<ZeusType::Object>(var);

        if (!obj.impl) {
            throw std::runtime_error("__time_mktime: invalid time struct");
        }

        auto getField = [&](const std::string& key, int defaultVal = 0) -> int {
            auto it = obj.impl->data.find(key);

            if (it == obj.impl->data.end()) {
                return defaultVal;
            }

            const auto& fv = it->second;

            if (std::holds_alternative<ZeusType::BigInteger>(fv)) {
                return static_cast<int>(std::get<ZeusType::BigInteger>(fv).toInt64());
            }

            if (std::holds_alternative<LexerType::Value>(fv)) {
                const auto& val = std::get<LexerType::Value>(fv);

                if (std::holds_alternative<ZeusType::BigInteger>(val.data)) {
                    return static_cast<int>(std::get<ZeusType::BigInteger>(val.data).toInt64());
                }
            }

            return defaultVal;
        };

        std::tm tm = {};
        tm.tm_year = getField("tm_year", 1970) - 1900;
        tm.tm_mon = getField("tm_mon", 1) - 1;
        tm.tm_mday = getField("tm_mday", 1);
        tm.tm_hour = getField("tm_hour", 0);
        tm.tm_min = getField("tm_min", 0);
        tm.tm_sec = getField("tm_sec", 0);
        tm.tm_isdst = getField("tm_isdst", -1);

        std::time_t timestamp = std::mktime(&tm);

        if (timestamp == -1) {
            throw std::runtime_error("__time_mktime: failed to convert time struct");
        }

        return ZeusType::UniversalVariant(ZeusType::BigInteger(static_cast<int64_t>(timestamp)));
    }

    // __time_strftime(format, tm_dict?): 格式化时间字符串
    inline ZeusType::UniversalVariant builtin_time_strftime(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.empty()) {
            throw std::runtime_error("__time_strftime: requires at least 1 argument (format)");
        }

        // 获取格式字符串
        std::string format;
        const auto& fmtArg = args[0];

        if (std::holds_alternative<std::string>(fmtArg)) {
            format = std::get<std::string>(fmtArg);
        } else if (std::holds_alternative<LexerType::Value>(fmtArg)) {
            const auto& val = std::get<LexerType::Value>(fmtArg);

            if (std::holds_alternative<std::string>(val.data)) {
                format = std::get<std::string>(val.data);
            } else {
                throw std::runtime_error("__time_strftime: format must be a string");
            }
        } else {
            throw std::runtime_error("__time_strftime: format must be a string");
        }

        std::tm tm;

        if (args.size() >= 2) {
            // 使用提供的时间结构
            const auto& v = args[1];

            if (!std::holds_alternative<ZeusType::Variant>(v)) {
                throw std::runtime_error("__time_strftime: second argument must be a time struct dict");
            }

            const auto& var = std::get<ZeusType::Variant>(v);

            if (!std::holds_alternative<ZeusType::Object>(var)) {
                throw std::runtime_error("__time_strftime: second argument must be a time struct dict");
            }

            const auto& obj = std::get<ZeusType::Object>(var);

            if (!obj.impl) {
                throw std::runtime_error("__time_strftime: invalid time struct");
            }

            auto getField = [&](const std::string& key, int defaultVal = 0) -> int {
                auto it = obj.impl->data.find(key);

                if (it == obj.impl->data.end()) {
                    return defaultVal;
                }

                const auto& fv = it->second;

                if (std::holds_alternative<ZeusType::BigInteger>(fv)) {
                    return static_cast<int>(std::get<ZeusType::BigInteger>(fv).toInt64());
                }

                if (std::holds_alternative<LexerType::Value>(fv)) {
                    const auto& val = std::get<LexerType::Value>(fv);

                    if (std::holds_alternative<ZeusType::BigInteger>(val.data)) {
                        return static_cast<int>(std::get<ZeusType::BigInteger>(val.data).toInt64());
                    }
                }

                return defaultVal;
            };

            tm = {};
            tm.tm_year = getField("tm_year", 1970) - 1900;
            tm.tm_mon = getField("tm_mon", 1) - 1;
            tm.tm_mday = getField("tm_mday", 1);
            tm.tm_hour = getField("tm_hour", 0);
            tm.tm_min = getField("tm_min", 0);
            tm.tm_sec = getField("tm_sec", 0);
            tm.tm_wday = getField("tm_wday", 0);
            tm.tm_yday = getField("tm_yday", 1) - 1;
            tm.tm_isdst = getField("tm_isdst", -1);
        } else {
            // 使用当前时间
            std::time_t now = std::time(nullptr);
            std::tm* tmPtr = std::localtime(&now);

            if (!tmPtr) {
                throw std::runtime_error("__time_strftime: failed to get current time");
            }

            tm = *tmPtr;
        }

        char buffer[256];
        size_t result = std::strftime(buffer, sizeof(buffer), format.c_str(), &tm);

        if (result == 0) {
            throw std::runtime_error("__time_strftime: format string too long or invalid");
        }

        return ZeusType::UniversalVariant(std::string(buffer));
    }

    // __time_monotonic(): 单调时钟（秒，浮点数），用于测量时间间隔
    inline ZeusType::UniversalVariant builtin_time_monotonic(const std::vector<ZeusType::UniversalVariant>& args) {
        auto now = std::chrono::steady_clock::now();
        auto duration = now.time_since_epoch();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
        double seconds = ns / 1000000000.0;

        return ZeusType::UniversalVariant(ZeusType::BigDecimal(std::to_string(seconds)));
    }

    // __time_monotonic_ns(): 单调时钟（纳秒，整数）
    inline ZeusType::UniversalVariant builtin_time_monotonic_ns(const std::vector<ZeusType::UniversalVariant>& args) {
        auto now = std::chrono::steady_clock::now();
        auto duration = now.time_since_epoch();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();

        return ZeusType::UniversalVariant(ZeusType::BigInteger(ns));
    }

    // __time_sleep(seconds): 休眠指定秒数（支持浮点数）
    inline ZeusType::UniversalVariant builtin_time_sleep(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.empty()) {
            throw std::runtime_error("__time_sleep: requires 1 argument (seconds)");
        }

        double seconds = 0.0;
        const auto& v = args[0];

        if (std::holds_alternative<ZeusType::BigInteger>(v)) {
            seconds = static_cast<double>(std::get<ZeusType::BigInteger>(v).toInt64());
        } else if (std::holds_alternative<ZeusType::BigDecimal>(v)) {
            seconds = std::get<ZeusType::BigDecimal>(v).toDouble();
        } else if (std::holds_alternative<LexerType::Value>(v)) {
            const auto& val = std::get<LexerType::Value>(v);

            if (std::holds_alternative<ZeusType::BigInteger>(val.data)) {
                seconds = static_cast<double>(std::get<ZeusType::BigInteger>(val.data).toInt64());
            } else if (std::holds_alternative<ZeusType::BigDecimal>(val.data)) {
                seconds = std::get<ZeusType::BigDecimal>(val.data).toDouble();
            } else {
                throw std::runtime_error("__time_sleep: argument must be a number");
            }
        } else {
            throw std::runtime_error("__time_sleep: argument must be a number");
        }

        if (seconds > 0) {
            auto ms = static_cast<int64_t>(seconds * 1000);
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        }

        return ZeusType::UniversalVariant(LexerType::Value(true));
    }

    // =====================================================================
    // 注册函数
    // =====================================================================

    inline void registerTimeBuiltins() {
        registerBuiltin("__time_time", builtin_time_time, 0, 0);
        registerBuiltin("__time_time_ms", builtin_time_time_ms, 0, 0);
        registerBuiltin("__time_time_ns", builtin_time_time_ns, 0, 0);
        registerBuiltin("__time_clock", builtin_time_clock, 0, 0);
        registerBuiltin("__time_perf_counter", builtin_time_perf_counter, 0, 0);
        registerBuiltin("__time_perf_counter_ns", builtin_time_perf_counter_ns, 0, 0);
        registerBuiltin("__time_localtime", builtin_time_localtime, 0, 1);
        registerBuiltin("__time_gmtime", builtin_time_gmtime, 0, 1);
        registerBuiltin("__time_mktime", builtin_time_mktime, 1, 1);
        registerBuiltin("__time_strftime", builtin_time_strftime, 1, 2);
        registerBuiltin("__time_monotonic", builtin_time_monotonic, 0, 0);
        registerBuiltin("__time_monotonic_ns", builtin_time_monotonic_ns, 0, 0);
        registerBuiltin("__time_sleep", builtin_time_sleep, 1, 1);
    }


}  // namespace ZeusBuiltins
