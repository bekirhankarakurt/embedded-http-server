# Contributing

Thanks for taking the time to contribute.

## Development setup

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DEHS_WARNINGS_AS_ERRORS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Before opening a pull request, also run the sanitizer build and the static analysers used in CI:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DEHS_SANITIZE=ON
cmake --build build-asan -j && ctest --test-dir build-asan --output-on-failure
clang-tidy -p build --quiet src/*.cpp examples/device_panel/*.cpp examples/hello/*.cpp
```

## Guidelines

- C++17, no third-party dependencies, Linux only.
- RAII for every resource; no raw `new`/`delete`.
- Zero warnings with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`.
- Every behaviour change comes with a test. Protocol changes need a parser or session test and,
  where relevant, a socket test in `tests/test_server.cpp`.
- Keep commits small and focused; write commit messages in the imperative mood
  ("Add chunked trailer limit", not "Added ...").
- Update `CHANGELOG.md` under `[Unreleased]`.
