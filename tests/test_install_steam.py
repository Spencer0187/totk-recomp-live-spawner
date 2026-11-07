"""Offline tests for the optional Steam launcher integration installer."""
import importlib.util
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

    def test_refuse_unknown_launcher(self):
        with self.assertRaisesRegex(ValueError,'Unexpected existing launcher'):
            installer.patch_launcher('#!/bin/sh\nexec totk\n',Path('/tmp/mod'))

    def test_refuse_partial_markers(self):
        with self.assertRaisesRegex(ValueError,'incomplete'):
            installer.patch_launcher(KNOWN_LAUNCHER+'\n'+installer.BEGIN+'\n',Path('/tmp/mod'))

if __name__=='__main__': unittest.main()
