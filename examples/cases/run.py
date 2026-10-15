#!/usr/bin/env python3
"""Case-driven DetSim checks. Every search gets a fresh tracer and store."""
import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import re
import resource
import shlex
import shutil
import signal
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
ENV = {"LC_ALL": "C", "LANG": "C", "PYTHONHASHSEED": "0"}


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def limits():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    resource.setrlimit(resource.RLIMIT_AS, (2 * 1024**3, 2 * 1024**3))


def command(args, cwd, timeout=30):
    argv = [str(a) for a in args]
    proc = subprocess.Popen(argv,cwd=cwd,env=os.environ | ENV,stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT,text=True,start_new_session=True)
    try:
        output,_ = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        try: os.killpg(proc.pid,signal.SIGKILL)
        except ProcessLookupError: pass
        proc.communicate()
        raise
    return subprocess.CompletedProcess(argv,proc.returncode,output)



class CaseFailure(RuntimeError):
    pass


class Session:
    def __init__(self, tracer, argv, plugin, directory, backend, timeout, choices=False, fsmap=()):
        self.directory = directory
        directory.mkdir(parents=True)
        self.timeout = timeout
        self.started = time.monotonic()
        self.offset = 0
        self.commands = []
        self.log = directory / "transcript.log"
        config = {"Nodes": 1, "Loglevel": 4, "Tracee": [argv], "Addr": ["127.0.0.1"],
                  "RuntimeProfile": {"startup_syscall": 11}, "Plugins": [str(p) for p in plugin] if isinstance(plugin,(list,tuple)) else [str(plugin)],
                  "FSMap": list(fsmap), "StateStore": {"hot_cache_mb": 32, "warm_cache_mb": 64}}
        if choices:
            config["ChoosePoint"] = [{"syscall": 96, "choose": 2}]
        (directory / "config.json").write_text(json.dumps(config, indent=2))
        self.output = self.log.open("wb")
        self.proc = subprocess.Popen([str(tracer), "-n", "-c", str(directory / "config.json")],
            cwd=directory, stdin=subprocess.PIPE, stdout=self.output, stderr=subprocess.STDOUT,
            env=os.environ | ENV | {"DETSIM_SNAPSHOT_BACKEND": backend}, start_new_session=True,
            preexec_fn=limits)

    def send(self, text=None):
        if text is not None:
            self.commands.append(text)
            try:
                self.proc.stdin.write((text + "\n").encode())
                self.proc.stdin.flush()
            except BrokenPipeError:
                pass
        deadline = self.started + self.timeout
        while time.monotonic() < deadline:
            content = self.log.read_text(errors="replace")
            reply = content[self.offset:]
            if reply.endswith("(ptmc) "):
                self.offset = len(content)
                return reply
            if self.proc.poll() is not None:
                raise CaseFailure(reply[-4000:] or f"tracer exited {self.proc.returncode}")
            time.sleep(.005)
        raise TimeoutError(f"command deadline: {text}")

    def close(self):
        try:
            if self.proc.poll() is None:
                self.proc.communicate(b"q\n", timeout=5)
        except subprocess.TimeoutExpired:
            pass
        finally:
            if self.proc.poll() is None:
                os.killpg(self.proc.pid, signal.SIGKILL)
            self.proc.wait()
            self.output.close()
            (self.directory / "commands.json").write_text(json.dumps(self.commands, indent=2))
        return self.proc.returncode

    def current_hash(self):
        edges = re.findall(r"([0-9a-f]{16}) => ([0-9a-f]{16})", self.send("info"))
        return edges[-1][1] if edges else None


def classify_error(error, log=None):
    text = str(error)
    if log and log.exists():
        text = log.read_text(errors="replace") + "\n" + text
    failure = re.findall(r"DetSim state/execution error: ([^\n]+)", text)
    violations = re.findall(r"([^\n]*: property violation: [^\n]+)", text)
    if isinstance(error, (TimeoutError, subprocess.TimeoutExpired)):
        status = "timeout"
    elif failure:
        status = "unsupported" if re.search(r"unsupported|unmodeled|not modeled|exceeds model limit|only .+ supported|startup boundary must precede",
                                               failure[-1], re.IGNORECASE) else "internal_error"
    else:
        status = "oracle_failed" if violations else "failed"
    return {"status": status, "diagnostic": failure[-1] if failure else violations[-1] if violations else text[-2000:]}



