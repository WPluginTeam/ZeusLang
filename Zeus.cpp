
#pragma once 

#include "./ZeusParser/LexerParser.hpp"
#include "./ZeusParser/TreeParser.hpp"
#include "./ZeusIR/IRGenerator.hpp"
#include "./ZeusIR/IRScope.hpp"
#include "./ZeusParser/LexerType.hpp"
#include "./ZeusIR/IRExprFold.hpp"
#include "./ZeusIR/IRPriorityFold.hpp"
#include "./ZeusRunner/ZeusVM.hpp"
#include "./ZeusRunner/ZeusBuiltins.hpp"
#include "./ZeusCompile.hpp"
#include "./ZeusDebug/Utils.hpp"
#include "./ZeusRunner/ZeusHotspot.hpp"
#include "./ZeusPackage/ZeusPackageLoader.hpp"
#include "./ZeusIR/IRImport.hpp"
#include "./ZeusBuiltins/BuiltinCompilation.hpp"
#include "./ZeusLLVM/ZeusEmitter.hpp"
#include "./ZeusLLVM/ZeusModuleGraph.hpp"
#include "./ZeusRunner/ZeusThread.hpp"
#include "./ZeusTyping/TypeChecker.hpp"

#include <algorithm>
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#endif

int main(int argc, char* argv[]) {
#ifdef _WIN32
    // Ensure console uses UTF-8 so our UTF-8 messages print correctly
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    ZeusBuiltins::initCoreBuiltins();
    IRHotspotList::registerHotspots();
    
    // 设置Zeus可执行文件目录,用于包管理查找ZeusBuiltins/
    if (argc > 0) {
        IRImport::setZeusExecutableDir(argv[0]);
    }


    if (argc == 1) {
        std::cout << "Zeus: no args -> running built-in test (Lexer+Parser+IR+VM)" << std::endl;

    Test::testLexerParserIRGen(true);
        return 0;
    }

    bool strictDecl = false;
    bool timingFlag = false;
    bool enableHotspot = true;
    bool strictCapture = true;

    // 第一遍：扫描全局 flag
    std::string debugChannelsPending;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--strict-decl") {
            strictDecl = true;
            continue;
        }
        if (a == "--strict-capture") {
            strictCapture = true;
            continue;
        }
        if (a == "--debug") {
            ZeusDebug::setEnabled(true);
            continue;
        }
        if (a == "-t" || a == "--time") {
            timingFlag = true;
            continue;
        }
        if (a == "--no-hotspot") {
            enableHotspot = false;
            continue;
        }
        const std::string prefix = "--debug-channels=";
        if (a.rfind(prefix, 0) == 0) {
            debugChannelsPending = a.substr(prefix.size());
            ZeusDebug::setEnabled(true);
            continue;
        }
    }

    if (!debugChannelsPending.empty()) {
        ZeusDebug::parseAndSetChannels(debugChannelsPending);
    }

    // 将 strictCapture 的设置应用到解析作用域
    IRScope::allowImplicitCapture = !strictCapture;

    ZeusCompile::enableTiming = timingFlag;

    // 第二步：找到第一个非全局 flag
    int argIndex = 1;

    while (argIndex < argc) {
        std::string a = argv[argIndex];
        if (a == "--strict-decl" ||
            a == "--debug" ||
            a == "-t" ||
            a == "--time" ||
            a == "--no-hotspot" ||
            a == "--strict-capture" ||
            a.rfind("--debug-channels=", 0) == 0) {
            argIndex++;
            continue;
        }
        break;
    }

    if (argIndex >= argc) {
    std::cout << "Zeus: only global flags provided -> running built-in test (Lexer+Parser+IR+VM)" << std::endl;
    Test::testLexerParserIRGen(enableHotspot);
        return 0;
    }

    std::string arg1 = argv[argIndex++];
    bool compileSingle = false;
    bool emitLLVM = false;
    std::string emitOut;
    std::string srcPath;

    if (arg1 == "-c" || arg1 == "-s") {
        if (argIndex >= argc) {
            std::cout << arg1 << " requires a source file path" << std::endl;
            return 1;
        }
        compileSingle = true;
        srcPath = argv[argIndex++];
    } else if (arg1 == "--emit-llvm") {
        if (argIndex >= argc) {
            std::cout << "--emit-llvm requires a source file path" << std::endl;
            return 1;
        }
        emitLLVM = true;
        srcPath = argv[argIndex++];

        // 可选位置参数：输出 .ll 路径（默认 <src>.ll）。
        if (argIndex < argc) {
            std::string maybe = argv[argIndex];

            if (!maybe.empty() && maybe[0] != '-') {
                emitOut = maybe;
                argIndex++;
            }
        }
    } else if (arg1 == "-r") {
        if (argIndex >= argc) {
            std::cout << "-r requires a .zir file path" << std::endl;
            return 1;
        }

        std::string zirPath = argv[argIndex++];
        std::string err;
        auto cr = ZeusCompile::read(zirPath, &err, enableHotspot);

        cr.strictDecl = strictDecl;

        if (cr.program.empty()) {
            std::cout << "Read failed: " << err << std::endl;
            return 1;
        }

        if (ZeusDebug::enabled() && ZeusDebug::channelEnabled(ZeusDebug::CH_IR)) {
            std::cout << "-------------------- IR Gen --------------------" << std::endl;
            std::cout << "(Read) chunk count: " << cr.program.size() << std::endl;
            for (size_t ci = 0; ci < cr.program.size(); ++ci) {
                std::cout << "== Chunk #" << ci << " ==" << std::endl;
                for (auto &instr : cr.program[ci]) {
                    std::cout << instr.toString() << std::endl;
                }
            }
            std::cout << "-------------------- Output --------------------" << std::endl;
        } else {
            // non-debug mode: keep silent unless IR channel enabled
        }
        ZeusCompile::run(cr);
        
        return 0;
    } else {
        srcPath = arg1;
    }

        std::ifstream ifs(srcPath);
        if (!ifs) {
            std::cout << "Cannot open source file: " << srcPath << std::endl;
            return 1;
        }

        std::string code((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

        // 清除之前的源文件信息（在所有编译开始之前）
        VMException::clearSourceFiles();
        
        // 注册主文件作为 source file ID 0
        VMException::setSourceFilePath(srcPath);
        VMException::setSourceCode(code);

        const auto prescanTokens = LexerParser::parse(code);
        bool usesBytesLiteral = false;
        for (const auto& token : prescanTokens) {
            if (std::holds_alternative<LexerType::BytesLiteral>(token)) {
                usesBytesLiteral = true;
                break;
            }
        }

        const auto builtinTarget = emitLLVM
            ? ZeusBuiltins::CompilationTarget::LLVM
            : ZeusBuiltins::CompilationTarget::VirtualMachine;
        const auto builtinCompilationPlan = ZeusBuiltins::makeCompilationPlan(
            builtinTarget, usesBytesLiteral);

        ZeusLLVM::ModuleGraphResult llvmModuleGraph;
        if (emitLLVM) {
            llvmModuleGraph = ZeusLLVM::buildModuleGraph(
                srcPath, code, builtinCompilationPlan.implementationModules);
            if (!llvmModuleGraph.ok()) {
                std::cout << llvmModuleGraph.error << std::endl;
                return 1;
            }

            auto findModule = [&](const std::string& packageName) -> const ZeusLLVM::ModuleNode* {
                for (const auto& module : llvmModuleGraph.dependencyOrder) {
                    if (module.packageName == packageName) return &module;
                }
                return nullptr;
            };

            auto registerImportedInterfaces = [&](ZeusTypeInfer::TypeChecker& checker,
                                                   const std::vector<ZeusLLVM::ModuleImport>& imports) {
                for (const auto& imported : imports) {
                    const ZeusLLVM::ModuleNode* module = findModule(imported.packageName);
                    if (module == nullptr) continue;

                    std::string alias = imported.alias;
                    if (alias.empty()) {
                        alias = imported.packageName;
                        const size_t dot = alias.find_last_of('.');
                        if (dot != std::string::npos) alias = alias.substr(dot + 1);
                    }

                    for (const auto& function : module->functions) {
                        checker.registerExternalFunction(
                            imported.packageName + "." + function.name,
                            function.returnType,
                            function.parameterTypes
                        );
                        if (imported.namespaceMode) {
                            checker.registerExternalFunction(
                                alias + "." + function.name,
                                function.returnType,
                                function.parameterTypes
                            );
                        } else {
                            checker.registerExternalFunction(
                                function.name,
                                function.returnType,
                                function.parameterTypes
                            );
                        }
                    }
                }
            };

            for (const auto& module : llvmModuleGraph.dependencyOrder) {
                auto moduleTokens = LexerParser::parse(module.source);
                auto moduleAst = TreeParser::parse(moduleTokens);
                ZeusTypeInfer::TypeChecker moduleChecker(moduleAst);
                registerImportedInterfaces(moduleChecker, module.imports);
                if (!moduleChecker.check(ZeusTypeInfer::TypeCheckOptions::llvm())) {
                    for (const auto& diagnostic : moduleChecker.getDiagnostics()) {
                        if (diagnostic.severity == ZeusTypeInfer::TypeCheckSeverity::Error) {
                            std::cout << "[emit-llvm] type error in module '" << module.packageName
                                      << "' (node " << diagnostic.nodeIndex << "): "
                                      << diagnostic.message << std::endl;
                        }
                    }
                    return 1;
                }
            }

            auto typingTokens = LexerParser::parse(code);
            auto typingAst = TreeParser::parse(typingTokens);
            ZeusTypeInfer::TypeChecker typeChecker(typingAst);
            registerImportedInterfaces(typeChecker, llvmModuleGraph.rootImports);
            if (!typeChecker.check(ZeusTypeInfer::TypeCheckOptions::llvm())) {
                for (const auto& diagnostic : typeChecker.getDiagnostics()) {
                    if (diagnostic.severity == ZeusTypeInfer::TypeCheckSeverity::Error) {
                        std::cout << "[emit-llvm] type error (node " << diagnostic.nodeIndex
                                  << "): " << diagnostic.message << std::endl;
                    }
                }
                return 1;
            }
        }

        // 第一阶段:预扫描import语句
        ZEUS_DEBUG_PRINT("[Main] Phase 1: Pre-scanning imports...");
        
        // 快速词法分析找imports
        // Reuse the tokenized source snapshot created before module planning.
        
        // 简单扫描:找 "import" keyword后跟package name  
        std::vector<std::string> importedPackages;
        for (size_t i = 0; i < prescanTokens.size(); ++i) {
            if (std::holds_alternative<LexerType::Keyword>(prescanTokens[i])) {
                auto& kw = std::get<LexerType::Keyword>(prescanTokens[i]);
                if (kw.name == "import" && i + 1 < prescanTokens.size()) {
                    // 收集包名(跳过import关键字,从i+1开始)
                    std::string packageName;
                    bool started = false;
                    
                    for (size_t j = i + 1; j < prescanTokens.size(); ++j) {
                        if (std::holds_alternative<LexerType::Keyword>(prescanTokens[j])) {
                            // 添加keyword到包名
                            auto& pkgkw = std::get<LexerType::Keyword>(prescanTokens[j]);
                            packageName += pkgkw.name;
                            started = true;
                        } else if (std::holds_alternative<LexerType::Value>(prescanTokens[j])) {
                            // 添加value(标识符)到包名
                            auto& pkgval = std::get<LexerType::Value>(prescanTokens[j]);
                            packageName += pkgval.toString();
                            started = true;
                        } else if (std::holds_alternative<LexerType::Operator>(prescanTokens[j])) {
                            auto op = std::get<LexerType::Operator>(prescanTokens[j]).toString();
                            if (op == ".") {
                                packageName += ".";
                            } else if (op == ";" || op == "[") {
                                break;  // 包名结束
                            } else {
                                break;  // 其他操作符结束包名
                            }
                        } else {
                            // 其他类型token,结束包名收集
                            if (started) break;
                        }
                    }
                    
                    if (!packageName.empty()) {
                        importedPackages.push_back(packageName);
                        ZEUS_DEBUG_PRINT("[Main] Pre-scanned import: %s", packageName.c_str());
                    }
                }
            }
        }

        if (emitLLVM) {
            importedPackages.clear();
            for (const auto& module : llvmModuleGraph.dependencyOrder) {
                importedPackages.push_back(module.packageName);
            }
        }
        
        builtinCompilationPlan.prependMissingModules(importedPackages);

        std::vector<ZeusPackage::CompiledPackage*> packages;
        std::map<std::string, int> packageGlobalOffsets;  // package名 -> 全局变量起始索引
        int packageGlobalCount = 0;  // 记录包注册的全局变量数量
        
        if (emitLLVM) IRScope::clearImportedSymbols();
        if (emitLLVM) ZeusPackage::loadedPackages.clear();

        ZeusPackage::PackageLoadGraph vmPackageGraph;
        if (!emitLLVM) {
            vmPackageGraph = ZeusPackage::buildPackageLoadGraph(importedPackages, srcPath);
            if (!vmPackageGraph.ok()) {
                std::cout << vmPackageGraph.error << std::endl;
                return 1;
            }
            importedPackages = vmPackageGraph.dependencyOrder;
            ZEUS_DEBUG_PRINT("[Main] VM package dependency order contains %zu packages", importedPackages.size());
        }

        if (!importedPackages.empty()) {
            ZEUS_DEBUG_PRINT("[Main] Phase 2: Loading packages (without re-registering globals)...");
            
            for (const auto& packageName : importedPackages) {
                ZEUS_DEBUG_PRINT("[Main] Loading package: %s", packageName.c_str());

                if (emitLLVM) {
                    IRScope::clearImportedSymbols();
                    const ZeusLLVM::ModuleNode* currentModule = nullptr;
                    for (const auto& module : llvmModuleGraph.dependencyOrder) {
                        if (module.packageName == packageName) {
                            currentModule = &module;
                            break;
                        }
                    }

                    if (currentModule != nullptr) {
                        for (const auto& imported : currentModule->imports) {
                            ZeusPackage::CompiledPackage* dependency = nullptr;
                            for (auto* loaded : packages) {
                                if (loaded && loaded->packageName == imported.packageName) {
                                    dependency = loaded;
                                    break;
                                }
                            }
                            if (dependency == nullptr) continue;

                            std::string alias = imported.alias;
                            if (alias.empty()) {
                                alias = imported.packageName;
                                const size_t dot = alias.find_last_of('.');
                                if (dot != std::string::npos) alias = alias.substr(dot + 1);
                            }

                            const int dependencyOffset = packageGlobalOffsets[dependency->packageName];
                            const ZeusLLVM::ModuleNode* dependencyModule = nullptr;
                            for (const auto& module : llvmModuleGraph.dependencyOrder) {
                                if (module.packageName == dependency->packageName) {
                                    dependencyModule = &module;
                                    break;
                                }
                            }

                            for (size_t gi = 0; gi < dependency->globalNames.size(); ++gi) {
                                const std::string& globalName = dependency->globalNames[gi];
                                const int globalIndex = dependencyOffset + static_cast<int>(gi);
                                const std::string localName = imported.namespaceMode
                                    ? alias + "." + globalName
                                    : globalName;
                                IRScope::importedGlobalSlots[localName] = globalIndex;

                                if (dependencyModule != nullptr) {
                                    for (const auto& function : dependencyModule->functions) {
                                        if (function.name != globalName) continue;
                                        IRScope::importedFunctionNames.insert(localName);
                                        IRScope::importedFunctionTargets[localName] =
                                            dependency->packageName + "." + globalName;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
                
                std::map<int, std::string> vmNamespaceAliasSentinels;
                std::map<int, int> vmImportedGlobalSlotSentinels;
                if (!emitLLVM) {
                    IRScope::clearImportedSymbols();
                    int nextImportSentinel = 0;
                    auto moduleImports = vmPackageGraph.importsByPackage.find(packageName);
                    if (moduleImports != vmPackageGraph.importsByPackage.end()) {
                        for (const auto& imported : moduleImports->second) {
                            ZeusPackage::CompiledPackage* dependency =
                                ZeusPackage::findPackage(packages, imported.packageName);
                            if (dependency == nullptr) {
                                std::cout << "Package dependency was not loaded before '" << packageName
                                          << "': " << imported.packageName << std::endl;
                                return 1;
                            }

                            const std::string alias = ZeusPackage::defaultImportAlias(imported);
                            if (imported.importMode == 1) {
                                auto dependencyOffset = packageGlobalOffsets.find(dependency->packageName);
                                if (dependencyOffset == packageGlobalOffsets.end()) {
                                    std::cout << "Package global layout is missing dependency '"
                                              << dependency->packageName << "'" << std::endl;
                                    return 1;
                                }
                                const int sentinel = -2 - nextImportSentinel++;
                                IRScope::importedGlobalSlots[alias] = sentinel;
                                vmNamespaceAliasSentinels[sentinel] =
                                    ZeusPackage::moduleNamespaceGlobalName(packageName, alias);
                                for (const auto& functionName : dependency->functionNames) {
                                    auto functionIt = std::find(
                                        dependency->globalNames.begin(), dependency->globalNames.end(), functionName);
                                    if (functionIt == dependency->globalNames.end()) continue;
                                    const std::string localName = alias + "." + functionName;
                                    const int functionSentinel = -2 - nextImportSentinel++;
                                    IRScope::importedGlobalSlots[localName] = functionSentinel;
                                    vmImportedGlobalSlotSentinels[functionSentinel] =
                                        dependencyOffset->second + static_cast<int>(std::distance(
                                            dependency->globalNames.begin(), functionIt));
                                    IRScope::importedFunctionNames.insert(localName);
                                    IRScope::importedFunctionTargets[localName] =
                                        dependency->packageName + "." + functionName;
                                }
                                continue;
                            }

                            auto offset = packageGlobalOffsets.find(dependency->packageName);
                            if (offset == packageGlobalOffsets.end()) {
                                std::cout << "Package global layout is missing dependency '"
                                          << dependency->packageName << "'" << std::endl;
                                return 1;
                            }
                            for (size_t gi = 0; gi < dependency->globalNames.size(); ++gi) {
                                const int sentinel = -2 - nextImportSentinel++;
                                IRScope::importedGlobalSlots[dependency->globalNames[gi]] = sentinel;
                                vmImportedGlobalSlotSentinels[sentinel] =
                                    offset->second + static_cast<int>(gi);
                                if (dependency->functionNames.find(dependency->globalNames[gi]) !=
                                    dependency->functionNames.end()) {
                                    IRScope::importedFunctionNames.insert(dependency->globalNames[gi]);
                                    IRScope::importedFunctionTargets[dependency->globalNames[gi]] =
                                        dependency->packageName + "." + dependency->globalNames[gi];
                                }
                            }
                        }
                    }
                }

                ZeusPackage::CompiledPackage* pkg = nullptr;
                if (emitLLVM) {
                    const ZeusLLVM::ModuleNode* module = nullptr;
                    for (const auto& candidate : llvmModuleGraph.dependencyOrder) {
                        if (candidate.packageName == packageName) {
                            module = &candidate;
                            break;
                        }
                    }
                    if (module == nullptr) {
                        std::cout << "[emit-llvm] module graph is missing '" << packageName << "'" << std::endl;
                        return 1;
                    }
                    try {
                        auto compiled = ZeusPackage::compilePackage(packageName, module->path, strictDecl);
                        ZeusPackage::loadedPackages[packageName] = std::move(compiled);
                        pkg = &ZeusPackage::loadedPackages[packageName];
                    } catch (const std::exception& ex) {
                        std::cout << "[emit-llvm] module compile failed for '" << packageName
                                  << "': " << ex.what() << std::endl;
                        return 1;
                    }
                } else {
                    pkg = ZeusPackage::loadPackage(packageName, srcPath, strictDecl);
                    if (pkg != nullptr) {
                        pkg->namespaceAliasSentinels = std::move(vmNamespaceAliasSentinels);
                        pkg->importedGlobalSlotSentinels = std::move(vmImportedGlobalSlotSentinels);
                    }
                }
                
                if (pkg) {
                    packages.push_back(pkg);

                    if (emitLLVM) {
                        packageGlobalOffsets[packageName] = packageGlobalCount;
                        for (size_t gi = 0; gi < pkg->globalNames.size(); ++gi) {
                            IRScope::globals[packageName + "." + pkg->globalNames[gi]] =
                                packageGlobalCount + static_cast<int>(gi);
                        }
                        packageGlobalCount += static_cast<int>(pkg->globalNames.size());
                    } else {
                        packageGlobalOffsets[packageName] = packageGlobalCount;
                        packageGlobalCount += static_cast<int>(pkg->globalNames.size());
                    }
                    
                    ZEUS_DEBUG_PRINT("[Main] Package '%s' loaded: globalNames.size()=%zu", 
                        packageName.c_str(), pkg->globalNames.size());
                } else {
                    std::cout << "Failed to load package: " << packageName << std::endl;
                    return 1;
                }
            }
        }

        // 第2.5阶段:计算包全局变量布局,使用包名前缀注册到IRScope(避免同名冲突)
        ZEUS_DEBUG_PRINT("[Main] Phase 2.5: Calculating package global layout and registering with prefixes...");
        
        // 清空之前包编译时注册的全局变量(可能有重名)
        // 不要保留任何名字,内置函数应该通过ZeusBuiltins::findId查找,不应在IRScope中
        IRScope::clearImportedSymbols();
        IRScope::globals.clear();
        
        // 重新计算packageGlobalCount和offsets,并使用包名前缀注册
        packageGlobalCount = 0;
        for (auto* pkg : packages) {
            if (!pkg) continue;
            
            // 记录此包的全局变量起始索引
            packageGlobalOffsets[pkg->packageName] = packageGlobalCount;
            
            // 使用包名前缀注册每个全局变量到IRScope,避免同名冲突
            for (const auto& globalName : pkg->globalNames) {
                std::string prefixedName = pkg->packageName + "." + globalName;
                IRScope::declareGlobal(prefixedName);
                ZEUS_DEBUG_PRINT("[Main] Registered package global: %s (index=%d)", 
                    prefixedName.c_str(), packageGlobalCount);
                packageGlobalCount++;
            }
            
            ZEUS_DEBUG_PRINT("[Main] Package '%s': offset=%d, count=%zu, total=%d", 
                pkg->packageName.c_str(), 
                packageGlobalOffsets[pkg->packageName],
                pkg->globalNames.size(),
                packageGlobalCount);
        }
        
        ZEUS_DEBUG_PRINT("[Main] Total package globals: %d", packageGlobalCount);
        
        // 第2.6阶段: 为自动导入的包注册全局别名（global import 模式）
        ZEUS_DEBUG_PRINT("[Main] Phase 2.6: Registering global aliases for auto-imported packages...");
        
        const auto& autoImportedPackages = builtinCompilationPlan.unqualifiedModules;
        
        for (const auto& pkgName : autoImportedPackages) {
            for (auto* pkg : packages) {
                if (pkg && pkg->packageName == pkgName) {
                    ZEUS_DEBUG_PRINT("[Main] Registering %zu global aliases for auto-imported package: %s", 
                        pkg->globalNames.size(), pkgName.c_str());
                        
                    for (const auto& globalName : pkg->globalNames) {
                        std::string prefixedName = pkgName + "." + globalName;
                        
                        auto it = IRScope::globals.find(prefixedName);
                        if (it != IRScope::globals.end()) {
                            int existingIndex = it->second;
                            
                            // 注册不带前缀的版本，使用相同索引
                            IRScope::globals[globalName] = existingIndex;
                            ZEUS_DEBUG_PRINT("[Main] Registered auto-import alias: %s -> index %d (same as %s)", 
                                globalName.c_str(), existingIndex, prefixedName.c_str());
                        }
                    }
                    break;
                }
            }
        }
        
        // 第2.75阶段:注册 namespace import 的别名
        ZEUS_DEBUG_PRINT("[Main] Phase 2.75: Pre-registering namespace import aliases...");

        const auto& prescanTokensForAlias = prescanTokens;

        for (size_t i = 0; i < prescanTokensForAlias.size(); ++i) {
            if (std::holds_alternative<LexerType::Keyword>(prescanTokensForAlias[i])) {
                auto& kw = std::get<LexerType::Keyword>(prescanTokensForAlias[i]);

                if (kw.name == "import" && i + 1 < prescanTokensForAlias.size()) {
                    std::string pkgName;
                    std::string aliasName;
                    bool isNamespaceMode = false;
                    bool foundBracket = false;

                    for (size_t j = i + 1; j < prescanTokensForAlias.size(); ++j) {
                        if (std::holds_alternative<LexerType::Operator>(prescanTokensForAlias[j])) {
                            auto op = std::get<LexerType::Operator>(prescanTokensForAlias[j]).toString();

                            if (op == "[") {
                                foundBracket = true;
                            }
                            else if (op == "&" && foundBracket) {
                                isNamespaceMode = true;
                            }
                            else if (op == ";") {
                                break;
                            }
                            else if (op == ".") {
                                if (!foundBracket) pkgName += ".";
                            }
                        }
                        else if (std::holds_alternative<LexerType::Keyword>(prescanTokensForAlias[j])) {
                            auto& tokKw = std::get<LexerType::Keyword>(prescanTokensForAlias[j]);

                            if (foundBracket && isNamespaceMode) {
                                aliasName = tokKw.name;
                            }
                            else if (!foundBracket) {
                                pkgName += tokKw.name;
                            }
                        }
                    }

                    if (isNamespaceMode) {
                        if (aliasName.empty()) {
                            aliasName = pkgName;
                            
                            // 如果包名包含点号,只使用最后一段作为别名
                            // 例如: "ZeusBuiltins.math" -> "math"
                            size_t lastDot = aliasName.find_last_of('.');
                            if (lastDot != std::string::npos && lastDot + 1 < aliasName.size()) {
                                aliasName = aliasName.substr(lastDot + 1);
                                ZEUS_DEBUG_PRINT("[Main] Adjusted alias for '%s' to '%s' (removed package prefix)", 
                                    pkgName.c_str(), aliasName.c_str());
                            }
                        }

                        ZEUS_DEBUG_PRINT("[Main] About to register namespace alias: '%s' (pkgName='%s', isNamespaceMode=%d)", 
                            aliasName.c_str(), pkgName.c_str(), isNamespaceMode ? 1 : 0);
                        IRScope::declareGlobal(aliasName);
                        ZEUS_DEBUG_PRINT("[Main] Pre-registered namespace alias: %s", aliasName.c_str());
                    }
                    else if (!pkgName.empty()) {
                        // Global import模式: 为包的所有全局变量注册不带前缀的别名
                        // 这样主程序可以直接使用 PI, fibonacci 等名称访问
                        ZEUS_DEBUG_PRINT("[Main] Global import detected for package: %s", pkgName.c_str());
                        
                        // 查找对应的包
                        for (auto* pkg : packages) {
                            if (pkg && pkg->packageName == pkgName) {
                                ZEUS_DEBUG_PRINT("[Main] Registering %zu global import aliases for package: %s", 
                                    pkg->globalNames.size(), pkgName.c_str());
                                    
                                for (const auto& globalName : pkg->globalNames) {
                                    // 同时注册带前缀和不带前缀的版本
                                    // 带前缀的已经在Phase 2.5注册,这里只注册不带前缀的
                                    // 它们会映射到相同的索引
                                    std::string prefixedName = pkgName + "." + globalName;
                                    
                                    // 获取带前缀版本的索引
                                    auto it = IRScope::globals.find(prefixedName);
                                    if (it != IRScope::globals.end()) {
                                        int existingIndex = it->second;
                                        
                                        // 注册不带前缀的版本,使用相同索引
                                        IRScope::globals[globalName] = existingIndex;
                                        ZEUS_DEBUG_PRINT("[Main] Registered global import alias: %s -> index %d (same as %s)", 
                                            globalName.c_str(), existingIndex, prefixedName.c_str());
                                    }
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }

        // 第三阶段:编译主程序(现在能识别包的全局变量和namespace别名)
        if (emitLLVM) {
            IRScope::clearImportedSymbols();
            for (auto* package : packages) {
                if (package == nullptr) continue;
                const int offset = packageGlobalOffsets[package->packageName];
                const ZeusLLVM::ModuleNode* module = nullptr;
                for (const auto& candidate : llvmModuleGraph.dependencyOrder) {
                    if (candidate.packageName == package->packageName) {
                        module = &candidate;
                        break;
                    }
                }
                if (module == nullptr) continue;
                ZEUS_DEBUG_PRINT("[emit-llvm] module '%s' interface functions=%zu, globals=%zu",
                    module->packageName.c_str(), module->functions.size(), package->globalNames.size());
                for (const auto& function : module->functions) {
                    ZEUS_DEBUG_PRINT("[emit-llvm] module '%s' interface function='%s'",
                        module->packageName.c_str(), function.name.c_str());
                    auto nameIt = std::find(package->globalNames.begin(), package->globalNames.end(), function.name);
                    if (nameIt == package->globalNames.end()) continue;
                    const int index = offset + static_cast<int>(std::distance(package->globalNames.begin(), nameIt));
                    const std::string qualifiedName = package->packageName + "." + function.name;
                    IRScope::importedGlobalSlots[qualifiedName] = index;
                    IRScope::importedFunctionNames.insert(qualifiedName);
                    IRScope::importedFunctionTargets[qualifiedName] = qualifiedName;
                }
            }

            for (const auto& imported : llvmModuleGraph.rootImports) {
                ZeusPackage::CompiledPackage* package = nullptr;
                for (auto* candidate : packages) {
                    if (candidate && candidate->packageName == imported.packageName) {
                        package = candidate;
                        break;
                    }
                }
                if (package == nullptr) continue;

                std::string alias = imported.alias;
                if (alias.empty()) {
                    alias = imported.packageName;
                    const size_t dot = alias.find_last_of('.');
                    if (dot != std::string::npos) alias = alias.substr(dot + 1);
                }

                const int offset = packageGlobalOffsets[package->packageName];
                const ZeusLLVM::ModuleNode* module = nullptr;
                for (const auto& candidate : llvmModuleGraph.dependencyOrder) {
                    if (candidate.packageName == package->packageName) {
                        module = &candidate;
                        break;
                    }
                }

                for (size_t gi = 0; gi < package->globalNames.size(); ++gi) {
                    const std::string& globalName = package->globalNames[gi];
                    const std::string localName = imported.namespaceMode
                        ? alias + "." + globalName
                        : globalName;
                    IRScope::importedGlobalSlots[localName] = offset + static_cast<int>(gi);
                    bool isFunction = false;
                    if (module != nullptr) {
                        for (const auto& function : module->functions) {
                            if (function.name != globalName) continue;
                            isFunction = true;
                            IRScope::importedFunctionNames.insert(localName);
                            IRScope::importedFunctionTargets[localName] =
                                package->packageName + "." + globalName;
                            break;
                        }
                    }
                    ZEUS_DEBUG_PRINT("[emit-llvm] import binding '%s' -> global %d (function=%d)",
                        localName.c_str(), offset + static_cast<int>(gi), isFunction ? 1 : 0);
                }
            }
        }

        if (!emitLLVM) {
            for (const auto& imported : ZeusPackage::extractImportDeclarations(code)) {
                ZeusPackage::CompiledPackage* package =
                    ZeusPackage::findPackage(packages, imported.packageName);
                if (package == nullptr) continue;
                const std::string alias = ZeusPackage::defaultImportAlias(imported);
                const int offset = packageGlobalOffsets[package->packageName];
                for (const auto& functionName : package->functionNames) {
                    auto functionIt = std::find(
                        package->globalNames.begin(), package->globalNames.end(), functionName);
                    if (functionIt == package->globalNames.end()) continue;
                    const std::string localName = imported.importMode == 1
                        ? alias + "." + functionName
                        : functionName;
                    IRScope::importedGlobalSlots[localName] = offset + static_cast<int>(std::distance(
                        package->globalNames.begin(), functionIt));
                    IRScope::importedFunctionNames.insert(localName);
                    IRScope::importedFunctionTargets[localName] =
                        package->packageName + "." + functionName;
                }
            }
        }

        ZEUS_DEBUG_PRINT("[Main] Phase 3: Preparing to compile main program...");
        ZEUS_DEBUG_PRINT("[Main] Phase 3: packageGlobalCount = %d", packageGlobalCount);

        size_t globalsBeforeMain = IRScope::globals.size();
        ZEUS_DEBUG_PRINT("[Main] IRScope::globals already contains %zu entries from packages", globalsBeforeMain);

        // 重置 useOriginal 标志，确保主程序使用默认的包装行为
        IRScope::useOriginalList = false;
        IRScope::useOriginalBytes = false;
        IRScope::useOriginalString = false;
        
        // 同时重置 originalModeStack 回初始状态
        IRScope::originalModeStack.clear();
        IRScope::originalModeStack.push_back(IRScope::OriginalTypeMode());
        
        ZEUS_DEBUG_PRINT("[Main] useOriginal flags RESET to false before compiling main program");

        ZEUS_DEBUG_PRINT("[Main] Compiling main program (package globals: %d)...", packageGlobalCount);

        ZeusCompile::Result cr = ZeusCompile::compile(code, strictDecl, emitLLVM ? false : enableHotspot);
        
        // 设置源文件信息用于错误报告
        cr.sourceFilePath = srcPath;
        cr.sourceCode = code;
        
        ZEUS_DEBUG_PRINT("[Main] Compile done. cr.globalNames.size()=%zu", cr.globalNames.size());
        for (size_t i = 0; i < cr.globalNames.size(); ++i) {
            ZEUS_DEBUG_PRINT("  cr.globalNames[%zu] = '%s'", i, cr.globalNames[i].c_str());
        }
        
        // 记录编译前的全局变量数量(包括包的全局变量和namespace别名)
        size_t globalsIncludingAliases = globalsBeforeMain;
        
        // Extract only the NEW globals added by main program (similar to package compilation)
        // 包括namespace别名,因为mergePackageChunks需要知道它们的索引
        std::vector<std::string> mainAndAliasGlobals;

        // cr.globalNames包含: 包全局变量(带前缀) + 别名 + 主程序新变量
        // 我们需要提取从packageGlobalCount开始的所有内容(别名 + 主程序变量)
        if ((int)cr.globalNames.size() > packageGlobalCount) {
            mainAndAliasGlobals = std::vector<std::string>(
                cr.globalNames.begin() + packageGlobalCount,
                cr.globalNames.end()
            );

            ZEUS_DEBUG_PRINT("[Main] Main program + aliases added %zu new globals (package globals: %d, total: %zu)",
                mainAndAliasGlobals.size(), packageGlobalCount, cr.globalNames.size());
            
            // Debug: 打印mainAndAliasGlobals内容
            for (size_t i = 0; i < mainAndAliasGlobals.size(); ++i) {
                ZEUS_DEBUG_PRINT("  [%zu] %s", i, mainAndAliasGlobals[i].c_str());
            }

            cr.globalNames = mainAndAliasGlobals;
        }
        else {
            // cr.globalNames.size() == packageGlobalCount: 没有别名也没有新变量
            ZEUS_DEBUG_PRINT("[Main] Main program added no new globals beyond packages (total=%zu, package=%d)", 
                cr.globalNames.size(), packageGlobalCount);
            cr.globalNames.clear();
        }

        if (cr.program.empty()) {
            std::cout << "Compile failed (IR stopped)." << std::endl;
            return 1;
        }

        // 第四阶段:合并包chunks
        if (!packages.empty()) {
            ZEUS_DEBUG_PRINT("[Main] Phase 4: Merging package chunks...");
            
            // 提取imports (获取别名信息)
            auto imports = ZeusPackage::extractImports(cr.program);
            
            // 合并包到主程序
            // 注意: 传入mainAndAliasGlobals,这样索引从0开始,对应运行时的packageGlobalCount+0...
            ZeusPackage::mergePackageChunks(cr.program, mainAndAliasGlobals, packages, imports, packageGlobalOffsets);
            
            // mergePackageChunks修改了mainAndAliasGlobals,现在它包含完整的globalNames (package + main)
            // 需要赋值回cr.globalNames,这样VM才能正确访问
            cr.globalNames = mainAndAliasGlobals;
        }

        if (emitLLVM) {
            IRStaticOperator::annotate(cr.program);
            IRStaticCall::annotate(cr.program);
            ZeusLLVM::EmitResult emitted = ZeusLLVM::emit(cr);
            for (const auto& warning : emitted.warnings) {
                std::cout << "[emit-llvm] warning: " << warning << std::endl;
            }
            if (!emitted.ok) {
                std::cout << emitted.error << std::endl;
                return 1;
            }

            const std::string outPath = emitOut.empty() ? (srcPath + ".ll") : emitOut;
            std::ofstream output(outPath);
            if (!output) {
                std::cout << "Cannot open output file: " << outPath << std::endl;
                return 1;
            }
            output << emitted.text;
            std::cout << "Emit success -> " << outPath << std::endl;
            return 0;
        }

        if (ZeusDebug::enabled() && ZeusDebug::channelEnabled(ZeusDebug::CH_IR)) {
            std::cout << "-------------------- IR Gen --------------------" << std::endl;
            for (size_t ci = 0; ci < cr.program.size(); ++ci) {
                std::cout << "== Chunk #" << ci << " ==" << std::endl;
                for (auto &ins : cr.program[ci]) {
                    std::cout << ins.toString() << std::endl;
                }
            }
            std::cout << "-------------------- Output --------------------" << std::endl;
        }
        
        if (compileSingle) {
            std::string outPath = srcPath + ".zir";
            std::string err;
            if (ZeusCompile::write(cr, outPath, &err)) {
                std::cout << "Compile success -> " << outPath << std::endl;
            } else {
                std::cout << "Write failed: " << err << std::endl;
                return 1;
            }
            return 0;
        }
        ZeusCompile::run(cr);
        
        return 0;
    }
