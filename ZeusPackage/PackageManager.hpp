#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include "../ZeusIR/IRImport.hpp"

namespace ZeusPackage {

    class PackageManager {
        public:
            static int execute(int argc, char* argv[], int startIndex) {
                if (startIndex >= argc) {
                    printHelp();
                    return 0;
                }

                std::string subcommand = argv[startIndex++];
                if (subcommand == "help" || subcommand == "--help" || subcommand == "-h") {
                    printHelp();
                    return 0;
                }

                if (subcommand == "list") {
                    return listPackages();
                }

                if (subcommand == "install") {
                    if (startIndex >= argc) {
                        std::cerr << "[Zeus pkg] Error: 'install' requires a package file (.zeu) or directory path.\n";
                        std::cerr << "Usage: zeus pkg install <path> [--user]\n";
                        return 1;
                    }
                    std::string targetPath = argv[startIndex++];
                    bool toUser = false;
                    while (startIndex < argc) {
                        std::string opt = argv[startIndex++];
                        if (opt == "--user") toUser = true;
                    }
                    return installPackage(targetPath, toUser);
                }

                if (subcommand == "uninstall" || subcommand == "remove") {
                    if (startIndex >= argc) {
                        std::cerr << "[Zeus pkg] Error: 'uninstall' requires a package name.\n";
                        std::cerr << "Usage: zeus pkg uninstall <pkg_name> [--user]\n";
                        return 1;
                    }
                    std::string pkgName = argv[startIndex++];
                    bool fromUser = false;
                    while (startIndex < argc) {
                        std::string opt = argv[startIndex++];
                        if (opt == "--user") fromUser = true;
                    }
                    return uninstallPackage(pkgName, fromUser);
                }

                if (subcommand == "link") {
                    if (startIndex >= argc) {
                        std::cerr << "[Zeus pkg] Error: 'link' requires a package directory or file path.\n";
                        std::cerr << "Usage: zeus pkg link <path>\n";
                        return 1;
                    }
                    std::string targetPath = argv[startIndex++];
                    return linkPackage(targetPath);
                }

                std::cerr << "[Zeus pkg] Unknown subcommand: '" << subcommand << "'\n\n";
                printHelp();
                return 1;
            }

        private:
            static void printHelp() {
                std::cout << "Zeus Package Manager (pip-style zero-config packages)\n\n"
                          << "Usage:\n"
                          << "  zeus pkg list                 List all installed packages\n"
                          << "  zeus pkg install <path>       Install a .zeu module or package folder\n"
                          << "  zeus pkg uninstall <name>     Remove an installed package\n"
                          << "  zeus pkg link <path>          Link a local package for live development\n"
                          << "  zeus pkg help                 Show this help message\n\n"
                          << "Options:\n"
                          << "  --user                        Target ~/.zeus/packages instead of compiler site-packages\n\n"
                          << "Resolution Order for 'import foo;':\n"
                          << "  1. zeus.* (ZeusBuiltins)\n"
                          << "  2. Current directory / Project private modules\n"
                          << "  3. ZEUS_PATH environment variable\n"
                          << "  4. Compiler site-packages (<compiler-dir>/packages)\n"
                          << "  5. User global packages (~/.zeus/packages)\n";
            }

            static std::filesystem::path getGlobalPackagesDir() {
                if (!IRImport::zeusExecutableDir.empty()) {
                    return std::filesystem::path(IRImport::zeusExecutableDir) / "packages";
                }
                return std::filesystem::current_path() / "packages";
            }

            static int listPackages() {
                const auto siteDir = getGlobalPackagesDir();
                const auto userDir = IRImport::getUserPackagesDir();

                std::cout << "=== Zeus Site Packages (" << siteDir.string() << ") ===\n";
                int countSite = scanAndPrintDirectory(siteDir);
                if (countSite == 0) {
                    std::cout << "  (no packages installed)\n";
                }

                std::cout << "\n=== Zeus User Packages (" << userDir.string() << ") ===\n";
                int countUser = scanAndPrintDirectory(userDir);
                if (countUser == 0) {
                    std::cout << "  (no user packages installed)\n";
                }

                const auto zeusPaths = IRImport::getZeusPathDirs();
                if (!zeusPaths.empty()) {
                    std::cout << "\n=== ZEUS_PATH ===\n";
                    for (const auto& p : zeusPaths) {
                        std::cout << "  " << p.string() << "\n";
                    }
                }
                return 0;
            }

