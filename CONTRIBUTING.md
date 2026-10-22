# 参与开发

请按功能或缺陷拆分提交。文档脱敏、行为修复、格式整理和大文件拆分尽量独立提交，以便评审和回退。不要在修改行为时同时批量格式化整个仓库。

提交前执行 README 中的构建与测试命令。界面回归使用临时 HOME、私有 Xvfb / D-Bus；新增测试控制方法仅可放在不安装的测试程序中，不能扩大正式程序的公共接口。不要以跳过测试代替平台验证。

公开仓库不收录个人资料、实际知识库、真实文件列表、用户配置、安装备份或本机验收记录。文档路径使用 `~` 或占位符；缺陷报告先用虚构资料复现。提交前检查 `git diff --cached`，并运行：

```sh
python3 apps/ai-space/tests/publication_test.py .
git diff --cached --check
```

发布检查只能发现部分常见泄漏，不能代替人工核对。不要把真实客户正文、访问令牌或包含完整个人路径的截图加入 issue、日志或测试夹具。

Python 新代码请使用 UTF-8、清晰的控制流和独立语句；C++ 新代码遵循周边 Qt 风格。单一版本来源为根目录 CMake 的 `project(... VERSION ...)`。新增文档入口应链接到 README，使用稳定名称说明安装、使用或故障排查，避免发布私人过程记录。

开发检查（与应用运行环境分开）：

```sh
python3 -m venv .venv
.venv/bin/pip install -r requirements-dev.txt
.venv/bin/python tools/check_python.py
python3 tools/check_cpp.py
```

Python 风格工具固定版本，检查脚本同时核对当前解释器语法和自有生产代码的静态诊断；Python 3.8 下限由专用兼容矩阵实际编译全部脚本并运行测试；第三方 `_vendor` 保持原样。C++ 的标准版本为 clang-format-14，需另行安装。需要整理时给检查脚本加 `--write`，并将纯格式改动与行为改动分开提交。CI 另外执行选定进程和异步路径的 ASan/UBSan 检查；这不能替代完整业务回归。
