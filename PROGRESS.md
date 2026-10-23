# PROGRESS — AgPB

> 更新：2026-10-23。只保留当前状态、未解决问题、验收边界与下一步。
> 硬性要求见 [`AGENTS.md`](AGENTS.md)，命令见 [`COMMANDS.md`](COMMANDS.md)，
> 构建 / 部署见 [`README.md`](README.md)，验收标准见 [`VERIFY.md`](VERIFY.md)。

## 当前进度

当前处于 **M3：本地自主 bot 的导航与行为移植**。无 AI 假客户端、SendTable 反射、
CUserCmd 输入、手工路点与编辑器底座已落地；普通任务及地图目标已有基础。
de_dust2 的全图手工路点、最终 F4A8 构建的终点 / 方向覆盖和六项真实炸弹场景已验收。
M3a 生命周期及 M3b 必要清障代表矩阵已回归通过；M3c 已接本地最小战斗与 SyPB 五档瞄准，
de_dust2 四项战斗代表场景通过。可选 agent 意图与确定性 Python 客户端已接，过期后回到本地核心。
M3 尚未整体完成，武器选择、经济、梯子及更多组合场景继续分项推进。

| 能力 | 当前实现与验收边界 |
|---|---|
| 假客户端与输入 | 入队、出生、移动与视角已实测；行为统一走 CUserCmd，netvar 写入仅用于开发 / 诊断 |
| 反射 | SendTable 标量、数组、向量分量及完整 EHANDLE 读取已实现 |
| 路点与编辑器 | AGPW 存取、换图重载、手工有向边、A*、按需单源 Dijkstra、只读几何体检、中文 HUD 菜单及叠加层已落地 |
| 导航 | 行走、逐连接跳跃、蹲行、CAMP 与漫游已有实现；地面视角已平滑并与移动方向解耦；全图覆盖仍绑定 F4A8，CAMP 及多 bot 需分项回归 |
| 清障 | cs_office 的枪 / 刀、连续障碍、双向续路、攻击中 stop / death 及受控超时已在 28BC 回归；自然堵枪、六次无伤、换弹 / 换枪及更多类型仍待对应验收 |
| 普通任务 | Normal、Move、CAMP、Pause、DestroyBreakable 与地图目标共用任务仲裁，支持优先级、挂起 / 恢复、期限与失败反馈 |
| 普通战斗 | 本地选敌、视线、当前武器射击 / 换弹、失敌续路及 SyPB 五档瞄准已实现；de_dust2 四项代表检查通过，武器策略与经济尚未完成 |
| 地图目标 | C4 拾取 / 安放 / 拆除 / 逃爆、人质跟随 / 护送已有实现；本轮炸弹结果见下，人质仅有历史代表样本 |
| 模式与目标游戏 | csczs / css / csgo 编译期分支已接；`agpb_mode` 的 normal / zombie 开关已接，zombie 行为尚未实现 |
| 外部 agent / LLM | 有时限的 move / camp 意图与确定性 Python 客户端已接；续期及失联后兜底已有实测，M4 尚未整体完成，LLM 战术层未启动 |

普通模式行为以 SyPB 为基线、CS-EBOT 作实现对照；引擎源码用于核实公开接口、物理与伤害规则。
起跳由 AgPB 决定，自动蹲跳交游戏移动层。一般真人 / 临时阻挡的等待、避让和自动续原目标，
后续由任务层统一处理，不另建导航状态机，也不作为当前最小战斗的前置条件。

## de_dust2 全图验收基线（F4A8，冻结）

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

## M3a 生命周期验收

验收 DLL：`DA6BED7711D86017313AEBA8BFB2104E33F5B1AB13E74BB28F37E180B1517C6C`。
在 csczs / normal、de_dust2 的单 bot 手动导航上，停止朝向、行走冻结续原路、终点冻结重新停稳、
死亡重生、换队、真实回合及安全换图 **七项通过**；新构建不继承上节 175 终点 / 360 方向及炸弹全量结论。
冻结须真实 FL_FROZEN，保留原路线并重新累计连续 0.8 秒停稳；存活只认反射的 LIFE_ALIVE。
真人 slot 1 / userid 2；原 bot userid 3，新图 bot userid 4；换队按 CT→观察者→T→观察者→CT，
换图按先踢零 bot、de_dust2→cs_office→de_dust2。核心 CUserCmd 与独立 probe / 真实事件交叉核验。

