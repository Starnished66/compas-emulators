#!/usr/bin/env python3
"""Fit a QEMU CPU quota to the saved HiBy R1 headless benchmark profile."""

from __future__ import annotations

import argparse
import json
import math
import os
import shlex
import shutil
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


TOOL_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOL_DIR.parents[1]
PROFILE_PATH = TOOL_DIR / "profiles" / "r1.json"
VM_SCRIPT = TOOL_DIR / "vm.py"
RESULT_PATH = Path(os.environ.get("R1_VM_WORK", REPO_ROOT / "build" / "r1-vm")) / "calibration.json"
BENCH_LINE = re.compile(
    r"frames\s+(?P<frames>\d+)\s+wall\s+(?P<wall>[\d.]+)s.*?"
    r"speed\s+(?P<fps>[\d.]+)\s+fps",
    re.IGNORECASE,
)


def median(values: list[float]) -> float:
    if not values:
        raise ValueError("cannot take median of an empty measurement list")
    return float(statistics.median(values))


def quota_for_targets(
    target_fps: dict[str, float], measured_fps: dict[str, float]
) -> float:
    """Return robust median per-core quota estimate from an unthrottled run."""
    ratios = []
    for core, target in target_fps.items():
        measured = measured_fps.get(core)
        if target <= 0 or measured is None or measured <= 0:
            raise ValueError(f"missing or invalid rate for {core}")
        ratios.append(100.0 * target / measured)
    return min(100.0, max(5.0, median(ratios)))


def log_error_objective(
    target_fps: dict[str, float], measured_fps: dict[str, float]
) -> tuple[float, float]:
    """Return mean absolute log error and median log-rate error."""
    errors = []
    for core, target in target_fps.items():
        measured = measured_fps.get(core)
        if target <= 0 or measured is None or measured <= 0:
            raise ValueError(f"missing or invalid rate for {core}")
        errors.append(math.log(measured / target))
    return statistics.mean(abs(error) for error in errors), median(errors)


def parse_bench_output(output: str, expected_frames: int) -> tuple[float, float]:
    """Return (fps, wall seconds) from a core's normal --bench output."""
    matches = list(BENCH_LINE.finditer(output))
    if matches:
        match = matches[-1]
        frames = int(match.group("frames"))
        wall = float(match.group("wall"))
        fps = float(match.group("fps"))
        if frames <= 0 or wall <= 0 or fps <= 0:
            raise ValueError(f"invalid benchmark values in output: {match.group(0)}")
        if frames != expected_frames:
            raise ValueError(f"benchmark rendered {frames} frames, expected {expected_frames}")
        return fps, wall

    # Keep a fallback for compatible builds whose summary prints the wall rate
    # and frame count on separate lines.
    fps_matches = re.findall(r"speed\s+([\d.]+)\s+fps", output, re.IGNORECASE)
    wall_matches = re.findall(r"wall\s+([\d.]+)s", output, re.IGNORECASE)
    if fps_matches and wall_matches:
        fps, wall = float(fps_matches[-1]), float(wall_matches[-1])
        if fps > 0 and wall > 0:
            return fps, wall
    tail = output[-2000:]
    raise ValueError(f"could not parse --bench summary from VM output:\n{tail}")


def parse_json_object(raw: str, description: str) -> dict[str, Any]:
    """Accept a JSON object printed alone or as the final output line."""
    candidates = [raw.strip()]
    candidates.extend(line.strip() for line in reversed(raw.splitlines()))
    for candidate in candidates:
        if not candidate:
            continue
        try:
            value = json.loads(candidate)
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict):
            return value
    raise ValueError(f"{description} did not return a JSON object: {raw[-1000:]}")


def validate_quota(quota: float) -> float:
    if not math.isfinite(quota) or not 5.0 <= quota <= 100.0:
        raise ValueError(f"CPU quota must be between 5 and 100, got {quota}")
    return round(float(quota), 2)


