#!/usr/bin/env python3
"""make_cases.py <dir>: the minimal pages of the JLReq conformance report, one XHTML per case id.

Each case is our own text (the JLReq figures' wording where a figure is cited). They are drawn by reader-cli in both
writing modes (run_cases.sh); the case id is the screenshot name and the test reference in docs/jlreq-conformance.md.
"""
import os, sys

CSS = ("html{font-family:'Noto Serif CJK JP','Noto Sans CJK JP',serif;font-size:20px;line-height:1.75}"
       "body{margin:0}p{margin:0;text-align:justify}.n{color:#888;font-size:11px;line-height:1.4}"
       ".em{text-emphasis-style:sesame}.tcy{text-combine-upright:all}.i0{text-indent:0}.i1{text-indent:1em}")

# id: (title, body html); bodies are chosen so that the case falls at a line's start or end at the page's width
# (each run_cases.sh line gives the width).
CASES = {
    # 3.1.5: the JLReq figure's text, as Aozora books write it (U+3000 in the text, none before 「) and with CSS indent
    "b-aozora": ("3.1.5 括弧 / 青空文库式（正文全角空格，「 前无空格）",
                 "<p>　次のように発音どおりとならない事項もあり注意する。</p>"
                 "<p>『現代仮名遣い』では、助詞の「へ」は「へ」と書くのがルールである。</p>"
                 "<p>「へ」は「へ」と書く。</p>"),
    "b-indent": ("3.1.5 括弧 / CSS text-indent:1em",
                 "<p class='i1'>次のように発音どおりとならない事項もあり注意する。</p>"
                 "<p class='i1'>『現代仮名遣い』では、助詞の「へ」は「へ」と書くのがルールである。</p>"
                 "<p class='i1'>「へ」は「へ」と書く。</p>"),
    # 3.1.4 / 3.1.2 consecutive punctuation
    "p-consec": ("3.1.4 约物连续", "<p>あ、「い」。「う」」、え（お）・か。』」き</p>"),
    # 3.1.6 dividing punctuation followed by text, and ！」
    "p-dividing": ("3.1.6 ！？ 后", "<p>なんだ！そうか。本当？うそだ！」と言った。</p>"),
    # 3.1.1 / 3.1.2 horizontal ，．
    "p-comma-period": ("3.1.1 横排 ，．", "<p>表3，次の例．「あ」，（い）．</p>"),
    # 3.1.9 line-end 。 、 」 in a justified line, and a middle dot at line end
    "p-line-end": ("3.1.9 行末约物", "<p>あいうえおかきくけ。こさしすせそたちつ、てとなにぬねのはひ」ふへほまみむめも・やゆよ</p>"),
    # 3.1.7 / 3.1.8 kinsoku and 3.1.12 push-in vs push-out
    "k-kinsoku": ("3.1.7/3.1.8/3.1.12 禁则与调整", "<p>あいうえおかきくけこ。さしすせそたちつてと「なにぬねのはひふへほ」まみむめもやゆよ</p>"),
    # 3.1.10 / 3.1.11 inseparable and no spacing around brackets in justify
    "j-justify": ("3.1.11 两端对齐不在括弧内外加空", "<p>「あい」うえ『おか』きく、けこ「さし」すせ。そたちつてと</p>"),
    # 3.2.6 Japanese/Latin spacing
    "l-latin": ("3.2.6 和欧间", "<p>これはEPUB 3の例で、W3Cの規格です。全角ＡＢＣと123も。</p>"),
    # 3.2.3 / 3.2.5 tate-chu-yoko
    "t-tcy": ("3.2.5 縦中横", "<p>第12回は3月5日、第123回は2026年。<span class='tcy'>ABC</span>とA。!?も。</p>"),
    # 3.3.5 / 3.3.8: the JLReq figure (の旬に / 後旬又) and one kana per base
    "r-overhang": ("3.3.8 ルビの掛け", "<p>の<ruby>旬<rt>しゅん</rt></ruby>に、後<ruby>旬<rt>しゅん</rt></ruby>又、"
                                     "の<ruby>旬<rt>しゅん</rt></ruby>又、後<ruby>旬<rt>しゅん</rt></ruby>に。</p>"),
    "r-mono": ("3.3.5 モノルビ", "<p><ruby>木<rt>き</rt></ruby>の<ruby>葉<rt>は</rt></ruby>と<ruby>東<rt>ひがし</rt></ruby>と"
                               "<ruby>漢<rt>かん</rt></ruby><ruby>字<rt>じ</rt></ruby>。</p>"),
    "r-group": ("3.3.6 グループルビ", "<p><ruby>紫陽花<rt>あじさい</rt></ruby>と<ruby>山茶花<rt>さざんか</rt></ruby>と"
                                    "<ruby>九十九<rt>つくも</rt></ruby>と<ruby>東京<rt>とうきょう</rt></ruby>。</p>"),
    "r-jukugo": ("3.3.7 熟語ルビ", "<p>その<ruby>漢<rt>かん</rt>字<rt>じ</rt></ruby>と<ruby>東<rt>とう</rt>京<rt>きょう</rt></ruby>"
                                  "と<ruby>承<rt>うけたまわ</rt>る<rt></rt></ruby>。</p>"),
    "r-edge": ("3.3.8 行头行尾ルビ", "<p><ruby>旬<rt>しゅん</rt></ruby>のあいうえおかきくけこさしすせ<ruby>旬<rt>しゅん</rt></ruby>"
                                  "<ruby>東京<rt>とうきょう</rt></ruby>なにぬねのはひふへほまみむめも</p>"),
    "r-adjacent": ("3.3.8 规则2 注：ルビ相邻", "<p>あ<ruby>旬<rt>しゅん</rt></ruby><ruby>旬<rt>しゅん</rt></ruby>い、"
                                            "「<ruby>旬<rt>しゅん</rt></ruby>」と、<ruby>旬<rt>しゅん</rt></ruby>・と。</p>"),
    # 3.3.9 emphasis dots
    "e-dots": ("3.3.9 圏点", "<p>これは<span class='em'>大事なこと、「本当」だ。</span>ABC<span class='em'>abc</span></p>"),
    # 3.5.1 / 3.5.2 indents
    "i-indent": ("3.5.1/3.5.2 字下げ", "<p class='i1'>段落の一行目は一字下げにする。二行目以降は天ツキで続く。</p>"
                                     "<p style='margin-left:2em'>引用は二字下げにする例である。</p>"),
}


def page(title, body):
    return ("<?xml version='1.0' encoding='utf-8'?>\n<html xmlns='http://www.w3.org/1999/xhtml' lang='ja' xml:lang='ja'>"
            "<head><meta charset='utf-8'/><title>%s</title><style>%s</style></head><body>%s</body></html>\n" % (title, CSS, body))


out = sys.argv[1]
os.makedirs(out, exist_ok=True)
for cid, (title, body) in CASES.items():
    open(os.path.join(out, cid + ".xhtml"), "w").write(page(title, body))
print(" ".join(CASES))
