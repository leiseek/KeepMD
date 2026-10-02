**KeepMD：Windows 原生 Markdown 阅读与轻量编辑工具调研**

调研日期：2026-10-02。用户已确认：只用 Windows；查看、预览优先，编辑为次要功能；追求高性能、低资源占用；禁止 Electron、Tauri 及其他 WebView 方案；需要文本流程图支持。

后续已完成 [Tinta 深入源码评估](TINTA-AUDIT.zh-CN.md)、[实施 roadmap](../ROADMAP.md) 与 [验收约定](ACCEPTANCE.zh-CN.md)。具体阶段、支持范围和复用策略以这些后续文档为准：默认建立小型阅读核心，优先评估提取独立流程图模块；完整 Tinta 用作测量基线。

本文保留最初调研阶段的记录；后续已完成实现和本机验证，见 [验证报告](VALIDATION.zh-CN.md)。

本文基于官方文档、项目仓库及关键源码核查。没有安装运行竞品，没有进行本机性能基准测试。项目宣传数据、源码事实与本文的设计建议分别标明，不能把不同机器、不同版本的数据直接排名。

**建议采用的方向**

产品定位为“默认打开即阅读的 Windows Markdown 阅读器，按需进入源码编辑”。首选架构是 C++ + Win32 + DirectWrite + Direct2D + MD4C，图片使用系统 WIC。主要理由是可以直接使用 Windows 的窗口、字体排版、绘图及图片解码服务，控制第三方依赖、初始化工作和缓存生命周期。它是最适合当前约束的候选架构，不是未经测试的“绝对最快”结论。

第一实现参考应从 Typora/VNote 转向 **Tinta**：它的产品定位和底层路线与需求最接近。Typora 用来参考阅读版式；VNote 用来参考导航和键盘操作；Ferrite 用来参考原生流程图和编辑交互。先评估复用 Tinta 的阅读与图表模块，再决定是否独立实现。当前不应提前承诺完整 Mermaid 或 Typora 式所见即所得编辑。

**竞品与参考项目**

| 项目 | 核查到的实现路径 | 对当前需求的价值 | 判断 |
| --- | --- | --- | --- |
| Typora | Windows/Linux 使用 Electron；支持 Mermaid 与另一套 `flow` 流程图语法 | 阅读排版、目录、图表融入正文的方式 | 借鉴体验，技术架构不符合约束 |
| VNote | C++/Qt；构建明确链接 `WebEngineWidgets` 和 `WebChannel` | 文件与笔记导航、快捷键、编辑辅助 | 不能因为主体是 C++ 就视为无浏览器方案 |
| ghostwriter | Qt；预览相关构建链接 `WebEngineWidgets` | 简洁的源码与预览交互 | 完整应用路径不符合约束 |
| Tinta | C++、Win32、Direct2D/DirectWrite、MD4C；原生流程图模块 | Windows 阅读优先、选择复制、查找、目录、轻量编辑 | 最接近目标，应优先研究 |
| Ferrite | Rust + egui/eframe；原生 Markdown 和 Mermaid 渲染 | 原生图表、文档视口处理、编辑器设计 | 作为第二参考，现有产品功能范围比本项目大 |
| rmdv | Rust + Iced、pulldown-cmark、原生 Mermaid 和 resvg | 阅读器结构、视口内块处理、图表集成 | 架构参考；Windows 的功能和构建条件需独立验证 |
| rweijnen/MDView | Rust，但 GUI 依赖 WebView2 Runtime | Windows 文件打开与 Total Commander 集成 | 不符合约束，不能按安装包大小判断运行资源 |

