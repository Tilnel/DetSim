#!/usr/bin/env python3
"""CLI quiet searches preserve transition outcomes and explicit history/replay."""
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import re
import shlex
import signal
import threading
import time
import sys
import tempfile

from dfs_residency import GUEST, PLUGIN, Session, build_plugin, command, require, ROOT


def search(args, binary, plugin, payload, directory, backend, algorithm, quiet):
    session = Session(args.tracer, [str(binary)], plugin, directory, backend, args.timeout,
                      choices=True, fsmap=[{"Host": str(payload), "Target": "/payload0"}])
    try:
        session.send(); session.send("sw 0")
        require("no predecessor" in session.send("info current"), "root summary")
        reply = session.send(("bfs" if algorithm == "bfs" else "dfs 120") + (" --quiet" if quiet else ""))
        require("Complete explore all" in reply, reply[-2000:])
        require(("steps in total." in reply) != quiet, "automatic history quiet policy")
        count = re.search(r"all (\d+) sys_states \(new: (\d+)\), total unique: (\d+)", reply)
        require(count and int(count[3]) == 81, "finite search state count")
        log = session.log.read_text()
        transitions = Counter(re.findall(r"DFS_EDGE ([^\n]+)", log))
        require(sum(transitions.values()) == 80, "actual transition count")
        require(sorted(re.findall(r"DFS_TERMINAL (\d+)", log)) == ["0", "1", "2", "3"], "all terminal branches")
        short = session.send("info current")
        edge = re.findall(r"([0-9a-f]{16}) => ([0-9a-f]{16})", short)
        require(len(edge) == 1 and "exit_group(" in short and "<nonreturning>" in short, "terminal scalar edge")
        full = session.send("info")
        history = re.findall(r"([0-9a-f]{16}) => ([0-9a-f]{16})", full)
        require(len(history) > 1 and history[-1] == edge[0] and "steps in total." in full,
                "explicit full history restored after quiet search")
        for _ in range(3):
            require("Loaded state" in session.send("load " + edge[0][0]), "replay restore")
            session.send("si 0")
            replay = session.send("info current")
            require(re.findall(r"([0-9a-f]{16}) => ([0-9a-f]{16})", replay) == edge, "replay identity")
        return {"unique": int(count[3]), "transitions": sorted(transitions.items()), "replay": "terminal_3x"}
    finally:
        status = session.close()
        if sys.exc_info()[0] is None:
            require(status == 0, "tracer cleanup")


def violation(args, binary, plugin, payload, directory, backend, algorithm, quiet):
    session = Session(args.tracer, [str(binary)], plugin, directory, backend, args.timeout,
                      choices=True, fsmap=[{"Host": str(payload), "Target": "/payload0"}])
    try:
        session.send(); session.send("sw 0")
        reply = session.send(("bfs" if algorithm == "bfs" else "dfs 120") + (" --quiet" if quiet else ""))
        require("Stopped for illegal state" in reply and "Complete explore all" not in reply,
                "search stops at violation")
        require(("steps in total." in reply) != quiet, "violation history quiet policy")
        transitions = Counter(re.findall(r"DFS_EDGE ([^\n]+)", session.log.read_text()))
        short = session.send("info current")
        edge = re.findall(r"([0-9a-f]{16}) => ([0-9a-f]{16})", short)
        require(len(edge) == 1, "violation current edge")
        full = session.send("info")
        require("steps in total." in full and
                re.findall(r"([0-9a-f]{16}) => ([0-9a-f]{16})", full)[-1:] == edge,
                "explicit history restored after violation")
        for _ in range(3):
            require("Loaded state" in session.send("load " + edge[0][0]), "violation replay restore")
            session.send("si 0")
            require(re.findall(r"([0-9a-f]{16}) => ([0-9a-f]{16})", session.send("info current")) == edge,
                    "violation replay identity")
        return {"transitions": sorted(transitions.items()), "replay": "violation_3x"}
    finally:
        status = session.close()
        if sys.exc_info()[0] is None:
            require(status == 0, "violation tracer cleanup")


