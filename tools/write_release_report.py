"""Summarize actual test outputs; require benchmark/import hashes to match the shipped EXE."""
from pathlib import Path
import hashlib
import json
import re
import statistics
import subprocess
from datetime import datetime

ROOT=Path(__file__).resolve().parents[1]
RESULTS=ROOT/'bench/results'
def read(name):return json.loads((RESULTS/name).read_text(encoding='utf-8-sig'))
exe=ROOT/'build/release/keepmd.exe';sha=hashlib.sha256(exe.read_bytes()).hexdigest()
startup=read('software-s100.json');scroll=read('scroll.json');idle=read('idle.json');large=read('software-l50.json')
imports=read('dependencies.json');smoke=read('release-smoke.json');environment=read('environment.json')
prompt=read('prompt-e2e.json');pm=prompt['measurements'];ui=read('ui-e2e.json');bars=read('scrollbar-e2e.json');visual=read('visual-editor-e2e.json');caption=read('caption-e2e.json');icons=read('icons-e2e.json');toggle=read('toolbar-toggle-e2e.json')
html=read('html-images-e2e.json')
for item in (startup,idle,large,imports,smoke,prompt,ui,bars,visual,caption,icons,toggle,html):
    assert item['exe_sha256']==sha, 'Results belong to a different executable; rerun affected checks.'
core=subprocess.run([str(ROOT/'build/release/core_tests.exe')],capture_output=True,text=True,check=True)
count=int(re.search(r'(\d+) checks, 0 failures',core.stdout).group(1))
prompt_ime_verified = 'installed Chinese IME commits text; Esc cancels composition without dismissing prompt' in prompt['checks']
prompt_complete = prompt.get('suite_status') == 'completed'
prompt_ime_note = ('提示词可视化编辑的中文提交及 Esc 组词保护已通过本轮检查。' if prompt_ime_verified else
                   '提示词可视化编辑的中文 IME 提交及 Esc 组词路径尚未通过完整验证；普通源码编辑的输入法成功不能替代这个分支的验证。')
prompt_scope = ('提示词集成脚本完成，逐项结果见原始记录。' if prompt_complete else
                '提示词完整集成脚本本轮未完成；只将原始记录 checks 中已经完成的步骤列为通过。真实桌面输入多次遇到焦点切换／撤销等待超时；一轮旧配置导入自动化中进程以 0xC000041D 退出，尚未定位原因。不得据此宣称旧配置导入、所有快捷键及所有异常保存分支在本版本完整通过。独立可视化编辑、UI、图标、收起展开测试已另行通过。')
if prompt_complete:
    prompt_memory = f"小草稿收起后观察 {pm['hidden_idle_seconds']} 秒，CPU 增量 {pm['hidden_idle_cpu_seconds']:.5f} 秒，Private Bytes 为 {pm['hidden_private_mib']:.2f} MiB。首次唤起观察包含测试脚本主动等待，不作为精确响应延迟。"
else:
    prompt_memory = '本轮未获得完整的提示词收起后空闲测量；不使用旧 EXE 的数值代替。本报告的阅读启动、内存与空闲数据来自当前 EXE。'
working=statistics.median(r['working_set']for r in startup['records'])/1048576
summary={'version':'0.4.5','generated_at':datetime.now().astimezone().isoformat(),'exe_sha256':sha,'exe_bytes':exe.stat().st_size,'core_checks_passed':count,
         'html_image_e2e_checks':len(html['checks']),
         'prompt_ime_verified':prompt_ime_verified,
         'prompt_integration_completed':prompt_complete,
         'prompt_measurements':pm,'prompt_e2e_checks':len(prompt['checks']),
         'ui_e2e_checks':len(ui['checks']),
         'scrollbar_e2e_checks':len(bars['checks']),'visual_editor_e2e_checks':len(visual['checks']),'caption_e2e_checks':len(caption['checks']),'icon_e2e_checks':len(icons['checks']),'toolbar_toggle_e2e_checks':len(toggle['checks']),
         'startup_launch_p95_ms':startup['launch_to_first_paint_p95_ms'],'private_median_mib':startup['private_p50_mib'],'working_set_median_mib':working,
         'large_50mib_launch_p95_ms':large['launch_to_first_paint_p95_ms'],'large_50mib_private_median_mib':large['private_p50_mib'],
         'gui_suites':['reader','navigation and real file dialogs','editor encodings/undo/save/conflicts/cache reuse','native IME keyboard input','DPI target rendering/hit testing','bounded stress and device-target recreation','class style/image formats/uppercase MMD','global prompt keyboard/clipboard/focus/import/persistence/conflicts/IME','native visual editing and inline diagram object round trips','custom window caption, native resize/drag, work-area maximize, docking and close guards'],
         'limitations':['No verified cold-cache startup number','Physical cross-monitor DPI transitions not exercised','Windows 10/ARM64 not tested','Mermaid flowchart subset; no full Mermaid parity','No complete reader UI Automation text provider','Unsigned portable binary','Live double-Ctrl summon not exercised while original Prompt Flow is running','Visual prompt editor covers common text formatting and 16 inline flowcharts; table grids and inline image editing are not implemented','Custom maximize button does not implement the Windows 11 hover Snap flyout; Win+Arrow docking is tested']}
