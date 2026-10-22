#!/usr/bin/env python3
"""Aggregate saved R1 VM comparison JSON without running a VM or device."""
from __future__ import annotations

import argparse
import csv
import json
import re
import statistics
from pathlib import Path
from typing import Any

DEVICE_ROOT = Path(__file__).resolve().parents[2].parent / "compas-player" / "build" / "emulator-performance"
PHYSICAL_HEADLESS_LOGS = {
    "quicknes-NES-smb.nes.log", "quicknes-NES-croom.nes.log", "quicknes-NES-thwaite.nes.log",
    "gpsp-GBA-240pee_mb.gba.log", "gearsystem-MasterSystem-test.sms.log",
    "gearsystem-GameGear-test.gg.log", "gearsystem-SG-1000-test.sg.log",
    "gearboy-GB-gb240p.gb.log", "wswan-WonderSwan-test.ws.log",
    "wswan-WonderSwanColor-test.wsc.log", "pce-PCEngine-test.pce.log",
    "prboom-freedoom1.log",
}
BENCH = re.compile(r"frames\s+(\d+)\s+wall\s+([\d.]+)s\s+cpu\s+([\d.]+)s\s+speed\s+([\d.]+)\s+fps", re.I)
HASHES = re.compile(r"output check:\s*audio\s+([\w-]+)\s+video\s+([\w-]+)", re.I)
STATE = re.compile(r"state round trip\s+(ok|FAILED|unsupported)", re.I)


def median_range(values: list[float]) -> dict[str, float] | None:
    if not values:
        return None
    return {"median": statistics.median(values), "range": [min(values), max(values)]}


def resolve_device_log(raw: str | None) -> tuple[Path | None, str | None]:
    if not raw:
        return None, "case.device_log is absent"
    name = Path(raw).name
    if name not in PHYSICAL_HEADLESS_LOGS:
        return None, "device_log filename is not on the physical headless allowlist"
    candidate = DEVICE_ROOT / name
    if not candidate.is_file():
        return None, "allowlisted physical device log is missing from canonical root"
    return candidate.resolve(), None


def parse_device_log(raw_path: str | None) -> dict[str, Any]:
    resolved, reason = resolve_device_log(raw_path)
    if resolved is None:
        return {"status": "device headless not comparable", "reason": reason, "source_reference": raw_path}
    content = resolved.read_text(errors="replace")
    matches = list(BENCH.finditer(content))
    if not matches:
        return {"status": "device headless not comparable", "reason": "headless benchmark summary not found; paced live logs are not substituted", "source_path": str(resolved)}
    m = matches[-1]
    hashes = list(HASHES.finditer(content))
    state = list(STATE.finditer(content))
    rendered = re.findall(r"rendered\s+(\d+)\s+of\s+(\d+)\s+frames\s*\(video skipped on\s+(\d+)\)", content, re.I)
    hwm = re.findall(r"VmHWM\s+(\d+)\s*kB", content, re.I)
    frames, wall, cpu, fps = int(m[1]), float(m[2]), float(m[3]), float(m[4])
    flags: list[str] = []
    if not hashes:
        flags.append("missing_audio_video_hash")
    if not rendered:
        flags.append("missing_render_count")
    elif int(rendered[-1][0]) != frames:
        flags.append("rendered_frame_count_differs_from_bench_frames")
    if rendered and int(rendered[-1][2]) != 0:
        flags.append("video_frames_skipped")
    if not state:
        flags.append("missing_state_round_trip")
    elif state[-1][1].lower() != "ok":
        flags.append("state_round_trip_" + state[-1][1].lower())
    return {
        "status": "parsed physical headless log", "source_path": str(resolved),
        "frames": frames, "wall_seconds": wall, "cpu_seconds": cpu, "fps": fps,
        "post_timing_hashes": {"audio": hashes[-1][1], "video": hashes[-1][2]} if hashes else None,
        "vm_hwm_kb": int(hwm[-1]) if hwm else None,
        "rendered_frames": int(rendered[-1][0]) if rendered else None,
        "video_skipped": int(rendered[-1][2]) if rendered else None,
        "state_round_trip": state[-1][1].lower() if state else None,
        "state_flags": flags, "original_rom_sha256": None,
        "original_rom_sha256_note": "historical source run lacks original ROM SHA-256; matching post-timing hashes do not prove ROM identity",
    }


