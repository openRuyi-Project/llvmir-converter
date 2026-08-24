import importlib.util
import tempfile
import unittest
from pathlib import Path


SCRIPT_PATH = Path(__file__).resolve().parents[1] / "llvmir_pgo_rebuild.py"
SPEC = importlib.util.spec_from_file_location("llvmir_pgo_rebuild", SCRIPT_PATH)
PGO = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PGO)


class PgoRebuildTest(unittest.TestCase):
    def setUp(self):
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name)

    def tearDown(self):
        self.temporary_directory.cleanup()

    def test_scan_profraw_groups_by_target_recursively(self):
        (self.root / "nested").mkdir()
        (self.root / "app_100.profraw").touch()
        (self.root / "nested" / "app_200.profraw").touch()
        (self.root / "nested" / "lib_300.profraw").touch()

        groups = PGO.scan_profraw(self.root)

        self.assertEqual({"app", "lib"}, set(groups))
        self.assertEqual(2, len(groups["app"]))
        self.assertEqual("lib_300.profraw", groups["lib"][0].name)

    def test_output_subdir_matches_converter_lib_and_bin_mapping(self):
        self.assertEqual("lib", PGO.output_subdir(self.root / "llvmir" / "app_cmd"))
        self.assertEqual("bin", PGO.output_subdir(self.root / "llvmir-bin" / "app_cmd"))

    def test_build_rebuild_command_replaces_output_and_uses_profile(self):
        command = [
            "/usr/bin/clang-22", "-O3", "./test", "-o", "output/test",
            "-fprofile-instr-generate=profiles/test_%p.profraw",
        ]
        rebuilt = PGO.build_rebuild_command(
            command, self.root / "test.profdata", self.root / "test"
        )

        self.assertEqual("-o", rebuilt[3])
        self.assertEqual(str(self.root / "test"), rebuilt[4])
        self.assertNotIn("-fprofile-instr-generate=profiles/test_%p.profraw", rebuilt)
        self.assertEqual(
            f"-fprofile-use={self.root / 'test.profdata'}", rebuilt[-1]
        )

    def test_build_rebuild_command_removes_llvm_instrumentation_pair(self):
        command = [
            "/usr/bin/clang-22", "-O3", "-fprofile-generate", "-Xclang",
            "-fprofile-instrument-path=profiles/test_%p.profraw", "./test",
            "-o", "output/test",
        ]

        rebuilt = PGO.build_rebuild_command(
            command, self.root / "test.profdata", self.root / "test"
        )

        self.assertNotIn("-fprofile-generate", rebuilt)
        self.assertNotIn("-Xclang", rebuilt)
        self.assertNotIn(
            "-fprofile-instrument-path=profiles/test_%p.profraw", rebuilt
        )
        self.assertEqual(
            f"-fprofile-use={self.root / 'test.profdata'}", rebuilt[-1]
        )

    def test_converter_command_uses_versioned_converter_and_temp_cmd(self):
        cmd = self.root / "app_cmd"
        cmd.write_text(
            "cmd=(/usr/bin/clang-22 ./app -o output/app)\n",
            encoding="utf-8",
        )
        temporary_cmd, workspace = PGO.write_temporary_cmd(
            PGO.parse_command(cmd), cmd, self.root / "app.profdata", self.root / "app"
        )
        try:
            converter_command = PGO.build_converter_command(
                "/usr/bin/llvmir-converter-22",
                self.root / "rebuilt",
                self.root / "template.ll",
                temporary_cmd,
            )
            self.assertEqual("/usr/bin/llvmir-converter-22", converter_command[0])
            self.assertEqual(f"-o={self.root / 'rebuilt'}", converter_command[1])
            self.assertEqual(f"-t={self.root / 'template.ll'}", converter_command[2])
            self.assertEqual(temporary_cmd, Path(converter_command[3]))
            self.assertIn(
                f"-fprofile-use={self.root / 'app.profdata'}",
                temporary_cmd.read_text(encoding="utf-8"),
            )
        finally:
            workspace.cleanup()

    def test_find_converter_prefers_converter_dir_and_versioned_name(self):
        converter_dir = self.root / "converters"
        converter_dir.mkdir()
        converter = converter_dir / "llvmir-converter-22"
        converter.write_text("#!/bin/sh\n", encoding="utf-8")
        converter.chmod(converter.stat().st_mode | 0o111)

        self.assertEqual(
            str(converter), PGO.find_converter("22", str(converter_dir))
        )

    def test_find_converter_supports_alias_and_unversioned_fallback(self):
        converter_dir = self.root / "converters"
        converter_dir.mkdir()
        alias = converter_dir / "llvmir-convert-22"
        alias.write_text("#!/bin/sh\n", encoding="utf-8")
        alias.chmod(alias.stat().st_mode | 0o111)

        self.assertEqual(str(alias), PGO.find_converter("22", str(converter_dir)))

        alias.unlink()
        fallback = converter_dir / "llvmir-converter"
        fallback.write_text("#!/bin/sh\n", encoding="utf-8")
        fallback.chmod(fallback.stat().st_mode | 0o111)
        self.assertEqual(
            str(fallback), PGO.find_converter("22", str(converter_dir))
        )

    def test_parse_array_command_and_output_name(self):
        cmd = self.root / "app_cmd"
        cmd.write_text(
            "cmd=(\n"
            "  /usr/bin/clang-22\n"
            "  ./app\n"
            "  --output=output/my-app\n"
            ")\n"
            '"${cmd[@]}"\n',
            encoding="utf-8",
        )

        command = PGO.parse_command(cmd)

        self.assertEqual("my-app", PGO.output_name(command))

    def test_clang_version_comes_from_version_comment(self):
        cmd = self.root / "app_cmd"
        cmd.write_text(
            "# Clang version: clang version 22.1.8\n"
            "cmd=(\n"
            "  /usr/bin/clang\n"
            "  ./app\n"
            "  -o output/app\n"
            ")\n",
            encoding="utf-8",
        )

        command = PGO.parse_command(cmd)

        self.assertEqual("22", PGO.clang_version(cmd, command))

    def test_clang_version_comment_must_match_command(self):
        cmd = self.root / "app_cmd"
        cmd.write_text(
            "# Clang version: clang version 21.1.0\n"
            "cmd=(/usr/bin/clang-22 ./app -o output/app)\n",
            encoding="utf-8",
        )

        command = PGO.parse_command(cmd)

        with self.assertRaisesRegex(ValueError, "version mismatch"):
            PGO.clang_version(cmd, command)

    def test_find_cmd_files_accepts_multiple_paths_and_deduplicates(self):
        first = self.root / "first"
        second = self.root / "second"
        first.mkdir()
        second.mkdir()
        first_cmd = first / "first_cmd"
        second_cmd = second / "second_cmd"
        first_cmd.touch()
        second_cmd.touch()

        cmd_files = PGO.find_cmd_files([first, second, first_cmd])

        self.assertEqual([first_cmd.resolve(), second_cmd.resolve()], cmd_files)


if __name__ == "__main__":
    unittest.main()