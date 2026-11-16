#!/bin/bash
# FE3H needs only the game-code generator, not the LAS randomizer.
set -euo pipefail
cd "$(dirname "$0")/../.."
L=$PWD/build/linux/${1:?architecture required}
V=$L/gen-venv
if [ ! -f "$V/.ok" ]; then
    python3.12 -m venv "$V"
    "$V/bin/python" -m pip install --report "$L/helper-packages.json" capstone==5.0.9 unicorn==2.1.4 lz4==4.4.5 pyinstaller==6.22.3
    "$V/bin/python" -m pip freeze --all > "$L/helper-packages.txt"
    touch "$V/.ok"
fi
"$V/bin/python" -m PyInstaller --noconfirm --clean --log-level WARN --distpath "$L/helper" --workpath "$L/pyinstaller" tools/lasgen.spec
