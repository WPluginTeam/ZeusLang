#ifndef ZeusPackageLoader_hpp
#define ZeusPackageLoader_hpp

#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <map>
#include <set>
#include <fstream>
#include <sstream>
#include "./ZeusParser/LexerParser.hpp"
#include "./ZeusParser/TreeParser.hpp"
#include "./ZeusParser/BackendBlockSelector.hpp"
#include "./ZeusParser/SyntaxValidator.hpp"
#include "./ZeusParser/InterfaceConformance.hpp"
#include "./ZeusTyping/TypeChecker.hpp"
#include "./ZeusIR/IRGenerator.hpp"
#include "./ZeusIR/IRImport.hpp"
#include "./ZeusIR/IRScope.hpp"
#include "./ZeusDebug/Utils.hpp"
#include "./ZeusRuntimeLib/ZeusVMException.hpp"

#define ZEUS_DEBUG_FILE_CHANNEL ::ZeusDebug::CH_PackageLoader

namespace ZeusPackage {

    inline void relocateTypeImplPrototypes(IRType::IRInstr& instr, int prototypeOffset) {
        if (instr.op != VMOperator::TYPE_IMPL_BUILD) return;
        size_t cursor = 1;
        for (int group = 0; group < 3; ++group) {
            if (cursor >= instr.args.size() ||
                !std::holds_alternative<LexerType::Value>(instr.args[cursor])) {
                throw std::runtime_error("Malformed TYPE_IMPL_BUILD method count");
            }
            const int count = VMHelpers::getIntFromValue(
                std::get<LexerType::Value>(instr.args[cursor++]));
            if (count < 0 || static_cast<size_t>(count) >
                    (instr.args.size() - cursor) / 2) {
                throw std::runtime_error("Malformed TYPE_IMPL_BUILD method entries");
            }
            for (int method = 0; method < count; ++method) {
                if (!std::holds_alternative<std::string>(instr.args[cursor]) ||
                    !std::holds_alternative<LexerType::Value>(instr.args[cursor + 1])) {
                    throw std::runtime_error("Malformed TYPE_IMPL_BUILD method entry");
                }
                auto& value = std::get<LexerType::Value>(instr.args[cursor + 1]);
                value.data = ZeusType::BigInteger(
                    VMHelpers::getIntFromValue(value) + prototypeOffset);
                cursor += 2;
            }
        }
        if (cursor != instr.args.size()) {
            throw std::runtime_error("Unexpected TYPE_IMPL_BUILD method metadata");
        }
    }

    // 包编译结果
    struct CompiledPackage {
        std::string packageName;
        std::string filePath;
        std::vector<std::vector<IRType::IRInstr>> chunks;
        std::vector<std::string> globalNames;
        std::vector<ZeusDiagnostics::Diagnostic> warnings;
        std::string sourceCode;
        std::set<std::string> functionNames;
        ZeusParser::InterfaceContractRegistry interfaceContracts;
        std::map<int, std::string> namespaceAliasSentinels;
        std::map<int, int> importedGlobalSlotSentinels;
        int globalStartIndex;  // 包的全局变量起始索引
        size_t compileTimeGlobalsOffset = 0;  // IRScope::globals.size() before compiling this package
        int sourceFileId = 0;  // 源文件ID，用于错误报告
    };

    // Import信息结构体
    struct ImportInfo {
        std::string packageName;
        std::string alias;        // 别名(可能为空)
        int importMode;           // 0=global, 1=namespace
    };

    struct PackageLoadGraph {
        std::vector<std::string> dependencyOrder;
        std::map<std::string, std::vector<ImportInfo>> importsByPackage;
        std::map<std::string, std::string> pathsByPackage;
        std::string error;

        bool ok() const { return error.empty(); }
    };

    inline std::string defaultImportAlias(const ImportInfo& info) {
        if (!info.alias.empty()) return info.alias;
        const size_t dot = info.packageName.find_last_of('.');
        return dot == std::string::npos ? info.packageName : info.packageName.substr(dot + 1);
    }

    inline std::string moduleNamespaceGlobalName(const std::string& ownerPackage, const std::string& alias) {
        return "__zeus_module_import__" + ownerPackage + "::" + alias;
    }

    inline std::map<std::string, std::string> packageFieldInitializerNameRemap(
        const std::vector<std::vector<IRType::IRInstr>>& chunks,
        const std::string& packageName
    ) {
        std::map<std::string, std::string> remap;
        if (packageName.empty()) return remap;
        const std::string packagePrefix = packageName + ".";
        for (const auto& chunk : chunks) {
            for (const auto& instruction : chunk) {
                if (instruction.metadata.kind !=
                    IRType::IRMetadataKind::ClassLayout) {
                    continue;
                }
                std::string className;
                std::vector<std::string> fieldNames;
                std::vector<std::string> fieldTypes;
                std::vector<std::string> initializerNames;
                if (!instruction.metadata.readClassLayout(
                        className, fieldNames, fieldTypes, nullptr, nullptr,
                        nullptr, nullptr, &initializerNames)) {
                    continue;
                }
                for (const auto& initializerName : initializerNames) {
                    if (initializerName.empty() ||
                        initializerName.compare(
                            0, packagePrefix.size(), packagePrefix) == 0) {
                        continue;
                    }
                    remap.emplace(initializerName,
                                  packagePrefix + initializerName);
                }
            }
        }
        return remap;
    }

    inline void remapPackageFieldInitializerName(
        IRType::IRInstr& instruction,
        const std::map<std::string, std::string>& remap
    ) {
        if (remap.empty()) return;
        if (instruction.metadata.kind == IRType::IRMetadataKind::ClassLayout) {
            for (auto& initializerName :
                 instruction.metadata.fieldInitializerNames) {
                const auto replacement = remap.find(initializerName);
                if (replacement != remap.end()) {
                    initializerName = replacement->second;
                }
            }
        }

        const auto replacement = remap.find(instruction.metadata.symbol.name);
        if (replacement == remap.end() ||
            instruction.metadata.symbol.kind != IRType::SymbolRefKind::Function) {
            return;
        }
        instruction.metadata.symbol.name = replacement->second;
        if ((instruction.metadata.kind == IRType::IRMetadataKind::FunctionSignature ||
             instruction.metadata.kind ==
                 IRType::IRMetadataKind::ExternalFunctionSignature) &&
            instruction.args.size() > 1 &&
            std::holds_alternative<std::string>(instruction.args[1])) {
            instruction.args[1] = replacement->second;
        }
    }

    inline std::vector<ImportInfo> extractImportDeclarations(const std::string& source) {
        const auto tokens = LexerParser::parse(source);
        std::vector<ImportInfo> imports;

        for (size_t i = 0; i < tokens.size(); ++i) {
            if (!std::holds_alternative<LexerType::Keyword>(tokens[i]) ||
                std::get<LexerType::Keyword>(tokens[i]).name != "import") {
                continue;
            }

            ImportInfo info;
            info.importMode = 0;
            bool inBracket = false;
            bool expectAlias = false;

            for (size_t j = i + 1; j < tokens.size(); ++j) {
                if (std::holds_alternative<LexerType::Operator>(tokens[j])) {
                    const std::string op = std::get<LexerType::Operator>(tokens[j]).toString();
                    if (op == ";") break;
                    if (op == "." && !inBracket) {
                        info.packageName += ".";
                    } else if (op == "[") {
                        inBracket = true;
                    } else if (op == "]") {
                        inBracket = false;
                        expectAlias = false;
                    } else if (op == "&" && inBracket) {
                        info.importMode = 1;
                        expectAlias = true;
                    }
                    continue;
                }

                std::string tokenName;
                if (std::holds_alternative<LexerType::Keyword>(tokens[j])) {
                    tokenName = std::get<LexerType::Keyword>(tokens[j]).name;
                } else if (std::holds_alternative<LexerType::Value>(tokens[j])) {
                    tokenName = std::get<LexerType::Value>(tokens[j]).toString();
                } else {
                    if (!info.packageName.empty()) break;
                    continue;
                }

                if (inBracket) {
                    if (expectAlias) {
                        info.alias = tokenName;
                        expectAlias = false;
                    }
                } else {
                    info.packageName += tokenName;
                }
            }

            if (!info.packageName.empty()) {
                info.packageName = IRImport::canonicalPackageName(info.packageName);
                imports.push_back(std::move(info));
            }
        }

        return imports;
    }