def require(condition, reason):
    if not condition:
        raise CaseFailure(reason)


def build_plugin(source, destination, cxx):
    result = command(cxx + ["-std=gnu++20", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
                           "-I" + str(ROOT / "src"), "-I" + str(ROOT / "third_party"), str(source), "-o", str(destination)], ROOT)
    require(result.returncode == 0, result.stdout)


def manual_path(tracer, binary, plugin, directory, backend, bits, expected_bug, timeout):
    row = {"inputs": bits, "expected": "violation" if expected_bug else "pass",
           "artifacts": str(directory), "replay": "not_applicable"}
    session = Session(tracer, [str(binary)], plugin, directory, backend, timeout, choices=True)
    try:
        session.send(); session.send("sw 0")
        seen = 0
        for _ in range(80):
            before = session.current_hash()
            action = "si " + (bits[seen] if seen < 2 else "0")
            reply = session.send(action)
            if "gettimeofday(" in reply:
                seen += 1
            if "Reach illegal state" in reply:
                require(expected_bug and seen == 2 and before, "unexpected/vacuous violation")
                witness = session.current_hash()
                actions = list(session.commands)
                for _ in range(3):
                    require("Loaded state" in session.send("load " + before), "missing predecessor")
                    require("Reach illegal state" in session.send(action), "violation did not replay")
                    require(session.current_hash() == witness, "violation successor identity drift")
                row.update(status="violation", replay="same_process_hash_match_3x", witness=witness,
                           predecessor=before, action=action)
                (directory / "witness.json").write_text(json.dumps({"inputs": bits, "commands": actions,
                    "predecessor": before, "successor": witness, "replay_action": action,
                    "scope": "hashes local to this tracee; command/choice path is the restart recipe"}, indent=2))
                return row
            if "exit_group(" in reply or "has exited" in reply:
                require(not expected_bug and seen == 2, "mutation escaped checker or inputs missing")
                require("has exited with status 0" in session.send("si 0"), "guest did not exit zero")
                row.update(status="pass", guest_exit=0)
                return row
        raise CaseFailure("finite input path did not terminate within 80 syscall steps")
    except Exception as error:
        row.update(classify_error(error, session.log)); return row
    finally:
        code = session.close()
        row["tracer_exit"] = code
        if code != 0 and row.get("status") in ("pass", "violation"):
            row.update(status="failed", diagnostic="tracer cleanup failed")


def search(tracer, argv, plugin, directory, backend, timeout, *, bug=False, depth=None, fsmap=(), expected_diagnostic="account overdraft"):
    row = {"artifacts": str(directory), "startup": "not_started", "search": "not_started"}
    session = Session(tracer, argv, plugin, directory, backend, timeout, choices=depth is None, fsmap=fsmap)
    try:
        session.send(); row["startup"] = "admitted"
        session.send("sw 0")
        row["search"] = "started"
        reply = session.send("bfs" if depth is None else f"dfs {depth}")
        numbers = re.search(r"(?:all |Searched for )(\d+) sys_states(?: \(new: (\d+)\))?", reply)
        if numbers:
            row["visited"] = int(numbers[1])
            if numbers[2]: row["new"] = int(numbers[2])
        if "Stopped for illegal state" in reply:
            require(bug and expected_diagnostic in session.log.read_text(), "unexpected property violation")
            edges=re.findall(r"([0-9a-f]{16}) => ([0-9a-f]{16})",session.send("info"))
            require(edges,"violation lacks predecessor edge")
            predecessor,witness=edges[-1]
            recipe=list(session.commands)
            for _ in range(3):
                require("Loaded state" in session.send("load "+predecessor),"search witness predecessor unavailable")
                require("Reach illegal state" in session.send("si 0"),"search witness failed replay")
                require(session.current_hash()==witness,"search witness identity drift")
            row.update(status="violation",search="violation_found",replay="same_process_hash_match_3x",
                       predecessor=predecessor,witness=witness)
            (directory/"witness.json").write_text(json.dumps({"commands":recipe,"predecessor":predecessor,
                "successor":witness,"replay_action":"si 0","diagnostic":expected_diagnostic},indent=2))
        else:
            require("Complete explore all" in reply and not bug, "expected search result absent")
            history = session.send("info")
            operations = re.findall(r"(?m)^ Tracee \d+: (.+)$", history)
            terminal_candidate = bool(operations and "exit_group(" in operations[-1])
            if depth is not None and not terminal_candidate:
                row.update(status="bounded",search="bounded_no_violation",guest_exit=None)
            else:
                terminal = session.send("si 0")
                require("has exited with status 0" in terminal,"search finished but guest success not established")
                row.update(status="pass",guest_exit=0,
                           search="finite_fixture_exhausted" if depth is None else "bounded_guest_completed")
        return row
    except Exception as error:
        row.update(classify_error(error, session.log)); return row
    finally:
        code = session.close(); row["tracer_exit"] = code
        if code != 0 and row.get("status") in ("pass", "violation", "bounded"):
            row.update(status="failed", diagnostic="tracer cleanup failed")


