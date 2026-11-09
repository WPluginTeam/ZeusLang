# Zeus CLI

The Windows command entry is `Zeus.exe`, implemented in Zeus. `zeusc.bat` remains the AOT build driver and initializes MSVC only inside its own command session.

## Commands

```bat
Zeus.exe run app.zeu [program arguments...]
Zeus.exe build app.zeu -o app.exe --onefile
Zeus.exe --help
```

`run` executes the source with the sibling `ZeusCompiler.exe` and forwards remaining arguments and console input/output. Other compiler arguments are still accepted directly, so existing calls such as `Zeus.exe --emit-llvm app.zeu` remain usable.

`build` delegates to the sibling `zeusc.bat`. It supports `-o`/`--output`, `--opt-level`, and `--onefile`; optimization level `s` is the default. Onefile embeds `ZeusNativeRuntime.dll` into the executable; the runtime is extracted to a temporary file when first loaded and removed at process exit. Without `--onefile`, the build driver places the runtime DLL beside an output created in another directory.

## Building the command entry

Run `build.bat` from any working directory. It builds `ZeusCompiler.exe`, builds `ZeusNativeRuntime.dll`, then compiles `ZeusCLI/main.zeu` into the onefile `Zeus.exe`. MSVC environment changes stay inside the batch process, so they do not alter the caller's environment.

`Zeus.exe run` and `Zeus.exe build` expect `ZeusCompiler.exe` and `zeusc.bat` next to `Zeus.exe`. `ZeusCompiler.exe` also loads `ZeusNativeRuntime.dll` dynamically, so keep that DLL beside it. `build` needs the Windows MSVC x64 C++ tools and LLVM clang.
