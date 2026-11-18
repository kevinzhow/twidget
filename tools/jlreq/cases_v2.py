#!/usr/bin/env python3
"""cases_v2.py <out dir>: the report-v2 pages. For each case: ours/<id>.xhtml (reader-cli), chrome/<id>.html (Chromium
with the CSS that is meant to do the rule), ref/<id>.html (the reference: Chromium or Vivliostyle with CSS that
implements the rule, or the rule's geometry hand-set with margins), viv/<id>.html (Vivliostyle, where tried).

All pages: 20px Noto Serif CJK JP, line-height 1.75, 14 em lines (280 px), 8 px inset; -h horizontal, -v vertical-rl.
"""
import json, os, sys

BASE = ("html{font-family:'Noto Serif CJK JP',serif;font-size:20px;line-height:1.75}"
        "body{margin:0;padding:8px}p{margin:0;text-align:justify}")
VERT = "html{writing-mode:vertical-rl;-epub-writing-mode:vertical-rl}"

R = lambda b, r, attrs="": "<ruby%s>%s<rt>%s</rt></ruby>" % (attrs, b, r)
SHUN = R("旬", "しゅん")

# Hand-set ruby (Chromium draws it; the margins put it where the rule does): a ruby half an em longer than its base
# overhangs a quarter em on the given sides.
def hs(base, ruby, before=0.0, after=0.0, align=None):
    style = "margin-inline-start:-%gem;margin-inline-end:-%gem" % (before, after) if (before or after) else ""
    if align:
        style += (";" if style else "") + "ruby-align:" + align
    return R(base, ruby, ' style="%s"' % style if style else "")


