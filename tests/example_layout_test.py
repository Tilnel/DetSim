"""Case-owned source layout and launchers; no ptrace or target processes."""
import ast
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
EXAMPLES = ROOT / "examples"
CASES = {
    "two_debits": ("native_inputs.c", "native_inputs.cpp", "input_checker.cpp"),
    "lost_notification": ("lost_notification.c", "deadlock_checker.cpp"),
    "runtime_admission": ("probes/runtime.py", "probes/RuntimeProbe.java", "admission_checker.cpp"),
    "exact_once": ("producer_consumer.py", "producer_consumer.rs", "exact_once_checker.cpp"),
    "raft": ("raft_fixture.c", "raft_time_choice.cpp", "plugins/check.cpp"),
    "redisraft": ("redisraft_fixture.c", "redisraft_repair_fixture.c", "plugins/check.cpp", "search.py"),
    "redisraft_module": ("client.c", "checker.cpp", "time_choice.cpp"),
}


class ExampleLayoutTest(unittest.TestCase):
    def test_case_sources_and_documented_configuration_are_colocated(self):
        for case, files in CASES.items():
            with self.subTest(case=case):
                directory = EXAMPLES / case
                for name in ("run.py", "README.md", *files):
                    self.assertTrue((directory / name).is_file(), str(directory / name))
                self.assertIn("config.json", (directory / "README.md").read_text())
        support = ast.parse((EXAMPLES / "_support/session.py").read_text())
        names = {node.name for node in support.body if isinstance(node, ast.FunctionDef)}
        self.assertFalse(names & {"inputs", "threads", "runtimes", "manual_path", "observe_logs"})
        self.assertFalse((EXAMPLES / "_support/raft_search.py").exists())

    def test_launchers_work_from_unrelated_directory_without_legacy_imports(self):
        launchers = [EXAMPLES / case / "run.py" for case in CASES]
        launchers += [EXAMPLES / "redisraft" / name for name in ("quorum.py", "repair.py")]
        launchers += [EXAMPLES / "redis/run.py", EXAMPLES / "two_debits/zero_guard_benchmark.py"]
        launchers += [EXAMPLES / "redisraft" / name for name in
                      ("benchmark.py", "soft_dirty_benchmark.py", "page_snapshot_benchmark.py")]
        probe = """import runpy,sys
sys.argv=[sys.argv[1],'--help']
try: runpy.run_path(sys.argv[0],run_name='__main__')
except SystemExit as error:
 if error.code not in (None,0): raise
for module in tuple(sys.modules.values()):
 path=str(getattr(module,'__file__',''))
 assert '/examples/cases/' not in path and '/build/legacy-cases/' not in path, path
"""
        with tempfile.TemporaryDirectory(prefix="detsim-example-layout-") as directory:
            for launcher in launchers:
                with self.subTest(launcher=launcher):
                    result = subprocess.run([sys.executable, "-c", probe, str(launcher)], cwd=directory,
                        env=os.environ | {"PYTHONPATH": "", "PYTHONDONTWRITEBYTECODE": "1"},
                        capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn("--tracer", result.stdout)
            profile = subprocess.run([sys.executable, str(EXAMPLES / "redisraft/profile_summary.py"), "--help"],
                cwd=directory, capture_output=True, text=True, timeout=10)
            self.assertEqual(profile.returncode, 0, profile.stdout + profile.stderr)

    def test_catalog_references_current_case_files(self):
        catalog = json.loads((EXAMPLES / "catalog.json").read_text())
        text = json.dumps(catalog)
        self.assertNotIn("examples/cases", text)
        for name in re.findall(r"examples/[\w/.-]+\.(?:py|c|md)", text):
            self.assertTrue((ROOT / name).is_file(), name)
        for old in ("redis_scenario.py", "redisraft_benchmark.py", "redisraft_profile_summary.py",
                    "soft_dirty_benchmark.py", "page_snapshot_benchmark.py", "zero_guard_benchmark.py",
                    "redisraft_seed.c", "redisraft.mk"):
            self.assertFalse((ROOT / "tests" / old).exists(), old)

    def test_no_standalone_hello_source_or_binary_at_examples_root(self):
        self.assertFalse((EXAMPLES / "hello.c").exists())
        self.assertTrue((EXAMPLES / "hello/hello.c").is_file())
        self.assertTrue((EXAMPLES / "hello/Makefile").is_file())
        old = EXAMPLES / "cases"
        self.assertTrue(not old.exists() or old.is_symlink(), "old cases source tree remains")

    def test_case_make_targets_use_case_owned_drivers_and_plugins(self):
        env = {key: value for key, value in os.environ.items()
               if key not in ("MAKEFLAGS", "MFLAGS", "MAKEOVERRIDES", "MAKELEVEL")}
        benchmark = subprocess.run(["make", "-n", "NP=3", "benchmark-redisraft"], cwd=ROOT,
                                   env=env, capture_output=True, text=True, timeout=10)
        self.assertEqual(benchmark.returncode, 0, benchmark.stdout + benchmark.stderr)
        self.assertIn("python3 examples/redisraft/benchmark.py", benchmark.stdout)
        self.assertIn("build/release/np3/examples/redisraft/seed.so", benchmark.stdout)
        self.assertNotIn("tests/redisraft_benchmark.py", benchmark.stdout)
        redis = subprocess.run(["make", "-n", "NP=2", "check-redis"], cwd=ROOT,
                               env=env, capture_output=True, text=True, timeout=10)
        self.assertEqual(redis.returncode, 0, redis.stdout + redis.stderr)
        self.assertIn("python3 examples/redis/run.py", redis.stdout)


if __name__ == "__main__":
    unittest.main()
