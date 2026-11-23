#ifndef IRImport_hpp
#define IRImport_hpp

#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>
#include <cctype>
#include "../ZeusDebug/Utils.hpp"

namespace IRImport {

    // 包信息结构
    struct PackageInfo {
        std::string fullPath;           // 完整文件路径
        std::string packageName;        // 包名 (如 "package.math")
        std::vector<std::string> exports; // 导出的符号列表
        bool isLoaded = false;
    };

    // 包缓存: packageName -> PackageInfo
    static std::unordered_map<std::string, PackageInfo> packageCache;

    // 当前文件所在目录 (用于解析相对路径)
    static std::string currentWorkingDir = "";
    
    // Zeus可执行文件所在目录 (用于查找内置包)
    static std::string zeusExecutableDir = "";

    // Normalize a path without depending on the process working directory later.
    inline std::filesystem::path absoluteNormalizedPath(const std::filesystem::path& path) {
        return std::filesystem::absolute(path).lexically_normal();
    }

    inline bool packageNameToRelativePath(const std::string& packageName, std::filesystem::path& relativePath) {
        if (packageName.empty()) return false;

        size_t segmentStart = 0;
        while (segmentStart <= packageName.size()) {
            const size_t dot = packageName.find('.', segmentStart);
            const size_t segmentEnd = dot == std::string::npos ? packageName.size() : dot;
            const std::string segment = packageName.substr(segmentStart, segmentEnd - segmentStart);
            if (segment.empty() || segment == "." || segment == ".." ||
                segment.find('/') != std::string::npos ||
                segment.find('\\') != std::string::npos ||
                segment.find(':') != std::string::npos) {
                return false;
            }

            relativePath /= segment;
            if (dot == std::string::npos) break;
            segmentStart = dot + 1;
        }

        relativePath += ".zeu";
        return true;
    }

    inline bool hasPackagePrefix(const std::string& packageName, const std::string& prefix) {
        return packageName == prefix ||
            (packageName.size() > prefix.size() &&
             packageName.compare(0, prefix.size(), prefix) == 0 &&
             packageName[prefix.size()] == '.');
    }

    // Legacy package IDs are canonicalized before entering the import graph so
    // one source file cannot be loaded under both its old and new names.
    inline std::string canonicalPackageName(const std::string& packageName) {
        static const std::string legacyRoot = "ZeusBuiltins";
        if (packageName == legacyRoot || !hasPackagePrefix(packageName, legacyRoot)) {
            return packageName;
        }
        return "zeus" + packageName.substr(legacyRoot.size());
    }

    // 设置Zeus可执行文件目录
    inline void setZeusExecutableDir(const std::string& exePath) {
        std::filesystem::path p(exePath);

        if (p.has_parent_path()) {
            zeusExecutableDir = absoluteNormalizedPath(p.parent_path()).string();
        } else {
            zeusExecutableDir = absoluteNormalizedPath(std::filesystem::current_path()).string();
        }

        ZEUS_DEBUG_PRINT("[IRImport] Set Zeus executable directory: %s", zeusExecutableDir.c_str());
    }

    // 设置当前工作目录
    inline void setWorkingDirectory(const std::string& sourceFilePath) {
        std::filesystem::path p(sourceFilePath);

        if (p.has_parent_path()) {
            currentWorkingDir = absoluteNormalizedPath(p.parent_path()).string();
        } else {
            currentWorkingDir = absoluteNormalizedPath(std::filesystem::current_path()).string();
        }

        ZEUS_DEBUG_PRINT("[IRImport] Set working directory: %s", currentWorkingDir.c_str());
    }