    inline bool visitPackageDependencies(
        std::string packageName,
        const std::string& importingSourcePath,
        PackageLoadGraph& graph,
        std::map<std::string, int>& visitState,
        std::vector<std::string>& visitStack) {
        packageName = IRImport::canonicalPackageName(packageName);
        const auto state = visitState.find(packageName);
        if (state != visitState.end() && state->second == 2) return true;
        if (state != visitState.end() && state->second == 1) {
            std::ostringstream cycle;
            cycle << "Package import cycle: ";
            auto start = std::find(visitStack.begin(), visitStack.end(), packageName);
            for (auto it = start; it != visitStack.end(); ++it) {
                if (it != start) cycle << " -> ";
                cycle << *it;
            }
            cycle << " -> " << packageName;
            graph.error = cycle.str();
            return false;
        }

        IRImport::setWorkingDirectory(importingSourcePath);
        const std::string packagePath = IRImport::loadPackage(packageName);
        if (packagePath.empty()) {
            graph.error = "Package not found while resolving imports: " + packageName +
                " (imported from " + importingSourcePath + ")";
            return false;
        }

        std::ifstream packageFile(packagePath);
        if (!packageFile.is_open()) {
            graph.error = "Failed to open package file while resolving imports: " + packagePath;
            return false;
        }
        std::stringstream packageBuffer;
        packageBuffer << packageFile.rdbuf();

        visitState[packageName] = 1;
        visitStack.push_back(packageName);
        graph.pathsByPackage[packageName] = packagePath;
        auto imports = extractImportDeclarations(packageBuffer.str());
        graph.importsByPackage[packageName] = imports;

        for (const auto& imported : imports) {
            if (!visitPackageDependencies(imported.packageName, packagePath, graph, visitState, visitStack)) {
                return false;
            }
        }

        visitStack.pop_back();
        visitState[packageName] = 2;
        graph.dependencyOrder.push_back(packageName);
        return true;
    }

    inline PackageLoadGraph buildPackageLoadGraph(
        const std::vector<std::string>& rootPackages,
        const std::string& rootSourcePath) {
        PackageLoadGraph graph;
        std::map<std::string, int> visitState;
        std::vector<std::string> visitStack;
        for (const auto& packageName : rootPackages) {
            if (!visitPackageDependencies(packageName, rootSourcePath, graph, visitState, visitStack)) break;
        }
        return graph;
    }

    // ĺˇ˛ĺ č˝˝çĺçźĺ­?
    static std::unordered_map<std::string, CompiledPackage> loadedPackages;

    // çźčŻĺćäť?
    inline CompiledPackage compilePackage(
        const std::string& packageName,
        const std::string& sourceFilePath,
        bool strictDecl = false,
        ZeusParser::BackendTarget target = ZeusParser::BackendTarget::VirtualMachine,
        const std::unordered_map<TreeParser::NodeId, std::string>*
            inferredFunctionReturnTypes = nullptr,
        const std::unordered_map<TreeParser::NodeId, std::string>*
            inferredBindingTypesByNode = nullptr
    ) {
        
        ZEUS_DEBUG_PRINT("[PackageLoader] Compiling package: %s from %s", packageName.c_str(), sourceFilePath.c_str());

        // Record how many globals exist before compiling this package
        // So we can extract only the NEW globals added by this package
        size_t globalsBeforeCompile = IRScope::globals.size();
        ZEUS_DEBUG_PRINT("[PackageLoader] Globals before compile: %zu", globalsBeforeCompile);

        // Clear IRScope state to avoid pollution between packages
        // NOTE: IRScope::globals is NOT cleared - packages can reference previously loaded packages
        IRScope::funcStack.clear();
        IRScope::scopeStack.clear();

        IRScope::localCount = 0;
        IRScope::useOriginalList = false;    // Reset to default (wrap as Array)
        IRScope::useOriginalBytes = false;   // Keep legacy original-mode state in sync.
        IRScope::useOriginalString = false;  // Reset to default (wrap as String)
        
        // 同时重置 originalModeStack 回初始状态
        IRScope::originalModeStack.clear();
        IRScope::originalModeStack.push_back(IRScope::OriginalTypeMode());
        
        ZEUS_DEBUG_PRINT("[PackageLoader] useOriginal flags RESET to false for package: %s", packageName.c_str());

        // 读取源代码
        std::ifstream file(sourceFilePath);
        
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open package file: " + sourceFilePath);
        }
        
        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string source = buffer.str();
        
        ZEUS_DEBUG_PRINT("[PackageLoader] Source code loaded (%zu bytes)", source.size());

        // čŻćłĺć
        ZEUS_DEBUG_PRINT("[PackageLoader] Starting lexer...");
        std::vector<ZeusDiagnostics::Diagnostic> syntaxDiagnostics;
        auto tokens = LexerParser::parse(source, &syntaxDiagnostics);
        ZeusDiagnostics::append(syntaxDiagnostics, ZeusParser::validateSyntax(tokens));
        ZEUS_DEBUG_PRINT("[PackageLoader] Syntax checks done (%zu diagnostics)", syntaxDiagnostics.size());
        ZEUS_DEBUG_PRINT("[PackageLoader] Lexer done (%zu tokens)", tokens.size());
        if (!syntaxDiagnostics.empty()) {
            throw std::runtime_error(ZeusDiagnostics::render(
                syntaxDiagnostics.front(), sourceFilePath, source
            ));
        }

        // čŻ­ćłĺć
        ZEUS_DEBUG_PRINT("[PackageLoader] Starting tree parser...");
        auto trees = TreeParser::parse(tokens, &syntaxDiagnostics);
        std::vector<ZeusDiagnostics::Diagnostic> backendWarnings;
        ZeusParser::selectBackendBlocks(trees, target, &backendWarnings);
        ZEUS_DEBUG_PRINT("[PackageLoader] Tree parser returned (%zu diagnostics)", syntaxDiagnostics.size());
        ZeusDiagnostics::append(
            syntaxDiagnostics,
            ZeusParser::validateParsedSyntax(trees)
        );
        ZEUS_DEBUG_PRINT("[PackageLoader] AST syntax checks done (%zu diagnostics)", syntaxDiagnostics.size());
        if (!syntaxDiagnostics.empty()) {
            for (const auto& diagnostic : syntaxDiagnostics) {
                ZEUS_DEBUG_PRINT("[PackageLoader] %s at %d:%d: %s",
                                 diagnostic.code.c_str(), diagnostic.line,
                                 diagnostic.column, diagnostic.message.c_str());
            }
            throw std::runtime_error(ZeusDiagnostics::render(
                syntaxDiagnostics.front(), sourceFilePath, source
            ));
        }
        ZeusParser::InterfaceContractRegistry importedInterfaceContracts;
        for (const auto& imported : extractImportDeclarations(source)) {
            const auto dependency = loadedPackages.find(imported.packageName);
            if (dependency == loadedPackages.end()) continue;
            ZeusParser::bindImportedInterfaceContracts(
                importedInterfaceContracts,
                imported.packageName,
                imported.alias,
                imported.importMode == 1,
                dependency->second.interfaceContracts
            );
        }
        const auto interfaceDiagnostics = ZeusParser::validateInterfaceConformance(
            trees, importedInterfaceContracts);
        if (!interfaceDiagnostics.empty()) {
            throw std::runtime_error(ZeusDiagnostics::render(
                interfaceDiagnostics.front(), sourceFilePath, source
            ));
        }
        ZEUS_DEBUG_PRINT("[PackageLoader] Tree parser done (%zu trees)", trees.size());

