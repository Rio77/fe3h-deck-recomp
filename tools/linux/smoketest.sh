#!/bin/bash
# FE3H-specific private smoke test; never run in game-free CI.
set -euo pipefail
cd "$(dirname "$0")/../.."
exec python3 tools/linux/smoke_install.py "${1:?AppImage path required}"
