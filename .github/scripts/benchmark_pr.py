#!/usr/bin/env python3

import argparse
import json
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path


MARKER = "<!-- gnine-benchmark-comment -->"
BENCHMARK_THRESHOLD_PERCENT = 2.0


@dataclass(frozen=True)
class BenchmarkCase:
    name: str
    tracked_metric: str
    description: str
    command: tuple[str, ...]


BENCHMARKS = (
    BenchmarkCase(
        name="box_3x3",
        tracked_metric="benchmark.avg_iter_ms",
        description="Non-runtime filter",
        command=(
            "--benchmark",
            "--benchmark-no-write",
            "--times=50",
            "examples/box_3x3.psm",
            "example_data/lena.png",
            "{output}",
        ),
    ),
    BenchmarkCase(
        name="sepia_vector",
        tracked_metric="benchmark.avg_iter_ms",
        description="Vector color filter",
        command=(
            "--benchmark",
            "--benchmark-no-write",
            "--times=50",
            "examples/sepia_vector.psm",
            "example_data/lena.png",
            "{output}",
        ),
    ),
    BenchmarkCase(
        name="rgb_triptych",
        tracked_metric="benchmark.last_repeat_avg_iter_ms",
        description="Runtime single-pass benchmark",
        command=(
            "--runtime",
            "--benchmark",
            "--benchmark-no-write",
            "--benchmark-repeats=3",
            "examples/rgb_triptych.psm",
            "example_data/lena.png",
            "{output}",
        ),
    ),
    BenchmarkCase(
        name="runtime_pong_v2",
        tracked_metric="benchmark.last_repeat_avg_iter_ms",
        description="Runtime chained benchmark",
        command=(
            "--runtime",
            "--benchmark",
            "--benchmark-no-write",
            "--chain-times=150",
            "--benchmark-repeats=3",
            "examples/runtime_pong_v2.psm",
            "{output}",
        ),
    ),
    BenchmarkCase(
        name="runtime_snake_v2",
        tracked_metric="benchmark.last_repeat_avg_iter_ms",
        description="Runtime chained benchmark",
        command=(
            "--runtime",
            "--benchmark",
            "--benchmark-no-write",
            "--chain-times=150",
            "--benchmark-repeats=3",
            "examples/runtime_snake_v2.psm",
            "{output}",
        ),
    ),
)


def run(command: list[str], cwd: Path) -> str:
    completed = subprocess.run(
        command,
        cwd=cwd,
        check=True,
        text=True,
        capture_output=True,
    )
    return completed.stdout + completed.stderr


def parse_metrics(log_text: str) -> dict[str, float | str]:
    metrics: dict[str, float | str] = {}
    for line in log_text.splitlines():
        if not line.startswith("benchmark.") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        try:
            metrics[key] = float(value)
        except ValueError:
            metrics[key] = value
    return metrics


def make_worktree(repo_root: Path, ref: str, name: str, temp_root: Path) -> Path:
    worktree_path = temp_root / name
    subprocess.run(
        ["git", "-C", str(repo_root), "worktree", "add", "--detach", str(worktree_path), ref],
        check=True,
        capture_output=True,
        text=True,
    )
    subprocess.run(
        ["git", "-C", str(worktree_path), "submodule", "update", "--init", "--recursive"],
        check=True,
        capture_output=True,
        text=True,
    )
    return worktree_path


def remove_worktree(repo_root: Path, worktree_path: Path) -> None:
    if not worktree_path.exists():
        return
    subprocess.run(
        ["git", "-C", str(repo_root), "worktree", "remove", "--force", str(worktree_path)],
        check=True,
        capture_output=True,
        text=True,
    )


def build_gnine(source_dir: Path, build_dir: Path) -> Path:
    subprocess.run(
        [
            "cmake",
            "-S",
            str(source_dir),
            "-B",
            str(build_dir),
            "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
            "-DGNINE_ENABLE_SDL_PREVIEW=OFF",
        ],
        check=True,
    )
    subprocess.run(
        ["cmake", "--build", str(build_dir), "--target", "gnine", "-j4"],
        check=True,
    )
    return build_dir / "gnine"


