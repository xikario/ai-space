# 结构化正文与独立知识库提取

自 structured-1 起，`smart_space_extract.py` 是预览与知识库共用的本地提取内核。
文件快照仅作为文件清单。知识库 `build` 直接读取清单中的原文件，不再把 UI
预览正文作为全文，也不受 UI 单文件字符额度、全局正文预算或 64 MiB 文件门槛限制。
UI 仍保存有界的预览，不把全文塞入 Qt 内存。

支持的主要路径：

- OLE 魔数优先于 ZIP 尾部检测，再识别 Workbook/WordDocument 等容器内容。
- XLS 通过随包的 xlrd 读取；XLSX 回填共享字符串，按真实工作表关系、单元格坐标、
  日期样式和行号输出，字符串池使用临时 SQLite，避免全量加载。
- DOCX 按正文段落、内容控件、标题、表格顺序提取，过滤域指令、删除内容和隐藏文字。
- PPTX 以 presentation.xml 的幻灯片关系顺序为准，缺少关系时自然排序并标记部分恢复。
- PDF 按页提取文字；空文字页逐页 OCR，不再有五页或字符分摊额度。
- 大于 1 MiB 的 `.log` 明确采用头部、错误上下文、尾部摘要，保留原行号并标为 partial。
- 旧 Word/WPS 使用 Word piece table 恢复主要正文，不再将任意二进制 strings
  当成可靠语料；因版式和表格未恢复，状态明确为 partial。

## 操作

原有 `build --index-stream ... --index-json ... --db ...` 调用继续有效，默认包含
图片与空文字 PDF 页的 OCR。首次完整建库耗时较长，桌面菜单可暂停，已提交的文件会复用。

`--no-ocr` 运行文字阶段，待 OCR 的材料标记 partial；同一原文件已有的完整 OCR
不会因文字阶段降级。`--snapshot-only` 仅用于兼容导入，始终标记为预览。
`--include-root PATH` 可重复指定文件或目录，仅刷新清单中的对应范围，保留其他目录。
`--force` 强制重新提取。

`stats --db ...` 返回 extractionStates；`issues --db ... --limit 100` 返回问题清单。
状态 complete 表示当前提取路径完成，不承诺图像、复杂版式与所有嵌入对象完全保真。
partial、failed、missing 不可被当作完整语料。

数据库保持原 schema_version=2 的兼容表结构，新增 extraction_state 表存储状态、
问题、提取器版本、文件签名和正文统计。失败保留最后成功切片，不把文件静默删除；
查询结果附带 extractionStatus。首次升级会重新提取无版本状态的旧记录。
切片参数、OCR 策略、原文件大小/纳秒修改时间和提取器版本共同控制缓存。
更新解析行为时应升级 EXTRACTOR_VERSION。

每份文件使用独立 savepoint，文件解析中途出错会回滚其替换操作；整个建库有进程锁，
读取仍可并行。只在完整消费清单之后执行范围内的删除，并清理无正文对应的派生 FTS 记录。
局部刷新不宣称全库已经同步到新快照。

回归：`python3 -m unittest -v tests/test_smart_space_extract.py tests/test_smart_space_knowledge.py tests/test_smart_space_indexer.py`。
随包依赖及原许可证见 `scripts/_vendor/README.md`。