def run_valid(run: dict[str, Any], expected: int) -> bool:
    m = run.get("metrics", {})
    if not isinstance(m, dict):
        return False
    hashes_ok = bool(m.get("audio_hash") and m.get("video_hash"))
    return bool(run.get("ok") and m.get("frames") == expected and
                m.get("rendered_frames") == expected and m.get("video_skipped") == 0 and hashes_ok)


def run_metrics(runs: list[dict[str, Any]], build: str, expected: int) -> dict[str, Any]:
    trials = [r for r in runs if r.get("build") == build and r.get("phase") == "trial"]
    valid = [r for r in trials if run_valid(r, expected)]
    keys = ("fps", "wall_seconds", "cpu_seconds", "vm_hwm_kb", "rendered_frames", "video_skipped")
    result: dict[str, Any] = {"trial_count": len(trials), "valid_trial_count": len(valid), "measurements": {}}
    for key in keys:
        vals = [float(r["metrics"][key]) for r in valid if isinstance(r["metrics"].get(key), (int, float))]
        result["measurements"][key] = median_range(vals)
    result["correctness_flags"] = sorted({str(f) for r in trials for f in r.get("metrics", {}).get("correctness_flags", [])})
    result["state_flags"] = sorted({str(r.get("metrics", {}).get("state_round_trip") or "missing_state_round_trip") for r in trials if r.get("metrics", {}).get("state_round_trip") != "ok"})
    result["post_timing_hash_sets"] = {key: sorted({str(r["metrics"][key]) for r in valid if r["metrics"].get(key) is not None}) for key in ("audio_hash", "video_hash")}
    result["failed_runs"] = [{"phase": r.get("phase"), "trial": r.get("trial"), "error": r.get("error"), "exit_code": r.get("guest_exit_code"), "ok": r.get("ok")} for r in trials if not r.get("ok")]
    result["invalid_trials"] = [{"trial": r.get("trial"), "metrics": r.get("metrics"), "ok": r.get("ok")} for r in trials if not run_valid(r, expected)]
    return result


