# AgPB

Counter-Strike: Source（Source 1 / Win64）的 agent 控制 bot —— Metamod:Source 插件。
不用 SourceMod，也不用引擎自带的 `CCSBot`：外部 agent 下高层意图，插件逐 tick 生成 `CUserCmd`。

- 假客户端用 `server.dll` 的 `BotManager001` 创建，**不带任何 AI**；`RunPlayerMove` 与引擎
  驱动真人玩家是同一条路径，不需要特征码、硬编码偏移，也不链接 `server.dll`。
- 玩家侧的一切（移动 / 视角 / 按键）都走 `CUserCmd`，不写服务端实体字段。
- 现状 / 进度 / 下一步看 [`PROGRESS.md`](PROGRESS.md)。

## 需要什么

`E:\Plugins-Platform` 下同层放齐这几个目录：

```
E:\Plugins-Platform\
  hl2sdk-css\            # CS:S SDK（含 source-engine-czero）
  hl2sdk-manifests\
  metamod-source\
  AgPB\                  # 本插件
```

SDK 静态库必须先编好：`hl2sdk-css\source-engine-czero\build\{tier0,vstdlib,mathlib,tier1}\*.lib`
（Metamod:Source 自身也依赖，缺了直接 `LNK1181`）。

## 构建与部署

命令模板、包目录、部署步骤、红线都在 [`AGENTS.md`](AGENTS.md) §2，这里不重复。
一句话版本：`ambuild` 产出 `build\package\addons\...`（SourceMod 目录约定，不压 zip），
构建命令最后一条 `xcopy` 把它覆盖到 [`AGENTS.md`](AGENTS.md) §2 写的目标目录。

## 文档

| 文件 | 内容 |
|---|---|
| [`AGENTS.md`](AGENTS.md) | 硬性要求：构建 / 部署红线、项目铁律、编码约定 |
| [`PROGRESS.md`](PROGRESS.md) | 现状、里程碑、问题→解决、坑、下一步 |
| [`COMMANDS.md`](COMMANDS.md) | 控制台命令 / ConVar / 路点编辑器用法 |
| [`VERIFY.md`](VERIFY.md) | 回归验证清单（阶段 A–G） |
| [`CRASH_REPORT.md`](CRASH_REPORT.md) | 已知崩溃：freezetime 的 `Radio()`（未修，测试必须同队） |

## 许可证

[GPL-3.0](LICENSE)。M3 起移植 SyPB / CS-EBOT（GPL-3.0）的代码，基线是 SyPB，
CS-EBOT 只作对照（僵尸模式专用 fork）。
参考实现：[CS-EBOT](https://github.com/EfeDursun125/CS-EBOT) → [SyPB](https://github.com/CCNHsK-Dev/SyPB) → [YaPB](https://github.com/yapb/yapb)。
