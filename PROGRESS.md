# PROGRESS — AgPB

> 只写「现状 / 问题→怎么解决 / 坑 / 下一步」。函数名、地址、伪代码这类证据不在这里堆，
> 要翻的话看 git 历史里的 `ARCHIVE.md`（已删除）和源码注释。
> 硬性要求见 [`AGENTS.md`](AGENTS.md)；构建 / 部署命令在 [`README.md`](README.md)，验收在 [`VERIFY.md`](VERIFY.md)。

## 现状（2026-10-02）

- 能创建**无 AI 假客户端**，正常入队、出生，并由 `CUserCmd` 驱动移动与视角（已实测）。
- netvar 反射可用：按名字读实体的网络字段（标量 / 数组 / 向量元素 / EHANDLE）。
- 路点系统可用：打点 / 连线 / 删除 / 存盘 / 换图自动重载 / A* 寻路 / 路径代价 /
  几何体检（可达性、要不要跳）。
- 编辑器可用：HUD 中文数字菜单 + `agpb_wp_*` 命令 + 叠加层绘制（EBot 配色，上服实测）。
- 最小导航可用：`agpb_bot_goto` 沿路点转向 / 前进、按边起跳、按点蹲行、卡住就停；
  `agpb_bot_roam` 随机漫游（EBot/SyPB 式随机挑点，到了再挑；带目标分散 +
  最简队友让行，避免一坨 bot 堵门）；到 **CAMP** 点会原地蹲守一段随机时间
  （`agpb_camp_min`/`agpb_camp_max` 默认 8~20 秒，点带 CROUCH 就保持蹲姿）。
- 打包 / 部署可用：`ambuild` 产出 SourceMod 目录约定的 `build/package/addons/...`
  （`PackageScript`，不压 zip）；构建命令最后用 `xcopy` 覆盖到游戏 mod 目录
  （部署目标在 AGENTS.md §2，单一出处，改一行即可）。
- 模式开关：`agpb_mode`（normal / zombie）已落地（运行期、按图手动切，菜单显示当前模式）；
  **僵尸模式行为未实现**。
- 工具：`tools\agpw_view.py`（+ `.bat`）只读查看 `.agpw` 路点图 —— 图论力导向布局、
  单向/双向边、边 flag 着色、点选节点看出入边；默认打开 deploy 配置指向的游戏目录。
- 没做：EBot 替身层（`Entity` / `Client` / `Engine`）、真正的 `navigate` / `control` / `combat`、
  UDP 桥、LLM 战术层、**梯子（爬梯行为未实现）**。
- 已知问题：freezetime 时 bot 独占一队会触发 `Radio()` 崩溃（**未修**，见
  [`CRASH_REPORT.md`](CRASH_REPORT.md)，测试必须同队）；netvar 写入层已降级为开发工具。

## 里程碑

| 阶段 | 内容 | 状态 |
|---|---|---|
| M1 | 无 AI 假客户端 + ucmd 注入 + 队伍切换 | ✅ |
| M2 | netvar 反射层 | ✅ 已实测 |
| M2.5 | ucmd 注入端到端验证 | ✅ 已实测 |
| M3 | EBot 移植（路点先行） | 🟡 路点 / 编辑器 / 最小导航已落地 |
| M4 | UDP 桥 + Python agent | ⬜ |
| M5 | LLM 战术层 | ⬜ |

## Agent 架构（M4 / M5 计划）

- 分层：客观事实（队伍 / 血量 / 距离 / **可见性**）由插件算；主观优先级（打谁、去哪、
  保枪还是拼）给 LLM；执行（瞄准平滑、开火时机、走位）回到插件。
- 频率：服务器 66 tick（15.15 ms/帧），反射层必须本地跑满；agent 循环天然只有 1~5 Hz。
- 传输：插件侧工作线程收 UDP，主线程每 tick 取最新意图，绝不阻塞服务器；agent 掉线 →
  意图过期 → 保持视角、停火。

## 问题 → 解决

