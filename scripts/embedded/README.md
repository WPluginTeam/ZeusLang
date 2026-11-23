# Zeus MCU builds with PlatformIO

`build_platformio.py` compiles a Zeus MCU entry file to LLVM IR, compiles the IR
with an Xtensa or other target Clang, and starts the project's PlatformIO build.
Compiler and object outputs stay in the selected project's `.pio/zeus/<environment>`
directory. The script can be reused by projects with another source file,
target triple, CPU, compiler installation, or PlatformIO environment.

## Project setup

Replace any fixed Zeus object path in the selected environment's `build_flags`
with this linker argument:

```ini
build_flags =
    -Wl,"${sysenv.ZEUS_OBJECT}"
```

Keep any existing board flags in that `build_flags` block. The environment
variable points at the object generated for the current build, so each project
can keep its outputs separate.

## Build

From the Zeus2 checkout, point the tool at the PlatformIO project and target tools:

```powershell
python scripts/embedded/build_platformio.py C:/path/to/platformio-project `
  --source src/app.zeu `
  --environment your-platformio-environment `
  --clang C:/path/to/xtensa-clang/bin/clang.exe `
  --pio C:/path/to/platformio/penv/Scripts/pio.exe `
  --cpu esp32s3
```

The compiler defaults to `ZeusCompiler.exe` beside the build script's Zeus2
checkout. Set `ZEUS_COMPILER`, `ZEUS_MCU_CLANG`, or `PLATFORMIO_CLI` to use a
different installation. Add `--upload` to run PlatformIO's upload target after
compilation.

Project-specific packages under `packages/` are prepended to `ZEUS_PATH` during
the Zeus compile. Set `--include-path` when Zeus needs to scan framework headers.
