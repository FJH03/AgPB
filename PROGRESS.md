# PROGRESS — AgPB Agent 交接

更新于 2026-11-22。当前处于 **M3 普通模式导航与本地自主行为移植**。普通模式以 SyPB 为主、CS-EBOT 为对照。引擎源码用于核实公开接口、物理与伤害规则。 [项目约束](AGENTS.md)

| 接手任务 | 先读 |
|---|---|
| 项目约束、构建与部署 | [AGENTS.md](AGENTS.md) · [README.md](README.md) |
| 命令、菜单与回归验收 | [COMMANDS.md](COMMANDS.md) · [VERIFY.md](VERIFY.md) |
| 手工制图、原生控制台、TGA 与 3D 编辑 | [路点工作流](waypoint-workflow/README.md) · [3D 工具](tools/agpw_view3d_README.md) |
| 战斗、难度及原生 bot 对手验证 | [独立战斗验证](combat-tests/README.md) |
| csczs 假客户端无线电兼容 | [CRASH_REPORT.md](CRASH_REPORT.md) |

## 当前部署

csczs / Win64 的裸 DLL、分发包和现服 DLL 的 SHA-256 均为 `A9F82FD3F257F726459F3D58AB5FE56E08863236668A18696578ACD19A67B8BD`。历史实机结果各自绑定验收用 DLL、地图及路点。

| 部署地图 | 当前 AGPW | 已验收及待办 |
|---|---|---|
| cs_office | v2 · 405 点 / 1,136 有向边 / 61 跳边 · `81F0A7F66F4AE51138CCF50E99FF0310F0E4AD09EAAF81B673840577F92CD5CD` | 用户游戏保存版。新版本逐项实走待补。 [仓库 AGPW](waypoints/cs_office.agpw) |
| de_dust2 | v2 · 209 点 / 599 有向边 / 14 跳边 · `F229F1528EBF3D37F3C0B78B0EAB8E1BF64ADB1F3EA2372AA28828CE0AC4F087` | 用户游戏保存版。待对照原生 NAV / BSP 复核分区、多层覆盖及实走。 [仓库 AGPW](waypoints/de_dust2.agpw) |

两张 AGPW 的仓库、分发包和游戏文件 SHA-256 一致。已通过 `xcopy` 覆盖整个分发包的 `addons/`，包括 `configs/defaults/` 下的 `names.cfg` 与 `weapons.cfg`。无 bot 启动冒烟通过，`IBotManager=ok`，Dust2 与 Office 分别加载 209 与 405 点。

- office nav10 旧版有 405 点 / 1,080 条有向边，T / CT 有向图强连通，覆盖 14 个原生 NAV 命名区。两把梯子双向 4/4 通过（`926C`，同队 CT）。两处雨棚和泵坑深尾已布点，最高车库小箱因蹲姿净空不足排除。 [旧计划](build/waypoint-output/office-nav10-closeout-20261121/cs_office.plan.json) · [离线审计](build/waypoint-output/office-nav10-closeout-20261121/offline-audit.json) · [梯子实走](build/waypoint-output/office-nav10-closeout-20261121/ladder-verification.json) · [空间核对](build/waypoint-output/office-full-20261029-98d4da1b/office-spatial-review-nav08.md)
- Dust2 F4A8 旧版通过 175/175 终点、360/360 边和 A/B 的 T 与 CT 逃爆、CT 拆包六项。拾包及安放证据覆盖指定路线。 [冻结验收](build/waypoint-output/de_dust2-20261023-final/bundle-index.json)

## 接手清单

路点空间覆盖、图连通和逐项实走分别验收。严格停稳要求 XY≤max(原半径, 4)、Z≤4、接地、速度≤5，连续达到 0.8 秒。 [验收标准](VERIFY.md)

- **底座与导航** 无 AI 假客户端、SendTable 标量 / 数组 / 向量 / EHANDLE、CUserCmd、手工 AGPW 编辑和寻路。行走、蹲行、梯子、地面视角与移动解耦。单 bot 手动导航七项生命周期通过（DA6B）。

  **接续与待验收** 攻击中换队 / 回合 / 换图、冻结与死亡重叠、驻守与多 bot 生命周期，扩大梯子实测。门行为待补，中间点切换的一 tick 空输入仍待复核。

