#ifndef CHeaderScanner_hpp
#define CHeaderScanner_hpp

#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <cctype>
#include <algorithm>
#include <unordered_set>
#include <filesystem>

namespace ZeusParser {

    struct CConstantDeclaration {
        std::string name;
        std::string type; // "i32", "i64", "float", "double", "string", "bool"
        std::string value;
    };

    struct CFunctionParameter {
        std::string name;
        std::string zeusType;
        std::string originalType;
    };

    struct CFunctionDeclaration {
        std::string name;
        std::string zeusReturnType;
        std::string originalReturnType;
        std::vector<CFunctionParameter> parameters;
    };

    class CHeaderScanner {
        public:
            inline static std::vector<std::string> userIncludePaths;

            static void addIncludePath(const std::string& path) {
                if (!path.empty()) {
                    userIncludePaths.push_back(path);
                }
            }

            static void clearIncludePaths() {
                userIncludePaths.clear();
            }

            // 通用 C/C++ 头文件路径解析（无任何特定平台硬编码）：
            // 1. 绝对路径优先
            // 2. 导入源文件相对目录、同级及上级 include/ 与 lib/ 目录
            // 3. 当前运行工作目录 (CWD) 及 CWD/include/
            // 4. CLI -I 传入的用户包含目录
            // 5. 标准环境变量 ZEUS_INCLUDE_PATH、C_INCLUDE_PATH、CPATH
            static std::filesystem::path resolveHeaderPath(
                const std::string& headerName,
                const std::string& importerPath
            ) {
                std::filesystem::path raw(headerName);
                std::error_code ec;

                if (raw.is_absolute() && std::filesystem::exists(raw, ec)) {
                    return std::filesystem::weakly_canonical(raw, ec);
                }

                // 2. 导入源文件相对目录探测
                std::filesystem::path importerDir = std::filesystem::path(importerPath).parent_path();
                if (!importerDir.empty()) {
                    std::filesystem::path candidate = importerDir / raw;
                    if (std::filesystem::exists(candidate, ec)) {
                        return std::filesystem::weakly_canonical(candidate, ec);
                    }
                    candidate = importerDir / "include" / raw;
                    if (std::filesystem::exists(candidate, ec)) {
                        return std::filesystem::weakly_canonical(candidate, ec);
                    }
                    std::filesystem::path projectRoot = importerDir.parent_path();
                    if (!projectRoot.empty()) {
                        candidate = projectRoot / "include" / raw;
                        if (std::filesystem::exists(candidate, ec)) {
                            return std::filesystem::weakly_canonical(candidate, ec);
                        }
                        candidate = projectRoot / "lib" / raw;
                        if (std::filesystem::exists(candidate, ec)) {
                            return std::filesystem::weakly_canonical(candidate, ec);
                        }
                        candidate = projectRoot / raw;
                        if (std::filesystem::exists(candidate, ec)) {
                            return std::filesystem::weakly_canonical(candidate, ec);
                        }
                    }
                }

                // 3. 当前运行工作目录 (CWD) 及 include/
                {
                    std::filesystem::path cwd = std::filesystem::current_path();
                    std::filesystem::path candidate = cwd / raw;
                    if (std::filesystem::exists(candidate, ec)) {
                        return std::filesystem::weakly_canonical(candidate, ec);
                    }
                    candidate = cwd / "include" / raw;
                    if (std::filesystem::exists(candidate, ec)) {
                        return std::filesystem::weakly_canonical(candidate, ec);
                    }
                }

                // 4. 命令行 -I 指定的用户包含目录
                for (const auto& inc : userIncludePaths) {
                    if (inc.empty()) continue;
                    std::filesystem::path candidate = std::filesystem::path(inc) / raw;
                    if (std::filesystem::exists(candidate, ec)) {
                        return std::filesystem::weakly_canonical(candidate, ec);
                    }
                }

                // 5. 标准环境变量 ZEUS_INCLUDE_PATH / C_INCLUDE_PATH / CPATH
                auto checkEnvList = [&](const char* envName) -> std::filesystem::path {
                    const char* val = std::getenv(envName);
                    if (!val || !*val) return {};
                    std::stringstream ss(val);
                    std::string item;
                    char delimiter = ';';
#ifndef _WIN32
                    delimiter = ':';
#endif
                    while (std::getline(ss, item, delimiter)) {
                        if (!item.empty()) {
                            std::filesystem::path p = std::filesystem::path(item) / raw;
                            if (std::filesystem::exists(p, ec)) {
                                return std::filesystem::weakly_canonical(p, ec);
                            }
                        }
                    }
                    return {};
                };

                std::filesystem::path fromEnv = checkEnvList("ZEUS_INCLUDE_PATH");
                if (!fromEnv.empty()) return fromEnv;
                fromEnv = checkEnvList("C_INCLUDE_PATH");
                if (!fromEnv.empty()) return fromEnv;
                fromEnv = checkEnvList("CPATH");
                if (!fromEnv.empty()) return fromEnv;

                return {};
            }

