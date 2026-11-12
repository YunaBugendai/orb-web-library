#!/usr/bin/env python3
"""Interactive Orb! installer for Linux, macOS and supported MinGW environments on Windows."""
from __future__ import annotations

import argparse
import os
import platform
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def default_prefix() -> Path:
    if os.name == "nt":
        base = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local"))
        return base / "Programs" / "Orb"
    return Path.home() / ".local"


def yes_no(label: str, default: bool) -> bool:
    hint = "Y/n" if default else "y/N"
    answer = input(f"{label} [{hint}]: ").strip().lower()
    if not answer:
        return default
    return answer in ("y", "yes", "evet", "e")


def choose_components() -> set[str]:
    items = [
        ("vscode", "Visual Studio Code extension", shutil.which("code") is not None),
        ("examples", "Example projects", True),
        ("docs", "Offline documentation", True),
        ("stdlib", "Standard-library source files (for contributors)", False),
        ("websrc", "Browser host source (for Web runtime contributors)", False),
        ("path", "Add Orb! to the current user's PATH", True),
    ]
    print("\nComponents")
    print("  [required] Orb! compiler + standard modules + WebAssembly runtime")
    print("             The standard library is embedded in the compiler; it is not a separate download.")
    for i, (key, label, selected) in enumerate(items, 1):
        state = "x" if selected else " "
        print(f"  {i}. [{state}] {label}")
    print("\nPress Enter for the defaults, or enter numbers separated by commas (for example: 1,2,3,6).")
    raw = input("Selections: ").strip().lower()
    if not raw:
        return {key for key, _, default in items if default}
    if raw in ("all", "hepsi"):
        return {key for key, _, _ in items}
    selected: set[str] = set()
    for piece in raw.split(","):
        piece = piece.strip()
        if not piece:
            continue
        if not piece.isdigit() or not (1 <= int(piece) <= len(items)):
            raise ValueError(f"Invalid component selection: {piece}")
        selected.add(items[int(piece) - 1][0])
    return selected


def add_to_path(bin_dir: Path) -> str:
    target = str(bin_dir.resolve())
    if os.name == "nt":
        try:
            import winreg
            key = winreg.OpenKey(
                winreg.HKEY_CURRENT_USER, "Environment", 0,
                winreg.KEY_READ | winreg.KEY_SET_VALUE
            )
            try:
                try:
                    current, value_type = winreg.QueryValueEx(key, "Path")
                except FileNotFoundError:
                    current, value_type = "", winreg.REG_EXPAND_SZ
                parts = [p.strip() for p in current.split(";") if p.strip()]
                if not any(p.casefold() == target.casefold() for p in parts):
                    parts.append(target)
                    winreg.SetValueEx(key, "Path", 0, winreg.REG_EXPAND_SZ, ";".join(parts))
                return "Added the compiler directory to your user PATH. Open a new terminal."
            finally:
                winreg.CloseKey(key)
        except OSError as exc:
            return f"Could not update the user PATH automatically: {exc}"

    shell = Path(os.environ.get("SHELL", "")).name
    home = Path.home()
    if shell == "fish":
        rc = home / ".config" / "fish" / "config.fish"
        line = f"fish_add_path {shlex.quote(target)}"
    elif shell == "zsh":
        rc = home / ".zprofile"
        line = f"export PATH={shlex.quote(target)}:$PATH"
    elif shell == "bash":
        rc = home / ".bashrc"
        line = f"export PATH={shlex.quote(target)}:$PATH"
    else:
        rc = home / ".profile"
        line = f"export PATH={shlex.quote(target)}:$PATH"

    rc.parent.mkdir(parents=True, exist_ok=True)
    old = rc.read_text(encoding="utf-8") if rc.exists() else ""
    if line not in old:
        with rc.open("a", encoding="utf-8") as stream:
            if old and not old.endswith("\n"):
                stream.write("\n")
            stream.write("\n# Orb! compiler\n" + line + "\n")
    return f"Added PATH setup to {rc}. Open a new terminal."


