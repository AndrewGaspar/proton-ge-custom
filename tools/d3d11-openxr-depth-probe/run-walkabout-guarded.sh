#!/usr/bin/env bash
set -euo pipefail

mode=${1:-check}
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
heroic_root=${HEROIC_ROOT:-/home/ajg/code/HeroicGamesLauncher}
compat_data_dir=/home/ajg/Games/Heroic/Prefixes/MetaHorizon
prefix_dir="$compat_data_dir/pfx"
proton_dir=/home/ajg/.config/heroic/tools/proton/GE-Proton10-32
wineserver="$proton_dir/files/bin/wineserver"
player_log="$prefix_dir/drive_c/users/steamuser/AppData/LocalLow/Mighty Coconut/Walkabout Mini Golf/Player.log"
oaf_log="$prefix_dir/drive_c/users/steamuser/AppData/Local/HeroicMeta/oaf.jsonl"
heroic_electron="$heroic_root/node_modules/electron/dist/electron"
heroic_build="$heroic_root/build"
heroic_main="$heroic_root/build/main/main.js"
heroic_package="$heroic_root/package.json"
game_dir="$prefix_dir/drive_c/MetaGames/Software/mighty-coconut-walkabout-mini-golf"
plugins_dir="$game_dir/WalkaboutMiniGolf_Data/Plugins/x86_64"
ovr_plugin="$plugins_dir/OVRPlugin.dll"
oculus_xr_plugin="$plugins_dir/OculusXRPlugin.dll"
installed_json=/home/ajg/.config/heroic/meta_store/installed.json
game_url='heroic://launch?appName=mighty-coconut-walkabout-mini-golf&runner=meta&gui=false'
run_timeout=${WALKABOUT_RUN_TIMEOUT_SECONDS:-90}
healthy_seconds=${WALKABOUT_HEALTHY_SECONDS:-20}
artifact_root=${WALKABOUT_ARTIFACT_ROOT:-${XDG_STATE_HOME:-$HOME/.local/state}/hypxrland/walkabout-d32s8}

wineopenxr_pe="$proton_dir/files/lib/wine/x86_64-windows/wineopenxr.dll"
wineopenxr_so="$proton_dir/files/lib/wine/x86_64-unix/wineopenxr.so"
prefix_wineopenxr_pe="$prefix_dir/drive_c/windows/system32/wineopenxr.dll"
dxvk_dir="$proton_dir/files/lib/wine/dxvk/x86_64-windows"
prefix_system32="$prefix_dir/drive_c/windows/system32"
wineopenxr_json="$proton_dir/files/share/openxr/wineopenxr64.json"
prefix_wineopenxr_json="$prefix_dir/drive_c/openxr/wineopenxr64.json"

expected_wineopenxr_pe=5d81cdcf216c32ed4c29e3d6d5b58b1f6522568dbbef60422460fbb576e51c9b
expected_wineopenxr_so=daf53de43cc8df0e7428b13bd51b5d12533b55a431a7971f6d531c3a2ab20e2f
expected_d3d11=f5b94695a2f810128f375b66e643cb9d38a75ebae6ce48addeda9f10d30ae7a3
expected_dxgi=18acad33d52582876abf8b67fe1b2293ae3b68d46db50bb4a84b74626fc6775b
expected_ovr=242ad083abff0364a0618aaf1d841a494137720994a5f65981ce28f9f48aa082
expected_oculus_xr=cfde3a993a9ea64c8008fec795b79ce303952df49a3ca9ccec2f8265032ba4ff
expected_walkabout=543a6fe5f0c9218e70b59787b38f86e3c597dc3e70cd5f67895cf1f008f639f1
expected_wineopenxr_json=414d9c592a68ee6051441aab8b0df2437f3448e5f96003c25fc7a40d152d6316
expected_heroic_main=e7748b515afe02550f0668f30d616e00e835a41bcc257f57deb5f376b3b0929f
expected_heroic_build=204931131c07372280f9d4dafef0704acb86b6d670cfdcc68c92bd455be81e03
expected_heroic_electron=86c4ad14174d04fdc9e52537ac386c807ffd2757452190cb8a1fdbb39b02d01e
expected_heroic_package=59080b51746e0c289aadf48c8a6cda1f87c27d004471d7d40d951b6a4f8f946c

