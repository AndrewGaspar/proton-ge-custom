#!/usr/bin/env bash
set -euo pipefail

stage=${1:-enumerate}
probe_dir=${2:-/tmp/d3d11-openxr-depth-probe}
proton_dir=/home/ajg/.config/heroic/tools/proton/GE-Proton10-32
compat_data_dir=/home/ajg/Games/Heroic/Prefixes/MetaHorizon
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
  bootstrap|enumerate|views|clear|sample|teardown|recreate|discard|draw|frame|unity) ;;
  *) echo "usage: $0 [bootstrap|enumerate|views|clear|sample|teardown|recreate|discard|draw|frame|unity] [probe-dir]" >&2; exit 2 ;;
esac

case "$stage" in
  bootstrap) stage_timeout=30s ;;
  enumerate|views|clear|sample) stage_timeout=45s ;;
  teardown|recreate|discard|draw) stage_timeout=60s ;;
  frame|unity) stage_timeout=90s ;;
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
  timeout --kill-after=5s "$stage_timeout" \
    env WINEPREFIX="$compat_data_dir" \
        PROTONPATH="$proton_dir" \
        GAMEID=999999999 \
        UMU_USE_STEAM=0 \
        PROTON_VERB=run \
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
  bootstrap) expected='PASS: Proton and Wine VR bootstrap completed' ;;
  enumerate) expected='PASS: create and enumerate only' ;;
  views) expected='PASS: create, enumerate, and OVR-style views' ;;
  clear) expected='PASS: two clear cycles and GPU event fences completed' ;;
  sample) expected='PASS: two clear/sample cycles and GPU event fences completed' ;;
  teardown) expected='PASS: swapchain teardown drained outstanding D3D11 work; post-destroy fence completed' ;;
  recreate) expected='PASS: six depth swapchains destroyed, recreated, cleared, fenced, and destroyed again' ;;
  discard) expected='PASS: six depth swapchains destroyed, recreated, discarded/cleared, fenced, and destroyed again' ;;
  draw) expected='PASS: paired color RTV and D32S8 DSV draw completed with an event fence' ;;
  frame) expected='PASS: stereo color/depth draw and projection+depth xrEndFrame completed' ;;
  unity) expected='PASS: Unity-style color/depth rebuild and projection+depth frame completed' ;;
esac

if (( status != 0 )); then
  echo "FAIL: UMU/Proton returned status $status for stage $stage" >&2
  exit "$status"
fi
if ! tr -d '\r' < "$stdout_log" | grep -Fqx "$expected"; then
  echo "FAIL: stage $stage did not write its expected PASS marker: $expected" >&2
  exit 1
fi

exit 0
