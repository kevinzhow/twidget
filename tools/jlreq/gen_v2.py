#!/usr/bin/env python3
"""gen_v2.py <html dir>: report v2. For every missing/partial row and every batch-1 and batch-2 row of tools/rows.py:
the JLReq figure, a reference rendering of the case (with how it was made), Chromium's rendering with the CSS meant for
the rule and a verdict, and ours (before the row's batch, and now). Crops from out/ (cases_v2.py, render_chrome.sh,
reader-cli): ours-png/base-* before batch 1, b1-* after it (before batch 2), b2-* after batch 2, b3-* after batch 3,
pre4-* reader main 4984634 + twidget main 15ba438 (before batch 4), b4-* after batch 4.
"""
import html, json, os, shutil, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
from rows import ROWS

OUT = sys.argv[1]
IMG = os.path.join(OUT, "img")
os.makedirs(IMG, exist_ok=True)
META = json.load(open(os.path.join(HERE, "out", "meta.json")))
CHROME = "Chromium 156.0.8078.4（Chrome for Testing，headless shell）"
VIV = "Vivliostyle Viewer 2.46.0（在同一 Chromium 中）"
JL = "https://www.w3.org/TR/jlreq/"

# rule id -> (case, [figure file, number], chrome verdict, chrome note, viv verdict or None)
# verdicts: yes / partly / no
SPEC = {
    "3.1.1-2": ("p-comma-period", [("img2_1", 57)], "yes", "默认 text-spacing-trim: normal 已把 ，． 按句读点压缩（「 在 ． 后减半）。", "yes"),
    "3.1.3": ("p-kanji-num", [("img2_11", 66), ("img2_10", 67), ("img2_12", 68)], "no", "无对应 CSS；、・ 照常留空。", None),
    "3.1.5-1": ("b-aozora", [("img2_15", 71)], "no",
                "默认 normal（trim-start 相同）：段首『 与折行「 一样贴顶，不分段首。CSS 只能写成每段 text-indent:1em + trim-start（见参考）。", "partly"),
    "3.1.6-1": ("p-dividing", [("img2_17", 73), ("img2_18", 74)], "no", "无对应 CSS；？！ 后不补空。", None),
    "3.1.6-2": ("p-dividing-end", [("img2_20", 75)], "yes", "行末 U+3000 悬挂，下行天ツキ。", None),
    "3.1.10-4": ("x-abbrev", [("img2_26", 82), ("img2_27", 83)], "yes", "line-break: strict：¥100、50％ 不分开。", None),
    "3.1.10-7": ("r-jukugo-break", [("img2_29", 85), ("img2_30", 86)], "yes", "一个 ruby 内的多对可在亲字间断行（漢|字）。", None),
    "3.1.10-8": ("r-jukugo-break", [("img2_30", 86)], "yes", "同上。", None),
    "3.1.10-9": ("s-sup", [("img2_31", 87)], "no", "数 与 ² 之间断行。", None),
    "3.1.11-2": ("j-adjust", [("img2_33", 89)], "no", "text-justify: inter-character：空加在所有字间，包括括弧前后。", None),
    "3.1.11-3": ("j-expand", [("img2_33", 89)], "no", "空加在所有字间，包括 。 前后。", None),
    "3.1.12-2": ("j-adjust", [("img2_33", 89)], "no", "只会追出し（し。推到下一行），不追込み。", None),
    "3.2.6-2": ("l-justify", [("img2_2_10", 102)], "partly", "默认（日文 inter-character）空加在所有字间；text-justify: inter-word 时只加在欧文间隔（见参考）。", None),
    "3.2.6-4": ("l-latin", [("img2_2_10", 102)], "partly", "text-autospace: normal 加 1/8 字（CSS 规定），JLReq 要四分。", "partly"),
    "3.3.4": ("r-under", [("img2_3_12", 117)], "yes", "ruby-position: under。", None),
    "3.3.5-2": ("r-overhang", [("img2_3_16", 121), ("img2_3_30", 135)], "partly",
                "ruby-overhang: auto（默认）会掛假名，但也掛汉字（後旬又 紧排，违反规则 1），行头不掛后字。", None),
    "3.3.6-2": ("r-short", [("img2_3_21", 126)], "no", "ruby-align: space-around，两端空不设上限。", None),
    "3.3.7-2": ("r-jukugo-room", [("img2_3_24", 129), ("img2_3_25", 130)], "no", "每对单独排，胡 与 瓜 之间空开，きゅう 不掛同词的 瓜。", None),
    "3.3.7-3": ("r-jukugo-break", [("img2_3_28", 133)], "yes", "可在亲字间断行。", None),
    "3.3.8-1": ("r-overhang", [("img2_3_30", 135), ("img2_3_31", 136)], "no", "也掛汉字：後旬又 紧排。", None),
    "3.3.8-2": ("r-overhang", [("img2_3_30", 135), ("img2_3_31", 136)], "yes", "掛假名（の旬に 紧排）。", None),
    "3.3.8-2n": ("r-adjacent", [("img2_3_29_2", 137), ("img2_3_29_3", 138)], "yes", "两组ルビ之间不掛（空开）。", None),
    "3.3.8-3": ("r-punct", [("img2_3_32", 139), ("img2_3_33", 140)], "no", "」、 后和「 前的二分空不掛，亲字空开（と、旬 因此放不下，旬 到了下一行）。", None),
    "3.3.8-4": ("r-adjacent", [("img2_3_32", 139)], "yes", "掛 …… 和 ・。", None),
    "3.3.8-6": ("r-adjacent", [("img2_3_33", 140)], "yes", "掛「与 」的字面（JLReq 正文允许；注里有不掛「」的做法）。", None),
    "3.3.8-e0": ("r-edge", [("img2_3_34", 141)], "yes", "不伸出行。", None),
    "3.3.8-ea": ("r-edge", [("img2_3_34", 141), ("img2_3_35", 142)], "partly",
                 "行头亲字与ルビ同贴行头，但不掛后面的假名（留 1/2 字空）；行尾放不下，ルビ推到下一行。", None),
    "3.3.8-ec": ("r-group-edge", [("img2_3_36", 143)], "no", "行头仍 1:2:1，第一个亲字不贴行头。", None),
    "3.3.8-ef": ("r-jukugo", [("img2_3_36", 143)], "no", "无熟語ルビ处理。", None),
    "3.5.3-4": ("a-even", [("img2_5_9", 158)], "yes", "text-align-last: justify。", None),
    "3.5.4-1": ("w-short", [], "yes", "text-wrap: pretty 避免了一字的段末行（默认 wrap 不避免）。", None),
    "3.7.1": ("s-sup", [("img2_7_1", 170)], "no", "sub/sup 默认 font-size: smaller（约 83%），JLReq 约 60%。", None),
    "3.8.2-1": ("j-adjust", [("img2_33", 89)], "no", "只扩展，不压缩。", None),
    "3.8.4-a": ("l-justify", [], "partly", "默认否；text-justify: inter-word 时是（见参考）。", None),
    "3.8.4-b": ("l-latin", [("img2_2_10", 102)], "partly", "有 1/8 字的和欧间（text-autospace），但不参与扩展的顺序。", None),
    "3.8.4-c": ("j-expand", [("img2_33", 89)], "no", "空加在 。 前后，且无上限。", None),
    "3.8.2-2": ("j-expand", [("img2_33", 89)], "yes", "压不进时追出し。", None),
    "3.8.4-d": ("j-expand", [("img2_33", 89)], "partly", "均分到所有字间，包括 。 前后。", None),
}
VERDICT = {"yes": "符合", "partly": "部分", "no": "不符合"}
ST = {"done": "已实现", "partial": "部分", "missing": "缺失", "na": "不适用"}


