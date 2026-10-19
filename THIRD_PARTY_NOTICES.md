# 来源与第三方许可

AI空间由 [xikario/ukui-liquid-desktop](https://github.com/xikario/ukui-liquid-desktop) 拆出，来源基线 `fda6f80442db863536696f441ea8ad25162992de`（2026-10-19），同时移入当时工作区的结构化文档提取增强。主体为 GPL-3.0-or-later，保留原文件版权声明；根目录 LICENSE 为 GPL v3 正文。

- `apps/ai-space/src/SmartSpaceWidget.*`、提取工具、图标、FenceGlassRenderer、GlassFinish，以及 `shared/` 的 Qt 公共模块，来自 UKUI Liquid Desktop。WallpaperBackdrop 由原面板的只读壁纸采样器适配。
- `glass.glsl`、`snells-glass.glsl` 直接来源：[xikario/NextKde-kylinos](https://github.com/xikario/NextKde-kylinos/tree/0dcd60cbd8723a6da95349a82fbc63385081c22a)，上游 [SuceV587/NextKde](https://github.com/SuceV587/NextKde)，GPL v3。许可证位于 `shared/liquid-glass/vendor/nextkde-glass/LICENSE` 与 `apps/ai-space/vendor/nextkde-glass/LICENSE`。
- 随包 `olefile 0.47`（BSD）和 `xlrd 2.0.2`（BSD）：版本、作者与完整许可见 `apps/ai-space/scripts/_vendor/README.md`、对应 dist-info 和 LICENSE 文件。

Qt、OpenGL、Zlib、Python、SQLite、Poppler、Tesseract 与系统图标主题为外部依赖；仓库不打包系统二进制、SDK 或用户数据。
