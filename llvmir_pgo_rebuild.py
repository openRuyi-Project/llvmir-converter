#!/usr/bin/env python3

"""Merge profraw files and rebuild the corresponding ELF files with PGO."""

import argparse
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from collections import defaultdict
from pathlib import Path


PROFRAW_RE = re.compile(r"^(?P<target>.+)_[^/]+\.profraw$")
OUTPUT_OPTIONS = {"-o", "--output"}


def log(message):
    print(message, flush=True)


def find_clang():
    for candidate in ("clang", "clang-22", "clang-21", "clang-20", "clang-19", "clang-18"):
        path = shutil.which(candidate)
        if path:
            return path
    raise RuntimeError("cannot find clang in PATH")


def generate_native_template(output_dir):
    """Generate the same native target-feature template as batch runner."""
    output_dir.mkdir(parents=True, exist_ok=True)
    template_path = output_dir / ".llvmir-native-template.ll"
    clang = find_clang()
    with tempfile.TemporaryDirectory(prefix="llvmir-template-") as temp_dir:
        source_path = Path(temp_dir) / "template.c"
        source_path.write_text("void template(void) {}\n", encoding="utf-8")
        command = [
            clang,
            "-S",
            "-emit-llvm",
            "-O2",
            "-march=native",
            str(source_path),
            "-o",
            str(template_path),
        ]
        log("Generating native template: " + shell_command(command))
        subprocess.run(command, check=True)
    return template_path


def shell_command(command):
    return " ".join(shlex.quote(str(part)) for part in command)


def scan_profraw(profraw_dir):
    groups = defaultdict(list)
    for path in sorted(profraw_dir.rglob("*.profraw")):
        match = PROFRAW_RE.match(path.name)
        if not match:
            log(f"Warning: cannot infer target from {path}, skipping")
            continue
        groups[match.group("target")].append(path.resolve())
    return dict(groups)


def parse_command(cmd_path):
    text = cmd_path.read_text(encoding="utf-8", errors="replace")
    tokens = []

    array_match = re.search(r"\bcmd\s*=\s*\((.*?)\)", text, re.DOTALL)
    if array_match:
        tokens = shlex.split(array_match.group(1), comments=True, posix=True)
    else:
        assignment = re.search(r"\bcmd\s*=\s*(.+)", text)
        if assignment:
            value = assignment.group(1).strip()
            tokens = shlex.split(value, comments=True, posix=True)

    if not tokens:
        for line in text.splitlines():
            if re.search(r"(?:^|/)clang(?:\+\+)?(?:-[0-9]+)?(?:\s|$)", line):
                tokens = shlex.split(line.strip(), comments=True, posix=True)
                break

    if not tokens:
        raise ValueError(f"cannot find clang command in {cmd_path}")
    if not re.search(r"(?:^|/)clang(?:\+\+)?(?:-[0-9]+)?$", Path(tokens[0]).name):
        raise ValueError(f"command does not start with clang: {cmd_path}")
    return tokens


def output_name(command):
    for index, token in enumerate(command):
        if token in OUTPUT_OPTIONS:
            if index + 1 >= len(command):
                raise ValueError("output option has no value")
            return Path(command[index + 1]).name
        for option in ("-o", "--output="):
            if token.startswith(option) and token != option:
                return Path(token[len(option):]).name
    raise ValueError("cannot find -o/--output in clang command")


def output_subdir(cmd_path):
    parent_name = cmd_path.parent.name
    if parent_name == "llvmir":
        return "lib"
    if parent_name == "llvmir-bin":
        return "bin"
    return parent_name


def clang_version(cmd_path, command):
    """Get the major version from the command, or its Clang version comment."""
    command_match = re.search(r"clang(?:\+\+)?-(\d+)$", Path(command[0]).name)
    command_version = command_match.group(1) if command_match else None

    text = cmd_path.read_text(encoding="utf-8", errors="replace")
    comment_match = re.search(r"clang\s+version\s+(\d+)(?:\.\d+)*", text, re.IGNORECASE)
    comment_version = comment_match.group(1) if comment_match else None

    if command_version and comment_version and command_version != comment_version:
        raise ValueError(
            f"clang version mismatch in {cmd_path}: command={command_version}, "
            f"comment={comment_version}"
        )
    version = command_version or comment_version
    if not version:
        raise ValueError(f"cannot determine clang version from _cmd: {cmd_path}")
    return version


