#!/usr/bin/env python3
"""Paired baseline/optimized emulator benchmark runs in an already-running R1 VM."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import shlex
import statistics
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

TOOL_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOL_DIR.parents[1]
VM_SCRIPT = TOOL_DIR / "vm.py"
WORK = Path(os.environ.get("R1_VM_WORK", REPO_ROOT / "build" / "r1-vm"))
BENCH_LINE = re.compile(r"frames\s+(\d+)\s+wall\s+([\d.]+)s.*?speed\s+([\d.]+)\s+fps", re.I)


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def json_object(raw: str, label: str) -> dict[str, Any]:
    for line in [raw.strip(), *reversed(raw.splitlines())]:
        try:
            value = json.loads(line)
        except (json.JSONDecodeError, TypeError):
            continue
        if isinstance(value, dict):
            return value
    raise ValueError(f"{label} did not return JSON object: {raw[-1000:]}")


def parse_bench(output: str, expected: int) -> dict[str, Any]:
    matches = list(BENCH_LINE.finditer(output))
    if not matches:
        raise ValueError("benchmark summary not found")
    m = matches[-1]
    frames, wall, fps = int(m[1]), float(m[2]), float(m[3])
    if frames != expected or min(wall, fps) <= 0:
        raise ValueError(f"invalid benchmark summary: {m[0]}")
    def find(pattern: str, cast: Any = str) -> Any:
        matches = list(re.finditer(pattern, output, re.I))
        return cast(matches[-1][1]) if matches else None
    checks = re.findall(r"output check:\s*audio\s+([\w-]+)\s+video\s+([\w-]+)", output, re.I)
    result = {
        "frames": frames, "wall_seconds": wall, "fps": fps,
        "cpu_seconds": find(r"\bcpu\s+([\d.]+)s", float),
        "avg_ms_per_frame": find(r"avg\s+([\d.]+)\s+ms/frame", float),
        "scale_avg_ms": find(r"scale\s+avg\s+([\d.]+)\s+ms", float),
        "audio_hash": checks[-1][0] if checks else None,
        "video_hash": checks[-1][1] if checks else None,
        "vm_hwm_kb": find(r"VmHWM\s+(\d+)\s*kB", int),
        "rendered_frames": find(r"rendered\s+(\d+)\s+of\s+\d+", int),
        "video_skipped": find(r"video skipped on\s+(\d+)", int),
        "audio_resampling_avg_ms": find(r"audio resampling avg\s+([\d.]+)\s+ms", float),
        "state_round_trip": find(r"state round trip\s+(ok|FAILED|unsupported)", str),
    }
    flags = {
        "missing_audio_hash": not bool(result["audio_hash"]),
        "missing_video_hash": not bool(result["video_hash"]),
        "missing_render_count": result["rendered_frames"] is None,
        "render_count_mismatch": result["rendered_frames"] is not None and result["rendered_frames"] != expected,
        "frames_skipped": bool(result["video_skipped"]),
        "state_round_trip_failed": result["state_round_trip"] == "FAILED",
        "state_round_trip_unsupported": result["state_round_trip"] == "unsupported",
        "missing_state_round_trip": result["state_round_trip"] is None,
    }
    result["correctness_flags"] = [key for key, value in flags.items() if value]
    return result


class VM:
    def invoke(self, *args: str, timeout: int = 210) -> str:
        result = subprocess.run([sys.executable, str(VM_SCRIPT), *args], capture_output=True,
                                text=True, timeout=timeout, check=False)
        if result.returncode:
            raise RuntimeError(f"vm.py {' '.join(args)} failed ({result.returncode}): {result.stderr[-1200:]} {result.stdout[-1200:]}")
        return result.stdout

    def status(self) -> dict[str, Any]:
        return json_object(self.invoke("status", "--json"), "vm.py status")

    def exec(self, command: str, timeout: int = 180, check: bool = True) -> dict[str, Any]:
        result = json_object(self.invoke("exec", "--json", "--timeout", str(timeout), command,
                                         timeout=timeout + 30), "vm.py exec")
        if check and result.get("exit_code") != 0:
            raise RuntimeError(f"guest command exited {result.get('exit_code')}: {result.get('output', '')[-1500:]}")
        return result


def guest_snapshot(vm: VM) -> dict[str, Any]:
    cmd = "; ".join([
        "cat /proc/meminfo | head -5", "cat /proc/self/status | grep '^VmHWM:'",
        "printf 'ASLR='; cat /proc/sys/kernel/randomize_va_space", "printf 'MOUNTS\\n'; cat /proc/mounts",
    ])
    status = vm.status()
    group = status.get("systemd", {}).get("ControlGroup")
    cpu_max = None
    if group:
        try:
            cpu_max = Path("/sys/fs/cgroup" + group + "/cpu.max").read_text().strip()
        except OSError:
            pass
    return {"vm_status": status, "guest": vm.exec(cmd, 15)["output"], "cgroup_cpu_max": cpu_max}


def validate_cases(raw: Any) -> list[dict[str, str]]:
    if not isinstance(raw, list) or not raw:
        raise ValueError("cases JSON must be a nonempty list")
    seen: set[str] = set()
    for case in raw:
        if not isinstance(case, dict) or not all(k in case for k in ("id", "core", "rom")):
            raise ValueError("each case requires id, core, and rom")
        for k in ("id", "core"):
            if not re.fullmatch(r"[A-Za-z0-9_.-]+", str(case[k])) or ".." in str(case[k]):
                raise ValueError(f"unsafe case {k}: {case[k]}")
        if case["id"] in seen:
            raise ValueError(f"duplicate case id: {case['id']}")
        seen.add(case["id"])
        if not str(case["rom"]).startswith("/"):
            raise ValueError(f"ROM must be an absolute guest path: {case['rom']}")
    return raw


def identity_checks(cases: list[dict[str, str]], manifest: dict[str, Any], vm: VM) -> dict[str, Any]:
    checks: dict[str, Any] = {"binaries": {}, "ancillary": {}, "roms": {}}
    binaries = manifest.get("binaries", {})
    seen_cores: set[str] = set()
    for case in cases:
        core = case["core"]
        if core not in seen_cores:
            for build, dirname in (("baseline", ".emulators-baseline"), ("optimized", ".emulators")):
                expected = binaries.get(f"{build}/{core}")
                guestpath = f"/usr/data/mnt/sd_0/{dirname}/{core}/{core}"
                if not expected:
                    raise ValueError(f"image-manifest.json missing binaries/{build}/{core}")
                got = vm.exec(f"sha256sum {shlex.quote(guestpath)}", 15)["output"].split()[0]
                checks["binaries"][f"{build}/{core}"] = {"manifest_sha256": expected, "guest_sha256": got, "match": got == expected}
                if got != expected:
                    raise ValueError(f"{build}/{core} guest binary hash does not match host image manifest")
                ancillary = {"emulator_json": f"/usr/data/mnt/sd_0/{dirname}/{core}/emulator.json"}
                if core == "prboom":
                    ancillary["wad"] = f"/usr/data/mnt/sd_0/{dirname}/{core}/prboom.wad"
                for name, path in ancillary.items():
                    digest = vm.exec(f"sha256sum {shlex.quote(path)}", 15)["output"].split()[0]
                    checks["ancillary"][f"{build}/{core}/{name}"] = {"guest_path": path, "guest_sha256": digest}
                if core == "prboom":
                    cfgdir = f"/usr/data/mnt/sd_0/{dirname}/{core}/games"
                    cfgs = vm.exec(f"find {shlex.quote(cfgdir)} -type f -name '*.cfg' -exec sha256sum {{}} +", 20)["output"]
                    checks["ancillary"][f"{build}/{core}/cfg_files"] = cfgs.splitlines()
            seen_cores.add(core)
        romhash = vm.exec(f"sha256sum {shlex.quote(case['rom'])}", 15)["output"].split()[0]
        checks["roms"][case["id"]] = {"guest_path": case["rom"], "guest_sha256": romhash}
    return checks


def atomic_save(path: Path, data: dict[str, Any]) -> None:
    temp = path.with_name(path.name + ".tmp")
    temp.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
    os.replace(temp, path)


def serial_offset() -> int:
    try:
        return (WORK / "serial.log").stat().st_size
    except OSError:
        return 0


def serial_since(offset: int) -> str:
    try:
        with (WORK / "serial.log").open("rb") as stream:
            stream.seek(offset)
            return stream.read().decode(errors="replace")
    except OSError:
        return ""


def summarize(runs: list[dict[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {"builds": {}, "paired_speedup_ratios": [], "median_speedup": None}
    numeric = ("fps", "wall_seconds", "cpu_seconds", "avg_ms_per_frame", "scale_avg_ms",
               "vm_hwm_kb", "audio_resampling_avg_ms", "rendered_frames", "video_skipped")
    for build in ("baseline", "optimized"):
        rows = [r for r in runs if r.get("build") == build and r.get("ok") and r.get("phase") == "trial"]
        measures = {key: [r["metrics"][key] for r in rows if r["metrics"].get(key) is not None] for key in numeric}
        flags = sorted({flag for r in rows for flag in r["metrics"].get("correctness_flags", [])})
        result["builds"][build] = {"n": len(rows), "measurements": {
            key: {"median": statistics.median(values), "range": [min(values), max(values)]} if values else None
            for key, values in measures.items()},
                                   "correctness_flags": flags}
    bytrial: dict[int, dict[str, float]] = {}
    for r in runs:
        if r.get("ok") and r.get("phase") == "trial":
            bytrial.setdefault(int(r["trial"]), {})[r["build"]] = r["metrics"]["fps"]
    ratios = [x["optimized"] / x["baseline"] for x in bytrial.values() if set(x) == {"baseline", "optimized"}]
    result["paired_speedup_ratios"] = ratios
    result["median_speedup"] = statistics.median(ratios) if ratios else None
    hashes = {}
    for build in ("baseline", "optimized"):
        rows = [r["metrics"] for r in runs if r.get("ok") and r.get("build") == build]
        hashes[build] = {key: sorted({m.get(key) for m in rows if m.get(key) is not None}) for key in ("audio_hash", "video_hash")}
    all_audio = {v for data in hashes.values() for v in data["audio_hash"]}
    all_video = {v for data in hashes.values() for v in data["video_hash"]}
    complete = bool(runs) and all(r.get("ok") and r["metrics"].get("audio_hash") and r["metrics"].get("video_hash")
                                  for r in runs)
    result["output_hashes"] = hashes
    result["av_hash_parity"] = ("post-timing check-window hashes match" if complete and len(all_audio) == len(all_video) == 1 and hashes["baseline"] == hashes["optimized"]
                                else "varies; semantic parity not established" if len(all_audio) > 1 or len(all_video) > 1
                                else "different or missing hashes")
    result["performance_qualification"] = ("review correctness flags; compared work may differ" if any(
        r.get("metrics", {}).get("correctness_flags") for r in runs if r.get("ok")) else "no parsed correctness flags")
    base = result["builds"]["baseline"]["measurements"]
    opt = result["builds"]["optimized"]["measurements"]
    for key in ("rendered_frames", "video_skipped"):
        if base[key] and opt[key] and base[key]["median"] != opt[key]["median"]:
            result["performance_qualification"] = f"{key} differs between builds; speed comparison uses unequal rendered work"
    return result


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cases", required=True, type=Path, help="JSON list of {id, core, rom, optional device_log}")
    p.add_argument("--frames", type=int, default=1800)
    p.add_argument("--trials", type=int, default=3)
    p.add_argument("--timeout", type=int, default=180, help="per guest command seconds")
    p.add_argument("--output", required=True, type=Path)
    a = p.parse_args()
    if min(a.frames, a.trials, a.timeout) < 1:
        p.error("frames, trials, and timeout must be positive")
    try:
        cases = validate_cases(json.loads(a.cases.read_text()))
        manifest_path = WORK / "image-manifest.json"
        manifest = json.loads(manifest_path.read_text())
        a.output.parent.mkdir(parents=True, exist_ok=True)
        rawdir = a.output.with_name(a.output.stem + "-raw")
        rawdir.mkdir(parents=True, exist_ok=True)
        vm = VM()
        status = vm.status()
        if not status.get("running"):
            raise RuntimeError("R1 VM is not running")
        data: dict[str, Any] = {"started_at": datetime.now(timezone.utc).isoformat(), "frames": a.frames,
            "trials": a.trials, "host": {"uname": platform.uname()._asdict(), "started_at": time.time()},
            "benchmark_options": {"frameskip": "off", "device_log": "physical source metadata only"},
            "output_hash_note": "audio/video hashes cover the post-timing check window, not the full frame stream",
            "environment_before": guest_snapshot(vm), "identity_checks": identity_checks(cases, manifest, vm), "cases": {}}
        atomic_save(a.output, data)
        group = status.get("systemd", {}).get("ControlGroup")
        try:
            data["host"]["cgroup_cpu_max"] = Path("/sys/fs/cgroup" + group + "/cpu.max").read_text().strip() if group else None
        except OSError:
            data["host"]["cgroup_cpu_max"] = None
        for case in cases:
            runs: list[dict[str, Any]] = []
            cdata = {"case": case, "runs": runs}
            data["cases"][case["id"]] = cdata
            failed_warmups: set[str] = set()
            def run(build: str, phase: str, trial: int | None = None) -> bool:
                dirname = ".emulators-baseline" if build == "baseline" else ".emulators"
                logpath = rawdir / f"{case['id']}-{build}-{phase}-{trial if trial is not None else 'warmup'}.log"
                cmd = f"ulimit -c 0; cd /usr/data/mnt/sd_0/{dirname}/{case['core']} && ./{case['core']} {shlex.quote(case['rom'])} --bench {a.frames} --frameskip off"
                started = time.monotonic()
                offset = serial_offset()
                row: dict[str, Any] = {"build": build, "phase": phase, "trial": trial,
                    "command": cmd, "host_started_at": datetime.now(timezone.utc).isoformat()}
                logpath.write_text("")
                try:
                    result = vm.exec(cmd, a.timeout, check=False)
                    output = str(result["output"])
                    logpath.write_text(output + "\n")
                    row["guest_exit_code"] = result.get("exit_code")
                    if result.get("exit_code") != 0:
                        raise RuntimeError(f"guest benchmark exit code {result.get('exit_code')}")
                    metrics = parse_bench(output, a.frames)
                    metrics["host_elapsed_seconds"] = time.monotonic() - started
                    row.update(ok=True, metrics=metrics, raw_output=str(logpath))
                except Exception as exc:
                    extra = serial_since(offset)
                    if extra:
                        with logpath.open("a") as stream:
                            stream.write("\n--- serial.log after command ---\n" + extra)
                    row.update(ok=False, error=str(exc), raw_output=str(logpath))
                    errorlog = logpath.with_suffix(".error.log")
                    errorlog.write_text(str(exc) + "\n")
                    row["error_log"] = str(errorlog)
                row["host_elapsed_seconds"] = time.monotonic() - started
                runs.append(row)
                cdata["summary"] = summarize(runs)
                atomic_save(a.output, data)
                print(f"{case['id']} {build} {phase} {trial or ''}: " +
                      (f"{row['metrics']['fps']:.2f} fps" if row.get("ok") else f"ERROR {row.get('error')}"), flush=True)
                # A failed run may have left the shell busy or killed the guest. Confirm recovery.
                try:
                    recovered = vm.status()
                    if not recovered.get("running"):
                        row["recovery_error"] = "VM stopped after benchmark failure"
                        atomic_save(a.output, data)
                        raise RuntimeError(row["recovery_error"])
                    vm.exec("echo R1_COMPARE_RECOVERED", 10)
                except Exception as exc:
                    row["recovery_error"] = str(exc)
                    atomic_save(a.output, data)
                    raise RuntimeError(f"guest console recovery failed after {case['id']}/{build}: {exc}") from exc
                return bool(row["ok"])
            for build in ("baseline", "optimized"):
                if not run(build, "warmup"):
                    failed_warmups.add(build)
            if len(failed_warmups) == 2:
                cdata["failed"] = "both warmups failed; repeated trials skipped"
                atomic_save(a.output, data); continue
            if failed_warmups:
                cdata["failed_warmups"] = sorted(failed_warmups)
            for trial in range(1, a.trials + 1):
                order = ("baseline", "optimized") if trial % 2 else ("optimized", "baseline")
                for build in order:
                    if build not in failed_warmups:
                        run(build, "trial", trial)
            cdata["summary"] = summarize(runs)
            atomic_save(a.output, data)
        data["environment_after"] = guest_snapshot(vm)
        before = data["environment_before"]
        after = data["environment_after"]
        aslr = lambda snapshot: re.search(r"(?:^|\n)ASLR=(\d+)(?:\n|$)", snapshot.get("guest", ""))
        before_aslr, after_aslr = aslr(before), aslr(after)
        before_status, after_status = before["vm_status"], after["vm_status"]
        checks = {
            "running": bool(after_status.get("running")),
            "ram_kib_unchanged": before_status.get("ram_kib") == after_status.get("ram_kib"),
            "cpu_quota_unchanged": before_status.get("cpu_quota_percent") == after_status.get("cpu_quota_percent"),
            "cgroup_cpu_max_unchanged": bool(before.get("cgroup_cpu_max") and after.get("cgroup_cpu_max") and
                                               before["cgroup_cpu_max"] == after["cgroup_cpu_max"]),
            "aslr_unchanged": bool(before_aslr and after_aslr and before_aslr[1] == after_aslr[1]),
        }
        data["environment_invariants"] = checks
        data["environment_invariants"]["all_passed"] = all(checks.values())
        data["finished_at"] = datetime.now(timezone.utc).isoformat()
        atomic_save(a.output, data)
        print(json.dumps({k: v.get("summary", v.get("failed")) for k, v in data["cases"].items()}, indent=2))
        if not checks["all_passed"]:
            print("environment invariants changed during benchmark", file=sys.stderr)
            return 1
        return 0
    except Exception as exc:
        print(f"compare.py: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
