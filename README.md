# G9 - Just In Time Image Processing With Eclipse OMR

An implementation of Luke Dodd's Pixslam https://github.com/lukedodd/Pixslam using Eclipse OMR JitBuilder.
See [Original README](README_ORIG.md) for the original project README.

#### Carleton University Honours Project Winter 2019. Under the supervision of David Mould. ####

## Getting Started

### Installing

Requires CMake and a C++ compiler. SDL2 is optional and enables `--preview` for runtime programs.
Works on macOS and Ubuntu Linux.

```bash
git clone --recurse-submodules https://github.com/gaduimovich/gNine
cd gNine
cmake -S . -B build
cmake --build build --target gnine gnine_runtime_tests gnine_semantic_tests gnine_color_tests -j4
```

### Running

Box Filter
```sh
./build/gnine ./examples/box_3x3.psm ./example_data/lena.png out.png
```

Runtime RGB Triptych
```sh
./build/gnine --runtime ./examples/rgb_triptych.psm ./example_data/lena.png rgb_triptych.png
```

Runtime Pong
```sh
./build/gnine --runtime --emit-frames=runtime_pong.png ./examples/runtime_pong.psm ./example_data/glider_gun.png runtime_pong_final.png
```

### Testing

Build the test targets:

```bash
cmake --build build-arm64 --target gnine_runtime_tests gnine_semantic_tests gnine_color_tests -j4
cmake --build build-x86_64 --target gnine_runtime_tests gnine_semantic_tests gnine_color_tests -j4
```

Run the test binaries:

```bash
./build-arm64/tests/gnine_runtime_tests
./build-arm64/tests/gnine_semantic_tests
./build-x86_64/tests/gnine_runtime_tests
./build-x86_64/tests/gnine_semantic_tests
```

## Examples

* `box_3x3.psm`, `box_5x5.psm`, `threshold.psm`, `posterize_4.psm` - basic image filters
* `abs_edges.psm`, `halo_edges.psm`, `detail_boost.psm`, `midtones_notch.psm` - edge and detail effects
* `compose.psm`, `min.psm` - compositing examples
* `metaballs.psm`, `metaballs_binary.psm`, `metaballs_fancy.psm` - procedural image generation
* `sepia_vector.psm`, `chroma_key_green.psm`, `rgb_triptych.psm` - RGB and vector color examples
* `runtime_pong.psm` - stateful runtime demo built with `iterate-until`
* `runtime_snake.psm` - interactive runtime preview demo built with `iterate-until`
* `examples/game_of_life/game_of_life.psm` - chained cellular automata example

## New Features

* `--runtime` executes managed image programs and can JIT fast paths for `map-image`, `zip-image`, and `canvas`.
* Top-level `iterate`, `iterate-state`, and `iterate-until` forms support chained execution, explicit runtime state, and stop conditions.
* Runtime tuple state supports `tuple`, `get`, and tuple destructuring in arguments and lambda parameters.
* RGB and vector lowering add `vec`, `rgb`, `color`, `r`, `g`, `b`, and `dot`.
* Runtime canvas generation adds `(canvas W H [C] expr)` plus `draw-rect` and `draw-circle`.
* `pipeline` fuses scalar stages into a single lowered kernel.
* Output helpers add `--emit-frames`, `--compare`, `--display-scale`, and `--display-size`.
* Runtime preview/input bindings add `--preview` plus keyboard, mouse, and frame-time inputs for interactive programs.
* Benchmarking helpers add `--benchmark`, `--benchmark-no-write`, and `--benchmark-repeats=N`.

## Runtime Demo

![Runtime Pong](readme_images/runtime_pong.gif)

## Performance

* Runtime `map-image`, `zip-image`, and `canvas` can reuse cached compiled kernels instead of recompiling the same symbolic program every pass.
* Runtime `zip-image` now has its own compiled fast path instead of always falling back to interpreted pixel evaluation.
* Three-channel runtime canvas rendering can use fused RGB kernels instead of issuing one scalar pass per channel.
* Scalar capture images used by compiled runtime paths are reused across runs to reduce allocation and warm-cache overhead.
* Lowered arithmetic trees for large `+` and `*` expressions are built with balanced folds to avoid deep linear expression chains.
* Row-invariant `define` expressions are hoisted out of the inner pixel loop during lowering.
* `pipeline` can fuse multi-stage scalar programs into one lowered kernel and avoid writing intermediate images.

## Built With

* [stb_image](http://nothings.org/stb_image.c) - image reading/writing.
* [Eclipse OMR](https://github.com/eclipse/omr) - high performance runtime technology.

## Authors

* **Geoffrey Duimovich** - *Adding OMR* - (https://github.com/gduimovi)
* **Luke Dodd** - *Original idea and base code* - (https://github.com/lukedodd)

## Acknowledgments

* Luke Dodd <https://github.com/lukedodd>
