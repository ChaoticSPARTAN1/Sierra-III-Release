#!/usr/bin/env bash
# Makes the bake bundle for this machine's OS from this checkout: builds the
# native tools from source (bundle/build_tools.sh: the port's Invader and
# atracdenc), then puts the bundle together and packs it (bundle/assemble.py).
# The release's bundles come from the bake-bundle workflow
# (.github/workflows/bake-bundle.yml), one per OS, the same way.
#   make_bundle.sh [OUT]      default: ./halo-psp-bake
# environment:
#   WORK   the build's work folder (default: OUT-work); kept, so a second
#          run reuses the libraries it built
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$PWD/halo-psp-bake}
WORK=${WORK:-$OUT-work}
bash "$P/bake/bundle/build_tools.sh" "$WORK" "$WORK/tools"
python3 "$P/bake/bundle/assemble.py" "$WORK/tools" "$OUT" --archive
