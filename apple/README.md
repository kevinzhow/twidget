# The Swift package

A Swift package for macOS on twidget's litehtml fork. Its card renderer was removed with the old card runtime and comes back later, on twcard. Its sources are in this folder; `Package.swift` at the repository root describes it, so SwiftPM finds it at the repository's URL. It is separate from [TwidgetCore](../docs/TWIDGET_CORE.md), the portable C++ renderer, and shares the litehtml fork with it.

| Product | What it is |
|---|---|
| `LiteHTML` | The renderer core: parsing, sanitizing, layout, CoreGraphics and Metal drawing, hit testing, text selection |
| `TwidgetAssets` | An icon set (a Phosphor Icons subset) and the portable C++ resolver for `icon:` URLs |
| `TwidgetIcons` | The same icons from Swift: an `icon:` URL as a `data:` URL |
| `Twidget3D` | three.js r160 with its WebGL renderer replaced by a Metal one |
| `HTMLSandbox` | The CPU quota policy and the large-stack script thread |

macOS 10.13+, Swift 5.5+, C++17.

## Rendering a document

```swift
import LiteHTML

let document = LHDocument(html: html, css: css)
document.colorSpace = CGColorSpace(name: CGColorSpace.displayP3)
let size = document.layout(withMaxWidth: 360)
document.draw(in: context, origin: .zero, clip: CGRect(origin: .zero, size: size))
```

- **Links and actions.** `anchorHandler` receives links that passed the sanitizer. `actionHandler` receives clicks on elements with `data-action` (and an optional `data-value`).
- **Pointer input.** `mouseMoved`, `mouseDown`, `mouseUp` and `cursor` handle hover and active states.
- **Animated documents.** Build each new frame with `LHDocument(html:css:previous:)`. Inline styles, text, classes, attributes and child lists are patched into the previous tree instead of parsing it again; inserted elements go through the same sanitizer and limits as a parse. `LHDocument(previous:styles:attribute:css:)` patches style values directly, with no HTML at all, and `patchDirtyRects` names the boxes a paint-only patch can have changed.
- **Partial repaints.** `displayList(maxWidth:scale:)` and `changedRects(since:)` give exact dirty rectangles on the device-pixel grid of `scale`, so a host repaints only what changed.
- **Images.** `copyImage(maxWidth:rect:scale:base:dirty:)` redraws only a dirty area on top of the previous bitmap.

## Security model

Every document is validated before layout. The validator is an allowlist, not a blocklist:

- **Tags.** HTML and SVG tags are allowlisted. Scripts, iframes, frames, objects, embeds, audio and video, `<base>`, `<meta>`, `<link>` and `<template>` are dropped. Unknown tags render as `<span>`, with the original name kept in `data-lh-tag`, so their text survives and their behavior does not.
- **Attributes.** Attributes are allowlisted, every `on*` handler is dropped, and attributes with the reserved `data-lh-` prefix cannot be forged.
- **URLs.** Links may use `http`, `https`, `mailto` and `#fragment`. Images may only use `data:image/…` with an allowlisted MIME type and `icon:` names from the built-in icon set. SVG images go through the same validator as inline SVG and load nothing external. A host adds its own schemes with `LHSanitizer.setLinkSchemes(_:imageSchemes:)`. URLs with those schemes reach the host's link handler or image provider. The renderer never fetches anything.
- **CSS.** At-rules are allowlisted, so `@import`, `@font-face` and `@namespace` are removed. Every `url()` and image function is checked against the image rules, and `behavior` and `-moz-binding` are blocked.
- **Structure limits.** The parser stops at 256 levels of nesting, 20,000 elements and 200,000 table cells, so a hostile document cannot make layout explode. Patched-in subtrees count against the same limits.
- **Canvas sources.** Only `<canvas>` may show a canvas bitmap, through the renderer's own `twcanvas:` source; `<img>`, SVG `<image>` and CSS `url()` can't reference one.
- **Bounded caches.** Shared font, canvas font, color, raster and interpolation caches are all bounded, so no widget can grow memory through them.
- **Reporting.** `sanitizerCounts` and `sanitizerDetails` report what was removed, for logs and tests.

The policy lives in portable C++ (`lhx::SanitizerPolicy`, `lhx::Sanitizer` in `Sources/LiteHTML/core`), so other ports can share it.

## Bring your own engine

### GPU

There are two levels to plug into.

