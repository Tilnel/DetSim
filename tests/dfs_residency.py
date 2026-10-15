#!/usr/bin/env python3
"""Finite CLI DFS residency gate, under the existing 2 GiB address-space limit."""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "examples" / "cases"))
from run import Session, build_plugin, command, require

GUEST = r'''
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
int main(void) {
    int bits[2] = {0, 0};
    int fd = open("/payload0", O_RDWR);
    if (fd < 0) return 2;
    for (int i = 0; i < 44; ++i) syscall(SYS_getpid);
    {
        struct timespec base;
        if (syscall(SYS_clock_gettime, CLOCK_REALTIME, &base)) return 3;
        long long previous = (long long)base.tv_sec * 1000000 + base.tv_nsec / 1000;
        for (int i = 0; i < 2; ++i) {
            struct timeval now;
            if (syscall(SYS_gettimeofday, &now, 0)) return 4;
            long long current = (long long)now.tv_sec * 1000000 + now.tv_usec;
            if (current - previous < 0 || current - previous > 1) return 5;
            bits[i] = current - previous; previous = current;
        }
    }
    char old = 0;
    if (read(fd, &old, 1) != 1 || old != 'a') return 6;
    char marker = '0' + bits[0] * 2 + bits[1];
    if (lseek(fd, 0, SEEK_SET) != 0 || write(fd, &marker, 1) != 1 ||
        lseek(fd, 0, SEEK_SET) != 0 || read(fd, &old, 1) != 1 || old != marker) return 7;
    char output[] = "DFS_OK 00\n";
    output[7] += bits[0]; output[8] += bits[1];
    if (write(1, output, sizeof(output)-1) != sizeof(output)-1) return 8;
    return 0;
}
'''
PLUGIN = r'''
#include "api/plugin.h"
#include <cstdio>
#include <cstring>
#include <sys/syscall.h>
struct Monitor { unsigned steps = 0, choices = 0, bits = 0, output = 0; };
static detsim_verdict fail(detsim_diagnostic d) {
    if (d.data && d.capacity) std::snprintf(d.data, d.capacity, "DFS branch oracle failed");
    return DETSIM_ERROR;
}
static detsim_verdict save(Monitor m, detsim_writer w) {
    return w.append && !w.append(w.context, &m, sizeof m) ? DETSIM_PASS : DETSIM_ERROR;
}
static detsim_verdict init(const detsim_host_v2 *, const detsim_state_view *, const char *,
                           detsim_writer w, detsim_diagnostic) { return save({}, w); }
static detsim_verdict advance(const detsim_host_v2 *h, const detsim_state_view *,
    const detsim_state_view *after, const detsim_event_v2 *e, detsim_bytes previous,
    detsim_writer w, detsim_diagnostic d) {
    if (previous.size != sizeof(Monitor)) return fail(d);
    Monitor m; std::memcpy(&m, previous.data, sizeof m);
    if (e->phase == DETSIM_SYSCALL_COMPLETED && e->result_valid) {
        if (e->syscall_number == SYS_getpid) ++m.steps;
        if (e->syscall_number == SYS_gettimeofday) {
            if (m.steps != 44 || m.choices >= 2 || e->choice < 0 || e->choice > 1 || e->result)
                return fail(d);
            m.bits = m.bits * 2 + e->choice; ++m.choices;
        }
        if (e->syscall_number == SYS_write && e->args[0] == 1) {
            char expected[] = "DFS_OK 00\n", output[sizeof expected]{};
            expected[7] += m.bits / 2; expected[8] += m.bits % 2;
            size_t copied = 0;
            detsim_buffer_selector selector{DETSIM_BUFFER_FILE, 1, 0, 0, 0};
            if (m.choices != 2 || m.output || e->result != sizeof(expected)-1 ||
                h->read_buffer(after, 0, &selector, output, sizeof(output)-1, &copied) ||
                copied != sizeof(expected)-1 || std::strcmp(output, expected)) return fail(d);
            m.output = 1;
        }
    }
    if (e->phase == DETSIM_THREAD_EXITED) {
        if (m.steps != 44 || m.choices != 2 || !m.output || e->args[0] ||
            (e->syscall_number != SYS_exit && e->syscall_number != SYS_exit_group)) return fail(d);
        // Observation only; semantic history remains in the branch-owned monitor.
        std::fprintf(stderr, "DFS_TERMINAL %u\n", m.bits);
    }
    std::fprintf(stderr, "DFS_EDGE %d %u %d %lld\n", e->syscall_number, e->phase, e->choice, (long long)e->result);
    return save(m, w);
}
static detsim_verdict choose(const detsim_host_v2 *, const detsim_state_view *, uint32_t,
    int32_t choice, detsim_time now, uint32_t, detsim_time_choice_v2 *out, detsim_diagnostic d) {
    if (choice < 0 || choice > 1) return fail(d);
    out->value = now; out->budget_cost = 0;
    if (choice && ++out->value.microseconds == 1000000) {
        ++out->value.seconds; out->value.microseconds = 0;
    }
    return DETSIM_PASS;
}
static const detsim_plugin_v2 plugin{DETSIM_PLUGIN_ABI_V2, sizeof(detsim_plugin_v2),
    "tests.dfs-residency", 1, nullptr, nullptr, init, advance, choose, nullptr};
extern "C" const detsim_plugin_v2 *detsim_plugin_entry_v2() { return &plugin; }
'''