        // IR生成
        ZEUS_DEBUG_PRINT("[PackageLoader] Starting IR generator...");
        auto irgen = IRGen::IRGenerator();
        irgen.strictDecl = strictDecl;
        if (inferredFunctionReturnTypes != nullptr) {
            irgen.inferredFunctionReturnTypes = *inferredFunctionReturnTypes;
        }
        if (inferredBindingTypesByNode != nullptr) {
            irgen.inferredBindingTypesByNode = *inferredBindingTypesByNode;
        }
        
        // 保存当前编译状态
        bool savedUseOriginalList = IRScope::useOriginalList;
        bool savedUseOriginalBytes = IRScope::useOriginalBytes;
        bool savedUseOriginalString = IRScope::useOriginalString;
        
        // 设置包编译上下文，用于优先解析内置函数
        IRScope::isCompilingPackage = true;
        IRScope::currentPackageName = packageName;
        
        irgen.parse(trees);
        
        // 恢复非包编译状态和编译选项
        IRScope::isCompilingPackage = false;
        IRScope::currentPackageName = "";
        IRScope::useOriginalList = savedUseOriginalList;
        IRScope::useOriginalBytes = savedUseOriginalBytes;
        IRScope::useOriginalString = savedUseOriginalString;
        
        ZEUS_DEBUG_PRINT("[PackageLoader] IR generator done");
        
        // 检查编译状态
        if (irgen.status == IRType::IRCompilerStatus::Stopped) {
            ZEUS_DEBUG_PRINT("[PackageLoader] Package compilation failed for: %s", packageName.c_str());
            throw std::runtime_error("Package compilation failed: " + packageName);
        }

        // 注册源文件以支持准确的错误报告
        int sourceFileId = VMException::registerSourceFile(sourceFilePath, source);
        ZEUS_DEBUG_PRINT("[PackageLoader] Registered source file ID: %d for %s", sourceFileId, sourceFilePath.c_str());
        
        // 为所有生成的 IR 指令设置 sourceFileId
        for (auto& chunk : irgen.bytecodes) {
            for (auto& instr : chunk) {
                instr.sourceFileId = sourceFileId;
            }
        }

        // 构造编译结果
        CompiledPackage pkg;
        pkg.packageName = packageName;
        pkg.filePath = sourceFilePath;
        pkg.sourceCode = source;
        pkg.interfaceContracts = ZeusParser::collectInterfaceContracts(trees);
        pkg.warnings = std::move(backendWarnings);
        pkg.chunks = irgen.bytecodes;
        pkg.compileTimeGlobalsOffset = globalsBeforeCompile;  // Save for merge-time index adjustment
        pkg.sourceFileId = sourceFileId;
        
        // Extract only the NEW globals added by this package (not inherited from previous packages)
        if (irgen.globalNamesByIndex.size() > globalsBeforeCompile) {
            pkg.globalNames = std::vector<std::string>(
                irgen.globalNamesByIndex.begin() + globalsBeforeCompile,
                irgen.globalNamesByIndex.end()
            );
            ZEUS_DEBUG_PRINT("[PackageLoader] Package added %zu new globals (total was %zu, now %zu)", 
                pkg.globalNames.size(), globalsBeforeCompile, irgen.globalNamesByIndex.size());
        } else {
            // No new globals added
            pkg.globalNames.clear();
            ZEUS_DEBUG_PRINT("[PackageLoader] Package added no new globals");
        }

        const std::string functionPrefix = packageName + ".";
        for (const auto& chunk : pkg.chunks) {
            for (const auto& instr : chunk) {
                if (instr.metadata.kind != IRType::IRMetadataKind::FunctionSignature ||
                    instr.metadata.symbol.kind != IRType::SymbolRefKind::Function) {
                    continue;
                }
                const std::string& qualifiedName = instr.metadata.symbol.name;
                if (qualifiedName.compare(0, functionPrefix.size(), functionPrefix) != 0) continue;
                const std::string functionName = qualifiedName.substr(functionPrefix.size());
                if (std::find(pkg.globalNames.begin(), pkg.globalNames.end(), functionName) != pkg.globalNames.end()) {
                    pkg.functionNames.insert(functionName);
                }
            }
        }
        
        pkg.globalStartIndex = 0;  // 将在合并时设置

        ZEUS_DEBUG_PRINT("[PackageLoader] Package compiled: %s (%zu chunks, %zu globals)",
            packageName.c_str(), pkg.chunks.size(), pkg.globalNames.size());
        
        // 打印全局变量列表
        for (size_t i = 0; i < pkg.globalNames.size(); ++i) {
            ZEUS_DEBUG_PRINT("[PackageLoader]   Global[%zu]: %s", i, pkg.globalNames[i].c_str());
        }

        // Reset useOriginal flags after package compilation to avoid affecting subsequent imports or main program
        IRScope::useOriginalList = false;
        IRScope::useOriginalBytes = false;
        IRScope::useOriginalString = false;
        ZEUS_DEBUG_PRINT("[PackageLoader] useOriginal flags RESET to false after compiling: %s", packageName.c_str());

        // CRITICAL: Remove this package's own globals from IRScope::globals to prevent name collision
        // with subsequent packages. We keep the globals from PREVIOUS packages so they can be referenced.
        // EXCEPTION: compiler modules are NOT cleaned up - their globals (like Array class)
        // must remain available to all subsequent packages.
        const bool isBuiltinPackage =
            IRImport::hasPackagePrefix(packageName, "ZeusBuiltins") ||
            IRImport::hasPackagePrefix(packageName, "zeus");
        
        if (!isBuiltinPackage && IRScope::globals.size() > globalsBeforeCompile) {
            // Find and remove globals that have indices >= globalsBeforeCompile
            ZEUS_DEBUG_PRINT("[PackageLoader] Cleaning up user package globals (preserving builtins)");
            auto it = IRScope::globals.begin();
            while (it != IRScope::globals.end()) {
                if ((size_t)it->second >= globalsBeforeCompile) {
                    ZEUS_DEBUG_PRINT("[PackageLoader] Removing package global '%s' (index=%d) from IRScope", 
                        it->first.c_str(), it->second);
                    it = IRScope::globals.erase(it);
                } else {
                    ++it;
                }
            }
            ZEUS_DEBUG_PRINT("[PackageLoader] After cleanup: IRScope::globals.size()=%zu", IRScope::globals.size());
        } else if (isBuiltinPackage) {
            ZEUS_DEBUG_PRINT("[PackageLoader] Preserving builtin package globals for subsequent packages");
        }

