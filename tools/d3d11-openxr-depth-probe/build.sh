#!/usr/bin/env bash
set -euo pipefail

out_dir=${1:-/tmp/d3d11-openxr-depth-probe}
mkdir -p "$out_dir"

x86_64-w64-mingw32-g++ \
  -std=c++20 -O2 -g -Wall -Wextra -Wpedantic \
  -I"$(dirname "$0")/../../wineopenxr" \
  -I/tmp/codex-proton-d32-xid69/Vulkan-Headers/include \
  "$(dirname "$0")/main.cpp" \
  -o "$out_dir/d3d11-openxr-depth-probe.exe" \
  -ld3d11 -ld3dcompiler -ldxgi -ldxguid

cp /home/ajg/Games/Heroic/Prefixes/MetaHorizon/pfx/drive_c/MetaGames/Software/stress-level-zero-inc-bonelab/BONELAB_Oculus_Windows64_Data/Plugins/x86_64/openxr_loader.dll \
  "$out_dir/openxr_loader.dll"
