"""Summarize actual test outputs; require benchmark/import hashes to match the shipped EXEs."""
from pathlib import Path
import hashlib
import json
import re
import statistics
import subprocess
from datetime import datetime

ROOT=Path(__file__).resolve().parents[1]
RESULTS=ROOT/'bench/results'
VERSION='0.6.0'
PREVIOUS_TAG='v0.5.0' # Comparison baseline: the previous release's own recorded results, read from its tag.
def read(name):return json.loads((RESULTS/name).read_text(encoding='utf-8-sig'))
def previous(name):
    text=subprocess.run(['git','show',f'{PREVIOUS_TAG}:bench/results/{name}'],cwd=ROOT,capture_output=True,check=True).stdout
    return json.loads(text.decode('utf-8-sig'))
exe=ROOT/'build/release/keepmd.exe';sha=hashlib.sha256(exe.read_bytes()).hexdigest()
prompt_exe=ROOT/'build/release/KeepPrompt.exe';prompt_sha=hashlib.sha256(prompt_exe.read_bytes()).hexdigest()
startup=read('software-s100.json');scroll=read('scroll.json');idle=read('idle.json');large=read('software-l50.json')
imports=read('dependencies.json');smoke=read('release-smoke.json');environment=read('environment.json')
prompt=read('prompt-e2e.json');pm=prompt['measurements'];ui=read('ui-e2e.json');bars=read('scrollbar-e2e.json');visual=read('visual-editor-e2e.json');caption=read('caption-e2e.json');icons=read('icons-e2e.json');toggle=read('toolbar-toggle-e2e.json')
html=read('html-images-e2e.json')
prompt_process=read('prompt-process-e2e.json')
document=read('visual-document-e2e.json');remote=read('remote-images-e2e.json');brand=read('brand-ui-e2e.json');fullscreen=read('fullscreen-e2e.json')
math=read('math-e2e.json')
for item in (startup,idle,large,imports,smoke,ui,bars,visual,caption,icons,toggle,html,prompt_process,document,remote,brand,fullscreen,math):
    assert item['exe_sha256']==sha, 'Results belong to a different executable; rerun affected checks.'
assert prompt_process['prompt_exe_sha256']==prompt_sha and brand['prompt_exe_sha256']==prompt_sha, 'KeepPrompt results belong to a different executable.'
prompt_is_current = prompt.get('exe_sha256') == prompt_sha
core=subprocess.run([str(ROOT/'build/release/core_tests.exe')],capture_output=True,text=True,check=True)
count=int(re.search(r'(\d+) checks, 0 failures',core.stdout).group(1))
before=previous('release-summary.json');before_startup=previous('software-s100.json');before_large=previous('software-l50.json')
prompt_ime_verified = 'installed Chinese IME commits text; Esc cancels composition without dismissing prompt' in prompt['checks']
prompt_ime_committed = prompt_ime_verified or 'installed Chinese IME commits text; Escape path exercised' in prompt['checks']
prompt_complete = prompt.get('suite_status') == 'completed'
double_ctrl = 'double left Ctrl works; Ctrl+C then Ctrl does not summon' in prompt['checks']
prompt_hotkey = prompt.get('hotkey','Ctrl+Alt+Space')
if prompt_ime_verified:
    prompt_ime_note = '提示词可视化编辑的中文提交及 Esc 组词保护已通过本轮检查。'
elif prompt_ime_committed:
    prompt_ime_note = '提示词可视化编辑的中文输入法提交已通过真实输入验证；Esc 取消组词的按键路径已执行，但套件没有单独断言“组词被取消且窗口保留”，不据此宣称该分支完整通过。'
else:
    prompt_ime_note = '提示词可视化编辑的中文 IME 提交及 Esc 组词路径尚未通过完整验证；普通源码编辑的输入法成功不能替代这个分支的验证。'
prompt_scope = ('提示词集成脚本完成，逐项结果见原始记录。' if prompt_complete else
                '提示词完整集成脚本本轮未完成；只将原始记录 checks 中已经完成的步骤列为通过。不得据此宣称旧配置导入、所有快捷键及所有异常保存分支在本版本完整通过。')
