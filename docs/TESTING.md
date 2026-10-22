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

Xvfb 不包含 OEM 窗口管理器和真实显卡驱动。跨架构 CI 也不等同于银河麒麟 OEM 桌面验收。长时间 OCR、断电/SIGKILL 恢复、全部格式内容准确性及 Wayland 仍需要独立验证。

公开仓库只保存可复现的方法与合成测试。个人路径、库规模、文件列表、进程号、安装备份和本机验收日志不属于公共发布材料；需要共享日志时，应先使用临时资料复现并检查脱敏。
