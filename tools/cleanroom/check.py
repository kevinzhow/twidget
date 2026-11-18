#!/usr/bin/env python3
"""Clean-room check (CLEANROOM §3 automated items; SWITCHOVER-PLAN §4). Test tooling, not product code.

Runs without the upstream sources: every comparison is against hashes in this directory, made once by the
clean-room reviewer from the upstream checkout (files: whole-file and normalised-content SHA-256; tokens: the
identifier denylist, hashed; fingerprints: winnowed 8-word shingles of upstream's cards and fixtures).

    check.py [--base <rev>] [--all]

  --base <rev>  added lines, commit messages and added/changed files are those of <rev>..HEAD
  --all         every tracked file counts as added (a full audit); also the default when no base is given

pending.txt lists files with known, owned leftovers: the full-file names and denylist checks skip them (added lines
are still checked). Empty it as they are fixed.

Checks (each failure is one line; exit status 1 if any):
  paths       no tracked path naming the old runtime or its bundle folder
  names       no tracked text file names the old runtime (zero references)
  blobs       no prebuilt engine bytecode, shared library or runtime bundle committed (twcard.js excepted)
  files       no tracked file equals an upstream file, byte for byte or after normalising whitespace and case
  denylist    no denylisted identifier in added lines or commit messages
  overlap     no added/changed card or fixture shares a run of 23 words or more with an upstream card or fixture
"""
import argparse
import hashlib
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SALT = b"twcc1:"
NAMES = re.compile(r"widget[-_ ]?sandbox|widgetjs|twidget_widget_sandbox", re.I)
PATH_NAMES = re.compile(r"(^|/)(widget-sandbox|widgetjs)(/|$)|\.deps/widget-sandbox", re.I)
BLOB_EXT = (".qbc", ".so", ".dylib", ".dll", ".node")
BLOB_NAME = re.compile(r"(^|/)[^/]*bundle[^/]*\.(js|mjs|qbc)$", re.I)
BLOB_ALLOWED = {"runtime/dist/twcard.js"}
CARD_EXT = (".html", ".htm", ".json", ".js", ".mjs", ".css", ".svg")
WORD = re.compile(r"[A-Za-z0-9_]+")
K, W = 8, 16          # shingle length (words), winnowing window (shingles): runs of K + W - 1 = 23 words always hit
MIN_NORM = 200        # normalised files shorter than this are not compared (empty files, "{}", one-line stubs)


def h(data, n=16):
    return hashlib.sha256(SALT + data).hexdigest()[:n]


def load(name):
    p = os.path.join(HERE, name)
    if not os.path.exists(p):
        return set()
    with open(p, encoding="utf-8") as f:
        return {l.split("#", 1)[0].strip() for l in f if l.split("#", 1)[0].strip()}


def git(*args):
    return subprocess.run(["git", *args], check=True, capture_output=True).stdout


def tracked():
    return [p for p in git("ls-files", "-z").decode("utf-8", "surrogateescape").split("\0") if p]


def is_text(data):
    return b"\0" not in data[:8192]


def norm(data):
    return re.sub(rb"\s+", b" ", data.lower()).strip()


def tokens(line, allow):
    """Every form a denylisted name can take in a line: identifiers, hyphenated names, path pieces."""
    low = line.lower()
    for a in allow:
        low = low.replace(a, " ")
    out = set(WORD.findall(low))
    out.update(re.findall(r"[a-z0-9_]+(?:-[a-z0-9_]+)+", low))
    for t in re.findall(r"[a-z0-9_.\-/]+", low):
        parts = [p for p in t.split("/") if p]
        out.update(parts)
        for i, p in enumerate(parts[:-1]):
            out.add(p + "/")
            out.add(p + "/" + parts[i + 1] + "/")
        m = re.match(r"v\d+\.\d+\.", t)
        if m:
            out.add(m.group(0))
    return out


