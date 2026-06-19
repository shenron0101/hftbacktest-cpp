# Changelog

All notable changes follow [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). This project uses semantic versioning before `1.0` to communicate compatibility changes.

## [0.1.0] - 2026-06-19

Initial public C++20 port.

### Added

- Event-driven local/exchange replay, L2 depth, orders, fills, position and fee accounting.
- Queue, latency, asset, fee, exchange, reader, and recorder models.
- NPY and NPZ input, 54 core tests, deterministic market-making example, and benchmark.
- Installable CMake package exporting `hbt::hbt` and `hbt::io`.
- Linux/macOS CI, sanitizers, analysis, package-consumer checks, CodeQL, and releases.

### Original implementation credit

This release ports the architecture and algorithms of [HftBacktest](https://github.com/nkaz001/hftbacktest), created by [nkaz001](https://github.com/nkaz001) and developed with its [contributors](https://github.com/nkaz001/hftbacktest/graphs/contributors). The pinned reference is `5f3ec40b2afb764e0fea112f941ed85523ef4e88`. This independent port is not affiliated with or endorsed by upstream.

[0.1.0]: https://github.com/shenron0101/hftbacktest-cpp/releases/tag/v0.1.0
