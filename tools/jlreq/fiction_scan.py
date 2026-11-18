#!/usr/bin/env python3
"""fiction_scan.py <epub|dir>...: per book, the OPF metadata a fiction test can use (dc:subject, dc:type, genre-like
meta, the series: belongs-to-collection / calibre:series) and the share of its Japanese paragraphs that start with
「『（, as the reader's 自动 bracket scheme measures it. Prints metadata and counts only."""
import os, re, sys, zipfile, posixpath, json

OPEN = "「『（"
SAMPLE = 600  # the reader stops counting after this many Japanese paragraphs


def paragraphs(xhtml):
    """Paragraph texts: what lies between block boundaries (p, div, h1-h6, li, blockquote) and <br>, ruby
    annotations left out."""
    m = re.search(r"<body\b[^>]*>(.*)</body>", xhtml, flags=re.S)
    body = m.group(1) if m else xhtml
    body = re.sub(r"<rt\b.*?</rt>|<rp\b.*?</rp>|<script\b.*?</script>|<style\b.*?</style>", "", body, flags=re.S)
    body = re.sub(r"</?(p|div|h[1-6]|li|blockquote|section|article|tr|td)\b[^>]*>|<br\b[^>]*>", "\n", body)
    body = re.sub(r"<[^>]+>", "", body)
    body = re.sub(r"&[a-z]+;|&#\d+;", "", body)
    return [t.strip(" \t\r") for t in body.split("\n") if t.strip(" \t\r\u3000")]


def japanese(t):
    jp = sum(1 for c in t if 0x3040 <= ord(c) <= 0x30FF or 0x4E00 <= ord(c) <= 0x9FFF or c in "「」『』（）、。")
    return jp >= max(2, len(t) // 4)


def scan(path):
    z = zipfile.ZipFile(path)
    container = z.read("META-INF/container.xml").decode("utf-8", "replace")
    opf_path = re.search(r'full-path="([^"]+)"', container).group(1)
    opf = z.read(opf_path).decode("utf-8", "replace")
    meta = {}
    for tag in ("subject", "type"):
        meta[tag] = [re.sub(r"\s+", " ", v).strip() for v in re.findall(r"<dc:%s[^>]*>([^<]*)</dc:%s>" % (tag, tag), opf)]
    meta["series"] = re.findall(r'property="belongs-to-collection"[^>]*>([^<]*)<', opf) + \
        re.findall(r'name="calibre:series"\s+content="([^"]*)"', opf)
    meta["genre"] = re.findall(r'(?:name|property)="[^"]*genre[^"]*"[^>]*(?:content="([^"]*)"|>([^<]*)<)', opf)
    base = posixpath.dirname(opf_path)
    manifest = {}
    for m in re.finditer(r"<(?:\w+:)?item\b([^>]*)/?>", opf):
        at = dict(re.findall(r'([\w:-]+)="([^"]*)"', m.group(1)))
        if "id" in at and "href" in at:
            manifest[at["id"]] = (posixpath.normpath(posixpath.join(base, at["href"])), at.get("media-type", ""))
    total = opening = 0
    sample_total = sample_opening = None
    for idref in re.findall(r'<(?:\w+:)?itemref\b[^>]*idref="([^"]+)"', opf):
        href, mt = manifest.get(idref, ("", ""))
        if "html" not in mt or href not in z.namelist():
            continue
        for t in paragraphs(z.read(href).decode("utf-8", "replace")):
            if not japanese(t):
                continue
            total += 1
            s = t.lstrip("　 ")
            if s[:1] in OPEN:
                opening += 1
            if total == SAMPLE:
                sample_total, sample_opening = total, opening
    if sample_total is None:
        sample_total, sample_opening = total, opening
    return dict(book=os.path.splitext(os.path.basename(path))[0], paragraphs=total, opening=opening,
                share=round(opening / total, 3) if total else None,
                sample_share=round(sample_opening / sample_total, 3) if sample_total else None, **meta)


files = []
for a in sys.argv[1:]:
    files += sorted(os.path.join(a, f) for f in os.listdir(a) if f.endswith(".epub")) if os.path.isdir(a) else [a]
for f in files:
    try:
        r = scan(f)
    except Exception as e:
        continue
    if r["paragraphs"] >= 20:
        print(json.dumps(r, ensure_ascii=False))