case "$mode" in
  check|run) ;;
  *) echo "usage: $0 [check|run]" >&2; exit 2 ;;
esac

fail()
{
  echo "FAIL: $*" >&2
  exit 1
}

require_hash()
{
  local path=$1
  local expected=$2
  local actual

  [[ -f "$path" ]] || fail "required artifact is missing: $path"
  actual=$(sha256sum "$path" | awk '{print $1}')
  [[ "$actual" == "$expected" ]] ||
    fail "unexpected SHA256 for $path: $actual (expected $expected)"
}

require_bytes()
{
  local path=$1
  local offset=$2
  local count=$3
  local expected=$4
  local actual

  actual=$(od -An -v -tx1 -j "$offset" -N "$count" "$path" | tr -d ' \n')
  [[ "$actual" == "$expected" ]] ||
    fail "unexpected bytes in $path at offset $offset: $actual (expected $expected)"
}

tree_hash()
{
  local root=$1

  (
    cd "$root"
    find . -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum
  ) | sha256sum | awk '{print $1}'
}

find_heroic_lock_holders()
{
  local proc exe

  for proc in /proc/[0-9]*; do
    exe=$(readlink "$proc/exe" 2>/dev/null || true)
    case "$exe" in
      "$heroic_electron"|/opt/Heroic/heroic)
        printf '%s\n' "${proc##*/}"
        ;;
    esac
  done
}

for command in journalctl ss jq sha256sum readelf strings od hyprctl pgrep python3 find sort xargs readlink setsid; do
  command -v "$command" >/dev/null || fail "$command is required"
done

systemctl --user is-active --quiet wivrn.service || fail "wivrn.service is not active"
[[ -x "$wineserver" ]] || fail "exact GE-Proton wineserver is missing: $wineserver"
[[ -x "$heroic_electron" ]] || fail "the local Heroic Electron launcher is missing: $heroic_electron"
[[ -d "$heroic_build" ]] || fail "the complete local Heroic build is missing: $heroic_build"
[[ -f "$heroic_main" ]] || fail "the local Heroic main bundle is missing: $heroic_main"
[[ -f "$heroic_build/index.html" ]] || fail "the local Heroic renderer is missing"
[[ -f "$heroic_build/preload/index.js" ]] || fail "the local Heroic preload is missing"
[[ -f "$heroic_package" ]] || fail "the local Heroic package manifest is missing"
grep -q 'Meta Horizon shared runtime' "$heroic_main" ||
  fail "the local Heroic bundle does not contain the Meta runner"
require_hash "$heroic_electron" "$expected_heroic_electron"
require_hash "$heroic_package" "$expected_heroic_package"
actual_heroic_build=$(tree_hash "$heroic_build")
[[ "$actual_heroic_build" == "$expected_heroic_build" ]] ||
  fail "unexpected local Heroic build tree SHA256: $actual_heroic_build (expected $expected_heroic_build)"
heroic_lock_holders=$(find_heroic_lock_holders)
[[ -z "$heroic_lock_holders" ]] ||
  fail "Heroic already owns the single-instance lock (PIDs: ${heroic_lock_holders//$'\n'/,}); stop it before testing"

build_id=$(jq -r '.installed[] | select(.appName == "mighty-coconut-walkabout-mini-golf") | .buildId' "$installed_json")
[[ "$build_id" == 780 ]] || fail "Walkabout buildId is $build_id, expected 780"
[[ -f "$game_dir/WalkaboutMiniGolf.exe" ]] || fail "Walkabout executable is missing"
require_hash "$game_dir/WalkaboutMiniGolf.exe" "$expected_walkabout"
require_hash "$heroic_main" "$expected_heroic_main"