            static int scanAndPrintDirectory(const std::filesystem::path& dir) {
                if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) {
                    return 0;
                }
                int count = 0;
                std::error_code ec;
                for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
                    const auto& p = entry.path();
                    if (entry.is_regular_file() && p.extension() == ".zeu") {
                        std::cout << "  * " << p.stem().string() << " (single-file module) -> " << p.filename().string() << "\n";
                        count++;
                    } else if (entry.is_directory()) {
                        std::string entryPoint;
                        if (std::filesystem::exists(p / "mod.zeu")) entryPoint = "mod.zeu";
                        else if (std::filesystem::exists(p / "main.zeu")) entryPoint = "main.zeu";
                        else if (std::filesystem::exists(p / (p.filename().string() + ".zeu"))) entryPoint = p.filename().string() + ".zeu";

                        if (!entryPoint.empty()) {
                            std::cout << "  * " << p.filename().string() << " (directory package, entry: " << entryPoint << ")\n";
                            count++;
                        }
                    }
                }
                return count;
            }

            static int installPackage(const std::string& srcPathStr, bool toUser) {
                std::filesystem::path src(srcPathStr);
                if (!std::filesystem::exists(src)) {
                    std::cerr << "[Zeus pkg] Error: Path not found: " << srcPathStr << "\n";
                    return 1;
                }

                std::filesystem::path destDir = toUser ? IRImport::getUserPackagesDir() : getGlobalPackagesDir();
                std::error_code ec;
                std::filesystem::create_directories(destDir, ec);
                if (ec) {
                    std::cerr << "[Zeus pkg] Error: Failed to create packages directory: " << destDir.string() << " (" << ec.message() << ")\n";
                    return 1;
                }

                if (std::filesystem::is_regular_file(src)) {
                    if (src.extension() != ".zeu") {
                        std::cerr << "[Zeus pkg] Error: File module must have .zeu extension: " << src.string() << "\n";
                        return 1;
                    }
                    std::filesystem::path target = destDir / src.filename();
                    std::filesystem::copy_file(src, target, std::filesystem::copy_options::overwrite_existing, ec);
                    if (ec) {
                        std::cerr << "[Zeus pkg] Error: Failed to copy file: " << ec.message() << "\n";
                        return 1;
                    }
                    std::cout << "[Zeus pkg] Successfully installed module '" << src.stem().string() << "' -> " << target.string() << "\n";
                    return 0;
                }

                if (std::filesystem::is_directory(src)) {
                    std::string pkgName = src.filename().string();
                    if (pkgName.empty() || pkgName == ".") {
                        pkgName = src.parent_path().filename().string();
                    }
                    std::filesystem::path target = destDir / pkgName;
                    std::filesystem::copy(src, target, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
                    if (ec) {
                        std::cerr << "[Zeus pkg] Error: Failed to copy package directory: " << ec.message() << "\n";
                        return 1;
                    }
                    std::cout << "[Zeus pkg] Successfully installed package '" << pkgName << "' -> " << target.string() << "\n";
                    return 0;
                }

                std::cerr << "[Zeus pkg] Error: Unsupported path type for install: " << src.string() << "\n";
                return 1;
            }

            static int uninstallPackage(const std::string& pkgName, bool fromUser) {
                std::filesystem::path targetDir = fromUser ? IRImport::getUserPackagesDir() : getGlobalPackagesDir();
                std::filesystem::path fileTarget = targetDir / (pkgName + ".zeu");
                std::filesystem::path dirTarget = targetDir / pkgName;

                bool removed = false;
                std::error_code ec;
                if (std::filesystem::exists(fileTarget)) {
                    std::filesystem::remove(fileTarget, ec);
                    if (!ec) {
                        std::cout << "[Zeus pkg] Removed single-file module: " << fileTarget.string() << "\n";
                        removed = true;
                    }
                }
                if (std::filesystem::exists(dirTarget)) {
                    std::filesystem::remove_all(dirTarget, ec);
                    if (!ec) {
                        std::cout << "[Zeus pkg] Removed directory package: " << dirTarget.string() << "\n";
                        removed = true;
                    }
                }

                if (!removed) {
                    std::cerr << "[Zeus pkg] Package '" << pkgName << "' not found in " << targetDir.string() << "\n";
                    return 1;
                }

                std::cout << "[Zeus pkg] Successfully uninstalled '" << pkgName << "'.\n";
                return 0;
            }

            static int linkPackage(const std::string& srcPathStr) {
                std::filesystem::path src = std::filesystem::absolute(std::filesystem::path(srcPathStr));
                if (!std::filesystem::exists(src)) {
                    std::cerr << "[Zeus pkg] Error: Source path does not exist: " << srcPathStr << "\n";
                    return 1;
                }

                std::filesystem::path destDir = getGlobalPackagesDir();
                std::error_code ec;
                std::filesystem::create_directories(destDir, ec);

                std::string pkgName = src.filename().string();
                if (std::filesystem::is_regular_file(src)) {
                    std::filesystem::path target = destDir / src.filename();
                    std::filesystem::create_symlink(src, target, ec);
                    if (ec) {
                        // On Windows, create_symlink might require admin or developer mode; fallback to copy or report
                        std::cerr << "[Zeus pkg] Warning: Symlink failed (" << ec.message() << "), trying hard link...\n";
                        ec.clear();
                        std::filesystem::create_hard_link(src, target, ec);
                    }
                    if (ec) {
                        std::cerr << "[Zeus pkg] Link failed: " << ec.message() << ". You can use 'zeus pkg install' instead.\n";
                        return 1;
                    }
                    std::cout << "[Zeus pkg] Linked module '" << src.stem().string() << "' -> " << target.string() << "\n";
                    return 0;
                }

                if (std::filesystem::is_directory(src)) {
                    std::filesystem::path target = destDir / pkgName;
                    std::filesystem::create_directory_symlink(src, target, ec);
                    if (ec) {
                        std::cerr << "[Zeus pkg] Directory symlink failed (" << ec.message() << "). You can use 'zeus pkg install' or set ZEUS_PATH instead.\n";
                        return 1;
                    }
                    std::cout << "[Zeus pkg] Linked directory package '" << pkgName << "' -> " << target.string() << "\n";
                    return 0;
                }

                return 1;
            }
    };

} // namespace ZeusPackage
