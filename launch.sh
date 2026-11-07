#!/usr/bin/env bash
# Opt-in TotK live actor spawner with an X11-only, focus-aware J shortcut.
# The existing Steam Controller / DSU launcher is left untouched.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
recomp_root="${TOTK_RECOMP_ROOT:-$HOME/Applications/TotK-Recomp}"
export TOTK_RECOMP_GAME_EXE="$recomp_root/game/totk"
if [[ ! -s "$root/libtotkspawn.so" ]]; then
    echo "Build the plugin first: $root/build.sh" >&2
    exit 2
fi
export TOTK_LIVE_SPAWN=1
export TOTK_LIVE_SPAWN_SVC="${TOTK_LIVE_SPAWN_SVC:-1}"
export LD_PRELOAD="$root/libtotkspawn.so${LD_PRELOAD:+:$LD_PRELOAD}"

python3 -u "$root/hotkey.py" &
hotkey_pid=$!
launcher_pid=''
cleanup() {
    trap - EXIT INT TERM
    if [[ -n "$launcher_pid" ]] && kill -0 "$launcher_pid" 2>/dev/null; then
        kill -TERM "$launcher_pid" 2>/dev/null || true
        wait "$launcher_pid" 2>/dev/null || true
    fi
    if kill -0 "$hotkey_pid" 2>/dev/null; then
        kill -TERM "$hotkey_pid" 2>/dev/null || true
        wait "$hotkey_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
if [[ ! -x "$recomp_root/launch.sh" ]]; then
    echo "TotK launcher missing: $recomp_root/launch.sh (set TOTK_RECOMP_ROOT)" >&2
    exit 2
fi
"$recomp_root/launch.sh" "$@" &
launcher_pid=$!
set +e
wait "$launcher_pid"
result=$?
set -e
exit "$result"
