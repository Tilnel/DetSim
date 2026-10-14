"""Exercise the real Makefile with a fake compiler in disposable directories."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch


MAKEFILE = Path(__file__).resolve().parents[1] / "Makefile"


class BuildContractTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="detsim-build-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "sample.cpp").write_text("int main() { return 0; }\n")
        (self.root / "nr2call.c").write_text("/* fixture */\n")
        self.compiler = self.root / "compiler"
        self.compiler.write_text(
            "#!/usr/bin/env python3\n"
            "import os, pathlib, sys\n"
            "args = sys.argv[1:]\n"
            "with open('calls', 'a') as log: log.write(repr(args) + '\\n')\n"
            "if os.environ.get('FAIL_COMPILE') and '-c' in args: sys.exit(23)\n"
            "if '-o' in args:\n"
            "    out = pathlib.Path(args[args.index('-o') + 1])\n"
            "    out.parent.mkdir(parents=True, exist_ok=True)\n"
            "    out.write_text('fixture output\\n')\n"
        )
        self.compiler.chmod(0o755)

    def make(self, *args, fail=False, ok=True):
        env = os.environ.copy()
        # Each fixture owns its configuration, independent of `make check` flags.
        for name in ("MAKEFLAGS", "MFLAGS", "MAKEOVERRIDES", "MAKELEVEL"):
            env.pop(name, None)
        if fail:
            env["FAIL_COMPILE"] = "1"
        result = subprocess.run(
            ["make", "--no-print-directory", "-f", str(MAKEFILE),
             "SRCS=sample.cpp", f"CC={self.compiler}",
             f"CXX={self.compiler}", f"LD={self.compiler}", *args],
            cwd=self.root, env=env, text=True, capture_output=True,
        )
        if ok:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def compilations(self):
        calls = self.root / "calls"
        return [line for line in calls.read_text().splitlines()
                if "'sample.cpp'" in line and "'-c'" in line] if calls.exists() else []

    def test_compiler_failure_is_not_hidden(self):
        result = self.make("NP=1", fail=True, ok=False)
        self.assertNotEqual(result.returncode, 0)
        # A failed compile must stop make before attempting the final link.
        self.assertNotIn("'-o', 'tracer'", (self.root / "calls").read_text())

    def test_np_change_rebuilds_and_returning_reuses_correct_objects(self):
        self.make("NP=1")
        self.make("NP=2")
        self.make("NP=1")
        calls = self.compilations()
        self.assertEqual(len(calls), 2, calls)
        self.assertIn("'-DNP=1'", calls[0])
        self.assertIn("'-DNP=2'", calls[1])
        self.assertIn("build/release/np1/sample.o", (self.root / "calls").read_text().splitlines()[-1])

    def test_mode_change_rebuilds(self):
        self.make("NP=1", "release")
        self.make("NP=1", "debug")
        self.assertEqual(len(self.compilations()), 2)
        self.assertIn("'-g'", self.compilations()[1])

    def test_flags_change_rebuilds_but_unchanged_build_is_incremental(self):
        self.make("NP=1", "CXXFLAGS=-O0")
        self.make("NP=1", "CXXFLAGS=-O0")
        self.assertEqual(len(self.compilations()), 1)
        self.make("NP=1", "CXXFLAGS=-O1")
        self.assertEqual(len(self.compilations()), 2)

    def test_invalid_np_rejected_before_compilation(self):
        for np in ("", "0", "-1", "abc"):
            with self.subTest(np=np):
                self.assertNotEqual(self.make(f"NP={np}", ok=False).returncode, 0)
        self.assertEqual(self.compilations(), [])

    def test_compiler_change_rebuilds(self):
        self.make("NP=1")
        other = self.root / "other-compiler"
        other.write_bytes(self.compiler.read_bytes())
        other.chmod(0o755)
        self.make("NP=1", f"CXX={other}")
        self.assertEqual(len(self.compilations()), 2)

    def test_user_cppflags_preserve_project_includes_and_np(self):
        self.make("NP=2", "CPPFLAGS=-DUSER_FLAG")
        call = self.compilations()[0]
        self.assertIn("'-DUSER_FLAG'", call)
        self.assertIn("'-DNP=2'", call)
        self.assertIn("'-Isrc'", call)

    def test_parent_make_configuration_does_not_leak_into_fixtures(self):
        with patch.dict(os.environ, {"MAKEFLAGS": "-- BUILD_DIR=wrong"}):
            self.make("NP=1")
        self.assertIn("build/release/np1/sample.o", (self.root / "calls").read_text())

    def test_clean_preserves_tracked_parser_and_runtime_snapshots(self):
        for name in ("src/utils/expr_parser.cpp", "src/utils/expr_parser.hpp",
                     "src/utils/expr_lexer.cpp", "memory/snapshot", "sstate/snapshot"):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("preserve me\n")
        self.make("clean")
        self.assertTrue((self.root / "src/utils/expr_parser.cpp").exists())
        self.assertTrue((self.root / "src/utils/expr_parser.hpp").exists())
        self.assertTrue((self.root / "src/utils/expr_lexer.cpp").exists())
        self.assertTrue((self.root / "memory/snapshot").exists())
        self.assertTrue((self.root / "sstate/snapshot").exists())

    def test_pgo_phase_change_rebuilds_without_parallel_clean_race(self):
        self.make("NP=1", "pgo-generate", "-j4")
        self.make("NP=1", "pgo-use", "-j4")
        self.assertEqual(len(self.compilations()), 2)
        self.assertIn("'-fprofile-generate'", self.compilations()[0])
        self.assertIn("'-fprofile-use'", self.compilations()[1])


if __name__ == "__main__":
    unittest.main()
