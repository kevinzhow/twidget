# twidget

Native renderers for Telegram's HTML widgets, built on a heavily optimized fork of [litehtml](https://github.com/litehtml/litehtml).

Widgets (cards) follow Telegram's public widget guide (https://core.telegram.org/bots/widgets):
- **The card runtime and host** (`twcard/`). A sandboxed QuickJS runtime runs the card as written, and a validator checks every DOM operation against profile 1.
- **The renderer.** Validated operations come in, events and layout answers go out, and pixels come from litehtml instead of a WebView.

**TwidgetCore** is the cross-platform renderer: portable C++20, with backends for Windows (Direct2D), Android, the web, Qt and Apple (iOS and macOS), built with CMake from the repository root. **The Swift package** (`Package.swift`) holds the Apple-side libraries on the same litehtml fork; its card renderer was removed with the old runtime and comes back later, on twcard.

## Layout

| Folder | What it holds | Used by |
|---|---|---|
| `litehtml/` | The litehtml fork, with its Gumbo parser | TwidgetCore and the Apple renderer |
| `core/` | TwidgetCore: the protocol, layout on the fork, animations, canvas, compositor layers, the paint limit. `core/include/twidget_litehtml.h` is its C ABI | Every backend in `platforms/` |
| `twcard/` | The card runtime's host: QuickJS engine glue, the validator, Intl and time zones (`twcard::CardHost`) | twidget-flutter |
| `platforms/` | One backend per platform: `windows` (DirectWrite, Direct2D), `qt` (QPainter), `apple` (CoreGraphics), `android` (JNI and Kotlin), `web` (WebAssembly and TypeScript) | Clients |
| `tests/` | TwidgetCore's checks, run by `ctest`, and portable checks of the Apple renderer's `lhx` helpers | |
| `tools/` | `build-apple.sh` builds the iOS and macOS static library; `cjk_rules` generates and checks the CJK line-breaking table | |
| `apple/` | The Swift package `Package.swift` describes: its sources, tests, Lab and examples. Cards on Apple platforms come back later, on twcard | |
| `docs/` | The documentation below | |

`Package.swift` stays at the root so that SwiftPM finds the package at the repository's URL.

## Building

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

This builds `twidget::litehtml`, `twidget::core` and the platform's backend. Android and Emscripten builds go through their toolchain files, and `-DTWIDGET_QT=ON` adds the Qt backend.

## Documentation

- [docs/TWIDGET_CORE.md](docs/TWIDGET_CORE.md): what TwidgetCore does, what a platform implements, each backend and the build.
- [apple/README.md](apple/README.md): the Swift package, three.js on Metal and the Lab.
- [docs/WIDGET_ASSETS.md](docs/WIDGET_ASSETS.md): images and the `icon:` set of standalone documents.

## License

BSD 3-Clause, see `LICENSE`. Bundled third-party code (litehtml, Gumbo, three.js and Phosphor Icons) keeps its own license, listed in `THIRD_PARTY_NOTICES.md`.
