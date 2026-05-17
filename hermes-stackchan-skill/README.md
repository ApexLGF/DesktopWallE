# hermes-stackchan-skill

StackChan 控制 skill 的 **Hermes Agent 版**。Hermes Agent(运行在 mini `<bridge-host>`)
加载它,把 StackChan 桌面机器人当作外设来驱动。

与同级 `../openclaw-stackchan-skill/`(OpenClaw 版)是同一台机器人的两套 skill,面向
不同的 agent 宿主、用不同的格式和控制路径。两者**部分同步**(见下方「同步规则」)。

## 目录结构

```
hermes-stackchan-skill/
├── SKILL.md        # Hermes 格式 skill:YAML frontmatter + markdown 正文
├── scripts/
│   └── sc          # bash 助手脚本,封装 stackproxy + 设备 HTTP,自动发现设备 IP
└── README.md       # 本文件
```

Hermes skill 的标准布局是 `~/.hermes/skills/<category>/<skill>/`,`SKILL.md` 为必需
入口文件,可选 `scripts/` `references/` `templates/` `assets/` 子目录。**没有
`agents/` 目录** —— 那是 OpenClaw 的概念;Hermes 直接读 `SKILL.md` 的 YAML
frontmatter 和正文。(第一次迁移时建的扁平布局
`~/.hermes/skills/stackchan/SKILL.md` + `agents/openai.yaml` 是错的,已删除。)

## Hermes 怎么用它

- 安装到 mini 的 `~/.hermes/skills/smart-home/stackchan/`。
- 自动暴露为 slash 命令 `/stackchan`,也能被自然语言("让 StackChan …")触发。
- Hermes 在 terminal sandbox 里跑 `scripts/sc`。`sc` 打两条 HTTP 控制面,都在 mini
  本机:`stackproxy` daemon(`localhost:8766` —— 豆包 TTS、状态、表情、动作)和
  设备固件(`http://<device-ip>` —— LED、屏幕、舵机、传感器、事件)。

## 设计依据(2026-05-14 实测)

这一版**不是**对 openclaw 版的逐行适配,而是基于 mini 上实际环境重新设计的。实测:

- stackproxy 的 `/agent/say` 走豆包 TTS(实测 ~457ms 合成、4.2s 语音推流到设备)——
  这就是 Hermes 让 StackChan **真正开口**的路径,**不需要 OpenClaw 插件**。Hermes
  与 stackproxy 同机,直接 curl `localhost:8766` 即可。
- 设备固件 HTTP API 同时也通,提供 stackproxy 没代理的底层能力(LED/屏幕/舵机/传感器)。
- ⚠️ 相机路径(`/agent/look`)实测会**硬重启设备**(约 50s 恢复,boot_count++)——
  已知 esp_camera 固件 bug,skill 里标为不稳定、慎用。

## 部署

```sh
ssh user@<bridge-host> 'mkdir -p ~/.hermes/skills/smart-home/stackchan/scripts'
scp hermes-stackchan-skill/SKILL.md   user@<bridge-host>:~/.hermes/skills/smart-home/stackchan/SKILL.md
scp hermes-stackchan-skill/scripts/sc user@<bridge-host>:~/.hermes/skills/smart-home/stackchan/scripts/sc
ssh user@<bridge-host> 'chmod +x ~/.hermes/skills/smart-home/stackchan/scripts/sc && hermes gateway run --replace'
```

`hermes gateway run --replace` 重启网关,重新扫描 skill 列表。

## 同步规则(给维护这个仓库的 agent)

当 `../openclaw-stackchan-skill/SKILL.md` 改动时:

1. **设备固件 HTTP endpoint 变了**(`src/main.cpp` 加/改 `/display` `/led` `/head`
   `/camera` `/sensors` `/action` `/events` …)→ 同步到本目录:更新对应的 `sc`
   子命令 和/或 SKILL.md 的引用。设备 API 传输无关,两个 agent 宿主打的是同一套固件。
2. **stackproxy 的 HTTP admin 接口变了**(`tools/stackproxy/server.py` 的
   `build_http_app` 路由表)→ 同步 `sc` 脚本里走 `localhost:8766/agent/*` 的部分。
   这是 Hermes 版最关键的契约来源。
3. **OpenClaw 插件方向的改动**(`stackchan_say`/`_express`/`_move`/`_look` 注册工具、
   `stackproxy.invoke` gateway 方法、`openclaw gateway call`、mem0 hooks)→ **不要**
   搬进来。Hermes 没有插件层,经 `sc` 走 HTTP 直连;照搬插件工具引用会让 Hermes
   agent 去调不存在的工具。

注意:`stackproxy` 本身是**共享基础设施** —— Hermes 直接用 `localhost:8766/agent/*`
(尤其是豆包 TTS)。所以 stackproxy 的 *HTTP* endpoint 改动对两边都相关,只有 OpenClaw
的 *插件* 封装层不相关。

关联文档:Obsidian `20_项目/HermesAgent/`。
