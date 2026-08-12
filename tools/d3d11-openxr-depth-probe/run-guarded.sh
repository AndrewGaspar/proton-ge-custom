#!/usr/bin/env bash
set -euo pipefail

stage=${1:-enumerate}
probe_dir=${2:-/tmp/d3d11-openxr-depth-probe}
proton_dir=/home/ajg/.config/heroic/tools/proton/GE-Proton10-32
prefix_dir=/home/ajg/Games/Heroic/Prefixes/MetaHorizon/pfx
umu_run=${UMU_RUN:-}

if [[ -z "$umu_run" ]]; then
  umu_run=$(command -v umu-run || true)
fi
if [[ -z "$umu_run" && -x /home/ajg/.local/share/lutris/runtime/umu/umu-run ]]; then
  umu_run=/home/ajg/.local/share/lutris/runtime/umu/umu-run
fi
if [[ -z "$umu_run" ]]; then
  echo "FAIL: umu-run was not found; set UMU_RUN to its absolute path" >&2
  exit 127
fi

case "$stage" in
  enumerate|views|clear|sample|teardown|recreate|discard) ;;
  *) echo "usage: $0 [enumerate|views|clear|sample|teardown|recreate|discard] [probe-dir]" >&2; exit 2 ;;
esac

before=$(journalctl -k --no-pager | grep -c 'NVRM: Xid' || true)
started=$(date --iso-8601=seconds)

set +e
(
  cd "$probe_dir"
  WINEPREFIX="$prefix_dir" \
  PROTONPATH="$proton_dir" \
  GAMEID=0 \
  DXVK_LOG_LEVEL=debug \
  "$umu_run" ./d3d11-openxr-depth-probe.exe "$stage"
)
status=$?
set -e

after=$(journalctl -k --no-pager | grep -c 'NVRM: Xid' || true)
if (( after > before )); then
  echo "FAIL: a new NVIDIA Xid appeared after $started; aborting the probe sequence" >&2
  journalctl -k --no-pager --since "$started" | grep 'NVRM: Xid' >&2 || true
  exit 99
fi

exit "$status"
