**决策 0002：按需使用 Windows 纯文本 RichEdit**

日期：2026-10-02。状态：已实现并通过编码/保存/撤销/替换的端到端验证。

轻量编辑采用 Windows 自带 `Msftedit.dll` 的 RichEdit 纯文本模式，替代 roadmap 中待验证的 Scintilla 候选。理由是目前只需要源码输入、原生文本操作、撤销/重做与可靠保存；系统控件满足这些需求，便携包无需携带另一个编辑器组件。控件仅在进入编辑时创建，退出编辑时销毁。

它不会输出 RTF，不负责 Markdown 序列化。保存仍由 KeepMD 的编码和原子替换模块处理；预览仍使用相同的 MD4C/DirectWrite/Direct2D 阅读管线。首版编辑源码上限为 8 MiB，阅读上限为 128 MiB；含 NUL 的文本保留只读，避免控件截断。超出编辑范围可继续阅读。

已验证：UTF-8、UTF-8 BOM、UTF-16 LE/BE，LF/CRLF；未编辑保存及另存为保留原字节，包括混合换行；撤销后保存保留原格式；中文与 emoji 文本内容、撤销/重做、查找替换、单次撤销全部替换、保存冲突拒绝、未保存取消关闭、编辑器释放、未变图表和布局复用。

已通过本机已安装中文输入法的真实键盘测试：输入 `nihao`、观察候选窗口、按空格提交“你好”，并保存验证。证据为 `bench/results/ime-e2e.json` 及候选/提交截图。跨物理显示器 DPI 切换尚未实测；阅读目标的 96/120/144/192 DPI 绘制和坐标命中已有独立测试，不将其冒充物理显示器测试。

未来若需要多光标、语法着色或更复杂的超大源码编辑，可通过独立 `Editor` 接口切换 Scintilla；阅读核心和文件格式不会因此改变。

依据：[Microsoft RichEdit 文档](https://learn.microsoft.com/en-us/windows/win32/controls/about-rich-edit-controls)、[纯文本模式](https://learn.microsoft.com/en-us/windows/win32/controls/em-settextmode)。
