# JLReq 规则级对照（新阅读器）

## 摘要

本表逐条对照 W3C《日本語組版処理の要件》(JLReq, 2020-08-11 WG Note) 第 3 章与新阅读器（twidget litehtml + oyomi 阅读器）。共 88 条：已实现 61、部分 10、缺失 9、不适用 8（第 2 批之前：已实现 53、部分 10、缺失 17）。第 1 批做了 3.1.5 ①/③ 段首括号与ルビ掛假名（3.3.8 规则 1、2）。第 2 批（标“本次”的行）做了行的调整处理：行末溢出时先按 3.8.3 的顺序压缩句读点、中点和括弧的空（追込み），压不进才推到下一行；扩展时不在括弧、句读点、中点旁加空，先扩欧文间隔、再扩和欧之间、再扩日文字间（3.8.4、3.1.11）；以及ルビ掛句读点和括弧（3.3.8 规则 3–6）。仍缺：欧文间隔与和欧四分的压缩、和欧间四分本身（3.2.6）、横排 ，．（3.1.1）、熟語ルビ；见“下一批候选”。

代码与测试的位置以 twidget 分支 alice/jlreq-line-adjust（main 3e0f21a + 第 2 批）为准。截图是 `reader-cli` 画的我们自己的小页面（`tools/jlreq/make_cases.py`，20px Noto Serif CJK JP，14 字一行，横排 -h、竖排 -v）。频次由 `tools/jlreq/corpus_scan.py` 统计：公共语料（w3c、idpf，及公有领域的 Aozora/Gutenberg 书：kumo、neko、rashomon、g1982、g31617、g34013）中的出现次数与书数；“K8 版权书”是 K8 上 4 本版权书只取总数（文字不离开 K8）。行头行尾类的频次用每行 20 字的等宽模拟估计。草枕在 idpf 里有完整版两份和预览一份，与视觉回归一样都计入。（截图为第 1 批之后；第 2 批各条的对照图在报告 v2。）

| 状态 | 条数 |
|---|---|
| 已实现 | 61 |
| 部分 | 10 |
| 缺失 | 9 |
| 不适用 | 8 |

## 对照表

