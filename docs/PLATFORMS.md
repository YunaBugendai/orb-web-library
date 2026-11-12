# Platforms: Linux, Windows, macOS

Orb! compiles to **x86-64** machine code and always uses the machine's own assembler and linker.
What differs between systems is the object format, the system-call layer and the available tools.

| you use | what to do | status |
|---|---|---|
| **Linux x86-64** | `make && sudo make install` | **fully supported, fully tested** (79 checks + fuzzer) |
| **Windows 10/11 — via WSL2** (recommended) | run `tools\windows\setup-wsl.ps1` once; use `orb` from PowerShell/cmd or the VS Code extension | **works through the Linux build**; the Windows glue (path translation, `orb.cmd`, extension) is unit tested, but I could not run it on a real Windows machine |
| **Windows — native** (`orb.exe`, produces `.exe`) | MSYS2 / MinGW-w64, see below | **experimental**: code generation and kernel32 layer written, assembly output checked for syntax, **never executed** |
| **macOS Intel** | Xcode command line tools, `make`, see below | **experimental**: same caveat |
| **macOS Apple Silicon** | same; programs are x86-64 and run through Rosetta 2 | **experimental** (no native arm64 backend) |
| **Browser / WebAssembly** (`--target wasm`) | `orb build prog.orb --target wasm`; see [WASM.md](WASM.md) and [WEB.md](WEB.md) | **works, tested** in Chromium and Node (no assembler or linker needed); Firefox / Safari not tested |
| **Dev Container / Codespaces / Docker** | `.devcontainer/` is included | Linux, supported |

> **What "experimental" means here.** The development environment for this project is Linux only.
> For the Windows and macOS targets the compiler's output was generated for every example and test
> program and checked to assemble, and the generated entry points, calling sequences and data sections
> were reviewed — but no program has been *run* on those systems. Run `orb doctor`: it compiles
> and runs a small program and tells you precisely which step fails. If something fails, the message
> (plus `orb build file.orb --target windows -S`) is what is needed to fix it.

## Linux
```sh
sudo apt install build-essential binutils     # or your distribution's equivalent
make && sudo make install
orb doctor
```

## Windows

### Recommended: WSL2
1. PowerShell as Administrator: `wsl --install`, reboot, finish the Ubuntu setup.
2. In PowerShell, from the unpacked Orb! folder:
   `powershell -ExecutionPolicy Bypass -File tools\windows\setup-wsl.ps1`
   (installs the build tools inside WSL, builds and installs `orb`, and adds an `orb.cmd` shim to your PATH).
3. `orb run examples\hello.orb` works from any Windows prompt. Programs run inside Linux; compiled
   executables are Linux ELF files (run them with `wsl ./name`).
4. VS Code: install the extension (see `VSCODE.md`). It detects that there is no native `orb.exe` and routes
   everything through `wsl.exe`, translating `C:\...` paths automatically. Alternatively use the
   *WSL* extension, open your folder inside WSL, and everything is plain Linux.

Tip: for speed keep projects inside the WSL file system (`\\wsl$\Ubuntu\home\you\...`) rather than `C:\`.

### Native Windows (experimental)
1. Install [MSYS2](https://www.msys2.org), open the **MSYS2 MinGW x64** shell and run:
   `pacman -S --needed make mingw-w64-x86_64-gcc mingw-w64-x86_64-binutils`
2. In the Orb! folder: `make` (produces `build/orb.exe`). Add `build` to your PATH or copy `orb.exe` somewhere on it.
3. `orb doctor`, then `orb run examples/hello.orb` (the shell must be the MinGW x64 one: `as` and `gcc` come from it).
4. `orb build main.orb` produces `main.exe`. The executable links the MinGW C runtime only for argument parsing
   and uses `kernel32` for everything else; the stack is set to 8 MB.

Cross-compiling from Linux: install `mingw-w64` (`x86_64-w64-mingw32-gcc`), then
`orb build main.orb --target windows` produces `main.exe`.

How it works: `std/os_windows.orb` implements files, console I/O and memory with `kernel32`
(`WriteFile`, `ReadFile`, `CreateFileA`, `VirtualAlloc`, `ExitProcess`, ...) declared as
`extern "win64" fn ...`, which makes the compiler use the Microsoft x64 calling convention
(`rcx rdx r8 r9`, 32 bytes of shadow space) for those calls. The program entry is `main`, called by the
MinGW C start-up code.

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
