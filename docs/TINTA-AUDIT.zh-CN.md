**Tinta 源码复用评估**

日期：2026-10-02。评估对象固定为 `oipoistar/tinta@db70698e49a1f700c7ad98add1bebe713ac796bd`（项目版本 3.7.4）。目的：判断它是否适合作为 KeepMD 的阅读和流程图基础。

以下为最初源码评估记录；后续已构建固定参考并测量，见 [决策 0001](decisions/0001-native-reader.md) 和 [验证报告](VALIDATION.zh-CN.md)。初次评估时仅检查关键源码路径与官方文档，尚未构建或运行 Tinta。以下性能风险是由代码结构推导的待测问题，不是已经测得的卡顿、泄漏或内存数值。验收方案见 [ACCEPTANCE.zh-CN.md](ACCEPTANCE.zh-CN.md)。

**结论与路线选择**

推荐建立小型、模块分离的 KeepMD 原生阅读核心，优先评估复用 Tinta 的 Mermaid 解析、布局与测试语料。Tinta 整体作为同机比较基线。初版不直接继承其全部窗口状态、编辑器、导出和后台功能。

原因是阅读优先与低资源目标要求我们直接控制布局对象、图片引用和调度的生命周期。独立模块的复用收益较明确，整套阅读状态的迁移成本需要 P0 实测后判断。以下选择是工程建议，可以由 P0 的证据修订。

| 候选做法 | 预期收益 | 代价与决定条件 |
| --- | --- | --- |
| 整体 fork Tinta | 最快取得现成功能 | 若既有资源模型已经通过 KeepMD 语料与预算，且精简无需大改，可重新考虑 |
| 小型阅读核心 + 有选择复用 | 模块与资源所有权清晰，聚焦 Windows 阅读 | 默认路线；需要自己实现阅读排版、导航和选择交互 |
| 全部重新实现 | 完整控制 | 流程图布局维护成本高，暂不采用；先验证已有独立模块 |

**关键源码发现**

| 核查项 | 代码事实 | 对 KeepMD 的影响 |
| --- | --- | --- |
| 首屏排版 | `layoutDocumentViewportFirst` 优先排约两屏，`layoutDocumentContinue` 继续处理剩余顶层块 | 可以借鉴首屏调度；不能据此宣称长期内存只随视口增长 |
| 排版预算 | `layoutStep` 在顶层块之间检查时间；单个块内部不受这层预算打断 | 巨型表格、列表或代码块需要单独测试和细分调度 |
| 强制完成 | `ensureLayoutComplete` 可以无时间预算完成剩余布局 | 查找、复制、跳转等功能要检查是否把全文排版带回交互路径 |
| 本地图片 | `getOrLoadImage` 的本地图片路径直接调用解码，远程图片另走线程 | KeepMD 的本地大图也应异步，优先可见图片 |

