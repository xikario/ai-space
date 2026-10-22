# 来源与第三方许可

AI空间由 [xikario/ukui-liquid-desktop](https://github.com/xikario/ukui-liquid-desktop) 拆出，来源基线 `fda6f80442db863536696f441ea8ad25162992de`（2026-10-19），同时移入当时工作区的结构化文档提取增强。主体为 GPL-3.0-or-later，保留原文件版权声明；根目录 LICENSE 为 GPL v3 正文。

- `apps/ai-space/src/SmartSpaceWidget.*`、提取工具、图标以及 `shared/` 中保留的 Qt 公共模块，来自 UKUI Liquid Desktop。独立应用已移除未使用的液态玻璃渲染器、着色器与壁纸采样器；历史来源保留在 Git 记录中。
- 随包 `olefile 0.47`（BSD）和 `xlrd 2.0.2`（BSD）：版本、作者与完整许可见 `apps/ai-space/scripts/_vendor/README.md`、对应 dist-info 和 LICENSE 文件。
- `apps/ai-space/scripts/legacy_ppt.py` 是参考微软公开 MS-PPT 文件格式规范编写的本项目原创解析代码，使用随包 `olefile` 读取 OLE 数据流；没有复制或打包 catppt、LibreOffice 等项目的代码或二进制。

Qt、OpenGL、Zlib、Python、SQLite、Poppler、Tesseract 与系统图标主题为外部依赖；仓库不打包系统二进制、SDK 或用户数据。