- **弹道跳与编辑** 手工有向跳边支持 SyPB 起跳前世界 XYZ 初速度，默认零走普通跳跃，起跳对准跳跃方向。非零注入仅覆盖静态世界支撑。AGPW v1/v2 兼容，3D 工具可改半径、标志和现有跳边 XYZ。准星反馈已有用户截图确认。

  **接续与待验收** 同一落地点核对 `agpb_wp_nearest`、`getpos 2`、默认 `getpos` 视点及计划 Z，继续验证关闭 xray、密集和多层选点。游戏内移动点与撤销待补，3D 工具的位置与连线仍只读。

- **清障与避让** 枪刀、连续障碍、受控超时及攻击中 stop / death 的代表矩阵通过（28BC）。同队玩家让行保留原目标，历史三项通过（414C8），已接可移动物件避障。

  **接续与待验收** 当前让行策略待 Dust2 同队回归。物件避障曾在 7742／office nav03 代表场景走通，当前部署待回归。清障待验自然堵枪、六次无伤及换弹 / 换枪，继续补初始重叠、近身接触、狭道、拥挤交战和不同物件类型。

- **普通战斗** 保留 SyPB 五档 difficulty 和 Godlike，按武器 / 距离 / 局势选进退、横移或停枪。逐枪主攻击与驻守代表场景已验。CAMP 保持位置，CAMP+CROUCH 到位后才蹲，交战仍按原期限。

  **接续与待验收** 更多武器、友军遮挡、攻击中断与多 bot。完整刀法、致盲射击及最后位置压制待补。穿透和敌情调查按用户安排后置，待验遮挡部位、不同墙体 / 动态物件实伤、失敌和真实声源。

- **武器与经济** SyPB 固定选枪、空闲换弹、近战拔刀、三种性格购买与拾枪。Glock/FAMAS 三连发、SyPB 开镜和 ZBot 消音器偏好已接。11 月 22 日同队 CT 监听局有钱购买通过，SG550 等六笔确认，余额 16000→9350。

  **接续与待验收** 经济局、实际交战切枪、主动拾枪和拾枪视角，以及三连发、开镜和消音器的实机效果待用户验收。

- **菜单与平台** 机器人管理五项及排队填满服务器已接。首版同队 CT 核验了指定性格、技能 100、csczs class 10 和三种踢人。csczs 每队五个外观，CS:S 四个，CS:GO 暂随机。

  **接续与待验收** 待用户验收新键位、快速添加自动选队、填满及取消队列。待验 CS:S 外观及 CS:GO SDK。与 SyPB 相比仍缺结束回合、武器模式、默认性格、调试切换、双人跳协助、丢弃 C4／武器、检查后保存、人质禁行及驻守朝向终点。

- **地图目标与模式** 普通模式有 C4 拾取、安放、拆除、逃爆及人质跟随 / 护送。运行期 `agpb_mode normal` / `zombie` 开关已接。

  **接续与待验收** office 曾确认两名人质实际获救，更多人质路线及地图待验。携包选点、全图搜包、SyPB 经验和危险评分待补。僵尸模式行为尚未实现。 [按地图类型规划](waypoint-workflow/references/map-types.md)

- **自主决策与 agent** 本地按感知、人格、攻守职责和占用自主选任务、巡逻、驻守与调查。高层目前只提交有期限的 `move` / `camp`，绑定地图、回合及 userid，TTL 为 0.5–30 游戏秒。相同意图续期保留 token / 路线，失联或到期清理意图所属子任务并回到本地。手动 stop 保持停止。

  **接续与待验收** 真实紧急目标下的高层清障与长期多 bot 待验。M4 仍需扩展意图与观察协议，地图与 ZM/ZE 可并行。M5 LLM+harness 待接入，高层仲裁任务，动作仍由本地 CUserCmd 执行。

导航及生命周期见 [bot.cpp](src/bot.cpp)，本地任务和观察见 [bot_tasks.cpp](src/bot_tasks.cpp)、[bot_autonomy.cpp](src/bot_autonomy.cpp)、[bot_search.cpp](src/bot_search.cpp)。高层意图见 [bot_intention.cpp](src/bot_intention.cpp) 和 [Python 客户端](tools/agpb_agent.py)。战斗见 [bot_combat.cpp](src/bot_combat.cpp)，选枪、购买与拾枪分别见 [bot_weapons.cpp](src/bot_weapons.cpp)、[bot_purchase.cpp](src/bot_purchase.cpp)、[bot_pickup.cpp](src/bot_pickup.cpp)。

