#!/usr/bin/env python3

import argparse
import json
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path


MARKER = "<!-- gnine-gameplay-comment -->"
FRAME_RATE = 60


@dataclass(frozen=True)
class CaptureCase:
    name: str
    scenario: str
    example_path: str
    display_scale: int
    duration_ms: int


CAPTURES = (
    CaptureCase(
        name="snake",
        scenario="snake",
        example_path="examples/runtime_snake_v2.psm",
        display_scale=20,
        duration_ms=10000,
    ),
    CaptureCase(
        name="pong",
        scenario="pong",
        example_path="examples/runtime_pong_v2.psm",
        display_scale=4,
        duration_ms=10000,
    ),
    CaptureCase(
        name="flappy",
        scenario="flappy",
        example_path="examples/runtime_flappy_bird_v2.psm",
        display_scale=4,
        duration_ms=10000,
    ),
    CaptureCase(
        name="brick_breaker",
        scenario="brick-breaker",
        example_path="examples/runtime_brick_breaker_v2.psm",
        display_scale=4,
        duration_ms=10000,
    ),
)


def pretty_name(name: str) -> str:
    return name.replace("_", " ").title()


def run(command: list[str], cwd: Path) -> None:
    subprocess.run(command, cwd=cwd, check=True)


def run_capture(binary: Path, repo_root: Path, case: CaptureCase, output_dir: Path) -> dict[str, object]:
    case_dir = output_dir / case.name
    frames_dir = case_dir / "frames"
    if case_dir.exists():
        shutil.rmtree(case_dir)
    case_dir.mkdir(parents=True, exist_ok=True)
    frames_dir.mkdir(parents=True, exist_ok=True)

    frame_prefix = frames_dir / "frame.png"
    output_image = case_dir / "final.png"
    command = [
        str(binary),
        "--runtime",
        f"--preview-playback={case.scenario}",
        f"--preview-duration-ms={case.duration_ms}",
        f"--display-scale={case.display_scale}",
        f"--emit-frames={frame_prefix}",
        str(repo_root / case.example_path),
        str(output_image),
    ]
    run(command, cwd=repo_root)

    frame_files = sorted(frames_dir.glob("frame_*.png"))
    if not frame_files:
        raise RuntimeError(f"no frames were produced for {case.name}")

    gif_path = case_dir / f"{case.name}.gif"
    if shutil.which("ffmpeg") is not None:
        palette_path = case_dir / "palette.png"
        run(
            [
                "ffmpeg",
                "-y",
                "-framerate",
                str(FRAME_RATE),
                "-i",
                str(frames_dir / "frame_%04d.png"),
                "-vf",
                "fps=20,scale=iw:-1:flags=lanczos,palettegen",
                "-frames:v",
                "1",
                "-update",
                "1",
                str(palette_path),
            ],
            cwd=case_dir,
        )
        run(
            [
                "ffmpeg",
                "-y",
                "-framerate",
                str(FRAME_RATE),
                "-i",
                str(frames_dir / "frame_%04d.png"),
                "-i",
                str(palette_path),
                "-lavfi",
                "fps=20,scale=iw:-1:flags=lanczos[x];[x][1:v]paletteuse",
                "-loop",
                "0",
                str(gif_path),
            ],
            cwd=case_dir,
        )
    elif shutil.which("magick") is not None:
        run(
            [
                "magick",
                "-delay",
                str(max(1, int(round(100.0 / FRAME_RATE)))),
                "-loop",
                "0",
                *[str(frame) for frame in frame_files],
                str(gif_path),
            ],
            cwd=case_dir,
        )
    else:
        raise RuntimeError("ffmpeg or magick is required to generate GIF captures")

    return {
        "name": case.name,
        "scenario": case.scenario,
        "duration_ms": case.duration_ms,
        "display_scale": case.display_scale,
        "frame_count": len(frame_files),
        "frames_dir": str(frames_dir.relative_to(output_dir)),
        "output_image": str(output_image.relative_to(output_dir)),
        "gif": str(gif_path.relative_to(output_dir)) if gif_path.exists() else None,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--output-json", required=True)
    parser.add_argument("--output-markdown", required=True)
    parser.add_argument("--run-url", required=True)
    parser.add_argument("--inline-base-url", required=True)
    args = parser.parse_args()

    repo_root = Path(args.repo_root).resolve()
    binary = Path(args.binary).resolve()
    output_dir = Path(args.output_dir).resolve()
    output_json = Path(args.output_json).resolve()
    output_markdown = Path(args.output_markdown).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    results = []
    for case in CAPTURES:
        results.append(run_capture(binary, repo_root, case, output_dir))

    payload = {
        "run_url": args.run_url,
        "captures": results,
    }
    output_json.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")

    lines = [
        MARKER,
        "## Gameplay captures",
        "",
        f"Workflow run: {args.run_url}",
        "",
    ]
    for capture in results:
        lines.append(f"### {pretty_name(str(capture['name']))}")
        lines.append(f"- Scenario: `{capture['scenario']}`")
        lines.append(f"- Duration: `{capture['duration_ms']} ms`")
        lines.append(f"- Frames: `{capture['frame_count']}`")
        if capture["gif"] is not None:
            lines.append(f"- GIF: `{capture['gif']}`")
            inline_url = f"{args.inline_base_url}/runtime_{capture['name']}.gif"
            lines.append(f"![{pretty_name(str(capture['name']))}]({inline_url})")
        else:
            lines.append(f"- Frames: `{capture['frames_dir']}`")
        lines.append(f"- Cover: `{capture['output_image']}`")
        lines.append("")
    lines.append("Artifacts are attached to this workflow run.")
    output_markdown.write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
