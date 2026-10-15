"""Real NP=1 tracer scenarios; each run owns a temporary directory and session."""

import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]
TRACER = Path(os.environ.get("DETSIM_TRACER", ROOT / "tracer")).resolve()
PROFILE = "--profile" in sys.argv
if PROFILE:
    sys.argv.remove("--profile")


class TracerScenarios(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ds-scenario-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        source = self.root / "probe.c"
        source.write_text(
            '#include <unistd.h>\n'
            'volatile int counter = 0;\n'
            'int main(void) { for (int i=0; i<3; ++i) {\n'
            '  ++counter; write(1, "tick\\n", 5);\n'
            '} return 0; }\n'
        )
        subprocess.run(
            shlex.split(os.environ.get("CC", "/usr/bin/gcc"))
            + ["-O0", "-g", "-gdwarf-4", "-fno-pie", "-no-pie",
               str(source), "-o", str(self.root / "probe")], check=True,
        )

    def run_tracer(self, commands, *, name="probe", batch=False, sibling=False,
                   terminate=False, assertions=(), verify_failure_cleanup=False,
                   fail_storage_sync=False, extra_env=None, plugins=(), tracee_args=("0",), runtime_profile=None, fsmap=()):
        work = self.root / (name + ("-batch" if batch else "-interactive"))
        work.mkdir()
        binary = work / name
        binary.write_bytes((self.root / "probe").read_bytes())
        binary.chmod(0o755)
        config = work / "config.json"
        config.write_text(json.dumps({
            "Nodes": 1, "Loglevel": 4, "Tracee": [[str(binary), *tracee_args]],
            "Addr": ["192.168.0.1"],
            "Assertions": list(assertions),
            "Plugins": list(plugins),
            "RuntimeProfile": runtime_profile or {},
            "FSMap": list(fsmap),
        }))
        args = [str(TRACER), "-n", "-c", str(config)]
        if PROFILE:
            if not shutil.which("strace"):
                self.skipTest("--profile requires strace")
            args = ["strace", "-e", "trace=madvise", "-o", str(work / "syscalls")] + args
        if batch:
            script = work / "commands"
            script.write_text(commands)
            args += ["-b", str(script)]
        if sibling:
            # Keep the driver alive to report a group-wide SIGTERM; the sibling
            # retains the default disposition and must survive the tracer exit.
            driver = """
import json, os, select, signal, subprocess, sys, time
from pathlib import Path
signal.signal(signal.SIGTERM, lambda *_: None)
sibling = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])
try:
    terminate = sys.argv[2] == 'term'
    tracer = subprocess.Popen(json.loads(sys.argv[1]), stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE if terminate else None,
                              stderr=subprocess.STDOUT)
    if terminate:
        data = b''
        deadline = time.monotonic() + 15
        while not data.endswith(b'(ptmc) '):
            if time.monotonic() >= deadline or tracer.poll() is not None:
                raise RuntimeError('tracer failed to initialize')
            if select.select([tracer.stdout], [], [], 0.1)[0]:
                data += os.read(tracer.stdout.fileno(), 65536)
        sys.stdout.buffer.write(data)
        sys.stdout.flush()
        children = Path(f'/proc/{tracer.pid}/task/{tracer.pid}/children').read_text().split()
        assert children, 'no tracee to test cleanup'
        tracer.terminate()
        tail, _ = tracer.communicate(timeout=15)
        sys.stdout.buffer.write(tail)
        assert tracer.returncode == 128 + signal.SIGTERM, tracer.returncode
        assert not any(Path('/proc', child).exists() for child in children), 'tracee leaked'
    else:
        tracer.communicate(sys.stdin.buffer.read(), timeout=15)
    try:
        sibling.wait(timeout=0.1)
    except subprocess.TimeoutExpired:
        print('SIBLING_SURVIVED', flush=True)
        sys.exit(0 if terminate else tracer.returncode)
    print('SIBLING_WAS_KILLED', flush=True)
    sys.exit(1)
finally:
    sibling.kill()
    sibling.wait()
"""
            args = [sys.executable, "-c", driver, json.dumps(args), "term" if terminate else "normal"]
        started = time.perf_counter()
        with (work / "output").open("wb") as output:
            proc = subprocess.Popen(args, cwd=work, start_new_session=True,
                                    stdin=subprocess.PIPE, stdout=output,
                                    stderr=subprocess.STDOUT, env=os.environ | (extra_env or {}))
            try:
                if callable(commands):
                    offset = 0

                    def prompt(command=None):
                        nonlocal offset
                        if command is not None:
                            proc.stdin.write((command + "\n").encode())
                            proc.stdin.flush()
                        deadline = time.monotonic() + 20
                        while time.monotonic() < deadline:
                            transcript = (work / "output").read_text(errors="replace")
                            reply = transcript[offset:]
                            if reply.endswith("(ptmc) "):
                                offset = len(transcript)
                                return reply
                            if proc.poll() is not None:
                                self.fail("Tracer exited before prompt:\n" + transcript[-4000:])
                            time.sleep(0.001)
                        self.fail("Tracer prompt timed out:\n" + transcript[-4000:])

                    prompt()
                    commands(prompt)
                    proc.communicate(b"q\n", timeout=20)
                else:
                    children = []
                    if verify_failure_cleanup:
                        deadline = time.monotonic() + 20
                        while not (work / "output").read_bytes().endswith(b"(ptmc) "):
                            if proc.poll() is not None or time.monotonic() > deadline:
                                self.fail("Tracer failed before scenario commands")
                            time.sleep(0.001)
                        children = Path(f"/proc/{proc.pid}/task/{proc.pid}/children").read_text().split()
                        self.assertTrue(children, "No tracees to verify cleanup")
                    if fail_storage_sync:
                        (work / "inject-sync-error").touch()
                    proc.communicate(None if batch else commands.encode(), timeout=20)
                    if verify_failure_cleanup:
                        self.assertFalse(any(Path("/proc", child).exists() for child in children),
                                         "Tracee survived checker failure")
            except subprocess.TimeoutExpired:
                self.fail("Tracer timed out: " + (work / "output").read_text(errors="replace")[-4000:])
            finally:
                # The tracer currently owns a separate process group; do not kill
                # the test runner or unrelated workloads when a scenario fails.
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                proc.communicate()
        if PROFILE:
            calls = (work / "syscalls").read_text()
            sequential = [line for line in calls.splitlines() if "MADV_SEQUENTIAL" in line]
            print(f"PROFILE: {len(sequential)} MADV_SEQUENTIAL calls, "
                  f"{sum('ENOMEM' in line for line in sequential)} ENOMEM; "
                  f"{time.perf_counter() - started:.4f}s including strace/CLI", file=sys.stderr)
        return proc.returncode, (work / "output").read_text(errors="replace")

    def test_tracee_argument_storage(self):
        source = self.root / "arguments.c"
        source.write_text(
            '#include <string.h>\n'
            'int main(int argc, char **argv) {\n'
            ' if (argc != 5 && argc != 9) return 71;\n'
            ' for (int i=1; i<argc; ++i)\n'
            '  if (strcmp(argv[i], "argument with spaces")) return 72;\n'
            ' return argv[argc] != 0; }\n'
        )
        subprocess.run(
            shlex.split(os.environ.get("CC", "/usr/bin/gcc"))
            + ["-O0", "-g", "-gdwarf-4", "-fno-pie", "-no-pie",
               str(source), "-o", str(self.root / "probe")], check=True,
        )
        for argc in (5, 9):
            with self.subTest(argc=argc):
                code, output = self.run_tracer(
                    "sw 0\ndfs 30\nq\n", name=f"arguments-{argc}",
                    tracee_args=["argument with spaces"] * (argc - 1))
                self.assertEqual(code, 0, output)
                self.assertIn("exit_group(0)", output)
                self.assertIn("Complete explore all", output)

    def test_late_bootstrap_name_search_and_replay(self):
        source = self.root / "late_comm.c"
        source.write_text(r"""
#include <sys/prctl.h>
#include <string.h>
#include <unistd.h>
volatile int name_ok;
volatile int observed;
static char comm[16];
int main(void) {
    if (prctl(PR_SET_NAME, "prelude", 0, 0, 0)) return 71;
    observed = 1;
    if (prctl(PR_GET_NAME, comm, 0, 0, 0)) return 72;
    name_ok = !memcmp(comm, "prelude", 8);
    for (int i = 8; i < 16; ++i) if (comm[i]) name_ok = 0;
    if (!name_ok) return 73;
    observed = 2;
    if (write(1, "COMM_PRELUDE_OK\n", 16) != 16) return 74;
    observed = 3;
    return 0;
}
""")
        subprocess.run(shlex.split(os.environ.get("CC", "/usr/bin/gcc")) +
            ["-O0", "-g", "-gdwarf-4", "-fno-pie", "-no-pie", str(source),
             "-o", str(self.root / "probe")], check=True)
        native = subprocess.run([str(self.root / "probe")], check=True,
                                stdout=subprocess.PIPE, timeout=5)
        self.assertEqual(native.stdout, b"COMM_PRELUDE_OK\n")

        def scenario(send):
            def edge():
                matches = re.findall(r"([0-9a-f]{16}) => ([0-9a-f]{16})", send("info current"))
                self.assertEqual(len(matches), 1)
                return matches[0]

            def marker():
                reply = send("si")
                self.assertIn("COMM_PRELUDE_OK", reply)
                self.assertRegex(reply, r", 16\) = 16")
                self.assertIn("name_ok = 1", send("p name_ok"))
                self.assertIn("observed = 2", send("p observed"))
                return edge()[1]

            send("sw 0")
            # startup_syscall157 executes the first SET_NAME in the native prelude.
            # The first controlled call must therefore be GET_NAME.
            self.assertRegex(send("si"), r"prctl\(.*= 0")
            self.assertIn("observed = 1", send("p observed"))
            predecessor, query_hash = edge()
            marker_hash = marker()
            self.assertIn("Loaded state", send("load " + predecessor))
            searched = send("bfs")
            count = re.search(r"Complete explore all (\d+) sys_states", searched)
            self.assertIsNotNone(count, searched)
            self.assertGreater(int(count[1]), 1, searched)
            self.assertNotIn("illegal state", searched)
            self.assertIn("has exited with status 0", send("si"))
            exit_hash = None
            for _ in range(3):
                self.assertIn("Loaded state", send("load " + predecessor))
                self.assertIn("name_ok = 0", send("p name_ok"))
                self.assertRegex(send("si"), r"prctl\(.*= 0")
                self.assertEqual(edge()[1], query_hash, "GET_NAME replay changed identity")
                self.assertEqual(marker(), marker_hash, "name/output replay changed identity")
                self.assertIn("exit_group(0)", send("si"))
                self.assertIn("has exited with status 0", send("si"))
                current = edge()[1]
                if exit_hash is None:
                    exit_hash = current
                self.assertEqual(current, exit_hash, "completion replay changed identity")

        for backend in ("full", "pages"):
            with self.subTest(backend=backend):
                code, output = self.run_tracer(scenario, name="late_comm_" + backend,
                    tracee_args=(), runtime_profile={"startup_syscall": 157},
                    extra_env={"DETSIM_SNAPSHOT_BACKEND": backend})
                self.assertEqual(code, 0, output)

    def test_runtime_queries_restore_and_replay(self):
        source = self.root / "runtime_queries.c"
        source.write_text(r"""
#include <unistd.h>
#include <sys/syscall.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
volatile int counter;
static uint64_t mask[2];
static char path[4096];
int main(int argc, char **argv) {
    (void)argc;
    counter = 1; write(1, "start", 5);
    mask[1] = 0x12345678;
    if (syscall(SYS_sched_getaffinity, 0, sizeof(mask), mask) != 8 ||
        mask[0] != 1 || mask[1] != 0x12345678) return 71;
    counter = 2; write(1, "cpu", 3);
    long n = syscall(SYS_readlink, "/proc/self/exe", path, sizeof(path));
    if (n != (long)strlen(argv[0]) || memcmp(path, argv[0], n)) return 72;
    counter = 3; write(1, "exe", 3);
    n = syscall(SYS_readlinkat, AT_FDCWD, "/proc/self/exe", path, 3);
    if (n != 3 || memcmp(path, argv[0], 3)) return 73;
    counter = 4; write(1, "at", 2);
    return 0;
}
""")
        subprocess.run(shlex.split(os.environ.get("CC", "/usr/bin/gcc")) +
            ["-O0", "-g", "-gdwarf-4", "-fno-pie", "-no-pie", str(source),
             "-o", str(self.root / "probe")], check=True)

        def scenario(send):
            send("sw 0")
            send("si")
            self.assertIn("counter = 1", send("p counter"))
            for value in (2, 3, 4):
                saved = re.findall(r"=> ([0-9a-f]{16})", send("info"))[-1]
                send("si")  # query returns, before consuming the result
                send("si")  # application validates result and writes marker
                self.assertIn(f"counter = {value}", send("p counter"))
                expected = re.findall(r"=> ([0-9a-f]{16})", send("info"))[-1]
                for _ in range(3):
                    self.assertIn("Loaded state", send("load " + saved))
                    send("si")
                    send("si")
                    self.assertIn(f"counter = {value}", send("p counter"))
                    self.assertEqual(re.findall(r"=> ([0-9a-f]{16})", send("info"))[-1], expected)
            send("si")
            self.assertIn("has exited with status 0", send("si"))

        for backend in ("full", "pages"):
            code, output = self.run_tracer(scenario, name="queries_" + backend,
                extra_env={"DETSIM_SNAPSHOT_BACKEND": backend})
            self.assertEqual(code, 0, output)

    def test_large_virtual_file_branch_replay(self):
        source = self.root / "file_branches.c"
        source.write_text(r"""
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
volatile int observed;
static char contents[8192];
static char byte;
int main(void) {
    memset(contents, 'A', sizeof(contents));
    int fd = open("/branch-data", O_CREAT|O_RDWR, 0600);
    if (fd < 0 || write(fd, contents, sizeof(contents)) != sizeof(contents)) return 70;
    if (lseek(fd, 0, SEEK_SET) != 0 || read(fd, &byte, 1) != 1 || byte != 'A') return 71;
    if (lseek(fd, 0, SEEK_SET) != 0 || write(fd, "B", 1) != 1) return 72;
    if (lseek(fd, 0, SEEK_SET) != 0 || read(fd, &byte, 1) != 1 || byte != 'B') return 73;
    observed = byte;
    write(1, "verified", 8);
    close(fd);
    return 0;
}
""")
        subprocess.run(shlex.split(os.environ.get("CC", "/usr/bin/gcc")) +
            ["-O0", "-g", "-gdwarf-4", "-fno-pie", "-no-pie", str(source),
             "-o", str(self.root / "probe")], check=True)

        def scenario(send):
            send("sw 0")
            send("si")  # open
            self.assertIn("8192) = 8192", send("si"))
            saved = re.findall(r"=> ([0-9a-f]{16})", send("info"))[-1]
            successor = None
            for iteration in range(4):
                if iteration:
                    self.assertIn("Loaded state", send("load " + saved))
                    self.assertIn("observed = 0", send("p observed"))
                for _ in range(7):
                    send("si")
                self.assertIn("observed = 66", send("p observed"))
                actual = re.findall(r"=> ([0-9a-f]{16})", send("info"))[-1]
                if successor is None:
                    successor = actual
                self.assertEqual(actual, successor)
            send("si")  # close
            send("si")  # exit_group
            self.assertIn("has exited with status 0", send("si"))

        for backend in ("full", "pages"):
            code, output = self.run_tracer(scenario, name="file_branch_" + backend,
                extra_env={"DETSIM_SNAPSHOT_BACKEND": backend})
            self.assertEqual(code, 0, output)

    def test_access_syscall_abi(self):
        source = self.root / "access_probe.c"
        source.write_text(r"""
#include <errno.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
int main(void) {
    int fd = open("access-file", O_CREAT|O_RDWR, 0600);
    if (fd < 0 || close(fd)) return 70;
    if (syscall(SYS_access, "access-file", F_OK)) return 71;
    if (syscall(SYS_faccessat, AT_FDCWD, "access-file", F_OK, ~0UL)) return 72;
    if (syscall(SYS_faccessat2, AT_FDCWD, "access-file", F_OK, 0)) return 73;
    if (syscall(SYS_faccessat2, AT_FDCWD, (void*)0, 8, 0) != -1 || errno != EINVAL) return 74;
    if (syscall(SYS_access, "missing-file", F_OK) != -1 || errno != ENOENT) return 75;
    return 0;
}
""")
        subprocess.run(shlex.split(os.environ.get("CC", "/usr/bin/gcc")) +
            ["-O0", "-g", "-gdwarf-4", "-fno-pie", "-no-pie", str(source),
             "-o", str(self.root / "probe")], check=True)
        subprocess.run([str(self.root / "probe")], cwd=self.root, check=True, timeout=5)
        for backend in ("full", "pages"):
            code, output = self.run_tracer("sw 0\nbfs\nsi\nq\n", name="access_" + backend,
                extra_env={"DETSIM_SNAPSHOT_BACKEND": backend})
            self.assertEqual(code, 0, output)
            self.assertIn("has exited with status 0", output)

    def test_virtual_clock_origin(self):
        source = self.root / "clock_origin.c"
        source.write_text(r"""
#include <time.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    (void)argc;
    long expected = strtol(argv[1], 0, 10);
    struct timespec mono, real;
    if (syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &mono) ||
        syscall(SYS_clock_gettime, CLOCK_REALTIME, &real)) return 70;
    if (mono.tv_sec*1000000 + mono.tv_nsec/1000 != expected ||
        real.tv_sec*1000000 + real.tv_nsec/1000 != expected) return 71;
    return 0;
}
""")
        subprocess.run(shlex.split(os.environ.get("CC", "/usr/bin/gcc")) +
            ["-O0", "-g", "-gdwarf-4", "-fno-pie", "-no-pie", str(source),
             "-o", str(self.root / "probe")], check=True)
        for backend in ("full", "pages"):
            for expected, profile in ((1000000, None), (0, {"initial_time_us": 0}),
                                      (1234567, {"initial_time_us": 1234567})):
                with self.subTest(backend=backend, expected=expected):
                    code, output = self.run_tracer("sw 0\nbfs\nsi\nq\n",
                        name=f"clock_{backend}_{expected}", runtime_profile=profile,
                        tracee_args=[str(expected)], extra_env={"DETSIM_SNAPSHOT_BACKEND": backend})
                    self.assertEqual(code, 0, output)
                    self.assertIn("has exited with status 0", output)

    def test_dynamic_library_file_identity(self):
        libraries = []
        # Minimal DSOs isolate file identity: no CRT/BSS tail beyond file backing.
        # SIGBUS-backed mappings have their own explicit unsupported-policy test.
        for value in (11, 22):
            source = self.root / f"library{value}.c"
            source.write_text(f"int answer(void) {{ return {value}; }}\n")
            library = self.root / f"library{value}.so"
            subprocess.run(shlex.split(os.environ.get("CC", "/usr/bin/gcc")) +
                ["-shared", "-fPIC", "-nostartfiles", str(source), "-o", str(library)], check=True)
            libraries.append(library)
        source = self.root / "libraries.c"
        source.write_text(r"""
#include <dlfcn.h>
int main(int argc, char **argv) {
    (void)argc;
    void *one = dlopen(argv[1], RTLD_NOW|RTLD_LOCAL);
    if (!one) return 70;
    int (*first)(void) = (int (*)(void))dlsym(one, "answer");
    if (!first || first() != 11) return 71;
    void *two = dlopen(argv[2], RTLD_NOW|RTLD_LOCAL);
    if (!two) return 72;
    int (*second)(void) = (int (*)(void))dlsym(two, "answer");
    if (!second || second() != 22 || first() != 11) return 73;
    return 0;
}
""")
        subprocess.run(shlex.split(os.environ.get("CC", "/usr/bin/gcc")) +
            ["-O0", "-g", "-gdwarf-4", "-fno-pie", "-no-pie", str(source),
             "-ldl", "-o", str(self.root / "probe")], check=True)
        subprocess.run([str(self.root / "probe"), *map(str, libraries)], check=True, timeout=5)
        mappings = [{"Host": str(path), "Target": str(path)} for path in libraries]
        for backend in ("full", "pages"):
            code, output = self.run_tracer("sw 0\nbfs\nsi\nq\n", name="libraries_" + backend,
                tracee_args=list(map(str, libraries)), fsmap=mappings,
                extra_env={"DETSIM_SNAPSHOT_BACKEND": backend})
            self.assertEqual(code, 0, output)
            self.assertIn("has exited with status 0", output)

    def test_directory_cursor_restore(self):
        directory = self.root / "listing"
        directory.mkdir()
        (directory / "entry").write_text("captured input")
        source = self.root / "directory.c"
        source.write_text(r"""
#include <fcntl.h>
#include <sys/syscall.h>
#include <string.h>
#include <unistd.h>
volatile int observed;
static char bytes[64];
int main(int argc, char **argv) {
    (void)argc;
    int fd = open(argv[1], O_RDONLY|O_DIRECTORY);
    if (fd < 0) return 70;
    if (syscall(SYS_getdents64, fd, bytes, 24) != 24 || strcmp(bytes+19, ".")) return 71;
    if (syscall(SYS_getdents64, fd, bytes, 24) != 24 || strcmp(bytes+19, "..")) return 72;
    observed=2; write(1, "directory", 9);
    if (lseek(fd, 0, SEEK_SET) != 0 || syscall(SYS_getdents64, fd, bytes, 24) != 24 || strcmp(bytes+19, ".")) return 73;
    observed=3; write(1, "rewound", 7);
    close(fd); return 0;
}
""")
        subprocess.run(shlex.split(os.environ.get("CC", "/usr/bin/gcc")) +
            ["-O0", "-g", "-gdwarf-4", "-fno-pie", "-no-pie", str(source),
             "-o", str(self.root / "probe")], check=True)
        subprocess.run([str(self.root / "probe"), str(directory)], check=True, timeout=5,
                       stdout=subprocess.DEVNULL)

        def scenario(send):
            send("sw 0"); send("si")
            self.assertRegex(send("si"), r"getdents64.*= 24")
            saved = re.findall(r"=> ([0-9a-f]{16})", send("info"))[-1]
            successor = None
            for attempt in range(4):
                if attempt:
                    self.assertIn("Loaded state", send("load " + saved))
                self.assertRegex(send("si"), r"getdents64.*= 24")
                send("si")
                self.assertIn("observed = 2", send("p observed"))
                actual = re.findall(r"=> ([0-9a-f]{16})", send("info"))[-1]
                if successor is None:
                    successor = actual
                self.assertEqual(actual, successor)
            for _ in range(3):
                send("si")
            self.assertIn("observed = 3", send("p observed"))
            send("si"); send("si")
            self.assertIn("has exited with status 0", send("si"))
        for backend in ("full", "pages"):
            code, output = self.run_tracer(scenario, name="directory_" + backend,
                tracee_args=[str(directory)], fsmap=[{"Host": str(directory), "Target": str(directory)}],
                extra_env={"DETSIM_SNAPSHOT_BACKEND": backend})
            self.assertEqual(code, 0, output)

    def test_interactive_initial_state_and_clean_exit(self):
        code, output = self.run_tracer("sw 0\np counter\nq\n")
        self.assertRegex(output, r"(?m)^counter = 0\r?$", output)
        self.assertEqual(code, 0, output)

    def test_batch_preserves_command_arguments(self):
        code, output = self.run_tracer("# arguments and whitespace\n\t\nsw\t  0\np counter\nq\n", batch=True)
        self.assertIn("Switched to process 0", output)
        self.assertRegex(output, r"(?m)^counter = 0\r?$", output)
        self.assertEqual(code, 0, output)

    def test_program_name_does_not_change_initialization(self):
        code, output = self.run_tracer("sw 0\nsi\np counter\nq\n", name="test_probe")
        self.assertRegex(output, r"(?m)^counter = 1\r?$", output)
        self.assertEqual(code, 0, output)

    def test_shutdown_does_not_kill_unrelated_sibling(self):
        code, output = self.run_tracer("q\n", sibling=True)
        self.assertIn("SIBLING_SURVIVED", output)
        self.assertEqual(code, 0, output)

    def test_sigterm_reaps_tracees_and_preserves_sibling(self):
        code, output = self.run_tracer("", sibling=True, terminate=True)
        self.assertIn("SIBLING_SURVIVED", output)
        self.assertEqual(code, 0, output)

    def test_finite_program_exits_after_three_writes(self):
        code, output = self.run_tracer("sw 0\nsi\np counter\nsi\np counter\nsi\np counter\nsi\nsi\nq\n")
        self.assertEqual(re.findall(r"counter = (\d+)", output), ["1", "2", "3"], output)
        self.assertIn("has exited with status 0", output)
        self.assertEqual(code, 0, output)

    def pending_plugin(self):
        plugin = self.root / "pending.so"
        subprocess.run(shlex.split(os.environ.get("CXX", "/usr/bin/g++")) +
                       ["-std=gnu++20", "-shared", "-fPIC", "-I" + str(ROOT / "src"),
                        "-I" + str(ROOT / "third_party"), str(ROOT / "tests/native_pending_plugin.cpp"),
                        "-o", str(plugin)], check=True)
        return str(plugin)

    def test_fatal_stop_has_no_syscall_return_event(self):
        source = self.root / "crash_probe.c"
        source.write_text('#include <unistd.h>\nint main(void) { getpid(); __asm__ volatile("ud2"); }\n')
        subprocess.run(shlex.split(os.environ.get("CC", "/usr/bin/gcc")) +
                       ["-O0", "-g", "-gdwarf-4", "-fno-pie", "-no-pie",
                        str(source), "-o", str(self.root / "probe")], check=True)
        code, output = self.run_tracer("sw 0\nbfs\nsi\nq\n", plugins=[self.pending_plugin()])
        self.assertEqual(code,0,output)
        self.assertIn("stopped for signal",output)
        self.assertNotIn("Stopped for illegal state",output)

    def test_pipe_read_pending_completion_and_eof(self):
        source = self.root / "pipe_probe.c"
        source.write_text(r"""
#include <pthread.h>
#include <unistd.h>
static int ends[2];
volatile int counter;
static void *writer(void *unused) {
  (void)unused;
  if (write(ends[1], "xy", 2) != 2 || close(ends[1])) return (void*)1;
  return 0;
}
int main(void) {
  pthread_t t;void* result=0;char c;
  if (pipe(ends) || pthread_create(&t,0,writer,0)) return 2;
  if (read(ends[0],&c,1)!=1 || c!='x') return 3;
  ++counter;
  if (read(ends[0],&c,1)!=1 || c!='y') return 4;
  ++counter;
  if (read(ends[0],&c,1)!=0 || pthread_join(t,&result) || result) return 5;
  ++counter;return 0;
}
""")
        subprocess.run(shlex.split(os.environ.get("CC", "/usr/bin/gcc")) +
                       ["-O0", "-g", "-gdwarf-4", "-fno-pie", "-no-pie", "-pthread",
                        str(source), "-o", str(self.root / "probe")], check=True)
        subprocess.run([str(self.root / "probe")], check=True, timeout=10)
        plugin = self.pending_plugin()
        for backend in ("full", "pages"):
            code, output = self.run_tracer("sw 0\nbfs\np counter\nsi\nq\n",
                name="pipe_"+backend, plugins=[plugin], extra_env={"DETSIM_SNAPSHOT_BACKEND":backend})
            self.assertEqual(code,0,output)
            self.assertIn("counter = 3",output)
            self.assertIn("<pending:waiting>",output)
            self.assertIn("has exited with status 0",output)
            self.assertNotIn("Stopped for illegal state",output)

    def test_pthread_tls_join_owner_death_and_condition_wait(self):
        source = self.root / "pthread_probe.c"
        source.write_text(r"""
#include <pthread.h>
#include <stdint.h>
#include <errno.h>
static _Thread_local int tls = 11;
static pthread_mutex_t robust;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static int ready;
volatile int counter, recoveries;
static void *owner(void *arg) {
    if (tls != 11 || pthread_mutex_lock(&robust)) return (void *)-1;
    tls = 77 + (int)(intptr_t)arg;
    ++counter;
    return (void *)(intptr_t)tls; /* Deliberately retain robust mutex ownership. */
}
static void *notifier(void *arg) {
    (void)arg;
    if (pthread_mutex_lock(&gate)) return (void *)-1;
    ready = 1;
    ++counter;
    if (pthread_cond_signal(&condition) || pthread_mutex_unlock(&gate)) return (void *)-1;
    return 0;
}
int main(void) {
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) || pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) ||
        pthread_mutex_init(&robust, &attr) || pthread_mutexattr_destroy(&attr)) return 2;
    for (intptr_t i = 0; i < 3; ++i) {
        pthread_t thread; void *result = 0;
        if (pthread_create(&thread, 0, owner, (void *)i) || pthread_join(thread, &result)) return 3;
        if ((intptr_t)result != 77 + i || tls != 11) return 4;
        if (pthread_mutex_lock(&robust) != EOWNERDEAD) return 5;
        ++recoveries;
        if (pthread_mutex_consistent(&robust) || pthread_mutex_unlock(&robust)) return 6;
    }
    pthread_t thread; void *result = 0;
    if (pthread_mutex_lock(&gate) || pthread_create(&thread, 0, notifier, 0)) return 7;
    while (!ready) if (pthread_cond_wait(&condition, &gate)) return 8;
    if (pthread_mutex_unlock(&gate) || pthread_join(thread, &result) || result) return 9;
    return counter != 4 || recoveries != 3;
}
""")
        subprocess.run(shlex.split(os.environ.get("CC", "/usr/bin/gcc")) +
                       ["-O0", "-g", "-gdwarf-4", "-fno-pie", "-no-pie", "-pthread",
                        str(source), "-o", str(self.root / "probe")], check=True)
        subprocess.run([str(self.root / "probe")], check=True, timeout=10)
        plugin = self.pending_plugin()
        for backend in ("full", "pages"):
            for terminate in (False, True):
                with self.subTest(backend=backend, terminate=terminate):
                    owned_tasks = []
                    def scenario(send):
                        send("sw 0")
                        self.assertIn("Complete explore all", send("bfs"))
                        self.assertIn("counter = 4", send("p counter"))
                        self.assertIn("recoveries = 3", send("p recoveries"))
                        self.assertIn("has exited with status 0", send("si"))
                        listing = send("thread")
                        leader = int(re.search(r"pid=(\d+)", listing).group(1))
                        self.assertEqual(os.getpgid(leader), leader)
                        owned_tasks.extend(int(t.name) for t in Path(f"/proc/{leader}/task").iterdir())
                        self.assertGreaterEqual(len(owned_tasks), 2)
                        if terminate:
                            status = Path(f"/proc/{leader}/status").read_text()
                            tracer = int(re.search(r"(?m)^PPid:\s+(\d+)", status).group(1))
                            os.kill(tracer, signal.SIGTERM)
                    name = "pthread_" + backend + ("_term" if terminate else "_normal")
                    code, output = self.run_tracer(scenario, name=name, plugins=[plugin],
                        extra_env={"DETSIM_SNAPSHOT_BACKEND": backend})
                    self.assertEqual(code, 128 + signal.SIGTERM if terminate else 0, output)
                    self.assertTrue(owned_tasks)
                    self.assertFalse(any(Path(f"/proc/{tid}").exists() for tid in owned_tasks), output)

    def test_bfs_finishes_for_finite_program(self):
        code, output = self.run_tracer("sw 0\nbfs\nq\n")
        self.assertIn("Complete explore all 5 sys_states", output)
        self.assertNotIn("Fatal signal", output)
        self.assertEqual(code, 0, output)

    def test_bfs_reports_reachable_assertion_violation(self):
        code, output = self.run_tracer("sw 0\nbfs\nq\n", assertions=["tracee0(counter < 3)"])
        self.assertIn("Stopped for illegal state", output)
        self.assertNotIn("Complete explore all", output)
        self.assertEqual(code, 0, output)

    def test_search_modes_stop_at_first_property_violation(self):
        for mode in ("dfs", "rand"):
            code, output = self.run_tracer("sw 0\n" + mode + " 8\nq\n",
                                           name=mode + "_probe",
                                           assertions=["tracee0(counter < 2)"])
            self.assertEqual(code, 0, output)
            self.assertEqual(output.count("Stopped for illegal state"), 1, output)
            self.assertNotIn("Complete explore all", output)

    def test_restore_and_replay_preserve_counter_and_state_identity(self):
        def scenario(send):
            send("sw 0")
            send("si")
            self.assertIn("counter = 1", send("p counter"))
            first = re.findall(r"([0-9a-f]{16}) => ([0-9a-f]{16})", send("info"))
            self.assertEqual(len(first), 1)
            saved = first[-1][1]
            send("si")
            self.assertIn("counter = 2", send("p counter"))
            expected = re.findall(r"=> ([0-9a-f]{16})", send("info"))[-1]
            for _ in range(3):
                self.assertIn("Loaded state", send("load " + saved))
                self.assertIn("counter = 1", send("p counter"))
                send("si")
                self.assertIn("counter = 2", send("p counter"))
                actual = re.findall(r"=> ([0-9a-f]{16})", send("info"))[-1]
                self.assertEqual(actual, expected, "Replaying the same edge changed state identity")

        code, output = self.run_tracer(scenario)
        self.assertEqual(code, 0, output)

    def test_native_plugin_history_restore_and_violation_identity(self):
        plugin = self.root / "counter.so"
        subprocess.run(["/usr/bin/g++", "-std=c++20", "-shared", "-fPIC",
                        "-I" + str(ROOT / "src"), str(ROOT / "tests/native_counter_plugin.cpp"),
                        "-o", str(plugin)], check=True)

        def scenario(send):
            send("sw 0")
            self.assertNotIn("illegal state", send("si"))
            saved = re.findall(r"=> ([0-9a-f]{16})", send("info"))[-1]
            self.assertIn("Reach illegal state", send("si"))
            terminal = re.findall(r"=> ([0-9a-f]{16})", send("info"))[-1]
            for _ in range(2):
                self.assertIn("Loaded state", send("load " + saved))
                self.assertIn("counter = 1", send("p counter"))
                self.assertIn("Reach illegal state", send("si"))
                self.assertEqual(re.findall(r"=> ([0-9a-f]{16})", send("info"))[-1], terminal)
        code, output = self.run_tracer(scenario, plugins=[str(plugin)])
        self.assertEqual(code, 0, output)
        self.assertIn("counter reached two", output)

    def test_automatic_capture_failure_exits_cleanly(self):
        source = self.root / "unsupported.c"
        source.write_text(
            '#include <sys/mman.h>\n'
            'int main(void) { return mmap(0, 4096, PROT_READ | PROT_WRITE,\n'
            '  MAP_SHARED | MAP_ANONYMOUS, -1, 0) == MAP_FAILED; }\n'
        )
        subprocess.run(
            shlex.split(os.environ.get("CC", "/usr/bin/gcc"))
            + ["-O0", "-g", "-gdwarf-4", "-fno-pie", "-no-pie",
               str(source), "-o", str(self.root / "probe")], check=True,
        )
        for command in ("bfs", "dfs 3", "rand 3"):
            with self.subTest(command=command):
                code, output = self.run_tracer(
                    "sw 0\n" + command + "\nq\n", name=command.split()[0],
                    verify_failure_cleanup=True,
                )
                self.assertIn("Only readable private mappings or anonymous guards are supported", output)
                self.assertIn("DetSim state/execution error:", output)
                self.assertNotIn("terminate called", output)
                self.assertNotIn("Complete explore all", output)
                self.assertEqual(code, 1, output)

    def test_storage_flush_failure_is_nonzero_and_reaps_tracees(self):
        source = self.root / "fail_sync.c"
        source.write_text(
            '#include <errno.h>\n#include <unistd.h>\n#include <sys/syscall.h>\n'
            'int fsync(int fd) { if (!access("inject-sync-error", F_OK)) { errno=EIO; return -1; }'
            ' return syscall(SYS_fsync, fd); }\n'
            'int fdatasync(int fd) { if (!access("inject-sync-error", F_OK)) { errno=EIO; return -1; }'
            ' return syscall(SYS_fdatasync, fd); }\n'
        )
        library = self.root / "fail_sync.so"
        subprocess.run(shlex.split(os.environ.get("CC", "/usr/bin/gcc")) +
                       ["-shared", "-fPIC", str(source), "-o", str(library)], check=True)
        code, output = self.run_tracer(
            "q\n", verify_failure_cleanup=True, fail_storage_sync=True,
            extra_env={"LD_PRELOAD": str(library)},
        )
        self.assertEqual(code, 1, output)
        self.assertNotIn("terminate called", output)
        self.assertRegex(output, r"shutdown failed|state/execution error")

    def test_simd_registers_restore_with_memory(self):
        subprocess.run([
            "/usr/bin/gcc", "-O0", "-g", "-gdwarf-4", "-fno-pie", "-no-pie",
            str(ROOT / "tests/simd_probe.c"), "-o", str(self.root / "probe"),
        ], check=True)

        def scenario(send):
            send("sw 0")
            send("si")
            saved = re.findall(r"=> ([0-9a-f]{16})", send("info"))[-1]
            send("si")
            self.assertIn("observed = 11", send("p observed"))
            for _ in range(3):
                self.assertIn("Loaded state", send("load " + saved))
                self.assertIn("observed = 0", send("p observed"))
                send("si")
                self.assertIn("observed = 11", send("p observed"))

        code, output = self.run_tracer(scenario)
        self.assertEqual(code, 0, output)


if __name__ == "__main__":
    unittest.main()