def inputs(args, out, cc, cxx):
    plugin = out / "input_checker.so"
    build_plugin(HERE / "input_checker.cpp", plugin, cxx)
    rows = []
    for language, source, compiler in (("c", "native_inputs.c", cc), ("cpp", "native_inputs.cpp", cxx)):
        for variant in ("correct", "bug"):
            binary = out / f"{language}-{variant}"
            build = compiler + ["-O0", "-g", "-gdwarf-4", "-fno-pie", "-no-pie"]
            if variant == "bug": build += ["-DINJECT_BUG"]
            build += [str(HERE / source), "-o", str(binary)]
            result = command(build, ROOT); require(result.returncode == 0, result.stdout)
            oracle = []
            for pair in itertools.product("01", repeat=2):
                bits = "".join(pair); a, b = map(int, bits)
                balance = 1 - a - b if variant == "bug" else max(0, 1 - a - b)
                expected = f"CASE_{'BUG' if balance < 0 else 'OK'} {bits} {balance}\n"
                native = command([binary, bits], out)
                require(native.returncode == 0 and native.stdout == expected, "native oracle mismatch")
                oracle.append({"inputs": bits, "output": expected.strip()})
            for backend in args.backends:
                work = out / f"{language}-{variant}-{backend}"
                row = {"case": "two-debits", "language": language, "variant": variant, "backend": backend,
                       "binary_sha256": digest(binary), "source_sha256": digest(HERE / source),
                       "plugin_sha256": digest(plugin), "compile": build, "native_oracle": oracle}
                row["paths"] = [manual_path(args.tracer, binary, plugin, work / bits, backend, bits,
                    variant == "bug" and bits == "11", args.timeout) for bits in ("00", "01", "10", "11")]
                row["search"] = search(args.tracer, [str(binary)], plugin, work / "search", backend,
                                       args.timeout, bug=variant == "bug")
                row["toolchain"] = command(compiler+["--version"],ROOT).stdout.splitlines()[0]
                row["passed"] = all(p["status"] == ("violation" if variant == "bug" and p["inputs"] == "11" else "pass")
                                    for p in row["paths"]) and row["search"]["status"] == ("violation" if variant == "bug" else "pass")
                rows.append(row)
                print(json.dumps({k: row[k] for k in ("case", "language", "variant", "backend", "passed")}), flush=True)
    return rows


def threads(args,out,cc,cxx):
    oracle=out/"admission_checker.so"; deadlock=out/"deadlock_checker.so"
    build_plugin(HERE/"admission_checker.cpp",oracle,cxx)
    build_plugin(HERE/"deadlock_checker.cpp",deadlock,cxx)
    rows=[]
    for variant in ("correct","bug"):
        binary=out/("lost-notification-"+variant)
        build=cc+["-O0","-g","-gdwarf-4","-fno-pie","-no-pie","-pthread"]
        if variant=="bug":build += ["-DINJECT_BUG"]
        build += [str(HERE/"lost_notification.c"),"-o",str(binary)]
        compiled=command(build,ROOT);require(compiled.returncode==0,compiled.stdout)
        try:
            native=command([binary],out,1 if variant=="bug" else args.timeout)
            require(variant=="correct" and native.returncode==0 and "RUNTIME_OK\n" in native.stdout,
                    "unexpected native notification result")
            native_status="pass"
        except subprocess.TimeoutExpired:
            require(variant=="bug","healthy native notification timed out")
            native_status="expected_timeout_not_a_deadlock_proof"
        for backend in args.backends:
            result=search(args.tracer,[str(binary)],[oracle,deadlock],out/("thread-"+variant+"-"+backend),
                backend,args.timeout,bug=variant=="bug",depth=args.depth,
                expected_diagnostic="closed-world lost notification")
            row={"case":"lost-notification","language":"c-pthread","variant":variant,"backend":backend,
                 "compile":build,"native":native_status,"binary_sha256":digest(binary),
                 "source_sha256":digest(HERE/"lost_notification.c"),
                 "plugin_sha256":[digest(oracle),digest(deadlock)],"model":result,
                 "scope":"closed-world untimed futex fixture under current actor policy, not exhaustive scheduling"}
            row["passed"]=result["status"]==("violation" if variant=="bug" else "pass")
            rows.append(row)
            print(json.dumps({k:row[k] for k in ("case","variant","backend","passed")}),flush=True)
    return rows


