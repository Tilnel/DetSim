#!/usr/bin/env python3
"""Case-driven DetSim checks. Every search gets a fresh tracer and store."""
import argparse
import hashlib
import itertools
import json
import locale
import os
from pathlib import Path
import re
import resource
import shlex
import shutil
import signal
import stat
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
ENV = {"LC_ALL": "C", "LANG": "C", "PYTHONHASHSEED": "0"}
MODEL_ADDRESS_SPACE_LIMIT = 2 * 1024**3


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def limits():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    resource.setrlimit(resource.RLIMIT_AS, (MODEL_ADDRESS_SPACE_LIMIT, MODEL_ADDRESS_SPACE_LIMIT))


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
    def __init__(self, tracer, argv, plugin, directory, backend, timeout, choices=False, fsmap=(),
                 *, tracees=None, addresses=None, runtime_profile=None, choose_points=()):
        self.directory = directory
        directory.mkdir(parents=True)
        self.timeout = timeout
        self.started = time.monotonic()
        self.offset = 0
        self.commands = []
        self.log = directory / "transcript.log"
        targets = [argv] if tracees is None else list(tracees)
        require(targets and all(target for target in targets), "case requires nonempty tracee arguments")
        addresses = list(addresses) if addresses is not None else [f"127.0.0.{i+1}" for i in range(len(targets))]
        require(len(addresses) == len(targets), "case address and tracee counts differ")
        config = {"Nodes": len(targets), "Loglevel": 4, "Tracee": targets, "Addr": addresses,
                  "RuntimeProfile": {"startup_syscall": 11} | (runtime_profile or {}),
                  "Plugins": [str(p) for p in plugin] if isinstance(plugin,(list,tuple)) else [str(plugin)],
                  "FSMap": list(fsmap), "StateStore": {"hot_cache_mb": 32, "warm_cache_mb": 64}}
        points = ([{"syscall": 96, "choose": 2}] if choices else []) + list(choose_points)
        if points:
            config["ChoosePoint"] = points
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
        poll_delay = .0001
        while time.monotonic() < deadline:
            with self.log.open("rb") as transcript:
                transcript.seek(self.offset)
                content = transcript.read()
            # Offset counts raw bytes. Re-reading this unfinished reply preserves
            # split UTF-8 sequences and universal-newline behavior across polls.
            reply = content.decode(locale.getpreferredencoding(False), errors="replace").replace("\r\n", "\n").replace("\r", "\n")
            if reply.endswith("(ptmc) "):
                self.offset += len(content)
                return reply
            if self.proc.poll() is not None:
                raise CaseFailure(reply[-4000:] or f"tracer exited {self.proc.returncode}")
            time.sleep(poll_delay)
            poll_delay = min(poll_delay * 2, .005)
        raise TimeoutError(f"command deadline: {text}")

    def close(self, timeout=5):
        try:
            if self.proc.poll() is None:
                self.proc.communicate(b"q\n", timeout=timeout)
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
        reply = self.send("info current")
        edges = re.findall(r"([0-9a-f]{16}) => ([0-9a-f]{16})", reply)
        if edges:
            return edges[-1][1]
        initial = re.search(r"Current state ([0-9a-f]{16}) \(no predecessor\)", reply)
        return initial[1] if initial else None


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


def search_exit_receipt(reply):
    lines = re.findall(r"(?m)^SEARCH_EXITED ([^\n]+)$", reply)
    if not lines:
        return None
    require(len(lines) == 1, "search returned multiple completion receipts")
    edge = json.loads(lines[0])
    require(isinstance(edge, dict) and set(edge) ==
            {"schema", "source", "target", "process", "thread", "choice", "action", "exit_codes"},
            "invalid search completion receipt fields")
    require(type(edge["schema"]) is int and edge["schema"] == 1, "unsupported search receipt schema")
    for key in ("source", "target"):
        require(isinstance(edge[key], str) and re.fullmatch(r"[0-9a-f]{16}", edge[key]) and
                int(edge[key], 16) != 0, "invalid search receipt identity")
    require(edge["source"] != edge["target"], "completion receipt lacks an actual transition")
    # This helper starts one process. Other runners own their own NP contracts.
    require(type(edge["process"]) is int and edge["process"] == 0 and
            type(edge["thread"]) is int and 0 <= edge["thread"] < 64, "invalid search receipt actor")
    require(type(edge["choice"]) is int and -2 <= edge["choice"] <= 2147483647 and
            ((edge["action"] == "normal" and edge["choice"] >= -1) or
             (edge["action"] == "timeout" and edge["choice"] == -2)), "invalid search receipt action")
    require(isinstance(edge["exit_codes"], list) and len(edge["exit_codes"]) == 1 and
            all(type(code) is int and code == 0 for code in edge["exit_codes"]),
            "searched execution did not exit successfully")
    return edge