# id: dict(mode, body, chrome_css, chrome_body (default body), ref: (kind, css, body), ref_note, viv_css)
CASES = {
    # 3.1.5 ①: the JLReq figure's text. Ours/Chrome: as an Aozora book writes it. Ref: the same in CSS (text-indent
    # on every paragraph, no U+3000) with text-spacing-trim: trim-start, CSS Text 4 §8.5.3's scheme 1.
    "b-aozora": dict(mode="v",
        body="<p>　次のように発音どおりとならない事項もあり注意する。</p><p>『現代仮名遣い』では、助詞の「へ」は「へ」と書くのがルールである。</p><p>「へ」は「へ」と書く。</p>",
        chrome_css="",  # text-spacing-trim: normal (initial)
        ref=("chrome", "p{text-indent:1em;text-spacing-trim:trim-start}",
             "<p>次のように発音どおりとならない事項もあり注意する。</p><p>『現代仮名遣い』では、助詞の「へ」は「へ」と書くのがルールである。</p><p>「へ」は「へ」と書く。</p>"),
        ref_note="Chromium，CSS Text 4 §8.5.3 方案 1：每段 text-indent:1em（不用 U+3000）+ text-spacing-trim: trim-start",
        viv_css="p{text-spacing-trim:trim-start}"),
    "p-comma-period": dict(mode="h", body="<p>表3，次の例．「あ」，（い）．</p>",
        chrome_css="", ref=("chrome", "p{text-spacing-trim:trim-both}", None),
        ref_note="Chromium text-spacing-trim: trim-both（，．按句读点压缩）", viv_css=""),
    "p-kanji-num": dict(mode="v", body="<p>二、三日おきに三・五倍の量を飲む。</p>",
        chrome_css="",
        ref=("chrome", ".t{margin-inline-end:-0.5em}.d{margin-inline:-0.25em}",
             "<p>二<span class=t>、</span>三日おきに三<span class=d>・</span>五倍の量を飲む。</p>"),
        ref_note="按规则手排：位取り读点后紧排、小数点中点前后紧排（margin），Chromium 绘制"),
    "p-dividing": dict(mode="h", body="<p>なんだ！そうか。本当？うそだ！」と言った。</p>",
        chrome_css="",
        ref=("chrome", "", "<p>なんだ！　そうか。本当？　うそだ！」と言った。</p>"),
        ref_note="按规则手排：句末 ！？ 后补全角空（U+3000），接 」 时不补；Chromium 绘制"),
    "p-dividing-end": dict(mode="h", body="<p>あいうえおかきくけこさしす！　そたちつてと</p>",
        chrome_css="", ref=("chrome", "", "<p>あいうえおかきくけこさしす！<br>そたちつてと</p>"),
        ref_note="按规则手排：行末 ！ 后的空去掉，下行天ツキ"),
    # 12 kana and 50 fill 13.1 ems: ％ would start the next line; 13 kana and ¥ 13.6: 100 would.
    "x-abbrev": dict(mode="h", body="<p>あいうえおかきくけこさし50％の、</p><p>あいうえおかきくけこさしす¥100だ。</p>",
        chrome_css="p{line-break:strict}", ref=("chrome", "p{line-break:strict}", None),
        ref_note="Chromium line-break: strict（UAX #14 PR/PO 类：¥ 不在行末、％ 不在行首）"),
    "r-jukugo-break": dict(mode="h",
        body="<p>あいうえおかきくけこさしす<ruby>漢<rt>かん</rt>字<rt>じ</rt></ruby>と<ruby>東<rt>とう</rt>京<rt>きょう</rt></ruby>。</p>",
        chrome_css="", ref=("chrome", "", None), ref_note="Chromium（熟語/モノルビ可在亲字间断行）"),
    "s-sup": dict(mode="h", body="<p>水はH<sub>2</sub>O、面積は10m<sup>2</sup>。</p><p>あいうえおかきくけこさしす数<sup>2</sup>く</p>",
        chrome_css="", ref=("chrome", "sub,sup{font-size:60%;line-height:0}.nb{white-space:nowrap}",
                            "<p>水はH<sub>2</sub>O、面積は10m<sup>2</sup>。</p><p>あいうえおかきくけこさしす<span class=nb>数<sup>2</sup></span>く</p>"),
        ref_note="按规则：添え字约 60%，与亲字不分开（数² 一起换行）；Chromium 绘制"),
    "j-adjust": dict(mode="h", body="<p>あいう「えお」かきくけこさし。すせそたちつてと</p>",
        chrome_css="p{text-justify:inter-character}",
        ref=("chrome", ".o{margin-left:-0.25em}.c{margin-right:-0.25em}.e{margin-right:-0.5em}p{white-space:nowrap}",
             "<p>あいう<span class=o>「</span>えお<span class=c>」</span>かきくけこさし<span class=e>。</span><br>すせそたちつてと</p>"),
        ref_note="按规则手排：追込み——行末 。 去后二分，行中 「 前、」 后二分各压到四分（3.8.3 ⑤），Chromium 绘制"),
    # 13 characters and 「 that may not end the line: 追出し, the line widened by an em, but not next to 、
    # (3.1.11 ②-2): ten gaps of a tenth of an em.
    "j-expand": dict(mode="h", body="<p>あいう、えおかきくけこさし「すせそ</p>",
        chrome_css="p{text-justify:inter-character}",
        ref=("chrome", "p{white-space:nowrap}.g{margin-inline-end:0.1em}",
             "<p><span class=g>あ</span><span class=g>い</span>う、<span class=g>え</span><span class=g>お</span><span class=g>か</span><span class=g>き</span><span class=g>く</span><span class=g>け</span><span class=g>こ</span><span class=g>さ</span>し<br>「すせそ</p>"),
        ref_note="按规则手排：追出し后多出的 1 字只加在假名汉字之间（10 处各 0.1 字），、 前后不加（3.1.11 ②），Chromium 绘制"),
    "l-latin": dict(mode="h", body="<p>これはEPUB 3の例で、W3Cの規格です。</p>",
        chrome_css="p{text-autospace:normal}",
        ref=("chrome", ".q{margin:0 0.25em}",
             "<p>これは<span class=q>EPUB 3</span>の例で、<span class=q style=\"margin-left:0\">W3C</span>の規格です。</p>"),
        ref_note="按规则手排：和欧间四分（行首、读点后不加），Chromium 绘制", viv_css="p{text-autospace:normal}"),
    # A justified line of about 12.95 ems (「せ pushed to the next line): about an em to spare and two word spaces.
    # JLReq 3.8.4 a gives the spaces up to half an em each first, so nearly all of it.
    "l-justify": dict(mode="h", body="<p>あいうえおかきく Web さしす「せそ</p>",
        chrome_css="", ref=("chrome", "p{text-justify:inter-word}", None),
        ref_note="Chromium text-justify: inter-word（扩展先给欧文间隔；此行余量约 1 字，正好在两处各二分之内）"),
    "r-under": dict(mode="h", body="<p>これは<ruby style=\"ruby-position:under\">漢字<rt>かんじ</rt></ruby>の例。</p>",
        chrome_css="", ref=("chrome", "", None), ref_note="Chromium ruby-position: under"),
    "r-overhang": dict(mode="h",
        body="<p>の" + SHUN + "に、後" + SHUN + "又、の" + SHUN + "又、後" + SHUN + "に。</p>",
        chrome_css="", ref=("chrome", "ruby{ruby-overhang:none}", "<p>の" + hs("旬", "しゅん", 0.25, 0.25) + "に、後" + SHUN + "又、の" +
                            hs("旬", "しゅん", 0.25, 0) + "又、後" + hs("旬", "しゅん", 0, 0.5, "start") + "に。</p>"),
        ref_note="按规则手排：ルビ掛假名（每侧 1/4 字 ≤ 一个ルビ字），不掛汉字（margin），Chromium 绘制"),
    "r-adjacent": dict(mode="h", body="<p>あ" + SHUN + SHUN + "い、「" + SHUN + "」と、……" + SHUN + "・と。</p>",
        chrome_css="",
        ref=("chrome", "ruby{ruby-overhang:none}", "<p>あ" + hs("旬", "しゅん", 0.25, 0) + hs("旬", "しゅん", 0, 0.25) + "い、「" +
             hs("旬", "しゅん", 0, 0.25) + "」と、……" + hs("旬", "しゅん", 0.25, 0.25) + "・と。</p>"),
        ref_note="按规则手排：相邻ルビ之间不掛（规则 2 注）；掛 」（规则 6）、……（规则 4）、・（规则 5）各 1/4 字，不掛「（规则 6 注），Chromium 绘制"),
    "r-edge": dict(mode="v",
        body="<p>" + SHUN + "のあいうえおかきくけこさしすせそたちつてとなにぬねの" + SHUN + "はひふ</p>",
        chrome_css="",
        ref=("chrome", "ruby{ruby-overhang:none}p{white-space:nowrap}.mr{position:relative}"
             ".mr>span{position:absolute;font-size:50%;line-height:1;white-space:nowrap;inset-inline-end:0;inset-block-start:-1.1em}",
             "<p>" + hs("旬", "しゅん", 0, 0.5, "start") + "のあいうえおかきくけこさし<br>すせそたちつてとなにぬねの" +
             "<span class=mr>旬<span>しゅん</span></span><br>はひふ</p>"),
        ref_note="按规则手排：行头亲字与ルビ同贴行头、掛向后字；行尾同贴行尾、掛向前字，各一个ルビ字（3.3.8 a/b），Chromium 绘制"),
    "r-group-edge": dict(mode="h",
        body="<p>" + R("東京", "とうきょうと") + "のあいうえおかきくけこさ</p>",
        chrome_css="",
        ref=("chrome", "ruby{ruby-overhang:none}", "<p><ruby style=\"ruby-align:start\"><rb style=\"letter-spacing:10px\">東京</rb><rt>とうきょうと</rt></ruby>"
             "のあいうえおかきくけこさ</p>"),
        ref_note="按规则手排：行头グループルビ，ルビ顶对第一个亲字，亲字间与末端空 1:1（3.3.8 c），Chromium 绘制"),
    "r-jukugo": dict(mode="h",
        body="<p>その<ruby>東<rt>とう</rt>京<rt>きょう</rt></ruby>と<ruby>紫<rt>あじ</rt>陽<rt>さ</rt>花<rt>い</rt></ruby>と<ruby>承<rt>うけたまわ</rt></ruby>る。</p>",
        chrome_css="",
        ref=("chrome", "ruby{ruby-overhang:none}", "<p>その" + R("東京", "とうきょう") + "と<ruby>紫<rt>あじ</rt>陽<rt>さ</rt>花<rt>い</rt></ruby>と" +
             hs("承", "うけたまわ", 0.5, 0.5) + "る。</p>"),
        ref_note="按规则：熟語ルビ某字 ≥3 假名时按グループルビ（東京）；承 的ルビ掛前后假名各一个ルビ字；Chromium 绘制"),
    "r-short": dict(mode="h", body="<p>この" + R("東京都庁", "とう") + "の例。</p>",
        chrome_css="",
        ref=("chrome", "rt.c{ruby-align:space-between;padding:0 5px}", "<p>この<ruby>東京都庁<rt class=c>とう</rt></ruby>の例。</p>"),
        ref_note="按规则手排：短ルビ两端空不超过一个ルビ字（3.3.6 注），Chromium 绘制"),
    "a-even": dict(mode="h", body="<p style=\"text-align:justify;text-align-last:justify\">古池や蛙飛びこむ水の音</p>",
        chrome_css="", ref=("chrome", "", None), ref_note="Chromium text-align-last: justify（均等揃え）"),
    "w-short": dict(mode="h", body="<p>あいうえおかきくけこさしすせそたちつてとなにぬねのはひふほ</p>",
        chrome_css="p{text-wrap:pretty}", ref=("chrome", "p{text-wrap:pretty}", None),
        ref_note="Chromium text-wrap: pretty（避免段末行过短）"),
}


