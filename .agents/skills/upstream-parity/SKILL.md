---
name: upstream-parity
description: 'Before changing the core for a reported difference, find what upstream does at that exact point (twidget 3e25003, widget-sandbox renderer-bundle 0.6.3, Chromium). Use when a card behaves wrong, when a widget stops, and before adding a limit, a fast path, a cache or a stop rule in renderer.cpp or litehtml.'
---

# upstream-parity

This core is a fork of TelegramMessenger/twidget 3e25003 (`git show 3e25003:<file>`). Its rules (paint policy,
stops, the frozen frame) are widget-sandbox's: `packages/core/src/renderer/core.ts`,
`packages/core/src/shared/paint-policy.ts`, conformance `packages/conformance/auto/*`. A difference we show is either
the card's own (upstream does the same: keep CSS), or ours (restore upstream's behaviour), or speed (the reference is
a browser: extend upstream's own mechanisms, i.e. property patches, dirty bits, the layout cache, relayout boundaries).

Rules for fast paths in litehtml:
- A fast path that changes a layout input must set the dirty bit the layout cache reads (`mark_layout_dirty`).
  Correct "because some ancestor misses the cache anyway" is a bug waiting for the next cache improvement.
- A patch of non-inherited properties does not dirty the styles of what is under the element.
  `patch_inline_properties` refuses when a child declares `inherit` for one.
- Every new fast path gets a randomized "patched vs rebuilt paints the same" test in core_tests (see "rows patched
  with what is inside them"). Also run core_tests with `LH_LAYOUT_CACHE_CHECK=1`.

| class | instance | status |
|---|---|---|
| we freeze where upstream finishes | `Renderer::stop_paint` froze transitions mid-way (upstream cancels them) | fixed 6a6e6a9 |
| same | `Renderer::freeze` stopped CSS animations; upstream's browser keeps them | fixed (keep playing) |
| ported from an older upstream | `kFrozenBadgeCss` (`opacity:.85` vs upstream `filter:grayscale(.3)`) | fixed (grayscale on body: litehtml paints no filter on the root) |
| we rebuild where the reference invalidates incrementally | `apply_template_values` dropped ancestors' fast patches (leaderboard rows restyled whole every frame) | fixed 34d0563 |
| same | one-entry layout cache thrashes between flex passes | open |
| fast path without its invalidation | `patch_inline_properties` (sizes, offsets) did not mark layout dirty | fixed 34d0563 |
