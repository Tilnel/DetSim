#!/usr/bin/env python3
"""Real Rust std / CPython / Go producer-consumer pairs under bounded DetSim search."""
import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import sys
import tempfile

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from _support import session as common

HERE = Path(__file__).resolve().parent
SOURCES = HERE
ROOT = HERE.parents[1]
EXPECTED = {
    "healthy": "EXACT_ONCE 0 1\n",
    "omit": "EXACT_ONCE 0\n",
    "duplicate": "EXACT_ONCE 0 1 1\n",
}


def prepare(language, output, cc, python_target):
    output.mkdir(parents=True)
    runtime_profile = {"startup_syscall": 9 if language == "go-gc" else 11}
    if language == "rust":
        source = SOURCES / "producer_consumer.rs"
        tool = [shutil.which("rustc") or "rustc"]
        binary = output / "producer-consumer"
        build = tool + ["-g", "-C", "relocation-model=static", "-C", "linker=" + cc[0],
                        str(source), "-o", str(binary)]
        result = common.command(build, ROOT, 60)
        (output / "build.log").write_text(result.stdout)
        common.require(result.returncode == 0, result.stdout)
        argv = [str(binary)]
        runtime = "rust-std-thread-and-mpsc"
        input_language = "rs"
    elif language in ("go-gccgo", "go-gc"):
        source = SOURCES / "producer_consumer.go"
        tool = ["/usr/bin/go"] if language == "go-gccgo" else [str(ROOT / "build/toolchains/go1.23.12/go/bin/go")]
        binary = output / "producer-consumer"
        compiler = "gccgo" if language == "go-gccgo" else "gc"
        build = tool + ["build", "-compiler=" + compiler, "-o", str(binary), str(source)]
        result = common.command(build, ROOT, 120)
        (output / "build.log").write_text(result.stdout)
        common.require(result.returncode == 0, result.stdout)
        argv = [str(binary)]
        runtime = compiler
        input_language = "go"
    elif language == "python":
        source = SOURCES / ("producer_consumer_condition.py" if python_target == "condition" else "producer_consumer.py")
        tool = [os.environ.get("CASE_PYTHON", "/usr/bin/python3")]
        build = None
        argv = tool + ["-I", "-S", str(source)]
        runtime = "cpython-threading-" + python_target
        input_language = "py"
    else:
        raise common.CaseFailure("unknown language: " + language)
    version = common.command(tool + (["version"] if language.startswith("go-") else ["--version"]), output)
    common.require(version.returncode == 0, version.stdout)
    if language.startswith("go-"):
        common.require(("gccgo" in version.stdout) == (language == "go-gccgo"),
                       "Go compiler identity does not match requested runtime: " + version.stdout)
    mappings, manifest = common.runtime_files(input_language, tool, source, output)
    details = {
        "language": language, "runtime_implementation": runtime,
        "toolchain": version.stdout.strip(), "compile": build, "command": argv,
        "source": str(source), "source_sha256": common.digest(source), "binary_sha256": common.digest(argv[0]),
        "runtime_profile": runtime_profile,
        "runtime_environment": {key: os.environ.get(key) for key in
            ("GODEBUG", "GOMAXPROCS", "GOGC", "GOMEMLIMIT", "GOEXPERIMENT", "GOFLAGS", "GOTOOLCHAIN")} if language.startswith("go-") else {},
        "primitives": "goroutine, unbuffered task channel, channel close, result-channel join" if language.startswith("go-") else
                      "std::thread, std::sync::mpsc, JoinHandle::join" if language == "rust" else
                      "threading.Thread, threading.Condition, list, Thread.join" if python_target == "condition" else
                      "threading.Thread, queue.Queue (_queue extension), Thread.join",
        "rootfs_manifest": str(manifest), "rootfs_sha256": common.digest(manifest),
        "runtime_settings": "existing -I -S for CPython; no GC, preemption or thread flags changed",
    }
    return argv, mappings, details


def matrix_passed(rows, requested):
    requested = [tuple(key) for key in requested]
    keys = [(row.get("language"), row.get("variant"), row.get("backend")) for row in rows]
    return (bool(requested) and len(requested) == len(set(requested)) and
            len(keys) == len(requested) and len(keys) == len(set(keys)) and
            set(keys) == set(requested) and all(row.get("passed") is True for row in rows))


