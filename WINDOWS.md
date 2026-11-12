# Using Orb! on Windows

Orb! produces Linux x86-64 programs, so on Windows you run it inside WSL2 (Windows Subsystem for Linux).

1. Open PowerShell as Administrator and run:  `wsl --install`  (then reboot; this installs Ubuntu).
2. Unzip this archive, e.g. to `C:\orb`. Open the Ubuntu terminal and go there:  `cd /mnt/c/orb`
   (for best speed copy it into the Linux home instead: `cp -r /mnt/c/orb ~/orb && cd ~/orb`).
3. Install the tools once:  `sudo apt update && sudo apt install -y build-essential binutils`
4. Build the compiler:  `make`   (a prebuilt Linux binary is also included at `build/orb`)
5. Try it:  `./build/orb run examples/hello.orb`
   Run the tests:  `make test`

Edit your `.orb` files with any Windows editor (VS Code works great with its WSL extension).