def run(command: list[str], *, cwd: Path = ROOT) -> None:
    print("\n>", " ".join(str(part) for part in command))
    subprocess.run(command, cwd=cwd, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="Install Orb! with an interactive component selector.")
    parser.add_argument("--prefix", type=Path, help="installation prefix (default: user-local)")
    parser.add_argument("--yes", action="store_true", help="accept the default component selection")
    parser.add_argument("--dry-run", action="store_true", help="show the plan without building or installing")
    args = parser.parse_args()

    system = platform.system()
    print("Orb! Setup")
    print("==========")
    print(f"Host: {system} {platform.machine()}")
    print("The core compiler includes math, strings, I/O, WebAssembly and the browser host runtime.")
    if system == "Windows" and not (shutil.which("make") and (shutil.which("gcc") or shutil.which("cc"))):
        print("\nThis terminal does not have the native build toolchain.")
        print("For Windows, use the Orb! Setup.exe graphical installer, or open an MSYS2 MinGW x64 shell.")
        print("Native Windows code generation is experimental; WSL2 is the recommended runtime.")
        return 2

    make = shutil.which("make") or shutil.which("mingw32-make")
    compiler = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not make or not compiler:
        print("\nBuild prerequisites are missing.")
        print("Install a C compiler and make first. See docs/PLATFORMS.md.")
        if system == "Darwin":
            print("On macOS: xcode-select --install")
        elif system == "Linux":
            print("On Debian/Ubuntu: sudo apt install build-essential binutils")
        return 2

    try:
        selected = choose_components() if not args.yes else {
            "examples", "docs", "path"
        }
    except ValueError as exc:
        print(exc, file=sys.stderr)
        return 2

    prefix = (args.prefix or default_prefix()).expanduser().resolve()
    bin_dir = prefix / "bin"
    share_dir = prefix / "share" / "orb"
    exe_name = "orb.exe" if os.name == "nt" else "orb"
    source_exe = ROOT / "build" / exe_name
    installed_exe = bin_dir / exe_name

    print("\nInstallation plan")
    print(f"  Compiler: {installed_exe}")
    print("  Built-in: core, math, strings, vec, I/O, WASM, Web browser APIs")
    for component, label in [
        ("vscode", "VS Code extension"),
        ("examples", "Example projects"),
        ("docs", "Offline documentation"),
        ("stdlib", "Standard-library source files"),
        ("websrc", "Browser host source"),
        ("path", "PATH integration"),
    ]:
        if component in selected:
            print(f"  + {label}")
    if args.dry_run:
        print("\nDry run only; no files were changed.")
        return 0
    if not args.yes and not yes_no("\nInstall Orb! with these components?", True):
        print("Installation cancelled.")
        return 0

    try:
        run([make])
    except (subprocess.CalledProcessError, OSError) as exc:
        print(f"\nCompiler build failed: {exc}", file=sys.stderr)
        return 1

    if not source_exe.exists():
        print(f"Expected compiler was not produced: {source_exe}", file=sys.stderr)
        return 1

    bin_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source_exe, installed_exe)
    if os.name != "nt":
        installed_exe.chmod(installed_exe.stat().st_mode | 0o111)

    if "examples" in selected:
        shutil.copytree(ROOT / "examples", share_dir / "examples", dirs_exist_ok=True)
    if "docs" in selected:
        shutil.copytree(ROOT / "docs", share_dir / "docs", dirs_exist_ok=True)
    if "stdlib" in selected:
        shutil.copytree(ROOT / "std", share_dir / "std", dirs_exist_ok=True)
    if "websrc" in selected:
        (share_dir / "web").mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / "web" / "orb.js", share_dir / "web" / "orb.js")

    if "vscode" in selected:
        extension_dir = share_dir / "extensions"
        extension_dir.mkdir(parents=True, exist_ok=True)
        run([sys.executable, str(ROOT / "tools" / "package-vsix.py"), str(extension_dir)])
        vsix_files = sorted(extension_dir.glob("*.vsix"))
        code = shutil.which("code")
        if code and vsix_files:
            try:
                run([code, "--install-extension", str(vsix_files[0]), "--force"])
            except subprocess.CalledProcessError:
                print("VS Code extension was packaged, but automatic installation failed.")
                print(f"Install it manually from: {vsix_files[0]}")
        elif vsix_files:
            print("VS Code CLI was not found. The extension package is available at:")
            print(vsix_files[0])

    if "path" in selected:
        print(add_to_path(bin_dir))

    try:
        result = subprocess.run([str(installed_exe), "version"], check=True, text=True, capture_output=True)
        print("\n" + result.stdout.strip())
    except (subprocess.CalledProcessError, OSError) as exc:
        print(f"Installed compiler could not be launched: {exc}", file=sys.stderr)
        return 1

    print("\nInstallation complete.")
    print("Open a new terminal, then try:")
    print("  orb doctor")
    print("  orb new hello")
    print("  orb run hello/main.orb")
    print("For Orb! Web: orb build main.orb --target wasm -o app.wasm")
    print("The compiler also writes app.wasm.js; include it with a module script tag.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
