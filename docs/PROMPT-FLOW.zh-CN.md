# Prompt Flow 集成说明

本次调研对象是本地同级目录 `D:\Workspace\prompt-flow`，不是微软的 Azure Prompt Flow。阅读了 `native/main.cpp`、`ui.cpp`、`hotkey.cpp`、`config.cpp` 及头文件和构建配置。该项目为 C++17 / Win32 应用，没有浏览器渲染。KeepMD 按其实际操作流程重新实现模块，未复制它的程序、图标或源码文件，也不依赖原项目安装。

## 功能对应

| 同级 Prompt Flow 的能力 | KeepPrompt 0.6.0 的实现 |
| --- | --- |
| 托盘静默启动，单实例 | 独立 `KeepPrompt.exe --resident`；每份配置只有一个提示词实例，再次启动只转发唤起请求。KeepMD 不启动也不转发提示词 |
| 全局组合键 / 双击左 Ctrl | 默认 Ctrl+Alt+Space；可自定义，保留 LCtrl x2 选项 |
| 多行文本输入 | 按需加载 Windows RichEdit；支持中文输入法、撤销、软换行、可视化文字编辑，复制保存 Markdown |
| 再按快捷键或 Esc 自动复制并隐藏 | 再按快捷键或 Ctrl+Enter 复制并收起；Esc 不收起窗口，仅在设置展开时关闭设置；关闭或最小化收起到托盘；返回唤起前的窗口 |
| 复制按钮、清空、字数和行数 | 原生按钮；清空可撤销；显示 UTF-16 单元数和逻辑行数 |
| 草稿及窗口偏好持久化 | 独立 UTF-8 草稿与偏好文件（`KeepPrompt.draft.md`／`KeepPrompt.ini`，0.4.x 的 `.prompt.md`／`.prompt.ini` 原位沿用）；防抖保存、原子替换、写入失败提示 |
| 置顶、30–100% 不透明度、右侧定位 | 设置菜单；按唤起前窗口所在显示器的工作区定位 |
| 当前用户登录时启动 | 设置菜单显式启用或关闭；注册命令包含引号和配置路径 |
| JSON 配置 | 文件菜单可导入旧 JSON 的草稿、快捷键、置顶与透明度；不修改旧文件、不继承自启动 |
| 无 Markdown 渲染 | 原生 RichEdit 可视化编辑，支持常用 Markdown 文字结构和内嵌 Mermaid 图形 |
| 无 Markdown 文件入口 | 新增 Markdown 载入与导出，草稿可用普通编辑器读取 |

原项目没有模板库、提示词历史、变量插值、AI API 或自动发送功能。KeepPrompt 提供三份本地起步模板（任务说明、代码审查、写作润色），从顶部“模板”菜单追加到草稿，可撤销；没有模板管理、历史或联网生成功能。复制的是完整 Markdown 源码，保留缩进、空白行和结尾换行，统一为 LF；与旧工具不同，不裁剪首尾空白。不会模拟粘贴或 Enter 提交。

## 架构与资源

`prompt_model` 负责快捷键语法、双 Ctrl 状态机、偏好、旧 JSON 导入和自启动命令；`prompt_window` 管理提示词窗口、托盘、焦点与剪贴板；`prompt_main` 提供隐藏宿主消息窗口和独立进程生命周期。KeepMD 与 KeepPrompt 完全独立：KeepMD 不启动、不唤起 KeepPrompt，KeepPrompt 也不再提供“打开阅读器”；两者只在构建时共用 `keep_shared` 界面基础库，`prompt_model` 与 `prompt_window` 不链接进 `keepmd.exe`。

普通组合键使用 Windows [`RegisterHotKey`](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-registerhotkey) 和 `MOD_NOREPEAT`。先注册新键，成功后才撤销旧键；失败时保留旧配置。程序运行时可全局唤起，退出后不占用快捷键。运行期间始终显示托盘图标，进程只能从托盘右键菜单退出；登录启动默认关闭。

双 Ctrl 通过可选 Raw Input 输入接收实现，没有默认全局键盘钩子。两次完整的左 Ctrl 按下/松开需在 350 ms 内完成，按住、自动重复或夹杂其他按键不会按双击处理。发现旧 Prompt Flow 的窗口类仍存在时，拒绝启用该模式并说明原因，避免两个工具同时唤起；组合键可以与旧工具共存。

提示词窗口及热键服务按需创建在 KeepPrompt 进程内，与 KeepMD 进程无关。0.4 改为单个原生富文本编辑区，撤销状态保存有界 Markdown 快照；连续输入合并为一步撤销。180 ms 防抖将 Markdown 结构映射为 RichEdit 样式并隐藏标记，800 ms 防抖保存草稿；中文组词期间暂停排版和保存。流程图由本进程解析、GDI 绘制并以静态 DIB 图片插入 RichEdit；没有动态 OLE 外部文档、WebView 或远程渲染。图片与隐藏的原始 Mermaid 源码关联，复制保存时仅移除生成的图片对象，保留 Markdown。

可视化编辑器不承诺完整 Typora 语法和排版一致性：常用标题、强调、列表、引用、代码与最多 16 张图表直接显示，表格以 Markdown 源码行显示，表格网格与内嵌图片编辑后续扩展。其他 Markdown 仍保留。KeepMD 编辑文档时也使用同一套可视化编辑代码（各自编译进两个 EXE），并保留源码模式；完整阅读范围不变。稳定空闲不运行周期排版计时器。

提示词上限为 1 Mi UTF-16 单元；Markdown 渲染与图表仍受 KeepMD 原有预算限制。大于上限、无效编码、NUL 或损坏的草稿不会被空白草稿覆盖。保存失败时保留输入，磁盘草稿外部变化时停止覆盖，可导出内存版本；关闭时可取消或明确放弃未保存输入。数据为本地明文文件。

“复制全文”和“复制并收起”始终输出 Markdown。源码／预览切换入口已经移除，格式操作直接作用于编辑内容。没有引入浏览器或联网服务。

## 验证与范围

`tests/core_tests.cpp` 覆盖快捷键校验、双 Ctrl 时序、旧 JSON 转义/中文/emoji/损坏输入、设置往返与隔离注册表中的自启动添加/删除。`tools/test_prompt.py` 使用真实全局快捷键、独立进程的原生粘贴目标、Windows 文件对话框和本机中文输入法，验证唤起、焦点、原文复制、冲突恢复、单实例、保存、导入导出、重启和空闲；它直接启动 `KeepPrompt.exe`，默认快捷键被其他程序占用时改用等价的空闲组合键。`tools/test_prompt_process.py` 验证两个程序互不启动、各自保存设置，以及 0.4.x 提示词文件和登录启动项的升级沿用。

0.5.0 的测试桌面上，默认 Ctrl+Alt+Space 被其他程序占用，真实全局唤起以 Ctrl+Alt+Shift+F9 完成；默认键的注册与冲突提示由启动冲突检查覆盖。旧 Prompt Flow 未运行，双击左 Ctrl 唤起及 Ctrl+C 后单按 Ctrl 不误唤起已用真实桌面键盘输入验证。提示词中文输入法提交已通过真实输入验证；Esc 取消组词的按键路径已执行，但没有单独断言，不宣称该分支完整通过。物理跨显示器 DPI 切换和其他 Windows 版本仍未验证。最终执行记录与资源数值见 [交付验证报告](VALIDATION.zh-CN.md) 和 `bench/results/prompt-e2e.json`。
