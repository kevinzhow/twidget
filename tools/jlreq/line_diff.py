#!/usr/bin/env python3
"""line_diff.py <before reader-cli> <after reader-cli> <out.json> <epub>... : what JLReq line adjustment (batch 2) changes,
line by line. Each spine item with Japanese text is laid out by both reader-clis (phone size, 390x700, the book's own
CSS) and drawn whole (--dump-doc); its lines are rebuilt from the drawn text (ruby annotations left out) and paired by
their text. Per book, counts only (no book text, so it can run on copyrighted books):

  lines         lines after
  breaks        lines whose text changed (where the line breaks moved)
  spacing       lines with the same text whose characters moved (squeezed or spread differently)
  overflow      lines reaching past the column's end (the page less the margin before its lines, or where most lines
                end), not counting a U+3000 that hangs there (before / after)
  bracket_gap   brackets and 、。 with space added on their blank side beyond their own half em (and the chapter's usual
                gap between two kana or kanji, a book's letter-spacing) (before / after)

--samples DIR (public books only) also writes, per book, a few changed lines' texts before and after.
"""
import collections, difflib, json, os, posixpath, re, subprocess, sys, tempfile, zipfile

TEXT = 4  # twlite::Cmd::Text in reader-cli's dump
OPENING = set("「『（〔［｛〈《【〘〖〝‘“｟")
CLOSING = set("」』）〕］｝〉》】〙〗〟’”｠、。")
JAPANESE = re.compile(r"[぀-ヿ一-鿿]")


def wide(c):
    o = ord(c)
    return 0x2e80 <= o <= 0x9fff or 0xf900 <= o <= 0xfaff or 0xfe30 <= o <= 0xfe4f or 0xff00 <= o <= 0xff60 or 0x20000 <= o


def lay_out(cli, xhtml, work):
    doc = os.path.join(work, "doc.txt")
    r = subprocess.run([cli, xhtml, os.path.join(work, "p"), "--width", "390", "--height", "700", "--pages", "none",
                        "--dump-doc", doc], capture_output=True, timeout=300)
    if r.returncode != 0 or not os.path.exists(doc):
        return None
    ops = []
    for ln in open(doc, encoding="utf-8", errors="replace"):
        parts = ln.rstrip("\n").split(" ", 7)
        if len(parts) == 8 and parts[0] == str(TEXT):
            x, y, a, b, size = map(float, parts[1:6])
            ops.append((x, y, a, b, size, parts[6], parts[7]))
    os.remove(doc)
    return ops


def lines_of(ops, vertical):
    """The base-text lines: ops grouped by their line (y across horizontal text, x across vertical), in reading order;
    each a list of (char, position along the line, width or None)."""
    if not ops:
        return []
    body = collections.Counter(round(o[4]) for o in ops).most_common(1)[0][0]
    # A line's ops are within a fraction of an em of each other across it (a tate-chu-yoko token's a little off its
    # column's); lines are a line height apart.
    placed = sorted(((x, y) if vertical else (y, x)) + (size, feat, text)
                    for x, y, a, b, size, feat, text in ops if size >= 0.75 * body)  # not ruby annotations, small print
    groups = []
    for across, along, size, feat, text in placed:
        if not groups or across - groups[-1][0] > 0.45 * body:
            groups.append([across, []])
        groups[-1][0] = across
        groups[-1][1].append((along, size, feat, text))
    out = []
    for _, group in (reversed(groups) if vertical else groups):
        chars = []
        for along, size, feat, text in sorted(group):
            half = "halt" in feat or "vhal" in feat
            pos = along
            for c in text:  # a Latin character's width is not known: the rest of its op has no positions
                w = size / 2 if half else size if wide(c) else None
                chars.append((c, pos, w))
                pos = pos + w if pos is not None and w is not None else None
        out.append(chars)
    return out


def text_of(line):
    return "".join(c for c, _, _ in line if not c.isspace())


def gaps_of(line):
    return [(c0, c1, p1 - (p0 + w0)) for (c0, p0, w0), (c1, p1, w1) in zip(line, line[1:]) if None not in (p0, w0, p1)]


KANA_KANJI = re.compile(r"[\u3041-\u30fa\u4e00-\u9fff]")


def usual_gap(lines):
    """The most common gap between two kana or kanji: 0, or the book's letter-spacing."""
    c = collections.Counter(round(g, 1) for l in lines for c0, c1, g in gaps_of(l)
                            if KANA_KANJI.match(c0) and KANA_KANJI.match(c1))
    return c.most_common(1)[0][0] if c else 0.0