class VM:
    def __init__(self, dry_run: bool = False) -> None:
        self.dry_run = dry_run
        self.commands: list[list[str]] = []

    def invoke(self, *args: str, timeout: int = 180) -> str:
        command = [sys.executable, str(VM_SCRIPT), *args]
        self.commands.append(command)
        if self.dry_run:
            print("DRY RUN:", json.dumps(command))
            return "{}"
        result = subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
        if result.returncode != 0:
            raise RuntimeError(
                f"VM command failed ({result.returncode}): {command!r}\n"
                f"stdout:\n{result.stdout[-2000:]}\nstderr:\n{result.stderr[-2000:]}"
            )
        return result.stdout

    def status(self) -> dict[str, Any]:
        return parse_json_object(self.invoke("status", "--json"), "vm.py status")

    def set_quota(self, quota: float) -> None:
        quota = validate_quota(quota)
        self.invoke("quota", f"{quota:g}")

    def bench(self, case: dict[str, Any], frames: int, timeout: int) -> dict[str, float]:
        core = case["core"]
        fixture = case["fixture"]
        command = (
            f"cd /usr/data/mnt/sd_0/.emulators-baseline/{core} && "
            f"./{core} {shlex.quote('/usr/data/mnt/sd_0/Games/' + fixture)} --bench {frames}"
        )
        host_started = time.monotonic()
        raw = self.invoke("exec", "--timeout", str(timeout), "--json", command, timeout=timeout + 30)
        result = parse_json_object(raw, "vm.py exec")
        if result.get("exit_code") != 0:
            raise RuntimeError(
                f"benchmark {core}/{fixture} exited {result.get('exit_code')}:\n"
                f"{str(result.get('output', ''))[-2000:]}"
            )
        fps, wall = parse_bench_output(str(result.get("output", "")), frames)
        return {"fps": fps, "wall_seconds": wall, "host_command_seconds": time.monotonic() - host_started}


def measure_all(
    vm: VM,
    cases: list[dict[str, Any]],
    frames: int,
    trials: int,
    timeout: int,
    warmup_frames: int,
) -> dict[str, dict[str, Any]]:
    result: dict[str, dict[str, Any]] = {}
    for case in cases:
        vm.bench(case, warmup_frames, timeout)
        runs = [vm.bench(case, frames, timeout) for _ in range(trials)]
        result[case["core"]] = {
            "runs": runs,
            "median_fps": median([run["fps"] for run in runs]),
            "median_wall_seconds": median([run["wall_seconds"] for run in runs]),
        }
        print(
            f"{case['core']}: {result[case['core']]['median_fps']:.2f} fps "
            f"({trials} trial(s), {frames} frames each)",
            flush=True,
        )
    return result


def rates(measurements: dict[str, dict[str, Any]]) -> dict[str, float]:
    return {core: item["median_fps"] for core, item in measurements.items()}


def residuals(
    target_fps: dict[str, float], measurements: dict[str, dict[str, Any]]
) -> dict[str, dict[str, float]]:
    result = {}
    for core, target in target_fps.items():
        measured = measurements[core]["median_fps"]
        result[core] = {
            "target_fps": target,
            "measured_fps": measured,
            "error_percent": (measured / target - 1.0) * 100.0,
            "log_error": math.log(measured / target),
        }
    return result


def self_test() -> None:
    assert parse_bench_output(
        "frames 1800  wall 4.04s  cpu 3.74s  speed 445.4 fps (746% of real time)", 1800
    ) == (445.4, 4.04)
    assert parse_json_object('{"output":"ok","exit_code":0}', "test")["exit_code"] == 0
    assert validate_quota(25) == 25.0
    try:
        validate_quota(4.9)
    except ValueError:
        pass
    else:
        raise AssertionError("out-of-range quota was accepted")
    targets = {"easy": 50.0, "hard": 100.0}
    measured = {"easy": 100.0, "hard": 200.0}
    assert quota_for_targets(targets, measured) == 50.0
    assert log_error_objective(targets, measured) == (math.log(2), math.log(2))
    print("calibration self-test passed")


