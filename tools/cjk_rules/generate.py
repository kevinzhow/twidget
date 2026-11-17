#!/usr/bin/env python3
"""Generates litehtml/src/cjk_rules_table.inc, the CJK line-composition rule set behind litehtml/cjk_rules.h, from
stone_engine_js's src/punctuation.ts (vendored under upstream/ at the commit in manifest.json) and overlay.json.

usage:
  generate.py                     write the table
  generate.py --check             exit 1 unless the vendored files match manifest.json's hashes and the table is
                                  what this script writes (a ctest test: nobody edits the table by hand)
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
SOURCE = "stone_engine_js"  # the upstream whose tables are read; the others are kept for reference

# The class bits, in cjk_rules.h's order (litehtml::cjk::class_bit).
CLASSES = ["not_starting", "not_ending", "inseparable", "opening", "closing", "middle_dot", "full_stop_comma", "hanging"]


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


def licence_holder(path):
    m = re.search(r"^Copyright \(c\) (.+)$", read(path), re.M)
    if not m:
        fail(f"{path}: no `Copyright (c) ...` line")
    return m.group(1)


def render(manifest, overlay):
    source = upstream(manifest, SOURCE)
    base = os.path.join(UPSTREAM, SOURCE)
    table, added = rules(read(os.path.join(base, source["path"])), overlay)
    holder = licence_holder(os.path.join(base, "LICENSE"))
    out = [
        "// GENERATED by tools/cjk_rules/generate.py: do not edit. `generate.py --check` (a ctest test) fails on any",
        "// difference. To change the rules, edit tools/cjk_rules/overlay.json, or update the upstream as",
        "// .agents/skills/cjk-rules/SKILL.md says; then run python3 tools/cjk_rules/generate.py.",
        "//",
        f"// From {source['name']} {source['repo']}",
        f"// commit {source['commit']}, {source['path']}",
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


def vendor(manifest, name, checkout):
    entry = upstream(manifest, name)
    head = subprocess.run(["git", "-C", checkout, "rev-parse", "HEAD"], capture_output=True, text=True, check=True).stdout.strip()
    if head != entry["commit"]:
        fail(f"{checkout} is at {head}, manifest.json pins {name} at {entry['commit']}")
    paths = sorted(set([entry["path"], "LICENSE"] + list(entry["sha256"])))
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


def main(argv):
    manifest = json.loads(read(MANIFEST))
    overlay = json.loads(read(OVERLAY))
    if argv[:1] == ["--vendor"] and len(argv) == 3:
        vendor(manifest, argv[1], argv[2])
        return 0
    if argv == ["--check"]:
        problems = check_hashes(manifest)
        if not problems and (not os.path.isfile(TABLE) or open(TABLE, "rb").read() != render(manifest, overlay).encode()):
            problems.append(f"{os.path.relpath(TABLE, REPO)} is not what generate.py writes (edited by hand, or not regenerated)")
        for p in problems:
            print("cjk_rules: " + p, file=sys.stderr)
        if not problems:
            print("cjk_rules: the vendored upstream matches manifest.json and the table is generated from it")
        return 1 if problems else 0
    if argv:
        print(__doc__, file=sys.stderr)
        return 2
    problems = check_hashes(manifest)
    if problems:
        fail("; ".join(problems))
    with open(TABLE, "w", encoding="utf-8", newline="\n") as f:
        f.write(render(manifest, overlay))
    print(f"wrote {os.path.relpath(TABLE, REPO)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
