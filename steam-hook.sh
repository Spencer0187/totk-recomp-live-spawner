#!/usr/bin/env bash
# Sourced from the user's EXISTING Steam-registered TotK launcher.
# This script never launches TotK: Steam retains control of the original game.
# The caller already defines `root` and `adapter` and starts the DSU bridge.
totk_live_spawn_hotkey_pid=''

totk_live_spawn_setup() {
    if [[ "${TOTK_LIVE_SPAWN_DISABLE:-0}" == '1' ]]; then
        echo '[totk-live-spawn] disabled via TOTK_LIVE_SPAWN_DISABLE=1' >&2
        return 0
    fi
    if [[ ! -s "$TOTK_LIVE_SPAWNER_ROOT/libtotkspawn.so" ]]; then
        echo "[totk-live-spawn] missing $TOTK_LIVE_SPAWNER_ROOT/libtotkspawn.so; starting unmodified TotK" >&2
        return 0
    fi
    if [[ ! -f "$TOTK_LIVE_SPAWNER_ROOT/hotkey.py" ]]; then
        echo '[totk-live-spawn] missing hotkey.py; starting unmodified TotK' >&2
        return 0
    fi
    # Start before modifying LD_PRELOAD, so the injected .so is loaded only
    # into the native game, not Python or the Steam/DSU helper processes.
    python3 -u "$TOTK_LIVE_SPAWNER_ROOT/hotkey.py" \
        >"$root/input-adapter/live-spawner-hotkey.log" 2>&1 &
    totk_live_spawn_hotkey_pid=$!
    export TOTK_LIVE_SPAWN=1
    export TOTK_LIVE_SPAWN_SVC="${TOTK_LIVE_SPAWN_SVC:-1}"
    # The original launcher prepends the gamepad adapter AFTER this call,
    # preserving the existing input-adapter-first LD_PRELOAD ordering.
    export LD_PRELOAD="$TOTK_LIVE_SPAWNER_ROOT/libtotkspawn.so${LD_PRELOAD:+:$LD_PRELOAD}"
}

totk_live_spawn_cleanup() {
    if [[ -n "${totk_live_spawn_hotkey_pid:-}" ]] && \
       kill -0 "$totk_live_spawn_hotkey_pid" 2>/dev/null; then
        kill -TERM "$totk_live_spawn_hotkey_pid" 2>/dev/null || :
        wait "$totk_live_spawn_hotkey_pid" 2>/dev/null || :
    fi
}