| 条目 | 要求 | 状态 | 我们现在 | 代码 | 测试 | 截图 | 频次（公共 / K8） |
|---|---|---|---|---|---|---|---|
| 3.1.1 ① 竖排句读点用 、。 | 竖排的句点、逗点为 。、（字形位于字面右上）。 | 已实现 | 书中文字原样；竖排用字体的 vert 字形。 | `text/src/text_backend.cpp (vert/vrt2)` | vertical_text_tests.cpp: "with vertical forms, upright" | p-consec-v | — |
| 3.1.1 ② 横排 ，． 作为句读点 | 横排可用 ，．/，。/、。 三组之一；用 ，． 时它们与 、。 同属句读点（cl-06/07），后面二分空，可压缩。 | 部分 | ，．（U+FF0C/FF0E）在规则表里只有“不可行首”，不算 full_stop_comma：行末不去二分，后接「 时「 不减半，连续约物不压缩。 | `litehtml/src/cjk_rules_table.inc; tools/cjk_rules/overlay.json` | — | p-comma-period-h | 6874（2 本）/ K8 版权书 0 |
| 3.1.1 ③ 横排引号/方括号 | 横排可用 “ ” ‘ ’ 代替「」；〝〟只用于竖排；横排方括号用［］。 | 不适用 | 由书的文字决定，引擎不改字。 | `—` | — | — | — |
| 3.1.2 ①–⑤ 约物的基本配置 | 读点、句点、终わり括弧后二分空，始め括弧前二分空，中点前后四分空（与假名汉字相邻时整体看作全角）。 | 已实现 | 全角字形本身带这些空；压缩只在规则允许处去掉一半。 | `litehtml/src/cjk_rules.cpp:155 cjk::compression` | punctuation_tests.cpp: "？ and U+3000 stay full width" | p-consec-h | — |
| 3.1.3 汉数字中的 、・ 紧排 | 竖排里作位取り/概数的读点、汉数字间作小数点的中点，前后紧排（不留空）。 | 缺失 | 按普通读点/中点处理，留二分/四分空。 | `litehtml/src/cjk_rules.cpp:155` | — | — | 3（1 本）/ K8 版权书 17 |
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
| 3.1.6 ① ？！ 句末后全角空 | ？！ 用全角；作句末时前紧排、后空一字（接终わり括弧时紧排，括弧后二分）。 | 缺失 | 书里写了　（U+3000）就有空；没写时 ？！ 后直接接下一字。 | `litehtml/src/cjk_rules.cpp:155` | — | p-dividing-h | 31（4 本）/ K8 版权书 5 |
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
| 3.1.10 ⑦ モノルビ：亲字之间可断 | 一个亲字与其ルビ不分开；モノルビ的亲字与亲字之间可以断行。 | 部分 | 一个 <ruby> 里写多对（漢<rt>かん</rt>字<rt>じ</rt>）时整个 ruby 是一个 inline-block，亲字之间不能断行；每字一个 <ruby> 时可断。 | `litehtml/src/render_ruby.cpp:7 render_item_ruby::init` | cjk_rules_tests.cpp: "a line may break between a ruby and a kanji" | r-jukugo-v | 0（0 本）/ K8 版权书 1124 |
| 3.1.10 ⑧ グループルビ不分割；熟語ルビ按单位可断 | グループルビ整体不分开；熟語ルビ可在每个亲字的单位处断。 | 部分 | グループルビ不分开（是）；熟語ルビ不能断（同上）。 | `litehtml/src/render_ruby.cpp:7` | — | r-jukugo-v | 0（0 本）/ K8 版权书 1124 |
| 3.1.10 ⑨ 添え字不分割 | 亲字与上下标之间不断。 | 缺失 | 亲字与 <sup> 之间可断（数\|² 断开，v2 核对；Chromium 也断）。 | `litehtml/src/render_inline_context.cpp:191` | v2 例 s-sup | — | — |
| 3.1.10 ⑩–⑫ 合印、割注括弧、振分け | 合印前/内不断；cl-28 后、cl-29 前不断；振分け单位不断。 | 不适用 | EPUB 无这些结构。 | `—` | — | — | — |
| 3.1.11 ① 分割禁止处不加空 | 3.1.10 所列的不可分处，行调整时不加空。 | 已实现 | cjk::justifiable_gap：…… ―― 縦中横 拉丁单词内不加空。 | `litehtml/src/cjk_rules.cpp:141` | justify_tests.cpp: "the first line spread between its characters" | — | — |
| 3.1.11 ②-1 始め/终わり括弧前后不加空 **(第2批)**（本次；之前缺失） | 行调整（空け）时不在始め括弧、终わり括弧的前后加空。 | 已实现 | 扩展时不在日文始め/终わり括弧前后加空；只有全行除此之外没有可加空处时才照旧均分。 | `litehtml/src/cjk_rules.cpp justify_gap_kind; litehtml/src/line_box.cpp finish` | punctuation_tests.cpp: "3.1.11 ②-1: no space next to brackets" | j-justify-h | 4400（10 本）/ K8 版权书 3168 |
| 3.1.11 ②-2…②-6 句读点、中点、区切り約物、连字符、和字間隔前后不加空 **(第2批)**（本次；之前缺失） | 同上，对 cl-06/07、cl-05、cl-04、cl-03、cl-14。 | 已实现 | 同上：、。・：；、！？‼⁇、゠〜、U+3000 前后不加空（日文的；欧文的 ! ? ( ) 照旧）。 | `litehtml/src/cjk_rules.cpp justify_gap_kind` | punctuation_tests.cpp: "3.8.3: 。 inside a line keeps its half em; 3.1.11 ②: no space next to it" | j-justify-h | 4400（10 本）/ K8 版权书 3168 |
| 3.1.11 ②-7 熟語ルビ亲字间不加空 | 熟語ルビ的亲字之间不加空。 | 已实现 | ruby 是原子 inline-block。 | `litehtml/src/render_ruby.cpp` | — | — | — |
| 3.1.12 ② 追込み（先压缩） **(第2批)**（本次；之前缺失） | 行末溢出半字（如 、「）时，先把可压缩的二分减到四分，把字收进本行。 | 已实现 | 行末溢出不超过可压缩的量时先压缩（行末 」、。 二分、中点四分、行中括弧与读点二分，到ベタ），把字收进本行；压不进才推到下一行。 | `litehtml/src/line_box.cpp can_hold, squeeze_room, squeeze` | punctuation_tests.cpp: "3.8.3: JLReq Fig. 89 ③, 、「 and 」 squeezed to a quarter em" | k-kinsoku-h | 4400（10 本）/ K8 版权书 3168 |
| 3.1.12 ③ 追出し | 不能留在行末的字推到下一行，本行加空撑满。 | 已实现 | 是。 | `litehtml/src/line_box.cpp:981 can_hold` | cjk_rules_tests.cpp: "「 and a ruby that fit stay on the line" | k-kinsoku-v | — |
| 3.2.2 横排欧文：比例字体、欧文间隔 | 欧文用比例字体；欧文间隔约 1/3em，行首行末为 0。 | 已实现 | 字体的空格宽；行首行末空格按 CSS 折叠。 | `litehtml/src/line_box.cpp` | — | l-latin-h | — |
| 3.2.3 ① 竖排单个欧文字直立 | 单个字母/数字可一字一字直立。 | 已实现 | text-orientation: upright、全角字母直立。 | `litehtml/src/el_text.cpp (text-orientation)` | vertical_text_tests.cpp | t-tcy-v | — |
| 3.2.3 ② 竖排欧文单词顺时针转 90° | 单词、句子用比例字体横躺。 | 已实现 | text-orientation: mixed（默认）。 | `litehtml/src/el_text.cpp` | vertical_text_tests.cpp: "without, sideways" | t-tcy-v | 96（6 本）/ K8 版权书 20 |
| 3.2.3 ③/3.2.5 縦中横 | 两位数字等横排后居中放进一字；与前后字紧排，与括弧、句读点按二分规则。 | 已实现 | 自动：1–2 位 ASCII 数字、!? 等；CSS text-combine-upright。 | `litehtml/src/cjk_rules.cpp:170 tate_chu_yoko` | tate_chu_yoko_tests.cpp: "two digits side by side in one em", "three digits stay sideways" | t-tcy-v | 67（5 本）/ K8 版权书 8 |
| 3.2.4 全角欧文按汉字处理 | 全角拉丁字母、数字直立，按 cl-19 与假名汉字紧排。 | 已实现 | 是。 | `litehtml/src/cjk_rules.cpp:136 script_of` | — | l-latin-h | — |
| 3.2.6 ①③ 欧文单词不跨行、扩展时不在词内加空 | 欧文单词只在连字处断；行调整不在欧文单词、数字内加空。 | 已实现 | 是。 | `litehtml/src/cjk_rules.cpp:141` | justify_tests.cpp | — | — |
| 3.2.6 ② 调整时先动欧文间隔 **(第2批)** | 压缩和扩展都先用欧文间隔。 | 部分 | 扩展先用欧文间隔（每侧最多八分）；压缩不动欧文间隔（纯欧文书的断行不变）。 | `litehtml/src/line_box.cpp finish` | justify_tests.cpp: "the first line spread between its characters"; punctuation_tests.cpp: "3.8.4: word spaces first" | l-latin-h | 4400（10 本）/ K8 版权书 3168 |
| 3.2.6 ④ 和欧间四分空 | 假名汉字与欧文/数字之间空四分（行首行末不空；括弧、句读点旁按 4.1–4.3）。 | 缺失 | 不加（没有 text-autospace）。 | `—` | — | l-latin-h | 4600（11 本）/ K8 版权书 55 |
| 3.3.3 ルビ字号为亲字一半 | ルビ原则上为亲字的一半大小。 | 已实现 | rt { font-size: 50% }。 | `litehtml/include/litehtml/master_css.h:410` | core_tests.cpp: "ruby: the annotation over the base" | r-mono-v | — |
| 3.3.4 ルビ位置 | 竖排在右、横排在上；左/下少见。 | 部分 | 右/上（是）；不支持 ruby-position（左/下）。 | `litehtml/include/litehtml/render_ruby.h:12` | core_tests.cpp: "ruby: the annotation over the base" | r-mono-v | — |
| 3.3.5 モノルビ：2 假名等长，1 假名中付き | 2 个假名与亲字等长；1 个假名中付き（或竖排肩付き）。 | 已实现 | 居中（中付き）；不做肩付き（可选）。 | `litehtml/src/render_ruby.cpp:96 space_around` | core_tests.cpp: "ruby: the annotation widens the pair, the base centred in it" | r-mono-v | 3982（3 本）/ K8 版权书 574 |
| 3.3.5 ①② モノルビ 3 假名以上：居中并掛け **(第1批)** | 3 个以上假名的ルビ紧排、与亲字中心对齐，超出部分掛在前后字上（按 3.3.8）；肩付き时默认掛在后面。 | 已实现 | ルビ居中；超出部分掛到相邻的假名上（每侧最多一个ルビ字），不能掛的一侧空开亲字。 | `litehtml/src/line_box.cpp ruby_overlap; litehtml/src/render_ruby.cpp base_start/base_end` | ruby_overhang_tests.cpp: "rule 2: の旬に" | r-overhang-h | 2888（3 本）/ K8 版权书 739 |
| 3.3.6 グループルビ：长短不同时 1:2:1 | ルビ短：ルビ字间 2、两端各 1；ルビ长：亲字间 2、两端各 1（JIS X 4051）。 | 已实现 | ruby-align: space-around，正是 1:2:1。 | `litehtml/src/render_ruby.cpp:96; litehtml/src/render_inline_context.cpp:472` | core_tests.cpp: "ruby: the annotation widens the pair, the base centred in it" | r-group-v | — |
| 3.3.6 (注) 短ルビ两端空的上限 | ルビ比亲字短很多时，两端空不超过一个ルビ字（或 1.5 倍）。 | 缺失 | 两端空不设上限。 | `litehtml/src/render_inline_context.cpp:472` | — | r-group-v | — |
| 3.3.7 熟語ルビ：每字 ≤2 假名按モノルビ | 每个亲字的ルビ不超过 2 字时，逐字按モノルビ配置。 | 已实现 | 每对 rt 各成一对，居中。 | `litehtml/src/render_ruby.cpp:7` | — | r-jukugo-v | 0（0 本）/ K8 版权书 1124 |
| 3.3.7 熟語ルビ：某字 ≥3 假名 | 按グループルビ配置，或让ルビ掛到同一熟語的其他亲字上（不超过一个ルビ字）。 | 部分 | 该对单独加宽，亲字被拉开（承る 的 承 两侧大空）。 | `litehtml/src/render_ruby.cpp:96` | — | r-jukugo-v | 0（0 本）/ K8 版权书 166 |
| 3.3.7 熟語ルビ可在单位间断行 | 熟語ルビ可在亲字单位处分到两行（分开后按 3.3.8 h）。 | 缺失 | 整个 ruby 不能断（见 3.1.10-7）。 | `litehtml/src/render_ruby.cpp:7` | — | r-jukugo-v | 0（0 本）/ K8 版权书 1124 |
| 3.3.8 (总则) ルビ短于亲字：亲字与前后紧排 | ルビ不长于亲字时，亲字与前后字紧排。 | 已实现 | 是。 | `litehtml/src/render_ruby.cpp:96` | core_tests.cpp: Ruby | r-mono-v | — |
| 3.3.8 规则1 不掛汉字 **(第1批)** | ルビ不掛到相邻的汉字（cl-19）上；不能掛时空开亲字。 | 已实现 | 不掛汉字（也不掛标点、空格），该侧空开亲字。 | `litehtml/src/line_box.cpp ruby_overlap` | ruby_overhang_tests.cpp: "rule 1: 後旬又" | r-overhang-h | 237（3 本）/ K8 版权书 70 |
| 3.3.8 规则2 掛平假名、片假名、ー、小书き **(第1批)** | 相邻为平假名、片假名、长音、小书き假名时，ルビ可掛上去，最多一个ルビ字（亲字的一半）。 | 已实现 | 掛平假名、片假名、ー、小书き假名，每侧最多一个ルビ字，且不超过该侧伸出的量；行宽按掛后的宽度断行。 | `litehtml/src/cjk_rules.cpp ruby_may_overhang; litehtml/src/line_box.cpp ruby_overlap` | ruby_overhang_tests.cpp: "rule 2: …" 6 例, "the line holds what the overhang makes room for" | r-overhang-h | 2800（3 本）/ K8 版权书 662 |
| 3.3.8 规则2 注 相邻两个ルビ | 前后两组ルビ相邻时不能互相掛（会相碰）；可空一字或限制掛的量。 | 已实现 | ルビ不掛相邻的ルビ（两组ルビ之间照旧空开）。 | `litehtml/src/line_box.cpp ruby_overlap` | ruby_overhang_tests.cpp: "rule 2 note: a ruby does not overhang the ruby next to it" | r-adjacent-h | 165（3 本）/ K8 版权书 24 |
| 3.3.8 规则3 掛句读点/括弧的二分空 **(第2批)**（本次；之前缺失） | 前面是 」、。 的后二分、后面是「 的前二分时，ルビ可掛进这个空（最多一个ルビ字；空被压缩时以剩余量为限）。 | 已实现 | 掛进 」、。 后、「 前的二分空，最多一个ルビ字，不超过压缩后剩下的空；被ルビ掛住的空不再被行调整压缩。 | `litehtml/src/line_box.cpp overhang_room` | ruby_overhang_tests.cpp: "rule 3: into the half em after 」", "rule 3: not what the ruby overhangs" | r-adjacent-h | 722（3 本）/ K8 版权书 138 |
| 3.3.8 规则4/5 掛分離禁止文字、中点 **(第2批)**（本次；之前缺失） | 相邻为 ……―― 或中点时可掛，最多一个ルビ字（中点四分被压缩时有上限）。 | 已实现 | 掛 …… ―― 最多一个ルビ字；掛中点：其该侧四分加半个ルビ字，最多一个ルビ字。 | `litehtml/src/line_box.cpp overhang_room` | ruby_overhang_tests.cpp: "rule 4: onto ……", "rule 5: onto middle dots" | r-adjacent-h | — |
| 3.3.8 规则6 掛终わり括弧、句读点、始め括弧本身 **(第2批)**（本次；之前缺失） | 可掛到相邻的 」、。（后侧）或「（前侧）上，最多一个ルビ字，但不越过该字。 | 已实现 | 掛前面的「、后面的 」、。 的字面，最多一个ルビ字、不越过该字（按 JLReq 正文；注中“不掛「」”的做法未采用）。 | `litehtml/src/line_box.cpp overhang_room` | ruby_overhang_tests.cpp: "rule 6: onto 「 before and 」 after", "rule 6: no further than the mark itself" | r-adjacent-h | 722（3 本）/ K8 版权书 138 |
| 3.3.8 行头行尾 (总则) ルビ不超出版面 **(第1批)** | 行头行尾的ルビ不能伸出版面/栏。 | 已实现 | 行头行尾不向外掛，ルビ不超出行。 | `litehtml/src/line_box.cpp` | ruby_overhang_tests.cpp: line-edge 4 例 | r-edge-v | — |
| 3.3.8 行头行尾 a/b モノルビ在行头/行尾 **(第1批)** | 行头：ルビ顶与亲字顶对齐（超出部分掛向后字）；行尾：ルビ底与亲字底对齐。 | 已实现 | 行头：亲字与ルビ同时贴行头，超出部分掛向后面的假名；折行行尾：亲字与ルビ同时贴行尾，掛向前面的假名（各最多一个ルビ字）。 | `litehtml/src/render_ruby.cpp align_base; litehtml/src/line_box.cpp add_item, finish` | ruby_overhang_tests.cpp: "line-edge a: …", "line-edge b: …" | r-edge-v | 172（2 本）/ K8 版权书 48 |
| 3.3.8 行头行尾 c/d グループルビ在行头/行尾 | 行头：ルビ顶对第一个亲字，亲字间与末端空 1:1；行尾反之。 | 部分 | 仍按 1:2:1，第一个亲字不贴行头。 | `litehtml/src/render_inline_context.cpp:472` | — | r-edge-v | 172（2 本）/ K8 版权书 48 |
| 3.3.8 行头行尾 e–h 熟語ルビ在行头/行尾、跨行 | 按 c/d；或亲字贴行头、ルビ掛同词亲字；跨行时分成モノ/熟語两部分。 | 缺失 | 无熟語ルビ处理。 | `litehtml/src/render_ruby.cpp` | — | r-jukugo-v | — |
| 3.3.9 ①② 圏点：亲字一半大小，右/上居中 | 圏点为亲字一半大，竖排在右、横排在上，居中。 | 已实现 | text-emphasis：位置、居中。 | `litehtml/src/el_text.cpp:133 draw_emphasis` | engine_tests.cpp (vertical-rl ruby + sesame chapter) | e-dots-v | 1140（4 本）/ K8 版权书 1078 |
| 3.3.9 ③ 圏点字形：竖排﹅、横排• | 通常竖排用 ﹅，横排用 •。 | 已实现 | 按 CSS text-emphasis-style（书决定）。 | `litehtml/src/el_text.cpp:99 emphasis_mark` | — | e-dots-h | — |
| 3.3.9 (注) 句读点、括弧不加圏点 | 实践中 、。「」 不加圏点。 | 已实现 | 标点（Unicode P 类）和空格不加。 | `litehtml/src/el_text.cpp:88 emphasis_skips` | — | e-dots-v | — |
| 3.4.1–3.4.3 割注 | 割注两行排、括弧、跨行。 | 不适用 | EPUB/CSS 没有割注结构。 | `—` | — | — | — |
| 3.5.1 ①–③ 段首字下げ | 段首一字下げ（或全部天ツキ、标题后天ツキ）。 | 已实现 | text-indent 与 U+3000 照排。 | `litehtml/src/render_inline_context.cpp (text-indent)` | — | i-indent-h | 278（3 本）/ K8 版权书 7335 |
| 3.5.2 行头/行尾 indent | 引文、标题按固定量缩进。 | 已实现 | margin/padding。 | `litehtml` | — | i-indent-h | — |
| 3.5.3 ①–③ 中央/行头/行尾揃え | 单行的居中、左/上、右/下对齐。 | 已实现 | text-align。 | `litehtml/src/line_box.cpp:410` | — | — | — |
| 3.5.3 ④ 均等揃え | 单行字距均分撑满（俳句等）。 | 缺失 | 不支持 text-align-last / text-align: justify-all。 | `litehtml/src/line_box.cpp:410` | — | — | — |
| 3.5.4 段末行字数过少 | 避免段末行字太少（日文优先级低）。 | 缺失 | 不处理。 | `—` | — | — | — |
| 3.5.4 页末孤行 | 避免页面只剩一行。 | 已实现 | orphans/widows 默认 2（同 Blink 分栏）。 | `reader: native/src/chapter.cpp:947` | reader engine_tests.cpp: "widows: two lines go together to the next page" | — | — |
| 3.6 タブ | 制表位对齐。 | 不适用 | EPUB 无。 | `—` | — | — | — |
| 3.7.1 添え字 | 约亲字 60%，紧排，不断开，竖排横躺或縦中横。 | 部分 | sub/sup 用 font-size: smaller（约 83%）；不断开未保证。 | `litehtml/include/litehtml/master_css.h:382` | — | — | — |
| 3.7.2–3.7.4 振分け、字取り、数式 | 振分け块、字取り、数式符号的空。 | 不适用 | EPUB 无此结构（数式走 MathML）。 | `—` | — | — | — |
| 3.8.2 ①/3.8.3 先压缩（追込み）的优先顺序 **(第2批)**（本次；之前缺失） | 先压：欧文间隔（到四分）→行末 」、。 二分→行末中点四分→行中中点四分→行中括弧、读点二分→和欧四分（到八分）。 | 部分 | 句读点、中点、括弧部分按 3.8.3 的顺序，每步均分（句点二分在行中不动，段首括号不动）；欧文间隔与和欧间不压缩。 | `litehtml/src/line_box.cpp squeeze` | punctuation_tests.cpp: "3.8.3: a middle dot's quarters before brackets", "then the half em before 「" | k-kinsoku-h | 4400（10 本）/ K8 版权书 3168 |
| 3.8.2 ② 压不进时再扩展（追出し） **(第2批)** | 不能压缩时再扩展。 | 已实现 | 能压缩时先压缩，压不进才追い出し，再扩展。 | `litehtml/src/line_box.cpp can_hold` | punctuation_tests.cpp: "3.8.2: JLReq Fig. 89 ⑤, 「 too long to squeeze in goes on" | k-kinsoku-h | — |
| 3.8.2 (注) ぶら下げ | 句读点可伸出行末（可选，JIS 未正式规定）。 | 不适用 | 不做（规则表有 hanging 类，未用）。 | `litehtml/src/cjk_rules_table.inc` | — | — | — |
| 3.8.4 a 扩展先用欧文间隔（≤二分） **(第2批)**（本次；之前部分） | 先均分到欧文间隔，每个最多二分。 | 已实现 | 先扩欧文间隔，每个间隔两侧各最多八分（间隔本身约四分，合计约二分）。 | `litehtml/src/line_box.cpp finish` | punctuation_tests.cpp: "3.8.4: word spaces first" | l-latin-h | 4400（10 本）/ K8 版权书 3168 |
| 3.8.4 b 再扩和欧间（≤二分） **(第2批)**（本次；之前缺失） | 然后扩和欧间的四分。 | 部分 | 和欧之间第二个扩（每处最多二分）；但没有基本的和欧四分（见 3.2.6-4）。 | `litehtml/src/line_box.cpp finish` | punctuation_tests.cpp: "3.8.4: Japanese–Latin gaps before kana ones" | l-latin-h | 4600（11 本）/ K8 版权书 55 |
| 3.8.4 c 再扩其他允许处（≤四分） **(第2批)**（本次；之前部分） | 其他不禁止处，每处最多四分。 | 已实现 | 其余日文字间每处最多四分，括弧、句读点、中点旁不加（3.1.11 ②）。 | `litehtml/src/line_box.cpp finish` | punctuation_tests.cpp: "。 ending a wrapped line" | j-justify-h | 4400（10 本）/ K8 版权书 3168 |
| 3.8.4 d 最后均分到所有字间 **(第2批)** | 以上不够时，均分到所有非分離禁止的字间。 | 已实现 | 仍不够时均分到以上所有可加空处（不含括弧、句读点旁）。 | `litehtml/src/line_box.cpp finish` | punctuation_tests.cpp: "3.1.11 ②-1: no space next to brackets" | j-justify-h | — |
| 3.9 / 附录 A 字符类 cl-01…cl-30 | 按字符类决定行头行尾、空、可否断开。 | 已实现 | 规则表由 stone_engine_js 生成并加 overlay；cl-20…25、28/29 等结构类无对应。 | `litehtml/src/cjk_rules_table.inc; tools/cjk_rules/` | cjk_rules_tests.cpp: classify table | — | — |