    // A bare module name such as "kernel32" or "bcrypt" names a library the OS
    // already knows how to find. Such a name carries no directory part, so it
    // must not be turned into a path next to the importing source; the platform
    // loader runs its own search instead, which also covers the executable
    // directory for libraries shipped beside Zeus.
    inline bool isBareLibraryName(const std::string& libraryPath) {
        if (libraryPath.empty()) return false;
        if (libraryPath.find('/') != std::string::npos ||
            libraryPath.find('\\') != std::string::npos) {
            return false;
        }
        const auto endsWithIgnoreCase = [&](const std::string& suffix) {
            if (libraryPath.size() < suffix.size()) return false;
            const size_t offset = libraryPath.size() - suffix.size();
            for (size_t index = 0; index < suffix.size(); ++index) {
                const unsigned char actual = static_cast<unsigned char>(
                    libraryPath[offset + index]);
                const unsigned char expected = static_cast<unsigned char>(suffix[index]);
                if (std::tolower(actual) != std::tolower(expected)) return false;
            }
            return true;
        };

        if (libraryPath.find('.') == std::string::npos) return true;
        return endsWithIgnoreCase(".dll") ||
            endsWithIgnoreCase(".so") ||
            endsWithIgnoreCase(".dylib") ||
            libraryPath.find(".so.") != std::string::npos;
    }

    // Native-library paths are anchored to the source file that declares the
    // import, then stored as absolute paths so .zir and AOT builds remain stable
    // when the process is launched from another working directory. Bare module
    // names are passed through unchanged so the platform loader resolves them.
    inline std::string resolveNativeLibraryPath(const std::string& libraryPath) {
        if (isBareLibraryName(libraryPath)) {
            ZEUS_DEBUG_PRINT(
                "[IRImport] Native library '%s' is a module name; "
                "leaving it to the platform loader",
                libraryPath.c_str());
            return libraryPath;
        }
        std::filesystem::path path(libraryPath);
        if (path.is_absolute()) return absoluteNormalizedPath(path).string();
        const std::filesystem::path base = currentWorkingDir.empty()
            ? absoluteNormalizedPath(std::filesystem::current_path())
            : std::filesystem::path(currentWorkingDir);
        return absoluteNormalizedPath(base / path).string();
    }

    // 在指定根目录下探测模块候选：
    // 1. 单文件形式: <root>/<pkg>.zeu
    // 2. 目录包形式: <root>/<pkg>/mod.zeu
    // 3. 目录包形式: <root>/<pkg>/main.zeu
    // 4. 目录包形式: <root>/<pkg>/<last>.zeu
    inline std::string tryResolveInRoot(const std::filesystem::path& root, const std::string& packageName) {
        if (root.empty()) return "";

        std::filesystem::path relativeDir;
        std::string lastSegment;
        size_t segmentStart = 0;
        while (segmentStart <= packageName.size()) {
            const size_t dot = packageName.find('.', segmentStart);
            const size_t segmentEnd = (dot == std::string::npos) ? packageName.size() : dot;
            const std::string segment = packageName.substr(segmentStart, segmentEnd - segmentStart);
            if (segment.empty() || segment == "." || segment == ".." ||
                segment.find('/') != std::string::npos ||
                segment.find('\\') != std::string::npos ||
                segment.find(':') != std::string::npos) {
                return "";
            }
            relativeDir /= segment;
            lastSegment = segment;
            if (dot == std::string::npos) break;
            segmentStart = dot + 1;
        }

        // 候选 1: 单文件形式 <root>/<relativeDir>.zeu
        std::filesystem::path candidateFile = absoluteNormalizedPath(root / (relativeDir.string() + ".zeu"));
        if (std::filesystem::is_regular_file(candidateFile)) {
            return candidateFile.string();
        }

        // 候选 2: 目录包入口 <root>/<relativeDir>/mod.zeu
        std::filesystem::path candidateMod = absoluteNormalizedPath(root / relativeDir / "mod.zeu");
        if (std::filesystem::is_regular_file(candidateMod)) {
            return candidateMod.string();
        }

        // 候选 3: 目录包入口 <root>/<relativeDir>/main.zeu
        std::filesystem::path candidateMain = absoluteNormalizedPath(root / relativeDir / "main.zeu");
        if (std::filesystem::is_regular_file(candidateMain)) {
            return candidateMain.string();
        }

        // 候选 4: 目录同名文件 <root>/<relativeDir>/<lastSegment>.zeu
        std::filesystem::path candidateSelf = absoluteNormalizedPath(root / relativeDir / (lastSegment + ".zeu"));
        if (std::filesystem::is_regular_file(candidateSelf)) {
            return candidateSelf.string();
        }

        return "";
    }

