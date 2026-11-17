---
name: cjk-rules
description: 'Update or extend the CJK line-composition rule set (kinsoku, inseparable characters, punctuation classes, scripts, where justify may widen a gap, punctuation compression, tate-chu-yoko) that litehtml reads through litehtml/cjk_rules.h: follow a new stone_engine_js commit, or add a character or a tate-chu-yoko token of our own. Use when stone_engine_js changes its tables, its justification, its punctuation compression or its tate-chu-yoko logic, when a book needs a character or token the rules lack, or when the cjk_rules_table ctest test fails.'
---

# cjk-rules

The rules are data, generated from upstream, never written by hand:

| file | what |
|---|---|
| `tools/cjk_rules/manifest.json` | each upstream: repo, commit (full SHA), the files read (`paths`), licence, SHA-256 of each vendored file, and `mirrored`: the upstream functions litehtml follows by hand, each with the SHA-256 of its text as last reviewed and where we mirror it (`in`) |
| `tools/cjk_rules/upstream/<name>/` | those files at that commit, unmodified, with the upstream's LICENSE |
| `tools/cjk_rules/overlay.json` | our additions, each with a reason: `additions` (a character's classes) and `tate_chu_yoko` (whole tokens set in one em); an upstream update never touches it |
| `tools/cjk_rules/generate.py` | reads `upstream/stone_engine_js/src/punctuation.ts`, `src/unicode.ts` and the overlay, writes the tables |
| `litehtml/src/cjk_rules_table.inc` | the generated classes: one line a code point, overlay lines marked |
| `litehtml/src/cjk_scripts_table.inc` | the generated scripts: the ranges stone sets as Japanese or emoji (the rest is Latin) |
| `litehtml/src/cjk_tate_chu_yoko_table.inc` | the overlay's tate-chu-yoko tokens (!? ‼ ...), generated; stone's own (numbers) is code |
| `litehtml/include/litehtml/cjk_rules.h` | the one interface (`cjk::classify`, `cjk::no_break_between`, `cjk::script_of`, `cjk::justifiable_gap`, `cjk::compression`, `cjk::tate_chu_yoko`); callers never name stone |
| `litehtml/src/cjk_rules.cpp` | the logic around the tables, mirroring upstream functions (`justifiable_gap` is stone's `isJustifiableGap`, `compression` its `punctuationTrim` in "stone" mode, `tate_chu_yoko` its `isTateChuYoko`) |

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

Tate-chu-yoko (縦中横) is split the same way. `cjk::tate_chu_yoko(text, from, to)` returns the tokens of a piece that
vertical text sets upright, side by side in one em of the column: stone's `isTateChuYoko` / `isTateChuYokoToken` (a token
of at most 2 numbers the Latin font sets; of stone's `\p{N}`, we take the ASCII digits), with the token's ends drawn as
stone's `splitWords` (Intl.Segmenter "ja") draws them around a number (UAX #29: a letter, a full-width digit, or `.` `,`
between digits joins it), plus the overlay's `tate_chu_yoko` tokens with no Latin character next to them. The piece
(`el_text`) keeps them (`el_text::tate_chu_yoko`, found in its text node's whole text, since the characters around a
piece decide where a token ends); its width, `draw_vertical`, emphasis, justify's gaps (`line_box::finish`),
compression and selection (`advance_to`) all take a token as one em. `text-combine-upright: all` is one token over the
whole piece; `digits` and `digits 2` are the automatic rule (`digits 3` and `4` are not supported). A token wider than
the em is squeezed across it (Blink's fit), where stone lets two digits overhang (`applyTateChuYokoTbRl`). The generator
refuses an overlay token of more than 2 characters, with a digit, or of 2 characters not both Latin (justify would put a
gap inside it). The tests are in `tests/tate_chu_yoko_tests.cpp`.

## Follow a new upstream commit

1. Read what changed upstream: `git -C <clone> log -p <old>..<new> -- src/punctuation.ts src/unicode.ts src/layout.ts src/fonts.ts src/context.ts src/parser.ts`.
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
   `tests/cjk_rules_tests.cpp`, `tests/justify_tests.cpp`, `tests/punctuation_tests.cpp` or `tests/tate_chu_yoko_tests.cpp`), then put the new sha256 the check printed in
   `manifest.json`. One reported missing was renamed or moved: find it, and update `name` or `path`. Then twidget's
   ctest.
6. Measure the change on the corpus: the stone comparison tools (on K17 `/var/tmp/Alice/oyomi/stone-compare/tools`:
   `ours.py` with a reader-cli built against this twidget, then `compare.py` and `per_rule.py`; for justify,
   `justify_line_max_gap_em` and `justify`; for compression, the `punctuation` table; for tate-chu-yoko, `tcy`), before and after.
7. Run the visual test on the affected books (visual-test repo, `tool/visual/run.sh` with `OYOMI_READER_LIB` set to the
   new build, against the baseline, without `--accept`), and the same run on the old build, so only this change shows.
8. Pin the twidget commit up the chain (the reader's `pin-chain` skill).

## Add a character of our own

Add an entry to `overlay.json`: `chars`, `classes` (the names in `cjk_rules.h`), and a `reason` (the books that need
it). Regenerate, add a test of the behaviour in `tests/cjk_rules_tests.cpp`, then steps 5 to 8 above.

## Add a tate-chu-yoko token of our own

Add the token to a `tate_chu_yoko` entry of `overlay.json` (`tokens`, and a `reason`: the books or the page that need
it): one character, or two Latin ones; numbers are stone's rule. Regenerate, add a test in
`tests/tate_chu_yoko_tests.cpp` (drawn upright in one em between Japanese characters, sideways next to a Latin one),
then steps 5 to 8 above.
