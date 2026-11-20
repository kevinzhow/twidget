# JLReq 规则级对照（新阅读器）

## 摘要

本表逐条对照 W3C《日本語組版処理の要件》(JLReq, 2020-08-11 WG Note) 第 3 章与新阅读器（twidget litehtml + oyomi 阅读器）。共 88 条（其中 80 条适用、8 条因 EPUB/CSS 无对应结构或可选互斥方案而不适用）：**已实现 80 / 80（100%）、部分 0、缺失 0、错误 0、不适用 8**。

演进历程：
- 第 1–4 批（基础 JLReq）：段首括号 ①/③、ルビ掛假名/句读点/括弧/中点、行调整先压缩与括弧句读点旁不加空、熟語ルビ断行与掛け、和欧间四分、？！ 后全角空、汉数字中的 、・、グループルビ行头行尾 1:1、亲字与上下标不断行（3.1.10 ⑨）、上下标约 60% 不撑开行距（3.7.1）、短グループルビ两端空上限（3.3.6 注）、横排日文 ，． 按 、。 处理（3.1.1 ②）。
- Wave 1（`#41`/`#45`）：3.1.5 ①/③ 段首括弧（含连续对话与 `<br/><span>`）、3.3.5/3.3.8 单侧非对称ルビ掛け、3.3.9 竖排傍点分页归属、3.1.1 ② 无 lang / OPF 语言路由修复。
- Wave 2（`#42`/`#46`）：3.3.8 e–h 熟語ルビ整词行头/行尾与跨行（含空 `<rt>`）、3.3.10 跨 `<ruby>` 连续下划线/傍線/删除线、3.1.6 ？！ 句末全角空与 `‐`/`゠` 连字符、3.1.10 ④⑤⑨ 跨 `<span>` 缩写与上下标、3.1.3 汉数字 `零・五`、3.3.6 注短グループルビ 1:2:1 均分与 1-ruby-em 上限。
- Wave 3（本次，收官至 **80/80 适用条目 100%**）：
  1. **3.5.3 ④ 均等揃え**：支持 CSS `text-align-last`（`auto|start|end|left|right|center|justify`）与 `text-align: justify-all`，单行/段末/`<br>` 前均分字距（横排与竖排，遵守 3.1.11 括弧/标点旁不加空）。
  2. **3.3.4 ルビ位置**：支持 CSS `ruby-position: over | under | right | left` 及 `-webkit-ruby-position: before | after` / `-epub-ruby-position`，竖排左ルビ、横排下ルビ完整保留掛け、下划线避让、行间防碰撞与阅读器跨页归属。
  3. **3.2.6 ② & 3.8.2 ①/3.8.3 欧文间隔先压缩（追込み第 1 步）+ 非贪婪追込み**：日欧混排行在追込み时优先压缩欧文词间空格（`cl-26`）至四分/最小 0.65×（纯欧文段落断行不变）；且已自然满行的行不再过度压缩行中 `、` / `，`。
  4. **3.5.4 段末行字数过少**：多行两端对齐日文段落末尾出现 1–2 字孤尾（如 `る。` 或单字孤尾）时，先尝试在 JLReq 压缩预算内追込み收进倒数第二行，无法收进且为单字孤尾时按禁则安全追出し 1 字（倒数第二行每空扩展 ≤ 四分）；`<br>`、单行、左/中/右对齐及非日文段落不触发，可通过 `document::set_widow_adjustment(widow_adjustment::none)` 关闭。

所有适用条目均已通过 `twidget` 单元测试、`oyomi-reader-flutter` 真字体（Noto Serif CJK JP 横排+竖排）引擎测试，以及 **W3C JLReq 官方图示 + LuaTeX-ja `jlreq.cls`（同字体 Noto Serif CJK JP）** 出版级 Ground Truth 逐图比对验证（0 `MISMATCH`）。

代码与测试的位置以 twidget Wave 3 分支 `martin/jlreq-wave3-complete` 及 `oyomi-reader-flutter` Wave 3 分支 `martin/jlreq-wave3-verify` 为准。频次由 `tools/jlreq/corpus_scan.py` 统计：公共语料（w3c、idpf，及公有领域的 Aozora/Gutenberg 书）与 K8 统计计数。

| 状态 | 条数 |
|---|---|
| 已实现 | 80 |
| 部分 | 0 |
| 缺失 | 0 |
| 不适用 | 8 |

## 对照表

