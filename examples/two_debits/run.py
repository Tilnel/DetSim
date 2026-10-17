#!/usr/bin/env python3
"""Two-debits C/C++ finite-input checks."""
import itertools
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from _support import session as common
from _support.session import Session, CaseFailure, build_plugin, command, digest, require, classify_error, search

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
                                       args.timeout, bug=variant == "bug",
                                       expected_diagnostic="account overdraft")
                row["toolchain"] = command(compiler+["--version"],ROOT).stdout.splitlines()[0]
                row["passed"] = all(p["status"] == ("violation" if variant == "bug" and p["inputs"] == "11" else "pass")
                                    for p in row["paths"]) and row["search"]["status"] == ("violation" if variant == "bug" else "pass")
                rows.append(row)
                print(json.dumps({k: row[k] for k in ("case", "language", "variant", "backend", "passed")}), flush=True)
    return rows


def main():
    return common.run_cli(inputs, description=__doc__, output_name="two_debits")


if __name__ == "__main__":
    raise SystemExit(main())
