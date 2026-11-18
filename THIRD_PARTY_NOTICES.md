# Third-party notices

twidget includes code from these projects. Their license texts are kept next to the code.

| Project | Where | License |
|---|---|---|
| [litehtml](https://github.com/litehtml/litehtml) (modified) | `litehtml` | BSD 3-Clause, Copyright (c) 2013 Yuri Kobets (tordex), see `litehtml/LICENSE` |
| [gumbo-parser](https://github.com/google/gumbo-parser) | `litehtml/src/gumbo` | Apache License 2.0, Copyright 2010 Google Inc., see `litehtml/src/gumbo/LICENSE` |
| [three.js](https://github.com/mrdoob/three.js) r160 | `apple/Sources/Twidget3D/Resources/three.js` | MIT, Copyright (c) 2010-2023 three.js authors, see the file's header |
| [Phosphor Icons](https://github.com/phosphor-icons/core) 2.1.1 (a subset, unmodified) | `apple/Sources/TwidgetAssets/icons`, and the path data in `apple/Sources/TwidgetAssets/twa_icon_data.inc` | MIT, Copyright (c) 2023 Phosphor Icons, see `apple/Sources/TwidgetAssets/icons/LICENSE` |
| [stone_engine_js](https://github.com/non-standardworld/stone_engine_js) caae307, a port of [stone_engine](https://github.com/ndc-stone/stone_engine) f8b2dc5 (files unmodified) | `tools/cjk_rules/upstream`, and the CJK rule tables generated from it, `litehtml/src/cjk_rules_table.inc` and `litehtml/src/cjk_scripts_table.inc` | MIT, Copyright (c) 2024 Nihon Design Center, see `tools/cjk_rules/upstream/stone_engine_js/LICENSE` |

twidget::text (`text/`) is built with HarfBuzz, on Android fetched at build time at a pinned release (SHA-256 in
`text/CMakeLists.txt`) unless a checkout is given, elsewhere the system's; it is not in this repository:

| Project | Used for | License |
|---|---|---|
| [HarfBuzz](https://github.com/harfbuzz/harfbuzz) 15.1.0 | text shaping; glyph outlines for GPU text (hb-gpu) | MIT ("Old MIT"), see its `COPYING` |
| [OpenType Sanitizer](https://github.com/khaledhosny/ots) 9.3.0, fetched everywhere | book fonts checked; WOFF/WOFF2 unpacked | BSD 3-Clause, see its `LICENSE` |
| [woff2](https://github.com/google/woff2) 1.0.2 (the decoder), fetched everywhere | WOFF2 for OTS | MIT, see its `LICENSE` |
| [Brotli](https://github.com/google/brotli) 1.1.0 (the decoder), fetched everywhere | WOFF2 for OTS | MIT, see its `LICENSE` |

The tests' fonts (`tests/fixtures/fonts`): Pacifico (WOFF) and Macondo (WOFF2), unmodified, SIL Open Font License
1.1, the license texts beside them.

twidget::base (`base/`) and twcard (`twcard/`) are built with these, fetched at build time at pinned versions (SHA-256 in
`base/base.cmake` and `twcard/twcard.cmake`) unless a local copy is given; they are not in this repository:

| Project | Used for | License |
|---|---|---|
| [yyjson](https://github.com/ibireme/yyjson) 0.13.0 | JSON parsing and writing under `twidget::Json` | MIT, Copyright (c) 2020 YaoYuan, see its `LICENSE` |
| [QuickJS](https://github.com/TelegramMessenger/quickjs) (TelegramMessenger's fork of quickjs-ng) at b526a817 | the cards' JavaScript engine (twcard::Engine) | MIT, Copyright (c) 2017-2026 Fabrice Bellard, (c) 2017-2024 Charlie Gordon, (c) 2023-2026 Ben Noordhuis and the other authors its `LICENSE` names |
| [Ada URL](https://github.com/ada-url/ada) 3.2.9 | WHATWG URL parsing for card URLs and links | MIT or Apache-2.0, Copyright 2023 Yagiz Nizipli and Daniel Lemire, see its `LICENSE-MIT` and `LICENSE-APACHE` |
| [ICU4X](https://github.com/unicode-org/icu4x) at 38a49da | card Intl formatters and built-in locale data | Unicode-3.0, see `twcard/intl/LICENSE-ICU4X`; exact dependencies in `twcard/intl/Cargo.lock` |

## Phosphor Icons

```
MIT License

Copyright (c) 2023 Phosphor Icons

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## stone_engine

```
MIT License

Copyright (c) 2024 Nihon Design Center

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
