# Clean-room check

`check.py` is the automated half of the clean-room review: it runs in CI (`.github/workflows/cleanroom-check.yml`) on
every pull request and push to `main` and `cleanroom/*`, and locally with `python3 check.py --all` (a full audit) or
`--base <rev>` (what `<rev>..HEAD` adds). Python 3, no dependencies, no network.

It needs no copy of the old runtime: it compares against hashes only, made by the clean-room reviewer outside this
repository and never reversible to text.

| File | What it holds |
|---|---|
| `upstream-files.txt` | salted SHA-256 of every file of the old runtime's repository, as is and with whitespace and case normalised (files of the licensed fork base and third-party folders left out) |
| `upstream-tokens.txt` | salted SHA-256 of the denylisted names (identifiers, file names, paths) |
| `upstream-fingerprints.txt` | winnowed 8-word shingle hashes of the old runtime's cards and fixtures; any shared run of 23 words or more is found |
| `allow-tokens.txt` | exact strings of our own that look like a denylisted name, removed before that check |
| `pending.txt` | files with known leftovers and their owners; the full-file checks skip them until the list is empty |

A finding names the file and line, never the matched upstream text. To update the hashes (a new denylist entry, a
new upstream release to keep out), ask the clean-room reviewer; implementers never regenerate them.