def admission_controls(tracer, argv, plugin, directory, backend, timeout, fsmap):
    row = {"startup": "not_started", "controlled_steps": 0, "restore": "not_run", "artifacts": str(directory)}
    session = Session(tracer, argv, plugin, directory, backend, timeout, fsmap=fsmap)
    try:
        session.send();row["startup"]="admitted"
        session.send("sw 0")
        first=session.send("si 0")
        require(not any(word in first for word in ("Reach illegal state","has exited","is blocked","stopped for signal")),
                "admission first command did not execute a supported edge")
        require(re.search(r"(?m)^(?:\[TID:\d+\] )?[a-z][a-z0-9_]*\(",first),"first step has no syscall evidence")
        row["controlled_steps"]+=1
        predecessor=session.current_hash()
        second=session.send("si 0")
        require(not any(word in second for word in ("Reach illegal state","has exited","is blocked","stopped for signal")),
                "admission second command did not execute a supported edge")
        require(re.search(r"(?m)^(?:\[TID:\d+\] )?[a-z][a-z0-9_]*\(",second),"second step has no syscall evidence")
        row["controlled_steps"]+=1
        successor=session.current_hash()
        require(predecessor and successor and predecessor!=successor,"missing or no-op admission edge identity")
        require("Loaded state" in session.send("load "+predecessor),"admission predecessor unavailable")
        replay=session.send("si 0")
        require("Reach illegal state" not in replay and session.current_hash()==successor,
                "admission edge replay changed state identity")
        row.update(status="pass",restore="same_process_hash_match",predecessor=predecessor,successor=successor)
        return row
    except Exception as error:
        row.update(classify_error(error,session.log));return row
    finally:
        row["tracer_exit"]=session.close()
        if row["tracer_exit"] and row.get("status")=="pass":row.update(status="failed",diagnostic="cleanup failed")


