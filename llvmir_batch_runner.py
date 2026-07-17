#!/usr/bin/env python3

import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path


CLANG_VERSION_RE = re.compile(r"(?:^|[^\w.+-])(?:clang|clang\+\+|clang-cpp)(?:-(\d+))?(?=$|[^\w.+-])")
CLANG_VERSION_COMMENT_RE = re.compile(r"clang version:?\s+(\d+)", re.IGNORECASE)


def log(message):
    print(time.strftime("[%Y-%m-%d %H:%M:%S]"), message, flush=True)


def parse_args():
    parser = argparse.ArgumentParser(
        description="Convert LLVM IR _cmd files with CPU/memory throttling."
    )
    parser.add_argument("input_paths", nargs="+", metavar="<input>", help="_cmd file(s) or directory(ies) to scan for *_cmd files")
    parser.add_argument("--output-dir", "-o", default="output", help="output directory for converted files, default: output")
    parser.add_argument("--cpu-limit", type=float, default=90.0, help="pause when system CPU usage is at or above this percent, default: 90")
    parser.add_argument("--cpu-resume", type=float, default=60.0, help="resume when system CPU usage is at or below this percent, default: 60")
    parser.add_argument("--memory-limit", type=float, default=85.0, help="pause when system memory usage is at or above this percent, default: 85")
    parser.add_argument("--memory-resume", type=float, default=70.0, help="resume when system memory usage is at or below this percent, default: 70")
    parser.add_argument("--once", action="store_true", help="scan inputs once and exit instead of watching continuously")
    parser.add_argument("--scan-interval", type=float, default=5.0, help="scan interval in seconds, default: 5")
    parser.add_argument("--monitor-interval", type=float, default=1.0, help="monitor interval in seconds, default: 1")
    parser.add_argument("--template", "-t", help="use an existing template LL file instead of generating one")
    parser.add_argument("--keep-temp", action="store_true", help="pass --keep-temp to llvmir-converter")
    parser.add_argument("--incremental", action="store_true", help="pass --incremental to llvmir-converter")
    parser.add_argument("--verbose", "-v", action="store_true", help="pass -v to llvmir-converter")
    parser.add_argument("--pgo-output", help="PGO instrumented output directory")
    parser.add_argument("--pgo-profiles-output", help="directory for PGO .profraw output at runtime")
    parser.add_argument("--keep-going", action="store_true", help="continue after a _cmd conversion failure")
    parser.add_argument("--failure-log", help="JSON file used to persist unchanged failed _cmd files; default: <output-dir>/.llvmir-batch-failures.json")
    parser.add_argument("--dry-run", action="store_true", help="pass --dry-run to llvmir-converter")
    parser.add_argument("--converter-dir", help="directory to search first for llvmir-converter-XX")
    args = parser.parse_args()
    validate_thresholds(args)
    validate_pgo_args(args)
    return args


def validate_thresholds(args):
    values = {
        "cpu_limit": args.cpu_limit,
        "cpu_resume": args.cpu_resume,
        "memory_limit": args.memory_limit,
        "memory_resume": args.memory_resume,
    }
    for name, value in values.items():
        if not 0 <= value <= 100:
            raise SystemExit(f"{name} must be in [0, 100], got {value}")
    if args.cpu_resume > args.cpu_limit:
        raise SystemExit("cpu_resume must be less than or equal to cpu_limit")
    if args.memory_resume > args.memory_limit:
        raise SystemExit("memory_resume must be less than or equal to memory_limit")
    if args.scan_interval <= 0 or args.monitor_interval <= 0:
        raise SystemExit("scan and monitor intervals must be positive")


def validate_pgo_args(args):
    if args.pgo_output and not args.pgo_profiles_output:
        raise SystemExit("Error: --pgo-output requires --pgo-profiles-output=<directory>")
    if args.pgo_output and Path(args.output_dir).resolve() == Path(args.pgo_output).resolve():
        raise SystemExit("Error: --pgo-output must be different from --output-dir output directory")


def read_proc_stat():
    with open("/proc/stat", "r", encoding="utf-8") as proc_stat:
        fields = proc_stat.readline().split()[1:]
    values = [int(field) for field in fields]
    idle = values[3] + values[4]
    total = sum(values)
    return idle, total


def cpu_usage_percent(previous, current):
    previous_idle, previous_total = previous
    current_idle, current_total = current
    total_delta = current_total - previous_total
    idle_delta = current_idle - previous_idle
    if total_delta <= 0:
        return 0.0
    return max(0.0, min(100.0, 100.0 * (total_delta - idle_delta) / total_delta))


