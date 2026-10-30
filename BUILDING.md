# Building yaml99

Requires a C99 compiler and CMake 3.22 or later. Initialize dependencies after
checking out an existing clone:

```sh
git submodule update --init --recursive
```

## Options

| Option | Default | Purpose |
| --- | --- | --- |
| `YAML99_BUILD_TESTING` | On for a top-level build | Build the tests |
| `YAML99_WITH_HEAP` | `ON` | Build allocating string getters |
| `YAML99_USE_SYSTEM_DEPENDENCIES` | `OFF` | Find installed sio99 and utf99 through pkg-config |
| `BUILD_SHARED_LIBS` | `OFF` | Build shared libraries |

The core uses neither heap allocation nor stdio. For a minimal build:

```sh
cmake -S . -B build -DYAML99_WITH_HEAP=OFF -DSIO99_WITH_STDIO=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

## Dependencies

The bundled sio99 and utf99 submodules are pinned under `src`. A parent project
can supply targets named `sio99` and `utf99` before adding yaml99. Existing targets
take precedence over bundled or system dependencies. Only one sio99 target is
built even though utf99 also depends on it.

`yaml99_heap` requires `sio99_heap`. Enable `SIO99_WITH_HEAP` when providing a
custom dependency build. Library error codes remain independent. yaml99 maps
reader and writer failures to its own results, while implementations can retain
their detailed errors.

## Installation

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build
cmake --install build
pkg-config --cflags --libs yaml99
```

The bundled dependencies are installed with yaml99. To use installed dependencies
instead, configure with `-DYAML99_USE_SYSTEM_DEPENDENCIES=ON` and ensure their
pkg-config files are discoverable through `PKG_CONFIG_PATH`.

## Tests

Tests cover traversal, scalar decoding, Unicode validation, invalid syntax,
empty values, repeated-key traversal, depth limits, raw output, allocating
getters, reader operation scaling, and failure injection into reader and writer
callbacks. Sanitizers can be enabled
with compiler flags:

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```