def benchmark_case(binary_path: Path, source_dir: Path, build_dir: Path, case: BenchmarkCase, log_path: Path) -> dict[str, float | str]:
    output_path = build_dir / f"{case.name}.png"
    command = [
        str(binary_path),
        *[
            part.format(output=str(output_path))
            if part == "{output}"
            else str(source_dir / part)
            if part.endswith(".psm") or part.endswith(".png")
            else part
            for part in case.command
        ],
    ]
    log_text = run(command, cwd=build_dir)
    log_path.write_text(log_text)
    metrics = parse_metrics(log_text)
    if case.tracked_metric not in metrics:
        raise RuntimeError(f"Missing {case.tracked_metric} for {case.name}")
    return metrics


def classify_change(delta_percent: float) -> tuple[str, str]:
    if delta_percent <= -BENCHMARK_THRESHOLD_PERCENT:
        return "faster", "✅"
    if delta_percent >= BENCHMARK_THRESHOLD_PERCENT:
        return "slower", "⚠️"
    return "steady", "➖"


def format_delta(delta_percent: float) -> str:
    sign = "+" if delta_percent > 0 else ""
    return f"{sign}{delta_percent:.2f}%"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--base-ref", required=True)
    parser.add_argument("--head-ref", required=True)
    parser.add_argument("--output-json", required=True)
    parser.add_argument("--output-markdown", required=True)
    parser.add_argument("--logs-dir", required=True)
    args = parser.parse_args()

    repo_root = Path(args.repo_root).resolve()
    output_json = Path(args.output_json).resolve()
    output_markdown = Path(args.output_markdown).resolve()
    logs_dir = Path(args.logs_dir).resolve()
    logs_dir.mkdir(parents=True, exist_ok=True)

    results = []
    summary = {"faster": 0, "slower": 0, "steady": 0}

    with tempfile.TemporaryDirectory(prefix="gnine-bench-") as temp_dir:
        temp_root = Path(temp_dir)
        worktrees: dict[str, Path] = {}
        try:
            for name, ref in (("base", args.base_ref), ("head", args.head_ref)):
                worktrees[name] = make_worktree(repo_root, ref, name, temp_root)

            builds = {
                "base": temp_root / "build-base",
                "head": temp_root / "build-head",
            }
            binaries = {
                name: build_gnine(worktrees[name], builds[name])
                for name in ("base", "head")
            }

            for case in BENCHMARKS:
                base_log = logs_dir / f"base-{case.name}.log"
                head_log = logs_dir / f"head-{case.name}.log"
                base_metrics = benchmark_case(binaries["base"], worktrees["base"], builds["base"], case, base_log)
                head_metrics = benchmark_case(binaries["head"], worktrees["head"], builds["head"], case, head_log)
                base_value = float(base_metrics[case.tracked_metric])
                head_value = float(head_metrics[case.tracked_metric])
                delta_percent = ((head_value - base_value) / base_value) * 100.0 if base_value else 0.0
                change_type, emoji = classify_change(delta_percent)
                summary[change_type] += 1
                results.append(
                    {
                        "name": case.name,
                        "description": case.description,
                        "tracked_metric": case.tracked_metric,
                        "base": {"value": base_value, "metrics": base_metrics},
                        "head": {"value": head_value, "metrics": head_metrics},
                        "delta_percent": delta_percent,
                        "change_type": change_type,
                        "emoji": emoji,
                    }
                )
        finally:
            for worktree_path in worktrees.values():
                remove_worktree(repo_root, worktree_path)

    payload = {
        "base_ref": args.base_ref,
        "head_ref": args.head_ref,
        "threshold_percent": BENCHMARK_THRESHOLD_PERCENT,
        "summary": summary,
        "benchmarks": results,
    }
    output_json.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")

    lines = [
        MARKER,
        "## Benchmark results",
        "",
        f"Compared `{args.base_ref[:12]}` → `{args.head_ref[:12]}` using the tracked benchmark metric for each case.",
        "",
        "| Benchmark | Metric | Base | PR | Change |",
        "| --- | --- | ---: | ---: | --- |",
    ]
    for result in results:
        lines.append(
            f"| {result['name']} | `{result['tracked_metric']}` | "
            f"{result['base']['value']:.4f} ms | {result['head']['value']:.4f} ms | "
            f"{result['emoji']} {format_delta(result['delta_percent'])} |"
        )
    lines.extend(
        [
            "",
            f"- Faster: {summary['faster']}",
            f"- Slower: {summary['slower']}",
            f"- Within ±{BENCHMARK_THRESHOLD_PERCENT:.0f}%: {summary['steady']}",
            "",
            "Raw logs and JSON tracking data are attached to this workflow run as artifacts.",
        ]
    )
    output_markdown.write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
