@echo off
rem orb.cmd - runs the Orb! compiler installed inside WSL from a Windows prompt.
rem Put this file in a folder on your PATH. Use paths relative to the current folder
rem (or WSL paths such as /mnt/c/...).   Example:  orb run main.orb
wsl.exe --cd "%CD%" -e orb %*
exit /b %ERRORLEVEL%
