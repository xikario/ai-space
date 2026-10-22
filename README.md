# AI空间

独立的 Qt 5 本地文件检索应用，面向银河麒麟 V10 / UKUI X11。由 [UKUI Liquid Desktop](https://github.com/xikario/ukui-liquid-desktop) 的智能空间拆出，保留本地索引、OCR、结构化文档提取、知识库与独立界面；无需启动或安装 Fences。

当前版本 **0.1.1**。这是独立应用的初始版本，不是远程 AI 聊天客户端。默认检索和建库均在本机进行；用户显式配置的 Command / HTTP / D-Bus Provider 可调用外部程序或服务。

## 构建与运行

需要 CMake 3.16+、C++17、Qt 5.12+（Core、Gui、Widgets、DBus、Network）、Zlib、Python 3.8+。常见 Debian/Ubuntu 开发包：`build-essential cmake qtbase5-dev zlib1g-dev`。运行需要图形会话及会话 D-Bus；当前重点支持 X11。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
./build/ai-space --show
```

开发构建会在构建目录生成相对路径的运行时副本；正式安装仅从自身安装前缀定位脚本和 Skill，不回退到开发者源码目录。源码中的 Python 工具修改后需重新执行构建以同步副本。

系统缺少 Qt 开发文件时，可用 `-DAI_SPACE_QT_SDK_ROOT=/path/to/sdk`。无需引用另一个项目的源码。

安装到用户目录：

```sh
cmake --install build --prefix "$HOME/.local"
~/.local/bin/ai-space --show
```

安装包括程序、Python 提取工具、第三方纯 Python 依赖、图标、desktop 入口及可导出的查询 Skill。不会安装登录启动项；在 **设置 → 更新策略 → 登录后启动AI空间** 中自行开启。

PDF 提取需 Poppler（`pdftotext` / `pdftoppm`）；OCR 需 Tesseract 及中文语言包；缺失时会报告部分提取或失败，不冒充完整正文。旧 XLS/OLE 的 Python 依赖及许可证随源码提供。

旧版 PowerPoint 97–2003（`.ppt`）及采用相同结构的 WPS 演示（`.dps`）可直接提取页面文字并保留页码，无需启动办公软件或先转为 PPTX。已有失败项可用「重新处理选中」补入；图片文字、备注及嵌入对象不在本次提取范围，会明确标为部分提取，详见[旧版演示兼容与验证](docs/EXTRACTION.md)。

## 使用

从底部「知识库」打开管理与建库向导，先选择有足够空间的专用存储目录，设置单库上限和磁盘保留空间；按「全量文件清单 → 文本正文 → 可选 OCR」分阶段建库。普通打开不自动全盘扫描。支持 .aikb 单文件知识库导入/导出、只读挂载、版本切换及无原文件正文检索；查询与后台建库分开执行。

当前分词索引面向中文、拉丁字母与数字。俄文、日文假名、韩文等尚未建立可靠的词项索引；此类查询会明确标为语言覆盖不足，不应把零结果当作库内不存在。

命令行：`--show`、`--hide`、`--toggle`、`--settings`、`--status`、`--quit`、`--autostart`。旧 `--hide` 参数兼容为最小化到任务栏。同一会话只运行一个实例。窗口有自己的图标和任务栏身份；支持最小化、最大化/还原及标题栏拖动。Ctrl+K 随时返回搜索；宽窗口预览与结果并排，窄窗口以预览页显示。

- [知识库 v1：存储、AIKB、SKILL 与建库向导](docs/KNOWLEDGE_LIBRARIES.md)
- [构建、测试与验证边界](docs/TESTING.md)
- [失败项重试、忽略与恢复](docs/FAILURES.md)
- [索引、检索与界面](docs/SMART_SPACE.md)
- [知识库与命令行](docs/SMART_SPACE_KNOWLEDGE.md)
- [结构化正文提取](docs/SMART_SPACE_STRUCTURED_KNOWLEDGE.md)
- [迁移与进程接口](docs/MIGRATION.md)

## 数据与旧版迁移

首次启动可导入旧 Fences 的界面偏好和文件快照，但不再自动复制多 GB 知识库。已有独立库继续使用；未迁移时可只读查询旧库。需要搬运或共享时，显式选择目标盘并导出/导入 .aikb，避免登录时无提示地占用大量磁盘。新位置建库成功后切换，原库保留为只读来源。

| 数据 | 默认位置 |
| --- | --- |
| 配置 | `~/.config/xikario/ai-space.ini` |
| 文件快照 | `~/.cache/xikario/ai-space/smart-space/index.json` |
| 知识库 | `~/.local/share/ai-space/knowledge/smart-space-knowledge.sqlite` |
| 挂载库注册表 | `~/.config/ai-space/libraries.json` |
| 新建清单 / OCR 缓存 | 所选知识库目录下 `catalog/` / `ocr-cache/` |
| Provider | `~/.config/ai-space/providers.json` |

遵循 XDG 环境变量；自定义知识库目录以设置为准。迁移不会覆盖已有 AI空间文件，也不继承 Fences 的自动启动。旧版与独立版同时运行时会各自维护副本，建议完成迁移后只使用独立版索引。

## 外观和资源

采用不透明的纯色明暗主题，不读取桌面壁纸，不生成玻璃材质，不执行模糊或折射 shader。保留普通窗口圆角和矢量按钮；贴边隐藏、星标入口和收缩动画已移除。标题栏月亮/太阳按钮切换主题。

扫描、OCR、建库会消耗 CPU / 磁盘。默认保留至少 2 GiB、单库上限 32 GiB，可自行调整；OCR 默认单任务单线程、约 25% 单核时间片和 1 GiB 进程地址空间上限，内存紧张时暂停。逐页断点缓存最多 512 MiB，存于所选盘。实际性能仍取决于文档数量、格式与 OCR 依赖。

## 测试

```sh
cmake --build build -j2
(cd build && ctest --output-on-failure -j2)
```

Python 测试使用临时文档。安装 `xvfb dbus-x11 x11-utils` 后启用真实进程集成测试，覆盖 1 / 1.5 / 2 倍缩放、迁移、单实例、搜索与关闭；不读取真实个人资料。

本项目 GPL-3.0-or-later；来源、原作者和随包依赖许可见 [第三方声明](THIRD_PARTY_NOTICES.md)。
