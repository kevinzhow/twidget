#!/usr/bin/env python3
"""gen_report.py <public-scan.json> <private-totals.json> <shots dir> <md out> <html dir>: the conformance table as
docs/jlreq-conformance.md (twidget) and as the HTML report (index.html + img/)."""
import html, json, os, shutil, sys

sys.path.insert(0, os.path.dirname(__file__))
from rows import ROWS

pub = json.load(open(sys.argv[1]))
priv = json.load(open(sys.argv[2]))
shots, md_out, html_dir = sys.argv[3], sys.argv[4], sys.argv[5]

ST = {"done": "已实现", "partial": "部分", "missing": "缺失", "na": "不适用"}
ORDER = ["done", "partial", "missing", "na"]


def freq(row, src):
    return sum(src["total"].get(k, 0) for k in row.get("freq", []))


def books(row, src):
    return max([src["books_with"].get(k, 0) for k in row.get("freq", [])] or [0])


def freq_text(row):
    if not row.get("freq"):
        return "—"
    return "%d（%d 本）/ K8 版权书 %d" % (freq(row, pub), books(row, pub), freq(row, priv))


counts = {s: sum(1 for r in ROWS if r["status"] == s) for s in ORDER}
total = len(ROWS)

# batch 2: missing/partial rows outside batch 1, grouped by the same frequency keys, ranked by public occurrences
groups = {}
for r in ROWS:
    if r["status"] in ("missing", "partial") and r.get("batch") != 1:
        key = tuple(r.get("freq", [])) or ("none", r["id"])
        groups.setdefault(key, []).append(r)
ranked = sorted(groups.values(), key=lambda g: (-freq(g[0], pub), -freq(g[0], priv), g[0]["id"]))

SUMMARY = (
    "本表逐条对照 W3C《日本語組版処理の要件》(JLReq, 2020-08-11 WG Note) 第 3 章与新阅读器（twidget litehtml + oyomi 阅读器）。"
    "共 %d 条：已实现 %d、部分 %d、缺失 %d、不适用 %d。"
    "缺口集中在三处：(1) 行头始め括弧不分段首与折行（3.1.5）；(2) ルビ完全不掛（3.3.5、3.3.8），比亲字长的ルビ一律把亲字空开；"
    "(3) 两端对齐只会扩展、而且在括弧和句读点旁也加空，没有 JLReq 3.8 的先压缩（追込み）和优先顺序。"
    "此外缺少和欧间四分（3.2.6）、横排 ，． 的约物处理（3.1.1）和熟語ルビ。"
    "第 1 批做 3.1.5 ① 与ルビ掛け（规则 1、2 与行头行尾）；其余按公共语料出现次数排在“第 2 批候选”。" % (
        total, counts["done"], counts["partial"], counts["missing"], counts["na"]))

METHOD = (
    "代码与测试的位置以 twidget 9e7730b（main e6790b9 + #22）、阅读器 83076d2（#31）为准。截图是 `reader-cli` 画的我们自己的小页面"
    "（`tools/jlreq/make_cases.py`，20px Noto Serif CJK JP，14 字一行，横排 -h、竖排 -v）。频次由 `tools/jlreq/corpus_scan.py` 统计："
    "公共语料（w3c、idpf，及公有领域的 Aozora/Gutenberg 书：kumo、neko、rashomon、g1982、g31617、g34013）中的出现次数与书数；"
    "“K8 版权书”是 K8 上 4 本版权书只取总数（文字不离开 K8）。行头行尾类的频次用每行 20 字的等宽模拟估计。"
    "草枕在 idpf 里有完整版两份和预览一份，与视觉回归一样都计入。")


def md_cell(s):
    return str(s).replace("|", "\\|").replace("\n", " ")


def write_md():
    out = ["# JLReq 规则级对照（新阅读器）", "", "## 摘要", "", SUMMARY, "", METHOD, "",
           "| 状态 | 条数 |", "|---|---|"]
    out += ["| %s | %d |" % (ST[s], counts[s]) for s in ORDER]
    out += ["", "## 对照表", "",
            "| 条目 | 要求 | 状态 | 我们现在 | 代码 | 测试 | 截图 | 频次（公共 / K8） |", "|---|---|---|---|---|---|---|---|"]
    for r in ROWS:
        title = r["sec"] + " " + r["title"] + (" **(第1批)**" if r.get("batch") == 1 else "")
        out.append("| %s | %s | %s | %s | `%s` | %s | %s | %s |" % tuple(md_cell(x) for x in (
            title, r["req"], ST[r["status"]], r["ours"], r["code"], r["test"], r["shot"] or "—", freq_text(r))))
    out += ["", "## 第 2 批候选（按公共语料出现次数）", ""]
    for i, g in enumerate(ranked, 1):
        ids = "、".join(r["sec"] for r in g)
        names = "；".join(r["title"] for r in g)
        out.append("%d. %s：%s — %s" % (i, ids, names, freq_text(g[0]) if g[0].get("freq") else "未量化"))
    out += ["", "频次键见 `tools/jlreq/rows.py` 各行的 freq；表由 `tools/jlreq/gen_report.py` 生成。", ""]
    os.makedirs(os.path.dirname(md_out), exist_ok=True)
    open(md_out, "w").write("\n".join(out))


