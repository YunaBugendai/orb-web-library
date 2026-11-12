#!/usr/bin/env python3
"""Create a self-contained Orb! release archive for one host platform."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import tarfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def copy_tree(source: Path, destination: Path) -> None:
    if source.is_dir():
        shutil.copytree(source, destination, dirs_exist_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    parser.add_argument("--platform", required=True,
                        choices=("linux-x86_64", "macos-x86_64", "windows-x86_64"))
    parser.add_argument("--output-dir", type=Path, default=ROOT / "dist")
    args = parser.parse_args()

    windows = args.platform.startswith("windows")
    executable = ROOT / "build" / ("orb.exe" if windows else "orb")
    if not executable.is_file():
        parser.error(f"compiler executable not found: {executable}")

    stage = ROOT / "build" / "release-stage"
    if stage.exists():
        shutil.rmtree(stage)
    (stage / "bin").mkdir(parents=True)
    (stage / "share" / "orb").mkdir(parents=True)
    shutil.copy2(executable, stage / "bin" / executable.name)
    if not windows:
        (stage / "bin" / executable.name).chmod(0o755)

    optional = {
        "examples": ROOT / "examples",
        "docs": ROOT / "docs",
        "stdlib": ROOT / "std",
        "websrc": ROOT / "web",
        "extensions": ROOT / "build" / "dist",
    }
    for name, source in optional.items():
        if source.is_dir():
            target_name = "extensions" if name == "extensions" else name
            copy_tree(source, stage / "share" / "orb" / target_name)

    manifest = {
        "name": "Orb!",
        "version": args.version.lstrip("v"),
        "platform": args.platform,
        "required": ["compiler"],
        "optional": ["vscode", "examples", "docs", "stdlib", "websrc"],
        "standard_library": "Embedded in the compiler; no separate installation required.",
        "web_runtime": "Embedded in the compiler and emitted with WebAssembly launchers.",
    }
    (stage / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    (stage / "README.txt").write_text(
        "Orb! release package\n\n"
        "The compiler is in bin/. Optional components are in share/orb/.\n"
        "The standard library and WebAssembly browser runtime are embedded in the compiler.\n"
        "See share/orb/docs/ for documentation when that component is selected.\n",
        encoding="utf-8",
    )

    args.output_dir.mkdir(parents=True, exist_ok=True)
    stem = f"orb-{args.platform}"
    if windows:
        archive = args.output_dir / f"{stem}.zip"
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
            for path in sorted(stage.rglob("*")):
                if path.is_file():
                    z.write(path, path.relative_to(stage).as_posix())
    else:
        archive = args.output_dir / f"{stem}.tar.gz"
        with tarfile.open(archive, "w:gz") as tar:
            tar.add(stage / "bin", arcname="bin")
            tar.add(stage / "share", arcname="share")
            tar.add(stage / "manifest.json", arcname="manifest.json")
            tar.add(stage / "README.txt", arcname="README.txt")

    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    (args.output_dir / f"{archive.name}.sha256").write_text(
        f"{digest}  {archive.name}\n", encoding="ascii"
    )
    print(f"Created {archive}")
    print(f"SHA-256: {digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