def bracket_gaps(line, usual):
    """Brackets and 、。 with more space on their blank side than `usual`."""
    return sum(1 for c0, c1, g in gaps_of(line) if g > usual + 0.5 and (c1 in OPENING or c0 in CLOSING))


def line_end(line):
    ends = [p + w for _, p, w in line if p is not None and w is not None]
    return max(ends) if ends else None


def spine(z):
    opf = re.search(r'full-path="([^"]+)"', z.read("META-INF/container.xml").decode()).group(1)
    s = z.read(opf).decode("utf-8", "replace")
    base = posixpath.dirname(opf)
    man = {}
    for m in re.finditer(r"<(?:\w+:)?item\b([^>]*)/?>", s):
        at = dict(re.findall(r'([\w:-]+)="([^"]*)"', m.group(1)))
        if "id" in at and "href" in at:
            man[at["id"]] = posixpath.normpath(posixpath.join(base, at["href"]))
    return [man[i] for i in re.findall(r'<(?:\w+:)?itemref\b[^>]*idref="([^"]+)"', s) if i in man]


def main():
    args = sys.argv[1:]
    samples = None
    if "--samples" in args:
        k = args.index("--samples")
        samples = args[k + 1]
        del args[k:k + 2]
    before, after, out_json, epubs = args[0], args[1], args[2], args[3:]
    result = {}
    for epub in epubs:
        name = os.path.splitext(os.path.basename(epub))[0]
        book = collections.Counter()
        shown = []
        with tempfile.TemporaryDirectory() as work:
            z = zipfile.ZipFile(epub)
            z.extractall(work)
            for href in spine(z):
                path = os.path.join(work, href)
                if not os.path.exists(path) or not path.endswith(("html", "htm")):
                    continue
                raw = open(path, encoding="utf-8", errors="replace").read()
                if len(JAPANESE.findall(raw)) < 50:
                    continue
                ob, oa = lay_out(before, path, work), lay_out(after, path, work)
                if ob is None or oa is None:
                    book["failed"] += 1
                    continue
                vertical = sum("vert" in o[5] for o in oa) > len(oa) / 2  # upright glyphs drawn with vert=1
                lb, la = lines_of(ob, vertical), lines_of(oa, vertical)
                tb, ta = [text_of(l) for l in lb], [text_of(l) for l in la]
                book["chapters"] += 1
                book["lines"] += len(la)
                # The column's end: the page's (390 x 700) less the margin its lines start after, as if the same each
                # side (ragged text may never reach it before), or where most lines end if further (justified text, a
                # margin narrower at the end).
                starts = [p for l in lb + la for _, p, _ in l[:1] if p is not None]
                ends = collections.Counter(round(e) for e in (line_end(l) for l in lb + la) if e is not None)
                column_end = max((700 if vertical else 390) - min(starts), ends.most_common(1)[0][0]) if starts else None
                usual = usual_gap(lb + la)
                for tag, lines in (("before", lb), ("after", la)):
                    book["bracket_gap_" + tag] += sum(bracket_gaps(l, usual) for l in lines)
                    if column_end is not None:
                        # Not a line whose U+3000 hangs past the end (CSS Text 3 §4.1.3, as before).
                        book["overflow_" + tag] += sum(1 for l in lines if (line_end(l) or 0) > column_end + 1.5
                                                       and not (l and l[-1][0] == "\u3000"))
                for op, i0, i1, j0, j1 in difflib.SequenceMatcher(None, tb, ta, autojunk=False).get_opcodes():
                    if op == "equal":
                        for i, j in zip(range(i0, i1), range(j0, j1)):
                            pb = [p for _, p, _ in lb[i]]
                            pa = [p for _, p, _ in la[j]]
                            if any(x is not None and y is not None and abs(x - y) > 0.3 for x, y in zip(pb, pa)):
                                book["spacing"] += 1
                                if samples and len(shown) < 12:
                                    shown.append(("spacing", href, tb[i], ta[j]))
                    else:
                        book["breaks"] += j1 - j0
                        if samples and len(shown) < 12:
                            shown.append(("breaks", href, "|".join(tb[i0:i1]), "|".join(ta[j0:j1])))
        result[name] = dict(book)
        print(name, dict(book), flush=True)
        if samples and shown:
            os.makedirs(samples, exist_ok=True)
            json.dump(shown, open(os.path.join(samples, name + ".json"), "w"), ensure_ascii=False, indent=1)
    json.dump(result, open(out_json, "w"), indent=1)


if __name__ == "__main__":
    main()