    // 获取用户主目录下的全局 packages 目录 (~/.zeus/packages)
    inline std::filesystem::path getUserPackagesDir() {
        const char* home = std::getenv("USERPROFILE");
        if (!home || !*home) {
            home = std::getenv("HOME");
        }
        if (home && *home) {
            return absoluteNormalizedPath(std::filesystem::path(home) / ".zeus" / "packages");
        }
        return {};
    }

    // 获取环境变量 ZEUS_PATH 指定的目录列表
    inline std::vector<std::filesystem::path> getZeusPathDirs() {
        std::vector<std::filesystem::path> result;
        const char* zeusPath = std::getenv("ZEUS_PATH");
        if (zeusPath && *zeusPath) {
            std::stringstream ss(zeusPath);
            std::string item;
            char delimiter = ';';
#ifndef _WIN32
            delimiter = ':';
#endif
            while (std::getline(ss, item, delimiter)) {
                if (!item.empty()) {
                    result.push_back(absoluteNormalizedPath(std::filesystem::path(item)));
                }
            }
        }
        return result;
    }

    // Resolve compiler modules, site-packages, user packages, and project modules:
    //   1. zeus.*          -> <compiler-dir>/ZeusBuiltins/zeus/*.zeu
    //   2. project modules -> importer directory / CWD (single file or package dir)
    //   3. ZEUS_PATH       -> directories in environment variable
    //   4. site-packages   -> <compiler-dir>/packages/ (single file or package dir)
    //   5. user-packages   -> ~/.zeus/packages/
    inline std::string resolvePackagePath(const std::string& packageName) {
        const std::string canonicalName = canonicalPackageName(packageName);

        // 1. 如果是以 zeus. 开头的内置标准库，直接优先查找 ZeusBuiltins
        const bool isZeusModule = hasPackagePrefix(canonicalName, "zeus");
        if (isZeusModule && !zeusExecutableDir.empty()) {
            const std::filesystem::path builtinRoot =
                std::filesystem::path(zeusExecutableDir) / "ZeusBuiltins";
            std::string found = tryResolveInRoot(builtinRoot, canonicalName);
            if (!found.empty()) {
                ZEUS_DEBUG_PRINT("[IRImport] Found compiler module: '%s' -> '%s'",
                    canonicalName.c_str(), found.c_str());
                return found;
            }
        }

        // 2. 当前导入源文件所在目录（本地工程私有模块，支持单文件与目录包）
        if (!currentWorkingDir.empty()) {
            std::string found = tryResolveInRoot(std::filesystem::path(currentWorkingDir), canonicalName);
            if (!found.empty()) {
                ZEUS_DEBUG_PRINT("[IRImport] Found project module: '%s' -> '%s'",
                    packageName.c_str(), found.c_str());
                return found;
            }
        }

        // 3. 当前运行工作目录 (CWD)
        {
            std::string found = tryResolveInRoot(std::filesystem::current_path(), canonicalName);
            if (!found.empty()) {
                ZEUS_DEBUG_PRINT("[IRImport] Found module in CWD: '%s' -> '%s'",
                    packageName.c_str(), found.c_str());
                return found;
            }
        }

        // 4. 环境变量 ZEUS_PATH 指定的目录列表 (类似于 PYTHONPATH)
        for (const auto& pathDir : getZeusPathDirs()) {
            std::string found = tryResolveInRoot(pathDir, canonicalName);
            if (!found.empty()) {
                ZEUS_DEBUG_PRINT("[IRImport] Found module in ZEUS_PATH '%s': '%s' -> '%s'",
                    pathDir.string().c_str(), packageName.c_str(), found.c_str());
                return found;
            }
        }

        // 5. 编译器同级全局包仓库（Site-Packages：<compiler-dir>/packages）
        if (!zeusExecutableDir.empty()) {
            const std::filesystem::path sitePackagesRoot =
                std::filesystem::path(zeusExecutableDir) / "packages";
            std::string found = tryResolveInRoot(sitePackagesRoot, canonicalName);
            if (!found.empty()) {
                ZEUS_DEBUG_PRINT("[IRImport] Found global package: '%s' -> '%s'",
                    packageName.c_str(), found.c_str());
                return found;
            }
        }

        // 6. 用户主目录全局包仓库 (~/.zeus/packages)
        {
            std::filesystem::path userPackages = getUserPackagesDir();
            if (!userPackages.empty()) {
                std::string found = tryResolveInRoot(userPackages, canonicalName);
                if (!found.empty()) {
                    ZEUS_DEBUG_PRINT("[IRImport] Found user package: '%s' -> '%s'",
                        packageName.c_str(), found.c_str());
                    return found;
                }
            }
        }

        // 7. 回退 fallback 到 ZeusBuiltins
        if (!isZeusModule && !zeusExecutableDir.empty()) {
            const std::filesystem::path builtinRoot =
                std::filesystem::path(zeusExecutableDir) / "ZeusBuiltins";
            std::string found = tryResolveInRoot(builtinRoot, canonicalName);
            if (!found.empty()) {
                ZEUS_DEBUG_PRINT("[IRImport] Found module in builtins fallback: '%s' -> '%s'",
                    canonicalName.c_str(), found.c_str());
                return found;
            }
        }

        ZEUS_DEBUG_PRINT("[IRImport] Package '%s' not found in any search path", packageName.c_str());
        return "";
    }