require_hash "$wineopenxr_pe" "$expected_wineopenxr_pe"
require_hash "$prefix_wineopenxr_pe" "$expected_wineopenxr_pe"
require_hash "$wineopenxr_so" "$expected_wineopenxr_so"
require_hash "$wineopenxr_json" "$expected_wineopenxr_json"
require_hash "$prefix_wineopenxr_json" "$expected_wineopenxr_json"
require_hash "$dxvk_dir/d3d11.dll" "$expected_d3d11"
require_hash "$prefix_system32/d3d11.dll" "$expected_d3d11"
require_hash "$dxvk_dir/dxgi.dll" "$expected_dxgi"
require_hash "$prefix_system32/dxgi.dll" "$expected_dxgi"

readelf_symbols=$(readelf -Ws "$wineopenxr_so")
table_bytes=$(awk '$NF == "__wine_unix_call_funcs" {print $3; exit}' <<<"$readelf_symbols")
[[ "$table_bytes" == 3184 ]] ||
  fail "WineOpenXR Unix dispatch table is $table_bytes bytes, expected 3184"
pe_strings=$(strings "$wineopenxr_pe")
so_strings=$(strings "$wineopenxr_so")
grep -Fq 'PE/Unix ABI handshake failed' <<<"$pe_strings" ||
  fail "WineOpenXR PE lacks the private-ABI handshake"
grep -Fq 'PE/Unix ABI magic mismatch' <<<"$so_strings" ||
  fail "WineOpenXR Unix library lacks the private-ABI handshake"
grep -Fq 'PE/Unix ABI request marker missing' <<<"$so_strings" ||
  fail "WineOpenXR Unix library cannot reject a legacy PE safely"

require_hash "$ovr_plugin" "$expected_ovr"
require_hash "$oculus_xr_plugin" "$expected_oculus_xr"
require_bytes "$ovr_plugin" 790968 1 cf
require_bytes "$ovr_plugin" 1446208 2 751d
require_bytes "$oculus_xr_plugin" 41068 2 31c9
require_bytes "$oculus_xr_plugin" 50544 8 880df9720500c3cc

if pgrep -x WalkaboutMiniGo >/dev/null; then
  fail "Walkabout is already running"
fi
connections=$(ss -Htpn state established)
if ! grep -q 'wivrn-server' <<<"$connections"; then
  fail "no established WiVRn headset transport is visible"
fi

instances=$(env -u HYPRLAND_INSTANCE_SIGNATURE hyprctl instances -j)
instance_count=$(jq 'length' <<<"$instances")
(( instance_count == 1 )) ||
  fail "expected exactly one live Hyprland instance, found $instance_count"
instance_signature=$(jq -r '.[0].instance' <<<"$instances")
xr_status=$(HYPRLAND_INSTANCE_SIGNATURE="$instance_signature" hyprctl openxr status)
grep -Fqx 'state: focused' <<<"$xr_status" || fail "live OpenXR session is not focused"
grep -Fqx 'visible: yes' <<<"$xr_status" || fail "live OpenXR session is not visible"
grep -Fqx 'presence: yes' <<<"$xr_status" || fail "headset presence is not active"

echo "PASS: exact runtime, DXVK, game patches, Heroic launch path, and live headset gate are ready"
if [[ "$mode" == check ]]; then
  exit 0
fi
if [[ ${ALLOW_LIVE_WALKABOUT:-0} != 1 ]]; then
  fail "set ALLOW_LIVE_WALKABOUT=1 for one guarded live launch"
fi
[[ "$healthy_seconds" =~ ^[0-9]+$ ]] && (( healthy_seconds >= 20 )) ||
  fail "WALKABOUT_HEALTHY_SECONDS must be an integer of at least 20"
[[ "$run_timeout" =~ ^[0-9]+$ ]] && (( run_timeout >= healthy_seconds + 30 )) ||
  fail "WALKABOUT_RUN_TIMEOUT_SECONDS must leave at least 30 seconds for startup"

