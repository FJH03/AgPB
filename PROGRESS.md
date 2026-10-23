# PROGRESS — AgPB

> 更新：2026-10-23。只保留当前状态、未解决问题、验收边界与下一步。
> 硬性要求见 [`AGENTS.md`](AGENTS.md)，命令见 [`COMMANDS.md`](COMMANDS.md)，
> 构建 / 部署见 [`README.md`](README.md)，验收标准见 [`VERIFY.md`](VERIFY.md)。

## 当前进度

当前处于 **M3：本地自主 bot 的导航与行为移植**。无 AI 假客户端、SendTable 反射、
CUserCmd 输入、手工路点与编辑器底座已落地；普通任务及地图目标已有基础。
de_dust2 的全图手工路点、最终 F4A8 构建的终点 / 方向覆盖和六项真实炸弹场景已验收。
M3 尚未整体完成，下一步收束基础控制生命周期，再接普通模式最小战斗。

| 能力 | 当前实现与验收边界 |
|---|---|
| 假客户端与输入 | 入队、出生、移动与视角已实测；行为统一走 CUserCmd，netvar 写入仅用于开发 / 诊断 |
| 反射 | SendTable 标量、数组、向量分量及完整 EHANDLE 读取已实现 |
| 路点与编辑器 | AGPW 存取、换图重载、手工有向边、A*、按需单源 Dijkstra、只读几何体检、中文 HUD 菜单及叠加层已落地 |
| 导航 | 行走、逐连接跳跃、蹲行、CAMP 与漫游已有实现；de_dust2 的终点与方向覆盖通过，CAMP 及多 bot 仍需对应版本回归 |
| 清障 | 枪 / 刀攻击、有限换枪 / 换弹、破坏确认后续原路线已接入；有 cs_office 代表样本，未在 F4A8 重做完整清障回归 |
| 普通任务 | Normal、Move、CAMP、Pause、DestroyBreakable 与地图目标共用任务仲裁，支持优先级、挂起 / 恢复、期限与失败反馈 |
| 地图目标 | C4 拾取 / 安放 / 拆除 / 逃爆、人质跟随 / 护送已有实现；本轮炸弹结果见下，人质仅有历史代表样本 |
| 模式与目标游戏 | csczs / css / csgo 编译期分支已接；`agpb_mode` 的 normal / zombie 开关已接，zombie 行为尚未实现 |
| 外部 agent / LLM | M4 / M5 尚未启动 |

普通模式行为以 SyPB 为基线、CS-EBOT 作实现对照；引擎源码用于核实公开接口、物理与伤害规则。
起跳由 AgPB 决定，自动蹲跳交游戏移动层。一般真人 / 临时阻挡的等待、避让和自动续原目标，
后续由任务层统一处理，不另建导航状态机，也不作为当前最小战斗的前置条件。

## de_dust2 当前验收基线

### 地图与资产身份

- 正式图为 **175 个节点、360 条手工有向边**，含 2 个 GOAL 与 6 个跳跃方向。
- 可重建源计划：[`waypoint-workflow/maps/de_dust2.plan.json`](waypoint-workflow/maps/de_dust2.plan.json)。
  正式 AGPW 与计划的坐标、radius、flags、连接及 gravity 逐项一致；T / CT 允许节点图均强连通。
- 正式运行文件位于 AGENTS 指定部署目标下 `addons/AgPB/waypoints/de_dust2.agpw`。
  制作和验收未自动连边、自动补跳或放宽原半径。

以下 SHA-256 绑定本次验收，不把旧版本样本并入当前构建：

| 资产 | SHA-256 |
|---|---|
| AgPB DLL | `F4A8F77A36669ECD0792EC49C8052DE01E82CC4528EF67BEA5F4C876E88DC057` |
| de_dust2 AGPW | `1FFCAC98871987F505CEAEEAD00A7DC6EA9050A526F51F7D7CF96BC75E1B7544` |
| de_dust2 BSP | `3E24A3CF2954463F7B6D0AC640AEE758AA9E0A6F8D0BB06DDA6FA1CB9ED78359` |
| 手工源计划 | `840399A837865B6B73D10C4E84C4CA5D5DE4B63731082BF7B46239C9B8221366` |

### 已通过的范围

| 项目 | 最终结论 |
|---|---|
| 请求终点 | **175/175** 节点分别作为请求终点，严格停稳通过；19 个原 radius=0 节点全部通过 |
| 有向边覆盖 | 175 点批次加 22 段补测，共 **197 段有效记录、360/360 方向、0 拒绝** |
| 独立证据复核 | 路线 / 终点重算通过；2898 份人 / bot 采样、782 份停点后缀及 3489 份回执与原生日志一致 |
| 真实炸弹场景 | **T_A、T_B、CT_A、CT_B 逃爆及 A / B 拆包，六项通过** |
| 包完整性 | `verify-bundle` 接受，manifest 声明的 **147/147 文件**哈希一致，三项部署资产匹配 |

