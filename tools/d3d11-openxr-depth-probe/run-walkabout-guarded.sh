#!/usr/bin/env bash
set -euo pipefail

mode=${1:-check}
heroic_root=${HEROIC_ROOT:-/home/ajg/code/HeroicGamesLauncher}
compat_data_dir=/home/ajg/Games/Heroic/Prefixes/MetaHorizon
prefix_dir="$compat_data_dir/pfx"
proton_dir=/home/ajg/.config/heroic/tools/proton/GE-Proton10-32
wineserver="$proton_dir/files/bin/wineserver"
player_log="$prefix_dir/drive_c/users/steamuser/AppData/LocalLow/Mighty Coconut/Walkabout Mini Golf/Player.log"
heroic_electron="$heroic_root/node_modules/.bin/electron"
heroic_main="$heroic_root/build/main/main.js"
game_url='heroic://launch?appName=mighty-coconut-walkabout-mini-golf&runner=meta&gui=false'
run_timeout=${WALKABOUT_RUN_TIMEOUT_SECONDS:-90}
artifact_root=${WALKABOUT_ARTIFACT_ROOT:-${XDG_STATE_HOME:-$HOME/.local/state}/hypxrland/walkabout-d32s8}

case "$mode" in
  check|run) ;;
  *) echo "usage: $0 [check|run]" >&2; exit 2 ;;
esac

fail()
{
  echo "FAIL: $*" >&2
  exit 1
}

command -v journalctl >/dev/null || fail "journalctl is required"
command -v ss >/dev/null || fail "ss is required"
systemctl --user is-active --quiet wivrn.service || fail "wivrn.service is not active"
[[ -x "$wineserver" ]] || fail "exact GE-Proton wineserver is missing: $wineserver"
[[ -x "$heroic_electron" ]] || fail "the local Heroic Electron launcher is missing: $heroic_electron"
[[ -f "$heroic_main" ]] || fail "the local Heroic main bundle is missing: $heroic_main"
grep -q 'mighty-coconut-walkabout-mini-golf' /home/ajg/.config/heroic/meta_store/installed.json ||
  fail "Walkabout is not registered in Heroic's Meta store"
grep -q 'Meta Horizon shared runtime' "$heroic_main" ||
  fail "the local Heroic bundle does not contain the Meta runner"

if pgrep -f 'WalkaboutMiniGolf.exe|WalkaboutMiniGo' >/dev/null; then
  fail "Walkabout is already running"
fi

if ! ss -Htpn state established | grep -q 'wivrn-server'; then
  fail "no established WiVRn headset transport is visible"
fi
presence=$(journalctl --user -u wivrn.service --no-pager -b |
  grep -E 'headset is (not )?worn' | tail -n 1 || true)
if [[ "$presence" != *'headset is worn'* ]]; then
  fail "WiVRn's latest presence event does not say the headset is worn"
fi

echo "PASS: WiVRn, the headset transport, local Heroic Meta runner, and exact Meta prefix are ready"
if [[ "$mode" == check ]]; then
  exit 0
fi
if [[ ${ALLOW_LIVE_WALKABOUT:-0} != 1 ]]; then
  fail "set ALLOW_LIVE_WALKABOUT=1 for one guarded live launch"
fi

run_id=$(date +%Y%m%d-%H%M%S)
artifact_dir="$artifact_root/$run_id"
mkdir -p "$artifact_dir"
started=$(date --iso-8601=seconds)
before_xids=$(journalctl -k --no-pager | grep -c 'NVRM: Xid' || true)
heroic_pid=
classification=launcher-failed

stop_exact_prefix()
{
  WINEPREFIX="$prefix_dir" "$wineserver" -k || true
}

finish()
{
  status=$?
  if [[ -n "$heroic_pid" ]] && kill -0 "$heroic_pid" 2>/dev/null; then
    kill "$heroic_pid" 2>/dev/null || true
    wait "$heroic_pid" 2>/dev/null || true
  fi
  stop_exact_prefix
  journalctl -k --no-pager --since "$started" >"$artifact_dir/kernel.log" || true
  journalctl --user -u wivrn.service --no-pager --since "$started" >"$artifact_dir/wivrn.log" || true
  if [[ -f "$player_log" ]]; then
    cp -- "$player_log" "$artifact_dir/Player.log"
  fi
  ps -eo pid,ppid,lstart,stat,comm,args >"$artifact_dir/processes-after.txt"
  printf '%s\n' "$classification" >"$artifact_dir/classification.txt"
  printf 'started=%s\nbefore_xids=%s\nexit_status=%s\n' \
    "$started" "$before_xids" "$status" >"$artifact_dir/run.properties"
}
trap finish EXIT INT TERM

ps -eo pid,ppid,lstart,stat,comm,args >"$artifact_dir/processes-before.txt"
(
  cd "$heroic_root"
  exec "$heroic_electron" . --no-gui "$game_url"
) >"$artifact_dir/heroic.stdout.log" 2>"$artifact_dir/heroic.stderr.log" &
heroic_pid=$!

deadline=$((SECONDS + run_timeout))
unity_connected=0
unity_connected_at=0
while (( SECONDS < deadline )); do
  after_xids=$(journalctl -k --no-pager | grep -c 'NVRM: Xid' || true)
  if (( after_xids > before_xids )); then
    classification=nvidia-xid
    echo "FAIL: a new NVIDIA Xid appeared; stopping the exact Meta prefix" >&2
    exit 99
  fi

  recent=$(journalctl --user -u wivrn.service --no-pager --since "$started")
  if grep -q "application_name: 'Unity'" <<<"$recent"; then
    if (( ! unity_connected )); then
      unity_connected=1
      unity_connected_at=$SECONDS
    fi
  fi
  if (( unity_connected )) && grep -q 'Waiting on client fence timed out' <<<"$recent"; then
    classification=client-fence-timeout
    echo "FAIL: Unity connected and WiVRn timed out on its client fence" >&2
    exit 98
  fi
  if (( unity_connected )) && (( SECONDS - unity_connected_at >= 20 )); then
    classification=unity-survived-without-xid
    echo "PASS: Unity remained connected without a new Xid or client-fence timeout"
    exit 0
  fi
  sleep 0.25
done

classification=timeout-before-unity
fail "Walkabout did not reach a classifiable Unity state within ${run_timeout}s"
