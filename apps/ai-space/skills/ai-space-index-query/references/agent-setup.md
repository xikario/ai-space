# Codex 与 Antigravity 接入

此 SKILL 使用标准 `SKILL.md`，无需专用模型或 MCP 服务。Agent 需要本地命令执行权限、Python 3.8+、支持 FTS5 的 SQLite，以及库文件的读取权限。

## 完整目录

从 AI空间设置的 Skill 页导出整个 `ai-space-index-query` 目录。必须包含：

```text
ai-space-index-query/
├── SKILL.md
├── references/agent-setup.md
└── scripts/
    ├── query.py
    └── knowledge_library.py
```

安装版的 Skill 已带上述独立查询核心。仓库中的原始 Skill 由构建 / 安装步骤补齐核心；不要只复制仓库里的 `SKILL.md` 或 `query.py` 交给别人。

## 放置位置

把整个目录复制到对应位置；本机安装版也可用符号链接指向应用的 Skill 目录，随应用更新。转移到其他电脑时使用完整副本，避免携带指向原电脑的链接。

| 工具 | 位置 |
| --- | --- |
| Codex，用户级 | `~/.agents/skills/ai-space-index-query/` |
| Codex，已有旧配置 | 若已有 `~/.codex/skills/` 入口，更新同一份目录，避免两个不同版本并存 |
| Antigravity App，用户级 | `~/.gemini/config/skills/ai-space-index-query/` |
| Antigravity CLI，用户级 | `~/.gemini/antigravity-cli/skills/ai-space-index-query/` |
| 项目级 | `<workspace>/.agents/skills/ai-space-index-query/` |

Codex 可明确指定 `$ai-space-index-query`；Antigravity 可提及名称，App 也支持 `/ai-space-index-query`。新增入口后开新会话；若未发现，再重启对应 Agent。

原有 `ukui-fences-index-query` 是旧名称兼容入口，保留旧调用时也应更新它的查询脚本和相邻核心。新任务优先使用 `ai-space-index-query`。

## 核对与使用

先解析当前 Skill 的真实目录，将 `S` 设置为其 `scripts/query.py` 路径；不要写死其他用户的路径。

```sh
python3 "$S" --help
python3 "$S" libraries
python3 "$S" --db /资料盘/共享知识库.aikb stats
python3 "$S" --db /资料盘/共享知识库.aikb search --query '部署方案' --limit 10
python3 "$S" --db /资料盘/共享知识库.aikb fetch --reference 'aispace://…' --limit 8
```

本机应用设置和已挂载库可自动发现。分享时同时交付 `.aikb` 文件；接收方通过 `--db` 或 `AI_SPACE_KNOWLEDGE_DB` 指定自己的文件位置，不继承发送者的个人目录。无需原件、GUI、OCR 或建库工具即可只读检索已提取正文；源文件未入库的内容无法恢复。

独立副本不会自动更新，升级应用后需重新导出。符号链接入口则随目标安装目录更新。查询结果仍需核对覆盖情况、提取状态、正文与出处，不能将未命中视为完整库中不存在。

目录与发现规则参考 [OpenAI 官方说明](https://learn.chatgpt.com/docs/build-skills) 和 [Antigravity 官方说明](https://antigravity.google/docs/skills)。
