# 构建、测试与验证边界

从仓库根目录执行：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
(cd build && ctest --output-on-failure -j2)
cmake --install build --prefix "$PWD/build/staging"
```

测试覆盖文档提取、建库与恢复、AIKB 导入导出、离线检索、失败项操作、资源限制、窗口缩放、迁移及独立安装。GUI 测试使用临时资料、临时 HOME、私有 Xvfb 和 D-Bus，不依赖开发者的个人库。

真实 OCR 测试需要 Poppler、Tesseract、简体中文语言包、Pillow 和 ReportLab。缺少依赖时部分用例会跳过；阅读测试输出时必须区分通过、失败和跳过。持续集成应在测试前检查依赖，不能把跳过当作支持已获验证。

Xvfb 不包含 OEM 窗口管理器和真实显卡驱动。跨架构 CI 也不等同于银河麒麟 OEM 桌面验收。长时间 OCR、真实断电恢复、全部格式内容准确性及 Wayland 仍需要独立验证。

公开仓库只保存可复现的方法与合成测试。个人路径、库规模、文件列表、进程号、安装备份和本机验收日志不属于公共发布材料；需要共享日志时，应先使用临时资料复现并检查脱敏。

CI 配置包括 x86_64、ARM64 和 Qt 5.12 / Python 3.8 兼容环境，并要求简体中文 OCR 依赖和真实中文图片识别。新增 job 需在远端实际执行通过后才能算作验证证据；不能仅凭工作流配置宣称麒麟 OEM 支持。Python 3.7 未经验证，不再列入支持下限。

补充回归入口：

```sh
python3 -m unittest discover -s apps/ai-space/tests -p test_data_boundaries.py -v
python3 -m unittest discover -s apps/ai-space/tests -p test_review_regressions.py -v
(cd build && ctest -R worker-protocol --output-on-failure)
```

压力夹具覆盖大清单的 Python 堆内存、两万条失败记录的分页、无重命名/硬链接能力的发布降级、损坏注册表和 OCR 缓存、格式伪装、JSON 协议与真实后代进程回收。它们使用临时虚构资料，不能替代真实 USB / OEM GPU / 断电验收。

可选内存与未定义行为检查使用独立构建目录：

```sh
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DAI_SPACE_SANITIZERS=ON
cmake --build build-sanitize -j2
(cd build-sanitize && ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 ctest -R 'worker-protocol|async' --output-on-failure)
```

此命令关闭 Qt / 系统库的泄漏检测，仅检查已执行路径的 ASan / UBSan 错误；没有跑到的路径仍未经验证。

恢复回归会强杀真实流水线父进程，验证仍在解析的子任务继续持锁、第二个任务被拒绝；终止其独立进程组后，再启动扫描会回收中断会话，并核对之前已提交正文仍在、测试库通过 SQLite 完整性检查。此测试使用可控的慢解析器，不代替真实断电或所有第三方程序的故障验证。