def current_search_edge(history):
    edges = re.findall(r"([0-9a-f]{16}) => ([0-9a-f]{16})", history)
    actor = re.search(r"(?m)^ Tracee (\d+)(?: \((choose (\d+)|timeout)\))?: "
                      r"(?:\[TID:(\d+)\] )?([^\n]+)$", history)
    require(len(edges) == 1 and actor, "search result lacks a saved incoming actor edge")
    require(int(actor[1]) == 0, "case search requires a single process")
    require(actor[4] or not actor[5].startswith("<fatal signal"), "fatal-stop actor unavailable")
    thread = int(actor[4]) - 1 if actor[4] else 0
    require(0 <= thread < 64, "invalid saved incoming thread")
    return {"source": edges[0][0], "target": edges[0][1], "process": 0, "thread": thread,
            "choice": -2 if actor[2] == "timeout" else int(actor[3]) if actor[3] else -1,
            "action": "timeout" if actor[2] == "timeout" else "normal", "operation": actor[5]}


def replay_search_edge(session, edge):
    require("Loaded state " + edge["source"] in session.send("load " + edge["source"]),
            "search edge predecessor unavailable")
    require(session.current_hash() == edge["source"], "search edge predecessor identity mismatch")
    require("Switched to process " + str(edge["process"]) in session.send("sw " + str(edge["process"])),
            "search replay process selection failed")
    require("Switched to thread " + str(edge["thread"]) + " " in session.send("thread " + str(edge["thread"])),
            "search replay thread selection failed")
    action = "si timeout" if edge["action"] == "timeout" else "si " + str(max(0, edge["choice"]))
    reply = session.send(action)
    require("DetSim state/execution error:" not in reply, reply[-4000:])
    require(session.current_hash() == edge["target"], "search replay successor identity drift")
    return reply, action


