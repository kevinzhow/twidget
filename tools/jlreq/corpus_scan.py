#!/usr/bin/env python3
"""corpus_scan.py <out.json> <epub|dir>...: how often each JLReq case occurs in EPUBs (text only, no layout).

Counts are per book and in total; for each case a few examples (book, spine item, a short context) are kept so that a
reader can find the page. Only the public corpus is scanned for the report; nothing of a book's text leaves the
machine except the short contexts of public books.
"""
import json, re, sys, os, zipfile, posixpath
from html.parser import HTMLParser

OPEN = "「『（〔［｛〈《【〘〖〝‘“｟«"
CLOSE = "」』）〕］｝〉》】〙〗〟’”｠»"
STOP_COMMA = "。、．，"
MIDDLE = "・：；"
DIVIDING = "！？‼⁇⁈⁉"
SMALL = "ぁぃぅぇぉっゃゅょゎゕゖァィゥェォッャュョヮヵヶㇰㇱㇲㇳㇴㇵㇶㇷㇸㇹㇺㇻㇼㇽㇾㇿ"
BLOCKS = {"p", "div", "li", "h1", "h2", "h3", "h4", "h5", "h6", "blockquote", "td", "th", "dd", "dt", "body",
          "section", "article", "figcaption", "caption", "pre"}


def kind(c):
    if not c:
        return "none"
    o = ord(c)
    if c in OPEN: return "open"
    if c in CLOSE: return "close"
    if c in STOP_COMMA: return "stop"
    if c in MIDDLE: return "middle"
    if c in DIVIDING: return "dividing"
    if c in "ー〜": return "prolonged"
    if c in "々ゝゞヽヾ〻": return "iteration"
    if c in SMALL: return "small"
    if 0x3041 <= o <= 0x309F: return "hira"
    if 0x30A0 <= o <= 0x30FF or 0x31F0 <= o <= 0x31FF or 0xFF66 <= o <= 0xFF9F: return "kata"
    if 0x4E00 <= o <= 0x9FFF or 0x3400 <= o <= 0x4DBF or 0xF900 <= o <= 0xFAFF or 0x20000 <= o <= 0x3FFFF or c == "〇":
        return "kanji"
    if c in "…‥—―": return "inseparable"
    if c == "　": return "ideo-space"
    if c.isspace(): return "space"
    if c.isascii() and (c.isalnum()): return "latin"
    if 0xFF10 <= o <= 0xFF5A: return "fullwidth-latin"
    if c.isalpha(): return "latin"
    return "other"


def japanese(c):
    return kind(c) in ("hira", "kata", "kanji", "small", "prolonged", "iteration", "open", "close", "stop", "middle",
                       "dividing", "inseparable", "ideo-space")