def copy(src, name):
    if os.path.exists(src):
        shutil.copy(src, os.path.join(IMG, name))
        return "img/" + name
    return None


def esc(s):
    return html.escape(str(s))


rows = []
for r in ROWS:
    b1 = dict(r, **r.get("after", {}))  # after batch 1 (before batch 2)
    b2 = dict(b1, **r.get("after2", {}))  # after batch 2 (before batch 3)
    b3 = dict(b2, **r.get("after3", {}))  # after batch 3 (before batch 4)
    now = dict(b3, **r.get("after4", {}))
    if r.get("after3"):
        r = dict(r, batch=3)
    if r.get("after4"):
        r = dict(r, batch=4)
    if r["id"] not in SPEC:
        continue
    rows.append((r, b3 if r.get("batch") == 4 else b1 if r.get("batch") == 2 else b2, now, SPEC[r["id"]]))

used_cases = sorted({s[0] for _, _, _, s in rows})
for c in used_cases:
    copy(os.path.join(HERE, "out", "ref", c + ".png"), "ref-%s.png" % c)
    copy(os.path.join(HERE, "out", "chrome", c + ".png"), "chrome-%s.png" % c)
    copy(os.path.join(HERE, "out", "ours-png", "base-%s-p1.png" % c), "ours-base-%s.png" % c)
    copy(os.path.join(HERE, "out", "ours-png", "b1-%s-p1.png" % c), "ours-b1-%s.png" % c)
    copy(os.path.join(HERE, "out", "ours-png", "b2-%s-p1.png" % c), "ours-b2-%s.png" % c)
    copy(os.path.join(HERE, "out", "ours-png", "b3-%s-p1.png" % c), "ours-b3-%s.png" % c)
    copy(os.path.join(HERE, "out", "ours-png", "pre4-%s-p1.png" % c), "ours-pre4-%s.png" % c)
    copy(os.path.join(HERE, "out", "ours-png", "b4-%s-p1.png" % c), "ours-b4-%s.png" % c)
    copy(os.path.join(HERE, "out", "viv-png", c + ".png"), "viv-%s.png" % c)