def search(tracer, argv, plugin, directory, backend, timeout, *, bug=False, depth=None, fsmap=(), expected_diagnostic="account overdraft", runtime_profile=None):
    row = {"artifacts": str(directory), "startup": "not_started", "search": "not_started"}
    session = Session(tracer, argv, plugin, directory, backend, timeout, choices=depth is None, fsmap=fsmap, runtime_profile=runtime_profile)
    try:
        session.send(); row["startup"] = "admitted"
        session.send("sw 0")
        row["search"] = "started"
        reply = session.send("bfs --quiet" if depth is None else f"dfs {depth} --quiet")
        require("DetSim state/execution error:" not in reply, reply[-4000:])
        numbers = re.search(r"(?:all |Searched for )(\d+) sys_states(?: \(new: (\d+)\))?", reply)
        if numbers:
            row["visited"] = int(numbers[1])
            if numbers[2]: row["new"] = int(numbers[2])
        if "Stopped for illegal state" in reply:
            require(bug and expected_diagnostic in session.log.read_text(), "unexpected property violation")
            edge = current_search_edge(session.send("info current"))
            predecessor,witness=edge["source"],edge["target"]
            recipe=list(session.commands)
            for _ in range(3):
                replay, action = replay_search_edge(session, edge)
                require("Reach illegal state" in replay,"search witness failed replay")
            row.update(status="violation",search="violation_found",replay="same_process_hash_match_3x",
                       predecessor=predecessor,witness=witness)
            (directory/"witness.json").write_text(json.dumps({"commands":recipe,"predecessor":predecessor,
                "successor":witness,"replay_action":action,"actor":edge,"diagnostic":expected_diagnostic},indent=2))
        else:
            require("Complete explore all" in reply and not bug, "expected search result absent")
            receipt = search_exit_receipt(reply)
            if receipt:
                require("Loaded state " + receipt["target"] in session.send("load " + receipt["target"]),
                        "searched completion state unavailable")
                require(session.current_hash() == receipt["target"], "searched completion identity mismatch")
                edge = receipt
                terminal_candidate = True
                row["completion_receipt"] = receipt
            else:
                edge = current_search_edge(session.send("info current"))
                terminal_candidate = "exit_group(" in edge["operation"]
            if depth is not None and not terminal_candidate:
                row.update(status="bounded",search="bounded_no_violation",guest_exit=None)
            else:
                terminal = session.send("si 0")
                require("has exited with status 0" in terminal,"search finished but guest success not established")
                predecessor, successor = edge["source"], edge["target"]
                recipe = list(session.commands)
                for _ in range(3):
                    replay, action = replay_search_edge(session, edge)
                    require("Reach illegal state" not in replay,
                            "completion replay changed outcome or identity")
                    require("has exited with status 0" in session.send("si 0"), "completion replay did not exit zero")
                row.update(status="pass",guest_exit=0,replay="same_process_hash_match_3x",
                           predecessor=predecessor,completion=successor,
                           search="finite_fixture_exhausted" if depth is None else "bounded_guest_completed")
                (directory / "completion.json").write_text(json.dumps({"commands": recipe,
                    "predecessor": predecessor, "successor": successor, "replay_action": action, "actor": edge,
                    "scope": "same tracee lifecycle; checked output/zero exit preserved"}, indent=2))
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


def admission_controls(tracer, argv, plugin, directory, backend, timeout, fsmap, *, runtime_profile=None):
    row = {"startup": "not_started", "controlled_steps": 0, "restore": "not_run", "artifacts": str(directory)}
    session = Session(tracer, argv, plugin, directory, backend, timeout, fsmap=fsmap, runtime_profile=runtime_profile)
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