            static std::string trim(const std::string& s) {
                size_t start = 0;
                while (start < s.size() && (std::isspace(static_cast<unsigned char>(s[start])) || s[start] == '\r')) {
                    ++start;
                }
                size_t end = s.size();
                while (end > start && (std::isspace(static_cast<unsigned char>(s[end - 1])) || s[end - 1] == '\r')) {
                    --end;
                }
                return s.substr(start, end - start);
            }

            // 将 C/C++ 类型映射到精确的 Zeus 静态类型
            // 重点遵循 32 位 MCU / C ABI 标准整数提升规则
            static std::string mapCTypeToZeus(const std::string& rawType) {
                std::string t = trim(rawType);

                auto stripWord = [](std::string& text, const std::string& word) {
                    size_t pos = 0;
                    while ((pos = text.find(word, pos)) != std::string::npos) {
                        bool leftBoundary = (pos == 0 || std::isspace(static_cast<unsigned char>(text[pos - 1])));
                        bool rightBoundary = (pos + word.size() == text.size() || 
                                              std::isspace(static_cast<unsigned char>(text[pos + word.size()])) ||
                                              text[pos + word.size()] == '*');
                        if (leftBoundary && rightBoundary) {
                            text.erase(pos, word.size());
                        } else {
                            pos += word.size();
                        }
                    }
                };

                const bool isConstChar = (t.find("const") != std::string::npos && 
                                          t.find("char") != std::string::npos && 
                                          t.find('*') != std::string::npos);
                if (isConstChar) {
                    return "string";
                }

                bool isPointer = (t.find('*') != std::string::npos);
                stripWord(t, "const");
                stripWord(t, "volatile");
                stripWord(t, "struct");
                stripWord(t, "enum");
                stripWord(t, "register");
                stripWord(t, "restrict");
                t = trim(t);

                // 移除类型内部多余连续空格 (例如 "unsigned   int" -> "unsigned int")
                std::string compact;
                bool prevSpace = false;
                for (char c : t) {
                    if (std::isspace(static_cast<unsigned char>(c))) {
                        if (!prevSpace) {
                            compact.push_back(' ');
                            prevSpace = true;
                        }
                    } else {
                        compact.push_back(c);
                        prevSpace = false;
                    }
                }
                t = trim(compact);

                if (isPointer) {
                    // 在嵌入式 32 位环境 (ESP32/ARM) 与 Zeus AOT 中，裸指针映射为 i32 原生地址
                    return "i32";
                }

                if (t == "void") return "void";

                // 64 位整数
                if (t == "int64_t" || t == "uint64_t" || t == "long long" || 
                    t == "unsigned long long" || t == "signed long long" ||
                    t == "i64" || t == "u64") {
                    return "i64";
                }

                // 32 位、16 位、8 位整数映射到 i32 (C ABI 整数提升标准)
                if (t == "int" || t == "int32_t" || t == "uint32_t" || 
                    t == "int16_t" || t == "uint16_t" || t == "int8_t" || t == "uint8_t" ||
                    t == "size_t" || t == "ssize_t" || t == "long" || t == "unsigned long" ||
                    t == "short" || t == "unsigned short" || t == "unsigned int" || t == "signed int" ||
                    t == "unsigned" || t == "signed" || t == "byte" ||
                    t == "i32" || t == "u32" || t == "i16" || t == "u16" || t == "i8" || t == "u8") {
                    return "i32";
                }

                if (t == "float") return "float";
                if (t == "double" || t == "long double") return "double";
                if (t == "bool" || t == "_Bool" || t == "boolean") return "bool";
                if (t == "char" || t == "signed char" || t == "unsigned char") return "i32";

                // 默认兜底为 i32 (适用于大多数 typedef/句柄如 esp_err_t, wl_status_t)
                return "i32";
            }