for _, _, _, s in rows:
    for f, _ in s[1]:
        copy(os.path.join(HERE, "jlreq-fig", f + ".png"), "jlreq-%s.png" % f)

counts = {}
for _, _, _, s in rows:
    counts[s[2]] = counts.get(s[2], 0) + 1
ours_after = {}
for _, _, now, _ in rows:
    ours_after[now["status"]] = ours_after.get(now["status"], 0) + 1

CSS = """
:root{--bg:#fbfaf7;--fg:#1d1d1f;--mut:#6b6b70;--line:#e3e1da;--card:#fff;--yes:#2f7d4a;--partly:#b7791f;--no:#c0392b;--b1:#2b6cb0}
@media (prefers-color-scheme:dark){:root:not([data-theme=light]){--bg:#17171a;--fg:#ececf0;--mut:#a0a0a8;--line:#33333a;--card:#202024;--yes:#5fbf7f;--partly:#e0a84a;--no:#ef6b5b;--b1:#6aa7e8}}
:root[data-theme=dark]{--bg:#17171a;--fg:#ececf0;--mut:#a0a0a8;--line:#33333a;--card:#202024;--yes:#5fbf7f;--partly:#e0a84a;--no:#ef6b5b;--b1:#6aa7e8}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.6 system-ui,"Noto Sans CJK SC","Noto Sans CJK JP",sans-serif}
main{max-width:1240px;margin:0 auto;padding:24px 16px 64px}h1{font-size:26px;margin:0 0 4px}h2{font-size:20px;margin:32px 0 10px;border-bottom:1px solid var(--line);padding-bottom:4px}
.mut{color:var(--mut);font-size:13px}.tag{display:inline-block;font-size:12px;padding:0 7px;border-radius:9px;border:1px solid currentColor;white-space:nowrap}
.yes,.done{color:var(--yes)}.partly,.partial{color:var(--partly)}.no,.missing{color:var(--no)}.b1{color:var(--b1);font-weight:600;font-size:12px}
.tw{overflow-x:auto;border:1px solid var(--line);border-radius:10px;background:var(--card)}table{border-collapse:collapse;width:100%;font-size:13px}
th,td{text-align:left;vertical-align:top;padding:6px 8px;border-bottom:1px solid var(--line)}
.rule{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px 14px;margin:14px 0}
.rule h3{margin:0 0 4px;font-size:16px}.req{margin:4px 0 10px}
.cols{display:flex;gap:12px;flex-wrap:wrap;align-items:flex-start}.cols figure{margin:0;min-width:0;max-width:100%;flex:0 1 auto}
.cols img{display:block;max-width:100%;height:auto;border:1px solid var(--line);border-radius:6px;background:#fff}
.cols figcaption{font-size:12px;color:var(--mut);max-width:300px;margin-top:4px}.cols figcaption b{color:var(--fg)}
.fig img{max-height:240px;width:auto}
"""


def fig(src, width, cap):
    return '<figure><img src="%s" width="%d" alt=""><figcaption>%s</figcaption></figure>' % (src, width, cap)


