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
  bootstrap|enumerate|views|clear|sample|teardown|recreate|discard|draw-color|draw-depth|draw-native-pair|draw-bind|draw-dsv-disabled|draw|frame|unity) ;;
  *) echo "usage: $0 [bootstrap|enumerate|views|clear|sample|teardown|recreate|discard|draw-color|draw-depth|draw-native-pair|draw-bind|draw-dsv-disabled|draw|frame|unity] [probe-dir]" >&2; exit 2 ;;
esac

umu_env=(env -u HYPRLAND_INSTANCE_SIGNATURE)
probe_dll_overrides=${WINEDLLOVERRIDES:-}
if [[ -n "$probe_dll_overrides" ]]; then
  probe_dll_overrides+=';'
fi
probe_dll_overrides+='d3d11=n;dxgi=n'
umu_env+=(WINEDLLOVERRIDES="$probe_dll_overrides")
if [[ -n ${WINEOPENXR_STAGED_CROSS_GPU_COLOR:-} ]]; then
  umu_env+=(WINEOPENXR_STAGED_CROSS_GPU_COLOR="$WINEOPENXR_STAGED_CROSS_GPU_COLOR")
  echo "INFO: propagating WINEOPENXR_STAGED_CROSS_GPU_COLOR=$WINEOPENXR_STAGED_CROSS_GPU_COLOR to Proton"
fi
case "$stage" in
  bootstrap) stage_timeout=30s ;;
  enumerate|views|clear|sample) stage_timeout=45s ;;
  teardown|recreate|discard|draw-color|draw-depth|draw-native-pair|draw-bind|draw-dsv-disabled|draw) stage_timeout=60s ;;
  frame|unity) stage_timeout=90s ;;
esac

case "$stage" in
  draw-color|draw-depth|draw-native-pair|draw-bind|draw-dsv-disabled|draw|frame|unity)
    command -v jq >/dev/null || {
      echo "FAIL: jq is required to resolve the live Hyprland instance" >&2
      exit 127
    }
    instances=$(env -u HYPRLAND_INSTANCE_SIGNATURE hyprctl instances -j)
    instance_count=$(jq 'length' <<<"$instances")
    if (( instance_count != 1 )); then
      echo "FAIL: expected exactly one live Hyprland instance, found $instance_count" >&2
      exit 1
    fi
    instance_signature=$(jq -r '.[0].instance' <<<"$instances")
    xr_status=$(HYPRLAND_INSTANCE_SIGNATURE="$instance_signature" hyprctl openxr status)
    grep -Fqx 'state: focused' <<<"$xr_status" || {
      echo "FAIL: live Hyprland instance is not focused" >&2
      exit 1
    }
    grep -Fqx 'visible: yes' <<<"$xr_status" || {
      echo "FAIL: live Hyprland instance is not visible" >&2
      exit 1
    }
    grep -Fqx 'presence: yes' <<<"$xr_status" || {
      echo "FAIL: live Hyprland instance does not report user presence" >&2
      exit 1
    }
    umu_env+=(HYPRLAND_INSTANCE_SIGNATURE="$instance_signature")
    echo "PASS: live Hyprland instance $instance_signature is focused, visible, and present"
    ;;
esac

before=$(journalctl -k --no-pager | grep -c 'NVRM: Xid' || true)
started=$(date --iso-8601=seconds)
journal_cursor=$(journalctl --user -u wivrn.service -n 0 --show-cursor --no-pager |
  sed -n 's/^-- cursor: //p' | tail -n 1)
stdout_log="$probe_dir/d3d11-openxr-depth-probe.stdout.log"
stderr_log="$probe_dir/d3d11-openxr-depth-probe.stderr.log"
wivrn_log="$probe_dir/d3d11-openxr-depth-probe.wivrn.log"
runtime_capable=()
server_pid=$(pgrep -o -x wivrn-server || true)
if [[ -n "$server_pid" ]] &&
  strings "/proc/$server_pid/exe" |
    grep -F 'Waiting on client fence timed out > 100ms! client=%u' >/dev/null; then
  runtime_capable=(--runtime-capable)
fi
: > "$stdout_log"
: > "$stderr_log"

set +e
(
  cd "$probe_dir"
  timeout --kill-after=5s "$stage_timeout" \
    "${umu_env[@]}" WINEPREFIX="$compat_data_dir" \
        PROTONPATH="$proton_dir" \
        GAMEID=999999999 \
        UMU_USE_STEAM=0 \
        PROTON_USE_XALIA=0 \
        PROTON_VERB=runinprefix \
        DXVK_LOG_LEVEL=debug \
        "$umu_run" ./d3d11-openxr-depth-probe.exe "$stage"
)
status=$?
set -e

# A disconnect callback precedes compositor teardown. Give the per-client wait
# thread time to emit its terminal/recovery result before taking the journal.
sleep 2
journalctl --user -u wivrn.service --after-cursor "$journal_cursor" \
  -o short-precise --no-pager >"$wivrn_log" || true

after=$(journalctl -k --no-pager | grep -c 'NVRM: Xid' || true)
if (( after > before )); then
  echo "FAIL: a new NVIDIA Xid appeared after $started; aborting the probe sequence" >&2
  journalctl -k --no-pager --since "$started" | grep 'NVRM: Xid' >&2 || true
  exit 99
fi

cat "$stdout_log"
cat "$stderr_log" >&2

case "$stage" in
  bootstrap) expected='PASS: Proton and Wine OpenXR bootstrap completed without creating an XR session' ;;
  enumerate) expected='PASS: create and enumerate only' ;;
  views) expected='PASS: create, enumerate, and OVR-style views' ;;
  clear) expected='PASS: two clear cycles and GPU event fences completed' ;;
  sample) expected='PASS: two clear/sample cycles and GPU event fences completed' ;;
  teardown) expected='PASS: swapchain teardown drained outstanding D3D11 work; post-destroy fence completed' ;;
  recreate) expected='PASS: six depth swapchains destroyed, recreated, cleared, fenced, and destroyed again' ;;
  discard) expected='PASS: six depth swapchains destroyed, recreated, discarded/cleared, fenced, and destroyed again' ;;
  draw-color) expected='PASS: imported cross-GPU color-only draw completed with an event fence' ;;
  draw-depth) expected='PASS: client-local D32S8 depth-only draw completed with an event fence' ;;
  draw-native-pair) expected='PASS: native NVIDIA color plus client-local D32S8 draw completed with an event fence' ;;
  draw-bind) expected='PASS: mixed imported color and local D32S8 bind without Draw completed with an event fence' ;;
  draw-dsv-disabled) expected='PASS: mixed pair color draw with DSV bound and depth disabled completed with an event fence' ;;
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

case "$stage" in
  draw-color|draw-depth|draw-native-pair|draw-bind|draw-dsv-disabled|draw|frame|unity)
    for marker in \
      'cleanup: destroying OpenXR session' \
      'cleanup: releasing D3D11 context and device' \
      'cleanup: destroying OpenXR instance' \
      'cleanup: unloading OpenXR loader' \
      'cleanup: complete'; do
      grep -Fq "$marker" "$stderr_log" || {
        echo "FAIL: stage $stage omitted cleanup marker: $marker" >&2
        exit 1
      }
    done

    set +e
    python3 "$probe_dir/attribute_fence_log.py" "$wivrn_log" "${runtime_capable[@]}"
    attribution_status=$?
    set -e
    if (( attribution_status == 1 )); then
      exit 98
    fi
    ;;
esac

exit 0
