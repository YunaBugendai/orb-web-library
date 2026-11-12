# Platforms: Linux, Windows, macOS

Orb! compiles to **x86-64** machine code and always uses the machine's own assembler and linker.
What differs between systems is the object format, the system-call layer and the available tools.

| you use | what to do | status |
|---|---|---|
| **Linux x86-64** | `make && sudo make install` | **fully supported, fully tested** (79 checks + fuzzer) |
| **Windows 10/11 — via WSL2** (recommended) | use the graphical installer or run `tools\windows\setup-wsl.ps1` once | supported through the Linux build; WSL glue is unit tested |
| **Windows — native** (`orb.exe`, produces `.exe`) | graphical installer; WSL2 remains recommended | **experimental**: CI builds the native compiler on Windows UCRT64 and runs `orb doctor` plus a compiled hello-world program; broader hardware/toolchain coverage is still limited |
| **macOS Intel** | Xcode command line tools, `make`, see below | **experimental**: same caveat |
| **macOS Apple Silicon** | same; programs are x86-64 and run through Rosetta 2 | **experimental** (no native arm64 backend) |
| **Browser / WebAssembly** (`--target wasm`) | `orb build prog.orb --target wasm`; see [WASM.md](WASM.md) and [WEB.md](WEB.md) | **works, tested** in Chromium and Node (no assembler or linker needed); Firefox / Safari not tested |
| **Dev Container / Codespaces / Docker** | `.devcontainer/` is included | Linux, supported |

> **What "experimental" means here.** Linux is the fully supported native target. The Windows
> compiler is now built and smoke-tested on a real Windows CI runner (UCRT64): `orb doctor` and a
> compiled hello-world executable both run successfully. That is useful coverage, but not a guarantee
> for every Windows installation or third-party toolchain. The macOS target remains unexecuted on
> real macOS hardware; it is generated and checked from Linux. Run `orb doctor` on your own system
> before relying on a native target.

## Linux
```sh
sudo apt install build-essential binutils     # or your distribution's equivalent
make && sudo make install
orb doctor
```

## Windows

### Graphical installer

Download `Orb-Setup-0.2.0.exe` from the latest successful
[Windows installer workflow](https://github.com/YunaBugendai/orb-web-library/actions/workflows/windows-installer.yml).
The wizard installs the compiler and lets you select the VS Code extension, examples, offline
documentation and editable source files. It can also add Orb! to your current user's PATH.

The standard modules and WebAssembly browser runtime are embedded in the compiler; they are not
separate installation components.

### Recommended for running programs: WSL2

1. PowerShell as Administrator: `wsl --install`, reboot, finish the Ubuntu first-run setup.
2. In PowerShell, from the unpacked Orb! folder:
   `powershell -ExecutionPolicy Bypass -File tools\windows\setup-wsl.ps1`
   (installs the build tools inside WSL, builds and installs `orb`, and adds an `orb.cmd` shim to your PATH).
3. `orb run examples\hello.orb` works from any Windows prompt. Programs run inside Linux; compiled
   executables are Linux ELF files (run them with `wsl ./name`).
4. VS Code: install the extension (see `VSCODE.md`). It detects that there is no native `orb.exe` and routes
   everything through `wsl.exe`, translating `C:\...` paths automatically.

Tip: for speed keep projects inside the WSL file system (`\\wsl$\Ubuntu\home\you\...`) rather than `C:\`.

### Native Windows (experimental)

The installer includes a native x86-64 compiler build. The Windows code-generation target is still
experimental. For a more predictable development environment, use WSL2 as described above.
## macOS (experimental)
```sh
xcode-select --install        # provides clang and make
make && sudo make install
orb doctor
```
The compiler emits x86-64 assembly in Mach-O flavour (underscore-prefixed symbols, `L` local labels, `__TEXT`/`__DATA`
sections) and calls `clang -arch x86_64` to assemble and link against libSystem. `std/os_macos.orb` uses the C
functions `write`, `read`, `open`, `mmap`, `exit`. On Apple Silicon the resulting programs run under Rosetta 2
(macOS offers to install it the first time). A native arm64 backend is not implemented.

## Selecting the target
`orb build main.orb --target linux|macos|windows`. By default the target is the machine the compiler runs on.
`orb run` only runs programs for the current machine. `-S` prints assembly for any target on any machine.

## Environment variables
`ORB_AS`, `ORB_LD`, `ORB_CC` override the assembler, linker and C driver (e.g. to use a cross toolchain).
