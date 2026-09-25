#ifndef IRImport_hpp
#define IRImport_hpp

#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <fstream>
#include <sstream>
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

    // 设置Zeus可执行文件目录
    inline void setZeusExecutableDir(const std::string& exePath) {
        std::filesystem::path p(exePath);
        
        if (p.has_parent_path()) {
            zeusExecutableDir = p.parent_path().string();
        } else {
            zeusExecutableDir = std::filesystem::current_path().string();
        }
        
        ZEUS_DEBUG_PRINT("[IRImport] Set Zeus executable directory: %s", zeusExecutableDir.c_str());
    }

    // 设置当前工作目录
    inline void setWorkingDirectory(const std::string& sourceFilePath) {
        std::filesystem::path p(sourceFilePath);
        
        if (p.has_parent_path()) {
            currentWorkingDir = p.parent_path().string();
        } else {
            currentWorkingDir = std::filesystem::current_path().string();
        }
        
        ZEUS_DEBUG_PRINT("[IRImport] Set working directory: %s", currentWorkingDir.c_str());
    }

    // 解析包路径: "package.math" -> "package/math.zeu"
    // 查找顺序:
    // 1. Zeus.exe所在目录的ZeusBuiltins/
    // 2. 当前脚本所在目录
    inline std::string resolvePackagePath(const std::string& packageName) {
        std::string relPath = packageName;
        
        // 替换 '.' 为路径分隔符
        for (char& c : relPath) {
            if (c == '.') {
                c = std::filesystem::path::preferred_separator;
            }
        }
        
        // 添加 .zeu 后缀
        relPath += ".zeu";
        
        // 1. 先尝试从Zeus.exe所在目录的ZeusBuiltins/查找
        if (!zeusExecutableDir.empty()) {
            std::filesystem::path builtinPath = std::filesystem::path(zeusExecutableDir) / "ZeusBuiltins" / relPath;
            
            ZEUS_DEBUG_PRINT("[IRImport] Trying builtin path: %s", builtinPath.string().c_str());
            
            if (std::filesystem::exists(builtinPath)) {
                ZEUS_DEBUG_PRINT("[IRImport] Found in builtins: '%s' -> '%s'", packageName.c_str(), builtinPath.string().c_str());
                return builtinPath.string();
            }
        }
        
        // 2. 再尝试从当前脚本所在目录查找
        if (!currentWorkingDir.empty()) {
            std::filesystem::path workingPath = std::filesystem::path(currentWorkingDir) / relPath;
            
            ZEUS_DEBUG_PRINT("[IRImport] Trying working dir path: %s", workingPath.string().c_str());
            
            if (std::filesystem::exists(workingPath)) {
                ZEUS_DEBUG_PRINT("[IRImport] Found in working dir: '%s' -> '%s'", packageName.c_str(), workingPath.string().c_str());
                return workingPath.string();
            }
        }
        
        // 3. 最后尝试当前目录(兼容旧行为)
        std::filesystem::path currentPath(relPath);
        if (std::filesystem::exists(currentPath)) {
            ZEUS_DEBUG_PRINT("[IRImport] Found in current dir: '%s' -> '%s'", packageName.c_str(), currentPath.string().c_str());
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
        packageCache[packageName] = info;
        
        ZEUS_DEBUG_PRINT("[IRImport] Cached package: %s", packageName.c_str());
    }

    // 获取缓存的包
    inline PackageInfo* getCachedPackage(const std::string& packageName) {
        auto it = packageCache.find(packageName);
        
        if (it != packageCache.end()) {
            ZEUS_DEBUG_PRINT("[IRImport] Found cached package: %s", packageName.c_str());
            return &(it->second);
        }
        
        return nullptr;
    }

    // 加载包 (返回包文件的完整路径)
    inline std::string loadPackage(const std::string& packageName) {
        // 检查缓存
        PackageInfo* cached = getCachedPackage(packageName);
        
        if (cached && cached->isLoaded) {
            ZEUS_DEBUG_PRINT("[IRImport] Package already loaded: %s", packageName.c_str());
            return cached->fullPath;
        }
        
        // 解析路径
        std::string packagePath = resolvePackagePath(packageName);
        
        // 检查文件是否存在
        if (!packageExists(packagePath)) {
            ZEUS_DEBUG_PRINT("[IRImport] Package not found: %s", packageName.c_str());
            return "";
        }
        
        // 创建包信息
        PackageInfo info;
        info.fullPath = packagePath;
        info.packageName = packageName;
        info.isLoaded = true;
        
        // 缓存包信息
        cachePackage(packageName, info);
        
        ZEUS_DEBUG_PRINT("[IRImport] Loaded package: %s -> %s", packageName.c_str(), packagePath.c_str());
        
        return packagePath;
    }

    // 清除包缓存 (用于调试/重新加载)
    inline void clearPackageCache() {
        packageCache.clear();
        
        ZEUS_DEBUG_PRINT("[IRImport] Cleared package cache");
    }

} // namespace IRImport

#endif