def runtime_files(language, tool, source, out):
    """Explicit workload inputs; never discover host files during exploration."""
    roots = [source]
    dependencies = []
    if language == "py":
        probe = command(tool + ["-I", "-S", "-c",
            "import json,sysconfig; print(json.dumps(sorted(set(sysconfig.get_path(k) for k in ('stdlib','platstdlib')))))"], out)
        require(probe.returncode == 0, probe.stdout)
        roots += [Path(path) for path in json.loads(probe.stdout)]
    elif language == "java":
        home = Path(tool[0]).resolve().parent.parent
        roots += [home / "lib", home / "conf", home / "release", out / "RuntimeProbe.class"]
        # The launcher loads libjvm after the trusted loader boundary, so its
        # ordinary ELF dependencies must exist in the captured input namespace.
        for library in (home / "lib/server/libjvm.so", home / "lib/libjava.so"):
            traced = command(["ldd", str(library)], out)
            require(traced.returncode == 0, traced.stdout)
            dependencies.append({"command": ["ldd", str(library)], "output": traced.stdout})
            for resolved in re.findall(r"(?:=>\s*)?(/[\S]+)\s+\(0x[0-9a-fA-F]+\)", traced.stdout):
                path = Path(resolved)
                # Preserve the final symlink name, canonicalizing directory
                # aliases such as /lib -> /usr/lib without importing all /usr/lib.
                roots.append(path.parent.resolve() / path.name)
    # Retain external symlink referents as explicit mappings too; do not flatten
    # symlinks in the input namespace. Missing referents remain genuinely absent.
    entries = {}
    mappings = []
    seen_roots = set()
    while roots:
        root = Path(os.path.abspath(roots.pop(0)))
        if root in seen_roots:
            continue
        seen_roots.add(root)
        mappings.append({"Host": str(root), "Target": str(root)})
        paths = [root]
        if root.is_dir() and not root.is_symlink():
            for current, dirs, files in os.walk(root, followlinks=False):
                paths += [Path(current) / name for name in sorted(dirs + files)]
        for path in paths:
            if str(path) in entries:
                continue
            mode = path.lstat().st_mode
            entry = {"path": str(path), "mode": mode}
            if stat.S_ISLNK(mode):
                target = os.readlink(path)
                entry["link"] = target
                referent = Path(os.path.abspath(path.parent / target))
                if (referent.exists() or referent.is_symlink()) and not referent.is_relative_to(root):
                    roots.append(referent)
            elif stat.S_ISREG(mode):
                entry.update(size=path.stat().st_size, mtime_ns=path.stat().st_mtime_ns, sha256=digest(path))
            elif not stat.S_ISDIR(mode):
                raise CaseFailure("unsupported runtime input type: " + str(path))
            entries[str(path)] = entry
    manifest = out / ("rootfs-" + language + ".json")
    manifest.write_text(json.dumps({"mappings": mappings, "dependency_discovery": dependencies,
        "entries": sorted(entries.values(), key=lambda row: row["path"])}, indent=2))
    return mappings, manifest


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
            mappings, manifest = runtime_files(language, tool, source, out)
            row.update(rootfs_manifest=str(manifest), rootfs_sha256=digest(manifest))
            for backend in args.backends:
                result = dict(row, backend=backend)
                result["control"] = admission_controls(args.tracer, argv, plugin,
                    out / ("control-"+language+"-"+backend),backend,args.timeout,mappings, runtime_profile={"startup_syscall": args.startup_syscall})
                result["model"] = search(args.tracer, argv, plugin, out / ("admission-"+language+"-"+backend),
                    backend, args.timeout, depth=args.depth, fsmap=mappings,
                    runtime_profile={"startup_syscall": args.startup_syscall})
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
    global MODEL_ADDRESS_SPACE_LIMIT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("inputs", "threads", "runtimes", "checks", "all"), default="inputs")
    parser.add_argument("--tracer", type=Path, default=ROOT / "tracer")
    parser.add_argument("--output", type=Path, default=ROOT / "build/cases")
    parser.add_argument("--backend", choices=("full", "pages", "both"), default="both")
    parser.add_argument("--languages", default="c,cpp,rust,go,python,java")
    parser.add_argument("--startup-syscall", type=int, default=11, help="runtime admission: explicit loader boundary syscall number")
    parser.add_argument("--memory-mb", type=int, default=2048, help="explicit tracer/tracee address-space limit")
    parser.add_argument("--timeout", type=float, default=20)
    parser.add_argument("--depth", type=int, default=100)
    args = parser.parse_args()
    if not 0 <= args.startup_syscall < 450:
        parser.error("startup syscall must be in [0, 450)")
    if args.timeout <= 0 or args.depth < 1 or args.memory_mb < 1:
        parser.error("positive timeout, depth and memory limit required")
    MODEL_ADDRESS_SPACE_LIMIT = args.memory_mb * 1024**2
    args.tracer = args.tracer.resolve(); require(args.tracer.is_file(), "build NP=1 tracer first")
    args.backends = ("full", "pages") if args.backend == "both" else (args.backend,)
    import tempfile
    args.output.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix="run-", dir=args.output.resolve()))
    cc, cxx = shlex.split(os.environ.get("CC", "/usr/bin/gcc")), shlex.split(os.environ.get("CXX", "/usr/bin/g++"))
    report = {"git_head_at_run": command(["git","rev-parse","HEAD"],ROOT).stdout.strip(),
              "product_diff_sha256": hashlib.sha256(command(["git","diff","--","src"],ROOT).stdout.encode()).hexdigest(),
              "tracer": str(args.tracer), "tracer_sha256": digest(args.tracer), "environment": ENV,
              "depth": args.depth, "runtime_startup_syscall": args.startup_syscall, "session_timeout_s": args.timeout, "tracer_address_space_limit_bytes": MODEL_ADDRESS_SPACE_LIMIT,
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