| # | 现象 | 根因 / 结论 | 处理 |
|---|---|---|---|
| 1 | 想自己控制 bot，又不能让引擎 bot AI 介入 | `server.dll` 暴露 `BotManager001`，它创建的是无 AI 假客户端 | 用该接口建 bot；`RunPlayerMove` 与引擎驱动真人同路径 |
| 2 | 给假客户端发命令没反应，bot 一直不选队 | `engine->ClientCommand` 是 stuffcmd，要真人客户端回传 | 改走 `helpers->ClientCommand`（服务端本地执行） |
| 3 | 入队了但不出生 | `ChangeTeam` 只到选职业状态 | 补 `joinclass 0`；`hp=0` / 坐标全 0 时 `mp_restartgame 1` |
| 4 | 读实体字段没有稳定入口 | datamap 的虚函数索引编译期不可知（SourceMod 也要 gamedata） | 改走 SendTable 反射：名字 → 偏移 → 类型，零索引 / 零特征码 |
| 5 | 反射读出来偏移 / 数组不对 | Source 有两套数组机制、VECTORELEM 是负偏移、int 宽度只在 proxy 里 | 全按 SendTable 元数据展开；EHANDLE 直接读本地 8 字节句柄 |
| 6 | 不确定 ucmd 是否真的驱动了玩家 | —— | 注入 forwardmove + yaw，速度 / 视角字段跟着变，M2.5 实测通过 |
| 7 | `.nav` 寻路会「说连通、实际过不去」 | nav 的连通性是几何推出来的 | 放弃 `.nav`，改手工路点图：连边是人验证过的动作 |
| 8 | EBot 的 128MB 距离矩阵太重 | 矩阵只是 A* 的启发式 | 改单源 Dijkstra 按需算，最短路结果一样 |
| 9 | 想要 EBot 那种左上角数字菜单 | `CreateMessage` 在 CS:S 里弹 VGUI 对话框 | 用 `ShowMenu` usermessage + `menuselect` |
| 10 | 菜单 5 秒自己消失 / 太长不显示 | 客户端菜单超时；单条 usermessage 上限 255 字节 | 每 3 秒重发续命；超长按行分段发送 |
| 11 | 中文乱码 | 客户端 HUD 走 UTF-8；服务端控制台是 GBK | 菜单发中文，控制台输出保持英文 |
| 12 | 到达半径要一个个手调 | EBot 加点会把半径覆盖成固定 64 | 曾移植 wayzone 自动算；**2026-10-02 按用户要求删除**，改成加点固定默认值 + `agpb_wp_radius` 手调 |
| 13 | 蹲点走不进去 / 跳跃高度不对 | 到达判定用了兜底大半径；CS:S 站立跳 57、蹲跳 42；引擎会自动 crouch-jump，注入 `IN_DUCK` 反而打断 | 按 EBot 到达规则判定；跳跃边只按跳；蹲行段提前松蹲 |
| 14 | netvar 能读，能不能当行为手段写 | 写服务端字段等于改实体行为，偏离「只生成 ucmd」 | 写入层只留作开发 / 诊断工具；navigate / control / combat 一律走 ucmd |
| 15 | 想写手雷速度做定点投雷 | Source 里是非网络字段，而且本来不需要 | 投雷初速由视角 + 玩家当前速度决定，走 ucmd 即可 |
| 16 | wayzone 自动半径在平地也大量算出 0（cs_office 实测 98 点里 72 个 0，且没有任何 16/32/48/64/80/96 档） | 两处"向下探地面"判断写反：**命中有地面**被当成阻塞（EBot/SyPB 是"没命中才判没地面 → blocked"），平地第一档 32 就挂 | 先修正方向，随后按用户要求**整体删除自动算**；加点改固定默认（精确类 0 / Camp 32 / Jump 4 / 其它 64） |

## 踩过的坑

- `IVEngineServer::ClientCommand` 永远不要用来驱动假客户端（stuffcmd）。
- 假客户端必须保留 `FL_FAKECLIENT`（引擎内部按 `IsBot()` 分支）。
- bot 所在队伍里要有一个 slot 更小的人类玩家，否则 freezetime 会崩（见 `CRASH_REPORT.md`）。
- 控制台是 GBK：日志全英文，只有 HUD 菜单能用中文。
- 改 `src/*.h` 或新增 `src/*.cpp`：AMBuild 不跟踪头文件依赖，新 cpp 要加进 `AMBuilder`，
  否则出现「代码没生效」或 `LNK2019`。
- 构建脚本里不能有非 ASCII；源码要 `/utf-8`；SDK 静态库必须先编出来。
- EHANDLE 不能用 `agpb_netlist` 的值判读（那是网络压缩值），用 `agpb_nethandle` 看本地句柄。
- `agpb_wp_del` 会让后面的下标整体前移，删完重看 `agpb_wp_list`。
- 几何判定只做体检（`agpb_wp_check` / `agpb_wp_reach`），跳跃标志一律手工连。
- netvar 只能碰 SendTable 里有的字段：`agpb_netlist` 里没有就是没有（要写非网络字段得另开 datamap，暂不做）。
- `string_t` 不能跟 `0` / `NULL` 比，先 `STRING()` 再判空串；字符串 / 内存操作用 SDK 的 `V_*` / `Q_*`，别直接上 C 运行时。
- `setpos` 是客户端命令（要走 stuffcmd 发给真人客户端）；`noclip` 是服务端命令（走 `helpers->ClientCommand`）。