def memory_usage_percent():
    meminfo = {}
    with open("/proc/meminfo", "r", encoding="utf-8") as proc_meminfo:
        for line in proc_meminfo:
            key, value = line.split(":", 1)
            meminfo[key] = int(value.strip().split()[0])
    total = meminfo.get("MemTotal", 0)
    available = meminfo.get("MemAvailable", 0)
    if total <= 0:
        return 0.0
    return max(0.0, min(100.0, 100.0 * (total - available) / total))


def find_clang():
    for candidate in ("clang", "clang-22", "clang-21", "clang-20", "clang-19", "clang-18"):
        path = shutil.which(candidate)
        if path:
            return path
    raise RuntimeError("cannot find clang in PATH")


def generate_native_template(output_dir):
    output_dir.mkdir(parents=True, exist_ok=True)
    template_path = output_dir / ".llvmir-native-template.ll"
    clang = find_clang()
    with tempfile.TemporaryDirectory(prefix="llvmir-template-") as temp_dir:
        source_path = Path(temp_dir) / "template.c"
        source_path.write_text("void template(void) {}\n", encoding="utf-8")
        command = [clang, "-S", "-emit-llvm", "-O2", "-march=native", str(source_path), "-o", str(template_path)]
        log("Generating native template: " + " ".join(command))
        subprocess.run(command, check=True)
    return template_path


def scan_cmd_files(llvmir_path):
    return sorted(path for path in llvmir_path.rglob("*_cmd") if path.is_file())


def find_cmd_files(input_paths):
    cmd_files = []
    for input_path in input_paths:
        path = Path(input_path)
        if path.is_file():
            if path.name.endswith("_cmd"):
                cmd_files.append(path.resolve())
            else:
                log(f"Warning: {input_path} is not a _cmd file, skipping")
        elif path.is_dir():
            cmd_files.extend(scan_cmd_files(path.resolve()))
        else:
            log(f"Warning: {input_path} does not exist, skipping")

    seen = set()
    unique_files = []
    for cmd_file in cmd_files:
        if cmd_file not in seen:
            seen.add(cmd_file)
            unique_files.append(cmd_file)
    return sorted(unique_files)


def extract_converter_version(cmd_path):
    text = cmd_path.read_text(encoding="utf-8", errors="replace")
    match = CLANG_VERSION_RE.search(text)
    if match and match.group(1):
        return match.group(1)
    match = CLANG_VERSION_COMMENT_RE.search(text)
    return match.group(1) if match else None


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
    raise FileNotFoundError(f"cannot find executable llvmir-converter{suffix} or llvmir-convert{suffix}")


def build_converter_command(args, converter, template_path, cmd_path):
    command = [converter, f"-o={Path(args.output_dir).resolve()}", f"-t={template_path}"]
    if args.keep_temp:
        command.append("--keep-temp")
    if args.incremental:
        command.append("--incremental")
    if args.verbose:
        command.append("-v")
    if args.pgo_output:
        command.append(f"--pgo-output={Path(args.pgo_output).resolve()}")
    if args.pgo_profiles_output:
        command.append(f"--pgo-profiles-output={Path(args.pgo_profiles_output).resolve()}")
    if args.dry_run:
        command.append("--dry-run")
    command.append(str(cmd_path))
    return command


def cmd_file_version(cmd_path):
    stat_result = cmd_path.stat()
    return {"mtime_ns": stat_result.st_mtime_ns, "size": stat_result.st_size}


def load_failures(failure_log_path):
    try:
        contents = failure_log_path.read_text(encoding="utf-8")
        records = json.loads(contents)
        if not isinstance(records, dict):
            raise ValueError("top-level JSON value must be an object")
        return records
    except FileNotFoundError:
        return {}
    except (OSError, ValueError, json.JSONDecodeError) as error:
        log(f"Warning: cannot read failure log {failure_log_path}: {error}; ignoring it")
        return {}


def save_failures(failure_log_path, failures):
    failure_log_path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", dir=failure_log_path.parent,
        prefix=f".{failure_log_path.name}.", suffix=".tmp", delete=False,
    ) as temporary_file:
        json.dump(failures, temporary_file, ensure_ascii=True, indent=2, sort_keys=True)
        temporary_file.write("\n")
        temporary_name = temporary_file.name
    os.replace(temporary_name, failure_log_path)


def record_failure(failures, cmd_path, version, reason):
    failures[str(cmd_path)] = {
        **version,
        "failed_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "reason": reason,
    }