def search(args, binary, plugin, files, directory, backend, algorithm):
    session = Session(args.tracer, [str(binary)], plugin, directory, backend,
                      args.timeout, choices=True,
                      fsmap=[{"Host": str(path), "Target": f"/payload{i}"}
                             for i, path in enumerate(files)])
    try:
        session.send(); session.send("sw 0")
        reply = session.send("bfs" if algorithm == "bfs" else "dfs 120")
        require("Complete explore all" in reply, reply[-2000:])
        count = re.search(r"all (\d+) sys_states \(new: (\d+)\), total unique: (\d+)", reply)
        require(count, "missing search counts")
        branches = re.findall(r"DFS_TERMINAL (\d+)", session.log.read_text())
        require(sorted(branches) == ["0", "1", "2", "3"], f"terminal branches: {branches}")
        edges_observed = sorted(re.findall(r"DFS_EDGE ([^\n]+)", session.log.read_text()))
        require(len(edges_observed) == 80 and int(count[3]) == 81,
                "finite fixture did not explore the expected 80 edges / 81 states")
        info = session.send("info")
        edges = re.findall(r"([0-9a-f]{16}) => ([0-9a-f]{16})", info)
        require(edges, "missing terminal edge")
        mutations = re.findall(r"([0-9a-f]{16}) => ([0-9a-f]{16})\n Tracee 0: write\(3,", info)
        require(len(mutations) == 1, "missing file mutation edge")
        for predecessor, successor in (mutations[0], edges[-1]):
            for _ in range(3):
                require("Loaded state" in session.send("load " + predecessor), "cannot restore replay predecessor")
                session.send("si 0")
                require(session.current_hash() == successor, "mutation/terminal replay identity drift")
        return {"searched": int(count[1]), "new": int(count[2]), "unique": int(count[3]), "branches": branches, "transitions": edges_observed,
                "replay": "mutation_and_terminal_hash_match_3x", "artifacts": str(directory)}
    finally:
        code = session.close()
        if sys.exc_info()[0] is None:
            require(code == 0, "tracer cleanup failed: " + str(directory))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tracer", type=Path, default=Path(os.environ.get("DETSIM_TRACER", ROOT / "tracer")))
    parser.add_argument("--backends", nargs="+", default=["full", "pages"])
    parser.add_argument("--output", type=Path)
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args(); args.tracer = args.tracer.resolve()
    args.output = (args.output or Path(tempfile.mkdtemp(prefix="dfs-residency-", dir=ROOT / "build"))).resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    source = args.output / "probe.c"; source.write_text(GUEST)
    binary = args.output / "probe"
    result = command(shlex.split(os.environ.get("CC", "/usr/bin/gcc")) +
                     ["-O0", "-g", "-gdwarf-4", "-fno-pie", "-no-pie", str(source), "-o", str(binary)], ROOT)
    require(result.returncode == 0, result.stdout)
    source = args.output / "checker.cpp"; source.write_text(PLUGIN)
    plugin = args.output / "checker.so"
    build_plugin(source, plugin, shlex.split(os.environ.get("CXX", "/usr/bin/g++")))
    report = {}
    try:
        for backend in args.backends:
            for size, label in ((4096, "small"), (16 * 1024**2, "large")):
                files = []
                for i in range(4):
                    path = args.output / f"{label}-{i}"
                    path.write_bytes(bytes([ord('a') + i]) * size); files.append(path)
                for algorithm in ("bfs", "dfs"):
                    key = f"{backend}-{label}-{algorithm}"
                    print(key, flush=True)
                    report[key] = search(args, binary, plugin, files, args.output / key, backend, algorithm)
                    reference = report[f"{backend}-small-bfs"]
                    require((report[key]["unique"], report[key]["transitions"]) ==
                            (reference["unique"], reference["transitions"]), "search counts changed: " + key)
        print(f"PASS: {len(report)} searches; report: {args.output / 'report.json'}")
    finally:
        (args.output / "report.json").write_text(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
