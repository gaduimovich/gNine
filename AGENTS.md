# AGENTS.md

Use `rtk` to prefix commands.

## Build

```bash
rtk cmake --build /Users/gduimovi/dev/gNine/build-arm64 --target gnine -j4
rtk cmake --build /Users/gduimovi/dev/gNine/build-arm64 --target gnine_runtime_tests gnine_semantic_tests -j4
```

## Test

```bash
rtk /Users/gduimovi/dev/gNine/build-arm64/tests/gnine_runtime_tests
rtk /Users/gduimovi/dev/gNine/build-arm64/tests/gnine_semantic_tests
```

## Run Examples

```bash
rtk /Users/gduimovi/dev/gNine/build-arm64/gnine /Users/gduimovi/dev/gNine/examples/blur.psm /Users/gduimovi/dev/gNine/example_data/lena.png /tmp/blur.png

rtk /Users/gduimovi/dev/gNine/build-arm64/gnine --runtime /Users/gduimovi/dev/gNine/examples/runtime_pong.psm /Users/gduimovi/dev/gNine/example_data/glider_gun.png /tmp/runtime_pong.png

rtk /Users/gduimovi/dev/gNine/build-arm64/gnine --runtime --emit-frames=/tmp/runtime_pong.png /Users/gduimovi/dev/gNine/examples/runtime_pong.psm /Users/gduimovi/dev/gNine/example_data/glider_gun.png /tmp/runtime_pong_final.png
```

## Make GIF

```bash
rtk /Users/gduimovi/dev/gNine/build-arm64/gnine --runtime --emit-frames=/tmp/runtime_pong.png /Users/gduimovi/dev/gNine/examples/runtime_pong.psm /Users/gduimovi/dev/gNine/example_data/glider_gun.png /tmp/runtime_pong_final.png

rtk magick -delay 4 -loop 0 /tmp/runtime_pong_*.png /tmp/runtime_pong.gif
```