            // 剥除注释，同时保留行结构以供宏扫描
            static std::string stripComments(const std::string& source) {
                std::string result;
                result.reserve(source.size());
                size_t i = 0;
                const size_t n = source.size();
                bool inBlockComment = false;
                bool inLineComment = false;

                while (i < n) {
                    if (inBlockComment) {
                        if (i + 1 < n && source[i] == '*' && source[i + 1] == '/') {
                            inBlockComment = false;
                            i += 2;
                        } else {
                            if (source[i] == '\n') result.push_back('\n');
                            ++i;
                        }
                        continue;
                    }

                    if (inLineComment) {
                        if (source[i] == '\n') {
                            inLineComment = false;
                            result.push_back('\n');
                        }
                        ++i;
                        continue;
                    }

                    if (i + 1 < n && source[i] == '/' && source[i + 1] == '*') {
                        inBlockComment = true;
                        i += 2;
                        continue;
                    }

                    if (i + 1 < n && source[i] == '/' && source[i + 1] == '/') {
                        inLineComment = true;
                        i += 2;
                        continue;
                    }

                    result.push_back(source[i]);
                    ++i;
                }
                return result;
            }

            // 扫描 #define 宏常量 (忽略带参宏)
            static void scanDefineDirectives(
                const std::string& cleanSource,
                std::vector<CConstantDeclaration>& constants,
                std::unordered_set<std::string>& seenConstants
            ) {
                std::istringstream stream(cleanSource);
                std::string line;
                while (std::getline(stream, line)) {
                    std::string trimmed = trim(line);
                    if (trimmed.rfind("#define", 0) != 0) continue;

                    size_t pos = 7; // after "#define"
                    while (pos < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[pos]))) {
                        ++pos;
                    }
                    if (pos >= trimmed.size()) continue;

                    // 提取宏名字
                    size_t nameStart = pos;
                    while (pos < trimmed.size() && (std::isalnum(static_cast<unsigned char>(trimmed[pos])) || trimmed[pos] == '_')) {
                        ++pos;
                    }
                    std::string macroName = trimmed.substr(nameStart, pos - nameStart);
                    if (macroName.empty()) continue;

                    // 如果宏名后面紧跟 '(' 说明是带参宏，直接跳过
                    if (pos < trimmed.size() && trimmed[pos] == '(') continue;

                    // 提取宏的值
                    std::string rawVal = trim(trimmed.substr(pos));
                    if (rawVal.empty()) continue;

                    // 去掉外层括号，如 (1024) -> 1024
                    if (rawVal.front() == '(' && rawVal.back() == ')') {
                        rawVal = trim(rawVal.substr(1, rawVal.size() - 2));
                    }

                    // 去掉常见单片机类型强转如 (uint8_t)1, (int)0
                    if (rawVal.front() == '(') {
                        size_t closeParen = rawVal.find(')');
                        if (closeParen != std::string::npos) {
                            std::string afterCast = trim(rawVal.substr(closeParen + 1));
                            if (!afterCast.empty()) rawVal = afterCast;
                        }
                    }

                    // 去除整型后缀，如 100UL, 1000LL, 0xFFU
                    auto stripLiteralSuffix = [](std::string& s) {
                        while (!s.empty()) {
                            char c = s.back();
                            if (c == 'u' || c == 'U' || c == 'l' || c == 'L' || c == 'f' || c == 'F') {
                                s.pop_back();
                            } else {
                                break;
                            }
                        }
                    };

                    std::string constType;
                    std::string cleanVal = rawVal;