def fingerprints(text):
    """Winnowed shingle hashes with the word index each starts at."""
    words = [w.lower() for w in WORD.findall(text)]
    sh = [(int(h(" ".join(words[i:i + K]).encode(), 12), 16), i) for i in range(len(words) - K + 1)]
    picked = {}
    for s in range(max(0, len(sh) - W + 1)):
        win = sh[s:s + W]
        m = min(win, key=lambda x: (x[0], -x[1]))
        picked[m[1]] = m[0]
    if 0 < len(sh) < W:
        m = min(sh, key=lambda x: (x[0], -x[1]))
        picked[m[1]] = m[0]
    return sorted(picked.items())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base")
    ap.add_argument("--all", action="store_true")
    a = ap.parse_args()
    upstream_files = load("upstream-files.txt")
    upstream_tokens = load("upstream-tokens.txt")
    upstream_fp = {int(x, 16) for x in load("upstream-fingerprints.txt")}
    allow = sorted(load("allow-tokens.txt"), key=len, reverse=True)
    pending = load("pending.txt")
    own = os.path.relpath(HERE, git("rev-parse", "--show-toplevel").decode().strip()).replace(os.sep, "/")
    fails = []

    files = tracked()
    if a.base and not a.all:
        changed = set(git("diff", "--name-only", "--diff-filter=AMR", "-z", f"{a.base}...HEAD").decode().split("\0")) - {""}
        diff = git("diff", "-U0", "--no-color", f"{a.base}...HEAD").decode("utf-8", "replace")
        added, cur = [], None
        for l in diff.splitlines():
            if l.startswith("+++ "):
                cur = l[6:] if l.startswith("+++ b/") else None
            elif l.startswith("+") and cur and not cur.startswith(own + "/"):
                added.append(l[1:])
        # CLEANROOM §3 item 2: the documented build-removal commits (titled "cleanroom/base:") may name what they remove
        messages = []
        for m in git("log", "--format=%B%x00", f"{a.base}..HEAD").decode("utf-8", "replace").split("\0"):
            if m.strip() and not m.strip().startswith("cleanroom/base:"):
                messages += m.splitlines()
    else:
        changed = set(files)
        added = None  # read per file below
        messages = []

    for p in files:
        if PATH_NAMES.search(p):
            fails.append(f"paths: {p}")
        if (p.lower().endswith(BLOB_EXT) or BLOB_NAME.search(p)) and p not in BLOB_ALLOWED:
            fails.append(f"blobs: {p}")
        if p.startswith(own + "/") or not os.path.isfile(p):
            continue
        with open(p, "rb") as f:
            data = f.read()
        if h(data) in upstream_files:
            fails.append(f"files: {p} is an upstream file")
            continue
        text_file = is_text(data)
        if text_file:
            n = norm(data)
            if len(n) >= MIN_NORM and h(n) in upstream_files:
                fails.append(f"files: {p} is an upstream file (whitespace or case changed)")
            text = data.decode("utf-8", "replace")
            if p not in pending:
                for i, line in enumerate(text.splitlines(), 1):
                    if NAMES.search(line):
                        fails.append(f"names: {p}:{i}: {line.strip()[:120]}")
                        break
            if added is None and p not in pending:
                for i, line in enumerate(text.splitlines(), 1):
                    hits = {t for t in tokens(line, allow) if h(t.encode()) in upstream_tokens}
                    if hits:
                        fails.append(f"denylist: {p}:{i}: a denylisted name")
            if p in changed and p.lower().endswith(CARD_EXT) and upstream_fp:
                fp = fingerprints(text)
                hit = [i for i, v in fp if v in upstream_fp]
                # a run: matched shingles no more than W words apart; its span in words
                best, start, last = 0, None, None
                for i in hit:
                    if last is None or i - last > W:
                        start = i
                    last = i
                    best = max(best, last - start + K)
                if best >= K + W - 1:
                    fails.append(f"overlap: {p}: about {best} words shared with an upstream card or fixture")

    if added is not None:
        for line in added + messages:
            hits = {t for t in tokens(line, allow) if h(t.encode()) in upstream_tokens}
            if hits:
                fails.append(f"denylist: added line or commit message: {line.strip()[:120]}")

    for f in fails:
        print(f)
    print(f"cleanroom-check: {len(fails)} finding(s), {len(files)} tracked files", file=sys.stderr)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
