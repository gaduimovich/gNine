# AGENTS.md

## Build

```bash
cmake --fresh --preset macos-x86_64
cmake --build --preset build-x86_64-tools -j4

cmake --fresh --preset macos-arm64
cmake --build --preset build-arm64-gnine -j4

cmake --build --preset build-x86_64-gnine -j4
cmake --build /Users/gduimovi/dev/gNine/build-arm64 --target gnine_runtime_tests gnine_semantic_tests gnine_color_tests -j4
```

## Test

```bash
/Users/gduimovi/dev/gNine/build-arm64/tests/gnine_runtime_tests
/Users/gduimovi/dev/gNine/build-arm64/tests/gnine_semantic_tests
/Users/gduimovi/dev/gNine/build-arm64/tests/gnine_color_tests
```

## Run Examples

```bash
/Users/gduimovi/dev/gNine/build-arm64/gnine /Users/gduimovi/dev/gNine/examples/box_3x3.psm /Users/gduimovi/dev/gNine/example_data/lena.png /tmp/box_3x3.png

/Users/gduimovi/dev/gNine/build-arm64/gnine /Users/gduimovi/dev/gNine/examples/sepia_vector.psm /Users/gduimovi/dev/gNine/example_data/lena.png /tmp/sepia_vector.png

/Users/gduimovi/dev/gNine/build-arm64/gnine --runtime /Users/gduimovi/dev/gNine/examples/runtime_pong.psm /tmp/runtime_pong.png

/Users/gduimovi/dev/gNine/build-arm64/gnine --runtime --preview /Users/gduimovi/dev/gNine/examples/runtime_snake.psm /tmp/runtime_snake.png

/Users/gduimovi/dev/gNine/build-arm64/gnine --runtime --emit-frames=/tmp/runtime_pong.png /Users/gduimovi/dev/gNine/examples/runtime_pong.psm /tmp/runtime_pong_final.png
```

## Make GIF

```bash
/Users/gduimovi/dev/gNine/build-arm64/gnine --runtime --emit-frames=/tmp/runtime_pong.png /Users/gduimovi/dev/gNine/examples/runtime_pong.psm /tmp/runtime_pong_final.png

magick -delay 4 -loop 0 /tmp/runtime_pong_*.png /tmp/runtime_pong.gif
```

## Benchmark

Non-runtime filter benchmark:

```bash
/Users/gduimovi/dev/gNine/build-arm64/gnine --benchmark --benchmark-no-write /Users/gduimovi/dev/gNine/examples/box_3x3.psm /Users/gduimovi/dev/gNine/example_data/lena.png /tmp/box_3x3_bench.png
```

Single-pass runtime benchmark with warm repeats:

```bash
/Users/gduimovi/dev/gNine/build-arm64/gnine --runtime --benchmark --benchmark-no-write --benchmark-repeats=5 /Users/gduimovi/dev/gNine/examples/canvas_bench_color_squares_1080p.psm /tmp/canvas_bench.png
```

Chained runtime game benchmark, Pong:

```bash
/Users/gduimovi/dev/gNine/build-arm64/gnine --runtime --benchmark --benchmark-no-write --chain-times=300 --benchmark-repeats=3 /Users/gduimovi/dev/gNine/examples/runtime_pong.psm /tmp/runtime_pong_bench.png
```

Chained runtime game benchmark, Snake:

```bash
/Users/gduimovi/dev/gNine/build-arm64/gnine --runtime --benchmark --benchmark-no-write --chain-times=300 --benchmark-repeats=3 /Users/gduimovi/dev/gNine/examples/runtime_snake.psm /tmp/runtime_snake_bench.png
```

For chained game benchmarks:

* `benchmark.avg_iter_ms` is the average per timed iteration across all repeats.
* `benchmark.first_repeat_avg_iter_ms` is the cold-run per-iteration average.
* `benchmark.last_repeat_avg_iter_ms` is the warm-run per-iteration average.
