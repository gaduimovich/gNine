# gNine Language Guide

gNine is a small Lisp-like language for image processing, procedural image generation, and simple stateful runtime programs.

It has two distinct execution models:

- Default mode compiles a pixel program into a JIT kernel. This is the best fit for filters, compositing, neighborhood operations, and most stateless image work.
- `--runtime` runs a managed evaluator with closures, tuples, `canvas`, `map-image`, `zip-image`, and stateful chaining forms for demos and games.

The examples in [`examples/`](../examples) are the best reference programs after this guide, especially [`runtime_pong_v2.psm`](../examples/runtime_pong_v2.psm), [`runtime_snake_v2.psm`](../examples/runtime_snake_v2.psm), [`rgb_triptych.psm`](../examples/rgb_triptych.psm), [`sepia_vector.psm`](../examples/sepia_vector.psm), and [`game_of_life.psm`](../examples/game_of_life/game_of_life.psm).

## Core Syntax

Programs use s-expressions:

```lisp
((Arg1 Arg2 ...)
  (define name expr)
  ...
  result-expr)
```

Rules:

- Comments start with `;` and continue to end of line.
- Numbers are doubles.
- A program may contain any number of `define` forms, followed by exactly one result expression.
- `(() ...)` is a valid program with no input images.

Images are stored as normalized doubles. On load, image channels are scaled from `[0,255]` into roughly `[0,1]`. On write, values are clamped back into displayable image channels, so staying in `[0,1]` is the practical convention.

## Execution Model

### 1. Compiled scalar programs

A normal program such as:

```lisp
((A B)
  (* 0.5 (+ A B)))
```

is compiled into a kernel that runs once per pixel.

In this mode:

- Writing `A` means "the current pixel from image `A`".
- `(A di dj)` means relative sampling from the current pixel.
- `(@A y x)` means absolute sampling.
- Relative and absolute samples use reflected borders by default.
- `--danger` disables border reflection and assumes every sample is already in range.
- The output image size matches the input image size.

Special symbols in compiled pixel code:

- `i`: current row
- `j`: current column
- `c`: current output channel
- `iter`: current 1-based chain iteration, or `1` for a normal single pass
- `width`: current image width
- `height`: current image height

### 2. Runtime programs

With `--runtime`, the program is evaluated by the runtime and can return:

- a number
- an image
- a tuple

Runtime mode adds higher-level forms such as `lambda`, `tuple`, `get`, `map-image`, `zip-image`, and `canvas`.

This is the mode used by the Pong and Snake examples.

In runtime mode, bound image names evaluate to image objects, not current pixel values. Metadata is queried with:

- `(width A)`
- `(height A)`
- `(channels A)`

## Numeric and Boolean Forms

Available arithmetic:

- `+`, `-`, `*`, `/`
- `min`, `max`
- `abs`
- `clamp`
- `int`

Available comparisons and logic:

- `<`, `<=`, `>`, `>=`, `==`, `!=`
- `and`, `or`, `not`
- `if`

Notes:

- `+`, `*`, `min`, and `max` accept one or more arguments.
- `-` supports unary negation and left-associative subtraction.
- `/` is left-associative division.
- Booleans are represented as numbers: `0` is false, non-zero is true.
- `if` has the form `(if cond then else)`.

Less common compiled-only builtin:

- `fib`

Example:

```lisp
((A)
  (if (> A 0.5) 1 0))
```

## Sampling Images

### Current-pixel access

In compiled scalar code, a bare image symbol reads the current pixel:

```lisp
((A) A)
```

### Relative access

Relative indexing is the main tool for filters:

```lisp
((A)
  (/ (+ (A -1 0) A (A 1 0)) 3))
```

### Absolute access

Absolute indexing is useful for geometry, remapping, and pipeline stages:

```lisp
((A)
  (@A i (- width j 1)))
```

By default, out-of-bounds coordinates reflect back into the image. This is useful for blur kernels and other neighborhood code because you do not need to write border branches.

## Define, Lambdas, and Tuples

`define` binds a name inside the program:

```lisp
((A)
  (define x (+ A 1))
  (* x x))
```

Runtime mode adds first-class functions:

```lisp
(() 
  (define make-adder (lambda (x) (lambda (y) (+ x y))))
  ((make-adder 4) 9))
```

Runtime tuples are built with `tuple` and indexed with `get`:

```lisp
(() 
  (define state (tuple 7 9))
  (+ (get state 0) (get state 1)))
```

Both top-level runtime arguments and lambda parameters may destructure tuples:

```lisp
((pair)
  ((lambda ((x y)) (+ x y)) pair))
```

This matters for games, because state is usually passed around as one tuple.