run_id=$(date +%Y%m%d-%H%M%S)
artifact_dir="$artifact_root/walkabout-staged-$run_id"
dxvk_log_dir="$artifact_dir/dxvk"
mkdir -p "$dxvk_log_dir"
started=$(date --iso-8601=seconds)
before_xids=$(journalctl -k --no-pager | grep -c 'NVRM: Xid' || true)
heroic_pid=
walkabout_pid=
classification=launcher-failed
finalized=0
runtime_capable=()
server_pid=$(pgrep -o -x wivrn-server || true)
if [[ -n "$server_pid" ]]; then
  server_strings=$(strings "/proc/$server_pid/exe")
  if grep -Fq 'Waiting on client fence timed out > 100ms! client=%u' <<<"$server_strings"; then
    runtime_capable=(--runtime-capable)
  fi
fi

stop_exact_prefix()
{
  WINEPREFIX="$prefix_dir" "$wineserver" -k || true
}

capture_logs()
{
  journalctl -k --no-pager --since "$started" >"$artifact_dir/kernel.log" || true
  if [[ -n ${journal_cursor:-} ]]; then
    journalctl --user -u wivrn.service --after-cursor "$journal_cursor" \
      -o short-precise --no-pager >"$artifact_dir/wivrn.log" || true
  else
    journalctl --user -u wivrn.service --since "$started" \
      -o short-precise --no-pager >"$artifact_dir/wivrn.log" || true
  fi
  if [[ -f "$player_log" ]]; then
    cp -- "$player_log" "$artifact_dir/Player.log"
  fi
  if [[ -f "$oaf_log" ]]; then
    cp -- "$oaf_log" "$artifact_dir/oaf.jsonl"
  fi
  ps -eo pid,ppid,lstart,stat,comm,args >"$artifact_dir/processes-after.txt"
}

