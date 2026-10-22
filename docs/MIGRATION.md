# 迁移与进程接口

通过系统应用菜单或 `ai-space --show` 打开独立应用。首次启动只迁移旧 Fences 的界面偏好和文件快照，不自动复制多 GB 知识库。已有独立库继续使用，尚未迁移的旧库可只读查询；需要搬运时显式选择目标盘并导出、导入 AIKB。

旧偏好来自 `~/.config/kylin/ukui-fences.ini` 的 `smartSpace` 分组，快照来自 `~/.cache/kylin/ukui-fences/smart-space/`。迁移不修改源配置或快照、不覆盖已有目标、不继承旧应用的登录启动设置。Provider 配置和相对继承文件随偏好迁移，但执行仍需用户授权。

旧文件快照记录本机文件路径。迁移快照不等于迁移原文件；跨机器共享正文请使用 AIKB。应用不把资料内容或路径列表发布到 D-Bus 状态中。

## 公共会话接口

服务与接口均为 `io.github.xikario.AiSpace`，对象路径 `/aiSpace`。不再注册旧的 UKUI 命名空间。旧 Fences 客户端若硬编码旧服务名，需要更新调用方；使用 `ai-space --show` 的启动方式不受服务名变化影响。

```sh
ai-space --show
ai-space --hide
ai-space --toggle
ai-space --settings
ai-space --status
ai-space --quit
```

公共 D-Bus 仅导出 `showWindow`、`hideWindow`、`toggleWindow`、`showSettings`、`status` 和 `quit`。`hideWindow` 与 `--hide` 表示最小化到普通任务栏。修改索引范围、启动建库、切换登录启动等操作通过应用界面进行，没有公共 D-Bus 写入入口。

`status` 是只读 JSON，`statusVersion` 为 1；包括就绪状态、任务忙闲、窗口状态和计数，不再提供已移除的玻璃及贴边隐藏字段。会话总线不提供应用级沙箱，同一会话其他进程可以调用公开接口，包括打开窗口和请求正常退出。

自动化 UI 回归使用另一个不安装的 `ai-space-test-driver` 程序。其额外控制接口不编译进正式程序，设置测试环境变量也不会使正式程序导出这些方法。

## 任务退出

正常退出先请求暂停任务；后台控制器向已验证归属的进程组发出终止信号，超时后强制结束，并在销毁时等待回收。已提交的正文保留。断电、SIGKILL 和外部工具随意修改数据库不属于可捕获退出，残留临时文件自动恢复仍需进一步验证。
