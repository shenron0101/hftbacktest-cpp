# Contributing

Contributions should preserve upstream attribution, deterministic replay behavior, and portable C++20 builds.

## Development setup

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DHBT_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Format changed C++ files with `clang-format -i`. Run `clang-tidy` through the dedicated CMake build or CI before requesting review.

## Pull requests

- Keep changes focused and add tests for observable behavior.
- Explain any deliberate deviation from the pinned upstream implementation.
- Do not add private datasets, generated outputs, venue credentials, or coefficient artifacts.
- Do not remove or weaken attribution, SPDX identifiers, or licensing notices.
- Report benchmark numbers with compiler, CPU, build type, and exact command.

By contributing, you agree that your contribution is licensed under the MIT license in this repository.

## Original project

Changes that belong in the original Python/Rust project should be proposed to [HftBacktest](https://github.com/nkaz001/hftbacktest). Review its [contributor history](https://github.com/nkaz001/hftbacktest/graphs/contributors) before claiming authorship of an existing design.
