**第三方来源与许可证**

KeepMD 的 Markdown 与流程图能力使用以下固定源码。完整许可证随源码和便携包提供。

| 组件 | 固定来源 | 使用方式 | 许可证 |
| --- | --- | --- | --- |
| MD4C 0.5.2 | `mity/md4c@729e6b8b320caa96328968ab27d7db2235e4fb47` | `md4c.c/.h` 与实体表 `entity.c/.h` 静态编入 | [MIT](third_party/md4c/LICENSE.md) |
| Tinta flowchart | `oipoistar/tinta@db70698e49a1f700c7ad98add1bebe713ac796bd` | `mermaid.cpp/.h` 解析与布局静态编入；KeepMD 提供独立宿主、绘制及能力检查 | [MIT](third_party/tinta/LICENSE) |

原始来源、文件 SHA-256 和下载地址记录在 [third_party/manifest.json](third_party/manifest.json)。仓库中附带的上游 Mermaid 测试文件仅供参考，完整 Tinta 测试不属于 KeepMD 的测试目标；KeepMD 的核心与端到端测试另行维护。

Windows 的 DirectWrite、Direct2D、WIC、RichEdit 和常规系统控件由操作系统提供，不随包复制。Python、Pillow 和 psutil 仅用于开发验证或资源生成，不是应用运行时依赖。