| 条目 | 要求 | 状态 | 我们现在 | 代码 | 测试 | 截图 | 频次（公共 / K8） |
|---|---|---|---|---|---|---|---|
| 3.1.1 ① 竖排句读点用 、。 | 竖排的句点、逗点为 。、（字形位于字面右上）。 | 已实现 | 书中文字原样；竖排用字体的 vert 字形。 | `text/src/text_backend.cpp (vert/vrt2)` | vertical_text_tests.cpp: "with vertical forms, upright" | p-consec-v | — |
| 3.1.1 ② 横排 ，． 作为句读点 **(第4批)**（本次；之前部分） | 横排可用 ，．/，。/、。 三组之一；用 ，． 时它们与 、。 同属句读点（cl-06/07），后面二分空，可压缩。 | 已实现 | 横排、语言为日文（最近的 lang/xml:lang，没有时用容器的语言）时，，． 按 、。（cl-07/06）处理：行末去后二分、后接「 时「减半、」．等连续约物压缩、行调整时 ， 可压（． 在行中不压，同 。）、旁边不加空。竖排、标为中文等其他语言时照旧（中文的 ， 可能居中）。document::set_comma_period。 | `litehtml/src/el_text.cpp el_text::rule_char; litehtml/src/line_box.cpp edge_of` | punctuation_tests.cpp: "3.1.1 ②: …" 10 例（横排同 、。；竖排、zh-CN 照旧） | p-comma-period-h | 6874（2 本）/ K8 版权书 0 |
| 3.1.1 ③ 横排引号/方括号 | 横排可用 “ ” ‘ ’ 代替「」；〝〟只用于竖排；横排方括号用［］。 | 不适用 | 由书的文字决定，引擎不改字。 | `—` | — | — | — |
| 3.1.2 ①–⑤ 约物的基本配置 | 读点、句点、终わり括弧后二分空，始め括弧前二分空，中点前后四分空（与假名汉字相邻时整体看作全角）。 | 已实现 | 全角字形本身带这些空；压缩只在规则允许处去掉一半。 | `litehtml/src/cjk_rules.cpp:155 cjk::compression` | punctuation_tests.cpp: "？ and U+3000 stay full width" | p-consec-h | — |
| 3.1.3 汉数字中的 、・ 紧排 **(第3批)** | 竖排里作位取り/概数的读点、汉数字间作小数点的中点，前后紧排（不留空）。 | 已实现 | 汉数字之间的 、 去后二分、・ 去前后四分（二、三日；三・五），接假名时照旧。 | `litehtml/src/cjk_rules.cpp cjk::compression` | punctuation_tests.cpp: "3.1.3: 、 between kanji numerals, solid", "3.1.3: ・ between kanji numerals, solid" | — | 3（1 本）/ K8 版权书 17 |
| 3.1.4 ① 、。后接终わり括弧 | 之间紧排，终わり括弧后二分。 | 已实现 | 、。 在终わり括弧前去后一半。 | `litehtml/src/cjk_rules.cpp:155` | punctuation_tests.cpp: "。 before a closing bracket" | p-consec-h | — |
| 3.1.4 ② 终わり括弧后接、。 | 之间紧排，、。后二分。 | 已实现 | 终わり括弧在 、。前去后一半。 | `litehtml/src/cjk_rules.cpp:155` | punctuation_tests.cpp: "a closing bracket before 。" | p-consec-h | — |
| 3.1.4 ③ 、。后接始め括弧 | 之间二分空。 | 已实现 | 始め括弧在约物后去前一半。 | `litehtml/src/cjk_rules.cpp:155` | punctuation_tests.cpp: "an opening bracket after punctuation (。「)" | p-consec-h | — |
| 3.1.4 ④ 终わり括弧后接始め括弧 | 之间二分空。 | 已实现 | 「 去前一半，」保留。 | `litehtml/src/cjk_rules.cpp:155` | punctuation_tests.cpp: "」「: the opening one halves" | p-consec-h | — |
| 3.1.4 ⑤ 始め括弧连续 | 之间紧排，前面二分。 | 已实现 | 第二个始め括弧去前一半。 | `litehtml/src/cjk_rules.cpp:155` | cjk_rules_tests.cpp: compression table | p-consec-h | — |
| 3.1.4 ⑥ 终わり括弧连续 | 之间紧排，最后一个后二分。 | 已实现 | 前一个终わり括弧去后一半。 | `litehtml/src/cjk_rules.cpp:155` | cjk_rules_tests.cpp: compression table | p-consec-h | — |
| 3.1.4 ⑦ 终わり括弧后接中点 | 中点前后四分。 | 已实现 | 」去后一半，・保留全角（前后各四分）。 | `litehtml/src/cjk_rules.cpp:155` | punctuation_tests.cpp: "a closing bracket before a middle dot" | p-consec-h | — |
| 3.1.4 ⑧ 中点后接始め括弧 | 中点后四分。 | 已实现 | 「 去前一半。 | `litehtml/src/cjk_rules.cpp:155` | cjk_rules_tests.cpp: compression table | p-consec-h | — |
| 3.1.5 ① 行头始め括弧：段首全角アキ、折行天ツキ **(第1批)** | 段落第一行（改行行头）的始め括弧前空全角（＝一字下げ后括弧取半角），折返し行头的始め括弧天ツキ（不留空）。JIS X 4051 采用此法。 | 已实现 | 段首（块的第一行、<br> 后的一行）的始め括弧：段落自己的 text-indent ≥1em 时去前一半（墨在缩进处）；不足 1em 而本书其他段落有缩进（U+3000 或 text-indent）时括弧保留全角，再补足到墨离行头一字（全角アキ）；折行行头仍天ツキ。完全不缩进的书、标题、居中段落照旧。 | `litehtml/src/render_inline_context.cpp paragraphs_indented, new_box; litehtml/src/line_box.cpp set_paragraph_start, add_item` | punctuation_tests.cpp: "3.1.5 ①: a paragraph that starts with 「 after one indented with U+3000" 等 8 例 | b-aozora-v | 4279（8 本）/ K8 版权书 6139 |
| 3.1.5 ② 段首全角半アキ、折行二分アキ | 可选方案。 | 不适用 | 不做（取①为默认）。 | `—` | — | — | — |
| 3.1.5 ③ 段首二分アキ、折行天ツキ | 可选方案（小说常用）。 | 已实现 | document::set_paragraph_brackets(half_em)：段首括弧保留全角、不再补空（墨在半字处），折行天ツキ。阅读器「段首括号」设置：自动（小说 ③、其他书 ①，每本书打开时判定一次）/①/③。 | `litehtml/include/litehtml/document.h paragraph_brackets; litehtml/src/render_inline_context.cpp new_box; reader: native/src/brackets.cpp` | punctuation_tests.cpp: "3.1.5 ③: …" 6 例; reader engine_tests paragraph_bracket_tests | — | — |
| 3.1.6 ① ？！ 句末后全角空 **(第3批)** | ？！ 用全角；作句末时前紧排、后空一字（接终わり括弧时紧排，括弧后二分）。 | 已实现 | 书里没写空时，句末 ？！ 后加全角空（接假名、汉字、始め括弧时）；接终わり括弧、句读点、U+3000、接续句子的助词（と、って、か…）时不加；行末不加。助词判断是启发式。document::set_dividing_space。 | `litehtml/src/cjk_rules.cpp dividing_space_after; litehtml/src/el_text.cpp compression` | punctuation_tests.cpp: "3.1.6 ①: a full-width space after ！", "none before と" | p-dividing-h | 31（4 本）/ K8 版权书 5 |
| 3.1.6 ② 行末 ？！ 的空 | 句末 ？！ 在行末时其后的一字空可去，下一行天ツキ。 | 已实现 | 行末 ？！ 后的 U+3000 悬挂在行外（同 Chromium），下一行天ツキ（v2 核对：与 Chromium 相同）。 | `litehtml/src/line_box.cpp` | v2 例 p-dividing-end | — | — |
| 3.1.6 (hyphens) 连字符类的宽度 | ‐ 四分、–゠ 二分、〜 全角；与始め括弧间二分、与中点间四分。 | 已实现 | 用字体的字宽；‐゠〜 不可行首。 | `litehtml/src/cjk_rules_table.inc` | cjk_rules_tests.cpp: classify table | — | — |
| 3.1.7 行頭禁則 | cl-02/03/04/05/06/07/09/10/11 不在行头。 | 已实现 | 规则表 not_starting（含小书き假名、ー、々ゝ、？！、‐゠〜）；跨元素也生效。 | `litehtml/src/cjk_rules.cpp:127 no_break_between; litehtml/src/render_inline_context.cpp:191` | cjk_rules_tests.cpp: "ー does not start a line (サ\|ービス)", "small っ does not start a line" | k-kinsoku-v | — |
| 3.1.8 行末禁則 | cl-01（始め括弧）不在行末。 | 已实现 | 规则表 not_ending。 | `litehtml/src/cjk_rules.cpp:127` | cjk_rules_tests.cpp: "「 does not end a line" | k-kinsoku-v | — |
| 3.1.9 (原則) 行末 」、。 的二分 | 行末的终わり括弧、读点、句点后的二分在调整时可去（只取二分或零）。 | 已实现 | 折行的行末（不是段末、不是 <br> 前）去后一半。 | `litehtml/src/line_box.cpp:336` | punctuation_tests.cpp: "。 ending a wrapped line" | p-line-end-h | 2481（8 本）/ K8 版权书 1598 |
| 3.1.9 (JIS) JIS：行末句点保留二分 | JIS X 4051 的另一做法：行末句点的二分必须保留。 | 不适用 | 按原則（去掉）。 | `—` | — | — | — |
| 3.1.9 (中点) 行末中点 | 行末中点前后各四分（也可紧排）。 | 已实现 | 中点不压缩。 | `litehtml/src/cjk_rules.cpp:155` | — | p-line-end-h | 21（1 本）/ K8 版权书 9 |
| 3.1.10 ① 二倍ダッシュ 不分割 | ——/―― 不分开；与括弧、句读点、中点之间的空按 1.1–1.3。 | 已实现 | inseparable 类，不断开；两端对齐也不在其中加空。 | `litehtml/src/cjk_rules_table.inc` | cjk_rules_tests.cpp: "―― (U+2015) is not split" | — | — |
| 3.1.10 ② …… ‥‥ 不分割 | 二倍三点/二点リーダ不分开。 | 已实现 | inseparable 类。 | `litehtml/src/cjk_rules_table.inc` | cjk_rules_tests.cpp: "…… is not split" | — | — |
| 3.1.10 ③ 欧文数字连续不分割 | 连续欧文数字（含小数点、位取り逗号、空格）不分开。 | 已实现 | 拉丁字母数字按单词处理，内部不断。 | `litehtml/src/el_text.cpp` | justify_tests.cpp: "the first line spread between its characters" | — | — |
| 3.1.10 ④⑤ 前置/后置省略记号与数字 | ¥ $ 与后面的数字、% ‰ 与前面的数字不分开。 | 已实现 | ¥100、50％ 不分开（v2 核对：与 Chromium line-break: strict 相同）。 | `tools/cjk_rules/overlay.json` | v2 例 x-abbrev | — | — |
| 3.1.10 ⑥ 欧文单词、单位不分割 | 不可连字的欧文单词、单位（km 等）不分开；连字符处可断。 | 已实现 | 单词内不断；hyphens 按 CSS。 | `litehtml/src/el_text.cpp:609` | justify_tests.cpp | — | — |
| 3.1.10 ⑦ モノルビ：亲字之间可断 **(第3批)** | 一个亲字与其ルビ不分开；モノルビ的亲字与亲字之间可以断行。 | 已实现 | 一个 <ruby> 里有多对（熟語ルビ）时可在亲字之间断行，不在一对之内断。document::set_ruby_breaking。 | `litehtml/src/render_ruby.cpp render_item_ruby_word; litehtml/src/iterators.cpp` | ruby_overhang_tests.cpp: "3.3.7: a line breaks between a word's base units" | r-jukugo-v | 0（0 本）/ K8 版权书 1124 |
| 3.1.10 ⑧ グループルビ不分割；熟語ルビ按单位可断 **(第3批)** | グループルビ整体不分开；熟語ルビ可在每个亲字的单位处断。 | 已实现 | グループルビ（一对）不分开；熟語ルビ按单位可断。 | `litehtml/src/render_ruby.cpp` | ruby_overhang_tests.cpp: "3.3.7: っ, which may not start a line, takes 字 with it" | r-jukugo-v | 0（0 本）/ K8 版权书 1124 |
| 3.1.10 ⑨ 添え字不分割 **(第4批)**（本次；之前缺失） | 亲字与上下标之间不断。 | 已实现 | 亲字与其上下标（vertical-align: super/sub 的框）之间不断（数²、H₂O、。后的注号）；空格之后的上标仍可行首。document::set_script_breaking。 | `litehtml/src/render_inline_context.cpp no_break_opportunity` | script_tests.cpp: "3.1.10 ⑨: 数² is not split" 等 | — | — |
| 3.1.10 ⑩–⑫ 合印、割注括弧、振分け | 合印前/内不断；cl-28 后、cl-29 前不断；振分け单位不断。 | 不适用 | EPUB 无这些结构。 | `—` | — | — | — |
| 3.1.11 ① 分割禁止处不加空 | 3.1.10 所列的不可分处，行调整时不加空。 | 已实现 | cjk::justifiable_gap：…… ―― 縦中横 拉丁单词内不加空。 | `litehtml/src/cjk_rules.cpp:141` | justify_tests.cpp: "the first line spread between its characters" | — | — |
| 3.1.11 ②-1 始め/终わり括弧前后不加空 **(第2批)** | 行调整（空け）时不在始め括弧、终わり括弧的前后加空。 | 已实现 | 扩展时不在日文始め/终わり括弧前后加空；只有全行除此之外没有可加空处时才照旧均分。 | `litehtml/src/cjk_rules.cpp justify_gap_kind; litehtml/src/line_box.cpp finish` | punctuation_tests.cpp: "3.1.11 ②-1: no space next to brackets" | j-justify-h | 4400（10 本）/ K8 版权书 3168 |
| 3.1.11 ②-2…②-6 句读点、中点、区切り約物、连字符、和字間隔前后不加空 **(第2批)** | 同上，对 cl-06/07、cl-05、cl-04、cl-03、cl-14。 | 已实现 | 同上：、。・：；、！？‼⁇、゠〜、U+3000 前后不加空（日文的；欧文的 ! ? ( ) 照旧）。 | `litehtml/src/cjk_rules.cpp justify_gap_kind` | punctuation_tests.cpp: "3.8.3: 。 inside a line keeps its half em; 3.1.11 ②: no space next to it" | j-justify-h | 4400（10 本）/ K8 版权书 3168 |
| 3.1.11 ②-7 熟語ルビ亲字间不加空 **(第3批)** | 熟語ルビ的亲字之间不加空。 | 已实现 | 熟語ルビ的亲字之间两端对齐不加空（同一个 <ruby> 的两对之间）。 | `litehtml/src/line_box.cpp finish` | ruby_overhang_tests.cpp: "3.1.11 ②-7: justify puts no space between a word's bases" | — | — |
| 3.1.12 ② 追込み（先压缩） **(第2批)** | 行末溢出半字（如 、「）时，先把可压缩的二分减到四分，把字收进本行。 | 已实现 | 行末溢出不超过可压缩的量时先压缩（行末 」、。 二分、中点四分、行中括弧与读点二分，到ベタ），把字收进本行；压不进才推到下一行。 | `litehtml/src/line_box.cpp can_hold, squeeze_room, squeeze` | punctuation_tests.cpp: "3.8.3: JLReq Fig. 89 ③, 、「 and 」 squeezed to a quarter em" | k-kinsoku-h | 4400（10 本）/ K8 版权书 3168 |
| 3.1.12 ③ 追出し | 不能留在行末的字推到下一行，本行加空撑满。 | 已实现 | 是。 | `litehtml/src/line_box.cpp:981 can_hold` | cjk_rules_tests.cpp: "「 and a ruby that fit stay on the line" | k-kinsoku-v | — |
| 3.2.2 横排欧文：比例字体、欧文间隔 | 欧文用比例字体；欧文间隔约 1/3em，行首行末为 0。 | 已实现 | 字体的空格宽；行首行末空格按 CSS 折叠。 | `litehtml/src/line_box.cpp` | — | l-latin-h | — |
| 3.2.3 ① 竖排单个欧文字直立 | 单个字母/数字可一字一字直立。 | 已实现 | text-orientation: upright、全角字母直立。 | `litehtml/src/el_text.cpp (text-orientation)` | vertical_text_tests.cpp | t-tcy-v | — |
| 3.2.3 ② 竖排欧文单词顺时针转 90° | 单词、句子用比例字体横躺。 | 已实现 | text-orientation: mixed（默认）。 | `litehtml/src/el_text.cpp` | vertical_text_tests.cpp: "without, sideways" | t-tcy-v | 96（6 本）/ K8 版权书 20 |
| 3.2.3 ③/3.2.5 縦中横 | 两位数字等横排后居中放进一字；与前后字紧排，与括弧、句读点按二分规则。 | 已实现 | 自动：1–2 位 ASCII 数字、!? 等；CSS text-combine-upright。 | `litehtml/src/cjk_rules.cpp:170 tate_chu_yoko` | tate_chu_yoko_tests.cpp: "two digits side by side in one em", "three digits stay sideways" | t-tcy-v | 67（5 本）/ K8 版权书 8 |
| 3.2.4 全角欧文按汉字处理 | 全角拉丁字母、数字直立，按 cl-19 与假名汉字紧排。 | 已实现 | 是。 | `litehtml/src/cjk_rules.cpp:136 script_of` | — | l-latin-h | — |
| 3.2.6 ①③ 欧文单词不跨行、扩展时不在词内加空 | 欧文单词只在连字处断；行调整不在欧文单词、数字内加空。 | 已实现 | 是。 | `litehtml/src/cjk_rules.cpp:141` | justify_tests.cpp | — | — |
| 3.2.6 ② 调整时先动欧文间隔 **(Wave 3)** | 压缩和扩展都先用欧文间隔。 | 已实现 | 扩展与压缩均先用欧文间隔（`cl-26`）：日欧混排行在追込み时优先压缩行内欧文词间空格（至四分 / 最小 0.65×，单空格上限八分），纯欧文段落断行不变；扩展每侧最多八分。 | `litehtml/src/line_box.cpp squeeze_room, squeeze, finish` | punctuation_tests.cpp: "3.2.6 ② / 3.8.3: Western word spaces compress first", "3.8.4: word spaces first"; reader engine_tests jlreq_wave3_tests | gt-w3-03-326-382-western-oikomi | 4400（10 本）/ K8 版权书 3168 |
| 3.2.6 ④ 和欧间四分空 **(第3批)** | 假名汉字与欧文/数字之间空四分（行首行末不空；括弧、句读点旁按 4.1–4.3）。 | 已实现 | 假名、汉字与欧文字母、数字之间空四分；行末不空；括弧、句读点旁不加；纵中横数字和行内框旁不加。document::set_latin_spacing。 | `litehtml/src/cjk_rules.cpp latin_space_after; litehtml/src/el_text.cpp compression` | punctuation_tests.cpp: "3.2.6 ④: a quarter em each side of a Latin letter" | l-latin-h | 4600（11 本）/ K8 版权书 55 |
| 3.3.3 ルビ字号为亲字一半 | ルビ原则上为亲字的一半大小。 | 已实现 | rt { font-size: 50% }。 | `litehtml/include/litehtml/master_css.h:410` | core_tests.cpp: "ruby: the annotation over the base" | r-mono-v | — |
| 3.3.4 ルビ位置 **(Wave 3)** | 竖排在右、横排在上；左/下少见。 | 已实现 | 默认右/上；完整支持 CSS `ruby-position: over \| under \| right \| left` 及 `-webkit-ruby-position: before \| after` / `-epub-ruby-position`，竖排左ルビ、横排下ルビ保留掛け、下划线避让、行间防碰撞与阅读器跨页归属。 | `litehtml/src/style.cpp; litehtml/src/css_properties.cpp; litehtml/src/render_ruby.cpp; litehtml/src/render_inline_context.cpp make_room_for_ruby; reader: native/src/chapter.cpp` | ruby_overhang_tests.cpp: "3.3.4: ruby-position …" 7 例; reader engine_tests jlreq_wave3_tests | gt-w3-02-334-ruby-position | — |
| 3.3.5 モノルビ：2 假名等长，1 假名中付き | 2 个假名与亲字等长；1 个假名中付き（或竖排肩付き）。 | 已实现 | 居中（中付き）；不做肩付き（可选）。 | `litehtml/src/render_ruby.cpp:96 space_around` | core_tests.cpp: "ruby: the annotation widens the pair, the base centred in it" | r-mono-v | 3982（3 本）/ K8 版权书 574 |
| 3.3.5 ①② モノルビ 3 假名以上：居中并掛け **(第1批)** | 3 个以上假名的ルビ紧排、与亲字中心对齐，超出部分掛在前后字上（按 3.3.8）；肩付き时默认掛在后面。 | 已实现 | ルビ居中；超出部分掛到相邻的假名上（每侧最多一个ルビ字），不能掛的一侧空开亲字。 | `litehtml/src/line_box.cpp ruby_overlap; litehtml/src/render_ruby.cpp base_start/base_end` | ruby_overhang_tests.cpp: "rule 2: の旬に" | r-overhang-h | 2888（3 本）/ K8 版权书 739 |
| 3.3.6 グループルビ：长短不同时 1:2:1 | ルビ短：ルビ字间 2、两端各 1；ルビ长：亲字间 2、两端各 1（JIS X 4051）。 | 已实现 | ruby-align: space-around，正是 1:2:1。 | `litehtml/src/render_ruby.cpp:96; litehtml/src/render_inline_context.cpp:472` | core_tests.cpp: "ruby: the annotation widens the pair, the base centred in it" | r-group-v | — |
| 3.3.6 (注) 短ルビ两端空的上限 **(第4批)**（本次；之前缺失） | ルビ比亲字短很多时，两端空不超过一个ルビ字（或 1.5 倍）。 | 已实现 | 日文ルビ比亲字短时仍按 1:2:1，但两端各不超过一个ルビ字（ルビ字号），其余分到ルビ字（断行单位）之间。document::set_short_ruby。 | `litehtml/src/render_ruby.cpp render_item_ruby_pair; litehtml/src/render_inline_context.cpp space_around` | ruby_overhang_tests.cpp: "3.3.6 (注): かな over 東京都庁, one ruby character at each end" | r-group-v | — |
| 3.3.7 熟語ルビ：每字 ≤2 假名按モノルビ | 每个亲字的ルビ不超过 2 字时，逐字按モノルビ配置。 | 已实现 | 每对 rt 各成一对，居中。 | `litehtml/src/render_ruby.cpp:7` | — | r-jukugo-v | 0（0 本）/ K8 版权书 1124 |
| 3.3.7 熟語ルビ：某字 ≥3 假名 **(第3批)** | 按グループルビ配置，或让ルビ掛到同一熟語的其他亲字上（不超过一个ルビ字）。 | 已实现 | 某字 ≥3 假名时，长ルビ掛到同词相邻亲字上其ルビ留下的空，最多一个ルビ字（JLReq 附录 F 的做法；未用 JIS 的グループルビ式）。 | `litehtml/src/line_box.cpp ruby_overlap` | ruby_overhang_tests.cpp: "3.3.7: きゅう overhangs 瓜, whose り leaves room" | r-jukugo-v | 0（0 本）/ K8 版权书 166 |
| 3.3.7 熟語ルビ可在单位间断行 **(第3批)** | 熟語ルビ可在亲字单位处分到两行（分开后按 3.3.8 h）。 | 已实现 | 熟語ルビ可在单位间断行；分开后各部分按モノルビ的规则排，行头行尾按 a/b。 | `litehtml/src/render_ruby.cpp render_item_ruby_word` | ruby_overhang_tests.cpp: "3.3.8 e–h: a word broken across lines" | r-jukugo-v | 0（0 本）/ K8 版权书 1124 |
| 3.3.8 (总则) ルビ短于亲字：亲字与前后紧排 | ルビ不长于亲字时，亲字与前后字紧排。 | 已实现 | 是。 | `litehtml/src/render_ruby.cpp:96` | core_tests.cpp: Ruby | r-mono-v | — |
| 3.3.8 规则1 不掛汉字 **(第1批)** | ルビ不掛到相邻的汉字（cl-19）上；不能掛时空开亲字。 | 已实现 | 不掛汉字（也不掛标点、空格），该侧空开亲字。 | `litehtml/src/line_box.cpp ruby_overlap` | ruby_overhang_tests.cpp: "rule 1: 後旬又" | r-overhang-h | 237（3 本）/ K8 版权书 70 |
| 3.3.8 规则2 掛平假名、片假名、ー、小书き **(第1批)** | 相邻为平假名、片假名、长音、小书き假名时，ルビ可掛上去，最多一个ルビ字（亲字的一半）。 | 已实现 | 掛平假名、片假名、ー、小书き假名，每侧最多一个ルビ字，且不超过该侧伸出的量；行宽按掛后的宽度断行。 | `litehtml/src/cjk_rules.cpp ruby_may_overhang; litehtml/src/line_box.cpp ruby_overlap` | ruby_overhang_tests.cpp: "rule 2: …" 6 例, "the line holds what the overhang makes room for" | r-overhang-h | 2800（3 本）/ K8 版权书 662 |
| 3.3.8 规则2 注 相邻两个ルビ | 前后两组ルビ相邻时不能互相掛（会相碰）；可空一字或限制掛的量。 | 已实现 | ルビ不掛相邻的ルビ（两组ルビ之间照旧空开）。 | `litehtml/src/line_box.cpp ruby_overlap` | ruby_overhang_tests.cpp: "rule 2 note: a ruby does not overhang the ruby next to it" | r-adjacent-h | 165（3 本）/ K8 版权书 24 |
| 3.3.8 规则3 掛句读点/括弧的二分空 **(第2批)** | 前面是 」、。 的后二分、后面是「 的前二分时，ルビ可掛进这个空（最多一个ルビ字；空被压缩时以剩余量为限）。 | 已实现 | 掛进 」、。 后、「 前的二分空，最多一个ルビ字，不超过压缩后剩下的空；被ルビ掛住的空不再被行调整压缩。 | `litehtml/src/line_box.cpp overhang_room` | ruby_overhang_tests.cpp: "rule 3: into the half em after 」", "rule 3: not what the ruby overhangs" | r-adjacent-h | 722（3 本）/ K8 版权书 138 |
| 3.3.8 规则4/5 掛分離禁止文字、中点 **(第2批)** | 相邻为 ……―― 或中点时可掛，最多一个ルビ字（中点四分被压缩时有上限）。 | 已实现 | 掛 …… ―― 最多一个ルビ字；掛中点：其该侧四分加半个ルビ字，最多一个ルビ字。 | `litehtml/src/line_box.cpp overhang_room` | ruby_overhang_tests.cpp: "rule 4: onto ……", "rule 5: onto middle dots" | r-adjacent-h | — |
| 3.3.8 规则6 掛终わり括弧、句读点、始め括弧本身 **(第2批)** | 可掛到相邻的 」、。（后侧）或「（前侧）上，最多一个ルビ字，但不越过该字。 | 已实现 | 掛前面的「、后面的 」、。 的字面，最多一个ルビ字、不越过该字（按 JLReq 正文；注中“不掛「」”的做法未采用）。 | `litehtml/src/line_box.cpp overhang_room` | ruby_overhang_tests.cpp: "rule 6: onto 「 before and 」 after", "rule 6: no further than the mark itself" | r-adjacent-h | 722（3 本）/ K8 版权书 138 |
| 3.3.8 行头行尾 (总则) ルビ不超出版面 **(第1批)** | 行头行尾的ルビ不能伸出版面/栏。 | 已实现 | 行头行尾不向外掛，ルビ不超出行。 | `litehtml/src/line_box.cpp` | ruby_overhang_tests.cpp: line-edge 4 例 | r-edge-v | — |
| 3.3.8 行头行尾 a/b モノルビ在行头/行尾 **(第1批)** | 行头：ルビ顶与亲字顶对齐（超出部分掛向后字）；行尾：ルビ底与亲字底对齐。 | 已实现 | 行头：亲字与ルビ同时贴行头，超出部分掛向后面的假名；折行行尾：亲字与ルビ同时贴行尾，掛向前面的假名（各最多一个ルビ字）。 | `litehtml/src/render_ruby.cpp align_base; litehtml/src/line_box.cpp add_item, finish` | ruby_overhang_tests.cpp: "line-edge a: …", "line-edge b: …" | r-edge-v | 172（2 本）/ K8 版权书 48 |
| 3.3.8 行头行尾 c/d グループルビ在行头/行尾 **(第3批)** | 行头：ルビ顶对第一个亲字，亲字间与末端空 1:1；行尾反之。 | 已实现 | 行头：ルビ贴行头，亲字间与末端 1:1；折行行尾：ルビ贴行尾（可掛前面的假名），亲字间与前端 1:1（第 1 批起即如此，本批补测试）。 | `litehtml/src/line_box.cpp add_item, finish` | ruby_overhang_tests.cpp: "line-edge c: a group ruby at the line's head, 1:1", "line-edge d" | r-edge-v | 172（2 本）/ K8 版权书 48 |
| 3.3.8 行头行尾 e–h 熟語ルビ在行头/行尾、跨行 **(Wave 2)** | 按 c/d；或亲字贴行头、ルビ掛同词亲字；跨行时分成モノ/熟語两部分。 | 已实现 | 熟語ルビ在行头/行尾时亲字贴齐行端并将溢出ルビ掛入同词相邻亲字（含空 `<rt>` 场景），跨行后各部分按モノ/熟語规则对齐且不超出版面（JLReq 附录 F 方案，与 LuaTeX-ja `luatexja-ruby` 1:1 对齐）。 | `litehtml/src/render_ruby.cpp; litehtml/src/line_box.cpp plan_ruby, add_item, finish` | ruby_overhang_tests.cpp: "3.3.8 e–h: a word broken across lines", "3.3.7: …" (Wave 1/2); reader engine_tests | gt-w1-04-337-jukugo-ruby | — |
| 3.3.9 ①② 圏点：亲字一半大小，右/上居中 | 圏点为亲字一半大，竖排在右、横排在上，居中。 | 已实现 | text-emphasis：位置、居中。 | `litehtml/src/el_text.cpp:133 draw_emphasis` | engine_tests.cpp (vertical-rl ruby + sesame chapter) | e-dots-v | 1140（4 本）/ K8 版权书 1078 |
| 3.3.9 ③ 圏点字形：竖排﹅、横排• | 通常竖排用 ﹅，横排用 •。 | 已实现 | 按 CSS text-emphasis-style（书决定）。 | `litehtml/src/el_text.cpp:99 emphasis_mark` | — | e-dots-h | — |
| 3.3.9 (注) 句读点、括弧不加圏点 | 实践中 、。「」 不加圏点。 | 已实现 | 标点（Unicode P 类）和空格不加。 | `litehtml/src/el_text.cpp:88 emphasis_skips` | — | e-dots-v | — |
| 3.4.1–3.4.3 割注 | 割注两行排、括弧、跨行。 | 不适用 | EPUB/CSS 没有割注结构。 | `—` | — | — | — |
| 3.5.1 ①–③ 段首字下げ | 段首一字下げ（或全部天ツキ、标题后天ツキ）。 | 已实现 | text-indent 与 U+3000 照排。 | `litehtml/src/render_inline_context.cpp (text-indent)` | — | i-indent-h | 278（3 本）/ K8 版权书 7335 |
| 3.5.2 行头/行尾 indent | 引文、标题按固定量缩进。 | 已实现 | margin/padding。 | `litehtml` | — | i-indent-h | — |
| 3.5.3 ①–③ 中央/行头/行尾揃え | 单行的居中、左/上、右/下对齐。 | 已实现 | text-align。 | `litehtml/src/line_box.cpp:410` | — | — | — |
| 3.5.3 ④ 均等揃え **(Wave 3)** | 单行字距均分撑满（俳句等）。 | 已实现 | 支持 CSS `text-align-last`（`auto\|start\|end\|left\|right\|center\|justify`）与 `text-align: justify-all`，单行标题/俳句、多行段末及 `<br>` 前均分字距撑满行长（横排与竖排，遵守 3.1.11 括弧/标点旁不加空）。 | `litehtml/src/css_properties.cpp; litehtml/src/style.cpp; litehtml/src/line_box.cpp finish` | justify_tests.cpp: "3.5.3 ④: …" 7 例; reader engine_tests jlreq_wave3_tests | gt-w3-01-353-justify-all | — |
| 3.5.4 段末行字数过少 **(Wave 3)** | 避免段末行字太少（日文优先级低）。 | 已实现 | 多行两端对齐日文段落末尾出现 1–2 字孤尾时，先在 JLReq 压缩预算内追込み收进倒数第二行；无法收进且为单字孤尾时按禁则安全追出し 1 字（倒数第二行每空扩展 ≤ 四分）；`<br>`、单行、左/中/右对齐及非日文段落不触发，`document::set_widow_adjustment` 可控。 | `litehtml/src/render_inline_context.cpp _render_content; litehtml/include/litehtml/document.h widow_adjustment` | justify_tests.cpp: "3.5.4: …" 6 例; reader engine_tests jlreq_wave3_tests | gt-w3-04-354-short-last-line | — |
| 3.5.4 页末孤行 | 避免页面只剩一行。 | 已实现 | orphans/widows 默认 2（同 Blink 分栏）。 | `reader: native/src/chapter.cpp:947` | reader engine_tests.cpp: "widows: two lines go together to the next page" | — | — |
| 3.6 タブ | 制表位对齐。 | 不适用 | EPUB 无。 | `—` | — | — | — |
| 3.7.1 添え字 **(第4批)**（本次；之前部分） | 约亲字 60%，紧排，不断开，竖排横躺或縦中横。 | 已实现 | 阅读器在 master_css 之后加 twidget 的 jlreq_scripts_css：sub/sup 为亲字的 60%，line-height 0，含上下标的行行距不变；书自己的字号优先。上下标与亲字及其后的字紧排（不加和欧四分，document::script_breaking）。不断开见 3.1.10 ⑨。竖排中上下标里的拉丁字母数字按 text-orientation: mixed 横躺（JLReq 允许的做法之一），不做縦中横。 | `litehtml/include/litehtml/master_css.h jlreq_scripts_css; reader: native/src/chapter.cpp` | script_tests.cpp: "3.7.1: a superscript is 60% of its base", "3.7.1: a line with <sup> keeps the pitch", "3.7.1: 数²く solid"; reader engine_tests script_tests | — | — |
| 3.7.2–3.7.4 振分け、字取り、数式 | 振分け块、字取り、数式符号的空。 | 不适用 | EPUB 无此结构（数式走 MathML）。 | `—` | — | — | — |
| 3.8.2 ①/3.8.3 先压缩（追込み）的优先顺序 **(Wave 3)** | 先压：欧文间隔（到四分）→行末 」、。 二分→行末中点四分→行中中点四分→行中括弧、读点二分→和欧四分（到八分）。 | 已实现 | 完整按 3.8.3 优先级逐级均分压缩：日欧混排行先压欧文词间空格（至四分 / 最小 0.65×）→行末 」、。 二分→行末中点四分→行中中点四分→行中括弧与读点二分→和欧四分（到八分）；纯欧文段不断行改变；且已自然满行的行不贪婪压行中读点。 | `litehtml/src/line_box.cpp can_hold, squeeze_room, squeeze` | punctuation_tests.cpp: "3.2.6 ② / 3.8.3: …" 4 例, "3.8.3: squeezed to an eighth em to fit"; reader engine_tests jlreq_wave3_tests | gt-w3-03-326-382-western-oikomi | 4400（10 本）/ K8 版权书 3168 |
| 3.8.2 ② 压不进时再扩展（追出し） **(第2批)** | 不能压缩时再扩展。 | 已实现 | 能压缩时先压缩，压不进才追い出し，再扩展。 | `litehtml/src/line_box.cpp can_hold` | punctuation_tests.cpp: "3.8.2: JLReq Fig. 89 ⑤, 「 too long to squeeze in goes on" | k-kinsoku-h | — |
| 3.8.2 (注) ぶら下げ | 句读点可伸出行末（可选，JIS 未正式规定）。 | 不适用 | 不做（规则表有 hanging 类，未用）。 | `litehtml/src/cjk_rules_table.inc` | — | — | — |
| 3.8.4 a 扩展先用欧文间隔（≤二分） **(第2批)** | 先均分到欧文间隔，每个最多二分。 | 已实现 | 先扩欧文间隔，每个间隔两侧各最多八分（间隔本身约四分，合计约二分）。 | `litehtml/src/line_box.cpp finish` | punctuation_tests.cpp: "3.8.4: word spaces first" | l-latin-h | 4400（10 本）/ K8 版权书 3168 |
| 3.8.4 b 再扩和欧间（≤二分） **(第3批)** | 然后扩和欧间的四分。 | 已实现 | 和欧四分之上最多再扩四分（合计二分），在欧文间隔之后、日文字间之前。 | `litehtml/src/line_box.cpp finish` | punctuation_tests.cpp: "3.8.4: Japanese–Latin gaps before kana ones" | l-latin-h | 4600（11 本）/ K8 版权书 55 |
| 3.8.4 c 再扩其他允许处（≤四分） **(第2批)** | 其他不禁止处，每处最多四分。 | 已实现 | 其余日文字间每处最多四分，括弧、句读点、中点旁不加（3.1.11 ②）。 | `litehtml/src/line_box.cpp finish` | punctuation_tests.cpp: "。 ending a wrapped line" | j-justify-h | 4400（10 本）/ K8 版权书 3168 |
| 3.8.4 d 最后均分到所有字间 **(第2批)** | 以上不够时，均分到所有非分離禁止的字间。 | 已实现 | 仍不够时均分到以上所有可加空处（不含括弧、句读点旁）。 | `litehtml/src/line_box.cpp finish` | punctuation_tests.cpp: "3.1.11 ②-1: no space next to brackets" | j-justify-h | — |
| 3.9 / 附录 A 字符类 cl-01…cl-30 | 按字符类决定行头行尾、空、可否断开。 | 已实现 | 规则表由 stone_engine_js 生成并加 overlay；cl-20…25、28/29 等结构类无对应。 | `litehtml/src/cjk_rules_table.inc; tools/cjk_rules/` | cjk_rules_tests.cpp: classify table | — | — |

## 下一批候选

**无剩余缺口** — JLReq 第 3 章全部 80 条适用规则已 100% 实现并通过单元测试、真字体（Noto Serif CJK JP 横排+竖排）引擎测试及 W3C JLReq + LuaTeX-ja `jlreq.cls` 出版级 Ground Truth 逐图比对验证（8 条不适用项均为 EPUB/CSS 无对应结构或可选互斥方案）。

频次键见 `tools/jlreq/rows.py` 各行的 freq。