### 阶段衔接

M3a 单 bot 生命周期、M3b 必要清障、M3c 最小战斗已有代表验收，后续按本次改动复跑。一般动态阻挡可与 M3c 并行。M3d 分批补梯子、门和地图目标，M3e 目前保留模式开关。M4 可与地图工作并行，M5 依赖 M4 协议与本地执行、恢复闭环。 [各阶段验收标准](VERIFY.md#主线阶段验收标准)

## 开放限制与构建提示

- **Radio** 补丁 1.0.2 已同步到分发包与游戏目录，适配 11 月 7 日版 csczs。匹配当前引擎的 `active=1 intact=1` 和实机触发仍需按兼容说明核对。测试常规保持同队，真人退出、换队、切观察者或换场景前先执行 `agpb_kick all`。受控敌对场景需逐次获得用户授权。 [兼容说明](CRASH_REPORT.md)
- **诊断** `agpb_ents` 异常退出根因待查，先用单实体只读查询。部分路点 Add / Load、调试速度和 netvar 写入仍缺有限值检查。原生 `jpeg` 会触发 engine.dll 崩溃，暂不修复，截图用 TGA。几何体检的跳跃提示仅供只读复核。
- **游戏差异** csczs 站立 / 蹲姿体积随 `sv_cs_use_legacy_viewvectors` 取 62/45 或 72/54，CS:S 固定 62/45，CS:GO 固定 72/54。入口为 [AgPB_RefreshHullSize](src/waypoint.cpp)。差异按 AGENTS 规定显式分支。
- **构建** [AMBuilder](AMBuilder) 登记 `src/` 头文件并按内容变化触发 AgPB C++ 重编。新增 `.cpp` 要登记，构建脚本保持 ASCII。服务器占用 DLL 时先停服。
- **测试工具** 旧游戏夹具已移出，现行夹具可按路点和战斗工作流从源码重编。日常查看与属性编辑使用独立 3D 工具。

## 冻结验收索引

证据位于本机 Git 忽略的 `build/`。复跑前核对 AGPW、BSP、DLL 和对应夹具版本。

| 基线 | 结论与证据 |
|---|---|
| M3a / M3b / M3c | DA6B 单 bot 生命周期 7 项，28BC 必要清障 6 类，030B 最小战斗 4 项与意图续期。 [生命周期](build/waypoint-output/m3a-lifecycle-20261023-final/acceptance.json) · [清障](build/waypoint-output/m3b-clearance-20261023-final/acceptance.json) · [最小战斗](build/waypoint-output/m3c-combat-20261023-final/acceptance.json) |
| 跳跃 | 9611 旧图的 34 条跳边与 9 段连续路线曾通过。局部调整分别绑定 414C8 / DAA326，144↔148 反向自然接近、连续往返和最终回存为旧图留待核对。 [跳边矩阵](build/waypoint-output/jump-config-20261024-final/acceptance-summary.json) · [蹲跳 52→62](build/waypoint-output/crouch-jump-20261024-daa326df/acceptance-summary.json) · [83↔86](build/waypoint-output/office83-86-bidirectional-20261024-d28984cc/acceptance-summary.json) · [144↔148](build/waypoint-output/office144-148-20261024-7d20ffdf/acceptance-summary.json) |
| 让行与驻守 | 414C8 三种让行及低 / 高技能 M4 对照，14062 驻守接近、到位和离开姿态。 [玩家让行](build/waypoint-output/player-yield-20261024-414c8b46/acceptance-summary.json) · [M4 调用顺序](build/combat-output/fire-frame-20261024-414c8b46/acceptance-summary.json) · [驻守姿态](build/waypoint-output/camp-approach-20261024-14062a05/acceptance-summary.json) |
| 战斗与武器 | 0979 四项移动交火和武器切换冒烟，6D985 十三项射击节奏 / 走位 / 驻守。 [战斗移动](build/combat-output/ground-movement-20261024-09797eea/acceptance-summary.json) · [武器选择](build/combat-output/owned-weapons-20261024-09797eea/smoke-summary.json) · [射击与驻守](build/combat-output/primary-camp-20261024-6d985fc7/acceptance-summary.json) |
| 本地自主 | 133D 同队五 bot 自主选点、驻守巡视及高层意图到期后回退。 [自主冒烟](build/combat-output/local-autonomy-20261023-133ddc3f/smoke-summary.json) |
