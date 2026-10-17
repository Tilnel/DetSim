#!/usr/bin/env python3
"""Optional repeated-startup diagnostic; differing saved bytes are a finding, not failure.

Build the NP=1 reader with tests/startup_layout.mk, then supply an NP=1 tracer.
This does not certify runtime admission, explore schedules, or normalize snapshots.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "examples/cases"))
import run as cases

ENVIRONMENT = {"PATH": "/usr/bin:/bin", "LANG": "C", "LC_ALL": "C",
               "PYTHONHASHSEED": "0", "CC": "/usr/bin/gcc", "CXX": "/usr/bin/g++"}
SOURCE = r'''#include <sys/auxv.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>
volatile unsigned long diagnostic_at_random;
volatile unsigned long diagnostic_stack;
struct timeval diagnostic_time;
int main(void) {
 unsigned long local = 0;
 diagnostic_at_random = getauxval(AT_RANDOM);
 diagnostic_stack = (unsigned long)&local;
 if (syscall(SYS_gettimeofday, &diagnostic_time, 0) != 0) return 3;
 if (syscall(SYS_write, 1, "LAYOUT_OK\n", 10) != 10) return 4;
 return 0;
}
'''


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")


def command(argv, cwd, timeout=60):
    return subprocess.run([str(arg) for arg in argv], cwd=cwd, env=ENVIRONMENT,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, timeout=timeout)


def checked(argv, cwd):
    result = command(argv, cwd)
    cases.require(result.returncode == 0, result.stdout)
    return result.stdout


def current_hash(session):
    reply = session.send("info current")
    edges = re.findall(r"([0-9a-f]{16}) => ([0-9a-f]{16})", reply)
    if edges:
        return edges[-1][1]
    initial = re.search(r"Current state ([0-9a-f]{16}) \(no predecessor\)", reply)
    cases.require(initial, "current state identity unavailable: " + reply)
    return initial[1]


def checked_step(session):
    reply = session.send("si 0")
    cases.require("DetSim state/execution error:" not in reply and
                  "Reach illegal state" not in reply, reply)
    return reply


def dump_closed(helper, row, key, destination):
    cases.require(not row["remaining_owned_pids"] and row["tracer_exit"] == 0,
                  "refusing to inspect store before owned processes close")
    destination.mkdir()
    result = command([helper, row[key], destination], Path(row["artifacts"]))
    (destination / "reader.log").write_text(result.stdout)
    cases.require(result.returncode == 0, result.stdout)
    return destination / "summary.json"


def run_session(tracer, binary, helper, directory, mode, backend, trial, timeout):
    row = {"mode": mode, "backend": backend, "trial": trial,
           "artifacts": str(directory), "diagnostic_completed": False}
    session = cases.Session(tracer, [str(binary)], [], directory, backend, timeout)
    try:
        session.send()
        children = Path(f"/proc/{session.proc.pid}/task/{session.proc.pid}/children").read_text().split()
        cases.require(len(children) == 1, "NP=1 diagnostic requires exactly one owned tracee")
        row["owned_pids"] = [session.proc.pid, *map(int, children)]
        row["personalities"] = {pid: Path(f"/proc/{pid}/personality").read_text().strip()
                                for pid in children}
        disabled = [bool(int(value, 16) & 0x40000) for value in row["personalities"].values()]
        cases.require(all(disabled) if mode == "fixed" else not any(disabled),
                      "tracee personality does not match requested address-layout policy")
        session.send("sw 0")
        for step in range(100):
            before = current_hash(session)
            reply = checked_step(session)
            if "gettimeofday(" in reply:
                target = current_hash(session)
                row.update(predecessor=before, checkpoint=target, controlled_steps=step + 1)
                break
            cases.require("has exited" not in reply, reply)
        else:
            raise cases.CaseFailure("controlled checkpoint missing")
        row["replays"] = []
        for _ in range(3):
            cases.require("Loaded state" in session.send("load " + before), "checkpoint load failed")
            cases.require("gettimeofday(" in checked_step(session), "checkpoint edge did not repeat")
            replay = current_hash(session)
            row["replays"].append(replay)
            cases.require(replay == target, "same-checkpoint replay identity drift")
        for _ in range(30):
            reply = checked_step(session)
            if "exit_group(" in reply:
                cases.require("has exited with status 0" in session.send("si 0"), "guest nonzero exit")
                row.update(terminal=current_hash(session), guest_exit=0)
                break
        else:
            raise cases.CaseFailure("controlled guest did not finish")
    except Exception as error:
        row["failure"] = cases.classify_error(error, session.log)
        raise
    finally:
        row["tracer_exit"] = session.close()
        row["remaining_owned_pids"] = [pid for pid in row.get("owned_pids", [session.proc.pid])
                                       if Path(f"/proc/{pid}").exists()]
        write_json(directory / "row.json", row)
        cases.require(row["tracer_exit"] == 0 and not row["remaining_owned_pids"], "owned cleanup failed")
    row["dump"] = str(dump_closed(helper, row, "checkpoint", directory / "dump"))
    checkpoint_dump = json.loads(Path(row["dump"]).read_text())
    cases.require(checkpoint_dump["format"] == backend, "checkpoint snapshot backend differs from request")
    cases.require((directory / "dump/stdout.bin").read_bytes() == b"" and
                  checkpoint_dump["stdout_bytes"] == 0 and checkpoint_dump["stdout_descriptor_open"],
                  "prewrite checkpoint must contain an open, empty captured stdout")
    row["checkpoint_stdout_bytes"] = 0
    row["terminal_dump"] = str(dump_closed(helper, row, "terminal", directory / "terminal-dump"))
    terminal_dump = json.loads(Path(row["terminal_dump"]).read_text())
    cases.require(terminal_dump["format"] == backend, "terminal snapshot backend differs from request")
    cases.require(not terminal_dump["stdout_descriptor_open"], "terminal stdout descriptor remained open")
    stdout = (directory / "terminal-dump/stdout.bin").read_bytes()
    cases.require(stdout == b"LAYOUT_OK\n", "saved terminal stdout differs from fixture oracle")
    row.update(stdout_hex=stdout.hex(), config_sha256=cases.digest(directory / "config.json"),
               runtime_profile=json.loads((directory / "config.json").read_text())["RuntimeProfile"],
               diagnostic_completed=True)
    write_json(directory / "row.json", row)
    return row


def load_dump(row):
    path = Path(row["dump"])
    result = json.loads(path.read_text())
    result["directory"] = path.parent
    return result


def saved_read(snapshot, address, size):
    for region in snapshot["regions"]:
        if region["start"] <= address and address + size <= region["end"]:
            data = (snapshot["directory"] / region["file"]).read_bytes()
            cases.require(len(data) == region["end"] - region["start"], "saved region short read")
            return data[address - region["start"]:address - region["start"] + size]
    raise cases.CaseFailure("unavailable saved address " + hex(address))


def difference_ranges(left, right, address):
    ranges = []
    start = None
    for index in range(len(left) + 1):
        changed = index < len(left) and left[index] != right[index]
        if changed and start is None:
            start = index
        elif not changed and start is not None:
            ranges.append({"start": address + start, "end": address + index,
                           "baseline_hex": left[start:index].hex(), "other_hex": right[start:index].hex()})
            start = None
    return ranges


def analyze(report, work):
    comparisons = []
    for mode in ("normal", "fixed"):
        for backend in ("full", "pages"):
            selected = [row for row in report["rows"] if (row["mode"], row["backend"]) == (mode, backend)]
            base = load_dump(selected[0])
            layout = lambda data: [(r["start"], r["end"], r["prot"], r["flags"]) for r in data["regions"]]
            for row in selected[1:]:
                other = load_dump(row)
                comparison = {"mode": mode, "backend": backend, "trial": row["trial"],
                              "layout_equal": layout(base) == layout(other), "region_differences": [],
                              "register_differences": {key: [value, other["registers"][key]]
                                  for key, value in base["registers"].items() if value != other["registers"][key]},
                              "metadata_equal": (base["directory"] / "tracee-metadata.bin").read_bytes() ==
                                                (other["directory"] / "tracee-metadata.bin").read_bytes(),
                              "maps_equal": (base["directory"] / "maps.txt").read_bytes() ==
                                            (other["directory"] / "maps.txt").read_bytes(),
                              "clock_equal": base["clock"] == other["clock"],
                              "model_identity_equal": base["model_identity"] == other["model_identity"]}
                aux = []
                for snapshot in (base, other):
                    pointer = struct.unpack("<Q", saved_read(snapshot, report["symbols"]["diagnostic_at_random"], 8))[0]
                    aux.append({"pointer": pointer, "bytes": saved_read(snapshot, pointer, 16).hex()})
                comparison["AT_RANDOM"] = aux
                if comparison["layout_equal"]:
                    for left, right in zip(base["regions"], other["regions"]):
                        a = (base["directory"] / left["file"]).read_bytes()
                        b = (other["directory"] / right["file"]).read_bytes()
                        cases.require(len(a) == len(b) == left["end"] - left["start"], "saved region short read")
                        ranges = difference_ranges(a, b, left["start"])
                        if ranges:
                            comparison["region_differences"].append({"start": left["start"], "end": left["end"],
                                "file": left["file"], "changed_bytes": sum(r["end"] - r["start"] for r in ranges),
                                "ranges": ranges})
                comparison["changed_memory_bytes"] = sum(r["changed_bytes"] for r in comparison["region_differences"]) \
                    if comparison["layout_equal"] else None
                comparison["bitwise_equal"] = (comparison["layout_equal"] and comparison["metadata_equal"] and
                    comparison["maps_equal"] and not comparison["register_differences"] and
                    comparison["changed_memory_bytes"] == 0 and comparison["clock_equal"] and comparison["model_identity_equal"])
                comparison["raw_diff_file"] = f"raw-diff-{mode}-{backend}-{row['trial']}.json"
                write_json(work / comparison["raw_diff_file"], comparison)
                comparisons.append(comparison)
    write_json(work / "analysis.json", {"scope": "Exact saved bytes only; identical saved extents required. No masks or normalization.",
                                       "comparisons": comparisons})
    return comparisons


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tracer", type=Path, required=True, help="immutable NP=1 tracer binary")
    parser.add_argument("--dump-helper", type=Path, default=ROOT / "build/release/np1/tests/startup_layout_dump")
    parser.add_argument("--output", type=Path, default=ROOT / "build/startup-layout-diagnostic")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args()
    if args.repeats < 2 or not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--repeats must be >=2 and --timeout must be finite and positive")
    # Session intentionally uses its caller environment; give it only this explicit fixture environment.
    os.environ.clear()
    os.environ.update(ENVIRONMENT)
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="run-", dir=args.output))
    work.chmod(0o700)
    report = {"scope": "Optional startup diagnostic, not a model-checking acceptance pass.",
              "diagnostic_completed": False, "cross_startup_bitwise_deterministic": None,
              "workdir": str(work), "rows": [], "environment": ENVIRONMENT,
              "session_environment": {"common": cases.ENV, "backend_variable": "DETSIM_SNAPSHOT_BACKEND"},
              "dump_helper_build_recipe": "make -f tests/startup_layout.mk NP=1 startup-layout-dump CC=/usr/bin/gcc CXX=/usr/bin/g++",
              "provenance_scope": "Executable hashes identify actual tools. Source hashes record files at run; caller supplies prebuilt tools."}
    report_path = work / "results.json"
    write_json(report_path, report)  # Freeze the fixture's ancestry before any target starts.
    try:
        tracer = args.tracer.resolve()
        helper = args.dump_helper.resolve()
        for path in (tracer, helper, Path("/usr/bin/setarch"), Path("/usr/bin/gcc"), Path("/usr/bin/nm")):
            cases.require(path.is_file() and os.access(path, os.X_OK), "missing executable: " + str(path))
        fixture = work / "fixture"
        fixture.mkdir()
        sessions = work / "sessions"
        sessions.mkdir()
        source, binary, wrapper = fixture / "probe.c", fixture / "probe", fixture / "fixed-layout-tracer"
        source.write_text(SOURCE)
        compile_command = ["/usr/bin/gcc", "-O0", "-g", "-gdwarf-4", "-fno-pie", "-no-pie",
                           "-fno-stack-protector", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary)]
        (fixture / "compile.log").write_text(checked(compile_command, work))
        wrapper.write_text("#!/bin/sh\nunset PWD OLDPWD SHLVL\nexec /usr/bin/setarch x86_64 -R " + shlex.quote(str(tracer)) + ' "$@"\n')
        wrapper.chmod(0o700)
        symbols = {}
        for line in checked(["/usr/bin/nm", "-n", binary], work).splitlines():
            fields = line.split()
            if len(fields) == 3 and fields[2].startswith("diagnostic_"):
                symbols[fields[2]] = int(fields[0], 16)
        cases.require("diagnostic_at_random" in symbols, "fixture symbol missing")
        report.update(symbols=symbols, compile=compile_command, compiler_version=checked(["/usr/bin/gcc", "--version"], work),
                      git_head=checked(["git", "rev-parse", "HEAD"], ROOT).strip(),
                      product_diff_sha256=hashlib.sha256(checked(["git", "diff", "--", "src"], ROOT).encode()).hexdigest(),
                      host_aslr_before=Path("/proc/sys/kernel/randomize_va_space").read_text().strip())
        artifacts = {"source": source, "binary": binary, "wrapper": wrapper, "tracer": tracer, "dump_helper": helper,
                     "runner": Path(__file__).resolve(), "common_session": ROOT / "examples/cases/run.py",
                     "reader_source": ROOT / "tests/startup_layout_dump.cpp", "reader_makefile": ROOT / "tests/startup_layout.mk",
                     "compiler": Path("/usr/bin/gcc").resolve(), "setarch": Path("/usr/bin/setarch"), "shell": Path("/bin/sh").resolve()}
        report["artifacts"] = {name: {"path": str(path), "sha256": cases.digest(path)} for name, path in artifacts.items()}
        report["fixture_ancestry"] = {str(path): {"mtime_ns": path.stat().st_mtime_ns, "ctime_ns": path.stat().st_ctime_ns}
                                      for path in (fixture, *fixture.parents)}
        write_json(report_path, report)
        for mode in ("normal", "fixed"):
            native = command((["/usr/bin/setarch", "x86_64", "-R"] if mode == "fixed" else []) + [binary], work)
            report[mode + "_native"] = {"exit": native.returncode, "output": native.stdout}
            cases.require(native.returncode == 0 and native.stdout == "LAYOUT_OK\n", "native fixture failed: " + native.stdout)
            for backend in ("full", "pages"):
                for trial in range(args.repeats):
                    directory = sessions / f"{mode}-{backend}-{trial}"
                    try:
                        row = run_session(wrapper if mode == "fixed" else tracer, binary, helper,
                                          directory, mode, backend, trial, args.timeout)
                    except Exception:
                        report["failed_session"] = str(directory)
                        if (directory / "row.json").exists():
                            report["rows"].append(json.loads((directory / "row.json").read_text()))
                        raise
                    report["rows"].append(row)
                    write_json(report_path, report)
                    print(mode, backend, trial, "diagnostic complete", flush=True)
        for name, artifact in report["artifacts"].items():
            cases.require(cases.digest(Path(artifact["path"])) == artifact["sha256"], "artifact changed during diagnostic: " + name)
        for path, expected in report["fixture_ancestry"].items():
            info = Path(path).stat()
            cases.require({"mtime_ns": info.st_mtime_ns, "ctime_ns": info.st_ctime_ns} == expected,
                          "fixture ancestor metadata changed during trials: " + path)
        report["host_aslr_after"] = Path("/proc/sys/kernel/randomize_va_space").read_text().strip()
        cases.require(report["host_aslr_after"] == report["host_aslr_before"], "host ASLR setting changed")
        comparisons = analyze(report, work)
        report["diagnostic_completed"] = True
        report["cross_startup_bitwise_deterministic"] = all(row["bitwise_equal"] for row in comparisons)
        report["fixed_layout_bitwise_deterministic"] = all(row["bitwise_equal"] for row in comparisons if row["mode"] == "fixed")
        return 0
    except Exception as error:
        report["failure"] = cases.classify_error(error)
        print("startup diagnostic failed:", error, file=sys.stderr)
        return 1
    finally:
        write_json(report_path, report)
        print("RESULT", report_path, flush=True)


if __name__ == "__main__":
    sys.exit(main())
