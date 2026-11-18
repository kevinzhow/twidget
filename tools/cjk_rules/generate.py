#!/usr/bin/env python3
"""Generates the CJK line-composition rule set behind litehtml/cjk_rules.h from stone_engine_js (vendored under
upstream/ at the commit in manifest.json) and overlay.json:
  litehtml/src/cjk_rules_table.inc     each character's classes, from src/punctuation.ts and the overlay
  litehtml/src/cjk_scripts_table.inc   the characters stone sets as Japanese or emoji, from src/unicode.ts
  litehtml/src/cjk_tate_chu_yoko_table.inc   the overlay's tate-chu-yoko tokens (stone's own, numbers, is code:
                                  cjk::tate_chu_yoko, pinned in manifest.json "mirrored")

usage:
  generate.py                     write the tables
  generate.py --check             exit 1 unless the vendored files match manifest.json's hashes, the tables are what
                                  this script writes (a ctest test: nobody edits them by hand), and each upstream
                                  function litehtml mirrors by hand (manifest.json "mirrored") is the text reviewed
  generate.py --vendor NAME DIR   copy upstream NAME's files from DIR, a git checkout at manifest.json's commit, into
                                  upstream/NAME and record their hashes in manifest.json

How to update the upstream: .agents/skills/cjk-rules/SKILL.md.
"""
import hashlib
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
MANIFEST = os.path.join(HERE, "manifest.json")
OVERLAY = os.path.join(HERE, "overlay.json")
UPSTREAM = os.path.join(HERE, "upstream")
TABLE = os.path.join(REPO, "litehtml", "src", "cjk_rules_table.inc")
SCRIPTS_TABLE = os.path.join(REPO, "litehtml", "src", "cjk_scripts_table.inc")
TCY_TABLE = os.path.join(REPO, "litehtml", "src", "cjk_tate_chu_yoko_table.inc")
SOURCE = "stone_engine_js"  # the upstream whose tables are read; the others are kept for reference

# The class bits, in cjk_rules.h's order (litehtml::cjk::class_bit).
CLASSES = ["not_starting", "not_ending", "inseparable", "opening", "closing", "middle_dot", "full_stop_comma", "hanging"]
# stone's scripts, as litehtml::cjk::script names them; a character of none of them is set in the Latin font
# (fontIdForChar in src/fonts.ts), so it is Latin.
SCRIPTS = ["latin", "japanese", "emoji"]


def fail(message):
    sys.exit("cjk_rules: " + message)


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def upstream(manifest, name):
    for entry in manifest["upstream"]:
        if entry["name"] == name:
            return entry
    fail(f"manifest.json has no upstream {name!r}")


def check_hashes(manifest):
    problems = []
    listed = set()
    for entry in manifest["upstream"]:
        for path, digest in entry["sha256"].items():
            local = os.path.join(UPSTREAM, entry["name"], path)
            listed.add(os.path.normpath(local))
            if not os.path.isfile(local):
                problems.append(f"{entry['name']}/{path}: missing")
            elif sha256(local) != digest:
                problems.append(f"{entry['name']}/{path}: sha256 {sha256(local)}, manifest.json says {digest}")
    for root, _, files in os.walk(UPSTREAM):
        for f in files:
            if os.path.normpath(os.path.join(root, f)) not in listed:
                problems.append(f"{os.path.relpath(os.path.join(root, f), UPSTREAM)}: not in manifest.json")
    return problems


# The upstream code litehtml mirrors by hand (cjk::justifiable_gap mirrors layout.ts isJustifiableGap): manifest.json
# records the SHA-256 of each declaration's text as reviewed, so an upstream change to it fails --check until someone
# reviews the C++ against it and records the new hash.
def declaration(ts, name):
    """The text of `function name(...) {...}`, a method `name(...) {...}`, or `const name = ...;` in TypeScript source,
    from its first line to its closing brace or semicolon."""
    starts = [m.start() for m in re.finditer(r"^[ \t]*(?:export[ \t]+)?(?:(?:private|public|protected|static|async)[ \t]+)*"
                                              r"(?:function[ \t]+|const[ \t]+)?%s\b[ \t]*[(:=<]" % re.escape(name), ts, re.M)]
    if len(starts) != 1:
        return None
    depth, opened = 0, False
    for i in range(starts[0], len(ts)):
        c = ts[i]
        if c in "([{":
            depth += 1
            opened = opened or c == "{"
        elif c in ")]}":
            depth -= 1
            if depth == 0 and c == "}" and opened and not ts[starts[0]:i].lstrip().startswith(("const", "export const")):
                return ts[starts[0]:i + 1]
        elif c == ";" and depth == 0:
            return ts[starts[0]:i + 1]
    return None