def page(css, body, vertical, xhtml):
    head = ("<?xml version='1.0' encoding='utf-8'?>\n<html xmlns='http://www.w3.org/1999/xhtml' lang='ja' xml:lang='ja'>"
            if xhtml else "<!doctype html>\n<html lang='ja'>")
    return "%s<head><meta charset='utf-8'/><style>%s%s%s</style></head><body>%s</body></html>\n" % (
        head, BASE, VERT if vertical else "", css, body)


def main():
    out = sys.argv[1]
    for d in ("ours", "chrome", "ref", "viv"):
        os.makedirs(os.path.join(out, d), exist_ok=True)
    meta = {}
    for cid, c in CASES.items():
        v = c["mode"] == "v"
        # reader-cli insets the page itself: no body padding
        open(os.path.join(out, "ours", cid + ".xhtml"), "w").write(page("body{padding:0}", c["body"].replace("<br>", "<br/>"), v, True))
        open(os.path.join(out, "chrome", cid + ".html"), "w").write(page(c.get("chrome_css", ""), c.get("chrome_body") or c["body"], v, False))
        kind, css, body = c["ref"]
        open(os.path.join(out, "ref", cid + ".html"), "w").write(page(css, body or c["body"], v, False))
        if "viv_css" in c:
            open(os.path.join(out, "viv", cid + ".html"), "w").write(page(c["viv_css"], c["body"], v, False))
        meta[cid] = dict(mode=c["mode"], chrome_css=c.get("chrome_css", ""), ref_kind=kind, ref_css=css,
                         ref_note=c["ref_note"], viv="viv_css" in c, viv_css=c.get("viv_css"))
    json.dump(meta, open(os.path.join(out, "meta.json"), "w"), ensure_ascii=False, indent=1)
    print(" ".join(CASES))


main()