class Doc(HTMLParser):
    """The text of a document as runs, with ruby pairs kept: a list of blocks, each a list of items: ("t", text) or
    ("ruby", [(base, annotation), ...]) or ("tag", name, attrs) for spans with a class (emphasis lookups)."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.blocks, self.cur, self.stack = [], [], []
        self.ruby = None  # [pairs, base_text, in_rt, rt_text]
        self.css_inline, self.links, self.classes_at = [], [], []
        self.in_style = self.skip = 0
        self.tcy_classes = set()

    def flush(self):
        if any(i[0] != "t" or i[1].strip() for i in self.cur):
            self.blocks.append(self.cur)
        self.cur = []

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == "style":
            self.in_style = 1
        if tag in ("script", "rp"):
            self.skip += 1
        if tag == "link" and "stylesheet" in (a.get("rel") or ""):
            self.links.append(a.get("href") or "")
        if tag == "br":
            self.cur.append(("t", "\n"))
        if tag in BLOCKS and self.ruby is None:
            self.flush()
        if tag == "ruby":
            self.ruby = [[], "", False, ""]
        elif self.ruby is not None and tag == "rt":
            self.ruby[2] = True
        self.stack.append((tag, (a.get("class") or "").split(), a.get("style") or ""))
        if a.get("class") or a.get("style"):
            self.cur.append(("open", tag, (a.get("class") or "").split(), a.get("style") or ""))

    def handle_endtag(self, tag):
        if tag == "style":
            self.in_style = 0
        if tag in ("script", "rp"):
            self.skip = max(0, self.skip - 1)
        if self.ruby is not None and tag == "rt":
            r = self.ruby
            r[0].append((r[1], r[3]))
            r[1], r[2], r[3] = "", False, ""
        elif tag == "ruby" and self.ruby is not None:
            r = self.ruby
            if r[1].strip():
                r[0].append((r[1], ""))
            self.cur.append(("ruby", r[0]))
            self.ruby = None
        while self.stack:
            t, cls, st = self.stack.pop()
            if cls or st:
                self.cur.append(("close", t))
            if t == tag:
                break
        if tag in BLOCKS and self.ruby is None:
            self.flush()

    def handle_data(self, d):
        if self.in_style:
            self.css_inline.append(d)
            return
        if self.skip:
            return
        if self.ruby is not None:
            if self.ruby[2]:
                self.ruby[3] += d.strip()
            else:
                self.ruby[1] += d.strip() if not d.strip() == "" else ""
            return
        self.cur.append(("t", d))


LINE = 20  # characters per line in the simulation (a phone page at the default size)
NOT_START = ("close", "stop", "middle", "dividing", "small", "prolonged", "iteration")


def simulate_lines(text, rubies, mode, add):
    """Fixed-pitch line breaking of a paragraph at LINE ems (each character an em, a ruby's base unbreakable, kinsoku
    by pushing characters out): counts what happens at the lines' edges."""
    if "\n" in text:
        for part in text.split("\n"):
            simulate_lines(part, [], mode, add)
        return
    n = len(text)
    if n <= LINE:
        return
    long_at = {}
    group_end = {}
    for start, pairs in rubies:
        base = "".join(b for b, _ in pairs)
        ann = "".join(a for _, a in pairs)
        group_end[start] = start + len(base)
        long_at[start] = len(ann) > 2 * len(base)
    i = 0
    first = True
    while n - i > LINE:
        end = i + LINE
        # a ruby base across the edge goes to the next line
        for s, e in group_end.items():
            if s < end < e:
                end = s
        adjusted = False
        while end > i + 1 and (kind(text[end]) in NOT_START or kind(text[end - 1]) == "open" or
                               (kind(text[end - 1]) == kind(text[end]) == "inseparable")):
            end -= 1
            adjusted = True
        if adjusted:
            add("sim-kinsoku-adjusted-line:" + mode)
        last = text[end - 1]
        if kind(last) in ("close", "stop"):
            add("sim-line-end-close-stop:" + mode)
        if kind(last) == "middle":
            add("sim-line-end-middle:" + mode)
        add("sim-wrapped-lines:" + mode)
        if any(s == end and long_at[s] for s in group_end) or any(e == end and long_at[s] for s, e in group_end.items()):
            pass
        for s, e in group_end.items():
            if long_at[s] and e == end:
                add("sim-ruby-long-line-end:" + mode)
            if long_at[s] and s == end:
                add("sim-ruby-long-line-head:" + mode)
        if kind(text[end]) == "open":
            add("sim-wrapped-line-start-open:" + mode)
        i = end
        first = False


def css_rules(css):
    css = re.sub(r"/\*.*?\*/", "", css, flags=re.S)
    for m in re.finditer(r"([^{}]+)\{([^{}]*)\}", css):
        yield m.group(1).strip(), m.group(2)


