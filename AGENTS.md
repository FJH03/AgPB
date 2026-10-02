# 项目要求（硬性要求 · 常驻）

> 本文件 = 用户给的硬性要求。**现状 / 进度 / 修复记录一律写 [`PROGRESS.md`](PROGRESS.md)**，不要写回这里。
> 新增要求按「追加 + 日期」并入，只增不删；冲突时以最新条目为准。最近更新：2026-10-02

## 1. 操作规则

- 构建 / 部署 / 文件增删在**普通 shell** 里执行；受限 / 沙箱环境里 MSVC 探测会失败
  （报找不到 `cl.exe`，但同一个 shell 里 `where cl.exe` 又能找到）。
- 临时脚本 / 日志 / 临时数据放系统临时目录，用完即删，仓库里不落临时文件。
- 只跟踪源码、文档、必要脚本；`build/`、构建产物、游戏二进制保持 ignore。
- 每次操作前后 `git status`；`commit` / `rebase` 由用户自己做。
- 换 DLL 前先备份旧文件（`agpb_mm.dll.<yyyyMMdd-HHmmss>.bak`），保留最近 2~3 个。
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
  cmd /c '"E:\vs\VC\Auxiliary\Build\vcvarsall.bat" amd64 && chcp 65001 && set PYTHONIOENCODING=utf-8 && py ../configure.py -s css --targets x86_64 --enable-optimize && ambuild'
  ```

- 产物：`build\agpb_mm\windows-x86_64\agpb_mm.dll`。
- 部署两件套：`cstrike/addons/AgPB/bin/win64/agpb_mm.dll` + `cstrike/addons/metamod/AgPB.vdf`。
  部署服务器在 `O:\...\cstrike\`，DLL 被运行中的服务器占用，替换前先停服。
- 冒烟：启动服务器，控制台出现 `[AgPB] loaded. ... IBotManager=ok ...` 才算成功。
- 改过 `src/*.h` 或新增 `src/*.cpp`：AMBuild 不跟踪头文件依赖 / 新文件要进 `AMBuilder`，
  否则会出现「代码没生效」或 `LNK2019`（详见 [`PROGRESS.md`](PROGRESS.md) 的坑）。

## 3. 铁律（项目原则，不能破）

- 不用 SourceMod；不用引擎自带的 `CCSBot` AI —— 假客户端必须是**无 AI** 的。
- bot 的行为（移动 / 视角 / 按键 / 战斗）一律走 `CUserCmd`；netvar 写入只留作开发 / 诊断工具。
- **零特征码、零硬编码 vtable 索引**：反射走 SendTable，接口走公开虚接口；不链接 `server.dll`。
- 路点连边一律手工；不做自动连线、不自动补跳跃标志（几何判定只做只读体检）。
- 控制台输出英文（控制台是 GBK）；HUD 菜单用中文（UTF-8）。
- 许可证 GPL-3.0：移植 CS-EBOT / SyPB 代码时保留来源与版权。

## 4. 编码约定

- 源码要 `/utf-8` 编译；`AMBuildScript` 等构建脚本里**不能有非 ASCII 字符**。
- 字符串 / 内存操作用 tier1 的 `V_*` / `Q_*`；`string_t` 先 `STRING()` 再判空串。
- 块注释里不要出现 `*/` 字样（会提前结束注释）。
- 中文注释可以；面向控制台的日志 / 回显不行。

## 5. 参考坐标

- 参考实现：[`E:\Plugins-Platform\refs\CS-EBOT`]（血统：POD-Bot → YaPB → SyPB → CS-EBOT）。
- CS:S SDK：`E:\Plugins-Platform\hl2sdk-css`（含完整引擎源码 `source-engine-czero\`）。
- 宿主：`E:\Plugins-Platform\metamod-source`（MMS 2.0，Source 1）。
- 文档分工：`README.md`（构建 / 部署）、`PROGRESS.md`（现状 / 历史 / 下一步）、
  `COMMANDS.md`（命令 / ConVar）、`VERIFY.md`（验收）、`CRASH_REPORT.md`（已知崩溃）。
