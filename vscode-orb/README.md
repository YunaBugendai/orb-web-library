# Orb! for Visual Studio Code

Language support and tooling for [Orb!](https://github.com/) — native performance, minimal complexity.

* Syntax highlighting, snippets (`main`, `fn`, `for`, `struct`, `catch`, ...), bracket and comment handling
* **Run**: ▶ button in the editor title bar, `Ctrl+Alt+R` (`Cmd+Alt+R` on macOS), or the `▶ Run` CodeLens above `fn main`
* **Build** (`Ctrl+Alt+B`), **Check**, **Run with arguments**, **Doctor** from the command palette (`Orb: ...`)
* **Inline errors**: files are type-checked when opened and saved; compiler errors appear in the editor and the Problems panel
* Tasks (`"type": "orb"`) with a problem matcher (`$orb`)

## Requirements

The `orb` compiler. See the Orb! docs (`docs/PLATFORMS.md`):

| your machine | how the extension reaches the compiler |
|---|---|
| Linux | `orb` on PATH |
| macOS | `orb` on PATH (needs Xcode command line tools) — *experimental target* |
| Windows | native `orb.exe` if found (*experimental*), otherwise **WSL** automatically: Windows paths are translated and the compiler runs inside your Linux distribution |
| VS Code "WSL", "Dev Container", Codespaces windows | plain Linux |

## Settings

| setting | meaning |
|---|---|
| `orb.path` | path to the compiler (empty = `orb` from PATH) |
| `orb.mode` | `auto` (default), `native` or `wsl` |
| `orb.wslDistro` | WSL distribution (empty = default) |
| `orb.extraArgs` | extra flags such as `["--unchecked"]` |
| `orb.checkOnSave` | inline diagnostics on open/save (default on) |

Run **Orb: Doctor** if something does not work: it checks the toolchain and compiles and runs a test program.
