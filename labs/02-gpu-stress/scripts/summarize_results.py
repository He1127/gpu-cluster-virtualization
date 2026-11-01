"""Summarize saved run evidence only. Never executes a workload or fills gaps.

Requires Python 3.8 or later (standard library only).
Optional argument is the results directory. --discover accepts custom labels.
Outputs: summary.json and 08_summary.txt. Refuses replacement unless --overwrite.
"""
from __future__ import annotations

import csv
import argparse
from collections import Counter
from datetime import datetime
import hashlib
import io
import json
import math
from pathlib import Path
import re
import statistics
import sys

NA = "N/A"
ROOT = Path(__file__).resolve().parents[1]
EXPECTED = (
    ("01_cpu", "cpu"),
    ("02_info", "info"),
    ("03_vector", "vector"),
    ("04_transfer", "transfer"),
    ("05_warmup60", "compute"),
    ("06_compute300", "compute"),
    ("07_memory120", "memory"),
)
NUMERIC_METRIC_FIELDS = {
    "elapsed_s": float, "iterations": int, "gpu_ms": float, "metric": float,
    "errors": int, "checked_values": int,
}
TELEMETRY_FIELDS = {
    "temperature_c": "temperature.gpu",
    "gpu_utilization_percent": "utilization.gpu [%]",
    "memory_utilization_percent": "utilization.memory [%]",
    "memory_used_mib": "memory.used [MiB]",
    "power_w": "power.draw [W]",
    "graphics_clock_mhz": "clocks.current.graphics [MHz]",
    "memory_clock_mhz": "clocks.current.memory [MHz]",
}


def numeric(value):
    if value is None:
        return None
    try:
        number = float(str(value).strip())
        return number if math.isfinite(number) else None
    except (ValueError, TypeError):
        return None


def stats(values):
    """Retain unsupported/missing values as N/A, never silently turn into zero."""
    raw = [str(x).strip() if x is not None else "" for x in values]
    valid = [v for x in raw if (v := numeric(x)) is not None]
    invalid = Counter(x or "<empty>" for x in raw if numeric(x) is None)
    return {
        "sample_count": len(raw), "valid_count": len(valid),
        "na_count": len(raw) - len(valid), "unavailable_values": dict(invalid),
        "min": min(valid) if valid else NA,
        "mean": statistics.fmean(valid) if valid else NA,
        "max": max(valid) if valid else NA,
    }


def read_saved(path, warnings, sources):
    if not path.is_file():
        warnings.append("missing file: " + path.name)
        return None
    blob = path.read_bytes()
    sources[path.name] = {
        "bytes": len(blob), "sha256": hashlib.sha256(blob).hexdigest()
    }
    return blob.decode("utf-8-sig", errors="replace")


def read_csv_saved(text, warnings, label):
    if text is None or not text.strip():
        return []
    result = []
    try:
        reader = csv.DictReader(io.StringIO(text))
        for line_no, row in enumerate(reader, 2):
            normalized = {str(k).strip(): (v.strip() if isinstance(v, str) else v)
                          for k, v in row.items() if k is not None}
            if not any(v for v in normalized.values()):
                continue
            if None in row or any(v is None for v in normalized.values()):
                warnings.append(f"{label}: incomplete/malformed CSV row {line_no} excluded")
                continue
            result.append(normalized)
    except csv.Error as exc:
        warnings.append(f"{label}: CSV error: {exc}")
    return result


def match_last(pattern, text, cast=str):
    matches = re.findall(pattern, text or "")
    if not matches:
        return NA
    try:
        return cast(matches[-1])
    except (ValueError, TypeError):
        return NA


