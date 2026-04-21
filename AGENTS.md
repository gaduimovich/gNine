# AGENTS.md

## Build

```bash
cmake --fresh --preset macos-x86_64
cmake --build --preset build-x86_64-tools -j4

cmake --fresh --preset macos-arm64
cmake --build --preset build-arm64-gnine -j4

cmake --build --preset build-x86_64-gnine -j4
cmake --build build-arm64 --target gnine_runtime_tests gnine_semantic_tests gnine_color_tests -j4
```

## Test

```bash
./build-arm64/tests/gnine_runtime_tests
./build-arm64/tests/gnine_semantic_tests
./build-arm64/tests/gnine_color_tests
```

## Run Examples

```bash
./build-arm64/gnine ./examples/box_3x3.psm ./example_data/lena.png /tmp/box_3x3.png

./build-arm64/gnine ./examples/sepia_vector.psm ./example_data/lena.png /tmp/sepia_vector.png

./build-arm64/gnine --runtime ./examples/runtime_pong_v2.psm /tmp/runtime_pong_v2.png

./build-arm64/gnine --runtime --preview ./examples/runtime_snake_v2.psm /tmp/runtime_snake_v2.png

./build-arm64/gnine --runtime --emit-frames=/tmp/runtime_pong_v2.png ./examples/runtime_pong_v2.psm /tmp/runtime_pong_v2_final.png
```

## Make GIF

```bash
./build-arm64/gnine --runtime --emit-frames=/tmp/runtime_pong_v2.png ./examples/runtime_pong_v2.psm /tmp/runtime_pong_v2_final.png

magick -delay 4 -loop 0 /tmp/runtime_pong_v2_*.png /tmp/runtime_pong_v2.gif
```

## Benchmark

Non-runtime filter benchmark:

```bash
./build-arm64/gnine --benchmark --benchmark-no-write ./examples/box_3x3.psm ./example_data/lena.png /tmp/box_3x3_bench.png
```

Single-pass runtime benchmark with warm repeats:

```bash
./build-arm64/gnine --runtime --benchmark --benchmark-no-write --benchmark-repeats=5 ./examples/canvas_bench_color_squares_1080p.psm /tmp/canvas_bench.png
```

Chained runtime game benchmark, Pong:

```bash
./build-arm64/gnine --runtime --benchmark --benchmark-no-write --chain-times=300 --benchmark-repeats=3 ./examples/runtime_pong_v2.psm /tmp/runtime_pong_v2_bench.png
```

Chained runtime game benchmark, Snake:

```bash
./build-arm64/gnine --runtime --benchmark --benchmark-no-write --chain-times=300 --benchmark-repeats=3 ./examples/runtime_snake_v2.psm /tmp/runtime_snake_v2_bench.png
```

For chained game benchmarks:

* `benchmark.avg_iter_ms` is the average per timed iteration across all repeats.
* `benchmark.first_repeat_avg_iter_ms` is the cold-run per-iteration average.
* `benchmark.last_repeat_avg_iter_ms` is the warm-run per-iteration average.
