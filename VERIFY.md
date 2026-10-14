# AgPB 验证清单

> 只列命令与验收标准；现状、开发顺序、历史证据与未验证范围统一看 [`PROGRESS.md`](PROGRESS.md)，命令参数看 [`COMMANDS.md`](COMMANDS.md)。
> A–G 是各模块的基线回归，不代表主线阶段已完成，也不是后续开发顺序。
>
> **测试前置：** 按 [`AGENTS.md`](AGENTS.md) §6，仅在兼容补丁报告 `active=1 intact=1` 后
> 允许敌队、bot 独占队伍与真人观察者场景；未加载、未确认有效或版本不匹配时，bot 必须与
> 活着且处于 STATE_ACTIVE 的人类同队，真人 slot 更小。原始根因见 [`CRASH_REPORT.md`](CRASH_REPORT.md)。
> 换场景 / 换图仍先 `agpb_kick all`；关闭 / 卸载补丁前先踢所有假客户端。

## 准备

```
cstrike/addons/AgPB/bin/win64/agpb_mm.dll
cstrike/addons/metamod/AgPB.vdf
```

加载成功（`IBotManager=ok` 才算能用）：

```
[AgPB] loaded. gpGlobals=..., IBotManager=..., helpers=...
```

---

## 基线 A —— 建 bot 并出生

以下以真人 CT / ACTIVE、slot 更小的同队场景为例；真人在 T 时改用 `agpb_add 2`。

```
agpb_kick all
agpb_add 3              // 3 = CT；不带参数用 agpb_team 默认值 2（T）
mp_restartgame 1        // 不是必须；列表里 hp=0 / 坐标全 0 时补一次
agpb_list
```

期望：`[0] ... team=3 want=3 hp=100` 且坐标有效，武器是 `weapon_usp`。

## 基线 B —— 反射层

```
agpb_netlist 0 m_iHealth
agpb_netlist 0 m_lifeState
agpb_netlist 0 m_vecOrigin
agpb_netlist 0 m_vecVelocity
agpb_netlist 0 m_angEyeAngles
agpb_netlist 0 m_iAmmo
agpb_nethandle 0 m_hActiveWeapon
```

| 字段 | 期望（2026-09-19 基线） |
|---|---|
| `m_iHealth` | `+364` int = 100 |
| `m_lifeState` | `+368` int = 0（内存里是 1 字节 char，宽度只能在 proxy 里看出来） |
| `m_vecOrigin` | `+1076` vec3（同名同偏移出现 3 次是正常的） |
| `m_vecVelocity` | `+888 / +892 / +896`（VECTORELEM，负偏移） |
| `m_angEyeAngles` | `+6984 / +6988` |
| `m_iAmmo` | `+2136` array `[32 x 4]`，USP 备用弹匣在 `[8]` |
| `m_hActiveWeapon` | `entry=97` 且 `resolved=yes class=CWeaponUSP`（serial 每次开服会变） |

> `agpb_netlist` 打印 EHANDLE 时是**网络压缩值**，没有判读价值 —— 句柄一律看 `agpb_nethandle`。

## 基线 C —— ucmd 注入（关键一步）

先手工打两个点并连边（基线 D），然后：

```
agpb_bot_goto 0
// 等 2 秒，看游戏里 bot 是否转身 + 前进
agpb_netlist 0 m_vecVelocity
agpb_netlist 0 m_angEyeAngles
agpb_bot_stop 0
```

期望（2026-09-19 实测基线）：`m_vecVelocity` 方向 = 目标方向、大小 ≈ 250（USP 上限）；
`m_angEyeAngles[1]` = 目标方向的 yaw。

判读：

| 现象 | 结论 |
|---|---|
| 速度非零、游戏里看到走动 | ✅ ucmd 注入通了 |
| 视角跟着变 | ✅ 假客户端视角跟随 `CUserCmd`，可作为朝向来源 |
| 视角不变但速度非零 | 核对 `CUserCmd::viewangles` 与对应反射字段的读取 |
| 全 0、bot 不动 | 先核对存活状态、冻结时间、驱动开关和有效路线，再检查 `RunPlayerMove` |

## 基线 D —— 路点编辑器与寻路

```
agpb_wp_add                     // 在「你」的位置加点
agpb_wp_add 1024 -512 0         // 也可显式给坐标
agpb_wp_nearest
agpb_wp_link 0 1                // 双向连边；flags 1=跳 2=连跳 4=仅通视
agpb_wp_list 10
agpb_wp_save
```

期望：`waypoint N added ...` / `link 0 <-> 1 ...` / `wp_save ok: saved M point(s), K link(s) -> addons/AgPB/waypoints/<map>.agpw`。
存盘失败会在控制台给出具体原因（没有 IFileSystem / 没有地图名 / 写文件失败）。

