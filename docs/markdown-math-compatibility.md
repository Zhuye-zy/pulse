# Markdown 数学兼容范围与独立参考

Pulse 的目标是可预测、有界、可回退的原生数学预览，不是完整 LaTeX 实现。下文的“完整支持”仅指明确列出的有限语法合同；不表示与 TeX、KaTeX 的所有布局规则或像素完全一致。一个命令能解析、完整公式能排版、字形正确、视觉接近参考，是四件需要分别验证的事。

## 固定的独立基准

开发参考固定为 **KaTeX 0.16.22**，来自[官方 npm 版本元数据](https://registry.npmjs.org/katex/0.16.22)和[对应上游发布](https://github.com/KaTeX/KaTeX/releases/tag/v0.16.22)。版本、归档地址、SHA-512 完整性、[MIT 许可证来源](https://github.com/KaTeX/KaTeX/blob/v0.16.22/LICENSE)及选项均记录在 `tools/math_reference/manifest.json`。固定旧版本是为了复现比较，不是宣称该版本最新。

KaTeX 仅在开发工具中执行。下载的源码、CSS、字体及许可证留在已忽略的 `build/math_reference_cache/`，不进入 Pulse 运行时、安装包或原生构建依赖。没有 npm 生命周期脚本或全局安装。参考调用使用 `renderToString`，分别生成 MathML 和 HTML+MathML；`throwOnError=true`、`strict=error`、`trust=false`，每次调用使用独立宏状态。选项意义见[KaTeX 官方选项文档](https://katex.org/docs/options.html)。

实际运行环境：Windows，Node.js v24.19.0。当前原创语料扩展为 **132 条**：保留原有 83 条 ID 与源码，追加 49 条宏／字体组合公式、作用域错误及预算边界；92 条原生接受合同、19 条明确原生回退、21 条非法输入。KaTeX 已实际离线接受 109 条、拒绝 23 条，后者包括 21 条非法输入和 2 条明确的参考实现局限。逐条结果及精确语料哈希以 `reference-results.tsv`、`reference-summary.json` 为准，完整 MathML、HTML 和错误文本位于缓存。两个可选首参默认值正例仍是原生接受合同，不伪造参考成功。

这些数字仅描述这套固定语料，不是“KaTeX 兼容率”。参考接受而原生有意回退的案例是已记录功能边界，不能当成原生通过。

历史架构里程碑曾实际对照全部 44 条：28 条双方接受、8 条原生按合同回退、8 条双方拒绝，分类与当时三类可比较结构计数无差异。该语料、参考快照和实际原生比较完整冻结在 `tools/math_reference/history/architecture-44/`。其中 `underbrace_derivation`、`explicit_spacing`、`phantom_alignment`、`array_vertical_rules` 四条仅在本轮解析和排版分支实现后改为接受合同，未重写历史结果。

上一结构里程碑的 83 条原生报告实际结果为 57 条双方接受、12 条预期原生回退、14 条双方拒绝，分类、ID、尺寸及五类可比结构计数均无差异；完整记录冻结在 `tools/math_reference/history/structures-83/`。本轮只在局部宏和字体分支实现后将 `macro_definition`、`calligraphic_space` 改为接受合同，保留历史原义。

本轮已实际读取最终作用域修复后的 132 条原生报告并运行比较：90 条双方接受、2 条原生接受但固定参考受限、19 条预期原生回退、21 条双方拒绝。ID 覆盖、尺寸和已知合同之外的问题均为零；90 条双方接受式的五类可比结构计数一致。`markdown-math-compatibility.json` 保存逐例实际结果和输入哈希。两个参考受限案例没有参考结构一致结论；最终构建与视觉检查的范围另列于下文，不由比较计数推断。

固定 KaTeX 0.16.22 的[上游源码](https://github.com/KaTeX/KaTeX/blob/v0.16.22/src/macros.js#L148)明确将 `newcommand` 的可选首参默认值列为未实现。实际调用拒绝 `macro_optional_root` 和 `macro_empty_optional_default`，但这两个输入仍是原生必须支持的合同。`reference-limitations.json` 用精确 ID、源码及说明限定此差异；比较结果独立标记 `native_accept_reference_limited`，没有这些输入的参考 MathML 结构通过结论。相反，固定 KaTeX 实际接受了十参数定义，原生最多九参数，因此该例按 `native_fallback` 处理，不能误写成双方拒绝。

## 完整支持：限定的语法合同

此表描述需要完整接受的有限语法组合；具体原生运行证据以 `--compat` 报告和 `compare.mjs` 比较结果为准。

| 类别 | 明确的支持合同 | 完整公式语料例 |
| --- | --- | --- |
| 分组与脚标 | 花括号分组、一个上标和一个下标、撇号导数记号及嵌套脚标；重复脚标拒绝 | `second_derivative`、`tensor_indices`、`root_and_scripts` |
| 分数与二项式 | `frac/dfrac/tfrac`、`binom/dbinom/tbinom`；完整递归分子、分母 | `quadratic_roots`、`binomial_probability`、`nested_continued_fraction` |
| 根号 | `sqrt{…}` 和 `sqrt[指数]{…}`；根式可含脚标和分数 | `normal_density`、`root_and_scripts` |
| 样式作用域 | display/text/script/scriptscript 样式；局部分组退出后恢复；`mathrm/mathbf/mathit` | `gradient_descent`、`inline_style_fraction`、`nested_style_scope` |
| 成对括号 | `left/middle/right`、多个直属 `middle`、嵌套括号、隐形点分隔符和固定大小 `big/Big/bigg/Bigg` 系列；缺失配对拒绝 | `multiple_middle_fences`、`nested_middle_fences`、`annotated_limit` |
| 矩阵与多行 | 既有矩阵、`cases/aligned/gathered`；`array` 的 l/c/r 列、单／双竖线、行边界单／双 `hline` 及有界额外行距 | `ruled_augmented_system`、`double_rule_table`、`array_row_gap_ex_pt`、`ruled_nested_matrix` |
| 运算符与上下限 | 已实现的三角/对数等命名运算符；求和、乘积、积分；`limits/nolimits`；`operatorname`、带星版本、`operatornamewithlimits`；`substack` 的单列内容 | `geometric_series`、`fourier_transform`、`constrained_sum`、`annotated_limit` |
| 简单文本 | `text/textrm/textbf` 的简单文本参数；空格保留，非任意嵌套文本 TeX | `text_with_spaces`、`convergence_arrow` |
| 注释与修饰 | `overset/underset`；横线、重音、帽子、波浪、点及向量方向标注的已实现命令 | `annotated_equality`、`vector_accents`、`sample_mean` |
| 框线、消去线、伸缩箭头 | `boxed`、`cancel/bcancel/xcancel`、`xrightarrow/xleftarrow` 的上下标签 | `boxed_energy`、`cancelled_ratio`、`convergence_arrow` |
| 大括弧与标签 | `overbrace/underbrace`；对应侧标签堆叠，另一侧为普通脚标，正文和标签可嵌套结构 | `brace_opposite_scripts`、`nested_brace_labels`、`underbrace_fraction_sum` |
| 隐形尺寸 | `phantom/hphantom/vphantom` 保留指定轴尺寸、不绘制正文；允许明确的纯隐形公式 | `phantom_fraction_alignment`、`horizontal_phantom_spacing`、`vertical_phantom_height`、`pure_vertical_phantom` |
| 显式长度 | 有符号 `hspace`：绝对值不超过 20em/40ex/200pt；额外行距同上限且非负 | `signed_hspace_equation`、`mixed_unit_hspace`、`hspace_boundary_em/ex/pt` |
| 局部宏 | `newcommand/renewcommand/providecommand`；0–9 参数、可选首参默认值、token／分组实参、嵌套展开、分组／环境单元／substack 行局部作用域及公式隔离 | `macro_nine_argument_matrix`、`macro_optional_root`、`macro_nested_arguments`、`macro_group_renewal`、`macro_environment_local_renewal` |
| 常用数学字体 | `mathcal/mathscr/mathfrak/mathsf/mathtt`、`boldsymbol/bm`；嵌套样式及简单文本隔离 | `font_calligraphic_functional`、`font_script_transform`、`font_fraktur_algebra`、`font_boldsymbol_vectors`、`font_text_not_mapped` |
| 基础符号与间距 | 明确实现的希腊字母、关系、集合、箭头符号；`pmod/bmod`；命名正/负间距 | `bayes_rule`、`modular_equation` |

“已实现的符号”是有限命令表，不是所有 Unicode 数学符号或所有 KaTeX 别名。未列入解析器的命令必须保持源码回退，不能丢掉命令后继续伪装成成功。

## 部分支持与排版边界

| 范围 | 已提供 | 仍有的边界 |
| --- | --- | --- |
| Markdown 嵌入 | `$…$`、`$$…$$`、`\(…\)`、`\[…\]`；列表、表格、强调和完整链接标签内公式 | 不跨空段落、表格单元或链接边界；代码、HTML、图片不作为公式。反斜杠候选遇已识别美元数学或显式引用续行 `>` 时保守回退。独立展示式居中，段落内展示标记保留段落语境。 |
| TeX 原子与样式排版 | 原子分类、样式状态和原生布局；覆盖语料中的组合 | 不是完整 TeX 算法；完整公式的间距、嵌套装饰和极端尺寸仍需视觉检查。接受或计数一致不能证明排版等价。 |
| 字体数学度量 | 使用系统 Cambria Math；架构读取可用 OpenType MATH 常量并保留有界回退 | 不包含完整伸缩字形拼接、所有字形变体、数学 kern 和设备修正；不打包数学字体，系统缺字或不可用时应回退。 |
| 字体族 | 普通、直立、粗体、斜体、有限双线体、Script／Fraktur／Sans／Mono 和粗数学字母 | `mathcal/mathscr` 共用系统 Unicode Script 家族，不是 KaTeX 两套相同字形；无变体的数字／非拉丁字符保留，简单文本不映射。缺字检查不等于完整字体族覆盖。 |
| 宏 | 有界的公式局部定义、更新、提供默认定义与参数展开 | 不实现 TeX 通用原语、跨公式宏或内建命令重定义；宏展开是解析前处理，错误仍映射原始源码。 |
| 文本模式 | 简单文本及已实现样式 | 不实现任意嵌套文本命令、完整 TeX 文本/宏模式和所有注释规则。 |
| 数组 | l/c/r 对齐、每边界至多两条竖／横规则线、非负有界额外行距 | 不支持 `cline/multicolumn`、p 列、`@{…}`、三重规则线、负行距和复杂列修饰；跨页、编号、多公式引用不支持。 |
| 中间分隔符 | 对应 `left/right` 直属内容中的多个 `middle` 和独立嵌套 | 不跨分组、命令参数或数组单元捕获外围作用域。 |
| 大括弧与隐形盒 | 原生大括弧及标签布局；指定轴隐形尺寸 | 不是字形拼接或全部 TeX 盒子语言；无墨迹不等于不存在，需独立尺寸与绘制验证。 |
| 省略号及标注 | 已实现点、重音、箭头和横线 | `dots` 不保证 TeX 的上下文自适应；装饰不等于字体原生 MATH 拼接。 |
| 超宽公式 | 有限缩小后保留源码回退 | 不提供任意自动数学断行；不能以无限缩小代替可读性。 |

## 未支持：应明确回退

| 范围 | 参考语料中的例子 | 预期 |
| --- | --- | --- |
| 通用宏原语与内建覆盖 | `def`、`let`、`renewcommand{\sin}` | 固定参考可接受，原生明确回退；不提供 `gdef/global/catcode`、包或文件能力 |
| 超宏预算 | 65 条定义、257 次调用、33 层展开链、原始／展开后超过 4096 单元，或十个参数 | 固定参考可接受，原生明确回退；不当作双方非法语法 |
| 超预算或其他单位的任意长度 | `hspace{20.01em}`、`hspace{40.01ex}`、`hspace{200.01pt}` | KaTeX 接受但超出原生预算，明确回退；不是非法 TeX |
| 颜色、HTML、图片和外部资源命令 | `color`；外部资源不由原生数学加载 | 不悄悄忽略样式后当成功 |
| 公式编号、标签与交叉引用 | `tag` | KaTeX 接受，原生保留源码 |
| 纯间距公式 | 单独 `hspace{2em}` | KaTeX 接受，原生合同仍为源码回退；与明确 phantom 隐形语义区分 |
| 超出数组子集 | 三重竖／横规则线、负额外行距、超长度预算 | KaTeX 接受，原生明确回退；`cline/multicolumn/p/@` 等也未实现 |
| 文档级 TeX 与扩展包 | 文档类、包加载、任意环境、跨公式宏状态等 | 不执行，不宣称完整兼容 |

非法输入另列 `invalid`，包括既有结构错误、宏重定义、更新未定义宏、参数缺失或非法引用、越出局部作用域和跨公式引用不存在的宏。错误应有原始位置与分类，原生排版不得产生残缺公式。超过原生预算而固定参考可接受的输入列为 `native_fallback`，可选首参默认值则明确列为固定参考限制，不能混为非法语法。

## 预算、复现与证据等级

预览预算为每文档最多 256 式、总 65536 个 UTF-16 单元、每式最多 4096 个单元（预览范围含分隔符）。解析器另限制 1024 个源码原子、4096 个构造节点与最终语法树节点（含空占位）、32 层解析嵌套、128 个矩阵单元；每环境最多 32 行、16 列。超限保留可读源码。源码原子和内部包装分开计数，以保留迁移前的公式容量。基准工具本身不取代这些预算测试。

宏预算为原始／展开后各 4096 个 UTF-16 单元、累计 64 条定义指令、256 次调用及 32 层展开链；无操作 `providecommand` 和 `renewcommand` 同样计入定义次数。八个确定性预算案例由 `generate-macro-budget-cases.mjs` 生成，分别覆盖限内与超限，不调用原生实现生成“期望”。固定参考为每个语料及每次 MathML／HTML 输出创建独立宏环境；相邻“本式定义成功／下一式未定义拒绝”案例独立验证了参考状态隔离。

```powershell
node tools/math_reference/run.mjs
node tools/math_reference/run.mjs --offline
# 以下构建在 x64 Visual Studio 开发者终端中执行
cmake --build build --target pulse_math_syntax_test pulse_math_test
.\build\pulse_math_syntax_test.exe
.\build\pulse_math_test.exe --architecture --render
# 原生测试输出 build/math-native-compat.tsv，再做独立比较
node tools/math_reference/compare.mjs
```

比较器真实读取原生 TSV 和固定参考 TSV，检查每条 ID/语料一致性、接受/回退/拒绝分类以及原生有效尺寸。自动结构对照为 `Fraction ↔ mfrac`、`Radical ↔ msqrt+mroot`、`Environment ↔ mtable`、`Phantom ↔ mphantom`，以及从真实 MathML 的伸缩 ⏞/⏟ 运算符识别的 `Brace`。上/下标、重音、标签和运算符上下限共用 MathML 标签，不能仅凭 `mover/munder` 数量武断等同；中间分隔符、规则线和长度尺寸由原生专项检查补充，不能宣称已做一对一参考等价。

宏案例比较展开后的实际结构，绝不把宏调用名计成公式节点。`font_variants` 记录参考 MathML 的字体属性，仅作信息展示；没有把相同属性计数当作字形、字体映射或视觉等价。已知固定参考不支持的两个默认参数案例只检查原生合同与真实参考拒绝，明确缺少独立 MathML 结构对照。

另有不改变 132 条主语料的独立语义探针：`tools/math_reference/local-scope-probe.mjs` 读取同一固定 KaTeX 0.16.22 开发缓存，实际结果记录在 `local-scope-results.json`，包含版本、完整输入、MathML 和错误文本。矩阵中局部更新的结果为 `[b,1;0,a]+a`，`substack` 内局部更新的两行加外层变量为 `b,a,a`；仅在 `substack` 第一行新定义的宏到下一行应未定义并拒绝。该探针发现了单纯结构计数无法识别的宏值泄漏，原生必须独立验证逐单元／逐行作用域，而不是以分数或环境计数相同替代语义验证。脚本完全离线，仍使用清单记录的官方 npm 来源及固定版本。

尺寸检查仅对 `metrics-exceptions.json` 中精确匹配 ID 与源码的 `pure_vertical_phantom` 允许零宽，仍要求高度有效、数值有限和基线在合法范围。没有将全部宽高校验放宽。结构差异输出为需审查项，不伪造视觉通过。

离线参考页为 `build/math_reference_cache/reference.html`，CSS 和 60 个引用字体文件已通过实际本地存在性检查，无 CDN 依赖。当前浏览器安全策略拒绝读取该本地页面，因此**尚未进行参考页浏览器截图验收**；没有以服务器或改写 URL 绕过。参考证据为真实 KaTeX 执行、MathML 和结构报告。原生截图、DPI/主题/窄布局及选择交互需要独立证据。

工具与字段说明见 `tools/math_reference/README.md`。本清单不以单个编译成功、参考接受成功或标签计数相同替代功能和视觉验证。

## 本轮局部宏与字体里程碑验证（132 条语料）

- 最终矩阵单元和 `substack` 行作用域修复后，`pulse_math_syntax_test`、`pulse_math_test`、`pulse` 三目标构建成功，无编译警告；独立语法、`--reliable` 逻辑、第一至第五轮功能、`--structures` 逻辑及 `--compat` 共九组检查均零失败，132 条合同全部通过。旧轮次仅第二轮重跑 `--render`；其深色 200% 图已目视，无缺字、裁剪或非预期重叠，负间距案例的刻意重叠保留。未把其余轮次未重跑的截图算成本轮验证。
- 独立 KaTeX 语义探针暴露了宏更新在单元之间泄漏的问题；原生已修复矩阵单元和 `substack` 行作用域，并增加宏值／几何等价及下一行未定义宏拒绝的专项断言。UTF-16 补充字符作为未分组单参数的边界也已修复并覆盖。这些语义验证独立于结构计数。
- 最终 `compare.mjs --update-snapshot` 实际读取最新原生报告，结果为 90 条双方接受、2 条原生接受但参考受限、19 条预期回退、21 条双方拒绝；无额外 ID、分类、尺寸或五类可比结构问题。原生输入 SHA-256 为 `69f4f149dd0fb2239628f2011c734daadde47f461d50b6c1542e6f35e9e172dc`。两条可选首参默认值仍明确缺少独立参考结构验收。
- 本轮较早已运行 `--architecture` 的 132 条合同并通过；后续宏边界和作用域修复以最终独立语法、专项功能及 `--compat` 覆盖，没有声称再次运行架构截图。
- 18 个新完整场景的浅／深色与 100%／150%／200% 离屏图在首次实现完成时实际生成，边界检查全部通过。已目视公式浅色 100%、深色 200% 及实际 MarkdownView 深色 100% 宽窄双栏，18 个场景完整，无缺字、裁剪或重叠。后续仅宏边界和作用域修复，未重新生成这组 18×6 截图。
- 固定 KaTeX 0.16.22 的 132 条参考与独立作用域探针均实际离线运行；每次使用独立宏环境。参考页仍因浏览器安全限制未截图，未使用服务器或其他 URL 绕过；没有逐像素等价或真实窗口交互验收结论。

## 上一结构里程碑历史验证（83 条语料）

- 最终 `pulse` 和 `pulse_math_test` 构建成功，无编译警告；日志为 `build/math-structures-final-build.log`。
- 独立语法测试通过；`--structures --render` 全部通过，包括数组右外边界与横线交接的新增断言。首次目视发现右外竖线内缩后已修正，再构建并重跑受影响检查，未以原先通过的尺寸或结构计数掩盖视觉缺陷。
- `--architecture` 的 83 条合同全部通过；最终边界修正后再次运行 `--compat`，83 条通过并刷新原生报告。比较器已再次读取这个最终报告并更新 JSON：57 条双方接受、12 条预期原生回退、14 条双方拒绝，五类可比结构和分类均无差异。
- 17 个新完整场景的浅／深色、100%／150%／200% 缩放离屏渲染和边界检查通过。最终目视检查公式浅色 100%／深色 200% 以及实际 MarkdownView 深色 100%，17 个场景均完整，无裁剪或重叠；末尾三个纯 phantom 场景按设计无墨迹，保留尺寸，不误报为丢失公式。
- 既有第一至第五轮功能回归通过；直接依赖括号／数组的第三、第四轮同时重跑离屏渲染，其余旧轮次未重复截图，也没有将未重跑的截图记为本轮通过。
- KaTeX 参考工具实际离线运行，83 条的接受／拒绝合同全部符合固定参考。参考页的浏览器读取仍受安全策略阻止，未绕过；没有参考页截图、逐像素等价或真实窗口交互验收结论。

## 上一架构里程碑历史验证（44 条语料）

- `pulse`、`pulse_math_test`、`pulse_math_syntax_test` 构建成功，最终构建日志未出现编译警告。
- 独立语法测试及 `--architecture` 通过，覆盖错误位置、预算、数学原子间距、真实 Cambria Math 常量读取及常量对绘图几何的影响。
- 原有第一至第五轮功能回归通过。首次回归发现内部结构节点挤占源码预算；修正后第一轮的 100 个分数组合重新通过，未缩减输入或弱化断言。
- 架构及五轮离屏渲染已运行，检查浅深主题、100%／150%／200% 缩放、宽窄布局、源码复制及绘制边界；预算修正仅改变接受限额，随后重跑语法、第一轮功能与架构测试。
- 架构图展示前 12 条原生接受式，实际 MarkdownView 图展示前 8 条；44 条全部参与分类与结构对照，但没有全语料逐图等价结论。已目视抽查架构浅色 100%、MarkdownView 深色 150%／浅色 200% 和第五轮深色 100%，未见裁剪或重叠。
- KaTeX 比较器在最终原生报告上再次运行，无分类或稳定结构计数差异；参考页浏览器截图及真实窗口交互仍未验证。