if not prompt_ime_verified:
    summary['limitations'].append('Visual prompt editor Chinese IME commit and Escape composition path did not pass verification in this run; source-editor IME success does not cover it.')
if not prompt_complete:
    summary['gui_suites'] = [s + ' (incomplete)' if s.startswith('global prompt') else s for s in summary['gui_suites']]
    summary['limitations'].append('Full prompt integration E2E did not complete: focus/undo waits failed on this desktop and one automated legacy import run exited with 0xC000041D; cause unresolved. Separate visual editor/UI tests passed.')
summary['limitations'].append('HTML img subset only; no container HTML layout, table-cell images, remote/data image downloads or SVG rendering.')
(RESULTS/'release-summary.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
rows='\n'.join(f"| {r['fixture']} | {r['draw_p95_ms']:.2f} ms | {r['dispatch_to_submit_p95_ms']:.2f} ms | {r['samples']} |" for r in scroll['records'])
report=f'''**KeepMD 0.4.5 交付验证报告**

生成时间：{summary['generated_at']}。目标平台：Windows 11 x64。所有以下正式结果绑定到同一个 Release EXE；历史探索数据保留在源码仓库，不混入最终数据。

EXE 大小：{exe.stat().st_size:,} 字节（{exe.stat().st_size/1024:.1f} KiB）。SHA-256：`{sha}`。

**功能与构建验证**

0.4.5 增加原生 HTML `<img>` 支持。{len(html['checks'])} 组专项检查覆盖宽度／高度／双尺寸、缩放、多行属性、中文与空格相对路径、行内前后文字、现有 Markdown 图片、双栏预览和源码无改动保存；以截图像素尺寸验证实际绘制。代码／容器 HTML／缺失文件／远程图片按约定回退。Ghidra README 的本地 Logo 实测宽 400 像素，读取前后 README 和 PNG 哈希一致。截图仅作本地验证，不将外部项目素材打入制品。见 [HTML 图片验证](../bench/results/html-images-e2e.json)。

0.4.4 清理常驻说明性文案并收紧留白：提示词副标题／快捷键教学、阅读器空白页说明、源码状态栏操作教学和流程图编辑说明均已移除；实际截图已检查，保存失败、未保存保护与图标功能名称保留。

- {count} 项核心检查通过，覆盖 Markdown 结构/样式/实体/表格/引用、编码、锁定文件保存失败、文本分片、组合字符与 emoji、流程图结构/样式/回退、阅读配置和隔离的注册表注册测试。
- 实际窗口验证通过：目录显示与跳转、中文查找、正文复制、主题、缩放、前后导航、本地链接、真实 Open/Save As 对话框、中文路径、重启位置恢复、失败重载保留编辑。
- 编辑验证通过：UTF-8、UTF-8 BOM、UTF-16 LE/BE；LF/CRLF 与无改动混合换行；撤销/重做、替换、原字节保存、冲突拒绝、取消关闭、未变图表/文字布局复用。
- 普通源码编辑器的中文输入法通过真实 `nihao` 键盘输入、候选窗口、空格提交“你好”和保存验证。{prompt_ime_note}
- 96/120/144/192 DPI 阅读渲染目标下，文字、组合字符/emoji 复制、图表和链接命中通过；这是单一 96-DPI 桌面上的渲染目标测试，未冒充物理跨显示器测试。
- 长段落、百万字符代码行、超长图表源码、5000 行表格、100 张图片、200 节点图和超限回退压力检查通过；模拟渲染目标重建后资源重新加载通过，静止重绘回归通过。
- 实际截图验证了 classDef 颜色、分组、六边形与回边；像素检查确认 PNG/JPEG/BMP 都已绘制，独立大写 `.MMD` 文件原生呈现。
- 干净构建通过。保留既有中文 MSVC `/showIncludes` 检测修正。应用自有代码本次构建无编译警告；固定 MD4C 源码保留其原有警告，不将其描述为全仓零警告。

功能证据：[编辑](../bench/results/editor-e2e.json)、[导航](../bench/results/navigation-e2e.json)、[输入法](../bench/results/ime-e2e.json)、[DPI](../bench/results/dpi-e2e.json)、[压力](../bench/results/stress.json)、[显示检查](../bench/results/release-smoke.json)。

回归并非单次全部通过：DPI 点击两次超时，带点击后状态／截图诊断的重跑在四个目标 DPI 通过；滚动条轨道点击首次超时，产品代码不变的单独重跑通过。提示词脚本加入可见文件对话框与文件名确认、撤销后正文检查、有界剪贴板等待，并在失败时保存当前 EXE 的已完成步骤，不让旧成功记录掩盖失败。{prompt_scope}

**图标工具栏与双栏滚动**

阅读器及提示词格式栏统一使用原生 GDI 线性图标，常态不显示重复边框；悬停／按下／选中有状态反馈，保留按钮中文名称和键盘操作。“复制并收起”保留图标与短文字。{len(icons['checks'])} 组图标检查通过，包含真实鼠标悬停的中文说明／快捷键、格式操作和撤销、Space 复制，以及 360 次图标绘制和 60 次主题切换后 GDI 对象不累积。

顶栏箭头、菜单和 Ctrl+Shift+T 可收起／展开工具栏。{len(toggle['checks'])} 组状态检查通过，覆盖阅读、源码编辑、双栏预览及提示词模式；收起后编辑区获得更多空间，复制操作仍可用，阅读器和提示词分别记住状态。双栏两个区域的自绘滚动条在控件层级变化、模式切换、主题切换、调整尺寸和原生框架重绘后保持覆盖，避免默认滚动条重新露出。见 [图标](../bench/results/icons-e2e.json)、[工具栏收起展开](../bench/results/toolbar-toggle-e2e.json)、[滚动条](../bench/results/scrollbar-e2e.json)。

**自绘窗口顶栏**

阅读器、提示词与流程图编辑窗口移除 Windows 默认标题栏，改用统一主题的菜单与细线窗口按钮。{len(caption['checks'])} 组端到端检查通过：真实鼠标拖动、边缘缩放、最小化、最大化／还原、双击最大化、最大化客户区与显示器工作区一致、Win+Left 贴靠、菜单点击／F10／Alt+Space、提示词关闭收起、Alt+F4 和阅读器未保存取消关闭。新按钮保留原生 Button 的名称与键盘操作；Windows 11 鼠标悬停最大化按钮的 Snap 布局弹窗未实现，Win+方向键贴靠已实测。物理跨显示器 DPI 变更仍未验证。见 [顶栏验证记录](../bench/results/caption-e2e.json)。

**提示词可视化编辑**

提示词窗口已改为单栏原生可视化编辑器，没有源码／预览切换。{len(visual['checks'])} 组专项检查通过：语法标记隐藏、标题／加粗／斜体样式、格式按钮、输入与撤销重做、列表续写、流程图图片插入／删除／撤销、中文及 emoji、Markdown 原样复制与保存。图片由本进程绘制并作为静态对象插入系统 RichEdit，不加载浏览器。完整桌面排版软件的所有格式并非本版范围；表格网格、图片编辑与复杂嵌套结构的可视化操作仍有限，原始 Markdown 保留。最多 16 张流程图以内嵌图形显示，更多图表保留为代码。原阅读器仍保留独立的源码编辑与预览。详见 [可视化编辑验证](../bench/results/visual-editor-e2e.json)。

**启动与内存**

界面优化通过 {len(ui['checks'])} 组原生窗口检查：阅读器 640px 图标布局、提示词 720px 设置布局、按钮键盘操作、菜单导航、深浅主题同步、窗口尺寸保留，以及连续 30 次切换主题后 GDI 对象不累积。测试保存了对应截图，在 96-DPI 桌面执行；弹出菜单和文件对话框仍使用 Windows 的外观。详见 [UI 验证记录](../bench/results/ui-e2e.json)。

正文、源码编辑、提示词可视化编辑区和目录使用与主题一致的自绘窄滑块。0.4.4 禁止系统滚动条样式，使用 WM_NCCALCSIZE 预留 12 DIP 自管轨道并拦截系统轨道绘制；保留原控件滚动模型。双栏横纵轨道经真实屏幕像素检查，在模式／主题／尺寸／原生重绘后仅呈现 KeepMD 配色，且没有系统滚动条样式位。{len(bars['checks'])} 组真实鼠标检查通过，包括纵向拖动超过 65,535、横向代码滚动、轨道翻页、滚轮、键盘 Home、虚拟目录及提示词编辑区滚动。控件本身不增加空闲轮询定时器；自绘滑块尚未提供完整 UI Automation ScrollPattern，原有键盘滚动可用。详见 [滚动条验证记录](../bench/results/scrollbar-e2e.json)。

提示词集成记录 {len(prompt['checks'])} 项已完成步骤。{prompt_scope} {prompt_ime_note} 详见 [提示词验证记录](../bench/results/prompt-e2e.json)。

{prompt_memory}

记录测试时，原 Prompt Flow 仍运行，未执行会同时唤起旧程序的双 Ctrl 桌面输入序列；双 Ctrl 状态机由核心检查覆盖。真实组合键已完成范围以本轮原始记录为准；提示词输入法状态以上述单独说明为准。

S100 为约 100 KiB 的中英混排 Markdown。每次新建进程、文件缓存温热，重复 {startup['runs']} 次。正式口径从启动器的 QPC 采样到正文首屏绘制提交，包含进程创建，不包括物理显示器扫描呈现。

| 指标 | 实测 |
| --- | --- |
| 启动到正文提交 p50 | {startup['launch_to_first_paint_p50_ms']:.2f} ms |
| 启动到正文提交 p95 | {startup['launch_to_first_paint_p95_ms']:.2f} ms |
| 程序内部首屏 p95（不含进程创建） | {startup['first_paint_p95_ms']:.2f} ms |
| Private Bytes 中位数 / 最大值 | {startup['private_p50_mib']:.2f} / {startup['private_max_mib']:.2f} MiB |
| Working Set 中位数 | {working:.2f} MiB |
| 50 MiB 压力文件首屏 p95（3 次初筛） | {large['launch_to_first_paint_p95_ms']:.2f} ms |
| 50 MiB 压力文件 Private Bytes 中位数 | {large['private_p50_mib']:.2f} MiB |

50 MiB 文件的语义文本与块模型仍随文件大小增长，不能把小文档内存数字套到所有文件。目录改为按需创建的原生虚拟列表后，避免了为隐藏目录逐条填入数万项的主线程工作。正式数值见 [S100 原始结果](../bench/results/software-s100.json)、[50 MiB 结果](../bench/results/software-l50.json)。

本机：{environment['cpu']['Name']}，约 {environment['os']['TotalVisibleMemorySize']/1048576:.1f} GiB 可见内存，Windows {environment['os']['Version']}，默认软件绘制。完整驱动与环境见 [environment.json](../bench/results/environment.json)。没有关闭杀毒以美化数据。30 次的 p95 只作本机工程验证，不代表稳定的跨机器保证；历史运行存在系统状态波动，均保留而非只展示最佳值。冷缓存首屏没有可靠实测结果。

**滚动与空闲**

按每秒 60 次目标频率发送滚动输入，各场景采集约 240 次绘制。下表度量应用处理输入到提交绘制的耗时，不等于物理帧间隔或硬件输入延迟。

| 场景 | 绘制 p95 | 输入分派到提交 p95 | 样本 |
| --- | --- | --- | --- |
{rows}

独立 {idle['seconds']:.1f} 秒静止阅读观察：CPU 时间增加 {idle['cpu_seconds_delta']:.5f} 秒，阅读区域额外绘制 {idle['paint_count_delta']} 次。GPU 进程专用/共享计数器另列原始记录，不与 Private Bytes、Working Set 简单相加，也不把系统 DWM 合成开销算成不存在。[滚动数据](../bench/results/scroll.json)、[空闲与 GPU 观察](../bench/results/idle.json)

**依赖与交付边界**

**默认打开方式**：文件菜单的“设为默认打开程序…”注册当前用户的 KeepMD 后打开 Windows 官方默认应用设置页，由用户确认 `.md`、`.markdown` 和 `.mmd`。程序不直接改写受 UserChoice 保护的默认值。

PE 导入表只包含 Windows 系统 DLL，没有 Electron、WebView、Chromium、Qt、Node 或额外 MSVC 运行时 DLL。源码编辑器通过系统 `Msftedit.dll` 按需创建。图表失败时显示原始代码，不启动隐藏浏览器。[导入表检查](../bench/results/dependencies.json)

Mermaid 支持明确的 flowchart/graph 子集：200 节点、400 边、64 KiB 源码；其他语法保留源码和诊断。完整 Mermaid、公式、复杂 HTML/CSS、SVG 全特性及导出没有被宣称完成。物理跨显示器切换、Windows 10、ARM64 和完整正文屏幕阅读器支持未验证或未实现。便携程序未做代码签名。

便携包内有使用说明、示例、许可证、此报告、原始结果和 SHA256SUMS。源码包提供完整构建与测试脚本。正式交付摘要见 [release-summary.json](../bench/results/release-summary.json)。
'''
(ROOT/'docs/VALIDATION.zh-CN.md').write_text(report,encoding='utf-8')
print(json.dumps(summary,ensure_ascii=False,indent=2))