换图后自动重载：

```
agpb_kick all
changelevel cs_office
agpb_wp_list
```

期望：`[AgPB] map=cs_office ... loaded M point(s), K link(s)`；没打过点的图提示
`no waypoint file for this map yet`，是正常的。

寻路（不用起 bot）：

```
agpb_wp_path 5                  // 从离你最近的点走到 5
agpb_wp_path 0 5
agpb_wp_dist 0 5
```

期望：列出完整路径且 `path distance` 明显大于直线距离；不连通时报 `no path ...`。
`PATH_DOUBLE` 的边一律当不通。

## 基线 E —— 菜单 / 绘制

```
agpb_menu
agpb_wp_show 1
agpb_wp_legend
agpb_wp_thick 4
agpb_wp_xray 0
agpb_wp_flag t
agpb_mode zombie          // 菜单头应显示"模式 zombie"；切回：agpb_mode normal
```

期望：

- 屏幕左上角出现**中文**数字菜单，`0` 退出；按数字键有英文控制台回显。
- 点按标志变色（`agpb_wp_flag t` 后上半截变红），最近点带信息文字，连线可辨类型。
- 菜单不会自己消失（每 3 秒续命）；弹不出菜单时控制台出现同样的文本菜单也算通过。
- `agpb_mode` 只验证开关与显示（菜单头"模式 xxx"）；行为范围与实现状态见 PROGRESS。

## 基线 F —— bot 走路（最小导航）

准备（同队）：真人是 CT 时用 `agpb_add 3`，真人是 T 时用 `agpb_add 2`；
`agpb_list` 确认 hp / 坐标正常，且真人 slot 更小、处于 STATE_ACTIVE。

```
// 站在 A 点
agpb_wp_type normal
// 走到 B 点
agpb_wp_type normal
agpb_wp_cache
// 人站回 A 点附近
agpb_wp_connect both           // 手工确认 A/B 连通，终点使用刚缓存的 B
agpb_bot_goto 0
```

期望：

- 控制台先打印路线（`walking to #1, 2 hop(s): 0 1`）；
- bot 转身朝 B、以 ~250 的速度走过去；
- 到达后打印 `arrived at waypoint #1` 并停住；
- 无进展时打印 `stuck at ...` 并停下，是基线退出保护；验收遇阻恢复时使用下文 M3b 标准。
  卡住后先记录实际位置、目标点 / hop、障碍检测和输入状态，再区分路点几何、临时阻挡与
  移动 / 到达控制问题，不能仅凭 `stuck` 就归因于路点数据。

跳跃：在起跳点用 `agpb_wp_connect jump` 连到落点，再 `agpb_bot_goto 0`。
期望是**站立跳**（57 高度）起跳；空中蹲由引擎自动完成，bot 不该在跳跃边按蹲。

蹲行：把 B 打在矮通道里、`agpb_wp_flag crouch`（蹲点默认半径 0，可用 `agpb_wp_radius` 核对），
再 `agpb_bot_goto 0`：期望 bot 蹲着走进去、速度掉到 ~85、离开矮区前保持蹲姿。

蹲守：把 B 打上 `camp` 标志，再 `agpb_bot_goto 0`。期望到达后控制台打印
`camping at #N for X s`，之后 8~20 秒原地不动（点同时带 `crouch` 的话保持蹲姿）；
结束后如果漫游还开着，会自动挑下一个点。

### 路径障碍识别样本（2026-10-14，cs_office 桌面 / 真人）

每个场景开始前 `agpb_kick all`，重新加入一个同队 bot；真人保持 ACTIVE。
长时间观察时临时 `mp_autokick 0`，可用活着的真人 noclip 镜头 / 第三人称观察；
仅在兼容补丁 `active=1 intact=1` 时允许切观察者。启用诊断后走已有路线：

```
agpb_obstacle_debug 1
agpb_bot_goto 0 14
agpb_bot_obstacle 0 -1 1       // 行走中：沿当前路线，扩大近处检测
```

- 真人挡路：回显 `kind=player`，实体下标应对应真人；人移开后候选应变化或清空。
- 桌面路线 `51→52→53→60→61`：复现卡住后，先检查自动 `obstacle:` 回显；
  也可用 `agpb_bot_obstacle 0 60 1` 从当前位置直扫 #60（单段诊断，不代表完整寻路）。
  记录实体下标、`class` / `serverclass`、碰撞组、`probe`、`hit` / `center` 和 `visible`，
  用 `agpb_netlist ent <实体下标>` 查看它实际拥有的网络字段。
