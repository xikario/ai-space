# AI空间

独立的 Qt 5 液态文件检索应用，面向银河麒麟 V10 / UKUI X11。由 [UKUI Liquid Desktop](https://github.com/xikario/ukui-liquid-desktop) 的智能空间拆出，保留本地索引、OCR、结构化文档提取、知识库与液态界面；无需启动或安装 Fences。

当前版本 **0.1.0**。这是独立应用的初始版本，不是远程 AI 聊天客户端。默认检索和建库均在本机进行；用户显式配置的 Command / HTTP / D-Bus Provider 可调用外部程序或服务。

## 构建与运行

需要 CMake 3.16+、C++17、Qt 5.12+（Core、Gui、Widgets、DBus、Network）、OpenGL 开发头文件、Zlib、Python 3.7+。常见 Debian/Ubuntu 开发包：`build-essential cmake qtbase5-dev libgl1-mesa-dev zlib1g-dev`。运行需要图形会话及会话 D-Bus；当前重点支持 X11。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
./build/ai-space --show
```

系统缺少 Qt 开发文件时，可用 `-DAI_SPACE_QT_SDK_ROOT=/path/to/sdk`；OpenGL 头文件可由 `-DAI_SPACE_GL_INCLUDE_DIR=/path/to/include` 指定。无需引用另一个项目的源码。

安装到用户目录：

```sh
cmake --install build --prefix "$HOME/.local"
~/.local/bin/ai-space --show
```

安装包括程序、Python 提取工具、第三方纯 Python 依赖、图标、desktop 入口及可导出的查询 Skill。不会安装登录启动项；在 **设置 → 更新策略 → 登录后启动AI空间** 中自行开启。

PDF 提取需 Poppler（`pdftotext` / `pdftoppm`）；OCR 需 Tesseract 及中文语言包；缺失时会报告部分提取或失败，不冒充完整正文。旧 XLS/OLE 的 Python 依赖及许可证随源码提供。

## 使用

打开设置，选择索引范围，先做快全量，再按需执行 OCR 或知识库建库。普通打开只读取已有快照，不自动全盘扫描。保持原有文件夹浏览、格式过滤、正文预览、暂停/续扫、贴边星标以及独立液态设置窗口。

命令行：`--show`、`--hide`、`--toggle`、`--settings`、`--status`、`--quit`、`--autostart`。同一会话只运行一个实例。窗口有自己的图标和任务栏身份；最小化可从任务栏恢复，不提供最大化。

- [索引、检索与界面](docs/SMART_SPACE.md)
- [知识库与命令行](docs/SMART_SPACE_KNOWLEDGE.md)
- [结构化正文提取](docs/SMART_SPACE_STRUCTURED_KNOWLEDGE.md)
- [迁移与进程接口](docs/MIGRATION.md)
- [拆分验证](docs/VALIDATION.md)

## 数据与旧版迁移

首次启动会复制旧 Fences 的智能空间偏好、索引快照、Provider 配置及知识库。源文件保留；新程序使用独立副本，后续改动不互相同步。知识库通过 SQLite 只读连接和 backup API 一致性迁移，包含已提交的 WAL 内容。

| 数据 | 默认位置 |
| --- | --- |
| 配置 | `~/.config/xikario/ai-space.ini` |
| 文件快照 | `~/.cache/xikario/ai-space/smart-space/index.json` |
| 知识库 | `~/.local/share/ai-space/knowledge/smart-space-knowledge.sqlite` |
| Provider | `~/.config/ai-space/providers.json` |

遵循 XDG 环境变量；自定义知识库目录以设置为准。迁移不会覆盖已有 AI空间文件，也不继承 Fences 的自动启动。旧版与独立版同时运行时会各自维护副本，建议完成迁移后只使用独立版索引。

## 外观和资源

复用 Fences 的 FenceGlassRenderer、Snell shader 和缓存材质；检索、滚动及鼠标反馈复用静态玻璃底图。壁纸由文件变化通知触发更新，后台解码，不定时截取屏幕。Fences 存在时可只读跟随其壁纸及视频首帧；否则使用系统壁纸。独立版不播放或解码视频。

扫描、OCR、建库会消耗 CPU / 磁盘；后台低优先级不代表零开销。实际性能取决于文档数量、格式与 OCR 依赖。

## 测试

```sh
cmake --build build -j2
(cd build && ctest --output-on-failure -j2)
```

Python 测试使用临时文档。安装 `xvfb dbus-x11 x11-utils` 后启用真实进程集成测试，覆盖 1 / 1.5 / 2 倍缩放、迁移、单实例、搜索与关闭；不读取真实个人资料。

本项目 GPL-3.0-or-later；来源、原作者和随包依赖许可见 [第三方声明](THIRD_PARTY_NOTICES.md)。