CSS = """
:root{--bg:#fbfaf7;--fg:#1d1d1f;--mut:#6b6b70;--line:#e3e1da;--card:#fff;--done:#2f7d4a;--partial:#b7791f;--missing:#c0392b;--na:#8a8a8f;--b1:#2b6cb0}
@media (prefers-color-scheme:dark){:root:not([data-theme=light]){--bg:#17171a;--fg:#ececf0;--mut:#a0a0a8;--line:#33333a;--card:#202024;--done:#5fbf7f;--partial:#e0a84a;--missing:#ef6b5b;--na:#8f8f96;--b1:#6aa7e8}}
:root[data-theme=dark]{--bg:#17171a;--fg:#ececf0;--mut:#a0a0a8;--line:#33333a;--card:#202024;--done:#5fbf7f;--partial:#e0a84a;--missing:#ef6b5b;--na:#8f8f96;--b1:#6aa7e8}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.6 system-ui,"Noto Sans CJK SC","Noto Sans CJK JP",sans-serif}
main{max-width:1180px;margin:0 auto;padding:24px 16px 64px}h1{font-size:26px;margin:0 0 4px}h2{font-size:20px;margin:36px 0 10px;border-bottom:1px solid var(--line);padding-bottom:4px}
.mut{color:var(--mut);font-size:13px}.tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));gap:10px;margin:16px 0}
.tile{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:10px 14px}.tile b{display:block;font-size:28px;line-height:1.2}
.s-done{color:var(--done)}.s-partial{color:var(--partial)}.s-missing{color:var(--missing)}.s-na{color:var(--na)}
.tag{display:inline-block;font-size:12px;padding:0 7px;border-radius:9px;border:1px solid currentColor;white-space:nowrap}
.b1{color:var(--b1);font-size:12px;font-weight:600;margin-left:4px}
.tw{overflow-x:auto;border:1px solid var(--line);border-radius:10px;background:var(--card)}table{border-collapse:collapse;width:100%;font-size:13px}
th,td{text-align:left;vertical-align:top;padding:6px 8px;border-bottom:1px solid var(--line)}th{position:sticky;top:0;background:var(--card)}
td code{font-size:11.5px;word-break:break-all}.filters{margin:8px 0;display:flex;gap:8px;flex-wrap:wrap}.filters button{font:inherit;font-size:13px;padding:3px 10px;border-radius:8px;border:1px solid var(--line);background:var(--card);color:var(--fg);cursor:pointer}
.filters button.on{border-color:var(--fg)}.gap{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px 14px;margin:12px 0}
.gap h3{margin:0 0 6px;font-size:16px}.gap dl{display:grid;grid-template-columns:7em 1fr;gap:4px 10px;margin:0 0 8px}.gap dt{color:var(--mut)}.gap dd{margin:0}
.imgs{display:flex;gap:10px;flex-wrap:wrap}.imgs figure{margin:0;min-width:0;max-width:100%}.imgs img{max-width:100%;height:auto;border:1px solid var(--line);border-radius:6px;background:#fff}
.imgs figcaption{font-size:12px;color:var(--mut)}ol li{margin:6px 0}
@media (max-width:640px){.gap dl{grid-template-columns:1fr}.imgs img{max-width:100%}}
"""


def esc(s):
    return html.escape(str(s))


def shot_files(r):
    s = r.get("shot")
    if not s:
        return []
    base = s.rsplit("-", 1)[0]
    return [base + "-h", base + "-v"]


