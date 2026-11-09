#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#if defined(_WIN32)
#define ZEUS_NATIVE_EXPORT __declspec(dllexport)
#else
#define ZEUS_NATIVE_EXPORT __attribute__((visibility("default")))
#endif

namespace {

const char* encodeTimeStruct(const std::tm& value) noexcept {
    static thread_local std::string encoded;
    try {
        encoded = std::to_string(value.tm_year + 1900) + "," +
            std::to_string(value.tm_mon + 1) + "," +
            std::to_string(value.tm_mday) + "," +
            std::to_string(value.tm_hour) + "," +
            std::to_string(value.tm_min) + "," +
            std::to_string(value.tm_sec) + "," +
            std::to_string(value.tm_wday) + "," +
            std::to_string(value.tm_yday + 1) + "," +
            std::to_string(value.tm_isdst);
        return encoded.c_str();
    } catch (...) {
        return nullptr;
    }
}

bool convertTimestamp(double timestamp, bool utc, std::tm& result) noexcept {
    if (!std::isfinite(timestamp)) return false;
    const std::time_t seconds = static_cast<std::time_t>(timestamp);
#if defined(_WIN32)
    return (utc ? gmtime_s(&result, &seconds) : localtime_s(&result, &seconds)) == 0;
#else
    return (utc ? gmtime_r(&seconds, &result) : localtime_r(&seconds, &result)) != nullptr;
#endif
}

bool collectProcessArguments(std::vector<std::string>& arguments) noexcept {
#if defined(_WIN32)
    int count = 0;
    LPWSTR* wideArguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!wideArguments || count < 1) return false;
    bool success = true;
    try {
        arguments.clear();
        arguments.reserve(static_cast<size_t>(count - 1));
        for (int index = 1; index < count; ++index) {
            const int required = WideCharToMultiByte(
                CP_UTF8, WC_ERR_INVALID_CHARS, wideArguments[index], -1,
                nullptr, 0, nullptr, nullptr);
            if (required <= 0) {
                success = false;
                break;
            }
            std::string value(static_cast<size_t>(required), '\0');
            if (required > 1 && WideCharToMultiByte(
                    CP_UTF8, WC_ERR_INVALID_CHARS, wideArguments[index], -1,
                    value.data(), required, nullptr, nullptr) != required) {
                success = false;
                break;
            }
            value.resize(static_cast<size_t>(required - 1));
            arguments.push_back(std::move(value));
        }
    } catch (...) {
        success = false;
    }
    LocalFree(wideArguments);
    if (!success) arguments.clear();
    return success;
#else
    (void)arguments;
    return false;
#endif
}

int32_t serializedProcessArgumentsSize(
    const std::vector<std::string>& arguments
) noexcept {
    int64_t total = 4;
    for (const std::string& argument : arguments) {
        if (argument.size() > static_cast<size_t>(INT32_MAX)) return -1;
        total += 4 + static_cast<int64_t>(argument.size());
        if (total > INT32_MAX) return -1;
    }
    return static_cast<int32_t>(total);
}

std::tm makeTimeStruct(
    int64_t year,
    int64_t month,
    int64_t day,
    int64_t hour,
    int64_t minute,
    int64_t second,
    int64_t weekday,
    int64_t yearDay,
    int64_t daylightSaving
) noexcept {
    std::tm value{};
    value.tm_year = static_cast<int>(year) - 1900;
    value.tm_mon = static_cast<int>(month) - 1;
    value.tm_mday = static_cast<int>(day);
    value.tm_hour = static_cast<int>(hour);
    value.tm_min = static_cast<int>(minute);
    value.tm_sec = static_cast<int>(second);
    value.tm_wday = static_cast<int>(weekday);
    value.tm_yday = static_cast<int>(yearDay) - 1;
    value.tm_isdst = static_cast<int>(daylightSaving);
    return value;
}

} // namespace