1. **Painter (immediate mode).** Subclass `lhx::Painter` (`Sources/LiteHTML/core/lhx_painter.h`) and call `paintWithMaxWidth:clip:painter:` from `LHDocumentPainting.h`. You receive every clip, group, text run, fill, gradient, image, border, shadow and SVG path in paint order, with resolved geometry. This is the right level for a Skia, Direct2D or WebGPU backend.
2. **Render list (retained).** Call `recordWithMaxWidth:recorder:` with an `lhx::RenderRecorder` (`Sources/LiteHTML/include/LHRenderList.h`). The document becomes a flat list of `RenderOp`s:
   - `Fill`: a solid device rectangle.
   - `Mask`: a glyph or rounded corner from an alpha atlas, tinted with a color.
   - `Bitmap`: a content-keyed BGRA texture that the container rasterizes into a context you allocate.

   Your engine owns the atlas and texture cache. The callbacks (`cached`, `allocate`, `store`, `glyph`, `mask`) let it skip work it already has. `lhx::rasterizeGlyph` and `lhx::rasterizeMask` produce the same alpha coverage the built-in renderer uses.

`LHGPURenderer` is the Metal client of the render list. It uses tiled IOSurface targets, a glyph atlas, a 32 MB texture LRU and batched submission. If a document needs something the list can't express, `LHDisplayList.gpuSupported` is false and the host draws that document with CoreGraphics.

### Images

A card is one self-contained document. Under widget profile 1 its pictures are `data:` images (PNG, JPEG, GIF, WebP) or inline SVG, and the validator drops every other URL before the renderer sees it.

Standalone documents (`LHDocument`) can also use `icon:heart-fill?size=24&color=ff2d55` URLs, resolved from a bundled, MIT-licensed icon set (`TwidgetAssets`, portable C++): see [docs/WIDGET_ASSETS.md](../docs/WIDGET_ASSETS.md).

`LHImages.setProvider { url in … }` supplies images for the host's own schemes (the scheme must also be allowed with `LHSanitizer`). For older Apple-only content, `LHImages.registerImage(_:name:)` registers `app:name` images and `sf:symbol.name?size=20&color=%23ff0000` renders SF Symbols on macOS 11+, falling back to the icon set's alias table; `LHImages.portableSymbols` resolves every `sf:` through the aliases.

## three.js and Metal

`Twidget3D` bundles three.js r160 (MIT). Its WebGL renderer is replaced by an adapter that sends geometry, instances and a compact per-frame scene record to a native Metal renderer, so there is no WebGL context. The renderer supports:
- Standard and basic materials, vertex colors and instancing.
- Points.
- Up to four lights, with a directional shadow map.
- Transparency sorting.

Its scripted host (`HTML3D` on `HTMLScript`) was removed with the old card runtime.

## Performance

- **Patches, not reparses.** Style, text, class, attribute and child-list changes patch the live document; paint-only changes reuse layout and repaint only the affected boxes.
- **Drawing.** 128 pt tiles with an 8-bit sRGB backing, per-rectangle invalidation, a raster cache for SVGs and images (a changing SVG rasterizes once per frame, not once per tile) and bounded font, color and canvas-font caches.

## Exactness

All work happens in the Lab, and every optimization is checked against a fresh render:

- **CoreGraphics path:** pixel-identical. `LiteHTMLLab bench --verify` replays recorded animation traces and compares every frame with a from-scratch render. `compare` diffs two runs trace by trace.
- **Metal path:** checked within a tolerance against CoreGraphics (`gpu --verify`). The thresholds are a maximum interior difference, a share of pixels more than 8 levels off (0.000% on the bundled traces) and zero structural differences.
- **Painter path:** receives exactly what the CoreGraphics path draws.

## Lab

`Lab/` is a separate package with these tools. `LiteHTMLLab` and `EngineLab` use `HTMLScript` and `HTML3D`, which were removed with the old card runtime: they do not build until those come back.

| Tool | What it does |
|---|---|
| `LiteHTMLLab` | Benchmarks, verification, `sanitize`, `scripttest [--runtime quickjs]`, `threetest` and a live viewer |
| `WebKitLab` | Renders the same chats in WKWebView, for comparison |
| `EngineLab` | Compares JavaScriptCore with QuickJS |
| `PresentLab` | Experiments with presentation paths |

Sample chats live in `Examples/chats`. The [native solar-system example](Examples/native-3d/README.md)
uses smooth meshes, native lighting and depth through the HTML3D path (removed with the old card runtime);
its offscreen checker exercises QuickJS and JavaScriptCore. It is standalone
HTMLScript content, outside widget profile 1.

```sh
cd apple/Lab
swift build -c release
.build/release/LiteHTMLLab record
.build/release/LiteHTMLLab bench --verify --label mine
.build/release/LiteHTMLLab gpu --verify
.build/release/LiteHTMLLab scripttest
```

## License

BSD 3-Clause, see [`LICENSE`](../LICENSE). Bundled third-party code (litehtml, Gumbo, three.js and Phosphor Icons) keeps its own license, listed in [`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md).
