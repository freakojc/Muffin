#!/usr/bin/env python3
"""Format tracked, project-owned C/C++ sources, or check them with --check."""

import argparse
import concurrent.futures
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import sysconfig


ROOT = Path(__file__).resolve().parents[1]
EXTENSIONS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".m", ".mm"}
SOURCE_ROOTS = {"src", "tests", "scripts"}
EXCLUDED = ("tests/vendor/", "tests/fixtures/")


def source_files():
    tracked = subprocess.check_output(["git", "ls-files", "-z"], cwd=ROOT)
    files = []
    for entry in tracked.split(b"\0"):
        if not entry:
            continue
        name = os.fsdecode(entry)
        path = Path(name)
        if (path.parts[0] in SOURCE_ROOTS and path.suffix.lower() in EXTENSIONS
                and not name.startswith(EXCLUDED) and (ROOT / path).is_file()):
            files.append(name)
    return files


def formatter_path(explicit):
    if explicit:
        path = Path(explicit).expanduser()
        return str(path.resolve()) if path.is_file() else explicit
    # Prefer the pinned package belonging to this Python (including a venv).
    scripts = Path(sysconfig.get_path("scripts"))
    for name in ("clang-format.exe", "clang-format"):
        candidate = scripts / name
        if candidate.is_file():
            return str(candidate)
    return shutil.which("clang-format")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="report formatting differences without modifying files")
    parser.add_argument("--clang-format", metavar="PATH", help="use a specific clang-format executable")
    args = parser.parse_args()
    pinned = re.search(r"^clang-format==([\d.]+)$", (ROOT / "requirements-format.txt").read_text(), re.MULTILINE).group(1)
    formatter = formatter_path(args.clang_format)
    if not formatter:
        parser.exit(2, "clang-format is missing. Run: python -m pip install -r requirements-format.txt\n")
    try:
        version = subprocess.check_output([formatter, "--version"], text=True)
        if not re.search(rf"\bversion {re.escape(pinned)}\b", version):
            parser.exit(2, f"clang-format {pinned} is required; found {version.strip()}\n")
        files = source_files()
        mode = ["--dry-run", "--Werror", "--ferror-limit=1"] if args.check else ["-i"]
        command = [formatter, "--style=file", "--fail-on-incomplete-format", *mode]
        batches = [files[start : start + 24] for start in range(0, len(files), 24)]
        failed = False
        with concurrent.futures.ThreadPoolExecutor(max_workers=min(8, os.cpu_count() or 1)) as executor:
            jobs = [
                executor.submit(subprocess.run, [*command, *batch], cwd=ROOT, capture_output=True)
                for batch in batches
            ]
            for job in concurrent.futures.as_completed(jobs):
                result = job.result()
                if result.returncode:
                    failed = True
                    sys.stderr.write(result.stderr.decode("utf-8", errors="replace"))
        if failed:
            return 1
        print(f"{'Checked' if args.check else 'Formatted'} {len(files)} C/C++ files with clang-format {pinned}.")
        return 0
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"Formatting failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