Runtime arrays are useful when the state is sparse or variable-sized. They are
built with `array` and updated immutably with helpers like `array-set`,
`array-push`, `array-pop`, and `array-slice`:

```lisp
(() 
  (define xs (array 10 20 30))
  (define ys (array-push xs 40))
  (array-get ys 3))
```

For reductions, use `array-fold`:

```lisp
(() 
  (array-fold (lambda (acc x) (+ acc x)) 0 (array 1 2 3 4)))
```

## Color and Vector Programs

If you write only scalar expressions, gNine runs the program channel-wise on RGB inputs. In that mode:

- `A` means "the current channel of `A`"
- grayscale inputs broadcast across RGB outputs
- `c` tells you which channel you are rendering

For whole-color logic, use vector forms:

- `vec` or `rgb` to build a 3-component vector
- `color` to sample a full RGB pixel
- `r`, `g`, `b` to extract components
- `dot` for dot products

Example:

```lisp
((A)
  (vec
    (clamp (+ (* 0.393 (r (color A))) (* 0.769 (g (color A))) (* 0.189 (b (color A)))) 0 1)
    (clamp (+ (* 0.349 (r (color A))) (* 0.686 (g (color A))) (* 0.168 (b (color A)))) 0 1)
    (clamp (+ (* 0.272 (r (color A))) (* 0.534 (g (color A))) (* 0.131 (b (color A)))) 0 1)))
```

Important rule:

- Once a program uses vector forms, use explicit `(color A)` sampling instead of bare `A` or `(A di dj)`.

That restriction keeps vector lowering simple and predictable.

## Runtime Image Forms

### `map-image`

Applies a function to every pixel of one image:

```lisp
(map-image (lambda (x) (+ x bias)) A)
```

Inside the lambda, `i`, `j`, and `c` are available.

Inside runtime pixel contexts, image objects are also callable for relative sampling:

```lisp
(canvas W H
  (A 0 -3))
```

That samples image `A` at the current runtime pixel plus the given row and column offsets, with reflected borders.

### `sample-image`

Reads one pixel from a runtime image at absolute coordinates:

```lisp
(sample-image A x y)
(sample-image A x y c)
```

Notes:

- `x` is the absolute column.
- `y` is the absolute row.
- `c` is optional and defaults to channel `0`.
- Out-of-bounds coordinates and channel indices reflect using the same border policy as other gNine sampling forms.
- Single-channel images always read channel `0`, even if `c` is provided.

### `zip-image`

Combines two images pixel-by-pixel:

```lisp
(zip-image (lambda (x y) (+ (* 0.25 x) (* 0.75 y))) A B)
```

### `canvas`

Builds a new image by evaluating an expression over output coordinates:

```lisp
(canvas W H 3
  (if (== c 0) 1 0))
```

Forms:

- `(canvas width height body)` for a 1-channel image
- `(canvas width height channels body)` for an explicit channel count

Inside `canvas`, the runtime binds:

- `i`: row
- `j`: column
- `c`: channel

### Shape helpers

Inside runtime pixel contexts, `draw-rect` and `draw-circle` produce masks:

```lisp
(* (draw-rect x y half-w half-h 1.0) color)
(* (draw-circle cx cy radius 1.0) color)
```

They return `value` inside the shape and `0` outside. In practice, you layer them with `max`, `+`, and `*`.

## Chaining and State

### `iterate`

Top-level compiled chaining:

```lisp
(iterate 3
  ((A) (+ A 1)))
```

This reruns the whole transform on its own output three times.

### `iterate-state`

Runtime stateful chaining with explicit initialization:

```lisp
(iterate-state 300
  ((A) initial-state)
  ((state) next-state))
```

The init program runs once with `iter = 0`.
The step program then runs with `iter = 1, 2, ...`.

### `iterate-until`

Runtime stateful chaining with a stop condition:

```lisp
(iterate-until 1000
  ((A) initial-state)
  ((state) next-state)
  ((state) done?))
```

Execution stops when `done?` returns non-zero or the iteration limit is reached.

For preview and chained runtime execution, the step program must take exactly one state argument.

## Game Patterns

gNine is functional and tuple-based, so game code works best when you model it as:

1. state tuple in
2. compute `next-*` values
3. render a frame
4. return a new state tuple

The Pong example uses:

```lisp
(tuple frame ball-x ball-y vx vy left-y right-y left-score right-score)
```

The Snake example uses a larger tuple with tick counters, direction, body segments, and food index.

### Put the frame first

For runtime chaining, preview, and `--emit-frames`, the runtime expects the final result to be:

- an image, or
- a tuple whose first element is an image

So for games, make the first tuple element the rendered frame.

