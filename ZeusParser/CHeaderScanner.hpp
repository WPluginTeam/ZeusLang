#ifndef CHeaderScanner_hpp
#define CHeaderScanner_hpp

#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <cctype>
#include <algorithm>

namespace ZeusParser {

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

            static std::string mapCTypeToZeus(const std::string& rawType) {
                std::string t = trim(rawType);
                // Remove const / volatile / unsigned / signed qualifiers
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

                const bool isConstChar = (t.find("const") != std::string::npos && t.find("char") != std::string::npos && t.find('*') != std::string::npos);
                if (isConstChar) {
                    return "string";
                }

                bool isPointer = (t.find('*') != std::string::npos);
                stripWord(t, "const");
                stripWord(t, "volatile");
                stripWord(t, "struct");
                t = trim(t);

                if (isPointer) {
                    // C pointers in Zeus AOT map to native integer address (@faddr compatible)
                    return "int";
                }

                if (t == "void") return "void";
                if (t == "int" || t == "int32_t" || t == "int64_t" || t == "int16_t" || t == "int8_t" ||
                    t == "uint32_t" || t == "uint64_t" || t == "uint16_t" || t == "uint8_t" ||
                    t == "size_t" || t == "ssize_t" || t == "long" || t == "short" ||
                    t == "unsigned int" || t == "signed int" || t == "unsigned" || t == "signed" ||
                    t == "unsigned long" || t == "unsigned short" || t == "long long" || t == "unsigned long long") {
                    return "int";
                }
                if (t == "float" || t == "double") return "float";
                if (t == "bool" || t == "_Bool") return "bool";
                if (t == "char") return "char";

                return "int";
            }

            static std::string stripCommentsAndDirectives(const std::string& source) {
                std::string result;
                result.reserve(source.size());
                size_t i = 0;
                const size_t n = source.size();
                bool inBlockComment = false;
                bool inLineComment = false;
                bool inDirective = false;

                while (i < n) {
                    if (inBlockComment) {
                        if (i + 1 < n && source[i] == '*' && source[i + 1] == '/') {
                            inBlockComment = false;
                            i += 2;
                        } else {
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

                    if (inDirective) {
                        if (source[i] == '\\' && i + 1 < n && (source[i + 1] == '\r' || source[i + 1] == '\n')) {
                            // Line continuation
                            i += (source[i + 1] == '\r' && i + 2 < n && source[i + 2] == '\n') ? 3 : 2;
                            continue;
                        }
                        if (source[i] == '\n') {
                            inDirective = false;
                            result.push_back('\n');
                        }
                        ++i;
                        continue;
                    }

                    // Check comments
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
                    if (source[i] == '#') {
                        inDirective = true;
                        ++i;
                        continue;
                    }

                    result.push_back(source[i]);
                    ++i;
                }
                return result;
            }

            static bool scanHeader(
                const std::string& headerPath,
                std::vector<CFunctionDeclaration>& declarations,
                std::string& error
            ) {
                std::ifstream file(headerPath, std::ios::binary);
                if (!file.is_open()) {
                    error = "Could not open C/C++ header file: " + headerPath;
                    return false;
                }
                std::ostringstream ss;
                ss << file.rdbuf();
                const std::string cleanSource = stripCommentsAndDirectives(ss.str());

                // Tokenize by semicolons, braces
                std::string currentDecl;
                for (size_t i = 0; i < cleanSource.size(); ++i) {
                    const char c = cleanSource[i];
                    if (c == ';') {
                        parseSingleDeclaration(currentDecl, declarations);
                        currentDecl.clear();
                    } else if (c == '{') {
                        // If it's extern "C" {, we ignore the brace. If it's a function body, skip to matching }
                        std::string trimmed = trim(currentDecl);
                        if (trimmed.find("extern") != std::string::npos && trimmed.find("\"C\"") != std::string::npos) {
                            currentDecl.clear();
                        } else {
                            // Skip inline function body
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
                return true;
            }

            static void parseSingleDeclaration(
                const std::string& rawStmt,
                std::vector<CFunctionDeclaration>& declarations
            ) {
                std::string stmt = trim(rawStmt);
                if (stmt.empty()) return;

                // Strip leading extern "C" or extern
                auto stripPrefix = [](std::string& s, const std::string& prefix) {
                    s = trim(s);
                    if (s.rfind(prefix, 0) == 0) {
                        s = trim(s.substr(prefix.size()));
                        return true;
                    }
                    return false;
                };

                stripPrefix(stmt, "extern \"C\"");
                stripPrefix(stmt, "extern");
                stripPrefix(stmt, "inline");
                stripPrefix(stmt, "static");
                stripPrefix(stmt, "__inline__");
                stripPrefix(stmt, "__inline");

                const size_t parenOpen = stmt.find('(');
                const size_t parenClose = stmt.rfind(')');
                if (parenOpen == std::string::npos || parenClose == std::string::npos || parenClose <= parenOpen) {
                    return;
                }

                std::string beforeParen = trim(stmt.substr(0, parenOpen));
                if (beforeParen.empty()) return;

                // Split beforeParen into return type and function name
                size_t namePos = beforeParen.size();
                while (namePos > 0 && (std::isalnum(static_cast<unsigned char>(beforeParen[namePos - 1])) ||
                                       beforeParen[namePos - 1] == '_')) {
                    --namePos;
                }

                if (namePos == beforeParen.size() || namePos == 0) return;

                std::string funcName = beforeParen.substr(namePos);
                std::string rawRetType = trim(beforeParen.substr(0, namePos));
                if (rawRetType.empty()) rawRetType = "void";

                // Parse parameter list
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

                        // Find param name
                        size_t pNamePos = p.size();
                        while (pNamePos > 0 && (std::isalnum(static_cast<unsigned char>(p[pNamePos - 1])) ||
                                                p[pNamePos - 1] == '_')) {
                            --pNamePos;
                        }

                        std::string paramName;
                        std::string paramType;
                        if (pNamePos == p.size()) {
                            // Anonymous parameter
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

            static std::string generateZeusDeclarations(
                const std::vector<CFunctionDeclaration>& declarations
            ) {
                std::ostringstream out;
                for (const auto& decl : declarations) {
                    out << "@native(\"" << decl.name << "\") decl func " << decl.name << "(";
                    for (size_t i = 0; i < decl.parameters.size(); ++i) {
                        if (i > 0) out << ", ";
                        out << decl.parameters[i].name << ": " << decl.parameters[i].zeusType;
                    }
                    out << ")";
                    if (!decl.zeusReturnType.empty()) {
                        out << " -> " << decl.zeusReturnType;
                    }
                    out << ";\n";
                }
                return out.str();
            }
    };

} // namespace ZeusParser

#endif // CHeaderScanner_hpp
