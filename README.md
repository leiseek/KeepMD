**KeepMD 0.4.0 — Windows 原生 Markdown 阅读器**

阅读、预览优先，附带按需开启的轻量源码编辑。使用 C++20、Win32、DirectWrite、Direct2D、WIC 和 MD4C；Mermaid 流程图原生解析与绘制。无 Electron、Tauri、WebView 或 JavaScript 运行时；提示词常驻可选开启。

交付目标为 Windows 11 x64 便携版。应用运行只使用系统动态库，第三方 Markdown/流程图代码静态编入 EXE。编辑使用按需创建的系统 RichEdit 纯文本控件。

**使用**

运行 `dist/KeepMD-0.4.0-windows-x64/keepmd.exe`，打开或拖入 `.md`、`.markdown`、`.mmd` 文件。也可以执行：

```powershell
.\dist\KeepMD-0.4.0-windows-x64\keepmd.exe .\README.md
```

`Ctrl+O` 打开，`Ctrl+F` 查找，`F9` 目录，`Ctrl+滚轮` 缩放，`Ctrl+D` 深浅主题，`F6` 切换阅读/源码，`Ctrl+S` 保存，`Ctrl+H` 替换。双击流程图切换适应宽度与原始大小，右键可复制图表源码。

支持正文、标题、列表、引用、代码、链接、常见表格和任务列表、本地图片，以及明确范围的 Mermaid flowchart/graph。详见 [使用说明与支持范围](docs/USAGE.zh-CN.md)。

**快捷输入 Markdown 提示词**

KeepMD 运行时按 `Ctrl+Alt+Space`，或点“提示词”打开独立输入窗口。直接在单栏可视化编辑区输入，用工具栏设置标题、加粗、斜体、列表、引用与代码；按 `Ctrl+Enter`、`Esc` 或再次按全局快捷键，复制源码并收起，然后在原窗口 `Ctrl+V`。中文输入法正在组词时，Esc 优先取消组词。

格式按钮应用于选中的文字；未选中文字时，“加粗／斜体／代码”会插入选中的占位文字，直接输入即可替换。流程图显示为图形，点“流程图”或双击图形修改该图的 Mermaid 语法。复制和保存仍保留 Markdown。

提示词窗口的“设置”支持自定义快捷键、双击左 Ctrl、置顶、透明度、常驻托盘与登录启动。“文件”支持载入／导出 Markdown 和导入旧 Prompt Flow JSON。草稿自动保存，不影响正在阅读或编辑的文档。

```powershell
.\keepmd.exe --prompt    # 立即输入提示词，并启用托盘常驻
.\keepmd.exe --resident  # 静默驻留，等待全局快捷键
```

默认关闭阅读器即退出；只有明确开启常驻后才保留托盘服务。旧 Prompt Flow 仍运行时请使用组合快捷键，退出旧工具后才能启用双击 Ctrl。能力对应和验证边界见 [Prompt Flow 集成说明](docs/PROMPT-FLOW.zh-CN.md)。

**可视化提示词编辑（0.4.0）**

提示词窗口提供原生可视化编辑、底部“复制并收起”主操作和可折叠快捷键设置，不再提供源码／预览双栏。深浅主题同步覆盖标题栏、顶层菜单、按钮、目录和状态栏；阅读器工具栏在窄窗口自动换行。输入区增加边距，提示词窗口本次运行中保留调整后的尺寸与位置。正文、编辑器、预览与目录统一使用主题色窄滑块，支持拖动、点击轨道翻页和滚轮；系统弹出菜单和文件对话框沿用 Windows 外观。

**构建与验证**

构建脚本使用 PowerShell 7；需要包含 MSVC、Windows SDK、CMake ≥ 3.25 和 Ninja 的 Visual Studio C++ Build Tools。依赖源码已固定并随仓库提供，普通构建无需下载它们。

```powershell
.\tools\build.ps1 -Test
# 输出：build/release/keepmd.exe
```

完整端到端验证需要可交互的 Windows 桌面、Python 和开发依赖；测试会打开自己的窗口，真实输入法测试会在测试窗口输入 `nihao`：

```powershell
python -m pip install -r tools/requirements-dev.txt
.\tools\validate.ps1 -Performance
# 没有可用中文输入法时可加 -SkipIme；会跳过输入法及提示词全链路测试，结果必须注明。
```

基准区分温热缓存、进程创建、首屏提交、绘制耗时、Private Bytes 和 Working Set。测量不等同于物理显示器帧时间，也不宣称其他机器具有相同性能。当前数据、环境和未验证项见 [验证报告](docs/VALIDATION.zh-CN.md) 及 `bench/results/`。

**项目资料**

- [实施与交付状态](ROADMAP.md)
- [迭代记录](docs/ITERATION-LOG.md)
- [技术调研](docs/RESEARCH.zh-CN.md) 与 [Tinta 复用评估](docs/TINTA-AUDIT.zh-CN.md)
- [验收约定](docs/ACCEPTANCE.zh-CN.md)
- [原生阅读决策](docs/decisions/0001-native-reader.md) 与 [编辑控件决策](docs/decisions/0002-source-editor.md)
- [第三方来源与许可证](THIRD_PARTY_NOTICES.md)

阅读器是本项目的重点。完整 Mermaid、完整桌面排版软件级的富文本编辑、公式、任意 HTML/CSS、SVG 全特性、PDF/DOCX 导出、云同步及跨平台不在 0.4 的交付范围。