def is_unchanged_failure(record, version):
    return (
        isinstance(record, dict)
        and record.get("mtime_ns") == version["mtime_ns"]
        and record.get("size") == version["size"]
    )


def send_process_group(process, sig):
    try:
        os.killpg(process.pid, sig)
        return True
    except ProcessLookupError:
        return False


def run_with_throttling(command, args):
    log("Executing: " + " ".join(command))
    process = subprocess.Popen(command, start_new_session=True)
    stopped = False
    previous_cpu = read_proc_stat()

    try:
        while True:
            status = process.poll()
            if status is not None:
                if stopped:
                    send_process_group(process, signal.SIGCONT)
                return status

            time.sleep(args.monitor_interval)
            current_cpu = read_proc_stat()
            cpu_usage = cpu_usage_percent(previous_cpu, current_cpu)
            previous_cpu = current_cpu
            mem_usage = memory_usage_percent()

            if not stopped and (cpu_usage >= args.cpu_limit or mem_usage >= args.memory_limit):
                if send_process_group(process, signal.SIGSTOP):
                    stopped = True
                    log(f"SIGSTOP pid={process.pid}: cpu={cpu_usage:.1f}% mem={mem_usage:.1f}%")
            elif stopped and cpu_usage <= args.cpu_resume and mem_usage <= args.memory_resume:
                if send_process_group(process, signal.SIGCONT):
                    stopped = False
                    log(f"SIGCONT pid={process.pid}: cpu={cpu_usage:.1f}% mem={mem_usage:.1f}%")
    except BaseException:
        if stopped:
            send_process_group(process, signal.SIGCONT)
        if process.poll() is None:
            send_process_group(process, signal.SIGTERM)
        raise


def process_cmd_file(cmd_path, args, template_path):
    version = extract_converter_version(cmd_path)
    converter = find_converter(version, args.converter_dir)
    command = build_converter_command(args, converter, template_path, cmd_path)
    return run_with_throttling(command, args)


def process_cmd_files(cmd_files, args, template_path, processed, failures, failure_log_path):
    success = 0
    failed = 0
    failures_changed = False
    for cmd_path in cmd_files:
        version = cmd_file_version(cmd_path)
        if processed.get(cmd_path) == version["mtime_ns"]:
            continue
        failure = failures.get(str(cmd_path))
        if is_unchanged_failure(failure, version):
            log(f"Skipping unchanged failed _cmd (see {failure_log_path}): {cmd_path}")
            continue
        if failure is not None:
            del failures[str(cmd_path)]
            failures_changed = True
            log(f"Retrying changed failed _cmd: {cmd_path}")
        log(f"Processing _cmd: {cmd_path}")
        try:
            return_code = process_cmd_file(cmd_path, args, template_path)
        except Exception as error:
            failed += 1
            log(f"Failed: {cmd_path}: {error}")
            record_failure(failures, cmd_path, version, str(error))
            failures_changed = True
            save_failures(failure_log_path, failures)
            if not args.keep_going:
                raise
            continue
        if return_code == 0:
            processed[cmd_path] = version["mtime_ns"]
            if str(cmd_path) in failures:
                del failures[str(cmd_path)]
                failures_changed = True
            success += 1
            log(f"Done: {cmd_path}")
        else:
            failed += 1
            log(f"Failed: {cmd_path}: exit code {return_code}")
            record_failure(failures, cmd_path, version, f"exit code {return_code}")
            failures_changed = True
            save_failures(failure_log_path, failures)
            if not args.keep_going:
                raise SystemExit(return_code)
    if failures_changed:
        save_failures(failure_log_path, failures)
    return success, failed


def main():
    args = parse_args()
    output_dir = Path(args.output_dir).resolve()
    template_path = Path(args.template).resolve() if args.template else generate_native_template(output_dir)
    processed = {}
    failure_log_path = Path(args.failure_log).resolve() if args.failure_log else output_dir / ".llvmir-batch-failures.json"
    failures = load_failures(failure_log_path)

    if args.once:
        cmd_files = find_cmd_files(args.input_paths)
        if not cmd_files:
            raise SystemExit("No _cmd files found in input paths")
        success, failed = process_cmd_files(cmd_files, args, template_path, processed, failures, failure_log_path)
        log(f"Summary: successful={success} failed={failed}")
        return 0 if failed == 0 else 1

    log("Watching inputs for *_cmd files: " + ", ".join(args.input_paths))
    while True:
        process_cmd_files(find_cmd_files(args.input_paths), args, template_path, processed, failures, failure_log_path)
        time.sleep(args.scan_interval)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        log("Interrupted")