#!/usr/bin/env python3
"""Run a scripted SML2 multiplayer scenario twice and compare frame reports.

This is a deterministic regression harness, not a claim that every gameplay
rule is correct. Use a legally obtained ROM on the local machine; never commit
commercial ROMs to the repository.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any


def read_script(path: Path) -> list[tuple[int, int, int]]:
    events: list[tuple[int, int, int]] = []
    previous = -1
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        fields = line.split()
        if len(fields) != 3:
            raise ValueError(f"{path}:{number}: expected frame P1-mask P2-mask")
        frame = int(fields[0], 10)
        p1, p2 = (int(value, 16) for value in fields[1:])
        if frame < 0 or frame < previous:
            raise ValueError(f"{path}:{number}: frame values must be nonnegative and ordered")
        if not 0 <= p1 <= 0xFF or not 0 <= p2 <= 0xFF:
            raise ValueError(f"{path}:{number}: masks must fit in one byte")
        events.append((frame, p1, p2))
        previous = frame
    return events


def expected_inputs(events: list[tuple[int, int, int]], frames: int) -> list[tuple[int, int]]:
    p1 = p2 = 0
    index = 0
    expected: list[tuple[int, int]] = []
    for frame in range(frames):
        while index < len(events) and events[index][0] <= frame:
            _, p1, p2 = events[index]
            index += 1
        expected.append((p1, p2))
    return expected


def read_report(path: Path, frames: int, inputs: list[tuple[int, int]]) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        try:
            record = json.loads(line)
        except json.JSONDecodeError as exc:
            raise ValueError(f"{path}:{line_number}: malformed JSONL: {exc}") from exc
        records.append(record)

    if len(records) != frames:
        raise ValueError(f"{path}: expected {frames} frame records, got {len(records)}")
    required = {
        "schema", "frame", "p1_input", "p2_input", "p1_world_x", "p1_world_y",
        "p2_spawned", "p2_world_x", "p2_world_y", "bg_map_hash",
        "level_ram_hash", "actor_region_hash", "render_hash",
    }
    for index, record in enumerate(records):
        missing = required.difference(record)
        if missing:
            raise ValueError(f"{path}: frame {index} is missing fields: {sorted(missing)}")
        if record["schema"] != 1 or record["frame"] != index:
            raise ValueError(f"{path}: unexpected schema/frame at record {index}")
        if (record["p1_input"], record["p2_input"]) != inputs[index]:
            raise ValueError(f"{path}: scripted inputs didn't match at frame {index}")
        if not 0 <= int(record.get("p2_sprite_count", 0)) <= 40:
            raise ValueError(f"{path}: invalid player-two sprite count at frame {index}")
    return records


def run_once(binary: Path, rom: Path, script: Path, frames: int, report: Path,
             timeout: int) -> list[dict[str, Any]]:
    command = [
        str(binary), "--no-crc-check", "--headless",
        "--mp-script", str(script), "--mp-report", str(report),
        "--frames", str(frames), str(rom),
    ]
    env = os.environ.copy()
    env.setdefault("SDL_VIDEODRIVER", "dummy")
    env.setdefault("SDL_AUDIODRIVER", "dummy")
    try:
        result = subprocess.run(command, capture_output=True, text=True,
                                timeout=timeout, env=env, check=False)
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(f"test runner timed out after {timeout}s: {' '.join(command)}") from exc
    if result.returncode:
        raise RuntimeError(
            f"test runner failed ({result.returncode}): {' '.join(command)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    if result.stderr.strip():
        print(result.stderr.strip(), file=sys.stderr)
    return read_report(report, frames, expected_inputs(read_script(script), frames))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path, help="built PipeClean executable")
    parser.add_argument("--rom", required=True, type=Path, help="local SML2 ROM (not copied by this tool)")
    parser.add_argument("--script", required=True, type=Path, help="ordered frame/P1/P2 input events")
    parser.add_argument("--frames", required=True, type=int, help="number of logical two-player frames")
    parser.add_argument("--report-dir", required=True, type=Path, help="directory for JSONL frame reports")
    parser.add_argument("--timeout", type=int, default=90, help="timeout for each replay, in seconds")
    parser.add_argument("--require-p2-spawn", action="store_true",
                        help="fail unless Player 2 reaches the game's spawned state")
    parser.add_argument("--require-p1-movement", action="store_true",
                        help="fail unless Player 1's world coordinates change")
    parser.add_argument("--require-p2-movement", action="store_true",
                        help="fail unless spawned Player 2's world coordinates change")
    parser.add_argument("--require-camera-scroll", action="store_true",
                        help="fail unless the camera position changes")
    parser.add_argument("--require-block-patch", action="store_true",
                        help="fail unless the runtime records at least one block/tile patch")
    parser.add_argument("--require-actor-region-change", action="store_true",
                        help="fail unless the actor-related RAM hash changes")
    args = parser.parse_args()

    for path, name in ((args.binary, "binary"), (args.rom, "ROM"), (args.script, "script")):
        if not path.exists():
            parser.error(f"{name} does not exist: {path}")
    if args.frames <= 0:
        parser.error("--frames must be greater than zero")
    if args.timeout <= 0:
        parser.error("--timeout must be greater than zero")

    binary = args.binary.resolve()
    rom = args.rom.resolve()
    script = args.script.resolve()
    report_dir = args.report_dir.resolve()
    report_dir.mkdir(parents=True, exist_ok=True)
    report_a = report_dir / "replay-a.jsonl"
    report_b = report_dir / "replay-b.jsonl"

    try:
        first = run_once(binary, rom, script, args.frames, report_a, args.timeout)
        second = run_once(binary, rom, script, args.frames, report_b, args.timeout)
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"MULTIPLAYER TEST: FAIL\n{exc}", file=sys.stderr)
        return 1

    for index, (left, right) in enumerate(zip(first, second)):
        if left != right:
            keys = sorted(key for key in set(left) | set(right) if left.get(key) != right.get(key))
            print(
                f"MULTIPLAYER TEST: NONDETERMINISTIC at frame {index}; "
                f"differing fields: {', '.join(keys)}\n"
                f"replay A: {report_a}\nreplay B: {report_b}",
                file=sys.stderr,
            )
            return 1

    spawned = sum(1 for record in first if record["p2_spawned"])
    checks = [
        (args.require_p2_spawn, spawned > 0, "Player 2 never spawned"),
        (args.require_p1_movement,
         len({(row["p1_world_x"], row["p1_world_y"]) for row in first}) > 1,
         "Player 1's world position never changed"),
        (args.require_p2_movement,
         len({(row["p2_world_x"], row["p2_world_y"]) for row in first if row["p2_spawned"]}) > 1,
         "Player 2's world position never changed while spawned"),
        (args.require_camera_scroll,
         len({(row["camera_x"], row["camera_y"]) for row in first}) > 1,
         "The camera never scrolled"),
        (args.require_block_patch,
         any(int(row.get("tile_patch_count", 0)) > 0 for row in first),
         "No block/tile patch was recorded"),
        (args.require_actor_region_change,
         len({row["actor_region_hash"] for row in first}) > 1,
         "The actor-related RAM hash never changed"),
    ]
    failed = [message for enabled, passed, message in checks if enabled and not passed]
    if failed:
        print(
            "MULTIPLAYER TEST: FAIL — " + "; ".join(failed) + "\\n"
            f"Reports: {report_a} and {report_b}",
            file=sys.stderr,
        )
        return 1

    print(
        f"MULTIPLAYER TEST: PASS — {args.frames} scripted two-player frames replayed "
        f"deterministically; Player 2 marked spawned in {spawned} frames.\\n"
        f"Reports: {report_a} and {report_b}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
