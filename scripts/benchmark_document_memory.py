"""Reproducible fixtures and fresh-process Windows GUI memory comparison.

python scripts/benchmark_document_memory.py --output build/document-memory-before
Requires a Release MuffinDocumentTabsTest and Conan-generated CMake presets.
"""

import argparse
import json
import os
from pathlib import Path
import statistics
import subprocess


def fixtures(root):
    target = root / "build/document-memory-fixtures"
    target.mkdir(exist_ok=True)
    samples = {
        "chinese": "# 写作笔记\n\n" + ("今天整理项目的思路，记录读书时的发现。文字应当清楚自然，操作也应流畅可靠。\n" * 12) + "\n",
        "dense": "# 项目笔记\n\n" + ("中文 **重点**、*说明*、~~旧文~~、`code`、[参考](notes.md#标题)，以及 ==标记==。\n" * 8) + "\n- 第一项 **待办**\n- 第二项 [方案](plan.md)\n\n| 名称 | 状态 |\n| --- | --- |\n| 项目 | **进行中** |\n\n",
        "code": 'const auto value = calculate(input, 123); // 中文说明\n',
    }
    result = []
    for mib in (1, 10):
        for kind, sample in samples.items():
            unit = sample.encode("utf-8")
            count = (mib * 1024 * 1024 + len(unit) - 1) // len(unit)
            data = unit * count
            if kind == "code":
                data = b"```cpp\n" + data + b"```\n"
            path = target / f"{kind}-{mib}mib.md"
            if not path.exists() or path.read_bytes() != data:
                path.write_bytes(data)
            result.append(path)
    return result


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--exe", type=Path, default=Path("build/Release/MuffinDocumentTabsTest.exe"))
    parser.add_argument("--repeats", type=int, default=2)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    presets = json.loads((root / "build/generators/CMakePresets.json").read_text(encoding="utf-8"))
    qt = presets["configurePresets"][0]["environment"]["QT_HOST_PATH"]
    env["PATH"] = str(Path(qt) / "bin") + os.pathsep + env["PATH"]
    env["QT_PLUGIN_PATH"] = presets["testPresets"][0]["environment"]["QT_PLUGIN_PATH"]
    env["QT_QPA_PLATFORM"] = "windows"
    env["QT_SCALE_FACTOR"] = "1"
    env["QT_FONT_DPI"] = "96"
    for fixture in fixtures(root):
        for repeat in range(args.repeats):
            name = f"{fixture.stem}-{repeat + 1}"
            report = output / f"{name}.json"
            env["MUFFIN_BENCH_FILE"] = str(fixture)
            env["MUFFIN_MEMORY_BENCH_OUTPUT"] = str(report)
            with (output / f"{name}.log").open("wb") as log:
                subprocess.run([str(args.exe.resolve()), "--document-memory"], env=env,
                               stdout=log, stderr=subprocess.STDOUT, timeout=180, check=True)
            row = json.loads(report.read_text(encoding="utf-8"))
            stable = statistics.median(s["private_commit_bytes"] for s in row["stable_samples"])
            print(f"{name}: open={row['open_ms']}ms peak private={row['sampled_peak_private_commit_bytes']/2**20:.2f}MiB "
                  f"stable private={stable/2**20:.2f}MiB", flush=True)


if __name__ == "__main__":
    main()