- 历史样本范围：2026-10-14 的 cs_office 桌面卡点候选是收音机 `prop_physics_multiplayer`
  （该次 ent=713，碰撞组 17）；不可把实体下标作为常量，每次开图应以诊断输出为准。证据见 PROGRESS。
- 世界墙体 / 静态模型应报 `world-or-static`；墙后道具不应成为当前候选。
- `breakable-candidate=yes` 只表示类别 / 已知属性符合候选筛选；`health=unknown` 或
  `damage=unknown` 不表示一定可破坏。生命值可通过公开只读 key 查询补读；实际破坏须由射击确认。
- 结束后 `agpb_obstacle_debug 0`、`agpb_kick all`。加载冒烟和上述实机结果写 PROGRESS。

### 自主破坏与续路样本（2026-10-14，cs_office）

- 每轮先 `agpb_kick all`，重载 `cs_office` 恢复原始道具；真人先进入 CT，确认 slot 1 / ACTIVE / hp=100，再 `agpb_add 3`。
- 用 `agpb_bot_goto 0 51` 自主走到起点，完成后只下达一次 `agpb_bot_goto 0 61`。
  路线应为 `51 52 53 60 61`，使用活着的同队真人在外侧或第三人称观察；
  切观察者须先确认兼容补丁 `active=1 intact=1`。
- 复测这组历史样本时，连续三轮核对 `breaking obstacle`、`obstacle shot confirmed`（弹药减少）、
  障碍破坏 / 实体消失、`resuming goal #61`、`arrived at waypoint #61`；以 TGA 截图核对动作。
  人工开火、删除障碍或重新下达路线不能计入成功。
- 真人挡路或挡住枪口时不开火；未知受伤属性的候选可有限试射，连续六枪无伤害确认应退出，
  10 秒超时也应退出。停止 / 死亡 / 换队 / 换图不能留下攻击输入。
- 空弹夹有备弹时由 bot 自行换弹；障碍处理保留蹲姿，起跳途中不打断原来的跳跃输入。
- 历史样本范围：2026-10-14 的 cs_office `51→61` 三轮路线、空弹夹 / 备弹和有限退出测试；
  实际结果、当次 DLL 与后续双向蹲行的边界见 PROGRESS，不能以单次清障代表完整导航链路通过。

#### 代表物品 / 武器样本（2026-10-14）

- 验证以通用障碍处理功能为单位，武器 / 物品只是代表样本，不要求先穷举组合再完成逻辑移植。
  最新实现与验证状态以 PROGRESS「障碍处理移植范围与验证状态」为准；未验证不等于未实现。
- sv_cheats 测试夹具 `agpb_bot_testweapon 0 weapon_m4a1 only` / `weapon_usp only` /
  `weapon_knife only` 必须在 bot 活着且没有路线时使用；武器准备完成后只给一次验收路线。
- USP、M4A1、刀与原始收音机的当次实测结果见 PROGRESS；其他物品可临时将地图原物品
  移入已存在的手工路线验证，不能改生命值或由真人代打。换场景先踢 bot、重载地图复原。
- 显示器 / 机箱实测见 PROGRESS；不能把“周围有可碎物”当作它们都挡住了当前路线。
- 持刀且拥有有弹枪械，远处取得目标时应通过 CUserCmd 切枪；近处可保留刀挥砍。
  取得目标的远近分类不能被活动目标的扩大几何复查改写。
- 近战按 SDK 40-unit ray 和未命中时的 hull fallback 做检查，必须首先命中目标；
  冷却推进只证明挥刀，生命值减少 / 破坏才证明伤害。六次无进展或 10 秒应退出。
- `agpb_obstacle_entity <targetname>` 只读查询位置与模型索引，可核对测试夹具；原生 setang 后
  应 wait 20 再使用 picker / ent_teleport，眼睛不能放进天花板。全实体枚举诊断限制见 PROGRESS，本组使用单实体查询。

## 基线 G —— 几何判定 / 体积模式

> 本节的 `sv_cs_use_legacy_viewvectors` **只有 csczs（当前项目）有**；css / csgo 没有这个开关，
> 体积规格编译期固定（见 PROGRESS「游戏差异登记」），跳过这两条 cvar 命令。

```
agpb_wp_hullmode 0          // 0=自动
agpb_wp_show 1
sv_cs_use_legacy_viewvectors 1   // 老 CS:S：站立 62 / 蹲姿 45
sv_cs_use_legacy_viewvectors 0   // CS:GO 风格：站立 72 / 蹲姿 54
```

期望：切换体积模式后，点的竖线高度和 `agpb_wp_reach` 的可达性跟着变。
想固定用 `agpb_wp_hullmode 1`（CSS）或 `2`（CS:GO）。

