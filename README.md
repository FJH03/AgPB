# AgPB

Counter-Strike: Source（Source 1 / Win64）的 agent 控制 bot —— Metamod:Source 插件。
不用 SourceMod，也不用引擎自带的 `CCSBot`：外部 agent 下高层意图，插件逐 tick 生成 `CUserCmd`。

- 假客户端用 `server.dll` 的 `BotManager001` 创建，**不带任何 AI**；`RunPlayerMove` 与引擎
  驱动真人玩家是同一条路径，不需要特征码、硬编码偏移，也不链接 `server.dll`。
- 玩家侧的一切（移动 / 视角 / 按键）都走 `CUserCmd`，不写服务端实体字段。
- 现状 / 进度 / 下一步看 [`PROGRESS.md`](PROGRESS.md)。

## 构建

`E:\Plugins-Platform` 下需要同层放齐这几个目录：

```
E:\Plugins-Platform\
  hl2sdk-css\            # CS:S SDK（含 source-engine-czero）
  hl2sdk-manifests\
  metamod-source\
  AgPB\                  # 本插件
```

```powershell
cd E:\Plugins-Platform\AgPB
mkdir build
cd build
cmd /c '"E:\vs\VC\Auxiliary\Build\vcvarsall.bat" amd64 && chcp 65001 && set PYTHONIOENCODING=utf-8 && py ../configure.py -s css --targets x86_64 --enable-optimize && ambuild'
```

产物：`build\agpb_mm\windows-x86_64\agpb_mm.dll`

前提：`hl2sdk-css\source-engine-czero\build\{tier0,vstdlib,mathlib,tier1}\*.lib` 必须先存在，
否则链接失败（Metamod:Source 自身也依赖这几个库）。

## 部署

```
cstrike/addons/AgPB/bin/win64/agpb_mm.dll
cstrike/addons/metamod/AgPB.vdf
```

`AgPB.vdf`：

```
"Metamod Plugin"
{
	"alias"		"AgPB"
	"file"		"addons/AgPB/bin/win64/agpb_mm"
}
```

启动后控制台出现 `[AgPB] loaded. ...` 即部署成功。建议 `server.cfg` 里关掉引擎自带 bot：

```
bot_quota 0
bot_quota_mode normal
```

服务器运行中 DLL 被占用，换文件必须先停服。

## 文档

| 文件 | 内容 |
|---|---|
| [`AGENTS.md`](AGENTS.md) | 硬性要求：构建 / 部署红线、项目铁律、编码约定 |
| [`PROGRESS.md`](PROGRESS.md) | 现状、里程碑、问题→解决、坑、下一步 |
| [`COMMANDS.md`](COMMANDS.md) | 控制台命令 / ConVar / 路点编辑器用法 |
| [`VERIFY.md`](VERIFY.md) | 回归验证清单（阶段 A–G） |
| [`CRASH_REPORT.md`](CRASH_REPORT.md) | 已知崩溃：freezetime 的 `Radio()`（未修，测试必须同队） |

## 许可证

[GPL-3.0](LICENSE)。M3 起会移植 CS-EBOT（上游 SyPB，GPL-3.0）的代码。
参考实现：[CS-EBOT](https://github.com/EfeDursun125/CS-EBOT) → [SyPB](https://github.com/CCNHsK-Dev/SyPB) → [YaPB](https://github.com/yapb/yapb)。