p = ['<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">',
     "<title>JLReq 对照报告 v2</title><style>%s</style></head><body><main>" % CSS,
     "<h1>JLReq 对照报告 v2：参考排版、Chromium、我们</h1>",
     '<p class="mut">2026-11-20 · 任务 #oyomi:#38 · %s · %s · 我们：阅读器 alice/jlreq-b4 + twidget alice/jlreq-b4-comma-period 32b1239（第 4 批之后）、阅读器 main 4984634 + twidget main 15ba438（第 4 批之前）、alice/jlreq-b3-side（第 3 批之后）、a1d529d（第 2 批之后）、3e0f21a（第 1 批之后）与 9e7730b（第 1 批之前）经 reader-cli</p>' % (CHROME, VIV)]
p.append("<p>v1 只有“我们”的截图，没有正确的参考。v2 为 %d 条（全部缺失/部分条目和第 1、2 批）各配四样东西："
         "JLReq 原图（W3C）；同一测试句按规则排对的参考排版（能用实现了该规则的渲染器就用它，否则在 Chromium 中按规则的几何量手排，每条注明做法）；"
         "Chromium 用该规则对应的 CSS 渲染并给出结论；我们的渲染（该条所在批次之前，与现在）。</p>" % len(rows))
p.append("<p><b>第 2 批</b>（标“第2批”）：行的调整处理——先压缩再推行（3.1.12、3.8.2/3.8.3），扩展时不在括弧、句读点旁加空、先扩欧文间隔与和欧之间（3.1.11、3.8.4）；"
         "ルビ掛句读点和括弧（3.3.8 规则 3–6）。改动的例子：j-expand 的 、 换成 。（按 JLReq 正文行中括弧与读点的二分可压到ベタ，原句 、「 合起来正好压出一字，就不再是追出し的例子了）；"
         "r-adjacent 的参考按规则 6 正文掛「（之前按注不掛）；新增 r-punct（规则 3）。</p>")
p.append("<p><b>第 3 批</b>（标“第3批”）：熟語ルビ可在亲字之间断行，长ルビ掛同词亲字留下的空（3.1.10 ⑦⑧、3.3.7、3.3.8 e–h）；和欧间四分（3.2.6 ④、3.8.4 b）；"
         "グループルビ行头行尾 1:1（3.3.8 c/d）；句末 ？！ 后补全角空（3.1.6 ①）；汉数字中的 、・ 紧排（3.1.3）。</p>")
p.append("<p><b>第 4 批</b>（标“第4批”）：亲字与上下标之间不断行（3.1.10 ⑨）；上下标约亲字 60%%、与亲字紧排、不撑开行距（3.7.1）；"
         "短グループルビ两端空不超过一个ルビ字（3.3.6 注）；横排日文中 ，． 按 、。 处理（3.1.1 ②）。"
         "核对 s-sup 时发现第 3 批的和欧间四分也加在了上标数字两侧（数 ² く），已先写测试再修好。</p>")
p.append("<p><b>Chromium 结论</b>：符合 %d、部分 %d、不符合 %d。<b>我们（第 4 批之后）</b>：%s。</p>" % (
    counts.get("yes", 0), counts.get("partly", 0), counts.get("no", 0),
    "、".join("%s %d" % (ST[k], v) for k, v in sorted(ours_after.items(), key=lambda kv: ["done", "partial", "missing", "na"].index(kv[0])))))
p.append("<p><b>v2 核对中改正的状态</b>（对照 Chromium 和参考后发现 v1 记错）：3.1.6 ②（行末 ？！ 的空）与 3.1.10 ④⑤（¥、％）我们本来就符合，改为已实现；"
         "3.1.10 ⑨（添え字不分割）我们会断开，由“部分”改为“缺失”。另外 v2 发现第 1 批行尾规则（3.3.8 b）有一处遗漏：只有按整段超出量掛前字才放得下的ルビ被推到下一行；已补测试并修复（twidget 24996b4）。全量视觉回归又发现一处：拉丁文ルビ（hefty-water 的 2H₂/hydrogen）在行头也被贴到ルビ起点；JLReq 的ルビ规则只用于日文，已限定（twidget 8319084，先写测试）。</p>")
p.append('<p class="mut">做法说明：所有页面 20px Noto Serif CJK JP、行高 1.75、每行 14 字（280px）、内边距 8px，2 倍像素。'
         "“手排”= 在 Chromium 中用 margin / 绝对定位把字放到规则给出的位置（ルビ类先设 ruby-overhang: none，避免与 Chromium 自己的掛叠加）。"
         "JLReq 原图：Copyright © 2020 W3C® (MIT, ERCIM, Keio, Beihang)，取自 <a href=\"%s\">Requirements for Japanese Text Layout</a>（W3C Working Group Note, 11 August 2020），按 W3C Document License 引用。</p>" % JL)