def check_mirrored(manifest):
    problems = []
    for entry in manifest["upstream"]:
        for m in entry.get("mirrored", []):
            local = os.path.join(UPSTREAM, entry["name"], m["path"])
            text = declaration(read(local), m["name"]) if os.path.isfile(local) else None
            where = f"{entry['name']}/{m['path']} {m['name']}"
            if text is None:
                problems.append(f"{where}: not found (renamed or moved upstream?): see what became of it, review {m['in']}")
                continue
            digest = hashlib.sha256(text.encode()).hexdigest()
            if digest != m["sha256"]:
                problems.append(f"{where} changed (sha256 {digest}, manifest.json says {m['sha256']}): review {m['in']} "
                                f"against it, then record the new sha256 in manifest.json")
    return problems


# punctuation.ts: the shapes this parser expects. Anything else fails, so an upstream change of shape is noticed.
def string_constant(ts, name):
    found = re.findall(r'^const %s = "([^"\\]*)";$' % name, ts, re.M)
    if len(found) != 1:
        fail(f"punctuation.ts: expected one `const {name} = \"...\";`, found {len(found)}")
    return found[0]


def set_of_string(ts, name):
    found = re.findall(r'^export const %s: ReadonlySet<string> = new Set\(\s*Array\.from\(\s*(?:"([^"\\]*)"|`([^`\\$]*)`)\s*\),?\s*\);'
                       % name, ts, re.M)
    if len(found) != 1:
        fail(f"punctuation.ts: expected one `export const {name}: ReadonlySet<string> = new Set(Array.from(\"...\"));`")
    return found[0][0] or found[0][1]


def set_of_groups(ts, name):
    """KINSOKU_NOT_STARTING: new Set(Array.from(["chars", // comment (cl-NN) ...].join(""))). Returns [(chars, cl)]."""
    found = re.findall(r'^export const %s: ReadonlySet<string> = new Set\(\s*Array\.from\(\s*\[\n(.*?)\n\s*\]\.join\(""\),?\s*\),?\s*\);'
                       % name, ts, re.M | re.S)
    if len(found) != 1:
        fail(f"punctuation.ts: expected one `export const {name}: ReadonlySet<string> = new Set(Array.from([...].join(\"\")));`")
    groups = []
    for line in found[0].split("\n"):
        m = re.fullmatch(r'\s*"([^"\\]*)",\s*//\s*(.*)', line)
        if not m:
            fail(f"punctuation.ts: {name}: expected `\"chars\", // comment`, got {line.strip()!r}")
        cl = re.search(r"[(（]cl-(\d\d)[)）]", m.group(2))
        groups.append((m.group(1), "cl-" + cl.group(1) if cl else None))
    return groups