def find_cmd_files(input_paths):
    """Find and de-duplicate _cmd files from files and recursive directories."""
    cmd_files = []
    for input_path in input_paths:
        path = Path(input_path)
        if path.is_file():
            if path.name.endswith("_cmd"):
                cmd_files.append(path.resolve())
            else:
                log(f"Warning: {path} is not a _cmd file, skipping")
        elif path.is_dir():
            cmd_files.extend(
                candidate.resolve()
                for candidate in path.rglob("*_cmd")
                if candidate.is_file()
            )
        else:
            log(f"Warning: input path does not exist, skipping: {path}")

    return sorted(set(cmd_files))


def build_rebuild_command(command, profile_path, output_path):
    result = []
    skip_next = False
    output_replaced = False
    for index, token in enumerate(command):
        if skip_next:
            skip_next = False
            continue
        if token in OUTPUT_OPTIONS:
            result.extend((token, str(output_path)))
            skip_next = True
            output_replaced = True
            continue
        if token.startswith("--output="):
            result.append(f"--output={output_path}")
            output_replaced = True
            continue
        if token.startswith("-o") and token != "-o":
            result.append(f"-o{output_path}")
            output_replaced = True
            continue
        if token in {"-fprofile-generate", "-fprofile-instr-generate"}:
            continue
        if token.startswith((
            "-fprofile-instr-generate=",
            "-fprofile-instrument-path=",
        )):
            continue
        if token == "-Xclang" and index + 1 < len(command) and command[index + 1].startswith(
            "-fprofile-instrument-path"
        ):
            skip_next = True
            continue
        result.append(token)
    if not output_replaced:
        result.extend(("-o", str(output_path)))
    result.append(f"-fprofile-use={profile_path}")
    return result


def run(command, cwd, dry_run):
    log(f"[{cwd}] {shell_command(command)}")
    if dry_run:
        return
    subprocess.run(command, cwd=cwd, check=True)


def executable_candidates(version, converter_dir):
    names = []
    if version:
        names.extend([f"llvmir-converter-{version}", f"llvmir-convert-{version}"])
    names.extend(["llvmir-converter", "llvmir-convert"])

    search_dirs = []
    if converter_dir:
        search_dirs.append(Path(converter_dir).resolve())
    search_dirs.append(Path(__file__).resolve().parent)
    search_dirs.append(Path.cwd())

    for directory in search_dirs:
        for name in names:
            yield directory / name
    for name in names:
        path = shutil.which(name)
        if path:
            yield Path(path)


def find_converter(version, converter_dir):
    for candidate in executable_candidates(version, converter_dir):
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate)
    suffix = f"-{version}" if version else ""
    raise FileNotFoundError(
        f"cannot find executable llvmir-converter{suffix} or llvmir-convert{suffix}"
    )


def write_temporary_cmd(command, cmd_path, profile_path, output_path):
    """Write a converter-compatible _cmd in /tmp, preserving output layout."""
    rebuilt = build_rebuild_command(command, profile_path, output_path)
    workspace = tempfile.TemporaryDirectory(prefix="llvmir-pgo-cmd-")
    workspace_path = Path(workspace.name) / cmd_path.parent.name
    workspace_path.mkdir()
    temporary_path = workspace_path / f".{cmd_path.name}.pgo_cmd"
    temporary_parent = temporary_path.parent

    def preserve_relative_path(token):
        if not token.startswith(("./", "../")):
            return
        source = (cmd_path.parent / token).resolve()
        link = temporary_parent / token
        link.parent.mkdir(parents=True, exist_ok=True)
        if not link.exists():
            link.symlink_to(source)

    absolute_command = []
    for token in rebuilt:
        if token.startswith("./") or token.startswith("../"):
            preserve_relative_path(token)
            absolute_command.append(token)
        elif "--version-script=" in token:
            prefix, relative_path = token.rsplit("=", 1)
            if relative_path.startswith(("./", "../")):
                preserve_relative_path(relative_path)
            absolute_command.append(token)
        else:
            absolute_command.append(token)
    try:
        with temporary_path.open("w", encoding="utf-8") as temporary:
            temporary.write("cmd=(\n")
            for token in absolute_command:
                temporary.write(f"  {shlex.quote(token)}\n")
            temporary.write(")\n\"${cmd[@]}\"\n")
    except Exception:
        workspace.cleanup()
        raise
    return temporary_path, workspace


