import importlib.util
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch


RUNNER_PATH = Path(__file__).resolve().parents[1] / "llvmir_batch_runner.py"
SPEC = importlib.util.spec_from_file_location("llvmir_batch_runner", RUNNER_PATH)
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)


class BatchRunnerFailureLogTest(unittest.TestCase):
    def setUp(self):
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name)
        self.cmd_path = self.root / "example_cmd"
        self.cmd_path.write_text("clang --version\n", encoding="utf-8")
        self.failure_log_path = self.root / "failures.json"
        self.args = SimpleNamespace(keep_going=True, converter_dir=None)

    def tearDown(self):
        self.temporary_directory.cleanup()

    def process(self, failures):
        return RUNNER.process_cmd_files(
            [self.cmd_path], self.args, self.root / "template.ll", {}, failures,
            self.failure_log_path,
        )

    def test_unchanged_failure_is_persisted_and_skipped(self):
        with patch.object(RUNNER, "process_cmd_file", return_value=2) as converter:
            self.assertEqual((0, 1), self.process({}))
            self.assertEqual(1, converter.call_count)

        failures = RUNNER.load_failures(self.failure_log_path)
        entry = failures[str(self.cmd_path)]
        self.assertEqual(self.cmd_path.stat().st_mtime_ns, entry["mtime_ns"])
        self.assertEqual(self.cmd_path.stat().st_size, entry["size"])
        self.assertEqual("exit code 2", entry["reason"])
        self.assertIn("failed_at", entry)

        with patch.object(RUNNER, "process_cmd_file", return_value=0) as converter:
            self.assertEqual((0, 0), self.process(failures))
            converter.assert_not_called()

    def test_changed_failure_is_retried_and_cleared_after_success(self):
        with patch.object(RUNNER, "process_cmd_file", return_value=2):
            self.process({})
        failures = RUNNER.load_failures(self.failure_log_path)

        self.cmd_path.write_text("clang --version\n# changed\n", encoding="utf-8")
        with patch.object(RUNNER, "process_cmd_file", return_value=0) as converter:
            self.assertEqual((1, 0), self.process(failures))
            converter.assert_called_once()

        self.assertEqual({}, RUNNER.load_failures(self.failure_log_path))


if __name__ == "__main__":
    unittest.main()