if prompt_hotkey != 'Ctrl+Alt+Space':
    prompt_scope += f'本轮以 {prompt_hotkey} 完成真实全局快捷键唤起；默认键本身的注册与冲突提示由“启动冲突”检查覆盖。'
if not prompt_is_current:
    prompt_scope += '该脚本记录绑定的是此前的 EXE，仅作历史诊断，不计入当前 EXE 的功能哈希验证。'
if prompt_complete:
    prompt_memory = f"小草稿收起后观察 {pm['hidden_idle_seconds']} 秒，CPU 增量 {pm['hidden_idle_cpu_seconds']:.5f} 秒，Private Bytes 为 {pm['hidden_private_mib']:.2f} MiB。首次唤起观察包含测试脚本主动等待，不作为精确响应延迟。"
else:
    prompt_memory = '本轮未获得完整的提示词收起后空闲测量；不使用旧 EXE 的数值代替。本报告的阅读启动、内存与空闲数据来自当前 EXE。'
double_ctrl_note = ('双击左 Ctrl 唤起，以及 Ctrl+C 后单按 Ctrl 不误唤起，已用真实桌面键盘输入验证。' if double_ctrl else
                    '记录测试时，原 Prompt Flow 仍运行，未执行会同时唤起旧程序的双 Ctrl 桌面输入序列；双 Ctrl 状态机由核心检查覆盖。')
html_cases={c.get('case'):c for c in html['checks']}
ghidra=html_cases.get('external Ghidra README');typewords=html_cases.get('external TypeWords README')
external=[]
if ghidra:
    bounds=ghidra['logo_pixel_bounds'];external.append(f"Ghidra README 的本地 Logo 实测宽 {bounds[2]-bounds[0]} 像素")