证据：`build/waypoint-output/m3a-lifecycle-20261023-final/` 的 `acceptance.json`、`audit-lifecycle.json`、
原生日志及清理记录；绑定计划、AGPW、BSP、DLL 和两份 SMX。Python **116 项通过**；
F4A8 冻结包 147/147 件复核一致，未回写。收尾确认零 bot、夹具卸载、九项设置读回恢复及游戏退出。
此 M3a 矩阵不覆盖攻击中断、冻结与死亡重叠、CAMP、多 bot 或一般动态阻挡；后续分项结果见下。


## M3b 必要清障验收

验收 DLL：`28BC319BC6C272466AF1B61B1D3314F75386776C227BD8FE1232E5E0E1CA0238`。
csczs / normal、cs_office 既有 144 点图上，枪、刀、连续障碍、预算退出及攻击中 stop / death **六类通过**。
USP 单次 51→61 清除 radio / phone7 后自动续原 owner / goal；仅刀 61→62 清除 phone8 并续行，两个终点严格停稳。
两方向使用不同原生物件，额外未 watch 的物件不认证；原图快照仅记录既有 AGPW，未改点、边或半径。
超时仅认证无路线时一次 DEV 准备远期玩家冷却后，原位 radio 在实际 10 秒期限退出且输入清空；
不据此认证自然堵枪或六次无伤。140 项 Python、八组运动仿真及跳蹲阶段回归通过。
当前姿态会在接地且接近下一手工 CROUCH 段时提前蹲行，起跳 / 空中仍交引擎；de_dust2 的三条跳边及两严格终点回归通过，起跳单 tick 脉冲未采到，不认证未采样 tick。

证据：`build/waypoint-output/m3b-clearance-20261023-final/` 的 `acceptance.json`、`audit-clearance.json`、
`audit-jump-posture.json` 及 `session-cleanup.json`；清障绑定七资产哈希与完整 398 条原始记录，跳台保留 592 条输入快照。
收尾已确认零 bot、三夹具卸载、十项设置读回恢复、真人静止接地及游戏退出；旧两份冻结包未回写。

## M3c 最小战斗、难度与意图入口

当前 DLL：`030BCA9798FD3FF3591246E122A2C6C05C9D3F1DDE5548270A611BFB753B7FD3`。
csczs / normal、de_dust2；AGPW、BSP 与手工计划未改。普通行为继续以 SyPB 为主线、EBot 作对照，
没有 agent 时本地核心自行规划与执行，ZBot 只在下述短测中作为对手。

| 本轮范围 | 验收结论与边界 |
|---|---|
| 最小战斗 | 同队目标不攻击、眼点遮挡且不获取目标、真实伤害后续原路线、开火中 stop 四项通过；分别绑定实际身份与六资产哈希 |
| 伤害与续路 | 代表场景 8 次真实开火、4 次目标受伤事件；独立快照确认伤害，原 owner=13 / goal=60 / 战斗中的 hop 保留，失敌后无需重发 goto 继续；终点连续 3.225 秒严格停稳 |
| 动作切换 | 战斗通过 CUserCmd 暂停地面移动，释放目标后交回原任务；目标及武器用完整句柄核验。冻结、死亡等仍走统一清理；本轮不扩大其它攻击中断的验收范围 |
| SyPB 五档 | `0..4` 分别取 skill `1–29 / 30–49 / 50–79 / 80–99 / 100`；保留反应、低技能偏差、头部选择、精确瞄准与后坐力停火。每档各采到两次真实开火，仅认证代表采样 |
| Godlike | 满足开火意图时 skill≥70 直接对准，100 另有背后识敌 / 源码预判分支；这并非五档统一调转向速度。档位 3 / 4 的样本有真实头部命中，不认证全武器命中率 |
| 地面转向 | 按 SyPB 比例转向并保留世界移动方向；不改手工点、边与半径。跳跃 / 空中 / 梯子不套用该地面转向策略，中间路点的一 tick 空输入仍待后续处理 |
| agent 干预 | 原生控制台只接有期限的 move / camp；不关闭本地自主。相同意图续期保留 token / 路线，过期后恢复本地任务；Python 客户端实测连续三次同 token 提交（含两次续期），退出后不再续期并自动回退 |