```
agpb_wp_reach        // 站在 A 点、准星指 B
agpb_wp_check
```

期望：`reach` 输出 `-> #N: dist ..., reachable=YES/NO, must-jump=YES/NO` 和边是否存在；
`check` 额外给出「像要跳但没打标志」和「打了多余的跳标志」的边的数量。
**只提示、不改数据** —— 跳跃标志一律手工连。

## 主线阶段验收标准

以下是开发完成后应达到的标准，不表示已实现或已验收。当前重点、先后依赖与阶段状态统一见
[`PROGRESS.md`](PROGRESS.md)；M4 外部 agent、M5 LLM 及僵尸模式的后续范围也在那里维护。

### M3a —— 基础导航控制

- 在已手工确认的路线验证普通行走、转弯、跳跃、蹲行与停止 / 到达；精确点附近重复测试，
  记录末段距离和速度，不能因“离目标很近”跳过仍在卡住的问题。
- 到达与主动停止后停止移动并保持当时朝向，不遗留攻击或跳跃输入。
- 冻结期间保留原目标与路线，不因无法移动误判卡住；解冻后自动继续原路线。
- 在满足测试前置的场景用 `agpb_bot_team` 确认实际队伍变化；`agpb_team` 只用于设置默认队伍。
  主动停止、死亡、换队、换图后核对移动与攻击状态清理，不能用原生测试入口替代该命令回归。
- 跳跃由 AgPB 决定起跳并发送 `IN_JUMP`，游戏移动层完成自动蹲跳；自动序列中不追加 `IN_DUCK`。
  普通 CROUCH 点与通道蹲伏、必要的起跳前松蹲按 AGENTS §7 验证。
- 无障碍路线的到达与停止行为通过后，作为遇阻恢复和战斗切换的回归基线。

### M3b —— 遇阻恢复与必要清障

- 对真人 / 临时阻挡执行等待或有限避让，阻挡解除后继续原目标与路线；不要求人工重新下达 goto。
- 用脚边低矮物理道具、连续阻挡和双向蹲行作为代表场景；可破坏且实际挡路的候选清除后续路，
  障碍未检出、仅攻击成功或随后仍卡住均不能算整条链路通过。物品与武器不做穷举组合。
- 不可处理或持续无进展时有限退出，保留失败路段、位置与原因供诊断；停止、死亡、换队、换图
  清理攻击状态。手工路点和连边保持不变，几何体检只给提示，跳跃标志由人决定。

### M3c —— 普通模式最小战斗闭环

- 先满足测试前置，再验证敌我判断与可见性、选敌、瞄准 / 攻击、目标丢失后恢复原导航。
- 导航、清障与战斗之间切换时，原目标与路线恢复正确；动作通过 `CUserCmd`，不调用原生 CCSBot AI。
- 用代表场景验证闭环与任务切换；详细武器策略不作为本轮前置。

### M3d —— 梯子、门与地图目标

- 对人工选定并连好的梯子 / 门路段验证相应动作、完成后的续路以及失败时的有限退出。
- 普通模式炸弹 / 人质等目标分别按对应地图验证；只有路点标志、寻路成功或到达目标点，
  不能替代实际目标行为的完成。

## Radio 三方假客户端兼容补丁验收

历史验证范围为 2026-10-14 的目标 DLL；证据、版本与未验证项统一见 PROGRESS。

- 以 `sm_agpb_guard_status` 的 active=1 intact=1 为测试前置；未加载 / 不匹配版本继续遵守同队约束。
- 真人 CT、AgPB 独占 T，冻结结束后服务器持续运行、null-bot 命中数增加；不要以“不崩”单独证明已触发原崩溃路径。
- 覆盖连续回合重启、死亡 / 重生、原生测试入口换队、真人观察者与双方仅 bot、换图后重复场景。
- 检查 bot 保有 FL_FAKECLIENT，普通 SendAudio 仍发送给同队；未以原生 CCSBot 替换 AgPB。
- 踢掉所有假客户端后验证关闭 / 卸载 / 重载，未知 DLL CRC 或签名不匹配必须在写内存前拒绝。
- 空 profile 与有效原生 bot/profile 分支分别验证；离线实际机器码仿真与原生 ZBot 实机语音回归
  分开记录，不以仿真结果替代实机结果。
- 补丁是独立 SourceMod 兼容层，AgPB 行为仍按 CUserCmd。

## 已搁置

netvar 写入验收（原阶段 H）已搁置：写入层降级为开发 / 诊断工具，不再列入常规回归。
工具本身还能用：`agpb_netwrite` / `agpb_bot_vel`，用法见 [`COMMANDS.md`](COMMANDS.md)。