        return pkg;
    }

    // ĺ č˝˝ĺ?ĺŚććŞĺ č˝˝ĺçźčŻ)
    inline CompiledPackage* loadPackage(std::string packageName, const std::string& currentSourcePath, bool strictDecl = false) {
        packageName = IRImport::canonicalPackageName(packageName);
        
        // ćŁćĽçźĺ­?
        auto it = loadedPackages.find(packageName);
        
        if (it != loadedPackages.end()) {
            ZEUS_DEBUG_PRINT("[PackageLoader] Package already loaded: %s", packageName.c_str());
            return &(it->second);
        }

        // čŽžç˝ŽĺˇĽä˝çŽĺ˝
        IRImport::setWorkingDirectory(currentSourcePath);

        // č§ŁćĺčˇŻĺž?
        std::string packagePath = IRImport::loadPackage(packageName);
        
        if (packagePath.empty()) {
            ZEUS_DEBUG_PRINT("[PackageLoader] Package not found: %s", packageName.c_str());
            return nullptr;
        }

        // çźčŻĺ?
        CompiledPackage pkg = compilePackage(
            packageName, packagePath, strictDecl, ZeusParser::BackendTarget::VirtualMachine);

        ZEUS_DEBUG_PRINT("[PackageLoader] Package globalNames:");
        for (size_t i = 0; i < pkg.globalNames.size(); ++i) {
            ZEUS_DEBUG_PRINT("[PackageLoader]   [%zu] %s", i, pkg.globalNames[i].c_str());
        }

        // çźĺ­ĺ?
        loadedPackages[packageName] = pkg;

        ZEUS_DEBUG_PRINT("[PackageLoader] Package loaded and cached: %s", packageName.c_str());

        return &loadedPackages[packageName];
    }

    // ćĺććimportčŻ­ĺĽ (čżĺ package_name -> ImportInfo çć ĺ°?
    inline std::map<std::string, ImportInfo> extractImports(const std::vector<std::vector<IRType::IRInstr>>& chunks) {
        
        std::map<std::string, ImportInfo> imports;

        for (const auto& chunk : chunks) {
            for (const auto& instr : chunk) {
                if (instr.op == VMOperator::IMPORT) {
                    if (instr.args.size() >= 3 && std::holds_alternative<LexerType::Value>(instr.args[0])) {
                        const auto& val = std::get<LexerType::Value>(instr.args[0]);
                        
                        if (std::holds_alternative<std::string>(val.data)) {
                            ImportInfo info;
                            info.packageName = IRImport::canonicalPackageName(
                                std::get<std::string>(val.data));
                            
                            // 获取别名 (args[1])
                            if (std::holds_alternative<LexerType::Value>(instr.args[1])) {
                                const auto& aliasVal = std::get<LexerType::Value>(instr.args[1]);
                                if (std::holds_alternative<std::string>(aliasVal.data)) {
                                    info.alias = std::get<std::string>(aliasVal.data);
                                }
                            }
                            
                            // 获取导入模式 (args[2])
                            info.importMode = 0;  // 默认为全局导入
                            if (instr.args.size() >= 3 && std::holds_alternative<LexerType::Value>(instr.args[2])) {
                                const auto& modeVal = std::get<LexerType::Value>(instr.args[2]);
                                if (std::holds_alternative<ZeusType::BigInteger>(modeVal.data)) {
                                    info.importMode = (int)std::get<ZeusType::BigInteger>(modeVal.data).toInt64();
                                }
                            }
                            
                            imports[info.packageName] = info;
                        }
                    }
                }
            }
        }

        return imports;
    }

    inline bool readImportInfo(const IRType::IRInstr& instr, ImportInfo& info) {
        if (instr.op != VMOperator::IMPORT || instr.args.size() < 3 ||
            !std::holds_alternative<LexerType::Value>(instr.args[0])) {
            return false;
        }

        const auto& packageValue = std::get<LexerType::Value>(instr.args[0]);
        if (!std::holds_alternative<std::string>(packageValue.data)) return false;
        info.packageName = IRImport::canonicalPackageName(
            std::get<std::string>(packageValue.data));
        info.alias.clear();
        info.importMode = 0;

        if (std::holds_alternative<LexerType::Value>(instr.args[1])) {
            const auto& aliasValue = std::get<LexerType::Value>(instr.args[1]);
            if (std::holds_alternative<std::string>(aliasValue.data)) {
                info.alias = std::get<std::string>(aliasValue.data);
            }
        }
        if (std::holds_alternative<LexerType::Value>(instr.args[2])) {
            const auto& modeValue = std::get<LexerType::Value>(instr.args[2]);
            if (std::holds_alternative<ZeusType::BigInteger>(modeValue.data)) {
                info.importMode = static_cast<int>(std::get<ZeusType::BigInteger>(modeValue.data).toInt64());
            }
        }
        return true;
    }

    inline CompiledPackage* findPackage(
        const std::vector<CompiledPackage*>& packages,
        const std::string& packageName) {
        for (auto* package : packages) {
            if (package && package->packageName == packageName) return package;
        }
        return nullptr;
    }

    inline std::vector<IRType::IRInstr> makeNamespaceImportInitChunk(
        const std::string& packageName,
        CompiledPackage* targetPackage,
        const std::vector<CompiledPackage*>& packages,
        const std::vector<std::string>& mergedGlobalNames,
        int aliasGlobalIndex) {
        if (targetPackage == nullptr) return {};

        int namespaceClassIndex = -1;
        for (auto* package : packages) {
            if (!package) continue;
            for (size_t i = 0; i < package->globalNames.size(); ++i) {
                if (package->globalNames[i] == "Namespace") {
                    namespaceClassIndex = package->globalStartIndex + static_cast<int>(i);
                    break;
                }
            }
            if (namespaceClassIndex >= 0) break;
        }
        if (namespaceClassIndex < 0) {
            for (size_t i = 0; i < mergedGlobalNames.size(); ++i) {
                if (mergedGlobalNames[i] == "Namespace") {
                    namespaceClassIndex = static_cast<int>(i);
                    break;
                }
            }
        }
        if (namespaceClassIndex < 0) return {};

        std::vector<IRType::IRInstr> chunk;
        chunk.push_back(IRType::IRInstr{
            VMOperator::LOAD_VALUE,
            {std::string("global"), LexerType::Value(ZeusType::BigInteger(namespaceClassIndex))}
        });
        chunk.push_back(IRType::IRInstr{VMOperator::PUSH, {LexerType::Value(packageName)}});
        for (const auto& name : targetPackage->globalNames) {
            chunk.push_back(IRType::IRInstr{VMOperator::PUSH, {LexerType::Value(name)}});
            chunk.push_back(IRType::IRInstr{VMOperator::PUSH, {LexerType::Value(true)}});
        }
        chunk.push_back(IRType::IRInstr{
            VMOperator::DICT_BUILD,
            {LexerType::Value(ZeusType::BigInteger(static_cast<int>(targetPackage->globalNames.size())))}
        });
        chunk.push_back(IRType::IRInstr{VMOperator::OBJ_BUILD, {LexerType::Value(ZeusType::BigInteger(2))}});
        chunk.push_back(IRType::IRInstr{
            VMOperator::STORE_VALUE_GLOBAL,
            {LexerType::Value(ZeusType::BigInteger(aliasGlobalIndex)), std::string("mutable")}
        });
        chunk.front().metadata = IRType::IRMetadata::moduleImportInit(packageName);
        return chunk;
    }


    inline void mergePackageChunks(
        std::vector<std::vector<IRType::IRInstr>>& mainChunks,
        std::vector<std::string>& mainGlobalNames,
        const std::vector<CompiledPackage*>& packages,
        const std::map<std::string, ImportInfo>& imports,
        const std::map<std::string, int>& packageGlobalOffsets) {
        
        // 保存原始的主程序全局变量列表(用于查找别名索引)
        for (auto* package : packages) {
            if (!package) continue;
            const auto packageImports = extractImports(package->chunks);
            for (const auto& importPair : packageImports) {
                const ImportInfo& info = importPair.second;
                if (info.importMode != 1) continue;
                const std::string hiddenName = moduleNamespaceGlobalName(
                    package->packageName, defaultImportAlias(info));
                if (std::find(mainGlobalNames.begin(), mainGlobalNames.end(), hiddenName) == mainGlobalNames.end()) {
                    mainGlobalNames.push_back(hiddenName);
                }
            }
        }

        std::vector<std::string> originalMainGlobals = mainGlobalNames;
        std::vector<std::string> newGlobalNames;
        

        int totalPackageGlobals = 0;
        for (auto* pkg : packages) {
            if (!pkg) continue;
            totalPackageGlobals += (int)pkg->globalNames.size();
        }

        newGlobalNames.resize(totalPackageGlobals);
        
        // 按照packageGlobalOffsets指定的位置摆放package globals
        for (auto* pkg : packages) {
            if (!pkg) continue;

            auto it = packageGlobalOffsets.find(pkg->packageName);
            if (it != packageGlobalOffsets.end()) {
                pkg->globalStartIndex = it->second;
                int baseIndex = it->second;
                
                // 使用包名前缀注册globalNames,确保和IRScope::globals一致
                for (size_t i = 0; i < pkg->globalNames.size(); ++i) {
                    std::string prefixedName = pkg->packageName + "." + pkg->globalNames[i];
                    newGlobalNames[baseIndex + i] = prefixedName;
                }
            } else {
                pkg->globalStartIndex = 0;
            }
        }
        
        ZEUS_DEBUG_PRINT("[PackageLoader] Before merge: mainGlobalNames.size()=%zu, newGlobalNames.size()=%zu",
            mainGlobalNames.size(), newGlobalNames.size());
        for (const auto& globalName : mainGlobalNames) {
            newGlobalNames.push_back(globalName);
            ZEUS_DEBUG_PRINT("[PackageLoader] Added main global: %s", globalName.c_str());
        }
        
        mainGlobalNames = newGlobalNames;
        ZEUS_DEBUG_PRINT("[PackageLoader] After merge: mainGlobalNames.size()=%zu", mainGlobalNames.size());

        std::vector<std::vector<IRType::IRInstr>> newChunks;
        int totalInsertedChunks = 0;
        int totalInsertedProtos = 0;
        
        for (auto* pkg : packages) {
            if (!pkg) continue;
            const auto fieldInitializerNameRemap =
                packageFieldInitializerNameRemap(pkg->chunks, pkg->packageName);
            
            ZEUS_DEBUG_PRINT("[PackageLoader] Inserting package chunks: %s (%zu chunks)", 
                pkg->packageName.c_str(), pkg->chunks.size());
            
            int chunkOffset = static_cast<int>(newChunks.size());
            
            int pkgProtoCount = 0;
            for (const auto& pkgChunk : pkg->chunks) {
                if (!pkgChunk.empty() && pkgChunk.front().op == VMOperator::FUNC_BEGIN) {
                    pkgProtoCount++;
                }
            }
            
            // Note: totalInsertedProtos累加移到调整CLASS_BUILD之后
            
            std::vector<int> packageChunkTargets(pkg->chunks.size(), chunkOffset);
            int nextChunkTarget = chunkOffset;
            for (size_t chunkIndex = 0; chunkIndex < pkg->chunks.size(); ++chunkIndex) {
                packageChunkTargets[chunkIndex] = nextChunkTarget;
                ImportInfo chunkImport;
                const bool isImport = !pkg->chunks[chunkIndex].empty() &&
                    readImportInfo(pkg->chunks[chunkIndex].front(), chunkImport);
                if (!isImport || chunkImport.importMode != 0) ++nextChunkTarget;
            }

            for (size_t packageChunkIndex = 0; packageChunkIndex < pkg->chunks.size(); ++packageChunkIndex) {
                const auto& pkgChunkSrc = pkg->chunks[packageChunkIndex];
                ZEUS_DEBUG_PRINT("[PackageLoader] Inspecting pkg=%s chunk=%zu/%zu instructions=%zu",
                    pkg->packageName.c_str(), packageChunkIndex, pkg->chunks.size(), pkgChunkSrc.size());
                ImportInfo chunkImport;
                const bool isPackageImport = !pkgChunkSrc.empty() &&
                    readImportInfo(pkgChunkSrc.front(), chunkImport);
                ZEUS_DEBUG_PRINT("[PackageLoader] pkg=%s chunk=%zu import=%d name=%s alias=%s mode=%d",
                    pkg->packageName.c_str(), packageChunkIndex, isPackageImport ? 1 : 0,
                    chunkImport.packageName.c_str(), chunkImport.alias.c_str(), chunkImport.importMode);
                if (isPackageImport) {
                    if (chunkImport.importMode == 0) {
                        ZEUS_DEBUG_PRINT("[PackageLoader] Skipping package global IMPORT chunk: %s (owner=%s)",
                            chunkImport.packageName.c_str(), pkg->packageName.c_str());
                        continue;
                    }

                    CompiledPackage* targetPackage = findPackage(packages, chunkImport.packageName);
                    const std::string alias = defaultImportAlias(chunkImport);
                    const std::string hiddenName = moduleNamespaceGlobalName(pkg->packageName, alias);
                    auto aliasIt = std::find(originalMainGlobals.begin(), originalMainGlobals.end(), hiddenName);
                    if (aliasIt == originalMainGlobals.end()) {
                        throw std::runtime_error("Missing namespace import slot for " + pkg->packageName + "." + alias);
                    }
                    const int aliasIndex = totalPackageGlobals +
                        static_cast<int>(std::distance(originalMainGlobals.begin(), aliasIt));
                    auto namespaceChunk = makeNamespaceImportInitChunk(
                        chunkImport.packageName, targetPackage, packages, mainGlobalNames, aliasIndex);
                    if (namespaceChunk.empty()) {
                        throw std::runtime_error("Cannot initialize namespace import " + chunkImport.packageName +
                            " for package " + pkg->packageName);
                    }
                    newChunks.push_back(std::move(namespaceChunk));
                    continue;
                }

                // Work on a local copy to avoid mutating cached package chunks.
                // Cached chunks are reused across compilations in the same process.
                auto pkgChunk = pkgChunkSrc;
                for (size_t i = 0; i < pkgChunk.size(); ++i) {
                    auto& instr = pkgChunk[i];
                    remapPackageFieldInitializerName(
                        instr, fieldInitializerNameRemap);
                    ZEUS_DEBUG_PRINT("[PackageLoader] Relocating pkg=%s chunk=%zu instr=%zu op=%d args=%zu",
                        pkg->packageName.c_str(), packageChunkIndex, i,
                        static_cast<int>(instr.op), instr.args.size());
                    relocateTypeImplPrototypes(instr, totalInsertedProtos);
                    
                    if (instr.op == VMOperator::MAKE_FUNCTION) {
                        if (instr.args.size() >= 1 && 
                            std::holds_alternative<LexerType::Value>(instr.args[0])) {
                            
                            auto& val = std::get<LexerType::Value>(instr.args[0]);
                            
                            if (std::holds_alternative<ZeusType::BigInteger>(val.data)) {
                                int oldChunkIndex = VMHelpers::getIntFromValue(val);
                                int newChunkIndex = oldChunkIndex >= 0 &&
                                    static_cast<size_t>(oldChunkIndex) < packageChunkTargets.size()
                                    ? packageChunkTargets[static_cast<size_t>(oldChunkIndex)]
                                    : oldChunkIndex + chunkOffset;
                                val.data = ZeusType::BigInteger(newChunkIndex);
                            }
                        }
                    }
                    
                    // 调整CLASS_BUILD中的protoId: 向前扫描PUSH指令中的protoId值
                    // CLASS_BUILD前的指令序列: PUSH "methodName" PUSH protoId ... DICT_BUILD n
                    // 我们需要找到所有PUSH整数值(protoId),并加上totalInsertedProtos偏移
                    if (instr.op == VMOperator::CLASS_BUILD) {
                        // 向前扫描,找到DICT_BUILD(构建instanceMethods dict)
                        // 模式: PUSH str, PUSH int, PUSH str, PUSH int, ..., DICT_BUILD n, ... (可能还有其他dict), CLASS_BUILD
                        // instanceMethods dict是CLASS_BUILD前的第4个元素(从后往前数)
                        
                        // 简化策略:从CLASS_BUILD向前扫描,找到PUSH整数值并判断是否是protoId
                        // protoId的特征:1)是PUSH指令 2)参数是整数Value 3)前一条是PUSH字符串(方法名)
                        for (int j = (int)i - 1; j >= 0; --j) {
                            auto& prevInstr = pkgChunk[j];
                            
                            // 如果遇到DICT_BUILD,停止向前扫描本dict之前的内容
                            if (prevInstr.op == VMOperator::DICT_BUILD) {
                                // 但继续向前扫描,因为可能有多个dict(static/instance/operator/fields)
                                continue;
                            }
                            
                            // 检查是否是PUSH整数值
                            if (prevInstr.op == VMOperator::PUSH && 
                                prevInstr.args.size() >= 1 &&
                                std::holds_alternative<LexerType::Value>(prevInstr.args[0])) {
                                
                                auto& pushVal = std::get<LexerType::Value>(prevInstr.args[0]);
                                
                                if (std::holds_alternative<ZeusType::BigInteger>(pushVal.data)) {
                                    // 这是一个整数,可能是protoId
                                    // 检查前一条指令是否是PUSH字符串(方法名)
                                    if (j > 0 && pkgChunk[j-1].op == VMOperator::PUSH &&
                                        pkgChunk[j-1].args.size() >= 1 &&
                                        std::holds_alternative<LexerType::Value>(pkgChunk[j-1].args[0]) &&
                                        std::holds_alternative<std::string>(std::get<LexerType::Value>(pkgChunk[j-1].args[0]).data)) {
                                        
                                    // 确认是protoId,调整值
                                    int oldProtoId = VMHelpers::getIntFromValue(pushVal);
                                    int protoIdOffset = totalInsertedProtos; // 之前所有包的proto总数
                                    int newProtoId = oldProtoId + protoIdOffset;
                                        pushVal.data = ZeusType::BigInteger(newProtoId);
                                        
                                        std::string methodName = std::get<std::string>(std::get<LexerType::Value>(pkgChunk[j-1].args[0]).data);
                                        ZEUS_DEBUG_PRINT("[PackageLoader] Adjusted CLASS protoId for method '%s': %d -> %d (pkg=%s, offset=%d)",
                                            methodName.c_str(), oldProtoId, newProtoId, pkg->packageName.c_str(), protoIdOffset);
                                    }
                                }
                            }
                            
                            // 停止条件:如果遇到不相关的指令,停止扫描
                            // (假设CLASS_BUILD相关的所有PUSH都在附近)
                            if (j < (int)i - 100) break; // 防止扫描过远
                        }
                    }
                    
                    if (instr.op == VMOperator::STORE_VALUE_GLOBAL ||
                        instr.op == VMOperator::ASSIGN_VALUE_GLOBAL) {
                        
                        // 包的STORE需要调整,使用包的globalStartIndex
                        // Same logic as LOAD_VALUE: only adjust indices for THIS package's globals
                        if (instr.args.size() >= 1 && 
                            std::holds_alternative<LexerType::Value>(instr.args[0])) {
                            
                            auto& val = std::get<LexerType::Value>(instr.args[0]);
                            
                            if (std::holds_alternative<ZeusType::BigInteger>(val.data)) {
                                int oldIndex = VMHelpers::getIntFromValue(val);
                                
                                // Check if this is THIS package's global
                                auto aliasSentinel = pkg->namespaceAliasSentinels.find(oldIndex);
                                if (aliasSentinel != pkg->namespaceAliasSentinels.end()) {
                                    auto aliasIt = std::find(originalMainGlobals.begin(), originalMainGlobals.end(), aliasSentinel->second);
                                    if (aliasIt == originalMainGlobals.end()) {
                                        throw std::runtime_error("Missing compiled namespace alias slot: " + aliasSentinel->second);
                                    }
                                    const int aliasIndex = totalPackageGlobals +
                                        static_cast<int>(std::distance(originalMainGlobals.begin(), aliasIt));
                                    val.data = ZeusType::BigInteger(aliasIndex);
                                } else if (auto importedSlot = pkg->importedGlobalSlotSentinels.find(oldIndex);
                                           importedSlot != pkg->importedGlobalSlotSentinels.end()) {
                                    val.data = ZeusType::BigInteger(importedSlot->second);
                                } else if (oldIndex >= 0 && static_cast<size_t>(oldIndex) >= pkg->compileTimeGlobalsOffset &&
                                           static_cast<size_t>(oldIndex) - pkg->compileTimeGlobalsOffset < pkg->globalNames.size()) {
                                    // This is a global defined IN THIS PACKAGE
                                    int relativeIndex = oldIndex - pkg->compileTimeGlobalsOffset;
                                    int newIndex = pkg->globalStartIndex + relativeIndex;
                                    val.data = ZeusType::BigInteger(newIndex);
                                    
                                    ZEUS_DEBUG_PRINT("[PackageLoader] Adjusted STORE_VALUE global %d -> %d (pkg=%s)",
                                        oldIndex, newIndex, pkg->packageName.c_str());
                                } else {
                                    // Global from previous package - don't adjust
                                    ZEUS_DEBUG_PRINT("[PackageLoader] Keeping STORE_VALUE global %d (from previous package, pkg=%s)",
                                        oldIndex, pkg->packageName.c_str());
                                }
                            }
                        }
                    }
                    
                    // 包的LOAD_VALUE global也需要调整
                    // BUT: only adjust indices for globals DEFINED IN THIS PACKAGE
                    // Globals from other packages (e.g. Array from zeus.list) should not be adjusted
                    if (instr.op == VMOperator::LOAD_VALUE &&
                        instr.args.size() >= 2 &&
                        std::holds_alternative<std::string>(instr.args[0]) &&
                        std::get<std::string>(instr.args[0]) == "global") {
                        
                        if (std::holds_alternative<LexerType::Value>(instr.args[1])) {
                            auto& val = std::get<LexerType::Value>(instr.args[1]);
                            
                            if (std::holds_alternative<ZeusType::BigInteger>(val.data)) {
                                int oldIndex = VMHelpers::getIntFromValue(val);
                                
                                // During compilation, IRScope::globals accumulates across packages
                                // pkg->compileTimeGlobalsOffset tells us how many globals existed BEFORE this package
                                // If oldIndex >= compileTimeGlobalsOffset, it's this package's own global - needs remapping
                                // Otherwise, it's from a previous package - already has correct index
                                
                                auto aliasSentinel = pkg->namespaceAliasSentinels.find(oldIndex);
                                if (aliasSentinel != pkg->namespaceAliasSentinels.end()) {
                                    auto aliasIt = std::find(originalMainGlobals.begin(), originalMainGlobals.end(), aliasSentinel->second);
                                    if (aliasIt == originalMainGlobals.end()) {
                                        throw std::runtime_error("Missing compiled namespace alias slot: " + aliasSentinel->second);
                                    }
                                    const int aliasIndex = totalPackageGlobals +
                                        static_cast<int>(std::distance(originalMainGlobals.begin(), aliasIt));
                                    val.data = ZeusType::BigInteger(aliasIndex);
                                } else if (auto importedSlot = pkg->importedGlobalSlotSentinels.find(oldIndex);
                                           importedSlot != pkg->importedGlobalSlotSentinels.end()) {
                                    val.data = ZeusType::BigInteger(importedSlot->second);
                                } else if (oldIndex >= 0 && static_cast<size_t>(oldIndex) >= pkg->compileTimeGlobalsOffset &&
                                           static_cast<size_t>(oldIndex) - pkg->compileTimeGlobalsOffset < pkg->globalNames.size()) {
                                    // This is a global defined IN THIS PACKAGE
                                    // Remap: oldIndex is compile-time cumulative, newIndex is runtime position
                                    // Runtime position = globalStartIndex + (oldIndex - compileTimeGlobalsOffset)
                                    int relativeIndex = oldIndex - pkg->compileTimeGlobalsOffset;
                                    int newIndex = pkg->globalStartIndex + relativeIndex;
                                    val.data = ZeusType::BigInteger(newIndex);
                                    
                                    ZEUS_DEBUG_PRINT("[PackageLoader] Adjusted LOAD_VALUE global %d -> %d (pkg=%s, offset=%zu, start=%d)",
                                        oldIndex, newIndex, pkg->packageName.c_str(), pkg->compileTimeGlobalsOffset, pkg->globalStartIndex);
                                } else {
                                    // This is a global from a PREVIOUS PACKAGE - don't adjust
                                    ZEUS_DEBUG_PRINT("[PackageLoader] Keeping LOAD_VALUE global %d (from previous package, pkg=%s)",
                                        oldIndex, pkg->packageName.c_str());
                                }
                            }
                        }
                    }
                }
                
                newChunks.push_back(std::move(pkgChunk));
            }
            
            // 在处理完当前package的所有chunks后，累加protoCount
            totalInsertedChunks = static_cast<int>(newChunks.size());
            totalInsertedProtos += pkgProtoCount;
        }
        
        int globalIndexAdjustment = totalPackageGlobals;
        
        // 计算实际需要调整的chunk偏移量:需要减去将被skip的IMPORT chunk数
        // 注意：只有global import (importMode==0)的chunk会被跳过，namespace import会被替换为新chunk
        int globalImportChunkCount = 0;
        for (const auto& chunk : mainChunks) {
            if (!chunk.empty() && chunk.front().op == VMOperator::IMPORT) {
                const auto& importInstr = chunk.front();
                int importMode = 0;
                if (importInstr.args.size() >= 3 && std::holds_alternative<LexerType::Value>(importInstr.args[2])) {
                    importMode = VMHelpers::getIntFromValue(std::get<LexerType::Value>(importInstr.args[2]));
                }
                if (importMode == 0) {
                    globalImportChunkCount++;
                }
            }
        }
        int blockIndexAdjustment = totalInsertedChunks - globalImportChunkCount;
        
        ZEUS_DEBUG_PRINT("[PackageLoader] blockIndexAdjustment calculated: total=%d - globalImports=%d = %d",
            totalInsertedChunks, globalImportChunkCount, blockIndexAdjustment);
        
        for (auto& chunk : mainChunks) {
            // 处理IMPORT chunks:
            // - 如果是global import (importMode==0): 跳过,因为包已经合并
            // - 如果是namespace import (importMode==1): 转换为创建Namespace对象的代码
            if (!chunk.empty() && chunk.front().op == VMOperator::IMPORT) {
                const auto& importInstr = chunk.front();
                
                // 提取包名和导入模式
                std::string packageName;
                int importMode = 0;
                
                // args[0] 是 LexerType::Value,需要从 data 中提取字符串
                if (importInstr.args.size() >= 1 && std::holds_alternative<LexerType::Value>(importInstr.args[0])) {
                    const auto& val = std::get<LexerType::Value>(importInstr.args[0]);
                    if (std::holds_alternative<std::string>(val.data)) {
                        packageName = std::get<std::string>(val.data);
                    }
                }
                if (importInstr.args.size() >= 3 && std::holds_alternative<LexerType::Value>(importInstr.args[2])) {
                    importMode = VMHelpers::getIntFromValue(std::get<LexerType::Value>(importInstr.args[2]));
                }
                
                if (importMode == 0) {
                    // Global import: 跳过chunk
                    ZEUS_DEBUG_PRINT("[PackageLoader] Skipping IMPORT chunk (global mode): %s", packageName.c_str());
                    continue;
                } else {
                    // Namespace import: 生成创建Namespace对象的代码
                    ZEUS_DEBUG_PRINT("[PackageLoader] Converting IMPORT to Namespace creation: %s", packageName.c_str());
                    
                    // 查找 imports 中的别名
                    auto importIt = imports.find(packageName);
                    if (importIt == imports.end()) {
                        ZEUS_DEBUG_PRINT("[PackageLoader] Warning: package '%s' not found in imports map", packageName.c_str());
                        continue;
                    }
                    
                    std::string alias = importIt->second.alias;
                    if (alias.empty()) {
                        alias = packageName;  // 如果没有别名,使用包名
                        
                        // 如果包名包含点号,只使用最后一段作为别名
                            // 例如: "zeus.math" -> "math"
                        size_t lastDot = alias.find_last_of('.');
                        if (lastDot != std::string::npos && lastDot + 1 < alias.size()) {
                            alias = alias.substr(lastDot + 1);
                            ZEUS_DEBUG_PRINT("[PackageLoader] Adjusted alias for '%s' to '%s' (removed package prefix)", 
                                packageName.c_str(), alias.c_str());
                        }
                    }
                    
                    // 找到对应的包
                    CompiledPackage* targetPkg = nullptr;
                    for (auto* pkg : packages) {
                        if (pkg && pkg->packageName == packageName) {
                            targetPkg = pkg;
                            break;
                        }
                    }
                    
                    if (!targetPkg) {
                        ZEUS_DEBUG_PRINT("[PackageLoader] Warning: package '%s' not found in packages", packageName.c_str());
                        continue;
                    }
                    
                    // 生成新的 chunk 来创建 Namespace 对象
                    std::vector<IRType::IRInstr> nsChunk;
                    
                    // 1. 加载 Namespace 类 (需要在所有包的全局变量中查找)
                    // Namespace 类定义在 zeus.namespace 包中
                    int namespaceClassIdx = -1;
                    
                    // 先在包的全局变量中查找
                    for (auto* pkg : packages) {
                        if (!pkg) continue;
                        
                        for (size_t i = 0; i < pkg->globalNames.size(); ++i) {
                            if (pkg->globalNames[i] == "Namespace") {
                                namespaceClassIdx = pkg->globalStartIndex + (int)i;
                                ZEUS_DEBUG_PRINT("[PackageLoader] Found Namespace class in package '%s' at global index %d",
                                    pkg->packageName.c_str(), namespaceClassIdx);
                                break;
                            }
                        }
                        
                        if (namespaceClassIdx >= 0) break;
                    }
                    
                    // 如果没找到,再在主程序全局变量中查找
                    if (namespaceClassIdx < 0) {
                        for (size_t i = 0; i < mainGlobalNames.size(); ++i) {
                            if (mainGlobalNames[i] == "Namespace") {
                                // 计算在运行时的实际全局索引
                                // 主程序的全局变量在包的全局变量之后
                                int totalPackageGlobals = 0;
                                for (auto* pkg : packages) {
                                    if (pkg) totalPackageGlobals += (int)pkg->globalNames.size();
                                }
                                namespaceClassIdx = totalPackageGlobals + (int)i;
                                ZEUS_DEBUG_PRINT("[PackageLoader] Found Namespace class in main program at global index %d", namespaceClassIdx);
                                break;
                            }
                        }
                    }
                    
                    if (namespaceClassIdx < 0) {
                        // LLVM does not need a runtime Namespace object for a
                        // namespace import: the emitter resolves exported class
                        // members from this marker. Keep the import and exports
                        // in a metadata-only chunk instead of dropping it.
                        ZEUS_DEBUG_PRINT("[PackageLoader] Building static namespace import marker for '%s'",
                            packageName.c_str());
                        std::string markerAlias = importIt->second.alias;
                        if (markerAlias.empty()) {
                            markerAlias = packageName;
                            const size_t lastDot = markerAlias.find_last_of('.');
                            if (lastDot != std::string::npos) {
                                markerAlias = markerAlias.substr(lastDot + 1);
                            }
                        }

                        int aliasIndex = -1;
                        for (size_t i = 0; i < originalMainGlobals.size(); ++i) {
                            if (originalMainGlobals[i] == markerAlias) {
                                aliasIndex = static_cast<int>(i);
                                break;
                            }
                        }
                        if (aliasIndex < 0) {
                            ZEUS_DEBUG_PRINT("[PackageLoader] Namespace alias '%s' has no global slot",
                                markerAlias.c_str());
                            continue;
                        }

                        std::vector<IRType::IRInstr> markerChunk;
                        IRType::IRInstr marker = importInstr;
                        marker.metadata = IRType::IRMetadata::moduleImportInit(packageName);
                        marker.args.push_back(LexerType::Value(
                            ZeusType::BigInteger(totalPackageGlobals + aliasIndex)));
                        markerChunk.push_back(std::move(marker));
                        for (const auto& name : targetPkg->globalNames) {
                            markerChunk.push_back(IRType::IRInstr{
                                VMOperator::PUSH, {LexerType::Value(name)}
                            });
                        }
                        newChunks.push_back(std::move(markerChunk));
                        continue;
                    }
                    
                    // 2. 先加载 Namespace 类 (压到栈底)
                    nsChunk.push_back(IRType::IRInstr{
                        VMOperator::LOAD_VALUE,
                        {std::string("global"), LexerType::Value(ZeusType::BigInteger(namespaceClassIdx))}
                    });
                    
                    // 3. 压入包名字符串
                    nsChunk.push_back(IRType::IRInstr{
                        VMOperator::PUSH,
                        {LexerType::Value(packageName)}
                    });
                    
                    // 4. 构建 exports 字典: {memberName1: true, memberName2: true, ...}
                    // 使用字典便于快速查找和hasKey验证
                    for (const auto& name : targetPkg->globalNames) {
                        // PUSH key (成员名称)
                        nsChunk.push_back(IRType::IRInstr{
                            VMOperator::PUSH,
                            {LexerType::Value(name)}
                        });
                        // PUSH value (true,表示该成员存在)
                        nsChunk.push_back(IRType::IRInstr{
                            VMOperator::PUSH,
                            {LexerType::Value(true)}
                        });
                    }
                    
                    // DICT_BUILD with pair count = globalNames.size()
                    nsChunk.push_back(IRType::IRInstr{
                        VMOperator::DICT_BUILD,
                        {LexerType::Value(ZeusType::BigInteger((int)targetPkg->globalNames.size()))}
                    });
                    
                    // 5. 调用 Namespace 构造函数: Namespace(packageName, exportsList)
                    // 栈布局现在是: [NamespaceClass, packageName, exports_array]
                    // 使用 OBJ_BUILD 指令: args[0]=argc
                    nsChunk.push_back(IRType::IRInstr{
                        VMOperator::OBJ_BUILD,
                        {LexerType::Value(ZeusType::BigInteger(2))}  // 2个参数
                    });
                    
                    // 6. 将创建的 Namespace 实例存储到别名对应的全局变量
                    // 在原始的主程序全局变量列表中查找别名索引
                    int aliasIdx = -1;

                    for (size_t i = 0; i < originalMainGlobals.size(); ++i) {
                        if (originalMainGlobals[i] == alias) {
                            aliasIdx = (int)i;
                            break;
                        }
                    }
                    
                    if (aliasIdx < 0) {
                        // 别名不存在,添加到原始列表和当前列表
                        aliasIdx = (int)originalMainGlobals.size();
                        ZEUS_DEBUG_PRINT("[PackageLoader] Added namespace alias '%s' at index %d", alias.c_str(), aliasIdx);
                    }
                    
                    // 计算运行时的全局索引: packageGlobals + aliasIdx
                    int totalPackageGlobals = 0;

                    for (auto* pkg : packages) {
                        if (pkg) totalPackageGlobals += (int)pkg->globalNames.size();
                    }

                    int runtimeAliasIdx = totalPackageGlobals + aliasIdx;
                    
                    ZEUS_DEBUG_PRINT("[PackageLoader] Alias '%s' will be stored at runtime global index %d (package globals: %d, main offset: %d)",
                        alias.c_str(), runtimeAliasIdx, totalPackageGlobals, aliasIdx);
                    
                    // STORE_VALUE_GLOBAL(index, mutOrConst)
                    nsChunk.push_back(IRType::IRInstr{
                        VMOperator::STORE_VALUE_GLOBAL,
                        {
                            LexerType::Value(ZeusType::BigInteger(runtimeAliasIdx)),
                            std::string("mutable")  // namespace对象是可变的
                        }
                    });
                    
                    // 将生成的 chunk 添加到 newChunks
                    if (!nsChunk.empty()) {
                        nsChunk.front().metadata = IRType::IRMetadata::moduleImportInit(packageName);
                    }
                    newChunks.push_back(nsChunk);
                    ZEUS_DEBUG_PRINT("[PackageLoader] Generated Namespace creation chunk for '%s' (alias: '%s')", 
                        packageName.c_str(), alias.c_str());
                    
                    continue;
                }
            }
            
            for (size_t i = 0; i < chunk.size(); ++i) {
                auto& instr = chunk[i];
                relocateTypeImplPrototypes(instr, totalInsertedProtos);
                
                if (instr.op == VMOperator::MAKE_FUNCTION) {
                    if (instr.args.size() >= 1 && 
                        std::holds_alternative<LexerType::Value>(instr.args[0])) {
                        
                        auto& val = std::get<LexerType::Value>(instr.args[0]);
                        
                        if (std::holds_alternative<ZeusType::BigInteger>(val.data)) {
                            int oldChunkIndex = VMHelpers::getIntFromValue(val);
                            int newChunkIndex = oldChunkIndex + blockIndexAdjustment;
                            val.data = ZeusType::BigInteger(newChunkIndex);
                            
                            ZEUS_DEBUG_PRINT("[PackageLoader] Adjusted main MAKE_FUNCTION: %d -> %d", 
                                oldChunkIndex, newChunkIndex);
                        }
                    }
                }
                
                // 处理 CLASS_BUILD 和 CLASS_BUILD_SHARED 的 protoId 调整
                // CLASS_BUILD 前的结构: PUSH className, PUSH alias, 
                //   staticMethods dict, instanceMethods dict, operatorMethods dict, fields dict
                // 需要调整 static/instance/operator 方法的 protoId，但不调整 fields 的默认值
                if (instr.op == VMOperator::CLASS_BUILD || instr.op == VMOperator::CLASS_BUILD_SHARED) {
                    // 从 CLASS_BUILD 向前扫描，找到 DICT_BUILD 指令
                    // 第一个遇到的是 fields dict（跳过），之后的是方法 dict（需要调整）
                    int dictCount = 0;
                    int lastDictBuildIdx = -1;
                    std::vector<int> dictBuildIndices;
                    
                    // 先收集所有 DICT_BUILD 的位置
                    for (int j = (int)i - 1; j >= 0 && j > (int)i - 100; --j) {
                        if (chunk[j].op == VMOperator::DICT_BUILD) {
                            dictBuildIndices.push_back(j);
                            dictCount++;
                            if (dictCount >= 4) break;  // 最多4个dict
                        }
                    }
                    
                    if (dictBuildIndices.size() >= 2) {
                        // dictBuildIndices[0] = fields dict (最靠近CLASS_BUILD的)
                        // dictBuildIndices[1] = operatorMethods dict
                        // dictBuildIndices[2] = instanceMethods dict
                        // dictBuildIndices[3] = staticMethods dict
                        
                        // 对于 operatorMethods, instanceMethods, staticMethods，调整 protoId
                        for (size_t d = 1; d < dictBuildIndices.size(); ++d) {
                            int dictEnd = dictBuildIndices[d];
                            int dictStart = (d + 1 < dictBuildIndices.size()) ? dictBuildIndices[d + 1] : 0;
                            
                            // 在这个 dict 范围内查找 PUSH string, PUSH int 模式
                            for (int j = dictEnd - 1; j > dictStart; --j) {
                                auto& pushInstr = chunk[j];
                                if (pushInstr.op == VMOperator::PUSH && pushInstr.args.size() >= 1 && 
                                    std::holds_alternative<LexerType::Value>(pushInstr.args[0])) {
                                    
                                    auto& pushVal = std::get<LexerType::Value>(pushInstr.args[0]);
                                    if (std::holds_alternative<ZeusType::BigInteger>(pushVal.data)) {
                                        // 检查前一条是否是 PUSH 字符串
                                        if (j > 0 && chunk[j-1].op == VMOperator::PUSH && 
                                            chunk[j-1].args.size() >= 1 &&
                                            std::holds_alternative<LexerType::Value>(chunk[j-1].args[0]) && 
                                            std::holds_alternative<std::string>(std::get<LexerType::Value>(chunk[j-1].args[0]).data)) {
                                            
                                            int oldProtoId = VMHelpers::getIntFromValue(pushVal);
                                            int newProtoId = oldProtoId + totalInsertedProtos;
                                            pushVal.data = ZeusType::BigInteger(newProtoId);
                                            std::string methodName = std::get<std::string>(std::get<LexerType::Value>(chunk[j-1].args[0]).data);
                                            ZEUS_DEBUG_PRINT("[PackageLoader] Adjusted main CLASS protoId for method '%s': %d -> %d (offset=%d)", 
                                                methodName.c_str(), oldProtoId, newProtoId, totalInsertedProtos);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            
            newChunks.push_back(chunk);
        }
        
        mainChunks = newChunks;

        ZEUS_DEBUG_PRINT("[PackageLoader] Merge complete. Total chunks: %zu", mainChunks.size());
    }
    

    inline void resolveUnresolvedGlobals(
        std::vector<std::vector<IRType::IRInstr>>& mainChunks,
        const std::vector<std::string>& mainGlobalNames) {

        int resolvedCount = 0;
        
        ZEUS_DEBUG_PRINT("[PackageLoader] Attempted to resolve unresolved globals (not implemented)");
    }

    // 清除已加载的包缓存
    inline void clearPackageCache() {
        loadedPackages.clear();
        ZEUS_DEBUG_PRINT("[PackageLoader] Package cache cleared");
    }

} // namespace ZeusPackage

#endif