意图限定当前地图 / 回合与实际 userid，TTL 为 0.5–30 游戏秒；手动 stop 保持停止。
客户端不接管既有租约、不自动打开 auto，也不输出逐 tick 动作；尚未连接 LLM 战术层。
四项战斗的对手例外仅限本轮用户明确授权：真人与受测 AgPB 同 CT，一名受控敌队假客户端，
事前及采样中核对匹配引擎的 Radio guard 为 active / intact。

### 原生 ZBot 对手短测

按本轮另行授权，一名 skill=100 AgPB CT 对两名 `bot_add_t expert` 的原生 T，
真人 CT 不参战；原生 NAV 从现成 VPK 只读加载，没有生成或改写导航资产。
双方准备为 M4A1、30 发弹匣 / 90 发备弹、100 HP、无甲；AgPB 的一次备弹准备走 DEV 反射入口，
仅发生在冻结、战斗关闭阶段，不属于购买或 AI 自动补给。

这一次指定中路意图的短测中，AgPB **13 次开火、7 次真实伤害、2 次击杀、存活 88 HP**；
对手共 3 次开火、1 次命中，真人 0 开火 / 0 伤害。它是单次固定装备探索，不能据此评定完整对局强弱、
经济、武器策略或长期稳定性。AgPB 仍没有购买、存钱与经济局规划。

证据入口：`build/waypoint-output/m3c-combat-20261023-final/` 的 `acceptance.json`、
`audit-combat.json`、`audit-native-match.json`、五档原始观察及 agent 回执。
四组 C++ 策略 / 运动检查通过；离线检查分为路点 / 导航工作流 140 项、独立战斗验证 18 项与 agent 客户端 4 项。
收尾确认零假客户端、测试插件卸载、原设置独立读回恢复；按用户要求退出游戏，实际退出码为 0。
收尾发现真人有后续输入，未声称该阶段真人静止。此前三个冻结交付包保持原样。

## 未完成与待验收

| 项目 | 当前边界 / 后续动作 |
|---|---|
| 生命周期扩展 | 单 bot 导航矩阵及清障中的 stop / death 已通过；攻击中的换队 / 回合 / 换图、CAMP 及多 bot 待验 |
| 一般阻挡恢复 | 仅有有限落地 / 清障重试；一般阻挡时等待 / 避让及解除后自动续原目标尚未完成 |
| 清障覆盖 | 已补脚边低体积扫掠，仍是接触近似；完整近身接触、属性 / 类型覆盖与战斗中清障后续实现 |
| 普通战斗扩展 | 最小地面闭环及五档瞄准已接；追敌、战斗走位、友军挡枪、自然失去视线与更多武器 / 中断组合仍需对应实测 |
| 武器与经济 | 战斗仅使用当前持有武器；择优选枪、购买、存钱、经济局及完整补给决策尚未实现 |
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
| M3a 基础导航 | de_dust2 冻结全图基线及新构建单 bot 生命周期已通过；范围见上 |
| M3b 清障与恢复 | 必要清障六类代表矩阵已通过；一般等待 / 避让 / 续路仍由任务层后续实现 |
| M3c 普通最小战斗 | 四项 de_dust2 代表检查通过，SyPB 五档已有开火采样；后续补武器、经济与更多组合回归 |
| M3d 地图能力 | 炸弹已有当前代表场景；梯子、门、更多目标与地图分项推进 |
| M3e ZM / ZE | 普通模式稳定后接运行期 profile，共用导航、格式与动作核心 |
| M4 外部意图桥 | 最小有时限意图与确定性 Python 客户端已接并验证续期 / 兜底；更多意图、观察协议及接入仍待收束 |
| M5 LLM 战术 | M4 及本地行为能独立执行与恢复后启动 |