def native_result(command, directory, timeout, variant):
    result = {"exit": None, "output": "", "matched": False, "expected_output": EXPECTED[variant],
              "oracle": "pass" if variant == "healthy" else "expected_exact_once_violation"}
    try:
        native = common.command(command, directory, timeout)
        result.update(exit=native.returncode, output=native.stdout)
        (directory / (variant + "-native.log")).write_text(native.stdout)
        result["matched"] = native.returncode == 0 and native.stdout == EXPECTED[variant]
        result["status"] = "pass" if result["matched"] else "failed"
        if not result["matched"]:
            result["diagnostic"] = "native exact-once oracle mismatch"
    except Exception as error:
        output = getattr(error, "output", None) or ""
        result["output"] = output.decode(errors="replace") if isinstance(output, bytes) else output
        result.update(common.classify_error(error))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tracer", type=Path, default=ROOT / "tracer")
    parser.add_argument("--output", type=Path, default=ROOT / "build/cases")
    parser.add_argument("--backend", choices=("full", "pages", "both"), default="both")
    parser.add_argument("--languages", default="rust,python")
    parser.add_argument("--variants", default="healthy,omit,duplicate")
    parser.add_argument("--python-target", choices=("condition", "queue"), default="condition",
                        help="independent CPython targets; queue imports the optional _queue extension")
    parser.add_argument("--timeout", type=float, default=600)
    parser.add_argument("--depth", type=int, default=1500)
    parser.add_argument("--memory-mb", type=int, default=4096)
    args = parser.parse_args()
    if args.timeout <= 0 or args.depth < 1 or args.memory_mb < 1:
        parser.error("positive timeout, depth and memory limit required")
    args.tracer = args.tracer.resolve()
    common.require(args.tracer.is_file(), "build NP=1 tracer first")
    languages = args.languages.split(",")
    variants = args.variants.split(",")
    common.require(all(language in ("rust", "python", "go-gccgo", "go-gc") for language in languages), "unknown language")
    common.require(all(variant in EXPECTED for variant in variants), "unknown variant")
    common.require(len(languages) == len(set(languages)) and len(variants) == len(set(variants)),
                   "requested languages and variants must be unique")
    common.MODEL_ADDRESS_SPACE_LIMIT = args.memory_mb * 1024**2
    backends = ("full", "pages") if args.backend == "both" else (args.backend,)
    requested = [(language, variant, backend) for language in languages
                 for variant in variants for backend in backends]
    args.output.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="concurrency-", dir=args.output.resolve()))
    cc = shlex.split(os.environ.get("CC", "/usr/bin/gcc"))
    cxx = shlex.split(os.environ.get("CXX", "/usr/bin/g++"))
    plugin = output / "exact_once_checker.so"
    common.build_plugin(SOURCES / "exact_once_checker.cpp", plugin, cxx)
    report = {
        "case": "runtime-producer-consumer-exact-once", "NP": 1,
        "invariant": "consumer receives task IDs 0 and 1 exactly once after producer announces completion",
        "scope": "bounded syscall-level search under existing actor policy; not exhaustive runtime interleavings",
        "tracer": str(args.tracer), "tracer_sha256": common.digest(args.tracer),
        "plugin_sha256": common.digest(plugin),
        "plugin_source_sha256": common.digest(SOURCES / "exact_once_checker.cpp"),
        "python_target": args.python_target, "requested_matrix": requested, "passed": False,
        "limits": {"timeout_seconds_per_model": args.timeout, "depth": args.depth,
                   "address_space_mb": args.memory_mb}, "rows": [],
    }
    def record(row):
        report["rows"].append(row)
        report["passed"] = matrix_passed(report["rows"], requested)
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps({"language": row["language"], "variant": row["variant"],
            "backend": row["backend"], "passed": row["passed"], "model": row.get("model")}), flush=True)

    try:
        for language in languages:
            try:
                argv, mappings, details = prepare(language, output / language, cc, args.python_target)
            except Exception as error:
                failure = common.classify_error(error)
                for variant in variants:
                    for backend in backends:
                        record({"language": language, "variant": variant, "backend": backend,
                            "passed": False, "stage": "prepare", **failure,
                            "native": {"status": "not_run", "exit": None},
                            "model": {"status": "not_run", "tracer_exit": None}})
                continue
            for variant in variants:
                command = argv + [variant]
                native = native_result(command, output / language, args.timeout, variant)
                for backend in backends:
                    directory = output / f"{language}-{variant}-{backend}"
                    row = details | {"language": language, "variant": variant, "backend": backend,
                        "command": command, "native": native, "passed": False,
                        "model": {"status": "not_run", "tracer_exit": None}}
                    if native["matched"]:
                        try:
                            row["model"] = common.search(args.tracer, command, plugin,
                                directory, backend, args.timeout, bug=variant != "healthy",
                                depth=args.depth, fsmap=mappings,
                                expected_diagnostic="exact-once producer/consumer mismatch",
                                runtime_profile=details.get("runtime_profile", {"startup_syscall": 11}))
                            row["passed"] = (row["model"]["status"] ==
                                ("pass" if variant == "healthy" else "violation") and
                                row["model"].get("tracer_exit") == 0)
                        except Exception as error:
                            row["stage"] = "model"
                            row["model"] = {"tracer_exit": None,
                                **common.classify_error(error, directory / "transcript.log")}
                    else:
                        row.update(stage="native", diagnostic=native.get("diagnostic", "native execution failed"))
                    record(row)
    finally:
        report["passed"] = matrix_passed(report["rows"], requested)
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        print("report: " + str(output / "report.json"), flush=True)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
