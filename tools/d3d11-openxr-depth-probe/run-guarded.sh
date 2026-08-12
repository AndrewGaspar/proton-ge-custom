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
stdout_log="$probe_dir/d3d11-openxr-depth-probe.stdout.log"
stderr_log="$probe_dir/d3d11-openxr-depth-probe.stderr.log"
: > "$stdout_log"
: > "$stderr_log"

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

cat "$stdout_log"
cat "$stderr_log" >&2

case "$stage" in
  enumerate) expected='PASS: create and enumerate only' ;;
  views) expected='PASS: create, enumerate, and OVR-style views' ;;
  clear) expected='PASS: two clear cycles and GPU event fences completed' ;;
  sample) expected='PASS: two clear/sample cycles and GPU event fences completed' ;;
  teardown) expected='PASS: swapchain teardown drained outstanding D3D11 work; post-destroy fence completed' ;;
  recreate) expected='PASS: six depth swapchains destroyed, recreated, cleared, fenced, and destroyed again' ;;
  discard) expected='PASS: six depth swapchains destroyed, recreated, discarded/cleared, fenced, and destroyed again' ;;
esac

if (( status != 0 )); then
  echo "FAIL: UMU/Proton returned status $status for stage $stage" >&2
  exit "$status"
fi
if ! grep -Fqx "$expected" "$stdout_log"; then
  echo "FAIL: stage $stage did not write its expected PASS marker: $expected" >&2
  exit 1
fi

exit 0
