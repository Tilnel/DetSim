#!/usr/bin/env python3
"""Closed-world pthread lost-notification check."""
import json
from pathlib import Path
import sys
import subprocess

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from _support import session as common
from _support.session import build_plugin, command, digest, require, search

def threads(args,out,cc,cxx):
    oracle=out/"admission_checker.so"; deadlock=out/"deadlock_checker.so"
    build_plugin(HERE.parent/"runtime_admission"/"admission_checker.cpp",oracle,cxx)
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


def main():
    return common.run_cli(threads, description=__doc__, output_name="lost_notification")


if __name__ == "__main__":
    raise SystemExit(main())