## 关键路径

| 位置 | 说明 |
|---|---|
| `src/plugin.cpp` | MMS 入口、GameFrame 钩子、命令注册与分发 |
| `src/bot.cpp` | 假客户端生命周期、队伍切换、ucmd 驱动、最小导航 |
| `src/netvars.*` | SendTable 反射层 / 写入层 |
| `src/waypoint.*` | 路点数据、文件 IO、A*、路径代价、几何判定 |
| `src/menu.*` | ShowMenu 菜单框架（中文 HUD） |
| `src/wpedit.*` | 编辑器状态、动作、ConVar |
| `src/wpdraw.*` | 叠加层绘制（EBot 配色） |
| `addons/AgPB/waypoints/<map>.agpw` | 每张图的路点数据（换图自动读盘） |
| `PackageScript` | AMBuild 打包脚本：组装 `build/package/addons/`（纯 ASCII） |
| `tools/agpw_view.py` | 只读路点图查看器（图论布局 / 单向双向 / flag 着色） |

## 已定决策（摘要）

- 不用 SourceMod；不用 `CCSBot`；不写服务端实体字段（写入层只做诊断）。
- 多目标适配：csczs（当前）/ css / csgo；游戏差异由用户指出后逐条登记，
  实现处用 `AGPB_GAME_*` 宏隔离（编译期硬编码，不探内存）。
- 移植基线用 **SyPB**（通用 bot），CS-EBOT 只作实现对照（自述仅面向 ZP/ZE/Biohazard）；
  同名文件 diff 着看，僵尸专用分支直接砍。
- netvar 走 SendTable（零特征码）；寻路用手工路点图 + 单源 Dijkstra；不做自动连线。
- 菜单用 `ShowMenu` usermessage，中文；控制台英文。
- 尺寸 / 跳跃高度取 CS:S 源码：csczs 运行期判定（跟随 `sv_cs_use_legacy_viewvectors`，
  站立 62 / 蹲姿 45 或 72/54），css / csgo 编译期固定各自规格；跳 57、蹲跳 42。
- 替身层的 `entvars_t` 走访存属性（代理类型），不再走影子结构方案。
- 许可证 GPL-3.0（要移植 EBot / SyPB 的代码）。

## 移植对照（SyPB ↔ CS-EBOT）

两边同名核心 10 个文件：`basecode` / `combat` / `control` / `engine` / `globals` /
`interface` / `navigate` / `netmsg` / `support` / `waypoint` —— **以 SyPB 版为基线**，
EBot 版逐个 diff，取它更新的实现（异步寻路、符号版本化等），僵尸专用分支砍掉。

- EBot 独有：`ssm/`（战斗状态机：投雷 / 致盲 / 破门 / 用按钮…）、`clib`、`bot_query_hook*`、`tinythread`
- SyPB 独有：`Experience`（经验 / 技能）、`chatlib`（聊天）—— 我们用不上

### 路点系统对照（SyPB / CS-EBOT / AgPB）

| 维度 | SyPB | CS-EBOT | AgPB（已落地） |
|---|---|---|---|
| 数据模型 | `Path` 带 camp 矩形 / `connectionVelocity[8]` / `distances[8]` / 可见性数据；上限 **1024** | 精简 `Path`（+ `mesh` / `gravity`）；上限 **8192** | 照 EBot 精简版；私有 `.agpw`（magic + version，逐字段读写） |
| 距离 | 1024² int 矩阵 + `.pmt` 缓存 | 8192² int16 矩阵（128MB，多线程算，可关） | **无矩阵**；单源 Dijkstra（`ComputeDistances`） |
| 寻路 | `navigate.cpp` 堆 + 矩阵查距 | `async_pathfinder` 异步线程 | 帧内同步 A*（二叉堆，欧氏启发式） |
| 可见性 | **预计算**（`InitializeVisibility` / `IsVisible` / `IsDuckVisible`） | 按需 trace | 按需 trace（`AgPB_Trace*`） |
| 半径默认 | `Add()` 末尾自动 `CalculateWayzone`（16 起步、36 方向）；但菜单随后 `SetRadius(g_sautoRadius=32)` 覆盖，只有 `g_sautoWaypoint`（Auto Put Waypoint 模式，**默认关**）开着时才保留自动值 | `Add()` 也自动算（32 起步、18 方向），但菜单一定 `SetRadius(64/32/0)` 覆盖，没有保留开关 | **自动算已删除**（2026-10-02 用户决定）：加点固定 精确类 0 / Camp 32 / Jump 4 / 其它 64，之后 `agpb_wp_radius` 手调 |
| 自动打点 | `CreateBasic` + 下载现成 `.pwf` + XML 导出 | 分析器 / 优化器全家桶 + 空间桶（`AddToBucket`） | 未做（纯手工打点） |
| 原生模式目标逻辑 | **炸弹 / 人质 / goal 分数**（`GetBombPoint` / `SetGoalVisited` / `AddGoalScore`） | 无（僵尸模式用不上） | 未做（只有 flag，没有逻辑） |
| 僵尸专用 | 少（`ZMHMCAMP`） | 多（`ZOMBIEONLY` / `HUMANONLY` / `ZOMBIEPUSH` / `HELICOPTER`…） | 不需要 |
| 几何体检 | `IsNodeReachable` / `Reachable` | 另加 `MustJump` / `CheckCrouchRequirement` | `MustJump` / `IsNodeReachable` / `Reachable`（EBot 版） |
| 文件 | `.pwf`（`PODWAY!` v7） | `.pwf`（`EBOTWP!`，兼容旧版 125/126） | `.agpw`（不兼容两边，刻意） |

