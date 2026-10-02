# 项目要求（硬性要求 · 常驻）

> 本文件 = 用户给的硬性要求。**现状 / 进度 / 修复记录一律写 [`PROGRESS.md`](PROGRESS.md)**，不要写回这里。
> 新增要求按「追加 + 日期」并入，只增不删；冲突时以最新条目为准。最近更新：2026-10-02

## 1. 操作规则

- 构建 / 部署 / 文件增删在**普通 shell** 里执行；受限 / 沙箱环境里 MSVC 探测会失败
  （报找不到 `cl.exe`，但同一个 shell 里 `where cl.exe` 又能找到）。
- 临时脚本 / 日志 / 临时数据放系统临时目录，用完即删，仓库里不落临时文件。
- 只跟踪源码、文档、必要脚本；`build/`、构建产物、游戏二进制保持 ignore。
- 每次操作前后 `git status`；`commit` / `rebase` 由用户自己做。
- 服务器运行中 DLL 被占用：**先停服再替换**。
- 测试必须让 bot 与人类**同队**（freezetime 的 `Radio()` 崩溃未修，见 [`CRASH_REPORT.md`](CRASH_REPORT.md)）；
  换场景前先 `agpb_kick all`。

## 2. 构建 → 部署（红线）

- 构建前置：`E:\Plugins-Platform` 下同层放齐 `hl2sdk-css\`、`hl2sdk-manifests\`、`metamod-source\`、`AgPB\`。
- SDK 静态库必须是先编好的：`hl2sdk-css\source-engine-czero\build\{tier0,vstdlib,mathlib,tier1}\*.lib`
  （MMS 自身也依赖，缺了直接 `LNK1181`）。
- AMBuild 命令模板（在 `build\` 里跑，不能少 `chcp` 和 `PYTHONIOENCODING`）：

  ```powershell
  cd E:\Plugins-Platform\AgPB
  mkdir build
  cd build
  cmd /c '"E:\vs\VC\Auxiliary\Build\vcvarsall.bat" amd64 && chcp 65001 && set PYTHONIOENCODING=utf-8 && py ../configure.py -s css --targets x86_64 --enable-optimize && ambuild && xcopy /E /I /Y .\package\addons "O:\SteamLibrary\steamapps\common\czero-game-x64\cstrike\addons"'
  ```

  csczs 基于 css 改造，**日常构建命令保持原样**（`-s css`）；`configure.py` 另接
  `--game csczs|css|csgo`（默认 `csczs`），映射到 `src/game_target.h` 的 `AGPB_GAME_*` 宏，
  只有真去编 css / csgo 变体时才需要显式指定；CS:GO 真机构建要等 `hl2sdk-csgo` manifest。

- 产物（裸 DLL）：`build\agpb_mm\windows-x86_64\agpb_mm.dll`。
- `ambuild` 同时按 SourceMod 目录约定组装好可分发的包目录（由 `PackageScript` 负责，
  不需要额外命令，**不压 zip**）：`build\package\addons\AgPB\bin\win64\agpb_mm.dll`
  \+ `addons\AgPB\waypoints\` + `addons\metamod\AgPB.vdf`。
- 部署目标：`O:\SteamLibrary\steamapps\common\czero-game-x64\cstrike`
  （构建命令里的 `xcopy` 和 `tools\agpw_view.py` 都看**本行**，换目录只改这一行。）
- 部署就是构建命令最后那条 `xcopy /E /I /Y .\package\addons "<目标>\addons"`：
  把 `build\package\addons\` 覆盖过去；服务器运行中 DLL 被占用，替换前先停服。
- VDF 模板在 `configs\metamod\AgPB.vdf`（打包时由 `PackageScript` 拷进包里）；
  `server.cfg` 建议 `bot_quota 0`，避免引擎自带 bot 干预。
- 冒烟：启动服务器，控制台出现 `[AgPB] loaded. ... IBotManager=ok ...` 才算成功。
- 改过 `src/*.h` 或新增 `src/*.cpp`：AMBuild 不跟踪头文件依赖 / 新文件要进 `AMBuilder`，
  否则会出现「代码没生效」或 `LNK2019`（详见 [`PROGRESS.md`](PROGRESS.md) 的坑）。

## 3. 铁律（项目原则，不能破）

- 不用引擎自带的 `CCSBot` AI —— 假客户端必须是**无 AI** 的。
- bot 的行为（移动 / 视角 / 按键 / 战斗）一律走 `CUserCmd`；netvar 写入只留作开发 / 诊断工具。
- **零特征码、零硬编码 vtable 索引**：反射走 SendTable，接口走公开虚接口；不链接 `server.dll`。
- 源码树里不建部署目录（`addons/`、`cstrike/`、`bin/`）：metamod vdf 放 `configs/metamod/`，
  `addons/` 只出现在构建产物 `build/package/` 里。
- 路点连边一律手工；不做自动连线、不自动补跳跃标志（几何判定只做只读体检）。
- 移植 EBot / SyPB 代码时**两个对照着看**：原生模式的行为基线是 **SyPB**；
  CS-EBOT 只当实现对照（其 README 自述只面向 zombie plague / escape / biohazard 模式），
  同名文件 diff，僵尸专用分支直接砍。
- 目标游戏：**csczs（当前）/ css / csgo**。游戏间差异**只认用户指出的**，不自己猜；
  每处差异在实现点用 `AGPB_GAME_<目标>` 宏显式分支（**允许编译期硬编码**，不要为了
  区分游戏去读内存 / 做运行期探测），并在代码注释里标 `[game-diff]` 说明差异内容。
- 游戏模式：**普通模式（SyPB 血统）和 ZM/ZE（EBot 血统）都要支持** —— 同一个插件、
  同一套路点格式，模式差异走运行期 profile 分支，不复制两套核心代码。
  切换入口是 cvar **`agpb_mode normal|zombie`**（用户按图在 cfg 里设置，不做自动判定）；
  **僵尸模式行为暂不实现**（先留开关）。
- 控制台输出英文（控制台是 GBK）；HUD 菜单用中文（UTF-8）。
- 许可证 GPL-3.0：移植 CS-EBOT / SyPB 代码时保留来源与版权。

## 4. 编码约定

- 源码要 `/utf-8` 编译；`AMBuildScript` / `PackageScript` / `AMBuilder` 等构建脚本里
  **不能有非 ASCII 字符**（AMBuild 用系统 locale 读脚本，中文注释会直接炸 `UnicodeDecodeError`）。
- 字符串 / 内存操作用 tier1 的 `V_*` / `Q_*`；`string_t` 先 `STRING()` 再判空串。
- 块注释里不要出现 `*/` 字样（会提前结束注释）。
- 中文注释可以；面向控制台的日志 / 回显不行。

## 5. 参考坐标

- 参考实现（本地副本，血统：POD-Bot → YaPB → SyPB → CS-EBOT）：
  - `E:\Plugins-Platform\refs\SyPB`（`Project SyPB\SyPB_BOT\`，通用基线；`SwNPC\` / `AMXX\` 与本项目无关）—— `4c364fb`
  - `E:\Plugins-Platform\refs\CS-EBOT`（ZP/ZE 专用 fork，做对照）—— `2333562`
- CS:S SDK：`E:\Plugins-Platform\hl2sdk-css`（含完整引擎源码 `source-engine-czero\`）。
- 宿主：`E:\Plugins-Platform\metamod-source`（MMS 2.0，Source 1）。
- 文档索引见 [`README.md`](README.md)。
