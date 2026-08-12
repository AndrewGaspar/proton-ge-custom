#!/usr/bin/env bash
set -euo pipefail

out_dir=${1:-/tmp/d3d11-openxr-depth-probe}
loader_dll=${OPENXR_LOADER_DLL:-}

if [[ -z "$loader_dll" || ! -f "$loader_dll" ]]; then
  echo "FAIL: set OPENXR_LOADER_DLL to a Windows OpenXR 1.1 loader DLL" >&2
  exit 2
fi

mkdir -p "$out_dir"

x86_64-w64-mingw32-g++ \
  -std=c++20 -O2 -g -Wall -Wextra -Wpedantic \
  -I"$(dirname "$0")/../../wineopenxr" \
  -I/tmp/codex-proton-d32-xid69/Vulkan-Headers/include \
  "$(dirname "$0")/main.cpp" \
  -o "$out_dir/d3d11-openxr-depth-probe.exe" \
  -ld3d11 -ld3dcompiler -ldxgi -ldxguid

cp "$loader_dll" "$out_dir/openxr_loader.dll"
