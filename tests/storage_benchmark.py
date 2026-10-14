"""Run byte-checked storage benchmarks in disposable stores, retaining JSON/logs."""
import argparse
import json
from pathlib import Path
import statistics
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("binary", type=Path)
parser.add_argument("--label", default="current")
parser.add_argument("--trials", type=int, default=3)
parser.add_argument("--modes", nargs="+", choices=("hot", "pressure", "metadata"),
                    default=["hot", "pressure", "metadata"])
parser.add_argument("--output", type=Path, default=Path("build/storage-benchmark"))
args = parser.parse_args()
if args.trials < 1:
    parser.error("--trials must be positive")
args.output.mkdir(parents=True, exist_ok=True)
results = []
for mode in args.modes:
    rows = []
    for trial in range(args.trials):
        directory = Path(tempfile.mkdtemp(prefix=f"{args.label}-{mode}-{trial}-", dir=args.output)).resolve()
        with (directory / "run.log").open("w") as log:
            subprocess.run([str(args.binary.resolve()), str(directory / "store"), mode, args.label],
                           stdout=log, stderr=subprocess.STDOUT, cwd=directory, timeout=60, check=True)
        row = next(json.loads(line) for line in (directory / "run.log").read_text().splitlines()
                   if line.startswith('{"label":'))
        rows.append(row)
    summary = {"mode": mode, "trials": rows}
    summary["median"] = {key: statistics.median(row[key] for row in rows)
                         for key in rows[0] if key.endswith("_ms") or key == "rss_kib"}
    results.append(summary)
    print(json.dumps(summary["median"] | {"mode": mode}))
(args.output / (args.label + "-results.json")).write_text(json.dumps(results, indent=2) + "\n")
