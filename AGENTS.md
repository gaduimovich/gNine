# AGENTS.md

Use `rtk` to prefix commands.

## Build

```bash
rtk cmake --fresh --preset macos-x86_64
rtk cmake --build --preset build-x86_64-tools -j4

rtk cmake --fresh --preset macos-arm64
rtk cmake --build --preset build-arm64-gnine -j4

rtk cmake --build --preset build-x86_64-gnine -j4
rtk cmake --build /Users/gduimovi/dev/gNine/build-arm64 --target gnine_runtime_tests gnine_semantic_tests gnine_color_tests -j4
```

## Test

```bash
rtk /Users/gduimovi/dev/gNine/build-arm64/tests/gnine_runtime_tests
rtk /Users/gduimovi/dev/gNine/build-arm64/tests/gnine_semantic_tests
rtk /Users/gduimovi/dev/gNine/build-arm64/tests/gnine_color_tests
```

## Run Examples

```bash
rtk /Users/gduimovi/dev/gNine/build-arm64/gnine /Users/gduimovi/dev/gNine/examples/box_3x3.psm /Users/gduimovi/dev/gNine/example_data/lena.png /tmp/box_3x3.png

rtk /Users/gduimovi/dev/gNine/build-arm64/gnine /Users/gduimovi/dev/gNine/examples/sepia_vector.psm /Users/gduimovi/dev/gNine/example_data/lena.png /tmp/sepia_vector.png

rtk /Users/gduimovi/dev/gNine/build-arm64/gnine --runtime /Users/gduimovi/dev/gNine/examples/runtime_pong.psm /Users/gduimovi/dev/gNine/example_data/glider_gun.png /tmp/runtime_pong.png

rtk /Users/gduimovi/dev/gNine/build-arm64/gnine --runtime --preview /Users/gduimovi/dev/gNine/examples/runtime_snake.psm /tmp/runtime_snake.png

rtk /Users/gduimovi/dev/gNine/build-arm64/gnine --runtime --emit-frames=/tmp/runtime_pong.png /Users/gduimovi/dev/gNine/examples/runtime_pong.psm /Users/gduimovi/dev/gNine/example_data/glider_gun.png /tmp/runtime_pong_final.png
```

## Make GIF

```bash
rtk /Users/gduimovi/dev/gNine/build-arm64/gnine --runtime --emit-frames=/tmp/runtime_pong.png /Users/gduimovi/dev/gNine/examples/runtime_pong.psm /Users/gduimovi/dev/gNine/example_data/glider_gun.png /tmp/runtime_pong_final.png

rtk magick -delay 4 -loop 0 /tmp/runtime_pong_*.png /tmp/runtime_pong.gif
```