def rules(ts, overlay):
    """{code point: set of class names}, and the code points the overlay adds a class to."""
    first_half = string_constant(ts, "FIRST_HALF")
    second_half = string_constant(ts, "SECOND_HALF")
    quarter = string_constant(ts, "QUARTER")
    not_starting = set_of_groups(ts, "KINSOKU_NOT_STARTING")
    # stone's FIRST_HALF is the closing brackets and 、。 (their glyphs in the em's first half); JLREQ's full stops
    # (cl-06) and commas (cl-07), tagged （cl-06） and （cl-07） in KINSOKU_NOT_STARTING, tell 、。 apart.
    stops = "".join(chars for chars, cl in not_starting if cl in ("cl-06", "cl-07"))
    if not any(cl == "cl-06" for _, cl in not_starting) or not any(cl == "cl-07" for _, cl in not_starting):
        fail("punctuation.ts: KINSOKU_NOT_STARTING has no group tagged (cl-06) or (cl-07)")
    sets = {
        "not_starting": "".join(chars for chars, _ in not_starting),
        "not_ending": set_of_string(ts, "KINSOKU_NOT_ENDING"),
        "inseparable": set_of_string(ts, "KINSOKU_INSEPARABLE"),
        "opening": second_half,
        "closing": "".join(c for c in first_half if c not in stops),
        "middle_dot": quarter,
        "full_stop_comma": "".join(c for c in first_half if c in stops),
        "hanging": set_of_string(ts, "KINSOKU_HANGING"),
    }
    for name, chars in sets.items():
        if not chars:
            fail(f"punctuation.ts: no characters for {name}")
    table = {}
    for name, chars in sets.items():
        for c in chars:
            table.setdefault(ord(c), set()).add(name)
    added = set()
    for addition in overlay["additions"]:
        if not addition.get("reason"):
            fail(f"overlay.json: {addition['chars']!r} has no reason")
        for name in addition["classes"]:
            if name not in CLASSES:
                fail(f"overlay.json: {addition['chars']!r}: no class {name!r} (one of {', '.join(CLASSES)})")
            for c in addition["chars"]:
                if name in table.get(ord(c), set()):
                    fail(f"overlay.json: U+{ord(c):04X} is {name} upstream already: drop it from the overlay")
                table.setdefault(ord(c), set()).add(name)
                added.add(ord(c))
    return table, added


# unicode.ts: scriptOfCodePoint is JAPANESE_CODE_POINTS first, then the first of UNICODE_BLOCKS holding the code point,
# then the script whose SCRIPT_CATEGORIES has that block; none is the Latin font (fonts.ts fontIdForChar). The pinned
# text of both (manifest.json "mirrored") makes a change to that order fail --check.
def scripts(ts):
    """[(first, last, script)]: the ranges of code points that are not Latin, in order, adjacent ones merged."""
    found = re.findall(r"^export const UNICODE_BLOCKS = \[\n(.*?)\n\] as const;$", ts, re.M | re.S)
    if len(found) != 1:
        fail("unicode.ts: expected one `export const UNICODE_BLOCKS = [ ... ] as const;`")
    blocks = []
    for line in found[0].split("\n"):
        m = re.fullmatch(r'\s*\["(\w+)", 0x([0-9a-fA-F]+), 0x([0-9a-fA-F]+)\],', line)
        if not m:
            fail(f"unicode.ts: UNICODE_BLOCKS: expected `[\"name\", 0xstart, 0xend],`, got {line.strip()!r}")
        blocks.append((m.group(1), int(m.group(2), 16), int(m.group(3), 16)))
    found = re.findall(r"^const SCRIPT_CATEGORIES: Record<Script, ReadonlySet<UnicodeCategory>> = \{\n(.*?)\n\};$", ts, re.M | re.S)
    if len(found) != 1:
        fail("unicode.ts: expected one `const SCRIPT_CATEGORIES: Record<Script, ReadonlySet<UnicodeCategory>> = { ... };`")
    script_of_block = {}
    for name, members in re.findall(r"^  (\w+): new Set<UnicodeCategory>\(\[(.*?)\]\),$", found[0], re.M | re.S):
        if name not in SCRIPTS:
            fail(f"unicode.ts: SCRIPT_CATEGORIES has {name!r}, not one of {', '.join(SCRIPTS)}")
        for block in re.findall(r'"(\w+)"', members):
            script_of_block.setdefault(block, name)
    if set(script_of_block.values()) != set(SCRIPTS):
        fail("unicode.ts: SCRIPT_CATEGORIES: expected latin, japanese and emoji")
    found = re.findall(r"^const JAPANESE_CODE_POINTS: ReadonlySet<number> = new Set\(\[\n(.*?)\n\]\);$", ts, re.M | re.S)
    if len(found) != 1:
        fail("unicode.ts: expected one `const JAPANESE_CODE_POINTS: ReadonlySet<number> = new Set([ ... ]);`")
    japanese = {int(h, 16) for h in re.findall(r"\b0x([0-9a-fA-F]+)\b", re.sub(r"//.*", "", found[0]))}
    if not japanese:
        fail("unicode.ts: JAPANESE_CODE_POINTS is empty")

    def script(cp):
        if cp in japanese:
            return "japanese"
        for name, start, end in blocks:
            if start <= cp < end:
                return script_of_block.get(name, "latin")
        return "latin"

    # The script is constant between the blocks' bounds and the Japanese code points: test each such stretch once.
    bounds = sorted({0, 0x110000} | {b for _, s, e in blocks for b in (s, e)} | {b for cp in japanese for b in (cp, cp + 1)})
    ranges = []
    for start, end in zip(bounds, bounds[1:]):
        name = script(start)
        if name == "latin":
            continue
        if ranges and ranges[-1][1] == start - 1 and ranges[-1][2] == name:
            ranges[-1] = (ranges[-1][0], end - 1, name)
        else:
            ranges.append((start, end - 1, name))
    return ranges


