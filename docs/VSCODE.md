# Orb! in Visual Studio Code

The extension lives in `vscode-orb/`. A ready-to-install package is `dist/orb-lang-0.1.0.vsix`
(rebuild it with `make vsix`).

## Install
**From the package (easiest).** In VS Code: *Extensions* view → `...` menu → **Install from VSIX...** → pick
`orb-lang-0.1.0.vsix`. Reload the window.

**From the folder.** Copy `vscode-orb` to
`%USERPROFILE%\.vscode\extensions\orb-technologies.orb-lang-0.1.0` (Windows) or
`~/.vscode/extensions/orb-technologies.orb-lang-0.1.0` (macOS/Linux) and restart VS Code.

**Development.** Open the `vscode-orb` folder in VS Code and press `F5` (Extension Development Host).

The extension also needs the `orb` compiler: see `PLATFORMS.md`. On Windows the extension finds `orb.exe` on
PATH, otherwise it goes through WSL automatically (setting `orb.mode`: `auto` | `native` | `wsl`).

## Using it
* Open any `.orb` file. Syntax highlighting and snippets work immediately (type `main`, `for`, `fn`, `catch`, ...).
* **Run** with the ▶ button at the top right of the editor, `Ctrl+Alt+R` (`Cmd+Alt+R`), right-click → *Orb: Run File*, or
  the `▶ Run` CodeLens above `fn main()`. The program runs in an integrated terminal called *Orb*, so
  programs that read from stdin work.
* **Build** (`Ctrl+Alt+B`) writes an executable next to the source file. **Orb: Run File With Arguments...** passes arguments.
* **Errors appear while you work**: the file is type-checked on open and on save; messages show as squiggles and in
  the Problems panel (`orb.checkOnSave`).
* **Tasks**: `Terminal → Run Task → orb: ...`, or in `tasks.json`:
  `{ "type": "orb", "command": "run", "file": "main.orb" }` (problem matcher `$orb` is built in).
* **Orb: Doctor** runs `orb doctor` and reports what is missing.

## Other editors and online IDEs
* **GitHub Codespaces / VS Code Dev Containers**: open the repository; `.devcontainer/` builds a Linux image with the
  compiler installed (Apple Silicon hosts run it as `linux/amd64`).
* **Any editor / terminal**: `orb run file.orb`; compiler errors use the standard `file:line:col: error: message`
  format, so any problem matcher or "quickfix" integration (Vim `errorformat`, Emacs `compilation-mode`, ...) works.
* **Repl.it / Gitpod / other Linux IDEs**: any Linux x86-64 environment with `gcc` and `binutils` works — run
  `make` and use `build/orb`.

## What has been tested
The extension's logic (path translation, WSL command construction, shell quoting, parsing real compiler output) has
unit tests, and `extension.js` is exercised end to end against a mock of the VS Code API using the real compiler
(diagnostics, run/build commands, CodeLens, tasks). It has **not** been loaded into a real VS Code window in the
environment where it was written; if something misbehaves, open *Orb: Show Output* and report what it says.
Run the tests with `node --test vscode-orb/test/lib.test.js vscode-orb/test/extension.mock.test.js`.