if typewords:external.append('TypeWords README 头部的 HTML 原生显示、根相对路径截图载入')
external_note=('；'.join(external)+'，读取前后两个外部仓库的文件哈希一致。截图仅作本地验证，不将外部项目素材打入制品。') if external else '本轮没有运行外部 README 检查。'
working=statistics.median(r['working_set']for r in startup['records'])/1048576
s100_target_met=startup['first_paint_p95_ms']<=100 and startup['launch_to_first_paint_p95_ms']<=100
summary={'version':VERSION,'generated_at':datetime.now().astimezone().isoformat(),'exe_sha256':sha,'exe_bytes':exe.stat().st_size,'prompt_exe_sha256':prompt_sha,'prompt_exe_bytes':prompt_exe.stat().st_size,'core_checks_passed':count,
         'html_image_e2e_checks':len(html['checks']),
         'prompt_process_e2e_checks':len(prompt_process['checks']),
         'visual_document_e2e_checks':len(document['checks']),'remote_images_e2e_checks':len(remote['checks']),
         'brand_ui_e2e_checks':len(brand['checks']),'fullscreen_e2e_checks':len(fullscreen['checks']),
         'math_e2e_checks':len(math['checks']),'math_formulas_typeset':math['formulas_typeset'],'math_formulas_source':math['formulas_source'],
         'prompt_ime_verified':prompt_ime_verified,'prompt_ime_commit_verified':prompt_ime_committed,
         'prompt_integration_completed':prompt_complete,'prompt_hotkey_exercised':prompt_hotkey,
         'prompt_measurements':pm,'prompt_e2e_checks':len(prompt['checks']),
         'ui_e2e_checks':len(ui['checks']),
         'scrollbar_e2e_checks':len(bars['checks']),'visual_editor_e2e_checks':len(visual['checks']),'caption_e2e_checks':len(caption['checks']),'icon_e2e_checks':len(icons['checks']),'toolbar_toggle_e2e_checks':len(toggle['checks']),
         'startup_launch_p95_ms':startup['launch_to_first_paint_p95_ms'],'private_median_mib':startup['private_p50_mib'],'working_set_median_mib':working,
         'large_50mib_launch_p95_ms':large['launch_to_first_paint_p95_ms'],'large_50mib_private_median_mib':large['private_p50_mib'],
         'validation_retries':{'initial_failures':['test_dpi.py: local-link hit-test timed out','test_visual_editor.py: transient source assertion failure','test_caption.py: foreground focus left test window after Win+Left snap'],
                              'final_results':{'test_dpi.py':'passed on retry','test_visual_editor.py':'passed on retry','test_caption.py':'passed after test harness reactivated the snapped window'},
                              'caption_baseline':'Unmodified v0.5.0 EXE reproduced the same foreground-focus assertion before the harness adjustment.'},
         'previous_release':{'tag':PREVIOUS_TAG,'exe_bytes':before['exe_bytes'],'startup_launch_p50_ms':before_startup['launch_to_first_paint_p50_ms'],
                             'startup_launch_p95_ms':before_startup['launch_to_first_paint_p95_ms'],'private_median_mib':before_startup['private_p50_mib']},
         'gui_suites':['reader','navigation and real file dialogs','editor encodings/undo/save/conflicts/cache reuse','native IME keyboard input','DPI target rendering/hit testing','bounded stress and device-target recreation','class style/image formats/uppercase MMD','README HTML subset and HTML images including external READMEs','network image switch and persistent URL cache with a local HTTP server','KeepMD document visual editing','KeepMD/KeepPrompt independence and legacy upgrade paths','global prompt keyboard/clipboard/focus/import/persistence/conflicts/IME','native visual editing and inline diagram object round trips','custom window caption, native resize/drag, work-area maximize, docking and close guards','shared visual system and compact menus','full screen reading and presenter paging','LaTeX formulas typeset in the reader with find/copy of the LaTeX source, theme and zoom'],
         'limitations':['No verified cold-cache startup number','Physical cross-monitor DPI transitions not exercised','Windows 10/ARM64 not tested','Mermaid flowchart subset; no full Mermaid parity','No complete reader UI Automation text provider','Unsigned portable binary','Visual prompt editor covers common text formatting and 16 inline flowcharts; table grids and inline image editing are not implemented','Custom maximize button does not implement the Windows 11 hover Snap flyout; Win+Arrow docking is tested',
                        'README HTML subset only: no CSS, style or script execution and no arbitrary layout; images inside table cells stay as source','SVG limited to the Direct2D subset: SVGs with text, style sheets, masks, patterns or symbols show a placeholder','Network images load only after the per-document switch is turned on; data: and network-share images are not loaded',
                        'LaTeX formulas: the MicroTeX subset in the reader only; no \\def, \\tag or LaTeX packages; editors show formulas as source',
                        'Ten upstream MicroTeX fonts with unclear licences are not shipped; their symbols are drawn from Windows fonts and \\android is blank']}
if not double_ctrl:
    summary['limitations'].append('Live double-Ctrl summon not exercised while original Prompt Flow is running')
if not prompt_ime_verified:
    summary['limitations'].append('Visual prompt editor: Chinese IME commit verified, Escape composition-cancel not separately asserted.' if prompt_ime_committed else
                                  'Visual prompt editor Chinese IME commit and Escape composition path did not pass verification in this run; source-editor IME success does not cover it.')
if prompt_hotkey != 'Ctrl+Alt+Space':
    summary['limitations'].append(f'Default prompt shortcut Ctrl+Alt+Space was held by another program on the test desktop; live global-hotkey delivery was exercised with {prompt_hotkey}.')
if not prompt_complete:
    summary['gui_suites'] = [s + ' (incomplete)' if s.startswith('global prompt') else s for s in summary['gui_suites']]
    summary['limitations'].append('Full prompt integration E2E did not complete in this run.')
if not s100_target_met:
    summary['limitations'].append(f"S100 warm first paint p95 is above the 100 ms acceptance target ({startup['first_paint_p95_ms']:.1f} ms in-process, {startup['launch_to_first_paint_p95_ms']:.1f} ms from launch).")
