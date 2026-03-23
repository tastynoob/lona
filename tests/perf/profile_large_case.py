#!/usr/bin/env python3

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

from generate_large_case import write_case


def parse_args() -> argparse.Namespace:
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument(
        "--manifest",
        type=Path,
        default=root / "tests/perf/large_case_manifest.json",
    )
    parser.add_argument(
        "--workdir",
        type=Path,
        default=root / "build/perf/profile-large-case",
    )
    parser.add_argument(
        "--report-stdio",
        action="store_true",
        help="Print a text report instead of opening the interactive perf report UI.",
    )
    return parser.parse_args()


def require_perf_binary() -> str:
    perf = shutil.which("perf")
    if perf is None:
        raise RuntimeError("missing `perf`; install Linux perf tools first")
    return perf


def write_text(path: Path, text: str) -> None:
    path.write_text(text, encoding="utf-8")


def prepare_workdir(workdir: Path) -> None:
    workdir.mkdir(parents=True, exist_ok=True)
    for path in (
        workdir / "fixed-large-100k.lo",
        workdir / "perf.data",
        workdir / "perf.data.old",
        workdir / "perf.record.stderr",
        workdir / "perf.report.stderr",
    ):
        if path.exists():
            path.unlink()


def run_perf_record(perf: str, compiler: Path, input_path: Path, workdir: Path) -> Path:
    perf_data = workdir / "perf.data"
    record_err = workdir / "perf.record.stderr"
    completed = subprocess.run(
        [
            perf,
            "record",
            "--call-graph",
            "dwarf",
            "-o",
            str(perf_data),
            "--",
            str(compiler),
            "--emit",
            "ir",
            "--verify-ir",
            str(input_path),
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    write_text(record_err, completed.stderr)
    if completed.returncode != 0:
        raise RuntimeError(
            "perf record failed; see "
            f"{record_err}. "
            "This usually means perf events are blocked by kernel policy "
            "(for example perf_event_paranoid) or missing capabilities."
        )
    return perf_data


def open_perf_report(perf: str, perf_data: Path, workdir: Path, stdio: bool) -> int:
    report_err = workdir / "perf.report.stderr"
    cmd = [perf, "report", "-i", str(perf_data)]
    if stdio:
        cmd.insert(2, "--stdio")
    completed = subprocess.run(
        cmd,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    write_text(report_err, completed.stderr)
    if completed.returncode != 0:
        raise RuntimeError(f"perf report failed; see {report_err}")
    return completed.returncode


def main() -> int:
    args = parse_args()
    compiler = args.compiler.resolve()
    if not compiler.is_file():
        raise RuntimeError(f"missing compiler binary: {compiler}")

    workdir = args.workdir.resolve()
    prepare_workdir(workdir)
    input_path = workdir / "fixed-large-100k.lo"
    metadata = write_case(input_path, args.manifest)

    perf = require_perf_binary()
    perf_data = run_perf_record(perf, compiler, input_path, workdir)

    print("[large-case-perf]")
    print(f"generator-version: {metadata['generator_version']}")
    print(f"feature-block-count: {metadata['feature_block_count']}")
    print(f"source-lines: {metadata['line_count']}")
    print(f"source-sha256: {metadata['sha256']}")
    print(f"sample: {input_path}")
    print(f"perf-data: {perf_data}")
    print("opening `perf report`...")
    return open_perf_report(perf, perf_data, workdir, args.report_stdio)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
