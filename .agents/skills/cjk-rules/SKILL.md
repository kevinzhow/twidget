---
name: cjk-rules
description: 'Update or extend the CJK line-composition rule set (kinsoku, inseparable characters, punctuation classes) that litehtml reads through litehtml/cjk_rules.h: follow a new stone_engine_js commit, or add a character of our own. Use when stone_engine_js changes its tables, when a book needs a character the rules lack, or when the cjk_rules_table ctest test fails.'
---

# cjk-rules

The rules are data, generated from upstream, never written by hand:

| file | what |
|---|---|
| `tools/cjk_rules/manifest.json` | each upstream: repo, commit (full SHA), the file read, licence, SHA-256 of each vendored file |
| `tools/cjk_rules/upstream/<name>/` | those files at that commit, unmodified, with the upstream's LICENSE |
| `tools/cjk_rules/overlay.json` | our additions, each with a reason; an upstream update never touches it |
| `tools/cjk_rules/generate.py` | reads `upstream/stone_engine_js/src/punctuation.ts` and the overlay, writes the table |
| `litehtml/src/cjk_rules_table.inc` | the generated table: one line a code point, overlay lines marked |
| `litehtml/include/litehtml/cjk_rules.h` | the one interface (`cjk::classify`, `cjk::no_break_between`); callers never name stone |

stone_engine_js is the source. The Swift original (`stone_engine`, `STObject.swift`) is vendored for reference only:
the JS port lists the sets it fixed in `src/punctuation.ts` and at the top of `src/layout.ts`.

`generate.py --check` (the ctest test `twidget_cjk_rules_table`) fails when a vendored file's hash differs from the
manifest, a file in `upstream/` is not in the manifest, or the table differs from what the generator writes. A hand
edit of the table or of a vendored file fails it.

## Follow a new upstream commit

1. Read what changed upstream: `git -C <clone> log -p <old>..<new> -- src/punctuation.ts src/layout.ts`.
2. Check the clone out at the new commit and set `commit` in `manifest.json` to its full SHA.
3. `python3 tools/cjk_rules/generate.py --vendor stone_engine_js <clone>`: copies the files at that commit into
   `upstream/` and records their hashes (it refuses a clone at another commit).
4. `python3 tools/cjk_rules/generate.py`, then review `git diff litehtml/src/cjk_rules_table.inc` line by line.
   - The generator fails loudly when `punctuation.ts` changed shape (a constant renamed, a set built another way, a
     group's `（cl-06）`/`（cl-07）` tag gone). Change the parser in `generate.py`; never edit the vendored file.
   - It fails when upstream now has a class the overlay adds: drop that entry from `overlay.json`.
5. `python3 tools/cjk_rules/generate.py --check`, then twidget's ctest.
6. Measure the change on the corpus: the stone comparison tools (on K17 `/var/tmp/Alice/oyomi/stone-compare/tools`:
   `ours.py` with a reader-cli built against this twidget, then `compare.py` and `per_rule.py`), before and after.
7. Run the visual test on the affected books (visual-test repo, `tool/visual/run.sh` with `OYOMI_READER_LIB` set to the
   new build, against the baseline, without `--accept`), and the same run on the old build, so only this change shows.
8. Pin the twidget commit up the chain (the reader's `pin-chain` skill).

## Add a character of our own

Add an entry to `overlay.json`: `chars`, `classes` (the names in `cjk_rules.h`), and a `reason` (the books that need
it). Regenerate, add a test of the behaviour in `tests/cjk_rules_tests.cpp`, then steps 5 to 8 above.
