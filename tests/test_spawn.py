"""Offline CLI and layout tests; no game or X11 session required."""
import importlib.util
import math
from pathlib import Path
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("totk_spawn_client", ROOT / "spawn.py")
spawn = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(spawn)


class LayoutTests(unittest.TestCase):
    def test_four_exact_actor_ids_and_scales(self):
        self.assertEqual(spawn.FOUR_ACTORS, (
            ("SpObj_LiftGeneratorWing_A_01_MiniGame", 1.0),
            ("AsbObj_WagonSteeringCollectObject_A_01", .15),
            ("SpObj_ControlStick_A_01", 1.0),
            ("SpObj_SpringPiston_A_01", .4),
        ))

    def test_aligned_with_links_facing(self):
        # World +Z forward, world +X right. Distances reflect the raw config.
        pose = (100, 5, -200, 0, 1, 1, 0)
        entries = spawn.four_layout(pose)
        self.assertEqual(len(entries), 4)
        self.assertEqual([e[2] for e in entries], [
            (-3.0, 1.0, 3.0),
            (-1.0, 1.0, 5.0),
            (1.0, 1.0, 7.0),
            (3.0, 1.0, 9.0),
        ])

    def test_rotates_with_link_and_keeps_two_unit_axis_offsets(self):
        pose = (-644, -412, 1507, -.8, -.6, -.6, .8)
        entries = spawn.four_layout(pose)
        self.assertEqual(entries[0][2][1], 1.0)
        for previous, later in zip(entries, entries[1:]):
            dx = later[2][0] - previous[2][0]
            dz = later[2][2] - previous[2][2]
            self.assertAlmostEqual(math.hypot(dx, dz), math.sqrt(8))
            self.assertAlmostEqual(dx * pose[3] + dz * pose[4], 2.0)
            self.assertAlmostEqual(dx * pose[5] + dz * pose[6], 2.0)
            self.assertEqual(later[2][1], previous[2][1])

    def test_pose_validation_rejects_missing_scene(self):
        for invalid in ('ERR player pose unavailable; load a save first', 'POSE 1 2 3',
                        'POSE 1 2 3 nan 0 0 1', 'POSE 1 2 3 0 0 1 0'):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                spawn.parse_pose(invalid)

    def test_batch_serializes_and_checks_each_completion(self):
        commands = []
        def fake_command(message):
            commands.append(message)
            if message == 'pose': return 'POSE 100 5 -200 0 1 1 0'
            if message == 'status': return 'READY pending=0 mode=svc'
            if message.startswith('spawn '): return 'QUEUED'
            raise AssertionError(message)
        with patch.object(spawn, 'command', side_effect=fake_command), patch.object(spawn.time, 'sleep'):
            self.assertTrue(spawn.spawn_four(verbose=False))
        actual = [s for s in commands if s.startswith('spawn ')]
        self.assertEqual(len(actual), 4)
        self.assertEqual(len([s for s in commands if s == 'status']), 4)
        for expected, text in zip(spawn.four_layout((100, 5, -200, 0, 1, 1, 0)), actual):
            actor, scale, xyz = expected
            tokens = text.split()
            self.assertEqual(tokens[1], actor)
            self.assertAlmostEqual(float(tokens[2]), scale)
            self.assertEqual(tuple(map(float, tokens[3:])), xyz)

    def test_batch_error_is_not_reported_as_success(self):
        def fake_command(message):
            if message == 'pose': return 'POSE 100 5 -200 0 1 1 0'
            return 'ERR another actor request is pending'
        with patch.object(spawn, 'command', side_effect=fake_command):
            with self.assertRaises(RuntimeError):
                spawn.spawn_four(verbose=False)


if __name__ == '__main__':
    unittest.main()