def summarize_case(case_entry: dict[str, Any], input_path: Path, doc: dict[str, Any]) -> dict[str, Any]:
    case = case_entry.get("case", {})
    runs = case_entry.get("runs", []) if isinstance(case_entry.get("runs", []), list) else []
    expected = int(doc.get("frames") or 0)
    b, o = run_metrics(runs, "baseline", expected), run_metrics(runs, "optimized", expected)
    trial_pairs: dict[Any, dict[str, dict[str, Any]]] = {}
    for r in runs:
        if r.get("phase") == "trial":
            trial_pairs.setdefault(r.get("trial"), {})[str(r.get("build"))] = r
    raw_pairs = []
    valid_ratios = []
    for trial, pair in sorted(trial_pairs.items(), key=lambda item: str(item[0])):
        row = {"trial": trial, "baseline_fps": pair.get("baseline", {}).get("metrics", {}).get("fps"),
               "optimized_fps": pair.get("optimized", {}).get("metrics", {}).get("fps"), "valid_for_comparison": False, "flags": []}
        if not {"baseline", "optimized"}.issubset(pair):
            row["flags"].append("missing_build_trial")
        else:
            br, op = pair["baseline"], pair["optimized"]
            bm, om = br.get("metrics", {}), op.get("metrics", {})
            if isinstance(bm, dict) and isinstance(om, dict) and bm.get("fps") and om.get("fps"):
                row["raw_optimized_over_baseline_ratio"] = om["fps"] / bm["fps"]
            for build, run in (("baseline", br), ("optimized", op)):
                if not run_valid(run, expected): row["flags"].append(f"{build}_invalid_frames_render_hash_or_exit")
                m = run.get("metrics") or {}
                if not isinstance(m, dict): m = {}
                if m.get("state_round_trip") != "ok": row["flags"].append(f"{build}_state_round_trip_not_ok")
                if m.get("correctness_flags"): row["flags"].append(f"{build}_correctness_flags_present")
            if bm.get("audio_hash") != om.get("audio_hash") or bm.get("video_hash") != om.get("video_hash"):
                row["flags"].append("build_post_timing_hash_mismatch")
            if not row["flags"]:
                row["valid_for_comparison"] = True
                if bm.get("fps") and om.get("fps"):
                    ratio = om["fps"] / bm["fps"]
                    row["raw_optimized_over_baseline_ratio"] = ratio
                    valid_ratios.append(ratio)
        raw_pairs.append(row)
    invariants_ok = isinstance(doc.get("environment_invariants"), dict) and doc["environment_invariants"].get("all_passed") is True
    collection_finished = bool(doc.get("finished_at")) and invariants_ok
    expected_trials = int(doc.get("trials") or 0)
    pairs_complete = bool(expected_trials) and len(raw_pairs) == expected_trials and len(valid_ratios) == expected_trials
    paired = {"description": "descriptive paired measurements only; no statistical certainty classification",
              "expected_trials": expected_trials, "n_valid": len(valid_ratios),
              "ratios_median": statistics.median(valid_ratios) if valid_ratios else None,
              "percent_change_median": statistics.median([(v - 1.0) * 100 for v in valid_ratios]) if valid_ratios else None,
              "raw_pairs": raw_pairs, "collection_finished_and_invariants_passed": collection_finished,
              "qualified_for_speed_claim": collection_finished and pairs_complete,
              "qualification_flags": ([] if collection_finished else ["source_collection_incomplete_or_invariants_failed"]) + ([] if pairs_complete else ["expected_trial_pairs_missing_or_invalid"])}
    dlog = parse_device_log(case.get("device_log"))
    baseline_trials = [r for r in runs if r.get("build") == "baseline" and r.get("phase") == "trial"]
    baseline_metrics_valid = bool(baseline_trials) and len(baseline_trials) == expected_trials and all(
        run_valid(r, expected) for r in baseline_trials)
    baseline_quality_valid = baseline_metrics_valid and all(
        (r.get("metrics") or {}).get("state_round_trip") == "ok" and
        not (r.get("metrics") or {}).get("correctness_flags") for r in baseline_trials)
    # Hash parity is factual and independent of state/correctness qualification.
    base_hashes_match = bool(baseline_metrics_valid and dlog.get("post_timing_hashes") and
        {r["metrics"]["audio_hash"] for r in baseline_trials} == {dlog["post_timing_hashes"]["audio"]} and
        {r["metrics"]["video_hash"] for r in baseline_trials} == {dlog["post_timing_hashes"]["video"]})
    device_frames_valid = bool(dlog.get("frames") == expected and dlog.get("rendered_frames") == expected and dlog.get("video_skipped") == 0 and not dlog.get("state_flags"))
    comparable = baseline_quality_valid and base_hashes_match and device_frames_valid and bool(b["measurements"]["fps"])
    delta = ((dlog["fps"] / b["measurements"]["fps"]["median"] - 1.0) * 100.0) if comparable else None
    failed = case_entry.get("failed")
    bad = [r for r in runs if not r.get("ok")]
    if not failed and bad:
        failed = [{"build": r.get("build"), "phase": r.get("phase"), "trial": r.get("trial"), "error": r.get("error"), "exit_code": r.get("guest_exit_code")} for r in bad]
    warnings = list(case_entry.get("warnings", [])) if isinstance(case_entry.get("warnings"), list) else []
    if doc.get("finished_at") is None: warnings.append("source comparison has no finished_at; collection is partial")
    if not isinstance(doc.get("environment_invariants"), dict) or doc["environment_invariants"].get("all_passed") is not True: warnings.append("source environment invariants are absent or did not all pass")
    if dlog.get("status") == "parsed physical headless log" and baseline_metrics_valid and not base_hashes_match: warnings.append("physical and VM baseline post-timing hashes do not match across every baseline trial")
    if not o["valid_trial_count"]: warnings.append("optimized VM trial metrics unavailable; physical-vs-VM-baseline comparison may still be valid")
    return {
        "source_input": str(input_path), "case_id": case.get("id"), "core": case.get("core"), "rom": case.get("rom"),
        "device_log_reference": case.get("device_log"), "device_headless": dlog,
        "vm_system_mode": {"environment": "R1 system QEMU VM; not physical R1", "baseline": b, "optimized": o, "paired_trials": paired, "source_summary": case_entry.get("summary")},
        "device_vs_vm_baseline": {"status": "comparable by complete frames, rendered work, and hashes" if comparable else "device headless not comparable",
            "frames_match": dlog.get("frames") == expected, "rendered_work_valid": device_frames_valid,
            "every_baseline_trial_valid": baseline_metrics_valid, "baseline_state_and_correctness_valid": baseline_quality_valid, "post_timing_hashes_match": base_hashes_match,
            "physical_baseline_fps_percent_difference": delta,
            "qualification": "same frames, all frames rendered with no skips, and all post-timing audio/video hashes match; historical original ROM SHA-256 is unavailable" if comparable else None},
        "source_finished_at": doc.get("finished_at"), "environment_invariants": doc.get("environment_invariants"),
        "failures": failed or [], "warnings": warnings,
        "state_flags": {"device": dlog.get("state_flags", []), "baseline": b["state_flags"], "optimized": o["state_flags"]},
        "correctness_flags": {"baseline": b["correctness_flags"], "optimized": o["correctness_flags"]},
        "post_timing_hash_parity": {"device_vs_baseline": base_hashes_match, "baseline_hashes": b["post_timing_hash_sets"], "optimized_hashes": o["post_timing_hash_sets"]},
        "source_runs_preserved": runs,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--inputs", nargs="+", required=True, type=Path, help="comparison JSON files from compare.py")
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--csv", type=Path, help="optional flat CSV summary")
    args = ap.parse_args()
    docs = []
    records = []
    errors = []
    for path in args.inputs:
        source = path.expanduser().resolve()
        try:
            doc = json.loads(source.read_text())
            if not isinstance(doc, dict) or not isinstance(doc.get("cases"), dict):
                raise ValueError("expected compare.py object with cases mapping")
            docs.append({"path": str(source), "finished_at": doc.get("finished_at"), "environment_invariants": doc.get("environment_invariants")})
            for cid, entry in doc["cases"].items():
                if not isinstance(entry, dict):
                    errors.append({"input": str(source), "case_id": cid, "error": "case entry is not an object"})
                    continue
                if not entry.get("case"):
                    entry = dict(entry); entry["case"] = {"id": cid}
                records.append(summarize_case(entry, source, doc))
        except Exception as exc:
            errors.append({"input": str(source), "error": str(exc)})
    collection_complete = bool(docs) and all(d.get("finished_at") and isinstance(d.get("environment_invariants"), dict) and d["environment_invariants"].get("all_passed") is True for d in docs) and not errors
    cases_successful = bool(records) and all(not c["failures"] and not c["state_flags"]["device"] and not c["state_flags"]["baseline"] and not c["state_flags"]["optimized"] and not c["correctness_flags"]["baseline"] and not c["correctness_flags"]["optimized"] and c["vm_system_mode"]["paired_trials"]["qualified_for_speed_claim"] and all(c["vm_system_mode"][b]["trial_count"] > 0 and c["vm_system_mode"][b]["valid_trial_count"] == c["vm_system_mode"][b]["trial_count"] for b in ("baseline", "optimized")) for c in records)
    cases_comparable = bool(records) and all(c["vm_system_mode"]["paired_trials"]["qualified_for_speed_claim"] for c in records)
    device_comparable = bool(records) and all(c["device_vs_vm_baseline"]["physical_baseline_fps_percent_difference"] is not None for c in records)
    report_status = "incomplete" if not collection_complete else ("complete" if cases_successful else "complete_with_failures")
    result = {
        "schema": 1,
        "report_status": report_status,
        "collection_complete": collection_complete,
        "all_cases_comparable": cases_comparable,
        "all_device_baselines_comparable": device_comparable,
        "all_cases_successful": cases_successful,
        "finished_at": None,
        "source_inputs": docs,
        "raw_input_paths": [str(p.expanduser().resolve()) for p in args.inputs],
        "all_source_invariants_passed": collection_complete,
        "cases": records,
        "input_errors": errors,
        "provenance_limits": [
            "VM results are from the R1 system VM, not the physical R1 device.",
            "No physical optimized R1 data is represented; the report does not extrapolate VM FPS to device FPS.",
            "Physical baseline comparison is reported only when exact frame counts and post-timing audio/video hashes match; original ROM SHA-256 remains unknown.",
            "No statistical certainty classification is made.",
            "All individual source runs are preserved in each case record; original comparison JSON files remain untouched.",
        ],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    if args.csv:
        args.csv.parent.mkdir(parents=True, exist_ok=True)
        with args.csv.open("w", newline="") as f:
            fields = ["source_input", "case_id", "core", "rom", "vm_baseline_fps_median", "vm_baseline_fps_min", "vm_baseline_fps_max", "vm_optimized_fps_median", "vm_optimized_fps_min", "vm_optimized_fps_max", "paired_n", "paired_ratio_median_descriptive", "paired_percent_change_median_descriptive", "paired_qualified", "device_fps", "device_vs_vm_baseline_percent_difference", "device_comparison_status", "hash_parity", "correctness_flags", "state_flags", "failures", "warnings"]
            writer = csv.DictWriter(f, fieldnames=fields); writer.writeheader()
            for c in records:
                bm = c["vm_system_mode"]["baseline"]["measurements"]["fps"] or {}
                om = c["vm_system_mode"]["optimized"]["measurements"]["fps"] or {}
                pair = c["vm_system_mode"]["paired_trials"]
                writer.writerow({"source_input": c["source_input"], "case_id": c["case_id"], "core": c["core"], "rom": c["rom"],
                    "vm_baseline_fps_median": bm.get("median"), "vm_baseline_fps_min": (bm.get("range") or [None,None])[0], "vm_baseline_fps_max": (bm.get("range") or [None,None])[1],
                    "vm_optimized_fps_median": om.get("median"), "vm_optimized_fps_min": (om.get("range") or [None,None])[0], "vm_optimized_fps_max": (om.get("range") or [None,None])[1],
                    "paired_n": pair["n_valid"], "paired_ratio_median_descriptive": pair["ratios_median"], "paired_percent_change_median_descriptive": pair["percent_change_median"], "paired_qualified": pair["qualified_for_speed_claim"],
                    "device_fps": c["device_headless"].get("fps"), "device_vs_vm_baseline_percent_difference": c["device_vs_vm_baseline"]["physical_baseline_fps_percent_difference"],
                    "device_comparison_status": c["device_vs_vm_baseline"]["status"], "hash_parity": json.dumps(c["post_timing_hash_parity"]), "correctness_flags": json.dumps(c["correctness_flags"]), "state_flags": json.dumps(c["state_flags"]), "failures": json.dumps(c["failures"]), "warnings": json.dumps(c["warnings"])})
    print("case                 core        VM B fps      VM O fps      paired Δ%   device fps   device vs VM")
    for c in records:
        bm = c["vm_system_mode"]["baseline"]["measurements"]["fps"]
        om = c["vm_system_mode"]["optimized"]["measurements"]["fps"]
        pair = c["vm_system_mode"]["paired_trials"]
        device = c["device_headless"].get("fps")
        delta = c["device_vs_vm_baseline"]["physical_baseline_fps_percent_difference"]
        def fmt(v: Any) -> str: return "—" if v is None else f"{v:.1f}"
        print(f"{str(c['case_id'] or '')[:20]:20} {str(c['core'] or ''):10} {fmt(bm and bm['median']):>10} {fmt(om and om['median']):>12} {fmt(pair['percent_change_median']):>12} {fmt(device):>12} {fmt(delta):>14}")
        print(f"  hash={c['post_timing_hash_parity']} correctness={c['correctness_flags']} state={c['state_flags']} failures={c['failures']}")
    print(f"report_status={result['report_status']} collection_complete={collection_complete} all_cases_comparable={cases_comparable} all_device_baselines_comparable={device_comparable} all_cases_successful={cases_successful} output={args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