# summary table
p.append("<h2>总表</h2><div class=tw><table><thead><tr><th>条目</th><th>参考的做法</th><th>Chromium</th><th>我们 前 → 后</th></tr></thead><tbody>")
def mark(r):
    return ' <span class=b1>第%d批</span>' % r["batch"] if r.get("batch") in (1, 2, 3, 4) else ""


for r, b1, now, s in rows:
    m = META[s[0]]
    p.append('<tr><td><a href="#r-%s"><b>%s</b> %s</a>%s</td><td>%s</td><td><span class="tag %s">%s</span></td><td><span class="tag %s">%s</span> → <span class="tag %s">%s</span></td></tr>' % (
        esc(r["id"]), esc(r["sec"]), esc(r["title"]), mark(r), esc(m["ref_note"]),
        s[2], VERDICT[s[2]], (b1 if r.get("batch") in (2, 4) else r)["status"], ST[(b1 if r.get("batch") in (2, 4) else r)["status"]],
        now["status"], ST[now["status"]]))
p.append("</tbody></table></div>")

p.append("<h2>逐条</h2>")
for r, b1, now, s in rows:
    case, figs, verdict, cnote, viv = s
    m = META[case]
    w = 296 if m["mode"] == "h" else 236
    cells = []
    for f, n in figs:
        cells.append('<figure class=fig><img src="img/jlreq-%s.png" alt="JLReq Figure %d"><figcaption><b>JLReq 原图</b> Figure %d · '
                     '<a href="%s">%s</a> · © W3C</figcaption></figure>' % (f, n, n, JL, r["sec"].split(" ")[0]))
    cells.append(fig("img/ref-%s.png" % case, w, "<b>参考</b>：" + esc(m["ref_note"]) +
                     ("<br>CSS: <code>%s</code>" % esc(m["ref_css"]) if m["ref_css"] else "")))
    cells.append(fig("img/chrome-%s.png" % case, w, '<b>Chromium</b> <span class="tag %s">%s</span> %s%s' % (
        verdict, VERDICT[verdict], esc(cnote), ("<br>CSS: <code>%s</code>" % esc(m["chrome_css"])) if m["chrome_css"] else "<br>CSS: 默认")))
    if viv and os.path.exists(os.path.join(IMG, "viv-%s.png" % case)):
        cells.append(fig("img/viv-%s.png" % case, w, '<b>Vivliostyle</b> <span class="tag %s">%s</span>' % (viv, VERDICT[viv])))
    if r.get("batch") == 4:
        cells.append(fig("img/ours-pre4-%s.png" % case, w, '<b>我们 第4批前</b> <span class="tag %s">%s</span>' % (b1["status"], ST[b1["status"]])))
    elif r.get("batch") == 3:
        cells.append(fig("img/ours-b2-%s.png" % case, w, '<b>我们 第3批前</b> <span class="tag %s">%s</span>' % (b1["status"], ST[b1["status"]])))
    elif r.get("batch") == 2:
        cells.append(fig("img/ours-b1-%s.png" % case, w, '<b>我们 第2批前</b> <span class="tag %s">%s</span>' % (b1["status"], ST[b1["status"]])))
    elif r.get("batch") == 1 or r["status"] != now["status"]:
        cells.append(fig("img/ours-base-%s.png" % case, w, '<b>我们 第1批前</b> <span class="tag %s">%s</span>' % (r["status"], ST[r["status"]])))
    cells.append(fig("img/ours-b4-%s.png" % case, w, '<b>我们 现在</b> <span class="tag %s">%s</span> %s' % (now["status"], ST[now["status"]], esc(now["ours"]))))
    p.append('<div class=rule id="r-%s"><h3>%s %s%s</h3><div class="req">%s <a class=mut href="%s">JLReq %s</a></div><div class=cols>%s</div></div>' % (
        esc(r["id"]), esc(r["sec"]), esc(r["title"]), mark(r), esc(r["req"]), JL,
        esc(r["sec"]), "".join(cells)))
p.append("</main></body></html>")
open(os.path.join(OUT, "index.html"), "w").write("\n".join(p))
print(len(rows), "rows", counts, ours_after)