### Keep simulation state scalar

Store positions, velocities, scores, timers, and flags as numbers in the tuple. Rebuild the frame each step with `canvas`.

This is usually simpler and faster than trying to treat the previous frame as the full authoritative game state.

### Use a logical grid

Snake is a good model:

- keep `head-x`, `head-y`, `dx`, `dy`, and body segments in grid units
- convert grid coordinates to pixels only during rendering

That makes collisions, stepping, and AI much easier than doing everything in screen coordinates.

### Separate simulation from rendering

Use `define` aggressively:

- `trial-*` for candidate state
- `next-*` for accepted state
- one final `next-frame`

That keeps the code readable and prevents the `canvas` body from carrying all of the game logic.

### Use fixed-step simulation first

A robust pattern is:

- increment a tick every frame
- move only when the tick hits a stride

That is how Snake avoids depending directly on frame time. Only reach for `preview-delta-ms` when you actually want time-scaled movement.

### Build visuals from masks

For simple games, `canvas` plus shape masks is enough:

- background layer
- playfield bounds
- sprites or blocks from `draw-rect`
- balls or circular markers from `draw-circle`
- combine with `max`

This style maps well to Pong, breakout-like prototypes, particle toys, and board games.

### Use `iterate-until` for win/lose conditions

Keep the done check separate from the step program. That keeps the state transition pure and makes the stop condition obvious.

### Encode finite content as functions

If you do not need real randomness, pre-baked sequences are simple and effective. Snake uses functions like `food-x-of` and `food-y-of` keyed by an index instead of adding a random subsystem.

This works well for:

- spawn tables
- scripted waves
- tutorial sequences
- deterministic benchmarks

### Treat images as views when it helps

The previous frame can still be useful as a view or texture. `canvas`, `map-image`, and relative sampling let you:

- scroll
- smear or trail
- build HUD strips
- compose channel views side-by-side

`rgb_triptych.psm` is a good small example of coordinate remapping inside `canvas`.

## Performance Tricks

### Prefer compiled scalar code when you can

If the job is a pure filter, neighborhood transform, or compositing pass, the default compiled mode is usually the simplest and fastest path.

### Use `pipeline` to fuse scalar stages

`pipeline` combines multiple scalar stages into one lowered kernel:

```lisp
(pipeline
  ((A) (+ A 1))
  ((P) (* P 2)))
```

Rules:

- pipeline stages are scalar-only
- in later stages, the first argument names the previous stage output
- additional arguments must match external inputs

This is useful when you want multi-stage logic without intermediate images.

### Use lambdas for runtime image ops

`map-image`, `zip-image`, and `canvas` have compiled fast paths. Those paths are easiest to hit when the callable is a closure or lambda.

Good:

```lisp
(map-image (lambda (x) (+ x bias)) A)
```

Less ideal:

```lisp
(map-image + A)
```

The second form can fall back to interpreted execution.

### Keep the symbolic program shape stable

The runtime caches compiled kernels by symbolic structure. If you keep the code shape the same and only change captured scalar values like `bias`, `iter`, or positions, later frames can reuse the compiled kernel.

That is exactly the pattern you want for games.

### Precompute row-invariant or frame-invariant values with `define`

Names like `W`, `H`, `mid-x`, `paddle-half-h`, or `cell-center` are not just cleaner. They also reduce duplicated work and make the generated kernels easier to optimize.

### Use `--danger` only when you mean it

`--danger` removes bounds normalization for compiled image sampling. That can help hot neighborhood kernels, but it is only safe if every computed coordinate is already valid.

For game rendering and exploratory filters, reflected indexing is usually the right default.

## Preview, Debugging, and Benchmarking

Useful runtime flags:

- `--runtime`
- `--preview`
- `--emit-frames=PATH`
- `--benchmark`
- `--benchmark-no-write`
- `--benchmark-repeats=N`
- `--chain-times=N`

Useful preview bindings in runtime mode:

- `key-up`, `key-down`, `key-left`, `key-right`
- `key-w`, `key-a`, `key-s`, `key-d`
- `key-space`, `key-return`, `key-escape`
- `mouse-x`, `mouse-y`, `mouse-left`, `mouse-right`
- `preview-time-ms`, `preview-delta-ms`

Recommended workflow for game-like programs:

1. Run with `--runtime --preview` until controls and state transitions feel right.
2. Add `--emit-frames=/tmp/name.png` when you need to inspect exact frame sequences.
3. Benchmark headless with `--runtime --benchmark --benchmark-no-write --chain-times=N --benchmark-repeats=M`.

For current build and test commands, see the repo [`README.md`](../README.md) and [`AGENTS.md`](../AGENTS.md).
