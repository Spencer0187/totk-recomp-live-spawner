"""Offline tests for the optional Steam launcher integration installer."""
import importlib.util
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('totk_install_steam',ROOT/'install-steam.py')
installer = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(installer)

KNOWN_LAUNCHER = '''#!/usr/bin/env bash
set -euo pipefail
bridge_pid=''
game_pid=''
cleanup() {
  trap - EXIT INT TERM
  echo cleanup
}
export LD_PRELOAD="$adapter${LD_PRELOAD:+:$LD_PRELOAD}"
./totk "$@" &
game_pid=$!
wait "$game_pid"
'''

class SteamIntegrationTests(unittest.TestCase):
    def test_inserts_only_hook_lines_and_preserves_game_launcher(self):
        edited=installer.patch_launcher(KNOWN_LAUNCHER,Path('/tmp/totk-live-spawner'))
        self.assertIn('TOTK_LIVE_SPAWNER_ROOT=/tmp/totk-live-spawner',edited)
        self.assertIn('TOTK_LIVE_SPAWNER_ENABLED=0',edited)
        self.assertIn('--live-spawner) TOTK_LIVE_SPAWNER_ENABLED=1',edited)
        self.assertIn('set -- "${totk_live_spawner_pass_args[@]}"',edited)
        self.assertIn('source "$TOTK_LIVE_SPAWNER_ROOT/steam-hook.sh"',edited)
        self.assertIn('  totk_live_spawn_cleanup\n',edited)
        self.assertIn('totk_live_spawn_setup\nexport LD_PRELOAD',edited)
        self.assertIn('./totk "$@" &',edited)
        self.assertIn('  echo cleanup\n',edited)
        self.assertIn('totk_live_spawn_setup() { :; }',edited)
        self.assertIn('totk_live_spawn_cleanup() { :; }',edited)
        self.assertEqual(edited.count(installer.BEGIN),1)
        self.assertEqual(edited.count(installer.END),1)

    def test_idempotent(self):
        once=installer.patch_launcher(KNOWN_LAUNCHER,Path('/tmp/mod'))
        self.assertEqual(installer.patch_launcher(once,Path('/tmp/mod')),once)

    def test_upgrades_previously_always_enabled_integration(self):
        # Previous public commit added source/setup/cleanup with no CLI gate.
        old = KNOWN_LAUNCHER.replace("game_pid=''\n", "game_pid=''\n"
                + installer.BEGIN + "\n"
                + 'TOTK_LIVE_SPAWNER_ROOT=/tmp/old-install\n'
                + 'source "$TOTK_LIVE_SPAWNER_ROOT/steam-hook.sh"\n'
                + installer.END + "\n")
        old = old.replace('  trap - EXIT INT TERM\n',
                          '  trap - EXIT INT TERM\n  totk_live_spawn_cleanup\n')
        old = old.replace('export LD_PRELOAD=',
                          'totk_live_spawn_setup\nexport LD_PRELOAD=')
        updated = installer.patch_launcher(old, Path('/tmp/new-install'))
        self.assertIn('TOTK_LIVE_SPAWNER_ENABLED=0', updated)
        self.assertIn('TOTK_LIVE_SPAWNER_ROOT=/tmp/new-install', updated)
        self.assertEqual(updated.count('totk_live_spawn_cleanup\n'), 1)
        self.assertEqual(updated.count('totk_live_spawn_setup\n'), 1)
        self.assertEqual(updated.count(installer.BEGIN),1)
        self.assertEqual(installer.patch_launcher(updated,Path('/tmp/new-install')),updated)

    def test_cli_is_opt_in_and_preserves_non_mod_args(self):
        # Run just the injected shell block against placeholder hook functions.
        block=installer._integration_block(Path('/not/installed'))
        for args, expected in [
            ([], (0, [])),
            (['--seconds','4'], (0, ['--seconds','4'])),
            (['--live-spawner'], (1, [])),
            (['--profile','/tmp/path with spaces','--live-spawner','--seconds','4'],
                (1,['--profile','/tmp/path with spaces','--seconds','4'])),
        ]:
            cmd = ('set -euo pipefail\nset -- "$@"\n' + block +
                   "printf 'enabled=%s\\n' \"$TOTK_LIVE_SPAWNER_ENABLED\"\n" +
                   "for entry in \"$@\"; do printf 'arg=<%s>\\n' \"$entry\"; done\n")
            result=subprocess.run(['bash','-c',cmd,'bash',*args],capture_output=True,
                                  text=True,check=True)
            expected_result='enabled=%d\n' % expected[0]
            expected_result+=''.join('arg=<%s>\n' % val for val in expected[1])
            self.assertEqual(result.stdout,expected_result, (args,result.stderr))

    def test_refuse_unknown_launcher(self):
        with self.assertRaisesRegex(ValueError,'Unexpected existing launcher'):
            installer.patch_launcher('#!/bin/sh\nexec totk\n',Path('/tmp/mod'))

    def test_refuse_partial_markers(self):
        with self.assertRaisesRegex(ValueError,'incomplete'):
            installer.patch_launcher(KNOWN_LAUNCHER+'\n'+installer.BEGIN+'\n',Path('/tmp/mod'))

if __name__=='__main__': unittest.main()
