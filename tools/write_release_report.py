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
for item in (startup,idle,large,imports,smoke):
    assert item['exe_sha256']==sha, 'Results belong to a different executable; rerun affected checks.'
core=subprocess.run([str(ROOT/'build/release/core_tests.exe')],capture_output=True,text=True,check=True)
count=int(re.search(r'(\d+) checks, 0 failures',core.stdout).group(1))
working=statistics.median(r['working_set']for r in startup['records'])/1048576
summary={'version':'0.2.0','generated_at':datetime.now().astimezone().isoformat(),'exe_sha256':sha,'exe_bytes':exe.stat().st_size,'core_checks_passed':count,
         'startup_launch_p95_ms':startup['launch_to_first_paint_p95_ms'],'private_median_mib':startup['private_p50_mib'],'working_set_median_mib':working,
         'large_50mib_launch_p95_ms':large['launch_to_first_paint_p95_ms'],'large_50mib_private_median_mib':large['private_p50_mib'],
         'gui_suites':['reader','navigation and real file dialogs','editor encodings/undo/save/conflicts/cache reuse','native IME keyboard input','DPI target rendering/hit testing','bounded stress and device-target recreation','class style/image formats/uppercase MMD'],
         'limitations':['No verified cold-cache startup number','Physical cross-monitor DPI transitions not exercised','Windows 10/ARM64 not tested','Mermaid flowchart subset; no full Mermaid parity','No complete reader UI Automation text provider','Unsigned portable binary']}
(RESULTS/'release-summary.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
rows='\n'.join(f"| {r['fixture']} | {r['draw_p95_ms']:.2f} ms | {r['dispatch_to_submit_p95_ms']:.2f} ms | {r['samples']} |" for r in scroll['records'])
report=f'''**KeepMD 0.2 交付验证报告**

生成时间：{summary['generated_at']}。目标平台：Windows 11 x64。所有以下正式结果绑定到同一个 Release EXE；历史探索数据保留在源码仓库，不混入最终数据。

EXE 大小：{exe.stat().st_size:,} 字节（{exe.stat().st_size/1024:.1f} KiB）。SHA-256：`{sha}`。

**功能与构建验证**

- {count} 项核心检查通过，覆盖 Markdown 结构/样式/实体/表格/引用、编码、锁定文件保存失败、文本分片、组合字符与 emoji、流程图结构/样式/回退、阅读配置和隔离的注册表注册测试。
- 实际窗口验证通过：目录显示与跳转、中文查找、正文复制、主题、缩放、前后导航、本地链接、真实 Open/Save As 对话框、中文路径、重启位置恢复、失败重载保留编辑。
- 编辑验证通过：UTF-8、UTF-8 BOM、UTF-16 LE/BE；LF/CRLF 与无改动混合换行；撤销/重做、替换、原字节保存、冲突拒绝、取消关闭、未变图表/文字布局复用。
- 本机已安装中文输入法通过真实 `nihao` 键盘输入、候选窗口、空格提交“你好”和保存验证。
- 96/120/144/192 DPI 阅读渲染目标下，文字、组合字符/emoji 复制、图表和链接命中通过；这是单一 96-DPI 桌面上的渲染目标测试，未冒充物理跨显示器测试。
- 长段落、百万字符代码行、超长图表源码、5000 行表格、100 张图片、200 节点图和超限回退压力检查通过；模拟渲染目标重建后资源重新加载通过，静止重绘回归通过。
- 实际截图验证了 classDef 颜色、分组、六边形与回边；像素检查确认 PNG/JPEG/BMP 都已绘制，独立大写 `.MMD` 文件原生呈现。
- 干净构建通过。发现并修复中文 MSVC `/showIncludes` 检测乱码；实际触碰头文件后，Ninja 能重建依赖它的 C++ 文件。应用自有代码本次构建无编译警告；固定 MD4C 源码保留其原有警告，不将其描述为全仓零警告。

功能证据：[编辑](../bench/results/editor-e2e.json)、[导航](../bench/results/navigation-e2e.json)、[输入法](../bench/results/ime-e2e.json)、[DPI](../bench/results/dpi-e2e.json)、[压力](../bench/results/stress.json)、[显示检查](../bench/results/release-smoke.json)。

**启动与内存**

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

PE 导入表只包含 Windows 系统 DLL，没有 Electron、WebView、Chromium、Qt、Node 或额外 MSVC 运行时 DLL。源码编辑器通过系统 `Msftedit.dll` 按需创建。图表失败时显示原始代码，不启动隐藏浏览器。[导入表检查](../bench/results/dependencies.json)

Mermaid 支持明确的 flowchart/graph 子集：200 节点、400 边、64 KiB 源码；其他语法保留源码和诊断。完整 Mermaid、公式、复杂 HTML/CSS、SVG 全特性及导出没有被宣称完成。物理跨显示器切换、Windows 10、ARM64 和完整正文屏幕阅读器支持未验证或未实现。便携程序未做代码签名。

便携包内有使用说明、示例、许可证、此报告、原始结果和 SHA256SUMS。源码包提供完整构建与测试脚本。正式交付摘要见 [release-summary.json](../bench/results/release-summary.json)。
'''
(ROOT/'docs/VALIDATION.zh-CN.md').write_text(report,encoding='utf-8')
print(json.dumps(summary,ensure_ascii=False,indent=2))
