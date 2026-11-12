#!/usr/bin/env python3
"""Orb! release installer for Linux and macOS (standard-library Python only)."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import shlex
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

REPOSITORY = "YunaBugendai/orb-web-library"
API = f"https://api.github.com/repos/{REPOSITORY}"


def request_json(url: str) -> dict:
    req = urllib.request.Request(url, headers={"User-Agent": "Orb-Setup/1"})
    with urllib.request.urlopen(req, timeout=30) as response:
        return json.load(response)


def download(url: str, destination: Path) -> None:
    req = urllib.request.Request(url, headers={"User-Agent": "Orb-Setup/1"})
    with urllib.request.urlopen(req, timeout=120) as response, destination.open("wb") as out:
        shutil.copyfileobj(response, out)


def release_info(version: str | None) -> dict:
    endpoint = f"{API}/releases/latest" if not version else f"{API}/releases/tags/v{version.lstrip('v')}"
    try:
        return request_json(endpoint)
    except Exception as exc:
        raise RuntimeError(
            "Could not find an Orb! release. A release must be published before the installer can download binaries."
        ) from exc


def platform_asset() -> tuple[str, str]:
    system = platform.system()
    machine = platform.machine().lower()
    if machine not in ("x86_64", "amd64", "arm64", "aarch64"):
        raise RuntimeError(f"Unsupported CPU architecture: {machine}")
    if system == "Linux":
        return "linux-x86_64", ".tar.gz"
    if system == "Darwin":
        if machine in ("arm64", "aarch64"):
            print("Note: Orb! currently ships an x86-64 macOS compiler; Apple Silicon needs Rosetta 2.")
        return "macos-x86_64", ".tar.gz"
    raise RuntimeError("This bootstrapper supports Linux and macOS. On Windows, use Orb-Setup-*.exe.")


def choose_components() -> set[str]:
    items = [
        ("vscode", "Visual Studio Code extension", shutil.which("code") is not None),
        ("examples", "Example projects", True),
        ("docs", "Offline documentation", True),
        ("stdlib", "Standard-library source files (for contributors)", False),
        ("websrc", "Browser host source (for contributors)", False),
        ("path", "Add Orb! to the current user's PATH", True),
    ]
    print("\nComponents")
    print("  [required] Orb! compiler + embedded standard modules + WebAssembly runtime")
    print("             math, strings, io, vec and web are available through use <module>;")
    print("             they are already inside the compiler.")
    for index, (_, label, selected) in enumerate(items, 1):
        print(f"  {index}. [{'x' if selected else ' '}] {label}")
    print("\nPress Enter for defaults, or enter numbers separated by commas (e.g. 1,2,3,6).")
    raw = input("Selections: ").strip().lower()
    if not raw:
        return {key for key, _, default in items if default}
    if raw in ("all", "hepsi"):
        return {key for key, _, _ in items}
    chosen: set[str] = set()
    for item in raw.split(","):
        item = item.strip()
        if not item:
            continue
        if not item.isdigit() or not 1 <= int(item) <= len(items):
            raise ValueError(f"Invalid selection: {item}")
        chosen.add(items[int(item) - 1][0])
    return chosen


def add_to_path(bin_dir: Path) -> str:
    target = str(bin_dir.resolve())
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
    return f"Added {target} to {rc}. Open a new terminal."


def safe_extract(archive: Path, destination: Path) -> None:
    with tarfile.open(archive, "r:gz") as tar:
        for member in tar.getmembers():
            name = Path(member.name)
            if name.is_absolute() or ".." in name.parts or member.issym() or member.islnk():
                raise RuntimeError(f"Unsafe path in release archive: {member.name}")
        tar.extractall(destination)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", help="install a specific release (default: latest)")
    parser.add_argument("--prefix", type=Path, default=Path.home() / ".local")
    parser.add_argument("--yes", action="store_true", help="accept the default component selection")
    parser.add_argument("--dry-run", action="store_true", help="show the installation plan without changes")
    args = parser.parse_args()

    try:
        target, suffix = platform_asset()
        if args.dry_run:
            print(f"Would install Orb! {args.version or 'latest'} for {target} into {args.prefix.expanduser()}")
            print("Required: compiler (standard modules and browser runtime are embedded).")
            print("Optional: VS Code extension, examples, offline docs, stdlib/web source, PATH.")
            return 0
        release = release_info(args.version)
        version = release["tag_name"].lstrip("v")
        asset_name = f"orb-{target}{suffix}"
        assets = {asset["name"]: asset for asset in release.get("assets", [])}
        if asset_name not in assets or f"{asset_name}.sha256" not in assets:
            raise RuntimeError(f"Release {release['tag_name']} is missing {asset_name} or its checksum.")
        selected = {"examples", "docs", "path"} if args.yes else choose_components()
        prefix = args.prefix.expanduser().resolve()
        bin_dir = prefix / "bin"
        share_dir = prefix / "share" / "orb"

        print("\nInstallation plan")
        print(f"  Version: Orb! {version}")
        print(f"  Compiler: {bin_dir / 'orb'}")
        print("  Built in: math, strings, vec, io, web, WebAssembly browser runtime")
        for key, label in (
            ("vscode", "VS Code extension"), ("examples", "Example projects"),
            ("docs", "Offline documentation"), ("stdlib", "Standard-library source"),
            ("websrc", "Browser host source"), ("path", "PATH integration"),
        ):
            if key in selected:
                print(f"  + {label}")
        if not args.yes and input("\nInstall with these components? [Y/n]: ").strip().lower() in ("n", "no", "hayir", "h"):
            print("Installation cancelled.")
            return 0

        with tempfile.TemporaryDirectory(prefix="orb-setup-") as temp:
            temp_dir = Path(temp)
            archive = temp_dir / asset_name
            checksum = temp_dir / f"{asset_name}.sha256"
            download(assets[asset_name]["browser_download_url"], archive)
            download(assets[f"{asset_name}.sha256"]["browser_download_url"], checksum)
            expected = checksum.read_text(encoding="ascii").split()[0].lower()
            actual = hashlib.sha256(archive.read_bytes()).hexdigest()
            if actual != expected:
                raise RuntimeError("Release archive checksum mismatch; refusing to install.")
            extracted = temp_dir / "unpacked"
            extracted.mkdir()
            safe_extract(archive, extracted)
            compiler = extracted / "bin" / "orb"
            if not compiler.is_file():
                raise RuntimeError("Release archive does not contain bin/orb.")
            bin_dir.mkdir(parents=True, exist_ok=True)
            shutil.copy2(compiler, bin_dir / "orb")
            (bin_dir / "orb").chmod((bin_dir / "orb").stat().st_mode | 0o111)

            source_share = extracted / "share" / "orb"
            components = {
                "examples": "examples", "docs": "docs", "stdlib": "stdlib",
                "websrc": "websrc", "vscode": "extensions",
            }
            for key, folder in components.items():
                if key not in selected:
                    continue
                source = source_share / folder
                if source.exists():
                    destination = share_dir / folder
                    if source.is_dir():
                        shutil.copytree(source, destination, dirs_exist_ok=True)
                    else:
                        destination.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copy2(source, destination)

            if "vscode" in selected:
                code = shutil.which("code")
                vsix_files = sorted((share_dir / "extensions").glob("*.vsix")) if (share_dir / "extensions").exists() else []
                if code and vsix_files:
                    subprocess.run([code, "--install-extension", str(vsix_files[0]), "--force"], check=False)
                elif vsix_files:
                    print(f"VS Code CLI not found. Extension package saved at {vsix_files[0]}")

        if "path" in selected:
            print(add_to_path(bin_dir))
        result = subprocess.run([str(bin_dir / "orb"), "version"], text=True, capture_output=True)
        if result.returncode != 0:
            raise RuntimeError(f"Installed compiler failed to start: {result.stderr.strip()}")
        print("\n" + result.stdout.strip())
        print("\nInstallation complete. Open a new terminal, then run: orb doctor")
        return 0
    except (OSError, ValueError, RuntimeError, tarfile.TarError, urllib.error.URLError) as exc:
        print(f"Orb! Setup failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