def runtimes(args, out, cc, cxx):
    plugin = out / "admission_checker.so"
    build_plugin(HERE / "admission_checker.cpp", plugin, cxx)
    rows = []
    for requested in args.languages.split(","):
        language = {"rust":"rs", "python":"py", "c++":"cpp"}.get(requested,requested)
        row = {"case": "runtime-admission", "language": {"rs":"rust","py":"python"}.get(language,language), "native": "not_run",
               "plugin_sha256":digest(plugin), "plugin_source_sha256":digest(HERE / "admission_checker.cpp")}
        try:
            source = HERE / "probes" / ("RuntimeProbe.java" if language == "java" else "runtime." + language)
            binary = out / ("runtime-" + language)
            version_args = ["--version"]
            if language in ("c", "cpp"):
                tool = cc if language == "c" else cxx
                build = tool + ["-O0", "-g", "-gdwarf-4", "-fno-pie", "-no-pie", str(source), "-o", str(binary)]
                argv = [str(binary)]
            elif language == "rs":
                tool = [shutil.which("rustc") or "rustc"]
                build = tool + ["-g", "-C", "relocation-model=static", "-C", "linker=" + cc[0], str(source), "-o", str(binary)]
                argv = [str(binary)]
            elif language == "go":
                tool = [shutil.which("go") or "go"]; version_args = ["version"]
                build = tool + ["build", "-o", str(binary), str(source)]; argv = [str(binary)]
            elif language == "py":
                tool = [os.environ.get("CASE_PYTHON", "/usr/bin/python3")]
                build = None; argv = tool + ["-I", "-S", str(source)]
            elif language == "java":
                tool = [shutil.which("java") or "java"]; version_args = ["-version"]
                build = [shutil.which("javac") or "javac", "-g", "-d", str(out), str(source)]
                argv = tool + ["-Xms16m", "-Xmx64m", "-XX:ReservedCodeCacheSize=32m",
                               "-XX:CompressedClassSpaceSize=64m", "-cp", str(out), "RuntimeProbe"]
            else: raise CaseFailure("unknown language: " + language)
            version = command(tool + version_args, out)
            require(version.returncode == 0, version.stdout)
            row.update(toolchain=version.stdout.strip(), runtime_implementation=("gccgo" if "gccgo" in version.stdout else "gc") if language=="go" else language, command=argv, compile=build,
                       source_sha256=digest(source))
            if build:
                built = command(build, ROOT, 60); require(built.returncode == 0, built.stdout)
            native = command(argv, out, args.timeout)
            row.update(native="pass" if native.returncode == 0 and "RUNTIME_OK\n" in native.stdout else "failed",
                       native_exit=native.returncode, native_output=native.stdout[-2000:])
            require(row["native"] == "pass", "native runtime probe failed")
            row["binary_sha256"] = digest(argv[0])
            mappings = [{"Host": str(source), "Target": str(source)}]
            if language == "java": mappings.append({"Host": str(out / "RuntimeProbe.class"), "Target": str(out / "RuntimeProbe.class")})
            for backend in args.backends:
                result = dict(row, backend=backend)
                result["control"] = admission_controls(args.tracer, argv, plugin,
                    out / ("control-"+language+"-"+backend),backend,args.timeout,mappings)
                result["model"] = search(args.tracer, argv, plugin, out / ("admission-"+language+"-"+backend),
                    backend, args.timeout, depth=args.depth, fsmap=mappings)
                result["passed"] = result["model"]["status"] == "pass" and result["control"]["status"]=="pass"
                rows.append(result)
                print(json.dumps({"case": "runtime-admission", "language": result["language"], "backend": backend,
                    "passed": result["passed"], "control": result["control"], "model": result["model"]}), flush=True)
        except FileNotFoundError as error:
            rows.append(dict(row, passed=False, status="toolchain_unavailable", diagnostic=str(error)))
        except Exception as error:
            rows.append(dict(row, passed=False, **classify_error(error)))
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("inputs", "threads", "runtimes", "checks", "all"), default="inputs")
    parser.add_argument("--tracer", type=Path, default=ROOT / "tracer")
    parser.add_argument("--output", type=Path, default=ROOT / "build/cases")
    parser.add_argument("--backend", choices=("full", "pages", "both"), default="both")
    parser.add_argument("--languages", default="c,cpp,rust,go,python,java")
    parser.add_argument("--timeout", type=float, default=20)
    parser.add_argument("--depth", type=int, default=100)
    args = parser.parse_args()
    if args.timeout <= 0 or args.depth < 1: parser.error("positive timeout and depth required")
    args.tracer = args.tracer.resolve(); require(args.tracer.is_file(), "build NP=1 tracer first")
    args.backends = ("full", "pages") if args.backend == "both" else (args.backend,)
    import tempfile
    args.output.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix="run-", dir=args.output.resolve()))
    cc, cxx = shlex.split(os.environ.get("CC", "/usr/bin/gcc")), shlex.split(os.environ.get("CXX", "/usr/bin/g++"))
    report = {"git_head_at_run": command(["git","rev-parse","HEAD"],ROOT).stdout.strip(),
              "product_diff_sha256": hashlib.sha256(command(["git","diff","--","src"],ROOT).stdout.encode()).hexdigest(),
              "tracer": str(args.tracer), "tracer_sha256": digest(args.tracer), "environment": ENV,
              "depth": args.depth, "session_timeout_s": args.timeout, "tracer_address_space_limit_bytes": 2*1024**3,
              "scope": "fixed NP1 profile; bounded runtime search is not a safety proof", "cases": []}
    try:
        if args.mode in ("inputs", "checks", "all"): report["cases"] += inputs(args, out, cc, cxx)
        if args.mode in ("threads", "checks", "all"): report["cases"] += threads(args,out,cc,cxx)
        if args.mode in ("runtimes", "all"): report["cases"] += runtimes(args, out, cc, cxx)
    except Exception as error:
        report["failure"] = classify_error(error)
    report["passed"] = bool(report["cases"]) and "failure" not in report and all(r["passed"] for r in report["cases"])
    (out / "results.json").write_text(json.dumps(report, indent=2))
    print("RESULT " + str(out / "results.json"), flush=True)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