结论：底座照 EBot（精简模型 + 几何 + 到达判定 + 单向边），距离矩阵按我们自己的决策改成 Dijkstra；
**原生模式要补的目标逻辑（炸弹/人质/GOAL 分数）只在 SyPB 里有**，EBot 对应位置是空的。

## 双模式支持方案（普通 / ZM-ZE，草案）

目标：同一个 AgPB 同时服务 SyPB 血统的**普通模式**和 EBot 血统的 **ZM/ZE**，
不搞两套代码、不搞两套路点格式。

两个正交的轴：

- **目标游戏**（csczs / css / csgo）—— 编译期 `AGPB_GAME_*`（已落地）。
- **游戏模式**（normal / zombie）—— **运行期 profile**：换图、换模式只切状态，不重编。
  切换入口 `agpb_mode normal|zombie`（用户按图在 cfg 里设置，不做自动判定）；
  当前只落地开关与菜单显示，**僵尸模式行为未实现**。

共用（不分模式）：路点模型 / `.agpw` / A* + 单源 Dijkstra / 几何体检 /
编辑器 / 绘制 / 到达判定。

分模式（行为层钩子，写 navigate / control / combat 时挂上）：

| 钩子 | normal（SyPB 血统） | zombie（EBot 血统） |
|---|---|---|
| `Mode_FilterWaypoint` | `GOAL` / `RESCUE` / `CROSSING` / `NOHOSTAGE` | `ZOMBIEONLY` / `HUMANONLY` / `ZOMBIEPUSH` / `HELICOPTER` / `ZMHMCAMP` |
| `Mode_IsEnemyValid` | 敌我 = 队伍比较 | 僵尸/人类阵营、无敌、隐身（EBot 的 `IsEnemyInvincible` / `IsEnemyHidden`） |
| `Mode_PickGoal` | 炸弹 / 人质 / goal 分数（抄 SyPB） | ZE 逃跑点 / 守点 |
| `Mode_Camp` | `CAMP` / `SNIPER` | ZM camp 点 |

路点 flag 取并集（u32 还剩位），编辑器显示全部、按当前模式标出"本模式有效"。

移植顺序：**先普通模式**（navigate / control / combat 以 SyPB 为基线，目标逻辑来自 SyPB），
再在 mode 钩子里补僵尸模式的 flag / 敌人判定 / camp；共用层保持单套。

## 游戏差异登记（csczs / css / csgo）

> 差异以用户指出的为准，逐条登记；实现处用 `src/game_target.h` 的 `AGPB_GAME_*` 宏显式分支，
> 代码注释标 `[game-diff]`。

| # | 功能点 | csczs（当前） | css | csgo | 实现位置 |
|---|---|---|---|---|---|
| 1 | 玩家碰撞体积 | 有 `sv_cs_use_legacy_viewvectors 1/0`，**同时支持两套**（62/45 与 72/54） | 无此 cvar，固定 **62/45** | 无此 cvar，固定 **72/54** | `AgPB_RefreshHullSize()`（`waypoint.cpp`）`#if AGPB_GAME_*` 分支；CSS / CS:GO 编译期定死 |

## 下一步

1. 拍板：弹道跳留不留 —— 唯一的候选例外是「跳跃边用 `SetVelocityOverride()`」，其余全走 ucmd。
2. 移植替身层（`Engine` → `Client`）→ EBot 的 `navigate` / `control` / `combat`。
3. 修 freezetime 的 `Radio()` 崩溃（在此之前测试必须同队）。
4. 铺路点数据，跑 [`VERIFY.md`](VERIFY.md) 阶段 F / G 回归。
