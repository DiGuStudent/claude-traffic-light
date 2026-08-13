// opencode 插件：把会话状态映射到 Claude 红绿灯
// 复用 claude-light 入口（--agent opencode），与 Claude Code 共用同一套灯。
// 多 agent 并发时由 daemon 按到达顺序串行执行，最后一条命令的状态生效。
import { spawn } from "node:child_process";

const LIGHT =
  process.env.CLAUDE_LIGHT_BIN || process.env.HOME + "/.local/bin/claude-light";

function send(event) {
  spawn(LIGHT, ["--agent", "opencode", event], { stdio: "ignore" }).on(
    "error",
    () => {},
  );
}

// work 节流：流式输出会高频触发 message.part.updated，2 秒内最多发一次
let lastWork = 0;
function work() {
  const now = Date.now();
  if (now - lastWork < 2000) return;
  lastWork = now;
  send("PreToolUse");
}

export const server = async () => ({
  event: async ({ event }) => {
    switch (event.type) {
      case "session.created":
        send("SessionStart"); // new：红黄绿闪一下 → 绿灯
        break;
      case "session.idle":
        send("Stop"); // 任一 agent 有错→红灯保持，否则绿灯
        break;
      case "session.error":
        send("PostToolUseFailure"); // 记错误标记 + 红灯闪烁
        break;
      case "session.deleted":
        send("SessionEnd"); // bye：绿灯呼吸
        break;
      case "message.updated":
        if (event.properties?.info?.role === "user") {
          send("UserPromptSubmit"); // 清除本 agent 错误标记 + 黄灯
        }
        work();
        break;
      case "message.part.updated":
        work();
        break;
      case "command.executed":
        work();
        break;
    }
  },
  "permission.ask": async () => {
    send("PermissionRequest"); // wait：黄灯闪烁（等待用户确认）
  },
  dispose: async () => {
    send("SessionEnd"); // opencode 退出 → 绿灯呼吸
  },
});
