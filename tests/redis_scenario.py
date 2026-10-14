"""Bounded NP=2 manual Redis SET/GET regression; no automatic exploration."""

import argparse
import json
import os
from pathlib import Path
import resource
import select
import shlex
import signal
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tracer", type=Path, default=ROOT / "tracer")
    parser.add_argument("--server", type=Path,
                        default=ROOT / "examples/redis/redis/src/redis-server")
    parser.add_argument("--output", type=Path, default=ROOT / "build/redis-check")
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--max-steps", type=int, default=1000)
    parser.add_argument("--diagnostic-strip", action="store_true",
                        help="diagnostic only: remove debug info from a copy")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    # Never reuse stores from a previous run, even when a prior run crashed.
    work = Path(tempfile.mkdtemp(prefix="run-", dir=args.output.resolve()))
    tracer, server = args.tracer.resolve(), args.server.resolve()
    result = {"passed": False, "diagnostic_strip": args.diagnostic_strip,
              "server_steps": 0, "client_steps": 0, "commands": 0,
              "workdir": str(work)}
    commands = []
    proc = None
    started = time.monotonic()
    transcript = work / "transcript.log"
    try:
        if args.timeout <= 0 or args.max_steps <= 0:
            raise ValueError("timeout and max-steps must be positive")
        for binary in (tracer, server):
            if not binary.is_file():
                raise FileNotFoundError(binary)
        if args.diagnostic_strip:
            copy = work / "redis-server-no-debug"
            subprocess.run(["objcopy", "--strip-debug", str(server), str(copy)], check=True)
            server = copy
        client = work / "client"
        subprocess.run(shlex.split(os.environ.get("CC", "/usr/bin/gcc")) + [
            "-O0", "-g", "-gdwarf-4", "-UNDEBUG", "-fno-pie", "-no-pie",
            str(ROOT / "examples/redis/client.c"), "-o", str(client),
        ], check=True)
        config = json.loads((ROOT / "examples/redis/simple_start.json").read_text())
        config.update({
            "Tracee": [[str(server), "/redis.conf"], [str(client)]],
            "FSMap": [{"Host": str(ROOT / "examples/redis/fs_root"), "Target": "/"}],
            "Loglevel": 2,
            "StateStore": {"hot_cache_mb": 32, "warm_cache_mb": 64,
                           "prefetch_window": 0},
        })
        (work / "config.json").write_text(json.dumps(config, indent=2))

        def limits():
            resource.setrlimit(resource.RLIMIT_AS, (2 * 1024**3, 2 * 1024**3))
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

        with transcript.open("wb") as log:
            proc = subprocess.Popen(
                [str(tracer), "-n", "-c", str(work / "config.json")], cwd=work,
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                start_new_session=True, env=dict(os.environ, LC_ALL="C", LANG="C"),
                preexec_fn=limits,
            )

            def prompt(command=None):
                if command is not None:
                    commands.append(command)
                    proc.stdin.write((command + "\n").encode())
                    proc.stdin.flush()
                reply = bytearray()
                while time.monotonic() - started < args.timeout:
                    if select.select([proc.stdout], [], [], 0.1)[0]:
                        chunk = os.read(proc.stdout.fileno(), 65536)
                        if not chunk:
                            raise RuntimeError(f"tracer exited before prompt ({proc.poll()})")
                        log.write(chunk)
                        log.flush()
                        reply.extend(chunk)
                        if reply.endswith(b"(ptmc) "):
                            text = reply.decode(errors="replace")
                            if "Fatal signal" in text or "stopped for signal" in text:
                                raise RuntimeError("tracee crashed: " + text[-1200:])
                            return text
                    if proc.poll() is not None:
                        raise RuntimeError(f"tracer exited ({proc.returncode})")
                raise TimeoutError("scenario wall-time limit reached")

            prompt()
            result["initialized"] = True
            prompt("sw 0")
            prompt("thread 0")
            result["server_threads"] = prompt("thread")
            for _ in range(args.max_steps):
                reply = prompt("si")
                result["server_steps"] += 1
                if "has exited" in reply or "exit_group(" in reply:
                    raise RuntimeError("Redis exited before reaching its event loop")
                if "epoll_wait(" in reply or "epoll_pwait(" in reply:
                    result["event_loop"] = True
                    break
            else:
                raise RuntimeError("Redis startup step limit reached")

            prompt("sw 1")
            prompt("thread 0")
            result["client_threads"] = prompt("thread")
            # Each edge is deliberately chosen; do not run bfs/dfs/rand.
            for _ in range(args.max_steps):
                prompt("sw 1")
                reply = prompt("si")
                result["client_steps"] += 1
                if "exit_group(0)" in reply:
                    # Verify the terminal status too, rather than trusting formatting.
                    if "has exited with status 0" not in prompt("si"):
                        raise RuntimeError("client exit state was not preserved")
                    result["client_exit"] = 0
                    break
                if "has exited" in reply or "exit_group(" in reply:
                    raise RuntimeError("client exited unsuccessfully: " + reply[-1200:])
                prompt("sw 0")
                reply = prompt("si")
                result["server_steps"] += 1
                if "has exited" in reply or "exit_group(" in reply:
                    raise RuntimeError("Redis exited while handling requests")
            else:
                raise RuntimeError("client request step limit reached")

            # The sample client asserts the exact RESP replies. Also retain readable
            # evidence from its final printf, even while stdout emulation is incomplete.
            text = transcript.read_text(errors="replace")
            for marker in ("SET response:", "GET response:"):
                if marker not in text:
                    raise RuntimeError("missing response evidence: " + marker)
            commands.append("q")
            tail, _ = proc.communicate(b"q\n", timeout=5)
            log.write(tail)
            result["tracer_exit"] = proc.returncode
            if proc.returncode != 0:
                raise RuntimeError("tracer did not exit normally")
            result["passed"] = True
    except Exception as exc:
        result["failure"] = str(exc)
    finally:
        if proc is not None:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            tail, _ = proc.communicate()
            if tail:
                with transcript.open("ab") as log:
                    log.write(tail)
        result["elapsed_s"] = round(time.monotonic() - started, 3)
        result["commands"] = len(commands)
        (work / "commands.txt").write_text("\n".join(commands) + "\n")
        (work / "result.json").write_text(json.dumps(result, indent=2))
        print(json.dumps(result, indent=2))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
