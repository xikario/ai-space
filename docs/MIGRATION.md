# 迁移与兼容接口

Fences 界面不再保留设置或菜单入口；通过系统应用菜单打开 AI空间。

独立应用会话服务为 `org.ukui.AiSpace`，对象 `/aiSpace`，接口同服务名。新 Fences 仅保留轻量客户端；启动 Fences、浏览设置都不会启动 AI空间、加载其索引或运行 OCR。

首次迁移读取旧 `~/.config/kylin/ukui-fences.ini` 的 `smartSpace` 分组、`~/.cache/kylin/ukui-fences/smart-space/index.json*`、旧 Provider 和 `~/.local/share/ukui-fences/knowledge/smart-space-knowledge.sqlite`。旧自定义知识库目录也会读取。不会修改源配置和源快照。SQLite backup 使用只读源连接，把已提交事务复制到独立数据库，不直接复制活动 WAL 文件。

已有目标文件不覆盖。成功后记录 `migration/legacyV1=true`；部分失败会提示并保留原数据，修复权限后下次启动重试。未迁入新应用的自动启动项、已退休的远程重排配置和旧窗口多空间状态，不会重新启用。

旧索引包含完整本机路径，迁移不等于把资料迁到另一台机器；原文件必须仍可访问。迁移后的两个应用目录互不回写，避免共用一份可写数据库。

常用调用：

```sh
ai-space --status
ai-space --settings
ai-space --quit
gdbus call --session --dest org.ukui.AiSpace --object-path /aiSpace \
  --method org.ukui.AiSpace.search '项目方案'
```

`status` 返回 JSON；还提供 showWindow / hideWindow / toggleWindow / showSettings / quit、窗口位置/尺寸、密度与全量索引入口。贴边收起能力已移除；旧 hideWindow / hideToEdge / --hide 兼容为最小化到任务栏，revealFromEdge 兼容为恢复普通窗口。显式全量索引仍有用户确认。无服务时 `--status` 不启动索引应用。

Fences 的旧 `showSmartSpaceWidget` 等 D-Bus 方法转发到此服务，查询使用异步状态缓存，因此状态可有短暂延迟。`ukui-fences --smart-space` 直接启动独立程序，不新建 Fences 桌面。可用 `AI_SPACE_BINARY=/absolute/path/ai-space` 指定开发构建；默认查找 PATH 和 Fences 相邻目录。两套应用退出互不影响。

## 分阶段导入与启动

偏好和当前文件快照先导入，然后显示主界面。知识库在后台通过 SQLite backup 复制，状态接口的 migrationPending 表示进行中；此时文件快照检索仍可用，知识库建库/搜索不会创建或改写尚未完成的目标库。备份完成后才原子发布数据库。退出会取消自己的备份进程，删除临时文件，下次启动可重试。

migration/legacyPreferencesV1 标记小数据阶段，migration/legacyV1 仅在全部完成后设置。历史 .bak / precompact 缓存不再随迁移复制，已有目标不覆盖。普通再次启动不弹出迁移准备窗口。

status 增加 uiReadyMs、indexReadyMs、indexApplyMs、indexLoading 和 migrationPending，用于区分界面就绪、索引读取与首次数据导入，不包含正文和文件路径。手动/定时更新模式不注册目录变化监听；首次只创建 32 张结果卡片，后续按原来的 100 项批次展开，总命中数不截断。
