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

    // Resolve compiler modules and project modules from explicit roots.
    //   zeus.mem          -> <compiler-dir>/ZeusBuiltins/zeus/mem.zeu
    //   ZeusBuiltins.mem  -> canonicalized to zeus.mem
    //   package.math      -> importer directory/package/math.zeu, then process CWD
    inline std::string resolvePackagePath(const std::string& packageName) {
        const std::string canonicalName = canonicalPackageName(packageName);
        std::filesystem::path relativePath;
        if (!packageNameToRelativePath(canonicalName, relativePath)) {
            ZEUS_DEBUG_PRINT("[IRImport] Invalid package name: %s", packageName.c_str());
            return "";
        }

        const bool isZeusModule = hasPackagePrefix(canonicalName, "zeus");
        if (isZeusModule) {
            if (zeusExecutableDir.empty()) return "";
            const std::filesystem::path builtinRoot =
                std::filesystem::path(zeusExecutableDir) / "ZeusBuiltins";
            const std::filesystem::path builtinPath = absoluteNormalizedPath(builtinRoot / relativePath);
            ZEUS_DEBUG_PRINT("[IRImport] Trying compiler module path: %s", builtinPath.string().c_str());
            if (std::filesystem::exists(builtinPath)) {
                ZEUS_DEBUG_PRINT("[IRImport] Found compiler module: '%s' -> '%s'",
                    canonicalName.c_str(), builtinPath.string().c_str());
                return builtinPath.string();
            }

            ZEUS_DEBUG_PRINT("[IRImport] Compiler module '%s' not found under '%s'",
                canonicalName.c_str(), builtinRoot.string().c_str());
            return "";
        }

        // Project modules are relative to the importing source, then the process CWD.
        if (!currentWorkingDir.empty()) {
            const std::filesystem::path workingPath =
                absoluteNormalizedPath(std::filesystem::path(currentWorkingDir) / relativePath);
            ZEUS_DEBUG_PRINT("[IRImport] Trying importer path: %s", workingPath.string().c_str());
            if (std::filesystem::exists(workingPath)) {
                ZEUS_DEBUG_PRINT("[IRImport] Found project module: '%s' -> '%s'",
                    packageName.c_str(), workingPath.string().c_str());
                return workingPath.string();
            }
        }

        const std::filesystem::path currentPath = absoluteNormalizedPath(relativePath);
        if (std::filesystem::exists(currentPath)) {
            ZEUS_DEBUG_PRINT("[IRImport] Found project module in CWD: '%s' -> '%s'",
                packageName.c_str(), currentPath.string().c_str());
            return currentPath.string();
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
