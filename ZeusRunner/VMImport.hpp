#ifndef VMImport_hpp
#define VMImport_hpp

#include <string>
#include <vector>
#include <unordered_map>
#include "../ZeusIR/IRImport.hpp"
#include "../ZeusDebug/Utils.hpp"

namespace VMImport {

    // VM运行时的包命名空间管理
    struct PackageNamespace {
        std::string packageName;
        int chunkStartIndex;  // 包的chunk起始索引
        int chunkEndIndex;    // 包的chunk结束索引
        std::unordered_map<std::string, int> exports; // 导出符号 -> globalIndex
    };

    // 包命名空间缓存: packageName -> PackageNamespace
    static std::unordered_map<std::string, PackageNamespace> loadedPackages;

    // 检查包是否已加载
    inline bool isPackageLoaded(const std::string& packageName) {
        return loadedPackages.find(packageName) != loadedPackages.end();
    }

    // 注册加载的包
    inline void registerPackage(const std::string& packageName, const PackageNamespace& ns) {
        loadedPackages[packageName] = ns;
        
        ZEUS_DEBUG_PRINT("[VMImport] Registered package: %s (chunks: %d-%d)",
            packageName.c_str(), ns.chunkStartIndex, ns.chunkEndIndex);
    }

    // 获取包命名空间
    inline PackageNamespace* getPackage(const std::string& packageName) {
        auto it = loadedPackages.find(packageName);
        
        if (it != loadedPackages.end()) {
            return &(it->second);
        }
        
        return nullptr;
    }

} // namespace VMImport

#endif
