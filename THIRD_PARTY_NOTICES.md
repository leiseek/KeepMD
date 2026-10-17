**第三方来源与许可证**

KeepMD 的 Markdown、流程图与 LaTeX 公式能力使用以下固定源码。完整许可证随源码和便携包提供。

| 组件 | 固定来源 | 使用方式 | 许可证 |
| --- | --- | --- | --- |
| MD4C 0.5.2 | `mity/md4c@729e6b8b320caa96328968ab27d7db2235e4fb47` | `md4c.c/.h` 与实体表 `entity.c/.h` 静态编入 | [MIT](third_party/md4c/LICENSE.md) |
| Tinta flowchart | `oipoistar/tinta@db70698e49a1f700c7ad98add1bebe713ac796bd` | `mermaid.cpp/.h` 解析与布局静态编入；KeepMD 提供独立宿主、绘制及能力检查 | [MIT](third_party/tinta/LICENSE) |
| MicroTeX | `NanoMichael/MicroTeX@0e3707f6dafebb121d98b53c64364d16fefe481d` | `src/` 下的公式解析与排版核心静态编入，未修改；不含上游的 GDI+/Cairo/Qt/Skia 后端与示例，KeepMD 提供 Direct2D 绘制 | [MIT](third_party/microtex/LICENSE) |
| MicroTeX 字体 | 同上，`res/fonts/` | 其中 20 个 TrueType 字体作为资源嵌入 `keepmd.exe`，未修改、未改名；另 10 个许可不明的字体不随 KeepMD 分发 | 见下文 |
| TinyXML-2 11.0.0 | `leethomason/tinyxml2@9148bdf719e997d1f474be6bcc7943881046dba1` | `tinyxml2.cpp/.h` 静态编入；MicroTeX 的可选外部字母表读取依赖它，KeepMD 不加载外部字母表 | [zlib](third_party/tinyxml2/LICENSE.txt) |

**公式字体**

MicroTeX 的字体来自 JLaTeXMath。下表依据 JLaTeXMath 许可文件（`opencollab/jlatexmath` 的 `LICENSE`）、各字体文件内嵌的版权信息及 CTAN 上的原始发行条款整理：

| 嵌入的字体 | 内嵌版权 | 许可 |
| --- | --- | --- |
| `cmr10`、`cmmi10`、`cmmib10`、`cmsy10`、`cmbsy10`、`cmbx10`、`cmbxti10`、`cmss10`、`cmssi10` | American Mathematical Society 1997 | JLaTeXMath 列为 [Knuth License](third_party/microtex/res/fonts/licences/Knuth_License.txt)（`cmmib10`、`cmssi10` 未在其清单中，版权方与同列字体相同） |
| `msam10`、`msbm10` | American Mathematical Society 1997 | [SIL Open Font License 1.1](third_party/microtex/res/fonts/licences/OFL.txt)，`msam10`、`msbm10` 为保留字体名 |
| `cmex10`、`cmssbx10`、`cmtt10`、`cmti10` | Basil K. Malyshev，BaKoMa Fonts Collection | JLaTeXMath 列为 Knuth License（`cmti10` 未在其清单中）；BaKoMa 自身的 [许可](third_party/microtex/LICENSE_BAKOMA) 允许出于任何目的免费复制和分发，须随附其版权与许可声明 |
| `eufb10`、`eufm10` | Basil K. Malyshev，BaKoMa Fonts Collection | JLaTeXMath 与上游许可文件列为 SIL Open Font License 1.1（AMS 保留字体名）；同时适用 BaKoMa 许可 |
| `rsfs10` | Taco Hoekwater 1998 | JLaTeXMath 列为公共领域；原始 METAFONT 字体的作者 Ralph Smith 允许自由使用和分发，修改后须改名并注明原作者 |
| `stmary10` | — | JLaTeXMath 列为公共领域；CTAN 上原始 St Mary Road 字体（Jeremy Gibbons、Alan Jeffrey）以 LaTeX Project Public License 发行 |
| `dsrom10` | — | [作者许可](third_party/microtex/res/fonts/licences/License_for_dsrom.txt)：可自由使用和分发，修改后不得使用 Knuth 的字体文件名 |

以下 10 个 MicroTeX 字体**不随 KeepMD 分发**，源码仓库和便携包中都不包含：JLaTeXMath 自制的拉丁文字体 `r10`、`i10`、`bx10`、`bi10`、`sb10`、`sbi10`、`si10`、`ss10`、`tt10`，在 JLaTeXMath 许可文件中没有列出许可；`special` 同样未列出，且其中 Android 图标、TeX 标志与欧元符号的字形来源不明（见 MicroTeX issue #85）。它们只承担少数符号（« » ‹ › ‰ ‱ ☡、€、µ、平行四边形和 TeX 标志字母），KeepMD 以 Windows 自带字体中的对应 Unicode 字符绘制；`\android` 不显示。

原始来源、文件 SHA-256 和下载地址记录在 [third_party/manifest.json](third_party/manifest.json)。仓库中附带的上游 Mermaid 测试文件仅供参考，完整 Tinta 测试不属于 KeepMD 的测试目标；KeepMD 的核心与端到端测试另行维护。

Windows 的 DirectWrite、Direct2D、WIC、RichEdit 和常规系统控件由操作系统提供，不随包复制。Python、Pillow 和 psutil 仅用于开发验证或资源生成，不是应用运行时依赖。