def scan_book(path, out, examples, public_name):
    z = zipfile.ZipFile(path)
    names = set(z.namelist())
    try:
        container = z.read("META-INF/container.xml").decode("utf-8", "replace")
        opf_path = re.search(r'full-path="([^"]+)"', container).group(1)
        opf = z.read(opf_path).decode("utf-8", "replace")
    except Exception:
        return
    base = posixpath.dirname(opf_path)
    manifest = {}
    for m in re.finditer(r"<(?:\w+:)?item\b([^>]*)/?>", opf):
        at = dict(re.findall(r'([\w:-]+)="([^"]*)"', m.group(1)))
        if "id" in at and "href" in at:
            manifest[at["id"]] = (posixpath.normpath(posixpath.join(base, at["href"])), at.get("media-type", ""))
    spine = [m.group(1) for m in re.finditer(r'<(?:\w+:)?itemref\b[^>]*idref="([^"]+)"', opf)]
    rtl = 'page-progression-direction="rtl"' in opf
    css_cache = {}
    c = out.setdefault(public_name, {})

    def add(key, n=1, ex=None):
        c[key] = c.get(key, 0) + n
        if ex is not None and len(examples.setdefault(key, [])) < 6 and not any(e[0] == public_name for e in examples[key][-2:]):
            examples[key].append(ex)

    for idref in spine:
        href, mt = manifest.get(idref, ("", ""))
        if href not in names or "html" not in mt:
            continue
        raw = z.read(href).decode("utf-8", "replace")
        d = Doc()
        try:
            d.feed(raw)
            d.close()
        except Exception:
            continue
        d.flush()
        css = "\n".join(d.css_inline)
        for l in d.links:
            p = posixpath.normpath(posixpath.join(posixpath.dirname(href), l.split("#")[0]))
            if p not in css_cache:
                css_cache[p] = z.read(p).decode("utf-8", "replace") if p in names else ""
            css += "\n" + css_cache[p]
        # style attributes count as css too, as one rule each
        vertical = bool(re.search(r"writing-mode\s*:\s*(vertical-rl|tb-rl)", css + raw)) or (rtl and "vertical" in css)
        mode = "v" if vertical else "h"
        emph_classes, tcy_classes, indent_classes = set(), set(), set()
        block_indent = False
        for sel, body in css_rules(css):
            names_in = set(re.findall(r"\.([\w-]+)", sel))
            if re.search(r"text-emphasis(-style)?\s*:", body):
                emph_classes |= names_in
            if re.search(r"text-combine(-upright|-horizontal)?\s*:\s*(all|digits)", body):
                tcy_classes |= names_in
            m = re.search(r"text-indent\s*:\s*([-\d.]+)\s*(em|rem|px|%)?", body)
            if m and float(m.group(1)) > 0:
                if names_in:
                    indent_classes |= names_in
                if re.search(r"(^|[\s,>])(p|body|div)\b", sel) and not names_in:
                    block_indent = True
        if raw.find("text-combine") >= 0:
            add("css:text-combine-upright:" + mode)
        ctx_doc = posixpath.basename(href)
        for blk in d.blocks:
            # the block's flat text, with ruby bases inline and markers of ruby positions
            flat, rubies, emph_depth, emph_chars, tcy_depth = [], [], 0, 0, 0
            open_stack = []
            for it in blk:
                if it[0] == "t":
                    flat.append(it[1])
                    if emph_depth:
                        emph_chars += len(it[1].strip())
                elif it[0] == "ruby":
                    start = sum(len(x) for x in flat)
                    text = "".join(b for b, _ in it[1])
                    rubies.append((start, it[1]))
                    flat.append(text)
                    if emph_depth:
                        emph_chars += len(text)
                elif it[0] == "open":
                    e = bool(set(it[2]) & emph_classes) or "text-emphasis" in it[3]
                    t = bool(set(it[2]) & tcy_classes) or "text-combine" in it[3]
                    open_stack.append((e, t))
                    emph_depth += e
                    tcy_depth += t
                elif it[0] == "close" and open_stack:
                    e, t = open_stack.pop()
                    emph_depth -= e
                    tcy_depth -= t
            text = "".join(flat)
            if emph_chars:
                add("emphasis-chars:" + mode, emph_chars, (public_name, ctx_doc, ""))
            body_text = text.lstrip(" \t\r\n")
            if not body_text.strip():
                continue
            jp = sum(1 for ch in body_text if japanese(ch))
            if jp < max(2, len(body_text.strip()) // 4):
                continue  # not a Japanese block
            add("jp-blocks:" + mode)
            first = body_text[0]
            if kind(first) == "open":
                add("para-start-open:" + mode, 1, (public_name, ctx_doc, body_text[:14]))
            elif first == "　" and len(body_text) > 1 and kind(body_text[1]) == "open":
                add("para-start-space-open:" + mode, 1, (public_name, ctx_doc, body_text[:14]))
            elif first == "　":
                add("para-start-ideo-space:" + mode)
            # opening brackets inside a paragraph (any of them may start a wrapped line)
            add("open-mid:" + mode, sum(1 for ch in body_text[1:] if kind(ch) == "open"))
            add("brackets-and-stops:" + mode, sum(1 for ch in body_text if kind(ch) in ("open", "close", "stop", "middle")))
            for m in re.finditer("[%s](?=[^%s%s%s\\s　])" % (DIVIDING, CLOSE, DIVIDING, STOP_COMMA), body_text):
                add("dividing-then-text:" + mode, 1, (public_name, ctx_doc, body_text[max(0, m.start() - 6):m.start() + 6]))
            for m in re.finditer("[，．]", body_text):
                add("fullwidth-comma-period:" + mode, 1, (public_name, ctx_doc, body_text[max(0, m.start() - 6):m.start() + 4]))
            for i in range(1, len(body_text)):
                a, b = kind(body_text[i - 1]), kind(body_text[i])
                if (a in ("hira", "kata", "kanji") and b == "latin") or (a == "latin" and b in ("hira", "kata", "kanji")):
                    add("jp-latin-boundary:" + mode, 1, (public_name, ctx_doc, body_text[max(0, i - 6):i + 6]))
            for m in re.finditer("[〇一二三四五六七八九][・、][〇一二三四五六七八九]", body_text):
                add("kanji-numeral-dot-comma:" + mode, 1, (public_name, ctx_doc, body_text[max(0, m.start() - 4):m.end() + 4]))
            for m in re.finditer("(——|――|……|‥‥)", body_text):
                add("double-dash-leader:" + mode)
            if mode == "v":
                for m in re.finditer(r"(?<![0-9A-Za-z.,])[0-9]{1,2}(?![0-9A-Za-z.,])", body_text):
                    add("tcy-digits-auto:" + mode)
                for m in re.finditer(r"(?<![0-9A-Za-z])[0-9]{3,4}(?![0-9A-Za-z])", body_text):
                    add("v-digits-3-4:" + mode)
                for m in re.finditer(r"[A-Za-z]{2,}", body_text):
                    add("v-latin-word:" + mode)
            simulate_lines(text.strip("\n"), rubies, mode, add)
            # ruby
            for k, (start, pairs) in enumerate(rubies):
                base = "".join(b for b, _ in pairs)
                ann = "".join(a for _, a in pairs)
                if not ann:
                    continue
                add("ruby:" + mode)
                if len(pairs) > 1:
                    add("ruby-multi-pair:" + mode, 1, (public_name, ctx_doc, base + "(" + "|".join(a for _, a in pairs) + ")"))
                    if any(len(a) > 2 * len(b) for b, a in pairs):
                        add("ruby-jukugo-long-unit:" + mode)
                if len(base) == 1 and len(ann) == 1:
                    add("ruby-mono-1-kana:" + mode)
                if len(ann) > 2 * len(base):
                    before = text[start - 1] if start > 0 else ""
                    after = text[start + len(base)] if start + len(base) < len(text) else ""
                    kb, ka = kind(before), kind(after)
                    adj_ruby = any(s == start + len(base) for s, _ in rubies) or any(s + len("".join(b for b, _ in p)) == start for s, p in rubies)
                    add("ruby-long:" + mode, 1, (public_name, ctx_doc, text[max(0, start - 3):start] + "[" + base + "/" + ann + "]" + text[start + len(base):start + len(base) + 3]))
                    for side, kk in (("before", kb), ("after", ka)):
                        if kk in ("hira", "kata", "small", "prolonged"):
                            add("ruby-long-%s-kana:%s" % (side, mode))
                        elif kk == "kanji" or kk == "iteration":
                            add("ruby-long-%s-kanji:%s" % (side, mode))
                        elif kk in ("open", "close", "stop", "middle", "inseparable", "dividing"):
                            add("ruby-long-%s-punct:%s" % (side, mode))
                        elif kk in ("none", "space", "ideo-space"):
                            add("ruby-long-%s-edge:%s" % (side, mode))
                        else:
                            add("ruby-long-%s-other:%s" % (side, mode))
                    if kb in ("hira", "kata", "small", "prolonged") or ka in ("hira", "kata", "small", "prolonged"):
                        add("ruby-long-any-kana:" + mode, 1, (public_name, ctx_doc, text[max(0, start - 3):start] + "[" + base + "/" + ann + "]" + text[start + len(base):start + len(base) + 3]))
                    if adj_ruby:
                        add("ruby-long-next-to-ruby:" + mode)
                if len(ann) * 2 < len(base):
                    add("ruby-short:" + mode)
        if indent_classes or block_indent:
            add("doc-css-text-indent:" + mode)
        add("docs:" + mode)


def main():
    out, examples = {}, {}
    files = []
    for a in sys.argv[2:]:
        if os.path.isdir(a):
            files += sorted(os.path.join(a, f) for f in os.listdir(a) if f.endswith(".epub"))
        else:
            files.append(a)
    for f in files:
        scan_book(f, out, examples, os.path.splitext(os.path.basename(f))[0])
    total = {}
    for book, cs in out.items():
        for k, v in cs.items():
            total[k] = total.get(k, 0) + v
    books_with = {}
    for book, cs in out.items():
        for k, v in cs.items():
            if v:
                books_with[k] = books_with.get(k, 0) + 1
    json.dump({"total": total, "books_with": books_with, "per_book": out, "examples": examples}, open(sys.argv[1], "w"),
              ensure_ascii=False, indent=1, sort_keys=True)
    for k in sorted(total):
        print("%-36s %8d  (%d books)" % (k, total[k], books_with.get(k, 0)))


main()
