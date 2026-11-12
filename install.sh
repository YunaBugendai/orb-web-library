#!/bin/sh
# Orb! bootstrap installer. Usage: curl -fsSL https://raw.githubusercontent.com/YunaBugendai/orb-web-library/main/install.sh | sh
set -eu

if ! command -v python3 >/dev/null 2>&1; then
  echo "Orb! Setup requires Python 3. Install Python 3, then run this command again." >&2
  exit 1
fi
tmp="$(mktemp -t orb-setup-XXXXXX.py)"
trap 'rm -f "$tmp"' EXIT HUP INT TERM
curl -fsSL "https://raw.githubusercontent.com/YunaBugendai/orb-web-library/main/tools/setup.py" -o "$tmp"
python3 "$tmp" "$@"