(RESULTS/'release-summary.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
rows='\n'.join(f"| {r['fixture']} | {r['draw_p95_ms']:.2f} ms | {r['dispatch_to_submit_p95_ms']:.2f} ms | {r['samples']} |" for r in scroll['records'])
target_note=('按程序内首屏和启动到提交两种口径均达到。' if s100_target_met else
             f"按程序内首屏（{startup['first_paint_p95_ms']:.2f} ms）和启动到提交（{startup['launch_to_first_paint_p95_ms']:.2f} ms）两种口径均未达到；{PREVIOUS_TAG} 的程序内首屏 p95 为 {before_startup['first_paint_p95_ms']:.2f} ms，同样未达到。")
def delta(label,now,then,unit='ms'):
    change=now-then
    return f"{label}{'慢' if unit=='ms' and change>0 else '快' if unit=='ms' else '多' if change>0 else '少'} {abs(change):.1f} {unit}"
comparison='，'.join([delta('S100 启动 p50 ',startup['launch_to_first_paint_p50_ms'],before_startup['launch_to_first_paint_p50_ms']),
                      delta('p95 ',startup['launch_to_first_paint_p95_ms'],before_startup['launch_to_first_paint_p95_ms']),
                      delta('Private Bytes 中位数',startup['private_p50_mib'],before_startup['private_p50_mib'],'MiB'),
                      delta('50 MiB 首屏 p95 ',large['launch_to_first_paint_p95_ms'],before_large['launch_to_first_paint_p95_ms'])])
report=f'''**KeepMD {VERSION} 交付验证报告**

生成时间：{summary['generated_at']}。目标平台：Windows 11 x64。所有以下正式结果绑定到同一组 Release EXE；历史探索数据保留在源码仓库，不混入最终数据。

`keepmd.exe` 大小：{exe.stat().st_size:,} 字节（{exe.stat().st_size/1024:.1f} KiB）。SHA-256：`{sha}`。
`KeepPrompt.exe` 大小：{prompt_exe.stat().st_size:,} 字节（{prompt_exe.stat().st_size/1024:.1f} KiB）。SHA-256：`{prompt_sha}`。

**{VERSION} 的变化与验证**

- **LaTeX 公式。** 阅读视图用固定版本的 MicroTeX 排版 `$…$` 与 `$$…$$`，由 KeepMD 自己的 Direct2D 后端绘制，字体嵌入 EXE，不联网、不启动浏览器。真实窗口中 {math['formulas_typeset']} 个公式排版、{math['formulas_source']} 个无法恢复的公式按源码显示；{len(math['checks'])} 组检查通过：查找命中公式源码、复制得到原样 LaTeX、主题切换与缩放后重新排版。核心检查覆盖离屏绘制的墨迹位于排版框内、绘制后恢复调用方变换、未知命令标红、无法恢复的错误退回源码、“价格 $5 和 $6”与“售价 $ 100 和 $ 200”不被识别为公式、超长段落切块不切开公式，以及替代符号可见。见 [公式验证](../bench/results/math-e2e.json)。
- **公式字体许可。** 只嵌入许可明确的 20 个字体；上游 10 个许可或来源不明的字体不随附，其中的 « » ‰ € µ 等符号改用 Windows 自带字体绘制，`\\android` 不显示。逐个字体的依据见第三方说明。
- **KeepPrompt 托盘与按键。** 运行期间始终显示托盘图标；最小化与关闭都保存并收起到托盘；Esc 不再收起窗口，只关闭设置面板；进程只能从托盘菜单退出。提示词全量套件 {len(prompt['checks'])} 项与视觉系统套件按新行为断言（Esc 后窗口保留、最小化后托盘图标存在、未开启常驻也有托盘图标）。
- **启动开销。** `keepmd.exe` 静态链接 MicroTeX 与公式字体后体积增大，见下文；在不含公式的文档上，MicroTeX 只在首次遇到公式时初始化，其全局表在启动时约多占 0.3 MiB 私有内存。

**0.5.0 引入的能力（本版回归）**

- **两个独立程序。** KeepMD 与 KeepPrompt 只共用界面基础库，互不链接、启动或发送消息，各自保存设置。{len(prompt_process['checks'])} 组检查通过：KeepMD 单独运行且没有提示词入口，旧的 `--prompt` 参数不再启动其他程序；KeepPrompt 隐藏常驻并转发第二次启动；关闭 KeepMD 不影响 KeepPrompt；0.4.x 便携目录与登录启动项升级后原位沿用提示词草稿，0.4.5 的 `keepmd.exe --resident` 启动项静默退出。见 [独立性验证](../bench/results/prompt-process-e2e.json)。
- **README HTML 子集与 HTML 图片。** 常见 README HTML 块转为原生块，子集外的块保留原文。{len(html['checks'])} 组检查覆盖 `<img>` 宽高／缩放／多行属性、中文与空格路径、行内图片、双栏预览、居中与跨块对齐、仓库根相对路径、代码／表格／缺失／远程回退及源码无改动保存；{external_note}见 [HTML 验证](../bench/results/html-images-e2e.json)。
- **KeepMD 文档可视化编辑。** {len(document['checks'])} 组真实键盘检查通过：打开即为可视化编辑且 Markdown 原样保留，Unicode 输入与 Ctrl+B，保存未触碰的表格和流程图，Ctrl+E 双向切换并记住模式，连续输入合并为一步撤销，新建文档进入可视化，含 U+FFFC 的文件保留阅读，超出可视化上限的文件使用源码编辑并说明原因。见 [文档可视化编辑验证](../bench/results/visual-document-e2e.json)。
- **网络图片开关与持久缓存。** 默认不联网；工具栏开关加载当前文档全部 HTTP(S) 图片。{len(remote['checks'])} 组检查由本地 HTTP 服务驱动：占位不触发请求、重复 URL 只下载一次、跨进程从磁盘复用、损坏缓存丢弃、跳转按原 URL 保存且不发送 Cookie／凭据／来源、错误不入缓存、取消与超时、两个窗口合并下载、写缓存失败仍显示、按上限淘汰。见 [网络图片验证](../bench/results/remote-images-e2e.json)。
- **图片失败原因与 SVG。** 占位卡片区分找不到文件、无法读取、无法解码与超出上限。SVG 由 Windows 自带的 Direct2D SVG 支持绘制，按显示尺寸两倍光栅化；Direct2D 不支持文字，含文字、样式表、遮罩、图案或符号的 SVG 显示占位与原因，避免画出缺字的徽章。由核心检查覆盖；实际窗口截图检查在开发构建上完成，没有单列为本版 EXE 的结果文件。
- **全屏阅读。** {len(fullscreen['checks'])} 组真实键盘／指针检查通过：F11 覆盖显示器并隐藏标题栏、工具栏、状态栏和页面卡片；Space／方向键／PageDown 等翻页位移与 PageDown 一致；指针静止 2.5 秒隐藏；查找栏与两次 Esc；最大化窗口往返；编辑器全屏中 Esc 留给编辑器。见 [全屏验证](../bench/results/fullscreen-e2e.json)。
- **查找。** 横跨长段落切块边界的命中两侧都高亮，查找高亮按位置二分选取；由核心检查覆盖，实际窗口截图检查同样在开发构建上完成。
- **视觉系统。** 两个程序统一黑白灰浅／深配色、32 DIP 紧凑标题栏、圆角与焦点边框，模式切换移入标题栏图标。{len(brand['checks'])} 组原生交互检查通过，覆盖 640／850／1180 px 标题栏布局、草稿预览保留撤销、模式切换不改 Markdown、收起工具栏与深色主题下的模式图标、提示词设置与 Esc、模板追加与撤销。见 [视觉系统验证](../bench/results/brand-ui-e2e.json)。

- {count} 项核心检查通过（{PREVIOUS_TAG} 为 {before['core_checks_passed']} 项），覆盖 Markdown 结构/样式/实体/表格/引用、README HTML 子集、编码、锁定文件保存失败、文本分片与跨块查找高亮、组合字符与 emoji、流程图结构/样式/回退、图片失败分类与 SVG 尺寸／光栅化／降级、网络图片 URL 规范化与缓存、阅读配置和隔离的注册表注册测试。
- 实际窗口验证通过：目录显示与跳转、中文查找、正文复制、主题、缩放、前后导航、本地链接、真实 Open/Save As 对话框、中文路径、重启位置恢复、失败重载保留编辑。
- 编辑验证通过：UTF-8、UTF-8 BOM、UTF-16 LE/BE；LF/CRLF 与无改动混合换行；撤销/重做、替换、原字节保存、冲突拒绝、取消关闭、未变图表/文字布局复用。
- 普通源码编辑器的中文输入法通过真实 `nihao` 键盘输入、候选窗口、空格提交“你好”和保存验证。{prompt_ime_note}
- 96/120/144/192 DPI 阅读渲染目标下，文字、组合字符/emoji 复制、图表和链接命中通过；这是单一 96-DPI 桌面上的渲染目标测试，未冒充物理跨显示器测试。
- 长段落、百万字符代码行、超长图表源码、5000 行表格、100 张图片、200 节点图和超限回退压力检查通过；模拟渲染目标重建后资源重新加载通过，静止重绘回归通过。
- 实际截图验证了 classDef 颜色、分组、六边形与回边；像素检查确认 PNG/JPEG/BMP 都已绘制，独立大写 `.MMD` 文件原生呈现。
- 干净构建通过。保留既有中文 MSVC `/showIncludes` 检测修正。应用自有代码本次构建无编译警告；固定 MD4C 源码保留其原有警告，不将其描述为全仓零警告。

功能证据：[编辑](../bench/results/editor-e2e.json)、[导航](../bench/results/navigation-e2e.json)、[输入法](../bench/results/ime-e2e.json)、[DPI](../bench/results/dpi-e2e.json)、[压力](../bench/results/stress.json)、[显示检查](../bench/results/release-smoke.json)。

回归记录：首轮 DPI、视觉编辑器和标题栏套件遇到桌面焦点／窗口状态问题。DPI 与视觉编辑器单独重跑通过；标题栏套件在 Windows 贴靠后失去前台焦点，未修改的 v0.5.0 基线在同一断言复现。测试补上贴靠后重新激活窗口再发送恢复快捷键的步骤，随后当前 0.6.0 EXE 的完整标题栏套件通过。没有修改产品代码。{prompt_scope}

**图标工具栏与双栏滚动**

阅读器及提示词格式栏统一使用原生 GDI 线性图标，常态不显示重复边框；悬停／按下／选中有状态反馈，保留按钮中文名称和键盘操作。“复制并收起”保留图标与短文字。{len(icons['checks'])} 组图标检查通过，包含真实鼠标悬停的中文说明／快捷键、格式操作和撤销、Space 复制，以及 360 次图标绘制和 60 次主题切换后 GDI 对象不累积。

顶栏箭头、菜单和 Ctrl+Shift+T 可收起／展开工具栏。{len(toggle['checks'])} 组状态检查通过，覆盖阅读、源码编辑、双栏预览及提示词模式；收起后编辑区获得更多空间，复制操作仍可用，阅读器和提示词分别记住状态。双栏两个区域的自绘滚动条在控件层级变化、模式切换、主题切换、调整尺寸和原生框架重绘后保持覆盖，避免默认滚动条重新露出。见 [图标](../bench/results/icons-e2e.json)、[工具栏收起展开](../bench/results/toolbar-toggle-e2e.json)、[滚动条](../bench/results/scrollbar-e2e.json)。

**自绘窗口顶栏**

阅读器、提示词与流程图编辑窗口移除 Windows 默认标题栏，改用统一主题的菜单与细线窗口按钮。{len(caption['checks'])} 组端到端检查通过：真实鼠标拖动、边缘缩放、最小化、最大化／还原、双击最大化、最大化客户区与显示器工作区一致、Win+Left 贴靠、菜单点击／F10／Alt+Space、提示词关闭收起、Alt+F4 和阅读器未保存取消关闭。新按钮保留原生 Button 的名称与键盘操作；Windows 11 鼠标悬停最大化按钮的 Snap 布局弹窗未实现，Win+方向键贴靠已实测。物理跨显示器 DPI 变更仍未验证。见 [顶栏验证记录](../bench/results/caption-e2e.json)。

**提示词可视化编辑**

提示词窗口为单栏原生可视化编辑器，没有源码／预览切换。{len(visual['checks'])} 组专项检查通过：语法标记隐藏、标题／加粗／斜体样式、格式按钮、输入与撤销重做、列表续写、流程图图片插入／删除／撤销、中文及 emoji、Markdown 原样复制与保存。图片由本进程绘制并作为静态对象插入系统 RichEdit，不加载浏览器。表格网格、图片编辑与复杂嵌套结构的可视化操作仍有限，原始 Markdown 保留。最多 16 张流程图以内嵌图形显示，更多图表保留为代码。KeepMD 的文档可视化编辑见上文。详见 [可视化编辑验证](../bench/results/visual-editor-e2e.json)。

**界面、滚动条与提示词**

界面通过 {len(ui['checks'])} 组原生窗口检查：阅读器 640px 图标布局、深色主题覆盖工具栏／目录／查找／状态栏和标题菜单、提示词主题独立保存、提示词 720px 设置布局、窗口尺寸保留、按钮键盘操作、菜单导航，以及连续 30 次切换主题后 GDI 对象不累积。测试在 96-DPI 桌面执行；弹出菜单和文件对话框仍使用 Windows 的外观。详见 [UI 验证记录](../bench/results/ui-e2e.json)。

正文、源码编辑、提示词可视化编辑区和目录使用与主题一致的自绘窄滑块：禁止系统滚动条样式，使用 WM_NCCALCSIZE 预留 12 DIP 自管轨道并拦截系统轨道绘制；保留原控件滚动模型。{len(bars['checks'])} 组真实鼠标检查通过，包括纵向拖动超过 65,535、横向代码滚动、轨道翻页、滚轮、键盘 Home、虚拟目录、双栏屏幕像素及提示词编辑区滚动。控件本身不增加空闲轮询定时器；自绘滑块尚未提供完整 UI Automation ScrollPattern，原有键盘滚动可用。详见 [滚动条验证记录](../bench/results/scrollbar-e2e.json)。

提示词独立进程记录 {len(prompt_process['checks'])} 项通过；提示词全量集成记录 {len(prompt['checks'])} 项。{prompt_ime_note} 详见 [提示词验证记录](../bench/results/prompt-e2e.json) 与 [独立进程记录](../bench/results/prompt-process-e2e.json)。

{prompt_memory}

{double_ctrl_note}

**启动与内存**

S100 为约 100 KiB 的中英混排 Markdown。每次新建进程、文件缓存温热，重复 {startup['runs']} 次。正式口径从启动器的 QPC 采样到正文首屏绘制提交，包含进程创建，不包括物理显示器扫描呈现。

| 指标 | 实测 | {PREVIOUS_TAG} |
| --- | --- | --- |
| 启动到正文提交 p50 | {startup['launch_to_first_paint_p50_ms']:.2f} ms | {before_startup['launch_to_first_paint_p50_ms']:.2f} ms |
| 启动到正文提交 p95 | {startup['launch_to_first_paint_p95_ms']:.2f} ms | {before_startup['launch_to_first_paint_p95_ms']:.2f} ms |
| 程序内部首屏 p95（不含进程创建） | {startup['first_paint_p95_ms']:.2f} ms | {before_startup['first_paint_p95_ms']:.2f} ms |
| Private Bytes 中位数 / 最大值 | {startup['private_p50_mib']:.2f} / {startup['private_max_mib']:.2f} MiB | {before_startup['private_p50_mib']:.2f} / {before_startup['private_max_mib']:.2f} MiB |
| Working Set 中位数 | {working:.2f} MiB | — |
| 50 MiB 压力文件首屏 p95（3 次初筛） | {large['launch_to_first_paint_p95_ms']:.2f} ms | {before_large['launch_to_first_paint_p95_ms']:.2f} ms |
| 50 MiB 压力文件 Private Bytes 中位数 | {large['private_p50_mib']:.2f} MiB | {before_large['private_p50_mib']:.2f} MiB |

与 {PREVIOUS_TAG} 相比：{comparison}；同期 `keepmd.exe` 从 {before['exe_bytes']:,} 字节增至 {exe.stat().st_size:,} 字节（加入了 MicroTeX 公式排版代码与 20 个嵌入字体）。差异的具体来源未逐项定位，不将其归因于某一项改动。50 MiB 只有 3 次初筛，波动较大。验收文档的 S100 温热首屏目标为 p95 ≤ 100 ms，{target_note}稳态内存远低于 40 MiB 的目标。

50 MiB 文件的语义文本与块模型仍随文件大小增长，不能把小文档内存数字套到所有文件。正式数值见 [S100 原始结果](../bench/results/software-s100.json)、[50 MiB 结果](../bench/results/software-l50.json)。

本机：{environment['cpu']['Name']}，约 {environment['os']['TotalVisibleMemorySize']/1048576:.1f} GiB 可见内存，Windows {environment['os']['Version']}，默认软件绘制。完整驱动与环境见 [environment.json](../bench/results/environment.json)。没有关闭杀毒以美化数据。30 次的 p95 只作本机工程验证，不代表稳定的跨机器保证；历史运行存在系统状态波动，均保留而非只展示最佳值。冷缓存首屏没有可靠实测结果。

**滚动与空闲**

按每秒 60 次目标频率发送滚动输入，各场景采集约 240 次绘制。下表度量应用处理输入到提交绘制的耗时，不等于物理帧间隔或硬件输入延迟。

| 场景 | 绘制 p95 | 输入分派到提交 p95 | 样本 |
| --- | --- | --- | --- |
{rows}

独立 {idle['seconds']:.1f} 秒静止阅读观察：CPU 时间增加 {idle['cpu_seconds_delta']:.5f} 秒，阅读区域额外绘制 {idle['paint_count_delta']} 次。GPU 进程专用/共享计数器另列原始记录，不与 Private Bytes、Working Set 简单相加，也不把系统 DWM 合成开销算成不存在。[滚动数据](../bench/results/scroll.json)、[空闲与 GPU 观察](../bench/results/idle.json)

**依赖与交付边界**

**默认打开方式**：文件菜单的“设为默认打开程序…”注册当前用户的 KeepMD 后打开 Windows 官方默认应用设置页，由用户确认 `.md`、`.markdown` 和 `.mmd`。程序不直接改写受 UserChoice 保护的默认值。

PE 导入表只包含 Windows 系统 DLL，没有 Electron、WebView、Chromium、Qt、Node 或额外 MSVC 运行时 DLL。源码编辑器通过系统 `Msftedit.dll` 按需创建；SVG 使用系统 Direct2D。图表失败时显示原始代码，不启动隐藏浏览器。网络图片只在用户打开当前文档的开关后通过系统 WinHTTP 下载。[导入表检查](../bench/results/dependencies.json)

Mermaid 支持明确的 flowchart/graph 子集：200 节点、400 边、64 KiB 源码；其他语法保留源码和诊断。完整 Mermaid、公式、任意 HTML/CSS、SVG 文字与全特性没有被宣称完成；PDF／Word 导出与多标签不在路线内。物理跨显示器切换、Windows 10、ARM64 和完整正文屏幕阅读器支持未验证或未实现。便携程序未做代码签名。

便携包内有使用说明、示例、许可证、此报告、原始结果和 SHA256SUMS。源码包提供完整构建与测试脚本。正式交付摘要见 [release-summary.json](../bench/results/release-summary.json)。
'''
(ROOT/'docs/VALIDATION.zh-CN.md').write_text(report,encoding='utf-8')
print(json.dumps(summary,ensure_ascii=False,indent=2))