                    if (cleanVal.front() == '"' && cleanVal.back() == '"') {
                        constType = "string";
                    } else if (cleanVal == "true" || cleanVal == "false") {
                        constType = "bool";
                    } else {
                        stripLiteralSuffix(cleanVal);
                        cleanVal = trim(cleanVal);
                        if (cleanVal.empty()) continue;

                        // 判断是否为有效数字 (支持 0x十六进制, 负数, 浮点)
                        bool isHex = (cleanVal.rfind("0x", 0) == 0 || cleanVal.rfind("0X", 0) == 0);
                        bool isFloat = (!isHex && cleanVal.find('.') != std::string::npos);
                        bool isValidNumber = true;

                        if (isHex) {
                            for (size_t k = 2; k < cleanVal.size(); ++k) {
                                if (!std::isxdigit(static_cast<unsigned char>(cleanVal[k]))) {
                                    isValidNumber = false;
                                    break;
                                }
                            }
                            if (isValidNumber) {
                                try {
                                    int64_t parsedHex = std::stoll(cleanVal, nullptr, 16);
                                    cleanVal = std::to_string(parsedHex);
                                } catch (...) {
                                    isValidNumber = false;
                                }
                            }
                        } else {
                            size_t startIdx = (cleanVal[0] == '-' || cleanVal[0] == '+') ? 1 : 0;
                            if (startIdx >= cleanVal.size()) isValidNumber = false;
                            for (size_t k = startIdx; k < cleanVal.size(); ++k) {
                                if (!std::isdigit(static_cast<unsigned char>(cleanVal[k])) && cleanVal[k] != '.') {
                                    isValidNumber = false;
                                    break;
                                }
                            }
                        }

                        if (!isValidNumber) continue;

                        constType = isFloat ? "float" : "i32";
                    }