严格停稳条件为 XY≤max(原 radius,4)、Z≤4、接地、速度≤5、连续≥0.8 秒。
全图路线由一名 CT 真人与一个 CT bot 同队完成；bot 关闭自主任务，沿既有图执行明确目标。
该结论覆盖本地图、本构建与记录条件，不代表两队全图、多 bot、动态阻挡或长期稳定性已验收。

炸弹测试使用真实 C4 交互。四项逃爆均有同身份、同回合及真实爆炸后的存活采样；
指定 A / B 路线后的拾包与安放通过，**未验收自主选包点或全图搜包**。
cs_office 曾有两名近图人质被真实护送获救的代表样本，未覆盖全图人质或当前 F4A8 回归。

### 最终证据入口

冻结交付目录：`E:\Plugins-Platform\AgPB\build\waypoint-output\de_dust2-20261023-final`。
源码 / 文档快照、完整日志与原始测试记录均在包内，后续工作不回写该包。

| 相对最终包路径 | 用途 |
|---|---|
| `manifest.json` / `bundle-index.json` | 资产清单与入口 |
| `audit/plan-agpw-compare.json` | 手工计划与 AGPW 字段一致性 |
| `audit/route-coverage.json` / `audit/routes-independent-final.json` | 当前版本终点与方向覆盖、原生日志复核 |
| `evidence/objectives/audit/audit-objectives-six-cases.json` | 六项真实炸弹场景复核 |
| `evidence/cleanup/session-cleanup.json` | 会话实际恢复与收尾 |
| `audit/verify-bundle.json` | 147 件清单文件及部署三资产校验 |

会话已确认 0 bot、夹具输入清空、真人 noclip 关闭且接地静止、按键释放、测试插件卸载，
九项 cvar 原值均独立读回一致；本次游戏及辅助进程已退出，日志稳定。
外部 PowerShell 进程对象的退出码为 null，只确认实际退出与恢复，不声称退出码为 0。

## 未完成与待验收

| 项目 | 当前边界 / 后续动作 |
|---|---|
| 停止朝向、冻结路线 | 源码已保留停止朝向及冻结期间原路线；待在当前构建核对冻结后续路及动作释放 |
| 生命周期 | 死亡、重生、换队、回合及换图已有清理路径，完整实机矩阵未结项；换队命令为 `agpb_bot_team` |
| 一般阻挡恢复 | 仅有有限落地 / 清障重试；一般阻挡时等待 / 避让及解除后自动续原目标尚未完成 |
| 清障覆盖 | 已补脚边低体积扫掠，仍是接触近似；完整近身接触、属性 / 类型覆盖与战斗中清障后续实现 |
| 普通战斗 | 敌我判断、可见性、选敌、瞄准 / 开火和丢失目标后恢复导航尚未形成闭环 |
| 梯子 / 门 | 标志与前瞻不代表行为已完成，仍需分项实现和实机验收 |
| 普通目标扩展 | 自主选包点 / 全图搜包待验收，更多地图 / 人质待覆盖；完整 GOAL 历史评分未移植 |
| 编辑改进 | 游戏内移动节点 / 撤销等按需安排；独立 3D 工具支持 radius / flags 写回及撤销 / 重做，位置与连线只读 |

以下独立问题仍有效：

- **Radio 引擎兼容限制**：已有独立 SourceMod guard，已在匹配引擎版本做过实机验证；
  核心插件不能据此假定所有 server.dll 安全。测试继续遵守 AGENTS 的同队与先踢 bot 规则，
  版本核验和启停约束见 [`CRASH_REPORT.md`](CRASH_REPORT.md)。
- **guard 离线验证缺口**：指令预算耗尽 / 死循环可能误报 PASS，需要预期出口断言；
  后续兼容补丁改动不能只依赖该验证器的 PASS。
- **全实体枚举诊断**：`agpb_ents` 曾异常退出，根因未定位；当前优先使用单实体只读查询。
- **异常数值入口**：部分路点 Add / Load、调试速度与 netvar 写入入口仍缺完整有限值检查；
  尚未实机复现由此导致的崩溃，需按数据入口补防护。
- **游戏原生 JPEG**：`jpeg` 会触发 engine.dll 崩溃，当前不修；制图 / 观察使用 TGA。
- **几何提示边界**：de_dust2 的只读体检仍有 157 条“可能需跳”提示，与实走结果不一致；
  不据此自动补跳，也不把路线通过写成全部几何提示已消除。

## 阶段与下一步