1. **M3a**：保留已通过的单 bot 导航 / 生命周期作为基线；后续改动按影响范围回归。
   攻击中的换队 / 回合 / 换图、CAMP 与多 bot 另取样本，不扩大为长期稳定性。
2. **M3b**：保留枪刀 / 连续障碍 / 中断 / 受控超时基线；自然遮线、六次无伤、换弹 / 换枪等按影响补测。
   一般动态阻挡恢复继续后置，不作为 M3c 的前置；不扩大当前单 bot / 代表物件结论。
3. **M3c**：保留四项最小战斗与五档瞄准代表样本；后续补择优选枪、经济、更多中断与遮挡组合。
   普通行为继续对照 SyPB 与 EBot；手雷、复杂射击、战斗走位和追敌中清障分项推进。
4. **后续阶段**：按实际需求逐项接 M3d / M3e，M4 不要求先完成全部地图、目标或僵尸模式。
   外部层只给意图，不能代替本地移动 / 清障 / 恢复，也不承担逐 tick 操作；协议在 M4 启动时收束。

## 源码与工具入口

| 入口 | 用途 |
|---|---|
| `src/plugin.cpp` / `src/bot.*` | MMS 入口、命令、生命周期、CUserCmd 与导航 |
| `src/route_control.h` / `src/bomb_escape_policy.h` | 终点控制、跳跃落地及逃爆预算 / 重试策略 |
| `src/tasks.*` / `src/bot_tasks.cpp` / `src/bot_intention.cpp` | 任务优先级、挂起 / 恢复、规划、路线归属及有时限的外部意图 |
| `src/bot_combat.cpp` / `src/combat_skill.h` / `src/movement_control.h` | 本地战斗、SyPB 五档瞄准、地面视角与移动投影 |
| `tools/agpb_agent.py` | 有限时长的确定性意图客户端；不代替本地行为核心 |
| `src/objectives.*` / `src/bot_objective_actions.cpp` | 只读 C4 / 人质快照与交互动作 |
| `src/obstacle.*` / `src/netvars.*` | 障碍检测、实体分类与 SendTable 反射 |
| `src/waypoint.*` / `src/wpedit.*` / `src/wpdraw.*` / `src/menu.*` | 路点、体检、编辑与 HUD |
| `PackageScript` / `AMBuilder` | 分发目录组装与源码构建清单 |
| [`waypoint-workflow/README.md`](waypoint-workflow/README.md) | 路点工作流的统一说明入口，含原生控制台、TGA、人工计划与版本绑定验收 |
| [`combat-tests/README.md`](combat-tests/README.md) | 独立战斗验证、难度采样、对手短测与战斗证据离线复核；战斗专用脚本及夹具集中在此目录 |
| [`tools/agpw_view3d_README.md`](tools/agpw_view3d_README.md) | 独立 3D 查看 / radius 与 flags 编辑；位置、连线与动态碰撞不在支持范围 |

构建涉及头文件时必须确保消费者重编；新增 cpp 加入 AMBuilder，构建脚本保持 ASCII。

### 游戏差异登记

仅登记用户已确认的条目；后续按 AGENTS 的 `AGPB_GAME_*` / `[game-diff]` 规则显式分支，不自行猜测差异。

| 功能 | csczs（当前） | css | csgo | 实现位置 |
|---|---|---|---|---|
| 站立 / 蹲姿体积高度 | 随 `sv_cs_use_legacy_viewvectors` 支持 62/45 与 72/54 | 编译期固定 62/45 | 编译期固定 72/54 | `AgPB_RefreshHullSize()`（`src/waypoint.cpp`） |

## 当前可清理资料与停用工具

临时资料与 junction 只列完整路径，由用户自行删除；见最新 M3c 冻结包的 `cleanup-paths.txt`。
正式 AGPW、源计划及交付包保留；未执行文件删除。

独立 2D 路点工具已从维护入口和制作流程移除；以下遗留文件待用户自行删除：

- `E:\Plugins-Platform\AgPB\tools\agpw_view.py`
- `E:\Plugins-Platform\AgPB\tools\agpw_view.bat`