finish()
{
  local status=$1
  local result=$2
  local after_xids
  local attribution_status=0
  local heroic_pgid

  (( finalized == 0 )) || return
  finalized=1
  trap - EXIT INT TERM HUP
  classification=$result

  if [[ -n "$heroic_pid" ]] && kill -0 "$heroic_pid" 2>/dev/null; then
    heroic_pgid=$(ps -o pgid= -p "$heroic_pid" 2>/dev/null | tr -d ' ' || true)
    if [[ "$heroic_pgid" == "$heroic_pid" ]]; then
      kill -- "-$heroic_pid" 2>/dev/null || true
    else
      kill "$heroic_pid" 2>/dev/null || true
    fi
    wait "$heroic_pid" 2>/dev/null || true
  fi
  stop_exact_prefix
  sleep 2
  capture_logs

  after_xids=$(journalctl -k --no-pager | grep -c 'NVRM: Xid' || true)
  if (( after_xids > before_xids )); then
    status=99
    classification=nvidia-xid
    echo "FAIL: a new NVIDIA Xid appeared; the exact Meta prefix was stopped" >&2
  fi

  if [[ -s "$artifact_dir/wivrn.log" && -n "$walkabout_pid" ]]; then
    set +e
    python3 "$script_dir/attribute_fence_log.py" "$artifact_dir/wivrn.log" \
      --application Unity --pid "$walkabout_pid" "${runtime_capable[@]}" \
      >"$artifact_dir/fence-attribution.txt" 2>&1
    attribution_status=$?
    set -e
    if (( attribution_status != 0 && status == 0 )); then
      status=98
      classification=fence-attribution-failed
      echo "FAIL: exact Unity client fence attribution did not pass" >&2
    fi
  elif (( status == 0 )); then
    status=98
    classification=fence-attribution-missing
    echo "FAIL: no exact Unity client was available for fence attribution" >&2
  fi

  printf '%s\n' "$classification" >"$artifact_dir/classification.txt"
  printf 'started=%s\nbefore_xids=%s\nafter_xids=%s\nwalkabout_pid=%s\nexit_status=%s\n' \
    "$started" "$before_xids" "$after_xids" "$walkabout_pid" "$status" \
    >"$artifact_dir/run.properties"
  sha256sum "$artifact_dir"/* "$artifact_dir"/dxvk/* 2>/dev/null \
    >"$artifact_dir/SHA256SUMS" || true
  echo "INFO: Walkabout evidence preserved at $artifact_dir"
  exit "$status"
}

trap 'finish $? "$classification"' EXIT
trap 'finish 130 interrupted' INT TERM HUP

# Make the launch cold and ensure the only Wine processes terminated by the
# harness belong to this exact Meta prefix.
stop_exact_prefix
sleep 1
journal_cursor=$(journalctl --user -u wivrn.service -n 0 --show-cursor --no-pager |
  sed -n 's/^-- cursor: //p' | tail -n 1)
before_xids=$(journalctl -k --no-pager | grep -c 'NVRM: Xid' || true)
started=$(date --iso-8601=seconds)
if [[ -f "$player_log" ]]; then
  cp -- "$player_log" "$artifact_dir/Player.before.log"
fi
if [[ -f "$oaf_log" ]]; then
  cp -- "$oaf_log" "$artifact_dir/oaf.before.jsonl"
fi
ps -eo pid,ppid,lstart,stat,comm,args >"$artifact_dir/processes-before.txt"
sha256sum "$wineopenxr_pe" "$wineopenxr_so" "$prefix_wineopenxr_pe" \
  "$dxvk_dir/d3d11.dll" "$dxvk_dir/dxgi.dll" \
  "$prefix_system32/d3d11.dll" "$prefix_system32/dxgi.dll" \
  "$wineopenxr_json" "$prefix_wineopenxr_json" \
  "$game_dir/WalkaboutMiniGolf.exe" "$ovr_plugin" "$oculus_xr_plugin" \
  >"$artifact_dir/active-artifacts.sha256"
printf '%s\n' "$xr_status" >"$artifact_dir/openxr-status-before.txt"

(
  cd "$heroic_root"
  exec setsid env \
    HYPRLAND_INSTANCE_SIGNATURE="$instance_signature" \
    ELECTRON_RENDERER_URL="file://$heroic_build/index.html" \
    WINEOPENXR_STAGED_CROSS_GPU_COLOR=1 \
    WINEDLLOVERRIDES='d3d11=n;dxgi=n' \
    DXVK_LOG_LEVEL=info \
    DXVK_LOG_PATH="$dxvk_log_dir" \
    "$heroic_electron" . --no-gui "$game_url"
) >"$artifact_dir/heroic.stdout.log" 2>"$artifact_dir/heroic.stderr.log" &
heroic_pid=$!

deadline=$((SECONDS + run_timeout))
unity_connected_at=0
unity_client_id=
workload_ready_at=0
while (( SECONDS < deadline )); do
  after_xids=$(journalctl -k --no-pager | grep -c 'NVRM: Xid' || true)
  if (( after_xids > before_xids )); then
    finish 99 nvidia-xid
  fi

  if [[ -z "$walkabout_pid" ]]; then
    walkabout_pid=$(pgrep -n -x WalkaboutMiniGo || true)
    if [[ -n "$walkabout_pid" ]]; then
      tr '\0' '\n' <"/proc/$walkabout_pid/environ" |
        grep -E '^(WINEOPENXR_STAGED_CROSS_GPU_COLOR|WINEDLLOVERRIDES|DXVK_LOG_LEVEL|DXVK_LOG_PATH)=' \
          >"$artifact_dir/walkabout.xr-environ"
      ps -p "$walkabout_pid" -o pid,ppid,lstart,stat,comm,args \
        >"$artifact_dir/walkabout.process"
      grep -Fqx 'WINEOPENXR_STAGED_CROSS_GPU_COLOR=1' "$artifact_dir/walkabout.xr-environ" ||
        finish 1 staged-env-missing
      grep -Eq '^WINEDLLOVERRIDES=.*d3d11=n.*dxgi=n' "$artifact_dir/walkabout.xr-environ" ||
        finish 1 native-dxvk-override-missing
      classification=walkabout-started
    fi
  elif ! kill -0 "$walkabout_pid" 2>/dev/null; then
    finish 1 walkabout-exited-early
  fi

  journalctl --user -u wivrn.service --after-cursor "$journal_cursor" --no-pager \
    >"$artifact_dir/wivrn-live.log"
  recent=$(<"$artifact_dir/wivrn-live.log")
  if [[ -n "$walkabout_pid" && -z "$unity_client_id" ]]; then
    set +e
    unity_client_id=$(python3 "$script_dir/attribute_fence_log.py" \
      "$artifact_dir/wivrn-live.log" --application Unity --pid "$walkabout_pid" \
      --print-client-id)
    resolver_status=$?
    set -e
    if (( resolver_status != 0 )); then
      unity_client_id=
    fi
  fi
  if [[ -n "$unity_client_id" ]]; then
    if (( unity_connected_at == 0 )); then
      unity_connected_at=$SECONDS
      classification=unity-connected
    fi
    if grep -Fq "Client $unity_client_id disconnected" <<<"$recent"; then
      finish 1 unity-client-disconnected-early
    fi
  fi

  if [[ -n "$walkabout_pid" && $unity_connected_at -ne 0 && -f "$player_log" ]]; then
    player_fresh=1
    if [[ -f "$artifact_dir/Player.before.log" ]] &&
      cmp -s "$player_log" "$artifact_dir/Player.before.log"; then
      player_fresh=0
    fi
    if (( player_fresh )) &&
      grep -Fq 'Shared Depth Buffer: Yes' "$player_log" &&
      grep -Eq 'XRTextureManager::SetupRenderTextureFromXRRequest .* d: 0*[1-9A-Fa-f][0-9A-Fa-f]*' "$player_log"; then
      if (( workload_ready_at == 0 )); then
        cp -- "/proc/$walkabout_pid/maps" "$artifact_dir/walkabout.maps"
        if grep -Fq "$prefix_wineopenxr_pe" "$artifact_dir/walkabout.maps"; then
          mapped_wineopenxr_pe=$prefix_wineopenxr_pe
        elif grep -Fq "$wineopenxr_pe" "$artifact_dir/walkabout.maps"; then
          mapped_wineopenxr_pe=$wineopenxr_pe
        else
          finish 1 required-map-missing-wineopenxr.dll
        fi
        for mapped_path in \
          "$wineopenxr_so" \
          "$prefix_system32/d3d11.dll" "$prefix_system32/dxgi.dll"; do
          grep -Fq "$mapped_path" "$artifact_dir/walkabout.maps" ||
            finish 1 "required-map-missing-$(basename "$mapped_path")"
        done
        awk '$NF ~ /(wineopenxr|d3d11|dxgi)\.(dll|so)$/ {print $NF}' \
          "$artifact_dir/walkabout.maps" | sort -u >"$artifact_dir/walkabout.mapped-paths"
        sha256sum "$mapped_wineopenxr_pe" "$wineopenxr_so" \
          "$prefix_system32/d3d11.dll" "$prefix_system32/dxgi.dll" \
          >"$artifact_dir/walkabout.mapped-artifacts.sha256"
        workload_ready_at=$SECONDS
        classification=depth-workload-observed
      fi
    fi
  fi

  if [[ -n "$walkabout_pid" && $workload_ready_at -ne 0 ]] &&
    (( SECONDS - workload_ready_at >= healthy_seconds )); then
    dxgi_log="$dxvk_log_dir/WalkaboutMiniGolf_dxgi.log"
    d3d11_log="$dxvk_log_dir/WalkaboutMiniGolf_d3d11.log"
    [[ -s "$dxgi_log" ]] || finish 1 dxgi-log-missing
    [[ -s "$d3d11_log" ]] || finish 1 d3d11-log-missing
    grep -Fq 'DXVK: 2.7.1' "$dxgi_log" || finish 1 native-dxvk-version-invalid
    grep -Fq 'NVIDIA' "$dxgi_log" || finish 1 wrong-dxgi-adapter
    grep -Fq 'D3D11' "$d3d11_log" || finish 1 native-d3d11-log-invalid
    grep -Fq 'NVIDIA' "$d3d11_log" || finish 1 wrong-d3d11-adapter
    finish 0 unity-survived-with-staged-color
  fi
  sleep 0.25
done

finish 1 timeout-before-healthy-unity