                    if (seenConstants.insert(macroName).second) {
                        constants.push_back(CConstantDeclaration{macroName, constType, cleanVal});
                    }
                }
            }

            // 扫描 enum 枚举常量
            static void scanEnums(
                const std::string& cleanSource,
                std::vector<CConstantDeclaration>& constants,
                std::unordered_set<std::string>& seenConstants
            ) {
                size_t pos = 0;
                while ((pos = cleanSource.find("enum", pos)) != std::string::npos) {
                    bool leftBoundary = (pos == 0 || std::isspace(static_cast<unsigned char>(cleanSource[pos - 1])));
                    bool rightBoundary = (pos + 4 >= cleanSource.size() || 
                                          std::isspace(static_cast<unsigned char>(cleanSource[pos + 4])) ||
                                          cleanSource[pos + 4] == '{');
                    if (!leftBoundary || !rightBoundary) {
                        pos += 4;
                        continue;
                    }

                    size_t openBrace = cleanSource.find('{', pos);
                    if (openBrace == std::string::npos) break;

                    // 检查 enum 到 '{' 之间是否有分号 (前向声明 enum Foo;)
                    std::string between = cleanSource.substr(pos, openBrace - pos);
                    if (between.find(';') != std::string::npos) {
                        pos = openBrace;
                        continue;
                    }

                    size_t closeBrace = cleanSource.find('}', openBrace);
                    if (closeBrace == std::string::npos) break;

                    std::string enumBody = cleanSource.substr(openBrace + 1, closeBrace - openBrace - 1);
                    std::istringstream es(enumBody);
                    std::string item;
                    int64_t currentEnumValue = 0;

                    while (std::getline(es, item, ',')) {
                        item = trim(item);
                        if (item.empty()) continue;

                        size_t eqPos = item.find('=');
                        std::string itemName;
                        std::string itemValStr;

                        if (eqPos != std::string::npos) {
                            itemName = trim(item.substr(0, eqPos));
                            itemValStr = trim(item.substr(eqPos + 1));
                            bool parsedOk = false;
                            try {
                                if (itemValStr.rfind("0x", 0) == 0 || itemValStr.rfind("0X", 0) == 0) {
                                    currentEnumValue = std::stoll(itemValStr, nullptr, 16);
                                    itemValStr = std::to_string(currentEnumValue);
                                    parsedOk = true;
                                } else {
                                    currentEnumValue = std::stoll(itemValStr);
                                    itemValStr = std::to_string(currentEnumValue);
                                    parsedOk = true;
                                }
                            } catch (...) {
                                parsedOk = false;
                            }
                            if (!parsedOk) {
                                continue;
                            }
                        } else {
                            itemName = item;
                            itemValStr = std::to_string(currentEnumValue);
                        }

                        // 验证标识符合法性
                        if (!itemName.empty() && (std::isalpha(static_cast<unsigned char>(itemName[0])) || itemName[0] == '_')) {
                            if (seenConstants.insert(itemName).second) {
                                constants.push_back(CConstantDeclaration{itemName, "i32", itemValStr});
                            }
                            currentEnumValue++;
                        }
                    }

                    pos = closeBrace + 1;
                }
            }

            // 清理编译器平台属性与调用修饰符噪声
            static std::string stripNoisyAttributes(const std::string& input) {
                std::string s = input;

                // 移除 __attribute__((...))
                size_t attrPos = 0;
                while ((attrPos = s.find("__attribute__", attrPos)) != std::string::npos) {
                    size_t p = s.find('(', attrPos);
                    if (p != std::string::npos) {
                        int depth = 0;
                        size_t endP = p;
                        while (endP < s.size()) {
                            if (s[endP] == '(') depth++;
                            else if (s[endP] == ')') {
                                depth--;
                                if (depth == 0) {
                                    endP++;
                                    break;
                                }
                            }
                            endP++;
                        }
                        s.erase(attrPos, endP - attrPos);
                    } else {
                        break;
                    }
                }

                // 移除单片机及标准常见修饰符
                static const std::vector<std::string> noisyTokens = {
                    "extern \"C\"", "extern", "inline", "static", "__inline__", "__inline",
                    "IRAM_ATTR", "ICACHE_RAM_ATTR", "ARDUINO_API", "DRAM_ATTR", "RTC_DATA_ATTR",
                    "NOINLINE_ATTR", "__cdecl", "__stdcall", "__fastcall", "ESP_OK"
                };

                for (const auto& token : noisyTokens) {
                    size_t pos = 0;
                    while ((pos = s.find(token, pos)) != std::string::npos) {
                        bool leftBoundary = (pos == 0 || std::isspace(static_cast<unsigned char>(s[pos - 1])));
                        bool rightBoundary = (pos + token.size() == s.size() || 
                                              std::isspace(static_cast<unsigned char>(s[pos + token.size()])));
                        if (leftBoundary && rightBoundary) {
                            s.erase(pos, token.size());
                        } else {
                            pos += token.size();
                        }
                    }
                }

                return trim(s);
            }

            // 扫描单一头文件内容并递归解析引用到的子头文件 (#include "...")
            static bool scanHeaderInternal(
                const std::string& headerPath,
                std::vector<CConstantDeclaration>& constants,
                std::vector<CFunctionDeclaration>& declarations,
                std::unordered_set<std::string>& seenConstants,
                std::unordered_set<std::string>& seenFunctions,
                std::unordered_set<std::string>& visitedFiles,
                int currentDepth,
                int maxDepth,
                std::string& error
            ) {
                std::error_code ec;
                std::string normPath;
                try {
                    normPath = std::filesystem::weakly_canonical(std::filesystem::path(headerPath), ec).string();
                } catch (...) {
                    normPath = headerPath;
                }
                if (normPath.empty()) normPath = headerPath;

                if (!visitedFiles.insert(normPath).second) {
                    return true; // 已扫描过，防止递归环路
                }

                std::ifstream file(headerPath, std::ios::binary);
                if (!file.is_open()) {
                    if (currentDepth == 0) {
                        error = "Could not open C/C++ header file: " + headerPath;
                        return false;
                    }
                    return true; // 子头文件缺失时平滑跳过
                }
                std::ostringstream ss;
                ss << file.rdbuf();
                const std::string rawSource = ss.str();
                const std::string cleanSource = stripComments(rawSource);

                // 1. 扫描当前头文件内部的 #define 常量
                scanDefineDirectives(cleanSource, constants, seenConstants);

                // 2. 扫描当前头文件内部的 enum 枚举常量
                scanEnums(cleanSource, constants, seenConstants);

                // 3. 扫描当前头文件内部的函数声明
                std::string currentDecl;
                for (size_t i = 0; i < cleanSource.size(); ++i) {
                    const char c = cleanSource[i];

                    // 跳过预处理指令行进入语句解析
                    if (c == '#') {
                        while (i < cleanSource.size() && cleanSource[i] != '\n') {
                            ++i;
                        }
                        continue;
                    }

                    if (c == ';') {
                        std::vector<CFunctionDeclaration> singleList;
                        parseSingleDeclaration(currentDecl, singleList);
                        for (auto& decl : singleList) {
                            if (seenFunctions.insert(decl.name).second) {
                                declarations.push_back(std::move(decl));
                            }
                        }
                        currentDecl.clear();
                    } else if (c == '{') {
                        // 跳过内联函数体或 struct/class 定义体
                        std::string trimmed = trim(currentDecl);
                        if (trimmed.find("extern") != std::string::npos && trimmed.find("\"C\"") != std::string::npos) {
                            currentDecl.clear();
                        } else {
                            int depth = 1;
                            ++i;
                            while (i < cleanSource.size() && depth > 0) {
                                if (cleanSource[i] == '{') ++depth;
                                else if (cleanSource[i] == '}') --depth;
                                ++i;
                            }
                            --i;
                            currentDecl.clear();
                        }
                    } else if (c == '}') {
                        currentDecl.clear();
                    } else {
                        currentDecl.push_back(c);
                    }
                }

                // 4. 递归扫描当前文件包含的 #include "..." 子头文件 (双引号头文件，排除 <...> 标准库)
                if (currentDepth < maxDepth) {
                    std::istringstream stream(cleanSource);
                    std::string line;
                    std::filesystem::path currentDir = std::filesystem::path(headerPath).parent_path();

                    while (std::getline(stream, line)) {
                        std::string trimmed = trim(line);
                        if (trimmed.rfind("#include", 0) != 0) continue;

                        size_t q1 = trimmed.find('"');
                        size_t q2 = (q1 != std::string::npos) ? trimmed.find('"', q1 + 1) : std::string::npos;
                        if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1 + 1) {
                            std::string subHeader = trimmed.substr(q1 + 1, q2 - q1 - 1);
                            std::filesystem::path subPath = currentDir / subHeader;
                            if (!std::filesystem::exists(subPath, ec)) {
                                subPath = resolveHeaderPath(subHeader, headerPath);
                            }
                            if (!subPath.empty() && std::filesystem::exists(subPath, ec)) {
                                std::string subError;
                                scanHeaderInternal(
                                    subPath.string(), constants, declarations,
                                    seenConstants, seenFunctions, visitedFiles,
                                    currentDepth + 1, maxDepth, subError
                                );
                            }
                        }
                    }
                }

                return true;
            }

            // 扫描头文件入口：支持递归追踪所有子头文件
            static bool scanHeader(
                const std::string& headerPath,
                std::vector<CConstantDeclaration>& constants,
                std::vector<CFunctionDeclaration>& declarations,
                std::string& error
            ) {
                std::unordered_set<std::string> seenConstants;
                std::unordered_set<std::string> seenFunctions;
                std::unordered_set<std::string> visitedFiles;
                return scanHeaderInternal(
                    headerPath, constants, declarations,
                    seenConstants, seenFunctions, visitedFiles,
                    0, 16, error
                );
            }

            // 保持向后兼容的 scanHeader 重载
            static bool scanHeader(
                const std::string& headerPath,
                std::vector<CFunctionDeclaration>& declarations,
                std::string& error
            ) {
                std::vector<CConstantDeclaration> constants;
                return scanHeader(headerPath, constants, declarations, error);
            }

            // 解析单条 C 函数声明
            static void parseSingleDeclaration(
                const std::string& rawStmt,
                std::vector<CFunctionDeclaration>& declarations
            ) {
                std::string stmt = stripNoisyAttributes(rawStmt);
                if (stmt.empty()) return;

                // 忽略 typedef 声明
                if (stmt.rfind("typedef", 0) == 0) return;

                const size_t parenOpen = stmt.find('(');
                const size_t parenClose = stmt.rfind(')');
                if (parenOpen == std::string::npos || parenClose == std::string::npos || parenClose <= parenOpen) {
                    return;
                }

                std::string beforeParen = trim(stmt.substr(0, parenOpen));
                if (beforeParen.empty()) return;

                // 拆分返回类型与函数名称
                size_t namePos = beforeParen.size();
                while (namePos > 0 && (std::isalnum(static_cast<unsigned char>(beforeParen[namePos - 1])) ||
                                       beforeParen[namePos - 1] == '_')) {
                    --namePos;
                }

                if (namePos == beforeParen.size() || namePos == 0) return;

                std::string funcName = beforeParen.substr(namePos);
                std::string rawRetType = trim(beforeParen.substr(0, namePos));
                if (rawRetType.empty()) rawRetType = "void";

                // 解析形参列表
                std::string paramsStr = trim(stmt.substr(parenOpen + 1, parenClose - parenOpen - 1));
                std::vector<CFunctionParameter> params;

                if (!paramsStr.empty() && paramsStr != "void") {
                    std::vector<std::string> rawParams;
                    size_t pstart = 0;
                    int pDepth = 0;
                    for (size_t k = 0; k < paramsStr.size(); ++k) {
                        if (paramsStr[k] == '(') ++pDepth;
                        else if (paramsStr[k] == ')') --pDepth;
                        else if (paramsStr[k] == ',' && pDepth == 0) {
                            rawParams.push_back(paramsStr.substr(pstart, k - pstart));
                            pstart = k + 1;
                        }
                    }
                    rawParams.push_back(paramsStr.substr(pstart));

                    int anonymousParamIndex = 0;
                    for (auto& rawParam : rawParams) {
                        std::string p = trim(rawParam);
                        if (p.empty() || p == "...") continue;

                        // 剔除形参默认值，如 int mode = 0 -> int mode
                        size_t eqPos = p.find('=');
                        if (eqPos != std::string::npos) {
                            p = trim(p.substr(0, eqPos));
                        }

                        // 查找参数名
                        size_t pNamePos = p.size();
                        while (pNamePos > 0 && (std::isalnum(static_cast<unsigned char>(p[pNamePos - 1])) ||
                                                p[pNamePos - 1] == '_')) {
                            --pNamePos;
                        }

                        std::string paramName;
                        std::string paramType;
                        if (pNamePos == p.size()) {
                            // 匿名参数，如 void func(int, float);
                            paramName = "arg" + std::to_string(anonymousParamIndex++);
                            paramType = p;
                        } else {
                            paramName = trim(p.substr(pNamePos));
                            paramType = trim(p.substr(0, pNamePos));
                            if (paramType.empty()) {
                                paramType = paramName;
                                paramName = "arg" + std::to_string(anonymousParamIndex++);
                            }
                        }

                        CFunctionParameter cp;
                        cp.name = paramName;
                        cp.originalType = paramType;
                        cp.zeusType = mapCTypeToZeus(paramType);
                        params.push_back(cp);
                    }
                }

                CFunctionDeclaration decl;
                decl.name = funcName;
                decl.originalReturnType = rawRetType;
                decl.zeusReturnType = mapCTypeToZeus(rawRetType);
                decl.parameters = std::move(params);
                declarations.push_back(std::move(decl));
            }

            // 生成完整的 Zeus 模块虚拟源码
            static std::string generateZeusDeclarations(
                const std::vector<CConstantDeclaration>& constants,
                const std::vector<CFunctionDeclaration>& declarations
            ) {
                std::ostringstream out;
                out << "@static;\n\n";

                // 1. 发射常量定义
                for (const auto& c : constants) {
                    out << "let " << c.name << ": " << c.type << " = " << c.value << ";\n";
                }
                if (!constants.empty()) {
                    out << "\n";
                }

                // 2. 发射函数原生接口
                for (const auto& decl : declarations) {
                    out << "@native(\"" << decl.name << "\") decl func " << decl.name << "(";
                    for (size_t i = 0; i < decl.parameters.size(); ++i) {
                        if (i > 0) out << ", ";
                        out << decl.parameters[i].name << ": " << decl.parameters[i].zeusType;
                    }
                    out << ")";
                    if (!decl.zeusReturnType.empty() && decl.zeusReturnType != "void") {
                        out << " -> " << decl.zeusReturnType;
                    } else {
                        out << " -> void";
                    }
                    out << ";\n";
                }
                return out.str();
            }

            // 保持向后兼容的 generateZeusDeclarations 重载
            static std::string generateZeusDeclarations(
                const std::vector<CFunctionDeclaration>& declarations
            ) {
                return generateZeusDeclarations({}, declarations);
            }
    };

} // namespace ZeusParser

#endif // CHeaderScanner_hpp