def write_html():
    img = os.path.join(html_dir, "img")
    os.makedirs(img, exist_ok=True)
    used = set()
    for r in ROWS:
        for f in shot_files(r):
            used.add(f)
    for f in used:
        src = os.path.join(shots, f + "-p1.png")
        if os.path.exists(src):
            shutil.copy(src, os.path.join(img, f + ".png"))
    for ref in ("kevin-brackets.jpg", "kevin-ruby-overhang.jpg"):
        src = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), ref)
        if os.path.exists(src):
            shutil.copy(src, os.path.join(img, ref))

    p = ['<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">',
         "<title>JLReq 对照报告</title><style>%s</style></head><body><main>" % CSS,
         "<h1>新阅读器 JLReq 规则级对照报告</h1>",
         '<p class="mut">2026-11-18 · 任务 #oyomi:#38 · W3C JLReq (WG Note 2020-08-11) 第 3 章</p>',
         "<p>%s</p>" % esc(SUMMARY),
         '<div class="tiles">' + "".join('<div class="tile"><b class="s-%s">%d</b>%s</div>' % (s, counts[s], ST[s]) for s in ORDER) +
         '<div class="tile"><b>%d</b>条目总数</div></div>' % total,
         '<p class="mut">%s</p>' % esc(METHOD)]

    # batch 1
    p.append("<h2>第 1 批（本任务在做）</h2>")
    p.append('<div class="imgs"><figure><img src="img/kevin-brackets.jpg" width="420" alt="JLReq 3.1.5 三种配置"><figcaption>JLReq 3.1.5 图（Kevin 提供）</figcaption></figure>'
             '<figure><img src="img/kevin-ruby-overhang.jpg" width="300" alt="JLReq 3.3.8 ルビの掛け"><figcaption>JLReq 3.3.8 图（Kevin 提供）</figcaption></figure></div>')
    p.append("<ul>" + "".join("<li><b>%s %s</b>（%s）：%s</li>" % (esc(r["sec"]), esc(r["title"]), ST[r["status"]], esc(r["ours"]))
                              for r in ROWS if r.get("batch") == 1) + "</ul>")

    # table
    p.append("<h2>对照表</h2>")
    p.append('<div class="filters">' + '<button class="on" data-f="all">全部</button>' +
             "".join('<button data-f="%s">%s（%d）</button>' % (s, ST[s], counts[s]) for s in ORDER) + "</div>")
    p.append('<div class="tw"><table><thead><tr><th>条目</th><th>要求</th><th>状态</th><th>我们现在</th><th>代码 / 测试</th><th>截图</th><th>频次</th></tr></thead><tbody>')
    for r in ROWS:
        sh = " ".join('<a href="img/%s.png">%s</a>' % (f, f.rsplit("-", 1)[1]) for f in shot_files(r) if f in used)
        p.append('<tr data-s="%s"><td><b>%s</b><br>%s%s</td><td>%s</td><td><span class="tag s-%s">%s</span></td><td>%s</td>'
                 "<td><code>%s</code><br><span class=\"mut\">%s</span></td><td>%s</td><td>%s</td></tr>" % (
                     r["status"], esc(r["sec"]), esc(r["title"]), '<span class="b1">第1批</span>' if r.get("batch") == 1 else "",
                     esc(r["req"]), r["status"], ST[r["status"]], esc(r["ours"]), esc(r["code"]), esc(r["test"]),
                     (esc(r["shot"].rsplit("-", 1)[0]) + " " + sh) if sh else "—", esc(freq_text(r))))
    p.append("</tbody></table></div>")

    # gaps
    p.append("<h2>缺失与部分实现：逐条说明</h2>")
    for r in ROWS:
        if r["status"] not in ("missing", "partial"):
            continue
        figs = "".join('<figure><img src="img/%s.png" width="%d" alt="%s"><figcaption>%s（我们现在）</figcaption></figure>' % (
            f, 296 if f.endswith("-h") else 236, esc(f), "横排" if f.endswith("-h") else "竖排") for f in shot_files(r) if f in used)
        p.append('<div class="gap" id="%s"><h3>%s %s <span class="tag s-%s">%s</span>%s</h3><dl><dt>JLReq 要求</dt><dd>%s</dd>'
                 "<dt>我们现在</dt><dd>%s</dd><dt>代码</dt><dd><code>%s</code></dd><dt>测试</dt><dd>%s</dd><dt>语料频次</dt><dd>%s</dd></dl>"
                 '<div class="imgs">%s</div></div>' % (
                     esc(r["id"]), esc(r["sec"]), esc(r["title"]), r["status"], ST[r["status"]],
                     '<span class="b1">第1批</span>' if r.get("batch") == 1 else "", esc(r["req"]), esc(r["ours"]), esc(r["code"]),
                     esc(r["test"]), esc(freq_text(r)), figs))

    # batch 2
    p.append("<h2>第 2 批候选（按公共语料出现次数排序）</h2><ol>")
    for g in ranked:
        p.append("<li><b>%s</b>：%s<br><span class=\"mut\">%s</span></li>" % (
            esc("、".join(r["sec"] for r in g)), esc("；".join(r["title"] for r in g)),
            esc(freq_text(g[0]) if g[0].get("freq") else "未量化（语料里没有直接可数的标记）")))
    p.append("</ol>")
    p.append('<p class="mut">频次：公共语料出现次数（书数）/ K8 上 4 本版权书的总数。行头行尾类为每行 20 字的模拟估计。</p>')
    p.append("""<script>
document.querySelectorAll('.filters button').forEach(b=>b.addEventListener('click',()=>{
 document.querySelectorAll('.filters button').forEach(x=>x.classList.toggle('on',x===b));
 const f=b.dataset.f;document.querySelectorAll('tbody tr').forEach(tr=>{tr.style.display=(f==='all'||tr.dataset.s===f)?'':'none'})}));
</script></main></body></html>""")
    open(os.path.join(html_dir, "index.html"), "w").write("\n".join(p))


write_md()
write_html()
print(counts, total, "batch2 groups:", len(ranked))
