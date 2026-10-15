#!/usr/bin/env python3
"""Reporting failures must not certify a partial concurrency matrix."""
import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "examples/cases"))
import concurrency_run as runner


class ConcurrencyReportTests(unittest.TestCase):
    def run_case(self, *, native_failure=None, session_failure=False, prepare_failure=False, model_exit=0):
        with tempfile.TemporaryDirectory(prefix="concurrency-report-test-") as temporary:
            output = Path(temporary)
            tracer = output / "tracer"; tracer.write_text("test tracer")
            def prepare(language, directory, _cc, _target):
                if prepare_failure:
                    raise RuntimeError("injected preparation failure")
                directory.mkdir(parents=True)
                return ["fixture"], [], {"language": language}
            def build(_source, destination, _cxx):
                destination.write_text("test plugin")
            def native(argv, _cwd, _timeout):
                if argv[-1] == "omit" and native_failure == "timeout":
                    raise subprocess.TimeoutExpired(argv, 1, output="partial output")
                if argv[-1] == "omit" and native_failure == "exit":
                    return subprocess.CompletedProcess(argv, 7, "native failed")
                return subprocess.CompletedProcess(argv, 0, runner.EXPECTED[argv[-1]])
            real_search = runner.common.search
            def search(*args, **kwargs):
                if args[1][-1] == "omit" and session_failure:
                    return real_search(*args, **kwargs)
                return {"status": "violation" if kwargs["bug"] else "pass", "tracer_exit": model_exit,
                        "replay": "same_process_hash_match_3x"}
            argv = ["concurrency_run.py", "--tracer", str(tracer), "--output", str(output),
                    "--languages", "rust", "--variants", "healthy,omit", "--backend", "both"]
            with mock.patch.object(sys, "argv", argv), \
                 mock.patch.object(runner, "prepare", side_effect=prepare), \
                 mock.patch.object(runner.common, "build_plugin", side_effect=build), \
                 mock.patch.object(runner.common, "command", side_effect=native), \
                 mock.patch.object(runner.common, "search", side_effect=search), \
                 mock.patch.object(runner.common, "Session", side_effect=RuntimeError("injected session creation failure")), \
                 contextlib.redirect_stdout(io.StringIO()):
                code = runner.main()
            report = json.loads(next(output.glob("concurrency-*/report.json")).read_text())
            return code, report

    def assert_complete_failed_matrix(self, code, report):
        self.assertEqual(code, 1)
        self.assertFalse(report["passed"])
        self.assertEqual(len(report["rows"]), 4)
        self.assertEqual({(row["language"], row["variant"], row["backend"]) for row in report["rows"]},
                         {("rust", variant, backend) for variant in ("healthy", "omit") for backend in ("full", "pages")})

    def test_mutant_native_timeout_after_healthy_pass(self):
        code, report = self.run_case(native_failure="timeout")
        self.assert_complete_failed_matrix(code, report)
        for row in report["rows"]:
            if row["variant"] == "healthy":
                self.assertTrue(row["passed"])
            else:
                self.assertFalse(row["passed"])
                self.assertEqual(row["native"]["status"], "timeout")
                self.assertIsNone(row["native"]["exit"])
                self.assertEqual(row["model"]["status"], "not_run")

    def test_session_constructor_exception_after_healthy_pass(self):
        code, report = self.run_case(session_failure=True)
        self.assert_complete_failed_matrix(code, report)
        for row in report["rows"]:
            if row["variant"] == "healthy":
                self.assertTrue(row["passed"])
            else:
                self.assertFalse(row["passed"])
                self.assertIn("injected session creation failure", row["model"]["diagnostic"])
                self.assertIsNone(row["model"]["tracer_exit"])

    def test_native_nonzero_exit_is_preserved(self):
        code, report = self.run_case(native_failure="exit")
        self.assert_complete_failed_matrix(code, report)
        self.assertEqual([row["native"]["exit"] for row in report["rows"] if row["variant"] == "omit"], [7, 7])

    def test_model_nonzero_exit_is_preserved_and_fails(self):
        code, report = self.run_case(model_exit=9)
        self.assert_complete_failed_matrix(code, report)
        self.assertEqual([row["model"]["tracer_exit"] for row in report["rows"]], [9] * 4)
        self.assertTrue(all(not row["passed"] for row in report["rows"]))

    def test_preparation_failure_records_every_combination(self):
        code, report = self.run_case(prepare_failure=True)
        self.assert_complete_failed_matrix(code, report)
        self.assertTrue(all(not row["passed"] and row["stage"] == "prepare" for row in report["rows"]))

    def test_matrix_requires_complete_unique_requested_keys(self):
        requested = [("rust", "healthy", "full"), ("rust", "omit", "full")]
        rows = [{"language": language, "variant": variant, "backend": backend, "passed": True}
                for language, variant, backend in requested]
        self.assertTrue(runner.matrix_passed(rows, requested))
        self.assertTrue(runner.matrix_passed(rows, [list(key) for key in requested]))
        self.assertFalse(runner.matrix_passed([], requested))
        self.assertFalse(runner.matrix_passed(rows[:1], requested))
        self.assertFalse(runner.matrix_passed([rows[0], rows[0]], requested))
        self.assertFalse(runner.matrix_passed(rows + [rows[0]], requested))
        self.assertFalse(runner.matrix_passed(rows, requested + [requested[0]]))
        unexpected = rows + [{"language": "python", "variant": "healthy", "backend": "full", "passed": True}]
        self.assertFalse(runner.matrix_passed(unexpected, requested))
        self.assertFalse(runner.matrix_passed(rows[:1] + [rows[1] | {"passed": False}], requested))

    def test_complete_matrix_passes(self):
        code, report = self.run_case()
        self.assertEqual(code, 0)
        self.assertTrue(report["passed"])
        self.assertEqual(len(report["rows"]), 4)


if __name__ == "__main__":
    unittest.main()