def load_profile(path: Path) -> dict[str, Any]:
    profile = json.loads(path.read_text())
    cases = profile.get("reference_cases")
    if not isinstance(cases, list) or not cases:
        raise ValueError("profile must contain at least one reference case")
    seen = set()
    for case in cases:
        core, fixture = case["core"], case["fixture"]
        if core in seen:
            raise ValueError(f"profile has multiple cases for core {core}")
        seen.add(core)
        if not math.isfinite(float(case["reference_wall_seconds"])) or float(case["reference_wall_seconds"]) <= 0:
            raise ValueError(f"invalid reference wall time for {core}")
        if not re.fullmatch(r"[A-Za-z0-9_-]+", core) or ".." in fixture or fixture.startswith("/"):
            raise ValueError(f"unsafe core or fixture path in profile: {core}/{fixture}")
    return profile


def parse_quota_list(raw: str | None) -> list[float] | None:
    if raw is None:
        return None
    values = [validate_quota(float(item.strip())) for item in raw.split(",") if item.strip()]
    if not values:
        raise ValueError("--quotas requires comma-separated values")
    return sorted(set(values))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", type=Path, default=PROFILE_PATH)
    parser.add_argument("--trials", type=int, default=3)
    parser.add_argument("--frames", type=int, default=None)
    parser.add_argument("--warmup-frames", type=int, default=None)
    parser.add_argument("--quick", action="store_true", help="one 600-frame trial per case")
    parser.add_argument("--quotas", help="optional explicit comma-separated quota sweep")
    parser.add_argument("--refinements", type=int, default=2)
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--output", type=Path, default=RESULT_PATH)
    parser.add_argument("--dry-run", action="store_true", help="print VM commands without executing them")
    parser.add_argument("--self-test", action="store_true", help="check parser and quota-fit math without a VM")
    args = parser.parse_args()

    if args.self_test:
        self_test()
        return 0
    if args.trials < 1 or args.refinements < 0 or args.timeout < 1:
        parser.error("trials and timeout must be positive; refinements must not be negative")

    profile = load_profile(args.profile)
    profile_frames = int(profile["frames"])
    frames = args.frames if args.frames is not None else (600 if args.quick else profile_frames)
    trials = 1 if args.quick else args.trials
    warmup_frames = args.warmup_frames if args.warmup_frames is not None else frames
    if frames < 1 or warmup_frames < 1:
        parser.error("frames and warmup-frames must be positive")
    explicit_quotas = parse_quota_list(args.quotas)
    cases = profile["reference_cases"]
    target_fps = {
        case["core"]: profile_frames / float(case["reference_wall_seconds"]) for case in cases
    }

    if args.dry_run:
        print(json.dumps({
            "profile": profile["id"],
            "warmup_frames": warmup_frames,
            "trials": trials,
            "frames": frames,
            "target_fps": target_fps,
            "commands": [
                f"cd /usr/data/mnt/sd_0/.emulators-baseline/{case['core']} && "
                f"./{case['core']} /usr/data/mnt/sd_0/Games/{case['fixture']} --bench {frames}"
                for case in cases
            ],
        }, indent=2))
    if args.dry_run:
        return 0

    vm = VM()
    original_status = vm.status()
    original_quota = float(original_status.get("cpu_quota_percent", 100.0))
    initial_status = original_status
    success = False
    started_at = time.time()
    try:
        vm.set_quota(100)
        print(f"Warmup before each core/quota measurement: {warmup_frames} frames", flush=True)
        print(f"Baseline: {len(cases)} cores at 100% quota", flush=True)
        baseline = measure_all(vm, cases, frames, trials, args.timeout, warmup_frames)
        baseline_rates = rates(baseline)
        initial_estimate = quota_for_targets(target_fps, baseline_rates)
        print(f"Ratio-based initial quota estimate: {initial_estimate:.2f}%", flush=True)

        candidates: dict[float, dict[str, dict[str, Any]]] = {100.0: baseline}
        if explicit_quotas is not None:
            test_quotas = explicit_quotas
        else:
            test_quotas = [initial_estimate]

        for quota in test_quotas:
            quota = validate_quota(quota)
            if quota in candidates:
                continue
            vm.set_quota(quota)
            print(f"Measure candidate quota {quota:g}%", flush=True)
            candidates[quota] = measure_all(vm, cases, frames, trials, args.timeout, warmup_frames)

        if explicit_quotas is None:
            for _ in range(args.refinements):
                current_quota, current_measurements = min(
                    candidates.items(),
                    key=lambda item: log_error_objective(target_fps, rates(item[1]))[0],
                )
                _, median_log_error = log_error_objective(target_fps, rates(current_measurements))
                next_quota = validate_quota(
                    min(100.0, max(5.0, current_quota * math.exp(-median_log_error)))
                )
                if next_quota in candidates or abs(next_quota - current_quota) < 0.25:
                    break
                vm.set_quota(next_quota)
                print(f"Refine quota to {next_quota:g}%", flush=True)
                candidates[next_quota] = measure_all(vm, cases, frames, trials, args.timeout, warmup_frames)

        selected_quota, selected_measurements = min(
            candidates.items(),
            key=lambda item: log_error_objective(target_fps, rates(item[1]))[0],
        )
        vm.set_quota(selected_quota)
        initial_status = vm.status()
        selected_rates = rates(selected_measurements)
        objective, median_log_error = log_error_objective(target_fps, selected_rates)
        output = {
            "schema": 1,
            "identity": initial_status.get("identity"),
            "profile_id": profile["id"],
            "profile_label": profile["label"],
            "quality": profile["quality"],
            "limitations": profile["limitations"],
            "created_unix_time": time.time(),
            "duration_seconds": time.time() - started_at,
            "trial_count": trials,
            "frames_per_trial": frames,
            "warmup_frames": warmup_frames,
            "selected_quota_percent": selected_quota,
            "objective_mean_absolute_log_error": objective,
            "median_log_error": median_log_error,
            "vm_status": initial_status,
            "cases": {
                case["core"]: {
                    "fixture": case["fixture"],
                    "reference_wall_seconds_1800_frames": case["reference_wall_seconds"],
                    "target_fps": target_fps[case["core"]],
                    "baseline_100_percent": baseline[case["core"]],
                    "selected_quota": selected_measurements[case["core"]],
                    "residual": residuals(target_fps, selected_measurements)[case["core"]],
                }
                for case in cases
            },
            "candidate_objectives": {
                str(quota): log_error_objective(target_fps, rates(measurements))[0]
                for quota, measurements in candidates.items()
            },
        }
        args.output.parent.mkdir(parents=True, exist_ok=True)
        if args.output.exists():
            shutil.copy2(args.output, args.output.with_name(args.output.stem + "-" + str(time.time_ns()) + args.output.suffix))
        temporary_output = args.output.with_suffix(args.output.suffix + ".part")
        temporary_output.write_text(json.dumps(output, indent=2) + "\n")
        temporary_output.replace(args.output)
        success = True
        print(f"Selected {selected_quota:g}% quota; wrote {args.output}")
        for core, item in output["cases"].items():
            err = item["residual"]["error_percent"]
            print(f"  {core}: {item['residual']['measured_fps']:.2f} vs {item['target_fps']:.2f} fps ({err:+.1f}%)")
        return 0
    finally:
        if not success:
            try:
                vm.set_quota(original_quota)
                print(f"Calibration did not finish; restored original quota {original_quota:g}%", file=sys.stderr)
            except Exception as restore_error:
                print(f"Failed to restore original quota: {restore_error}", file=sys.stderr)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("Calibration interrupted", file=sys.stderr)
        raise SystemExit(130)