上述事实来自固定提交的 [render.cpp](https://github.com/oipoistar/tinta/blob/db70698e49a1f700c7ad98add1bebe713ac796bd/src/render.cpp)。它们说明待验证的控制边界，不证明某种文件必定卡顿。

`WM_APP_LAYOUT_CHUNK` 在窗口消息处理中调用约 10 ms 预算的继续排版。这里是 UI 线程分时执行；CPU 密集图表在 KeepMD 中应有独立任务通道。[main_d2d.cpp](https://github.com/oipoistar/tinta/blob/db70698e49a1f700c7ad98add1bebe713ac796bd/src/main_d2d.cpp)

图片缓存设有 64 MiB 的估算预算，但 `LayoutBitmap` 另持有位图的 `ComPtr` 引用。因此缓存淘汰条目时，仍被布局持有的位图可能继续驻留；这个预算不等于整个应用的图片资源上限。`App` 同时包含阅读、编辑和窗口状态，迁移时需要拆分所有权。[app.h](https://github.com/oipoistar/tinta/blob/db70698e49a1f700c7ad98add1bebe713ac796bd/include/app.h)

以上是资源引用结构的判断，不能直接称为内存泄漏。KeepMD 的图片总账应统计仍被任何模块持有的资源；布局只保留资源 ID 和目标矩形，实际绘制时从有总预算的资源管理器获取短期引用。

**优先复用范围**

| 模块 | 结论 | 接入前必须验证 |
| --- | --- | --- |
| Mermaid flowchart 数据结构和布局 | 高优先级候选；接口接收节点尺寸，较易单独提取 | 单独编译、中文标签、回边、自环、分组、取消与超限行为 |
| Mermaid 扩展绘制模型 | 后续候选；用图元表达结果，并注入文本测量回调 | 只接入需要的图表类型，保持与 UI 状态分离 |
| Markdown 功能与视觉样例 | 作为参考语料 | KeepMD 自己声明语法范围，不能把竞品截图当作规范 |
| 图片缓存管理 | 借鉴后重做资源所有权 | CPU/GPU/在途解码统一预算，布局不长期钉住离屏资源 |
| 首屏排版 | 借鉴调度思路 | 进一步做到视口布局可淘汰，并处理巨大单块 |
| 完整 App 状态和自绘编辑器 | 暂不移植 | 阅读核心独立；编辑阶段评估 Scintilla |

流程图结构见 [mermaid.h](https://github.com/oipoistar/tinta/blob/db70698e49a1f700c7ad98add1bebe713ac796bd/include/mermaid.h)。扩展图表通过宿主回调测量文字，输出图元，头文件自身不依赖 Direct2D/DirectWrite，适合作为模块边界参考。[mermaid_ext.h](https://github.com/oipoistar/tinta/blob/db70698e49a1f700c7ad98add1bebe713ac796bd/include/mermaid_ext.h)

这些接口体现可提取性；只有完成独立构建、测试和依赖清点后，才能把“可提取”记为“已复用”。

Tinta 测试会拒绝部分 Mermaid 11 属性语法，并对部分未支持图表结构回退。KeepMD 首版只声明具体验收矩阵中的流程图子集。[Mermaid 测试](https://github.com/oipoistar/tinta/blob/db70698e49a1f700c7ad98add1bebe713ac796bd/tests/mermaid_tests.cpp)

**阅读核心的设计约束**

- 文档语义模型、阅读布局、窗口和编辑会话分别拥有状态，不引入共享的全功能 App 对象。
- 原始文本与解析后的块可随文件大小增长；昂贵的字体布局、解码像素和 GPU 资源采用有预算的缓存。不能把整个应用宣称为常量内存。
- MD4C 首版采用后台完整解析，以保证引用定义等跨块语义。首屏优先首先针对排版、图片和图表；不会把任意 Markdown 按行切开并声称语义等价。
- 将长期保留的文本位置和几何缓存分开。查找、复制、目录扫描应读取语义文本，不能强制创建全文字体布局。
- UI 线程管理窗口、交互和渲染目标；解析、图片解码、图表计算进入有限任务队列。结果带文档版本号，切换文件后旧任务不能覆盖新内容。
- 巨型表格按行调度；长代码块按行或窗口片段排版。单个超长自然段保留正确排版与软换行语义，其分段策略由 P0 的病理样例验证。
- 图表测量与正文使用一致的字体、字号及 DPI 规则。线程间不随意共享可变 COM/图形对象，资源创建和销毁遵守明确的线程归属。

DirectWrite 的布局对象支持文字命中测试，可作为选择和链接定位基础。[微软命中测试文档](https://learn.microsoft.com/en-us/windows/win32/directwrite/how-to-perform-hit-testing-on-a-text-layout) Direct2D 官方建议复用资源并区分设备相关资源；多线程访问需要显式设计，而不是给任意绘制对象加后台线程。[性能文档](https://learn.microsoft.com/en-us/windows/win32/direct2d/improving-direct2d-performance)、[多线程文档](https://learn.microsoft.com/en-us/windows/win32/direct2d/multi-threaded-direct2d-apps)

**P0 要解决的四个问题**

1. Tinta 在本机打开 100 KiB、10 MiB 和多图片文档时，首屏、完整排版及资源峰值分别是多少？
2. 同一文档阅读到末尾后，布局、图片和字体对象是否持续累积？关闭或切换后能否稳定释放？
3. 提取 flowchart 模块需要哪些文件，其基本语料、中文标签和复杂边连接是否正确？
4. 对本机而言，直接硬件绘制与先软件首屏后切换硬件的策略，哪一种在启动、滚动、内存上更合适？此选择必须测量后决定。

若一个候选模块需要把整个窗口状态或编辑器一并带入，就停止继续扩大提取范围，改为借鉴接口和算法。若 Mermaid 模块在规定子集内无法保持连接关系正确，则进入 Rust 原生后端对比；无论结果如何，都不加入浏览器兜底。

**环境与证据状态**

2026-10-02 本机预检：Windows 11 x64；已发现 Visual Studio Build Tools、MSVC `14.50.35717`、随 VS 安装的 CMake，以及 Windows SDK `10.0.26100.0`。当前普通 PowerShell 的 PATH 不直接包含 `cl`/`cmake`，后续使用开发者环境或显式定位；没有因此安装或修改工具链。此次未进行编译冒烟验证，工具链可用性仍由 P0 的实际构建确认。

上游代码按 MIT 许可发布；实际引入时记录提交、来源、修改和许可证。此阶段仅形成研究文档，未把上游实现复制进 KeepMD 产品源码。[Tinta LICENSE](https://github.com/oipoistar/tinta/blob/db70698e49a1f700c7ad98add1bebe713ac796bd/LICENSE)
