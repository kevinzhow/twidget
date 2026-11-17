---
name: cjk-rules
description: 'Update or extend the CJK line-composition rule set (kinsoku, inseparable characters, punctuation classes, scripts, where justify may widen a gap, punctuation compression) that litehtml reads through litehtml/cjk_rules.h: follow a new stone_engine_js commit, or add a character of our own. Use when stone_engine_js changes its tables, its justification or its punctuation compression logic, when a book needs a character the rules lack, or when the cjk_rules_table ctest test fails.'
---

# cjk-rules

The rules are data, generated from upstream, never written by hand:

| file | what |
|---|---|
| `tools/cjk_rules/manifest.json` | each upstream: repo, commit (full SHA), the files read (`paths`), licence, SHA-256 of each vendored file, and `mirrored`: the upstream functions litehtml follows by hand, each with the SHA-256 of its text as last reviewed and where we mirror it (`in`) |
| `tools/cjk_rules/upstream/<name>/` | those files at that commit, unmodified, with the upstream's LICENSE |
| `tools/cjk_rules/overlay.json` | our additions, each with a reason; an upstream update never touches it |
| `tools/cjk_rules/generate.py` | reads `upstream/stone_engine_js/src/punctuation.ts`, `src/unicode.ts` and the overlay, writes the tables |
| `litehtml/src/cjk_rules_table.inc` | the generated classes: one line a code point, overlay lines marked |
| `litehtml/src/cjk_scripts_table.inc` | the generated scripts: the ranges stone sets as Japanese or emoji (the rest is Latin) |
| `litehtml/include/litehtml/cjk_rules.h` | the one interface (`cjk::classify`, `cjk::no_break_between`, `cjk::script_of`, `cjk::justifiable_gap`, `cjk::compression`); callers never name stone |
| `litehtml/src/cjk_rules.cpp` | the logic around the tables, mirroring upstream functions (`justifiable_gap` is stone's `isJustifiableGap`, `compression` its `punctuationTrim` in "stone" mode) |

stone_engine_js is the source. The Swift original (`stone_engine`, `STObject.swift`) is vendored for reference only:
the JS port lists the sets it fixed in `src/punctuation.ts` and at the top of `src/layout.ts`.

`generate.py --check` (the ctest test `twidget_cjk_rules_table`) fails when a vendored file's hash differs from the
manifest, a file in `upstream/` is not in the manifest, a table differs from what the generator writes, or a `mirrored`
function's text differs from the hash recorded for it. A hand edit of a table or of a vendored file fails it.

Logic is not data: where litehtml follows an upstream function (stone's `isJustifiableGap` in
`cjk::justifiable_gap`, `isJustifiableGapTbRl` in `line_box::finish`, `scriptOfCodePoint` in `generate.py`'s `scripts`), it
is C++ or Python written by hand, and `mirrored` pins the upstream text it was checked against. Ours adds what stone gets
elsewhere: no gap inside a grapheme cluster (stone measures clusters), and no gap inside a `text-combine-upright` piece.

Punctuation compression is split the same way. `cjk::compression(prev, c, next, at)` is stone's `punctuationTrim` in its
"stone" mode (`halvesOpeningAfterPunctuation`, `halvesClosingBeforePunctuation`, `TRIM_FIRST_HALF`/`TRIM_SECOND_HALF`) plus
the line-end trim of `goNextLineLrTb`/`goNextLineTbRl`; `at` says whether c starts a line or ends one that wraps. Where
stone applies it is pinned too: `layoutRunLrTb`/`layoutRunTbRl` (a line's first character: `line_box::add_item`),
`inlineExtentOfRun`/`columnExtentOfRun` (what the fit counts: a piece's width in `place_inline`), `goNextLine*`
(`line_box::finish`, not before `<br>` nor on a paragraph's last line). stone's "half" mode (・：； always a quarter each
side) is not followed. A trimmed mark is drawn with the font's halt (vhal upright in vertical text) when
`document_container::has_half_width_form` says the font has it, else full width moved back; a change upstream to how the
glyph is placed (`shift`) is reviewed in `el_text.cpp`'s `draw_justified` and `draw_vertical`.

## Follow a new upstream commit

1. Read what changed upstream: `git -C <clone> log -p <old>..<new> -- src/punctuation.ts src/unicode.ts src/layout.ts src/fonts.ts`.
2. Check the clone out at the new commit and set `commit` in `manifest.json` to its full SHA.
3. `python3 tools/cjk_rules/generate.py --vendor stone_engine_js <clone>`: copies the files at that commit into
   `upstream/` and records their hashes (it refuses a clone at another commit).
4. `python3 tools/cjk_rules/generate.py`, then review `git diff litehtml/src/cjk_rules_table.inc litehtml/src/cjk_scripts_table.inc`
   line by line.
   - The generator fails loudly when `punctuation.ts` changed shape (a constant renamed, a set built another way, a
     group's `（cl-06）`/`（cl-07）` tag gone). Change the parser in `generate.py`; never edit the vendored file.
   - It fails when upstream now has a class the overlay adds: drop that entry from `overlay.json`.
5. `python3 tools/cjk_rules/generate.py --check`. For each `mirrored` function it reports changed: read the upstream
   change (`git -C <clone> diff <old> <new> -- <path>`), change the code named in its `in` to match (and its test in
   `tests/cjk_rules_tests.cpp`, `tests/justify_tests.cpp` or `tests/punctuation_tests.cpp`), then put the new sha256 the check printed in
   `manifest.json`. One reported missing was renamed or moved: find it, and update `name` or `path`. Then twidget's
   ctest.
6. Measure the change on the corpus: the stone comparison tools (on K17 `/var/tmp/Alice/oyomi/stone-compare/tools`:
   `ours.py` with a reader-cli built against this twidget, then `compare.py` and `per_rule.py`; for justify,
   `justify_line_max_gap_em` and `justify`; for compression, the `punctuation` table), before and after.
7. Run the visual test on the affected books (visual-test repo, `tool/visual/run.sh` with `OYOMI_READER_LIB` set to the
   new build, against the baseline, without `--accept`), and the same run on the old build, so only this change shows.
8. Pin the twidget commit up the chain (the reader's `pin-chain` skill).

## Add a character of our own

Add an entry to `overlay.json`: `chars`, `classes` (the names in `cjk_rules.h`), and a `reason` (the books that need
it). Regenerate, add a test of the behaviour in `tests/cjk_rules_tests.cpp`, then steps 5 to 8 above.
