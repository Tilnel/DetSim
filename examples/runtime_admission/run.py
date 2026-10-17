#!/usr/bin/env python3
"""Explicit runtime admission, control and bounded search probes."""
import json
import os
from pathlib import Path
import sys
import re
import shutil

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from _support import session as common
from _support.session import Session, CaseFailure, build_plugin, command, digest, require, classify_error, search, runtime_files

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
    return common.run_cli(runtimes, description=__doc__, output_name="runtime_admission")


if __name__ == "__main__":
    raise SystemExit(main())