def summarize_run(directory, label, mode, current_executable_hash):
    warnings, sources = [], {}
    contents = {}
    for suffix in ("session.json", "stdout.txt", "stderr.txt", "metrics.csv",
                   "telemetry.csv", "telemetry_stderr.txt"):
        path = directory / f"{label}_{suffix}"
        contents[suffix] = read_saved(path, warnings, sources)
    session = None
    if contents["session.json"]:
        try:
            session = json.loads(contents["session.json"])
        except json.JSONDecodeError as exc:
            warnings.append("invalid session JSON: " + str(exc))
    stdout = contents["stdout.txt"] or ""
    metrics = read_csv_saved(contents["metrics.csv"], warnings, label + " metrics")
    for row in metrics:
        for key, cast in NUMERIC_METRIC_FIELDS.items():
            value = numeric(row.get(key))
            if value is None:
                warnings.append(f"{label} metrics: unavailable {key} at phase {row.get('phase')}")
                row[key] = NA
            else:
                row[key] = cast(value)
    final_kernel = next((r for r in reversed(metrics) if r.get("phase") == "final_kernel_rate"), None)
    final_wall = next((r for r in reversed(metrics) if r.get("phase") == "final_wall_rate"), None)
    final_row = final_kernel or (metrics[-1] if metrics else None)
    telemetry_rows = read_csv_saved(contents["telemetry.csv"], warnings, label + " telemetry")
    telemetry = {
        "row_count": len(telemetry_rows),
        "window_note": "整次进程运行期间采集窗口，含初始化、参考计算及周期校验；未标记精确稳态区间。",
        "memory_utilization_note": "utilization.memory 是采样周期内显存读写忙碌时间比例，不是显存容量占用率；容量使用另看 memory_used_mib。",
        "first_timestamp": telemetry_rows[0].get("timestamp", NA) if telemetry_rows else NA,
        "last_timestamp": telemetry_rows[-1].get("timestamp", NA) if telemetry_rows else NA,
        "pstate_counts": dict(Counter(row.get("pstate", NA) for row in telemetry_rows)),
    }
    for output_key, csv_key in TELEMETRY_FIELDS.items():
        telemetry[output_key] = stats(row.get(csv_key) for row in telemetry_rows)
    result_match = re.findall(r"RESULT=(PASS|FAIL)\s+CUDA_API_errors=(\d+)", stdout)
    stdout_result = result_match[-1][0] if result_match else NA
    api_errors = int(result_match[-1][1]) if result_match else NA
    process_success = bool(session and session.get("exit_code") == 0 and
                           session.get("stop_reason") == "completed")
    if not session:
        status = "incomplete_or_not_run"
    elif process_success and stdout_result == "PASS" and api_errors == 0:
        status = "complete_pass"
    else:
        status = "failed_or_unverified"
    if mode in {"compute", "memory"} and status == "complete_pass" and not final_kernel:
        status = "failed_or_unverified"
        warnings.append("PASS session is missing final_kernel_rate evidence")
    stderr = contents["stderr.txt"] or ""
    telemetry_stderr = contents["telemetry_stderr.txt"] or ""
    if stderr.strip():
        warnings.append("workload stderr is not empty; inspect saved file")
    if telemetry_stderr.strip():
        warnings.append("telemetry stderr is not empty; inspect saved file")
    executable_hash = session.get("sha256", NA) if session else NA
    matches_current = (executable_hash == current_executable_hash
                       if executable_hash != NA and current_executable_hash != NA else NA)
    if matches_current is False:
        warnings.append("run executable SHA256 differs from current executable; this evidence belongs to an earlier binary")
    seconds = final_kernel.get("elapsed_s", NA) if final_kernel else NA
    if mode in {"compute", "memory"}:
        warnings.append("Only periodic full-output checks were performed; not every launch output was read back.")
    correctness = {
        "errors": final_row.get("errors", NA) if final_row else NA,
        "checked_values": final_row.get("checked_values", NA) if final_row else NA,
        "checked_unit": "bytes" if mode == "transfer" else "values",
        "checks": match_last(r"\bchecks=(\d+)\s+checked_values=", stdout, int),
        "max_abs_error": match_last(r"max_abs_error=(\S+)", stdout),
        "note": "取最终累计计数；不相加 progress/final 行。传输模式的 checked_values 实际按字节计数。",
    }
    scalar_results = {
        "kernel_min_ms": match_last(r"kernel_min_ms=([\d.eE+-]+)", stdout, float),
        "kernel_max_ms": match_last(r"kernel_max_ms=([\d.eE+-]+)", stdout, float),
        "sample_output": match_last(r"sample:\s*([^\r\n]+)", stdout),
        "cpu_sum": match_last(r"\bsum=(\d+)", stdout, int),
        "cpu_expected": match_last(r"\bexpected=(\d+)", stdout, int),
        "cpu_completed_threads": match_last(r"\bcompleted=(\d+)", stdout, int),
        "cpu_thread_work_and_wait_ms": match_last(r"thread_work_and_wait_ms=([\d.eE+-]+)", stdout, float),
    }
    return {
        "label": label, "mode": mode, "status": status, "session": session,
        "timed_work_elapsed_s": seconds,
        "metrics_rows": metrics, "final_kernel_rate": final_kernel,
        "final_wall_rate": final_wall, "stdout_result": stdout_result,
        "cuda_api_errors": api_errors, "correctness": correctness,
        "scalar_results": scalar_results, "telemetry": telemetry,
        "executable_sha256": executable_hash,
        "evidence_version_matches_current_executable": matches_current,
        "source_files": sources, "warnings": warnings,
    }


