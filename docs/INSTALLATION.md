# Installing Orb!

Orb! is distributed as a compiler with its standard library and browser runtime built in.

## Quick install

### Linux

```sh
curl -fsSL https://raw.githubusercontent.com/YunaBugendai/orb-web-library/main/install.sh | sh
```

### macOS

```sh
curl -fsSL https://raw.githubusercontent.com/YunaBugendai/orb-web-library/main/install.sh | sh
```

The current macOS compiler targets x86-64. Apple Silicon requires Rosetta 2.

### Windows

In PowerShell:

```powershell
irm https://raw.githubusercontent.com/YunaBugendai/orb-web-library/main/install.ps1 | iex
```

This downloads and opens the graphical Orb! Setup wizard. You can also download
`Orb-Setup-0.2.0.exe` from the latest GitHub Release.

## Component selection

The compiler is required. Its standard modules and browser runtime are embedded; they are not
separate components and do not need separate installation.

Optional components:

- Visual Studio Code language extension
- Example projects
- Offline documentation
- Standard-library source files (for contributors)
- Browser host source (for contributors)
- Add the compiler to the current user's PATH

The Linux/macOS bootstrapper downloads the matching release archive, verifies its SHA-256 checksum,
and installs only the selected components. The Windows installer provides the same component
selection in a graphical wizard.

## First run

Open a new terminal and check the toolchain:

```sh
orb version
orb doctor
orb new hello
orb run hello/main.orb
```

The compiler itself does not need a package manager. Native program compilation still uses the
host assembler/linker described in [PLATFORMS.md](PLATFORMS.md).

## Build from source instead

If you want to build Orb! from a checkout, see the repository README and run
`python3 tools/install.py` for the source-based installer wizard.