def interrupt(args, binary, plugin, payload, directory, backend, algorithm, quiet):
    session = Session(args.tracer, [str(binary)], plugin, directory, backend, args.timeout,
                      choices=True, fsmap=[{"Host": str(payload), "Target": "/payload0"}])
    sent = threading.Event()
    def stop_at_edge():
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline and session.proc.poll() is None:
            if "HISTORY_INTERRUPT" in session.log.read_text():
                os.kill(session.proc.pid, signal.SIGINT); sent.set(); return
            time.sleep(.005)
    stopper = None
    try:
        session.send(); session.send("sw 0")
        stopper = threading.Thread(target=stop_at_edge, daemon=True); stopper.start()
        reply = session.send(("bfs" if algorithm == "bfs" else "dfs 120") + (" --quiet" if quiet else ""))
        require(sent.is_set() and "Interrupt" in reply and "Complete explore all" not in reply,
                "search interrupt observed")
        require(("steps in total." in reply) != quiet, "interrupt history quiet policy")
        require("steps in total." in session.send("info"), "explicit history restored after interrupt")
        require("=>" in session.send("info current"), "current edge after interrupt")
        return {"interrupted": True, "explicit_history": True}
    finally:
        status = session.close()
        if stopper: stopper.join(timeout=1)
        if sys.exc_info()[0] is None:
            require(status == 0, "interrupt tracer cleanup")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tracer", type=Path, default=Path(os.environ.get("DETSIM_TRACER", ROOT / "tracer")))
    parser.add_argument("--backends", nargs="+", default=["full", "pages"])
    parser.add_argument("--output", type=Path)
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args(); args.tracer = args.tracer.resolve()
    args.output = (args.output or Path(tempfile.mkdtemp(prefix="history-presentation-", dir=ROOT / "build"))).resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    source = args.output / "probe.c"; source.write_text(GUEST)
    binary = args.output / "probe"
    result = command(shlex.split(os.environ.get("CC", "/usr/bin/gcc")) +
                     ["-O0", "-g", "-gdwarf-4", "-fno-pie", "-no-pie", str(source), "-o", str(binary)], ROOT)
    require(result.returncode == 0, result.stdout)
    source = args.output / "checker.cpp"; source.write_text(PLUGIN)
    plugin = args.output / "checker.so"
    build_plugin(source, plugin, shlex.split(os.environ.get("CXX", "/usr/bin/g++")))
    violation_source = args.output / "violation.cpp"
    violation_source.write_text(PLUGIN.replace("    return save(m, w);",
        "    if (m.steps == 2) { save(m, w); return DETSIM_VIOLATION; }\n    return save(m, w);"))
    violation_plugin = args.output / "violation.so"
    build_plugin(violation_source, violation_plugin, shlex.split(os.environ.get("CXX", "/usr/bin/g++")))
    interrupt_source = args.output / "interrupt.cpp"
    interrupt_source.write_text("#include <unistd.h>\n" + PLUGIN.replace("    return save(m, w);",
        '    if (m.steps == 10) { std::fprintf(stderr, "HISTORY_INTERRUPT\\n"); usleep(250000); }\n    return save(m, w);'))
    interrupt_plugin = args.output / "interrupt.so"
    build_plugin(interrupt_source, interrupt_plugin, shlex.split(os.environ.get("CXX", "/usr/bin/g++")))
    payload = args.output / "payload"; payload.write_bytes(b"a" * 4096)
    report = {}
    try:
        for backend in args.backends:
            for algorithm in ("bfs", "dfs"):
                for quiet in (False, True):
                    key = f"{backend}-{algorithm}-{'quiet' if quiet else 'normal'}"
                    print(key, flush=True)
                    report[key] = search(args, binary, plugin, payload, args.output / key, backend, algorithm, quiet)
                    require(report[key] == next(iter(report.values())), "quiet/backend/algorithm changed transitions: " + key)
        expected_violation = None
        for backend in args.backends:
            for algorithm in ("bfs", "dfs"):
                for quiet in (False, True):
                    key = f"{backend}-{algorithm}-violation-{'quiet' if quiet else 'normal'}"
                    print(key, flush=True)
                    report[key] = violation(args, binary, violation_plugin, payload, args.output / key,
                                            backend, algorithm, quiet)
                    if expected_violation is None: expected_violation = report[key]
                    require(report[key] == expected_violation, "quiet changed violation transitions: " + key)
        for backend in args.backends:
            for algorithm in ("bfs", "dfs"):
                for quiet in (False, True):
                    key = f"{backend}-{algorithm}-interrupt-{'quiet' if quiet else 'normal'}"
                    print(key, flush=True)
                    report[key] = interrupt(args, binary, interrupt_plugin, payload, args.output / key,
                                            backend, algorithm, quiet)
        print(f"PASS: {len(report)} finite/violation/interrupt searches; equivalent transitions and replay 3x")
    finally:
        (args.output / "report.json").write_text(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