def fmt(value, digits=3):
    return f"{value:.{digits}f}" if isinstance(value, (int, float)) else str(value)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results_directory", nargs="?", type=Path, default=ROOT / "results")
    parser.add_argument("--discover", action="store_true", help="read labels/modes from saved *_session.json files")
    parser.add_argument("--overwrite", action="store_true", help="explicitly replace an existing generated summary")
    args = parser.parse_args()
    directory = args.results_directory.resolve()
    if not directory.is_dir():
        raise SystemExit("Results directory does not exist: " + str(directory))
    outputs = [directory / "summary.json", directory / "08_summary.txt"]
    if not args.overwrite and any(path.exists() for path in outputs):
        raise SystemExit("Summary already exists. Use a new results directory or explicitly pass --overwrite.")
    exe = ROOT / "bin" / "api_stress.exe"
    current_hash = hashlib.sha256(exe.read_bytes()).hexdigest() if exe.is_file() else NA
    selected = EXPECTED
    if args.discover:
        discovered = []
        for path in sorted(directory.glob("*_session.json")):
            try:
                session = json.loads(path.read_text(encoding="utf-8-sig"))
            except (OSError, json.JSONDecodeError) as exc:
                raise SystemExit(f"Cannot read saved session {path.name}: {exc}")
            label, mode = session.get("label"), session.get("mode")
            if (not isinstance(label, str) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,79}", label)
                    or path.name != label + "_session.json"
                    or mode not in {"cpu", "info", "vector", "transfer", "compute", "memory"}):
                raise SystemExit("Invalid label or mode in saved session: " + path.name)
            discovered.append((label, mode))
        if not discovered:
            raise SystemExit("No saved *_session.json files found.")
        selected = tuple(discovered)
    runs = [summarize_run(directory, label, mode, current_hash) for label, mode in selected]
    run_by_label = {r["label"]: r for r in runs}
    result = {
        "generated_at_local": datetime.now().astimezone().isoformat(),
        "results_directory": str(directory), "schema_version": 1,
        "current_executable_sha256": current_hash,
        "all_seven_completed_pass": all(label in run_by_label and run_by_label[label]["mode"] == mode
                                        and run_by_label[label]["status"] == "complete_pass"
                                        for label, mode in EXPECTED),
        "all_available_completed_pass": bool(runs) and all(r["status"] == "complete_pass" for r in runs),
        "all_evidence_matches_current_executable": all(r["evidence_version_matches_current_executable"] is True for r in runs),
        "notes": [
            "Only existing files were read. Missing or unsupported numerical data remains N/A.",
            "Telemetry min/mean/max uses available samples over each process capture window; not exact steady-state GPU-only interval.",
            "Rates and correctness counts use final rows; cumulative progress rows are never summed.",
            "Final kernel-rate uses GPU event time; final wall-rate includes host submission, synchronization, and periodic checking within the timed phase.",
            "nvidia-smi utilization.memory is the memory read/write busy-time percentage, not allocated VRAM capacity percentage.",
            "PASS is bounded evidence for this workload and duration, not universal hardware stability certification.",
        ],
        "runs": runs, "run_by_label": run_by_label,
    }
    (directory / "summary.json").write_text(json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    lines = ["CPU/GPU 本机运行结果汇总", "生成时间：" + result["generated_at_local"],
             "数据：只读取现有 session / metrics / telemetry / stdout；缺失项保留 N/A。",
             "采样窗口：每阶段进程运行期间，含初始化/参考计算/校验；不是精确稳态区间。", ""]
    for run in runs:
        lines.append(run["label"] + "  " + run["status"])
        session = run["session"] or {}
        lines.append("  RESULT=" + run["stdout_result"] + " exit=" + str(session.get("exit_code", NA)) + " CUDA_errors=" + str(run["cuda_api_errors"]))
        for key in ("final_kernel_rate", "final_wall_rate"):
            rate = run[key]
            if rate:
                lines.append("  " + key + ": " + fmt(rate.get("metric", NA)) + " " + rate.get("units", "") + "; elapsed=" + fmt(rate.get("elapsed_s", NA)) + " s")
        c = run["correctness"]
        lines.append("  mismatches=" + str(c["errors"]) + "; checked=" + str(c["checked_values"]) + " " + c["checked_unit"])
        for key, title, unit in (("temperature_c", "温度", "C"), ("gpu_utilization_percent", "GPU利用率", "%"),
                                 ("power_w", "功耗", "W"), ("graphics_clock_mhz", "图形频率", "MHz")):
            s = run["telemetry"][key]
            lines.append("  " + title + " [min/mean/max]=" + "/".join(fmt(s[x]) for x in ("min", "mean", "max")) + " " + unit +
                         "; samples=" + str(s["sample_count"]) + " valid=" + str(s["valid_count"]) + " N/A=" + str(s["na_count"]))
        if not run["evidence_version_matches_current_executable"] is True:
            lines.append("  注意：本阶段可执行文件哈希与当前文件未确认一致。")
        lines.append("")
    lines += ["说明：", "1. progress 为累计记录，不能加总为总次数或总校验量。", "2. transfer 的 checked_values 列按字节计数；DtoD速率将复制负载字节仅计算一次。",
              "3. GPU事件速率与端到端速率分别报告；当前示例核函数测值不是显卡官方峰值。", "4. 具体错误、缺失文件或版本不一致信息见 summary.json 的 warnings。"]
    (directory / "08_summary.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("Saved " + str(directory / "summary.json"))
    print("Saved " + str(directory / "08_summary.txt"))
    print("all_seven_completed_pass=" + str(result["all_seven_completed_pass"]))
    print("all_evidence_matches_current_executable=" + str(result["all_evidence_matches_current_executable"]))


if __name__ == "__main__":
    main()
