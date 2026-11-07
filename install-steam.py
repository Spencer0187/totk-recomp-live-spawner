#!/usr/bin/env python3
"""Integrate the spawner into an existing Steam-registered TotK launcher.

This modifies only the launcher shell script. It does not touch Steam's
shortcuts.vdf, controller layouts, original game executable or user saves.

By default it patches ~/Applications/TotK-Recomp/launch.sh, which the user's
non-Steam game entry already launches. Re-running is idempotent. A 2KB
pre-install copy is stored beside the launcher for optional manual rollback.
"""
from __future__ import annotations
import argparse
import os
import shlex
import stat
import subprocess
import tempfile
from pathlib import Path

PROJECT = Path(__file__).resolve().parent
BEGIN = '# BEGIN TOTK_LIVE_SPAWNER_INTEGRATION'
END = '# END TOTK_LIVE_SPAWNER_INTEGRATION'


def patch_launcher(source: str, mod_root: Path) -> str:
    if BEGIN in source:
        if END not in source:
            raise ValueError('Existing integration marker is incomplete; refusing to modify launcher')
        return source
    # Reject unexpected launcher versions rather than accidentally breaking
    # the user's existing Steam Controller, DSU, gyro or snapback setup.
    anchors = (
        "game_pid=''\n",
        'cleanup() {\n  trap - EXIT INT TERM\n',
        'export LD_PRELOAD="$adapter${LD_PRELOAD:+:$LD_PRELOAD}"\n',
    )
    for anchor in anchors:
        if source.count(anchor) != 1:
            raise ValueError(f'Unexpected existing launcher structure: {anchor!r}')
    quoted = shlex.quote(str(mod_root))
    injection = (f'{BEGIN}\n'
                 f'TOTK_LIVE_SPAWNER_ROOT={quoted}\n'
                 'if [[ -f "$TOTK_LIVE_SPAWNER_ROOT/steam-hook.sh" ]]; then\n'
                 '  source "$TOTK_LIVE_SPAWNER_ROOT/steam-hook.sh"\n'
                 'else\n'
                 '  # The mod was uninstalled; preserve the original Steam launcher.\n'
                 '  totk_live_spawn_setup() { :; }\n'
                 '  totk_live_spawn_cleanup() { :; }\n'
                 'fi\n'
                 f'{END}\n')
    source = source.replace("game_pid=''\n", "game_pid=''\n" + injection)
    source = source.replace('cleanup() {\n  trap - EXIT INT TERM\n',
                            'cleanup() {\n  trap - EXIT INT TERM\n'
                            '  totk_live_spawn_cleanup\n')
    source = source.replace(
        'export LD_PRELOAD="$adapter${LD_PRELOAD:+:$LD_PRELOAD}"\n',
        'totk_live_spawn_setup\n'
        'export LD_PRELOAD="$adapter${LD_PRELOAD:+:$LD_PRELOAD}"\n')
    return source


def main() -> None:
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument('--launcher', type=Path,
                     default=Path.home() / 'Applications/TotK-Recomp/launch.sh')
    cli.add_argument('--dry-run', action='store_true')
    args = cli.parse_args()
    launcher = args.launcher.resolve()
    before = launcher.read_bytes()
    after = patch_launcher(before.decode('utf-8'), PROJECT).encode('utf-8')
    if before == after:
        print(f'Already integrated: {launcher}')
        return
    with tempfile.NamedTemporaryFile(dir=launcher.parent,
                                     prefix='.totk-steam-launch-',delete=False) as file:
        file.write(after)
        file.flush()
        os.fsync(file.fileno())
        candidate = Path(file.name)
    try:
        candidate.chmod(stat.S_IMODE(launcher.stat().st_mode))
        subprocess.run(['bash','-n',str(candidate)],check=True)
        if args.dry_run:
            print(f'Dry-run: would integrate {launcher}; bash syntax valid')
            return
        rollback = launcher.with_name(launcher.name + '.before-live-spawner')
        if not rollback.exists():
            with rollback.open('xb') as file:
                file.write(before)
        if launcher.read_bytes() != before:
            raise RuntimeError('Launcher modified concurrently, refusing overwrite')
        os.replace(candidate,launcher)
        print(f'Integrated TotK spawner into Steam launcher: {launcher}')
        print(f'Pre-install launcher backup: {rollback}')
        print('Use Steam > TotK Recomp (Experimental) > Play; do not start a separate mod launcher.')
    finally:
        candidate.unlink(missing_ok=True)

if __name__ == '__main__':
    main()