def render_scripts(manifest):
    source = upstream(manifest, SOURCE)
    ranges = scripts(read(os.path.join(UPSTREAM, SOURCE, "src/unicode.ts")))
    out = [
        "// GENERATED by tools/cjk_rules/generate.py: do not edit. `generate.py --check` (a ctest test) fails on any",
        "// difference. To change it, update the upstream as .agents/skills/cjk-rules/SKILL.md says.",
        "//",
        f"// From {source['name']} {source['repo']}",
        f"// commit {source['commit']}, src/unicode.ts (UNICODE_BLOCKS, SCRIPT_CATEGORIES, JAPANESE_CODE_POINTS;",
        "// MIT licence: tools/cjk_rules/upstream/stone_engine_js/LICENSE, THIRD_PARTY_NOTICES.md).",
        "//",
        "// {first, last, script}: the code points stone sets as Japanese or emoji, sorted; every other one is Latin.",
        "// litehtml/src/cjk_rules.cpp includes it in its table.",
    ]
    for first, last, name in ranges:
        out.append(f"{{0x{first:04X}, 0x{last:04X}, script::{name}}},")
    return "\n".join(out) + "\n"


def licence_holder(path):
    m = re.search(r"^Copyright \(c\) (.+)$", read(path), re.M)
    if not m:
        fail(f"{path}: no `Copyright (c) ...` line")
    return m.group(1)


def render(manifest, overlay):
    source = upstream(manifest, SOURCE)
    base = os.path.join(UPSTREAM, SOURCE)
    table, added = rules(read(os.path.join(base, "src/punctuation.ts")), overlay)
    holder = licence_holder(os.path.join(base, "LICENSE"))
    out = [
        "// GENERATED by tools/cjk_rules/generate.py: do not edit. `generate.py --check` (a ctest test) fails on any",
        "// difference. To change the rules, edit tools/cjk_rules/overlay.json, or update the upstream as",
        "// .agents/skills/cjk-rules/SKILL.md says; then run python3 tools/cjk_rules/generate.py.",
        "//",
        f"// From {source['name']} {source['repo']}",
        f"// commit {source['commit']}, src/punctuation.ts",
        f"// ({source['licence']} licence, Copyright (c) {holder}: tools/cjk_rules/upstream/{source['name']}/LICENSE,",
        "// THIRD_PARTY_NOTICES.md), with tools/cjk_rules/overlay.json (marked \"overlay\").",
        "//",
        "// {code point, classes}, sorted by code point; litehtml/src/cjk_rules.cpp includes it in its table.",
    ]
    for cp in sorted(table):
        bits = " | ".join(name for name in CLASSES if name in table[cp])
        shown = chr(cp) if cp > 0x20 and chr(cp) != "\\" else ""
        note = f"U+{cp:04X} {shown}".rstrip() + (" (overlay)" if cp in added else "")
        out.append(f"{{0x{cp:04X}, {bits}}}, // {note}")
    return "\n".join(out) + "\n"