extern "C" {

ZEUS_NATIVE_EXPORT void zeus_native_io_write(const char* text) noexcept {
    if (text) std::fputs(text, stdout);
}

ZEUS_NATIVE_EXPORT void zeus_native_io_flush() noexcept {
    std::fflush(stdout);
}

ZEUS_NATIVE_EXPORT const char* zeus_native_io_input() noexcept {
    static thread_local std::string line;
    try {
        line.clear();
        std::getline(std::cin, line);
        return line.c_str();
    } catch (...) {
        return nullptr;
    }
}

ZEUS_NATIVE_EXPORT int32_t zeus_native_process_arguments_size() noexcept {
    std::vector<std::string> arguments;
    if (!collectProcessArguments(arguments)) return -1;
    return serializedProcessArgumentsSize(arguments);
}

ZEUS_NATIVE_EXPORT int32_t zeus_native_process_arguments_write(
    char* destination, int32_t capacity
) noexcept {
    if (!destination || capacity < 4) return -1;
    std::vector<std::string> arguments;
    if (!collectProcessArguments(arguments)) return -1;
    const int32_t required = serializedProcessArgumentsSize(arguments);
    if (required < 4 || capacity < required) return -1;

    int32_t offset = 0;
    const int32_t count = static_cast<int32_t>(arguments.size());
    std::memcpy(destination + offset, &count, sizeof(count));
    offset += static_cast<int32_t>(sizeof(count));
    for (const std::string& argument : arguments) {
        const int32_t length = static_cast<int32_t>(argument.size());
        std::memcpy(destination + offset, &length, sizeof(length));
        offset += static_cast<int32_t>(sizeof(length));
        if (length > 0) {
            std::memcpy(destination + offset, argument.data(),
                static_cast<size_t>(length));
            offset += length;
        }
    }
    return offset;
}

ZEUS_NATIVE_EXPORT double zeus_native_time_time() noexcept {
    return std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

ZEUS_NATIVE_EXPORT int64_t zeus_native_time_time_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

ZEUS_NATIVE_EXPORT int64_t zeus_native_time_time_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

ZEUS_NATIVE_EXPORT double zeus_native_time_clock() noexcept {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
}

ZEUS_NATIVE_EXPORT double zeus_native_time_perf_counter() noexcept {
    return std::chrono::duration<double>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
}

ZEUS_NATIVE_EXPORT int64_t zeus_native_time_perf_counter_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
}

ZEUS_NATIVE_EXPORT const char* zeus_native_time_localtime(double timestamp) noexcept {
    std::tm value{};
    return convertTimestamp(timestamp, false, value)
        ? encodeTimeStruct(value) : nullptr;
}

ZEUS_NATIVE_EXPORT const char* zeus_native_time_gmtime(double timestamp) noexcept {
    std::tm value{};
    return convertTimestamp(timestamp, true, value)
        ? encodeTimeStruct(value) : nullptr;
}

ZEUS_NATIVE_EXPORT int64_t zeus_native_time_mktime(
    int64_t year,
    int64_t month,
    int64_t day,
    int64_t hour,
    int64_t minute,
    int64_t second,
    int64_t daylightSaving
) noexcept {
    std::tm value = makeTimeStruct(
        year, month, day, hour, minute, second, 0, 1, daylightSaving);
    return static_cast<int64_t>(std::mktime(&value));
}

ZEUS_NATIVE_EXPORT const char* zeus_native_time_strftime(
    const char* format,
    int64_t year,
    int64_t month,
    int64_t day,
    int64_t hour,
    int64_t minute,
    int64_t second,
    int64_t weekday,
    int64_t yearDay,
    int64_t daylightSaving
) noexcept {
    if (!format) return nullptr;
    static thread_local char output[256];
    std::tm value = makeTimeStruct(
        year, month, day, hour, minute, second,
        weekday, yearDay, daylightSaving);
    const size_t length = std::strftime(output, sizeof(output), format, &value);
    return length == 0 ? nullptr : output;
}

ZEUS_NATIVE_EXPORT const char* zeus_native_time_strptime(
    const char* text,
    const char* format
) noexcept {
    if (!text || !format) return nullptr;
    try {
        std::tm value{};
        value.tm_mday = 1;
        value.tm_isdst = -1;
        std::istringstream input(text);
        input >> std::get_time(&value, format);
        if (input.fail()) return nullptr;
        return encodeTimeStruct(value);
    } catch (...) {
        return nullptr;
    }
}

ZEUS_NATIVE_EXPORT double zeus_native_time_monotonic() noexcept {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

ZEUS_NATIVE_EXPORT int64_t zeus_native_time_monotonic_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

ZEUS_NATIVE_EXPORT void zeus_native_time_sleep(double seconds) noexcept {
    if (seconds > 0.0 && std::isfinite(seconds)) {
        std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    }
}

} // extern "C"
