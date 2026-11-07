#!/usr/bin/env python3
"""Send live actor requests to the experimental TotK Recomp spawner."""
from __future__ import annotations

import argparse
import math
import os
import socket
import sys
import time
from pathlib import Path

# J shortcut: the four parts of the flying build and their original UltraCam
# test-spawner scales. Change these literals to customize your build.
FOUR_ACTORS: tuple[tuple[str, float], ...] = (
    ("SpObj_LiftGeneratorWing_A_01_MiniGame", 1.0),
    ("AsbObj_WagonSteeringCollectObject_A_01", 0.15),
    ("SpObj_ControlStick_A_01", 1.0),
    ("SpObj_SpringPiston_A_01", 0.4),
)

# Layout relative to Link's current forward/right orientation, not world axes.
# First part: 3 units forward and 1 unit up. Each subsequent part moves
# another 2 units forward and 2 units sideways (diagonal for wing clearance).
FOUR_FORWARD = 3.0
FOUR_UP = 1.0
FOUR_SEPARATION = 2.0


def command(message: str) -> str:
    """Submit one newline-terminated command to this user's running game."""
    directory = os.environ.get("XDG_RUNTIME_DIR", "/tmp")
    target = Path(directory) / f"totk-live-spawn-{os.getuid()}.sock"
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(5)
        client.connect(str(target))
        client.sendall((message + "\n").encode("ascii"))
        result = client.recv(4096).decode(errors="replace").strip()
    return result


def parse_pose(value: str) -> tuple[float, ...]:
    items = value.split()
    if len(items) != 8 or items[0] != "POSE":
        raise ValueError(f"Cannot read Link's current pose: {value}")
    values = tuple(float(item) for item in items[1:])
    if not all(math.isfinite(v) for v in values):
        raise ValueError("Link's position/direction is not finite")
    fx, fz, rx, rz = values[3:]
    if not math.isclose(math.hypot(fx, fz), 1, abs_tol=.01):
        raise ValueError("Invalid Link forward vector")
    if not math.isclose(math.hypot(rx, rz), 1, abs_tol=.01):
        raise ValueError("Invalid Link right vector")
    return values


def four_layout(pose: tuple[float, ...], *,
                forward: float = FOUR_FORWARD,
                up: float = FOUR_UP,
                separation: float = FOUR_SEPARATION
                ) -> tuple[tuple[str, float, tuple[float, float, float]], ...]:
    """Return actor, scale, and world-axis displacement from Link.

    Each adjacent pair differs by `separation` along the horizontal right
    direction and `separation` along forward; the 3D distance between centers
    is therefore sqrt(2)*separation. No actor is glued to another actor.
    """
    _, _, _, fx, fz, rx, rz = pose
    num = len(FOUR_ACTORS)
    result = []
    for i, (actor, scale) in enumerate(FOUR_ACTORS):
        depth = forward + separation * i
        side = (i - (num - 1) / 2) * separation
        offset = (fx * depth + rx * side, up, fz * depth + rz * side)
        result.append((actor, scale, offset))
    return tuple(result)


def spawn_four(*, verbose: bool = True) -> bool:
    pose = parse_pose(command("pose"))
    for index, (actor, scale, offset) in enumerate(four_layout(pose), 1):
        x, y, z = offset
        message = f"spawn {actor} {scale:.8g} {x:.8g} {y:.8g} {z:.8g}"
        result = command(message)
        if not result.startswith("QUEUED"):
            raise RuntimeError(f"Actor {index}/4 ({actor}) rejected: {result}")
        # The SVC dispatcher runs on the actor creation thread. Complete one
        # queue submission before sending the next so they never overwrite the
        # shared pre-dispatch staging request. Every submitted job has its own
        # long-lived game request backing memory.
        deadline = time.monotonic() + 6
        while time.monotonic() < deadline:
            time.sleep(.035)
            status = command("status")
            if "pending=0" in status:
                break
        else:
            raise TimeoutError(f"Actor {index}/4 ({actor}) did not dispatch")
        if verbose:
            print(f"{index}/4 {actor}: scale={scale:g} world offset=({x:.2f}, {y:.2f}, {z:.2f})")
        time.sleep(.13)
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("actor", nargs="?", help="TotK actor identifier")
    parser.add_argument("--scale", type=float, default=1.0)
    parser.add_argument("--offset", nargs=3, type=float, metavar=("X", "Y", "Z"),
                        default=(0, 2, 2), help="relative to Link in world coordinates")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--status", action="store_true")
    mode.add_argument("--cancel", action="store_true")
    mode.add_argument("--four", action="store_true", help="spawn the four-part J build ahead of Link")
    args = parser.parse_args()
    if args.actor and (args.status or args.cancel or args.four):
        parser.error("actor name is not accepted with --status, --cancel or --four")
    if not any((args.actor, args.status, args.cancel, args.four)):
        parser.error("specify an actor or --status, --cancel, --four")
    if args.actor and (not math.isfinite(args.scale) or any(not math.isfinite(v) for v in args.offset)):
        parser.error("scale and offsets must be finite")
    try:
        if args.four:
            spawn_four()
            print("Submitted all four actor-creation requests.")
            return 0
        message = ("status" if args.status else "cancel" if args.cancel else
                   f"spawn {args.actor} {args.scale:g} " + " ".join(f"{v:g}" for v in args.offset))
        result = command(message)
    except (OSError, TimeoutError, ValueError, RuntimeError) as error:
        print(f"TotK live spawner: {error}", file=sys.stderr)
        return 2
    print(result)
    return 0 if result.startswith(("READY", "QUEUED", "CANCELLED")) else 1


if __name__ == "__main__":
    raise SystemExit(main())