依据：[Typora 框架说明](https://support.typora.io/What's-New-0.9.66/)、[Typora 图表文档](https://support.typora.io/Draw-Diagrams-With-Markdown/)、[VNote 构建依赖](https://github.com/vnotex/vnote/blob/master/src/CMakeLists.txt)、[ghostwriter 构建依赖](https://github.com/KDE/ghostwriter/blob/master/src/CMakeLists.txt)、[Tinta](https://github.com/oipoistar/tinta)、[Ferrite](https://github.com/OlaProeis/Ferrite)、[rmdv](https://github.com/minchenlee/rmdv)、[MDView](https://github.com/rweijnen/MDView)。

这里排除的是具体项目的 WebEngine/WebView 使用路径。Qt Widgets 自身不等于 WebView；例如 QTextBrowser 属于 Qt 的富文本控件，可以不使用浏览器内核。[Qt QTextBrowser 文档](https://doc.qt.io/qt-6/qtextbrowser.html)

**Tinta 的源码核查与可复用边界**

本次重点检查提交 `db70698e49a1f700c7ad98add1bebe713ac796bd`，其 CMake 项目版本为 3.7.4。以下链接固定到该提交，避免后续主分支变化影响结论。

- 构建使用 C++17，Markdown 依赖 MD4C，链接 Direct2D、DirectWrite、WIC 等 Windows 系统库。主应用目标中没有 Electron、Qt WebEngine 或 WebView2。[CMakeLists.txt](https://github.com/oipoistar/tinta/blob/db70698e49a1f700c7ad98add1bebe713ac796bd/CMakeLists.txt)
- 流程图有独立的节点、边、分组与布局结构；布局接收已测量的节点尺寸，可供原生绘制使用。[mermaid.h](https://github.com/oipoistar/tinta/blob/db70698e49a1f700c7ad98add1bebe713ac796bd/include/mermaid.h)
- 渲染源码使用 DirectWrite 测量图表标签，并提供首屏优先、余下内容分段继续的排版路径。这不代表已证明大文档全部布局对象都按需释放，仍要测长时间阅读的内存峰值。[render.cpp](https://github.com/oipoistar/tinta/blob/db70698e49a1f700c7ad98add1bebe713ac796bd/src/render.cpp)
- Mermaid 测试明确要求拒绝 `A@{ shape: rounded, label: "Fancy" }` 这类新属性语法，也包含复合状态图等未支持结构的回退检查。因此“覆盖多种图表类型”不等于“每种类型完整兼容”。[mermaid_tests.cpp](https://github.com/oipoistar/tinta/blob/db70698e49a1f700c7ad98add1bebe713ac796bd/tests/mermaid_tests.cpp)

官网宣称约 2.5 MB 程序、低于 100 ms 启动。这只是项目方数据，本次未复测，也不能据此推导其进程内存或 GPU 占用。[Tinta 官网](https://tinta.cc/)

对自用项目，建议先复用成熟阅读功能，而不是重新实现全部交互。具体选择应由中文排版、长文档、常用图表的验证结果决定。Tinta 为 MIT 项目；若采用代码，保留对应版权与许可文件。

**技术栈取舍**

| 方案 | 满足无 WebView | 对本项目的主要优势 | 主要成本 | 优先级 |
| --- | --- | --- | --- | --- |
| C++ + Win32 + DirectWrite/Direct2D | 是 | 系统能力直接复用，依赖和资源控制细，Tinta 可参考 | 原生阅读控件、选择复制、表格和可访问性需要工程实现 | 首选 |
| Rust + Windows API + DirectWrite/Direct2D | 是 | 可采用同样渲染架构，便于整合 Rust 图表库 | COM/FFI 与原生控件集成；不能仅凭语言声称更低内存 | 团队偏好 Rust 时可选 |
| C++ + Qt Widgets + QTextDocument 或自定义绘制 | 是，前提是不引入 WebEngine | 常规桌面控件、文本操作更容易落地 | 带上 Qt 依赖；大文档仍需要布局与缓存设计 | 开发成本优先时备选 |
| Rust + egui/Iced | 是，采用原生后端时 | 可借鉴现成原生编辑器/阅读器 | 字体、渲染后端、中文输入、缓存与空闲重绘需实测 | 第二梯队 |

DirectWrite 提供字体排版、测量和命中测试，Direct2D 可直接绘制文本布局；但 Markdown 的块结构、链接行为、表格、目录、跨段选择等仍需要应用实现。[Microsoft 文本渲染文档](https://learn.microsoft.com/en-us/windows/win32/direct2d/direct2d-and-directwrite)

若选 Qt，不应把每次编辑都调用全文 `setMarkdown()` 当作最终高性能架构：该接口替换整篇内容。QPlainTextEdit 的轻量布局适合源码编辑，但不支持富文本表格和嵌入框架，不能直接承担完整阅读视图。[QTextDocument](https://doc.qt.io/qt-6/qtextdocument.html)、[QPlainTextEdit](https://doc.qt.io/qt-6/qplaintextedit.html)

**建议的阅读器结构**

```text
Markdown 文件
    │
    ▼
编码处理 + 原始文本（保存时保留用户源码）
    │
    ▼
MD4C 解析 → 紧凑块模型 + 文本样式区间 + 目录索引
    │
    ├─ 段落/标题/列表/表格 → DirectWrite 测量与排版
    ├─ 图片 → 后台 WIC 解码 → 有预算的位图缓存
    └─ Mermaid → 后台原生解析/布局 → 图形与文字，或 SVG
    │
    ▼
首屏优先排版 + 视口附近缓存 + Direct2D 绘制
    │
    └─ 按需开启源码编辑 → 修改通知 → 重建受影响结果
```

这是一套建议设计，不是已经实现的代码。MD4C 通过回调报告块、行内样式和文本，便于构建原生模型；它不是现成的增量编辑引擎，也不提供完整阅读控件。[MD4C 官方说明](https://github.com/mity/md4c)

实现时优先处理以下问题：

1. **首屏与全文件解耦。** 先显示可见正文，继续解析和排版时保持界面响应；不能为了页尾图片或流程图延迟首屏。
2. **区分绘制裁剪和布局虚拟化。** 只画可见段落还不够，避免一次创建全文件的 DirectWrite 布局。保留紧凑块索引与高度估计，视口附近做精确排版；高度修正时稳定阅读锚点。
3. **控制复制。** 明确原始 UTF-8、供 DirectWrite 使用的 UTF-16、可搜索文本和布局对象的生命周期，不为每个功能常驻一份全文。偏移映射应区分字节、UTF-16 单元与用户可见字符。
4. **空闲不持续重绘。** 只在滚动、文件变化、缩放、窗口暴露或异步结果返回时更新；滚动动画结束后停止计时器。Direct2D 可使用 GPU，但不要通过持续刷帧换取表面上的流畅。
5. **缓存有总预算。** 图片按显示尺寸解码；CPU 像素、GPU 纹理、图表结果和文本布局分别计量。100 张大图不能同时解码驻留。
6. **图表不阻塞主线程。** 只优先处理可见和邻近图表；源文本、主题、字体、DPI、引擎版本共同参与缓存键。过时结果不覆盖新版本；复杂图表要有节点/边预算和耗时控制。
7. **编辑按需加载。** 进入编辑模式时才创建编辑控件。可以采用 Scintilla 处理源码、撤销和输入法，不自行重造输入系统；源码保存不经富文本反向转换。[Scintilla 文档](https://www.scintilla.org/ScintillaDoc.html)
8. **预览更新先正确再细化。** 小文档可防抖后在后台重新解析，复用未变图表与布局。跨块语法、引用定义、围栏变化可能影响后文，不能简单按行拆开解析并声称与 CommonMark 一致。

WIC 是 Windows 的原生图片解码基础设施，适合承担常规图片加载。大图缩放、资源回收和异步调度仍由应用负责。[WIC 文档](https://learn.microsoft.com/en-us/windows/win32/wic/-wic-about-windows-imaging-codec)

**流程图语法与渲染路线**

用户尚未指定必须完整兼容哪一种流程图方言。建议默认以 Mermaid 的常见 `flowchart` / `graph` 为第一阶段目标，公开列出已支持语法。Typora 的 `flow` 代码块使用 flowchart.js，是另一套语法，不能因为已支持 Mermaid 就宣称 Typora 图表全部兼容。[Typora 图表文档](https://support.typora.io/Draw-Diagrams-With-Markdown/)

第一阶段的建议支持范围：

- `flowchart TD/TB/BT/LR/RL` 与 `graph` 别名。
- 常见矩形、圆角、判断菱形、圆形节点。
- 箭头、边文字、虚线、回边与自环。
- 分组 `subgraph`、中文标签、换行标签。
- 基础 `classDef` / `class` / `style`；不承诺任意 CSS。
- 解析失败显示原始代码和明确错误；不生成看似成功但连接错误的图。

| 路线 | 运行时是否需要浏览器 | 适合场景 | 必须核验 |
| --- | --- | --- | --- |
| Tinta 式 C++ 解析/布局 → Direct2D + DirectWrite | 不需要 | 依赖少，中文测量统一，图中文字可保留搜索/选择能力 | 语法子集、循环图、交叉边、分组、复杂布局 |
| mermaid-rs-renderer → SVG → 原生 SVG 栅格化 | 不需要 | 接入已有原生 Mermaid 实现，扩展图表类型 | 真实语料兼容率、字体度量、SVG 支持范围 |
| Merman → SVG 或原生输出 | 不需要 | 对 Mermaid 对齐要求更高时作为候选 | 固定版本、中文测量、HTML 标签及原生嵌入成本 |
| Graphviz DOT | 不需要 | 用户愿意使用 DOT 时的可选语法 | 与 Mermaid 是不同语言，不能当作完整替代 |
| 官方 mermaid-cli | 通常启动 Puppeteer/浏览器 | 官方渲染参考工具 | 不纳入本项目运行时 |

依据：[mermaid-rs-renderer](https://github.com/1jehuang/mermaid-rs-renderer)、[Merman](https://github.com/Latias94/merman)、[Graphviz dot](https://graphviz.org/docs/layouts/dot/)、[官方 mermaid-cli 源码](https://github.com/mermaid-js/mermaid-cli/blob/master/src/index.js)。原生重实现确实存在，但项目宣称的速度倍数或图表类型数量不等于在本应用中已验证的兼容性和性能。

建议先复用原生 C++ 常见流程图路径，并让图表后端可替换。若真实文档语料表明兼容范围不足，再比较 Rust 渲染器；通过库接口接入或按需启动原生工作进程，两者都不需要 WebView。是否采用进程隔离，依据硬超时需求与额外内存决定，不能假设线程可以安全强制终止。

有两个容易遗漏的技术限制：

- **Direct2D 的 SVG 支持不是完整浏览器 SVG。** 官方支持列表不含 SVG `text` 和 `foreignObject`，不能把带文字的 Mermaid SVG 直接送进去就保证完整显示。可选处理是图形用 Direct2D、文字用 DirectWrite；或者通过 resvg 生成当前缩放级别的位图并缓存。后者需要重新栅格化以保持缩放清晰，文字选择也需额外模型。[Microsoft SVG 支持范围](https://learn.microsoft.com/en-us/windows/win32/direct2d/svg-support)、[resvg](https://github.com/linebender/resvg)
- **中文标签必须以真实字体测量。** Merman 明确说明其默认度量与字体无关，建议宿主提供真实文本度量，并说明 HTML 标签、`foreignObject` 等存在兼容差异。Windows 宿主应尽量统一图节点测量与最终显示字体，防止文本超框和连线压字。[Merman 兼容说明](https://github.com/Latias94/merman#compatibility)

**阅读兼容范围与功能顺序**

| 阶段 | 内容 | 完成标准 |
| --- | --- | --- |
| 阅读基础 | 标题、段落、粗斜体、列表、引用、代码块、链接、本地图片、常见表格、任务列表 | 中文清楚，选择复制准确；原文件不被修改 |
| 阅读操作 | 目录、页内搜索、缩放、深浅主题、最近文件、拖入打开、本地 Markdown 跳转、文件变化刷新 | 阅读位置稳定，操作无需等待全文重排 |
| 流程图 | 明确范围的 Mermaid flowchart/graph，缩放，错误回退 | 支持列表中的语料通过；未知语法不会静默误绘 |
| 轻量编辑 | 源码模式、撤销重做、查找替换、保存、按需双栏预览 | 中文输入法可靠，保存保留源码风格与换行 |
| 后续扩展 | 更多图表、公式、导出、更多 Markdown 方言 | 每项分别验证依赖、兼容性与资源预算 |

正文先以 CommonMark 加常用扩展为目标，注明表格、任务列表等具体支持项。脚注、Wiki 链接、告警块、公式、复杂 HTML/CSS 都不能仅凭选择了某个解析库就自动宣称支持。

纯原生阅读器需要限定 HTML：可以逐步支持简单标签，复杂布局保留源码或明确提示。无需把复制 Typora CSS 主题、嵌入任意网页、所见即所得编辑作为首版验收要求。

**性能测量与首轮预算**

“最高性能、最低资源”需要同时观察首屏延迟、滚动延迟、空闲 CPU、进程内存和 GPU 资源。可执行文件体积只是其中一项；也不能通过把重任务放到子进程后只统计主进程来得到好看的数字。

以下是建议的首轮工程目标，尚未实测，不是性能承诺。测试机器、Windows 版本、DPI、字体、存储、构建版本、杀毒状态固定后再决定正式门槛。

| 指标 | 建议初始目标/方法 |
| --- | --- |
| 小文档首屏 | 本地 SSD、100 KB 中英混排、无大图；新进程且文件缓存温热时 p95 ≤ 100 ms，明确冷缓存条件后争取 p95 ≤ 250 ms |
| 空闲 | 静止阅读 30 秒无应用持续动画，CPU 接近测量噪声；检查是否存在周期全窗口重绘 |
| 阅读内存 | 上述小文档打开稳定后，进程树合计 Private Bytes 先争取 ≤ 40 MiB；同时记录峰值、Working Set 与 GPU 内存 |
| 滚动 | 60 Hz 基线下检查帧间隔 p95/p99，避免反复超过 16.7 ms；120 Hz 作为独立目标测试 |
| 图表 | 先争取典型 50 节点流程图后台布局 p95 ≤ 100 ms；正文交互不等待图表，复杂图单独统计 |
| 大文档 | 10 MB、50 MB 文件要能尽早阅读首屏，连续滚动与跳转可响应；关注布局对象与缓存是否无限增长 |

启动测量终点是“目标文档正文首屏已显示并能响应”，不是空白窗口创建或进程启动事件。图表可以后完成，但应另记“首屏完整就绪时间”。冷/热文件缓存、已有进程/新进程四种情况分开记录；重复试验报告中位数、p95、最大值和样本数。

内存报告区分 Private Bytes、Working Set、CPU 解码缓存、GPU 专用/共享内存；不要将这些可能重叠的口径直接相加。窗口关闭、切换多个文档、缩放和图表重绘后，检查资源是否返回稳定范围。

建议语料包括：100 KB/1 MB/10 MB/50 MB 文本，中英混排和 emoji，超长段落及代码行，宽表格与大量列表，多张高分辨率图片，20 个以上图表，50/200/1000 节点图，循环与分组图，损坏语法，125%/150%/200% DPI，以及无变化文件重复刷新。

Mermaid 首轮准备约 30–50 个来自实际笔记的样例。逐个记录解析成功、节点/边数量与连接关系、标签完整性、布局可读性、耗时、峰值内存和回退行为。若要宣称与官方特定版本兼容，应固定语法版本和参考输出；成功返回 SVG 本身不是兼容性证据。运行时不借用隐藏 Chromium 兜底。

**落实建议**

先把 Tinta 固定提交作为参考基线，用自己的中文文档和流程图语料验证阅读正确性与性能。通过后，可针对自用需求裁剪、重构或复用其模块；若阅读核心的结构不适合预算，再按上述 Win32 原生架构独立实现。只保留一个清晰的首版目标：快速打开 Markdown，可靠阅读正文和常用流程图，必要时进入轻量源码编辑。

当前尚待用户进一步定义的范围是流程图方言与兼容程度。本文按“常用 Mermaid 流程图优先、公开支持范围”制定方案；操作系统与阅读优先级已经确定，无需再次确认。
