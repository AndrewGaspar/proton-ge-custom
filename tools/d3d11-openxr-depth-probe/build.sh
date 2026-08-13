#!/usr/bin/env bash
set -euo pipefail

out_dir=${1:-/tmp/d3d11-openxr-depth-probe}
loader_dll=${OPENXR_LOADER_DLL:-}
source_dir=$(cd "$(dirname "$0")" && pwd)
proton_root=$(cd "$source_dir/../.." && pwd)

if [[ -z "$loader_dll" || ! -f "$loader_dll" ]]; then
  echo "FAIL: set OPENXR_LOADER_DLL to a Windows OpenXR 1.1 loader DLL" >&2
  exit 2
fi

mkdir -p "$out_dir"

x86_64-w64-mingw32-g++ \
  -std=c++20 -O2 -g -Wall -Wextra -Wpedantic -Werror \
  -Wno-missing-field-initializers -Wno-cast-function-type -static \
  -I"$proton_root/wineopenxr" \
  -I"$proton_root/Vulkan-Headers/include" \
  "$source_dir/main.cpp" \
  -o "$out_dir/d3d11-openxr-depth-probe.exe" \
  -ladvapi32 -ld3d11 -ld3dcompiler -ldxgi -ldxguid

cp "$loader_dll" "$out_dir/openxr_loader.dll"
cp "$source_dir/run-guarded.sh" "$out_dir/run-guarded.sh"
cp "$source_dir/attribute_fence_log.py" "$out_dir/attribute_fence_log.py"
cp "$source_dir/README.md" "$out_dir/README.md"
