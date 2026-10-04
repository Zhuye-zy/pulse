# Markdown 数学预览

Pulse 使用自有 C++ 解析与 DirectWrite/Direct2D 排版，复用系统 Cambria Math。
不打包 KaTeX、MathJax 或数学字体。公式源码保留用于复制；不支持、格式错误或超预算时回退为文本。

语法语义参考 [KaTeX Supported Functions](https://katex.org/docs/supported.html)
及 [Support Table](https://katex.org/docs/support_table.html)，不承诺完整 LaTeX 兼容或相同像素输出。

## 支持范围

| 类别 | 当前支持 | 边界 |
| --- | --- | --- |
| Markdown 分隔符 | `$…$`、`$$…$$`、`\(…\)`、`\[…\]` | 不跨空段落；代码不识别为数学 |
| 基础结构 | 分组、上下标、撇号、分数、二项式、带可选指数的根号 | 有限 TeX 子集 |
| 样式 | display/text/script/scriptscript、直立、粗体、斜体、双线体、Script／Fraktur／Sans／Mono、`boldsymbol/bm` | `mathcal/mathscr` 共用系统 Unicode Script 家族；不是全部 TeX 字体及相同字形 |
| 括号 | `left/middle/right`、`big/Big/bigg/Bigg` 及 l/r/m 变体、常见独立括号 | `middle` 仅限对应 `left/right` 的直属内容；未实现 OpenType MATH 拼接 |
| 矩阵 | matrix、pmatrix、bmatrix、Bmatrix、vmatrix、Vmatrix、smallmatrix | 每环境最多 32 行、16 列 |
| 多行 | cases、aligned、gathered | 不提供公式编号或跨公式引用 |
| 数组与多行下标 | `array` 的 l/c/r 列、单／双竖线、行边界单／双 `hline`、有界额外行距；`substack{…\\…}` | 不支持 p 列、`@`、`cline`、`multicolumn`；substack 仅单列 |
| 上下方标注 | overset、underset、横线、帽子、点、波浪、向量 | 使用数学原子分类间距；不等于完整 TeX 排版算法 |
| 运算符 | 常见三角/对数函数、求和、积分、集合运算、pmod/bmod | 不是 KaTeX 全部符号表 |
| 文本 | text、textrm、textbf、operatorname | 参数为简单文本，不接受任意嵌套文本命令 |
| 大括弧标注 | `overbrace/underbrace` 及标签 | over 的上标签／under 的下标签堆叠；另一侧脚标按普通侧标排版 |
| 隐形盒与显式间距 | `phantom/hphantom/vphantom`；有界 `hspace` | `hspace` 支持有符号 em/ex/pt；纯间距公式仍源码回退 |
| 公式局部宏 | `newcommand/renewcommand/providecommand`、0–9 个总参数（首参可默认）、嵌套展开与局部作用域 | 不跨公式；内建命令禁止 new/renew 覆盖；展开受预算限制 |

`operatorname*`：展示样式默认把上下限堆叠在名称上下，行内样式默认放侧边，支持显式 `limits/nolimits`。
重音新增 `acute/grave/breve/check/mathring`；方向标注新增 `overrightarrow/overleftarrow/overleftrightarrow`
和相应三个 `under…` 命令。重音使用字体墨迹边界定位，箭头使用原生线段绘制。
数组单元使用 text 样式，substack 使用 script 样式，退出后恢复外围样式；缺失数组单元留空，超过声明列数则回退。

### 数组、分隔符及隐形尺寸

`array` 的每个列边界允许 0、1、2 根 `|`，每个行边界允许 0、1、2 条 `hline`；顶部、行间和底部均可放线。
已有多行环境允许在换行命令后附加 `[长度]` 额外行距，例如 `\\[0.6em]`，长度必须非负。
三重线、负行距和复杂列说明即使在其他 TeX 引擎中合法，也超出当前原生合同，应整式回退。

`left … middle … right` 可含多个 `middle` 和独立嵌套的 `left/right`。
`middle` 不能越过显式分组、命令参数或数组单元绑定外围括号。
`overbrace` 和 `underbrace` 的正文可以嵌套分数、脚标、另一层大括弧；标签保持自身作用域。

`phantom` 保留正文宽度和高度，`hphantom` 仅保留宽度，`vphantom` 仅保留高度，三者都不绘制正文墨迹。
这是明确的隐形尺寸语义，不能因为没有墨迹就丢掉正文尺寸。纯 `vphantom` 允许零宽但有效高度；纯 `hspace` 仍回退源码。
显式长度采用严格十进制，单位为 em/ex/pt，绝对值分别不超过 20em、40ex、200pt；行距使用相同上限但不允许负值。
不接受指数、NaN、无穷值或部分解析后剩余的单位字符。

### 公式局部宏与常用字体

宏名为 ASCII 字母组成的控制词，可使用 `\newcommand{\name}` 或 `\newcommand\name` 形式。
参数数目为 0–9，使用 `#1` 至 `#9`；可选首参默认值占用第一个参数，其余参数接受分组或单个 token。
花括号和环境作用域内定义／更新是局部的；矩阵／对齐环境的单元，以及 `substack` 的每一行，分别建立局部作用域。
因此在当前单元／行更新宏不会泄漏到下一单元／行，退出后恢复外层定义。嵌套展开不会额外添加改变数学间距的分组。
同一公式可以组合定义和使用；进入下一公式时宏状态清空。错误位置映射回原始源码，复制和回退保留原始定义与调用。

`newcommand` 不覆盖已有用户宏，`renewcommand` 要求用户宏已存在；两者都禁止覆盖内建命令。
`providecommand` 遇到已有用户或内建命令保留原义，但定义体仍需合法。
不实现带星定义、`def/gdef/let/global/catcode`、`##`、包加载或文件访问，不把这些原语当作局部宏悄悄执行。

字体命令包括 `mathcal/mathscr/mathfrak/mathsf/mathtt` 与 `boldsymbol/bm`。
前两者共用系统 Unicode Script 映射；其余对应 Fraktur、Sans、Mono。
`boldsymbol/bm` 将拉丁／希腊字母映射为粗斜体，数字为粗体；已有 Script／Fraktur／Sans 使用对应粗体映射，Mono 使用 DirectWrite 粗体。
没有对应家族变体的数字或非拉丁符号保留原字符，标准符号按可用字形保留或加粗；简单 `text` 正文不被数学字母映射改写。
缺字仍受覆盖检查和源码回退约束，不能以方框或无声丢字充当支持，也不宣称与 KaTeX 的字体完全相同。

### 框线、消去线和伸缩箭头

- `boxed{…}`：原生四边框，内部使用 display 样式，退出后恢复外围样式。
- `cancel{…}`、`bcancel{…}`、`xcancel{…}`：分别为正斜线、反斜线及双斜线，保留基底公式。
- `xrightarrow[下面标签]{上面标签}`、`xleftarrow[下面标签]{上面标签}`：下面标签可省略，箭头随标签宽度伸长，标签采用脚标样式。
- 命名运算符增加 `arg/deg/dim/hom/ker/coth/lg`，以及可堆叠上下限的 `inf/sup/liminf/limsup/Pr/argmax/argmin`；`operatornamewithlimits` 等同于 `operatorname*`。

空参数 `boxed{}` 和 `xrightarrow{}` 可以只绘制线条；纯分组或纯空白不会因此被当成可见公式。
框线和箭头使用原生线条，不提供彩色框、双线伸缩箭头或 OpenType 字形拼接。

`\dots` 当前等同于基线省略号 `\ldots`，不自动按上下文切换位置。
反斜杠分隔符候选会被代码、HTML、图片、链接边界及已识别美元公式中断。
含显式 `>` 引用续行标记的多行候选保守回退；链接标签内的完整公式可以渲染。

## 尚未实现

- 通用 TeX 宏原语、跨公式全局宏、包／文件处理、完整文本模式及字体族。
- 公式编号、标签、交叉引用和复杂数组列格式。
- 完整 TeX 排版规则、OpenType MATH 字形拼接、数学 kern 与字形倾斜修正。
- 与 KaTeX/MathJax 的完整兼容性测试；支持清单对照不等于兼容认证。

## 资源限制与验证

预览每文档最多排版 256 个公式、共 65536 个 UTF-16 单元，每式最多 4096 个单元（预览范围包含分隔符）。
引擎另限制 1024 个源码原子、4096 个语法树节点（含结构包装和空占位）、32 层解析嵌套、总计 128 个矩阵/多行单元。
源码原子与结构节点分开计数，避免引入语法树后挤占原有公式容量；构造过程同样受节点上限约束。
局部宏另限制原始／展开后源码各 4096 个 UTF-16 单元、累计 64 条定义指令（包括无操作 provide 和更新）、256 次调用、32 层展开链。
窗口过窄时先有限缩小，再回退文本，不无限压缩。

定向目标 `pulse_math_syntax_test` 独立验证语法树、错误位置、预算、间距和字体表边界，无绘图依赖。
`pulse_math_test --architecture` 验证兼容语料及字体布局；`--structures` 针对数组规则、分隔符、大括弧和隐形尺寸；
`--reliable` 针对局部宏、字体映射、作用域、源码保留与参数边界；
`--stage2` 至 `--stage5` 分别运行原有扩展，`--render` 生成离屏截图。
截图由实际 MarkdownView 组件产生，覆盖主题、缩放、宽窄布局及源码选择；不代替真实窗口交互验收。
测试夹具及生成图位于本地 `bench_data/`，该目录由仓库忽略规则排除。

## 解析与排版架构

`math_syntax` 将源码转换为带源码范围的语法树，不依赖 DirectWrite；分组、脚标、分数、根号和环境保留结构。
`math_layout` 只消费语法树，按 Ord/Op/Bin/Rel/Open/Close/Punct/Inner 分类处理相邻间距、二元符号退化及脚标样式。
`math_formula` 负责接入 DirectWrite 行内对象、缓存绘图结果和宽度回退。

`math_font_metrics` 对 OpenType MATH 常量表做有界解析；`math_font_face` 获取并释放系统字体表。
可用时使用字体提供的脚标缩放、数学轴、分数位移与间距、根式及上下横线参数；表缺失或无效时使用默认度量。
这不包含完整 MATH 字形拼接、设备修正或数学 kern。

完整支持边界和可复现的 KaTeX 对照方法见 [兼容清单](markdown-math-compatibility.md)。
