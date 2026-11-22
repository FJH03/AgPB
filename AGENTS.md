# AgPB 全局硬规则

项目现状、进度与修复记录统一写 [PROGRESS.md](PROGRESS.md)。保留当前行为、必要回归和验收结论，明确未解决与未验证事项。省去已修复问题的排查和失败尝试。工作流只保留可复用的操作和工具，合并重复说明。
新增全局要求按日期追加，冲突以用户最新要求为准。合并或删减已有条款须经用户授权。

路点制作与验收遵循[路点工作流](waypoint-workflow/README.md)。普通模式战斗行为与验证遵循[战斗验证](combat-tests/README.md)。其余入口见[项目文档索引](README.md#文档)。

## 1. 操作规则

- 构建、部署和文件操作在普通 shell 执行。用户已授权必要的普通 shell 操作和游戏插件扩展覆盖。替换被占用的 DLL 前先停服。
- 临时脚本、日志和临时数据放系统 TEMP。待删除的文件只列完整路径交用户处理。Git 只跟踪源码、文档和必要脚本，忽略 `build/`、构建产物和游戏二进制。
- 每次操作前后检查 `git status`。`commit` 和 `rebase` 由用户执行。子代理只使用 `gpt-6.2-sol / ultra`。
- 测试时 bot 必须与真人同队，换场景前先 `agpb_kick all`。Radio 兼容条件见 [CRASH_REPORT.md](CRASH_REPORT.md)。

## 2. 构建与部署

- `E:\Plugins-Platform` 下须同层放齐 `hl2sdk-css\`、`hl2sdk-manifests\`、`metamod-source\`（MMS 2.0，Source 1）和 `AgPB\`。
- 先构建 `hl2sdk-css\source-engine-czero\build\{tier0,vstdlib,mathlib,tier1}\*.lib`。Metamod:Source 和 AgPB 都依赖这些静态库。
- 部署目标：`O:\SteamLibrary\steamapps\common\czero-game-x64\cstrike`

构建模板和本机工具使用上面的部署目标。换目标目录时只改这一行。保留 `chcp 65001`、`PYTHONIOENCODING=utf-8` 和日常构建的 `-s css`。

```powershell
cd E:\Plugins-Platform\AgPB
$agpbRequirements = Get-Content -LiteralPath .\AGENTS.md -Raw -Encoding utf8
$agpbDeployTarget = [regex]::Match($agpbRequirements, '(?m)^- 部署目标：\s*`([^`]+)`').Groups[1].Value
if (!$agpbDeployTarget) { throw 'Deployment target missing' }
New-Item -ItemType Directory -Path build -Force | Out-Null
cd build
cmd /c ('"E:\vs\VC\Auxiliary\Build\vcvarsall.bat" amd64 && chcp 65001 && set PYTHONIOENCODING=utf-8 && py ../configure.py -s css --targets x86_64 --enable-optimize && ambuild && xcopy /E /I /Y .\package\addons "{0}\addons"' -f $agpbDeployTarget)
```

- `configure.py` 的 `--game csczs|css|csgo` 默认为 `csczs`，映射到 `src/game_target.h` 的 `AGPB_GAME_*` 宏。构建 CS:S / CS:GO 变体时显式指定，CS:GO 真机构建须有 `hl2sdk-csgo` manifest。
- 裸 DLL 在 `build\agpb_mm\windows-x86_64\agpb_mm.dll`。`ambuild` 的 `PackageScript` 按 SourceMod 目录约定直接组装 `build\package\addons\`，包含 `AgPB\bin\win64\agpb_mm.dll`、`AgPB\waypoints\` 和 `metamod\AgPB.vdf`。直接部署目录，保持不压 ZIP。
- VDF 源模板放 `configs/metamod/AgPB.vdf`。源码树不建 `addons/`、`cstrike/` 或 `bin/` 部署目录。
- 新 `.cpp` 必须登记到 `AMBuilder`。修改 `src/` 头文件后，核对 `AMBuilder` 基于头文件内容触发 C++ 消费者重编。
- 部署用模板末尾的 `xcopy /E /I /Y` 将 `build\package\addons\` 覆盖到目标 `addons\`。服务器启动后须看到 `[AgPB] loaded. ... IBotManager=ok ...`。
- 2026-11-22 补充：仓库根目录的 `waypoints/` 存放可纳入 Git 的正式 AGPW 路点。构建将此目录完整打包到 `addons/AgPB/waypoints/`，部署随 `addons/` 一起覆盖复制。

## 3. 项目铁律

- AgPB 假客户端必须无引擎 AI。任务、路点导航和执行行为由 AgPB 实现，禁止使用 `CCSBot` 或引擎 ZBot AI 代替。
- 本地核心依据感知、内部状态和游戏规则自主选择任务。LLM + harness 负责最高层任务决策与仲裁，提交任务或意图。有效干预由本地执行，无干预、结束或失联后回到本地自主核心。
- 移动、视角、按键和战斗统一由本地核心通过 `CUserCmd` 执行。netvar 写入限开发和诊断。手工弹道跳是运行时例外，允许本地核心为起跳写入 bot 的服务端变量。
- 零特征码、零硬编码 vtable 索引。反射用 SendTable，调用公开虚接口，禁止链接 `server.dll`。
- 路点、连边及跳跃标志一律手工制作。原生 NAV 和几何体检只读，禁止自动导入 NAV 图、自动连线和自动补跳跃标志。
- 目标游戏为 csczs（当前）、CS:S 和 CS:GO。只实现用户确认的游戏差异。实现点用 `AGPB_GAME_<目标>` 显式编译期分支，可用编译期常量，并用 `[game-diff]` 注释说明。禁止为区分游戏而读内存或运行期探测。
- 普通模式（SyPB）与 ZM / ZE（EBot）共用插件、路点格式和核心，通过运行期 profile 区分。用户在地图 CFG 中显式设置 `agpb_mode normal|zombie`。禁止自动判定模式，目前僵尸模式只留开关。
- 普通模式以 SyPB 为行为基线，CS-EBOT 仅作实现对照。移植时对照同名文件，普通模式剔除僵尸专用分支。引擎源码仅用于核实公开接口、物理和伤害规则。遵守 GPL-3.0，保留移植代码的来源与版权。
  - SyPB `4c364fb` 在 `E:\Plugins-Platform\refs\SyPB`，使用 `Project SyPB\SyPB_BOT\`。`SwNPC\` 和 `AMXX\` 不在移植范围内。
  - CS-EBOT `2333562` 在 `E:\Plugins-Platform\refs\CS-EBOT`，是 ZP / ZE 专用参考。
  - CS:S 引擎源码在 `E:\Plugins-Platform\hl2sdk-css\source-engine-czero\`。

## 4. 编码约定

- 源码用 `/utf-8` 编译。`AMBuildScript`、`PackageScript`、`AMBuilder` 等构建脚本只用 ASCII。
- 字符串和内存操作用 tier1 `V_*` / `Q_*`。`string_t` 先 `STRING()` 再判空串。
- 块注释正文禁止出现 `*/`。源码可用中文注释。面向 GBK 控制台的输出只用英文，HUD 菜单用中文 UTF-8。