def build_converter_command(converter, output_dir, template_path, cmd_path):
    return [
        converter,
        f"-o={output_dir}",
        f"-t={template_path}",
        str(cmd_path),
    ]


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--profraw-dir",
        type=Path,
        required=True,
        help="root directory to scan recursively for .profraw files",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("pgo-rebuilt"),
        help="rebuilt ELF output directory",
    )
    parser.add_argument(
        "--converter-dir",
        help="directory to search first for llvmir-converter-XX",
    )
    parser.add_argument(
        "--template",
        "-t",
        type=Path,
        help="use an existing template LL file instead of generating a native one",
    )
    parser.add_argument("--dry-run", action="store_true", help="print commands without executing them")
    parser.add_argument("--keep-going", action="store_true", help="continue after a target fails")
    parser.add_argument(
        "input_paths",
        nargs="+",
        type=Path,
        metavar="<ir-path>",
        help="one or more *_cmd files or directories, scanned recursively",
    )
    return parser.parse_args()


def find_profdata(version):
    if not version:
        raise FileNotFoundError(
            "cannot determine clang version from _cmd; use a versioned clang executable"
        )
    candidate = f"llvm-profdata-{version}"
    path = shutil.which(candidate)
    if path:
        return path
    raise FileNotFoundError(f"cannot find {candidate} for clang version {version}")


def main():
    args = parse_args()
    profraw_dir = args.profraw_dir.resolve()
    output_dir = args.output_dir.resolve()
    if not profraw_dir.is_dir():
        raise SystemExit(f"profraw directory does not exist: {profraw_dir}")

    template_path = (
        args.template.resolve()
        if args.template
        else generate_native_template(output_dir)
    )
    if not template_path.is_file():
        raise SystemExit(f"template LL file does not exist: {template_path}")
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "lib").mkdir(exist_ok=True)
    (output_dir / "bin").mkdir(exist_ok=True)

    groups = scan_profraw(profraw_dir)
    commands = {}
    for cmd_path in find_cmd_files(args.input_paths):
        command = parse_command(cmd_path)
        target = output_name(command)
        if target in commands:
            raise SystemExit(f"multiple _cmd files produce target {target}: {commands[target][0]} and {cmd_path}")
        commands[target] = (cmd_path, command)

    failures = 0
    with tempfile.TemporaryDirectory(prefix="llvmir-pgo-") as temporary_dir:
        profdata_dir = Path(temporary_dir)
        for target, profiles in groups.items():
            if target not in commands:
                log(f"Warning: no _cmd file found for profraw target {target}, skipping")
                failures += 1
                continue
            cmd_path, command = commands[target]
            temporary_cmd = None
            temporary_workspace = None
            try:
                version = clang_version(cmd_path, command)
                profdata = profdata_dir / f"{target}.profdata"
                output = output_dir / output_subdir(cmd_path) / target
                merge = [find_profdata(version), "merge", "-output", str(profdata)]
                merge.extend(str(profile) for profile in profiles)
                run(merge, profraw_dir, args.dry_run)
                temporary_cmd, temporary_workspace = write_temporary_cmd(
                    command, cmd_path, profdata, output
                )
                converter = find_converter(version, args.converter_dir)
                rebuild = build_converter_command(
                    converter, output_dir, template_path, temporary_cmd
                )
                run(rebuild, cmd_path.parent, args.dry_run)
                log(f"Rebuilt {target} from {len(profiles)} profraw file(s)")
            except (OSError, ValueError, subprocess.CalledProcessError) as error:
                failures += 1
                log(f"Failed to rebuild {target}: {error}")
                if not args.keep_going:
                    return 1
            finally:
                if temporary_cmd is not None:
                    temporary_workspace.cleanup()
    return 1 if failures else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)