# Tate-chu-yoko: stone sets a token of 1 or 2 numbers in one em (context.ts isTateChuYoko, which litehtml mirrors in
# cjk::tate_chu_yoko); the overlay adds whole tokens. Each is 1 or 2 characters (one em holds no more), has no digit
# (the numbers are stone's rule), and two are both Latin, so that justify has no gap between them (cjk::justifiable_gap).
def render_tate_chu_yoko(manifest, overlay):
    ranges = scripts(read(os.path.join(UPSTREAM, SOURCE, "src/unicode.ts")))

    def latin(c):
        return not any(first <= ord(c) <= last for first, last, _ in ranges)

    out = [
        "// GENERATED by tools/cjk_rules/generate.py: do not edit. `generate.py --check` (a ctest test) fails on any",
        "// difference. To change it, edit the \"tate_chu_yoko\" entries of tools/cjk_rules/overlay.json, then run",
        "// python3 tools/cjk_rules/generate.py.",
        "//",
        "// The tokens our overlay sets as tate-chu-yoko (縦中横), UTF-8, besides stone_engine's numbers (cjk::tate_chu_yoko).",
        "// litehtml/src/cjk_rules.cpp includes it in its table.",
    ]
    seen = set()
    for entry in overlay.get("tate_chu_yoko", []):
        if not entry.get("reason"):
            fail(f"overlay.json: tate_chu_yoko {entry.get('tokens')!r} has no reason")
        for token in entry["tokens"]:
            if not 1 <= len(token) <= 2:
                fail(f"overlay.json: tate_chu_yoko {token!r}: a token is 1 or 2 characters")
            if any(c.isdigit() for c in token):
                fail(f"overlay.json: tate_chu_yoko {token!r}: numbers are stone's rule, not the overlay's")
            if len(token) == 2 and not all(latin(c) and not c.isspace() for c in token):
                fail(f"overlay.json: tate_chu_yoko {token!r}: two characters must be Latin (justify would put a gap between them)")
            if token in seen:
                fail(f"overlay.json: tate_chu_yoko {token!r} is listed twice")
            seen.add(token)
            escaped = "".join(f"\\x{b:02X}" for b in token.encode())
            codes = " ".join(f"U+{ord(c):04X}" for c in token)
            out.append(f'"{escaped}", // {codes} {token} (overlay)')
    if not seen:
        out.append("nullptr, // none")
    return "\n".join(out) + "\n"


def vendor(manifest, name, checkout):
    entry = upstream(manifest, name)
    head = subprocess.run(["git", "-C", checkout, "rev-parse", "HEAD"], capture_output=True, text=True, check=True).stdout.strip()
    if head != entry["commit"]:
        fail(f"{checkout} is at {head}, manifest.json pins {name} at {entry['commit']}")
    paths = sorted(set(entry["paths"] + ["LICENSE"] + list(entry["sha256"]) + [m["path"] for m in entry.get("mirrored", [])]))
    entry["sha256"] = {}
    for path in paths:
        data = subprocess.run(["git", "-C", checkout, "show", f"{head}:{path}"], capture_output=True, check=True).stdout
        local = os.path.join(UPSTREAM, name, path)
        os.makedirs(os.path.dirname(local), exist_ok=True)
        with open(local, "wb") as f:
            f.write(data)
        entry["sha256"][path] = hashlib.sha256(data).hexdigest()
    with open(MANIFEST, "w", encoding="utf-8") as f:
        f.write(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")
    print(f"vendored {name} {head}: {', '.join(paths)}")


def tables(manifest, overlay):
    return [(TABLE, render(manifest, overlay)), (SCRIPTS_TABLE, render_scripts(manifest)), (TCY_TABLE, render_tate_chu_yoko(manifest, overlay))]


def main(argv):
    manifest = json.loads(read(MANIFEST))
    overlay = json.loads(read(OVERLAY))
    if argv[:1] == ["--vendor"] and len(argv) == 3:
        vendor(manifest, argv[1], argv[2])
        return 0
    if argv == ["--check"]:
        problems = check_hashes(manifest)
        if not problems:
            for path, text in tables(manifest, overlay):
                if not os.path.isfile(path) or open(path, "rb").read() != text.encode():
                    problems.append(f"{os.path.relpath(path, REPO)} is not what generate.py writes (edited by hand, or not regenerated)")
            problems += check_mirrored(manifest)
        for p in problems:
            print("cjk_rules: " + p, file=sys.stderr)
        if not problems:
            print("cjk_rules: the vendored upstream matches manifest.json, the tables are generated from it, and the "
                  "code litehtml mirrors is as reviewed")
        return 1 if problems else 0
    if argv:
        print(__doc__, file=sys.stderr)
        return 2
    problems = check_hashes(manifest)
    if problems:
        fail("; ".join(problems))
    for path, text in tables(manifest, overlay):
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print(f"wrote {os.path.relpath(path, REPO)}")
    for p in check_mirrored(manifest):
        print("cjk_rules: " + p, file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
