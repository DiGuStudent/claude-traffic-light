# Claude Traffic Light

把 Claude Code / OpenCode 的工作状态实时映射到一颗 ESP8266 红绿灯上：黄灯常亮 = 正在处理，黄灯闪烁 = 等待确认，红灯闪烁 = 出错，绿灯常亮 = 空闲，绿灯呼吸 = 对话结束。硬件只需要一块 NodeMCU 和 3 颗 LED。

MIT 协议，纯 Python 标准库 + Arduino 固件，无第三方运行时依赖。

## 状态映射

| 灯效 | 含义 | Claude Code 事件 | OpenCode 事件 |
|---|---|---|---|
| 红黄绿同闪一下 → 绿灯常亮 | 新对话开始 | `SessionStart` | `session.created` |
| 黄灯常亮 | 正在处理/思考/改文件/执行命令 | `UserPromptSubmit`、`PreToolUse`、`PostToolUse` | `message.updated`、`message.part.updated`、`command.executed` |
| 黄灯闪烁 | 等待用户确认或回复 | `PermissionRequest`、`Notification`（仅权限类，其余通知忽略） | `permission.ask` |
| 红灯闪烁 | 出错（仅错误发生当下闪红） | `PostToolUseFailure`、`tool_response.is_error` | `session.error` |
| 绿灯常亮 | 处理完成、空闲 | `Stop` | `session.idle` |
| 绿灯长周期呼吸 | 对话结束 | `SessionEnd` | `dispose`、`session.deleted` |

轮次结束恒为绿灯；错误只在发生当下闪红，错误记录可在监控后台查看。

## 架构

```
Claude Code (hooks)          OpenCode (插件)
      │                           │
      ▼                           ▼
 ~/.local/bin/claude-light  ──unix socket──►  ~/.local/bin/claude-lightd
 (事件→灯命令，错误标记按 agent 隔离)        (systemd 用户服务，长持链路)
                                                   │ 串口 115200（优先）
                                                   │ WiFi UDP 8266（兜底/无线）
                                                   ▼
                                       ESP8266 红绿灯（红D5 黄D6 绿D7）
                                                   ▲
                               监控后台 http://127.0.0.1:7800
                               （状态磁贴/手动控灯/WiFi 配置/活动流）
```

**多 agent 语义**：daemon 是唯一命令出口，按到达顺序串行转发到板子——多个 agent（或多个会话）并发时天然满足「按时间先后执行、最后状态生效」。错误标记按 agent 隔离（`err.<agent>`），互不误清。

## 硬件

- ESP8266 开发板（NodeMCU / Wemos D1 mini 等，FQBN `esp8266:esp8266:nodemcuv2`）
- 3 颗 LED + 3 颗 220Ω 电阻

| 灯 | 引脚 | GPIO |
|---|---|---|
| 红 | D5 | GPIO14 |
| 黄 | D6 | GPIO12 |
| 绿 | D7 | GPIO13 |

接线：引脚 → 220Ω → LED 阳极，LED 阴极 → GND。选 D5/D6/D7 是因为它们与启动/烧录逻辑无关。

## 目录结构

```
├── firmware/
│   └── claude_traffic_light.ino    ESP8266 固件（灯效状态机、串口+UDP、WiFi EEPROM、OTA）
├── host/
│   ├── claude-light                hook 入口：事件 JSON → 灯命令（多 agent 错误标记）
│   ├── claude-lightd               守护进程 + 监控后台（串口/UDP 长持、unix socket、HTTP）
│   ├── claude-light-web.html       监控后台页面
│   ├── claude-light.service        systemd 用户单元
│   └── claude-light.conf.example   配置示例（复制为 ~/.config/claude-light.conf）
├── integrations/
│   ├── claude-code-hooks.json      Claude Code settings.json 的 hooks 片段
│   └── opencode/                   OpenCode 插件（npm 本地包）
└── LICENSE / README.md
```

## 快速开始

### 1. 烧录固件（首次需 USB，之后可 OTA）

```bash
# 首次：USB 烧录
arduino-cli core install esp8266:esp8266
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 firmware/claude_traffic_light
arduino-cli upload  --fqbn esp8266:esp8266:nodemcuv2 --port /dev/ttyUSB0 firmware/claude_traffic_light

# 之后：OTA 无线烧录（板子连上 WiFi 后，烧录期间黄灯）
arduino-cli upload  --fqbn esp8266:esp8266:nodemcuv2 \
    --port <板子IP> --protocol network firmware/claude_traffic_light
```

固件行为：上电直接绿灯常亮（无自检闪烁）；`wifi:SSID:PASS` 命令把 WiFi 凭证存 EEPROM；命令端口 UDP 8266，OTA 端口 8267，mDNS 主机名 `claude-light`。

### 2. 安装主机端

```bash
install -m 755 host/claude-light host/claude-lightd ~/.local/bin/
install -m 644 host/claude-light-web.html ~/.config/
install -m 644 host/claude-light.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now claude-light
```

USB 连接时无需配置；无线模式把板子 IP 写进 `~/.config/claude-light.conf`（参考 `.example`）后重启服务。

### 3. 接入 Claude Code

把 `integrations/claude-code-hooks.json` 里的 `hooks` 字段合并进 `~/.claude/settings.json`（保留已有配置）。10 个事件覆盖：SessionStart / SessionEnd / UserPromptSubmit / PreToolUse / PostToolUse / PostToolUseFailure / PermissionRequest / Notification / SubagentStop / Stop。

### 4. 接入 OpenCode

```bash
cd ~/.config/opencode
npm install /path/to/claude-traffic-light/integrations/opencode
# opencode.json 的 plugin 数组加入 "opencode-claude-light"，重启 opencode
```

插件通过 `--agent opencode` 复用同一套灯；work 命令内置 2 秒节流防止流式输出刷屏。

### 5. 监控后台与手动控灯

- 监控台：http://127.0.0.1:7800 —— 当前灯效（圆点动画复刻灯效）、板子状态（解析 STATUS 回复）、串口/UDP 链路、「本轮有错误」徽标、手动控灯按钮、WiFi 配置表单、最近活动流（2 秒自动刷新）
- 手动控灯：`claude-light idle`、`claude-light off`、`claude-light wifi:SSID:PASS`（非事件名参数原样直发）
- 日志：`~/.cache/claude-light/hooks.log`（事件映射流水，带 agent 前缀）、`daemon.log`（命令/ACK/UDP 回执）

## 通信协议

一行一条命令（串口 115200 / UDP 8266）：`new` `work` `wait` `error` `idle` `bye` `off` `status` `wifi:SSID:PASS`。每条命令回 `OK <cmd>`；`status` 回 `STATUS fx=N wifi=<ip|off>`。

## 故障排查

- **烧录报端口占用**：daemon 长持串口，烧录前 `systemctl --user stop claude-light`，烧完再 start。
- **日志暴涨**：宿主串口驱动可能瞬时重放同一行数据（可达 2 万行/秒）。daemon 已内置防洪：连续相同行折叠计数（每 2 秒最多记一条）+ 日志超 1MB 启动时轮转。
- **插电瞬间灯闪一下**：供电毛刺导致板子棕复位，无碍（开机即绿灯）。
- **UDP 不达**：查板子当前 IP（`claude-light status` 看 STATUS 回复），路由器 DHCP 重分配后需更新 conf 的 IP= 行；建议给板子做静态租约。
- **错误灯语义**：红灯只在错误发生当下闪烁，轮次结束恒转绿灯；历史错误看监控台徽标。

## License

MIT — 见 LICENSE。
