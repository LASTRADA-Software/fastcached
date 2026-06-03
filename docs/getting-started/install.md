# Install

fastcached builds with CMake 3.28 or newer and a C++23 compiler.

## Linux / macOS

```sh
cmake --preset clang-debug
cmake --build --preset clang-debug
ctest --preset clang-debug
```

The clang-debug preset enables address and undefined-behavior
sanitizers and runs clang-tidy as part of compilation.

## Windows

```sh
cmake --preset cl-debug
cmake --build --preset cl-debug
ctest --preset cl-debug
```

Requires `VCPKG_ROOT` to be set in the environment.

## Other presets

The repository includes presets for:

- `gcc-debug` — GCC debug build on Linux.
- `clang-coverage` — Linux coverage build with an HTML report.
- `clang-asan-ubsan` — sanitizers without clang-tidy.
- `clang-tsan` — thread sanitizer.
- `clangcl-debug` — clang-cl on Windows.

See `CMakePresets.json` for the complete list.

## Running

The build produces a single executable, `fastcached`. By default it
listens on port 11211:

```sh
./fastcached
```

A `--help` flag prints the full configuration surface.
