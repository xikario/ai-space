# 代码导航与运行边界

下表入口均相对于 `apps/ai-space/`。

| 模块 | 入口 | 职责 |
| --- | --- | --- |
| 进程与会话接口 | `src/main.cpp`、`src/AiSpaceController.cpp` | 单实例、正式 D-Bus 接口、窗口状态、退出协调 |
| 主窗口 | `src/SmartSpaceWidget.cpp` | 布局、主题、结果卡片、目录导航、窗口交互 |
| 设置与知识库操作 | `src/SettingsPage.cpp`、`src/KnowledgeOperations.cpp` | 设置页、分阶段任务、查询结果应用、状态详情 |
| 管理与失败项 | `src/KnowledgeManagerDialog.cpp`、`src/KnowledgeFailuresDialog.cpp` | 存储向导、挂载版本、重试、分类和忽略 |
| 后台进程 | `src/KnowledgeJobController.cpp`、`src/JsonWorkerRequest.cpp` | 建库进程组生命周期；有界 JSON 请求、进度、超时与错误 |
| 清单加载与预览 | `src/IndexSnapshotLoader.cpp`、`src/PreviewPane.cpp` | 后台读取流式清单、正文与图片预览 |
| 清单与续跑 | `scripts/smart_space_indexer.py`、`scripts/snapshot_io.py` | 文件清单、磁盘复用缓存、两种清单格式、单次目录遍历 |
| 建库与资源 | `scripts/knowledge_jobs.py`、`scripts/job_recovery.py`、`scripts/ocr_budget.py` | 扫描 → 文本 → 可选 OCR；资源限额、互斥和中断会话恢复 |
| 提取 | `scripts/smart_space_extract.py`、`scripts/text_safety.py`、`scripts/xml_safety.py`、`scripts/legacy_ppt.py` | 格式识别、有界文本、结构化正文、旧版演示和明确的部分提取 |
| 知识库与查询 | `scripts/smart_space_knowledge.py`、`scripts/knowledge_library.py`、`scripts/failure_store.py` | SQLite / FTS、AIKB、索引迁移、只读查询、分类与修订号 |
| OCR 与扩展数据源 | `scripts/ocr_environment.py`、`scripts/ocr_cache.py`、`scripts/provider_policy.py` | 语言预检、容量账本、外部程序身份和配置授权 |
| Agent Skill | `skills/ai-space-index-query/scripts/query.py` | 相邻独立查询核心优先；无应用、无原文件的 AIKB 检索 |

小型共用呈现逻辑在 `src/SpacePresentation`、`src/SpaceFormats`、`src/UiMessages`、`src/WindowCornerFallback` 中。

界面不执行全文提取。建库进程负责事务与资源预算，查询使用独立只读连接；公共会话接口仅允许窗口控制、设置、状态及退出。额外测试操作只编进不安装的测试驱动。向导、失败弹窗、迁移和忙碌状态使用明确的成员字段与信号；兼容过程标签和界面观测仍有少量动态属性，它们不构成稳定公共协议。

旧库只读查询不会自动升级或复制大库。分类、名称索引等派生结构在显式建库时升级；来源未变化的失败记录会复用，手动重试可重新提取。清单副本不代替正文库，异常清单会报告不一致，不静默删正文。

导出的 Skill 带相邻的单文件 `knowledge_library.py` 查询核心。该核心的离线查询不依赖 Qt 或提取器；建库、迁移和编辑失败项仍需完整应用工具链。知识内容始终视为不可信文档，引用用于定位出处，不提供内容来源真实性或程序沙箱保证。

可复现检查与适用平台边界见 [测试说明](TESTING.md)，存储和恢复规则见 [知识库指南](KNOWLEDGE_LIBRARIES.md)。