| 阶段 | 状态与依赖 |
|---|---|
| M1 / M2 / M2.5 | 无 AI 假客户端、反射与 CUserCmd 端到端输入基础已实测 |
| M3a 基础导航 | de_dust2 已有当前基线；下一步验停止 / 冻结及死亡、换队、换图生命周期 |
| M3b 清障与恢复 | 清障主链已接；按影响范围回归；一般等待 / 避让 / 续路由任务层继续实现 |
| M3c 普通最小战斗 | 基础控制与必要清障稳定后开始；推迟的一般阻挡恢复不作前置 |
| M3d 地图能力 | 炸弹已有当前代表场景；梯子、门、更多目标与地图分项推进 |
| M3e ZM / ZE | 普通模式稳定后接运行期 profile，共用导航、格式与动作核心 |
| M4 外部意图桥 | M3a–c 稳定，命令 / 状态 / 失败反馈明确后接确定性 Python agent |
| M5 LLM 战术 | M4 及本地行为能独立执行与恢复后启动 |

1. **M3a**：核对停止朝向、冻结后续原路线，死亡 / 重生 / 换队 / 换图释放动作和清理状态。
   代表性无障碍路线应稳定到达；已通过且未受改动影响的基线不必每轮全量重跑。
2. **M3b**：在当前版本回归必要清障与双向续路，覆盖代表性连续杂物和预算内失败退出。
   每次只给一个目标，不人工代清障、传送或重发路线；枪与刀取代表样本即可。
3. **M3c**：统一导航 / 清障 / 战斗的动作优先级，经 CUserCmd 攻击敌人，目标丢失后续原导航。
   普通行为继续对照 SyPB 与 EBot；择优选枪、手雷、复杂射击和追敌中清障随后补。
4. **后续阶段**：按实际需求逐项接 M3d / M3e，M4 不要求先完成全部地图、目标或僵尸模式。
   外部层只给意图，不能代替本地移动 / 清障 / 恢复，也不承担逐 tick 操作；协议在 M4 启动时收束。

## 源码与工具入口

| 入口 | 用途 |
|---|---|
| `src/plugin.cpp` / `src/bot.*` | MMS 入口、命令、生命周期、CUserCmd 与导航 |
| `src/route_control.h` / `src/bomb_escape_policy.h` | 终点控制、跳跃落地及逃爆预算 / 重试策略 |
| `src/tasks.*` / `src/bot_tasks.cpp` | 任务优先级、挂起 / 恢复、规划与路线归属 |
| `src/objectives.*` / `src/bot_objective_actions.cpp` | 只读 C4 / 人质快照与交互动作 |
| `src/obstacle.*` / `src/netvars.*` | 障碍检测、实体分类与 SendTable 反射 |
| `src/waypoint.*` / `src/wpedit.*` / `src/wpdraw.*` / `src/menu.*` | 路点、体检、编辑与 HUD |
| `PackageScript` / `AMBuilder` | 分发目录组装与源码构建清单 |
| [`waypoint-workflow/README.md`](waypoint-workflow/README.md) | 路点工作流的统一说明入口，含原生控制台、TGA、人工计划与版本绑定验收 |
| [`tools/agpw_view3d_README.md`](tools/agpw_view3d_README.md) | 独立 3D 查看 / radius 与 flags 编辑；位置、连线与动态碰撞不在支持范围 |
| `tools/agpw_view.py` / `tools/agpw_view.bat` | 只读 2D 路点图查看 |

构建涉及头文件时必须确保消费者重编；新增 cpp 加入 AMBuilder，构建脚本保持 ASCII。

### 游戏差异登记

仅登记用户已确认的条目；后续按 AGENTS 的 `AGPB_GAME_*` / `[game-diff]` 规则显式分支，不自行猜测差异。

| 功能 | csczs（当前） | css | csgo | 实现位置 |
|---|---|---|---|---|
| 站立 / 蹲姿体积高度 | 随 `sv_cs_use_legacy_viewvectors` 支持 62/45 与 72/54 | 编译期固定 62/45 | 编译期固定 72/54 | `AgPB_RefreshHullSize()`（`src/waypoint.cpp`） |

## 当前可清理的会话资料

最终证据已另存上方冻结包。以下为本轮临时资料或映射，仅列完整路径，由用户自行删除；
正式 AGPW、手工源计划及最终交付包不在清理范围。

- `C:\Users\FJH_03\AppData\Local\Temp\agpb_wp_resume_20261023`
- `C:\Users\FJH_03\AppData\Local\Temp\agpb_wp_behavior_20261021`
- `O:\SteamLibrary\steamapps\common\czero-game-x64\cstrike\cfg\agpb_wp_resume_20261023`（junction）
- `O:\SteamLibrary\steamapps\common\czero-game-x64\cstrike\screenshots\agpb_wp_resume_20261023`（junction）
- `O:\SteamLibrary\steamapps\common\czero-game-x64\cstrike\addons\sourcemod\plugins\disabled\agpb_waypoint_probe.smx`
- `O:\SteamLibrary\steamapps\common\czero-game-x64\cstrike\addons\sourcemod\plugins\disabled\agpb_behavior_fixture.smx`