## 下一批候选（按公共语料出现次数）

1. 3.1.1 ②：横排 ，． 作为句读点 — 6874（2 本）/ K8 版权书 0
2. 3.2.6 ④、3.8.4 b：和欧间四分空；再扩和欧间（≤二分） — 4600（11 本）/ K8 版权书 55
3. 3.2.6 ②、3.8.2 ①/3.8.3：调整时先动欧文间隔；先压缩（追込み）的优先顺序 — 4400（10 本）/ K8 版权书 3168
4. 3.3.8 行头行尾 c/d：グループルビ在行头/行尾 — 172（2 本）/ K8 版权书 48
5. 3.1.6 ①：？！ 句末后全角空 — 31（4 本）/ K8 版权书 5
6. 3.1.3：汉数字中的 、・ 紧排 — 3（1 本）/ K8 版权书 17
7. 3.1.10 ⑦、3.1.10 ⑧、3.3.7：モノルビ：亲字之间可断；グループルビ不分割；熟語ルビ按单位可断；熟語ルビ可在单位间断行 — 0（0 本）/ K8 版权书 1124
8. 3.3.7：熟語ルビ：某字 ≥3 假名 — 0（0 本）/ K8 版权书 166
9. 3.1.10 ⑨：添え字不分割 — 未量化
10. 3.3.4：ルビ位置 — 未量化
11. 3.3.6 (注)：短ルビ两端空的上限 — 未量化
12. 3.3.8 行头行尾 e–h：熟語ルビ在行头/行尾、跨行 — 未量化
13. 3.5.3 ④：均等揃え — 未量化
14. 3.5.4：段末行字数过少 — 未量化
15. 3.7.1：添え字 — 未量化

频次键见 `tools/jlreq/rows.py` 各行的 freq；表由 `tools/jlreq/gen_report.py` 生成。