    // 检查包文件是否存在
    inline bool packageExists(const std::string& packagePath) {
        bool exists = std::filesystem::exists(packagePath);
        
        ZEUS_DEBUG_PRINT("[IRImport] Package '%s' exists: %d", packagePath.c_str(), exists);
        
        return exists;
    }

    // 读取包文件内容
    inline std::string readPackageContent(const std::string& packagePath) {
        std::ifstream file(packagePath);
        
        if (!file.is_open()) {
            ZEUS_DEBUG_PRINT("[IRImport] Failed to open package: %s", packagePath.c_str());
            return "";
        }
        
        std::stringstream buffer;
        buffer << file.rdbuf();
        
        std::string content = buffer.str();
        
        ZEUS_DEBUG_PRINT("[IRImport] Read package content (%zu bytes): %s", content.size(), packagePath.c_str());
        
        return content;
    }

    // 缓存包信息
    inline void cachePackage(const std::string& packageName, const PackageInfo& info) {
        const std::string canonicalName = canonicalPackageName(packageName);
        PackageInfo canonicalInfo = info;
        canonicalInfo.packageName = canonicalName;
        packageCache[canonicalName] = std::move(canonicalInfo);
        
        ZEUS_DEBUG_PRINT("[IRImport] Cached package: %s", canonicalName.c_str());
    }

    // 获取缓存的包
    inline PackageInfo* getCachedPackage(const std::string& packageName) {
        const std::string canonicalName = canonicalPackageName(packageName);
        auto it = packageCache.find(canonicalName);
        
        if (it != packageCache.end()) {
            ZEUS_DEBUG_PRINT("[IRImport] Found cached package: %s", packageName.c_str());
            return &(it->second);
        }
        
        return nullptr;
    }

    // 加载包 (返回包文件的完整路径)
    inline std::string loadPackage(const std::string& packageName) {
        const std::string canonicalName = canonicalPackageName(packageName);
        // 检查缓存
        PackageInfo* cached = getCachedPackage(canonicalName);
        
        if (cached && cached->isLoaded) {
            ZEUS_DEBUG_PRINT("[IRImport] Package already loaded: %s", canonicalName.c_str());
            return cached->fullPath;
        }
        
        // 解析路径
        std::string packagePath = resolvePackagePath(canonicalName);
        
        // 检查文件是否存在
        if (!packageExists(packagePath)) {
            ZEUS_DEBUG_PRINT("[IRImport] Package not found: %s", canonicalName.c_str());
            return "";
        }
        
        // 创建包信息
        PackageInfo info;
        info.fullPath = packagePath;
        info.packageName = canonicalName;
        info.isLoaded = true;
        
        // 缓存包信息
        cachePackage(canonicalName, info);
        
        ZEUS_DEBUG_PRINT("[IRImport] Loaded package: %s -> %s", canonicalName.c_str(), packagePath.c_str());
        
        return packagePath;
    }

    // 清除包缓存 (用于调试/重新加载)
    inline void clearPackageCache() {
        packageCache.clear();
        
        ZEUS_DEBUG_PRINT("[IRImport] Cleared package cache");
    }

} // namespace IRImport

#endif
