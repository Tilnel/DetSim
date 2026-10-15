"""Real runner regressions: bounded search and missing tools must not turn green."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--tracer", type=Path, required=True)
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix="ds-case-runner-") as directory:
    base = [sys.executable, str(ROOT / "examples/cases/run.py"), "--tracer", str(args.tracer.resolve()),
            "--mode", "runtimes", "--backend", "full", "--output", directory]
    bounded = subprocess.run(base + ["--languages", "c", "--depth", "1"],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=40)
    assert bounded.returncode == 1, bounded.stdout
    report = json.loads(Path(bounded.stdout.rsplit("RESULT ", 1)[1].strip()).read_text())
    row = report["cases"][0]
    assert row["native"] == "pass" and row["control"]["status"] == "pass", row
    assert row["control"]["controlled_steps"] == 2, row
    assert row["model"]["status"] == "bounded" and row["model"]["guest_exit"] is None, row
    assert not report["passed"] and not row["passed"], row
    completed = subprocess.run(base + ["--languages", "c", "--depth", "30", "--memory-mb", "1024"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=40)
    assert completed.returncode == 0, completed.stdout
    report = json.loads(Path(completed.stdout.rsplit("RESULT ", 1)[1].strip()).read_text())
    row = report["cases"][0]
    assert report["tracer_address_space_limit_bytes"] == 1024**3 and report["passed"], report
    assert row["model"]["guest_exit"] == 0 and row["model"]["replay"] == "same_process_hash_match_3x", row
    assert Path(row["model"]["artifacts"], "completion.json").is_file(), row
    absent = subprocess.run(base + ["--languages", "python"],
        env=os.environ | {"CASE_PYTHON": str(Path(directory) / "no-such-python")},
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=40)
    assert absent.returncode == 1, absent.stdout
    report = json.loads(Path(absent.stdout.rsplit("RESULT ", 1)[1].strip()).read_text())
    assert report["cases"][0]["status"] == "toolchain_unavailable" and not report["passed"], report
print("PASS case runner: bounded/missing tools remain non-green; checked completion replays with explicit memory limit")
