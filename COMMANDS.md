# 命令与 ConVar

> 控制台输出一律英文（控制台是 GBK）；HUD 菜单是中文。
> 命令用法速查见文末「打点流程 / 跳跃边 / 蹲行点」。

## bot / 调试

| 命令 | 说明 |
|---|---|
| `agpb_add [team]` | 创建 bot：1=观察者 2=T 3=CT（默认用 `agpb_team`） |
| `agpb_bot_team <idx> <team>` | 运行时切换队伍 |
| `agpb_list` | 列出所有 bot（队伍 / 血量 / 武器 / 坐标） |
| `agpb_bot_info [idx\|all]` | 只读生命周期快照，默认全部：身份、地图 / 回合代次、存活 / 队伍、任务 / 路线归属、终点计时、清理原因，以及最近实际提交的 CUserCmd |
| `agpb_bot_clearance [idx\|all]` | 独立只读清障快照，默认全部：完整目标 / 武器句柄、路线owner / goal、破坏任务 / 预算、武器状态、最近实际CUserCmd及累计提交攻击次数 |
| `agpb_kick <idx\|all>` | 移除 bot |
| `agpb_netlist <bot-idx> [filter]` ｜ `agpb_netlist ent <edict-idx> [filter]` | 打印实体 SendTable 字段（名字 / 偏移 / 当前值） |
| `agpb_nethandle <idx> <field>` | 解包 EHANDLE 并解析回实体（entry / serial / class） |
| `agpb_obstacle_entity <targetname>` | **只读诊断**：通过公开 server tools 查询一个命名实体的位置、模型索引、碰撞组、生命值与候选分类 |
| `agpb_ents [class-filter]` | 列出世界实体（edict 下标 / 类名 / 血量 / 坐标） |
| `agpb_netwrite <bot-idx> <field> <value> [element]` ｜ `... ent <edict-idx> ...` | **开发 / 诊断工具**：写网络字段并打印邻字段对照；不作为 bot 的行为手段（[`PROGRESS.md`](PROGRESS.md) 问题 14） |

`agpb_bot_info` 的 `cmdnum` / `cmdtick`、`buttons` / `move` / `view` 来自最近实际提交的
CUserCmd，读取时可能早于当前 tick。停止、死亡、换队或回合变化后应跨至少两个 tick 再取样，
并连续核对命令序号递增、输入释放和任务 / 路线状态；一帧快照不能代替生命周期验收。
清障诊断的 `attackcmds` 只统计实际提交含 `IN_ATTACK` 的命令，伤害与破坏另以真实事件和目标状态确认。

## 普通模式任务

新建 bot 默认开启自主任务；`agpb_mode normal` 下按地图目标选择炸弹安放 / 拆除 / 拾取 / 逃离、
人质带领 / 护送，或按性格、攻守职责和局势在目标、防守、巡逻与驻守之间选择。普通选点避开当前点和最近到访点，并考虑同队玩家及 bot 的目标占用；不会固定给 A/B 配额或修改路点。`zombie` 目前只保留模式开关，
不执行这些普通模式自主行为。可用以下命令控制并核对当前决策与地图观察：

| 命令 | 说明 |
|---|---|
| `agpb_bot_auto <idx\|all> [0\|1]` | 切换普通模式自主任务；不写第二个参数等于 `1`。切换时清当前任务和路线，关闭手动漫游；`0` 后 bot 待命，`1` 后重新规划 |
| `agpb_bot_tasks [idx\|all]` | 只读列出 bot 的 `autonomy`、地图类型 / 炸弹状态、当前路线所属任务，以及每个任务的 active / suspended、路点、优先级、截止时间、标识和目标实体；不带参数看全部 |
| `agpb_objectives` | 只读列出地图目标快照：爆破点 / 救援区 / 人质数量、炸弹状态与位置、每个人质的存活 / 获救 / 跟随状态；显示 `unknown` 时不能当成目标已完成 |

本地任务按需求优先级中断：可恢复任务保留为 `suspended`，较高需求任务完成后恢复原目标；有效高层意图另行仲裁目标控制权。
清障期间原路线与下一跳也保留。任务的截止时间是游戏时间，挂起时仍会到期。
普通目标的远距离移动、逃爆和巡逻依赖**现有、手工连通**的 `.agpw` 路点图，
不会自动加点或连边。没有爆破图路点时，只能检查 bot 已在目标附近的局部交互：
携带 C4 且已进入爆破区时安放、靠近已安放 C4 且剩余时间足够时拆除、
靠近掉落 C4 时拾取；需要接近目标时，起点须在地面且短段可行走体积检查通过。
此时不能据此验收跨图寻路或逃爆。

## 普通模式战斗与难度

`agpb_combat` 默认 `1`。本地战斗沿用 SyPB 普通模式机制，EBot 作实现对照：
使用当前持有武器，在地面停步瞄准、按冷却脉冲开火或换弹；每次开火复核实际指向与玩家遮挡。
失去视线时保留最后观测位置与时间，并释放攻击；本地可据近期目视或真实开火 / 脚步事件发起有期限的局部调查，沿已有手工图巡查，线索耗尽后续原任务。调查不刷新遮挡后敌人的坐标，不覆盖有效高层意图；空中和梯子阶段不接管导航。战斗保留原任务 / 路线归属；
逃爆及正在安放 / 拆除等优先交互仍由本地核心处理。开启战斗不会解除手动 stop。

| 命令 | 说明 |
|---|---|
| `agpb_bot_combat <idx\|all> [0\|1]` | 切换指定 bot 的本地战斗；省略开关等于 `1`，还须全局 `agpb_combat=1` |
| `agpb_bot_combat_info [idx\|all]` | 只读目标完整句柄 / userid、武器、瞄准、实际攻击命令 / 弹药消耗、任务与路线归属，以及 difficulty / skill / 反应和后坐力状态 |
| `agpb_bot_difficulty <idx\|all> <0..4>` | 给已有 bot 设置 SyPB 难度并在该档范围内重新选择 skill；清当前战斗目标，不改自主开关 |

五档预设的 skill 范围为 `0=1–29`、`1=30–49`、`2=50–79`、`3=80–99`、`4=100`；
内部技能表有六行，按 `skill / 20` 选择反应与后坐力参数。`agpb_difficulty` 默认 `3`，只影响之后创建的 bot。
瞄准同时区分反应、头部选择、低技能误差与弹道暂停；SyPB 的直接对准条件是已产生开火意图且
`skill >= 70`（狙击武器也有该分支），因此也覆盖档位 2 的高 skill 和档位 3，并非最高档独有。

普通巡逻到点后继续选择新目标；驻守只在有效 CAMP / SNIPER 点，或符合防守局势、占用和冷却条件的目标区执行。驻守会每 2–5 秒选择当前可通视的既有路点方向观察，不改变点、边或跳跃标志。`agpb_bot_tasks` 回显人格、战术、选点、观察点和控制层；`agpb_bot_combat_info` 同时回显目视 / 听觉线索身份、快照、期限及调查进度。

## 可选 agent 意图

本地 SyPB / EBot 血统核心默认自主运行；已提供原生控制台意图接口及确定性 Python 客户端，尚未接入 LLM 战术层。
外部调用者只给路点目标，由现有任务仲裁、手工图导航和 CUserCmd 执行移动 / 瞄准 / 按键。

| 命令 | 说明 |
|---|---|
| `agpb_agent_state` | 只读当前地图代次 `mapserial`、回合代次 `round`、实际 bot `userid`、租约及本地任务状态 |
| `agpb_agent_intent <mapserial> <round> <userid> move\|camp <wp> <ttl>` | 在当前地图 / 回合和该 bot 身份下提交目标；TTL 为有限的 `0.5..30` 游戏秒，仅接受普通模式、存活且 `auto=1` 的 bot |
| `agpb_agent_intent <mapserial> <round> <userid> clear` | 释放该 bot 的意图及其拥有的路线 / 清障子任务，交回本地任务 |

先查询 state 再使用当前 scope 与实际 userid，不用会前移的 bot 列表 idx 代替身份。
有效意图拥有最高层目标仲裁权，独立本地目标 / 逃爆任务不会静默覆盖它；行为执行仍由核心处理。清障以绑定意图 token 的子任务运行，完成后继续原目标；状态会显示 `clearing`。任务数值 desire 只用于本地任务需求排序，不代表控制权层级。
同目标、同模式续期保留 token 与当前 hop；TTL 在挂起和战斗期间继续计时。
`move` 到达即释放，`camp` 到达后保持至租约到期；过期、失败、清除或生命周期变化后自动回到本地核心，
释放只清理该意图拥有的路线及执行子任务，随后回退本地自治。没有后续续期时按 TTL 释放；接口不改变自主开关，
手动 `agpb_bot_stop` 后仍保持停止，须用户显式重新开启 auto。

`tools/agpb_agent.py` 向已运行的原生控制台会话提交有限时长的目标并续期；须显式 `--run`，
使用实际 `--userid`、`--mode move|camp`、`--waypoint`、`--session-name` 和 `--session-directory`。
默认续期 10 墙钟秒、TTL 为 3 游戏秒；客户端不打开 auto、不接管既有租约，退出后停止续期，
保留最后期限供本地核心自动回退。原始回执保留在它打印的系统 TEMP 目录。

## 路点：基础操作

| 命令 | 说明 |
|---|---|
| `agpb_wp_add [x y z]` | 加路点；不给坐标就用「你」的位置 |
| `agpb_wp_del <idx>` | 删路点（后面下标前移，连边自动修正） |
| `agpb_wp_link <from> <to> [flags]` | 连边（双向）；flags：1=跳 2=连跳 4=仅通视 |
| `agpb_wp_unlink <from> <to>` | 断开连边 |
| `agpb_wp_list [max]` | 打印路点与连边 |
| `agpb_wp_nearest` | 离你最近 / 最远的路点下标 |
| `agpb_wp_save` / `agpb_wp_load` | 存盘 / 重读 `addons/AgPB/waypoints/<map>.agpw` |
| `agpb_wp_clear` | 只清内存，不动文件 |
| `agpb_wp_path <to>` ｜ `<from> <to>` | 跑 A* 并打印每一跳（单参数时起点取离你最近的点） |
| `agpb_wp_dist <from> <to>` | 沿路点图的路径代价（梯子 / 蹲点位 ×2） |

## 路点：菜单与编辑器

`agpb_menu` 打开 HUD 菜单（`ShowMenu` usermessage，左上角数字菜单）。
菜单每 3 秒自动续命，不会自己消失；`0` 退出；取不到消息 id 时回落成控制台文本菜单。
菜单里的「起点」= 离你最近的路点（75 单位内），「终点」= 准星指向的点（没有就用缓存点）。

| 命令 | 说明 |
|---|---|
| `agpb_menu [n]` | 打开菜单 / 直接选第 n 项（`menuselect n` 等价） |
| `agpb_wp_show [0\|1]` | 开关路点绘制 |
| `agpb_wp_labels [0\|1]` | 每个点上方画下标 |
| `agpb_wp_alllinks [0\|1]` | 画所有连线 / 只画最近点的连线 |
| `agpb_wp_cache` | 把最近点存成连线目标（EBot 的 Cache waypoint） |
| `agpb_wp_type <type>` | 在你脚下加指定类型的点：`normal` `t` `ct` `avoid` `rescue` `camp` `goal` `jump` `crouch` `ladder` `usebutton` `sniper` `lift` `fallcheck` `fallrisk` |
| `agpb_wp_flag <flag\|clear>` | 切换最近点上的标志（flag 名同上，`clear` 清空） |
| `agpb_wp_radius <0..255>` | 设最近点的到达半径 |
| `agpb_wp_reach [idx]` | 几何体检：你站的位置到目标点是否可达、要不要跳 |
| `agpb_wp_connect <out\|in\|both\|jump\|boost\|visible>` | 连线：起点 = 最近点，终点 = 准星指向的点 |
| `agpb_wp_cut` | 断开「最近点 ↔ 目标点」 |
| `agpb_wp_teleport [idx]` | 传送到路点（需要 `sv_cheats 1`） |
| `agpb_wp_noclip` | 开 / 关 noclip（需要 `sv_cheats 1`） |
| `agpb_wp_check` | 结构 + 几何校验：越界 / 自连 / 孤立点 /「该跳但没打标志」的边 |
| `agpb_wp_stats` | 统计各类点与连线数 |
| `agpb_wp_legend` | 打印配色说明 |

加点默认到达半径（固定值，自动算已删）：精确类（`AVOID` / `LADDER` / `GOAL` / `RESCUE` / `CROUCH`）= **0**，
`CAMP` = **32**，`JUMP` 起跳点 = **4**，其它（普通 / T / CT）= **64**；要改就 `agpb_wp_radius <n>`。

## 最小导航

| 命令 | 说明 |
|---|---|
| `agpb_bot_goto <idx> [wp]` | 切到手动模式并让 bot 沿现有手工路点图的 A* 路线走到目标（不给 wp = 准星指向 / 缓存点）；到达后若目标点带 CAMP 标志会蹲守 |
| `agpb_bot_stop <idx\|all>` | 清任务和路线，关闭自主任务与漫游，停止行走；恢复自主须显式执行 `agpb_bot_auto <idx> 1` |
| `agpb_bot_obstacle <idx> [wp\|-1] [stuck=0\|1]` | **只读诊断**：按 ZBot 思路扫路径体积；默认检查当前路线，显式 wp 则从 bot 当前坐标直扫该点（不寻路）。`stuck=1` 扩大近处检测体积；停止后需显式给 wp。输出实体、碰撞组、位置、可见性与可破坏候选，未知受伤属性报 `unknown` |
| `agpb_bot_roam <idx\|all> [0\|1]` | 切到手动模式并清当前任务 / 路线；`1` 沿现有路点图随机漫游（无路线时选可达点，到达后再选），`0` 停止。会避开别的 bot 的目标点，正前方有队友时按序号错身让路；不加参数 = 开 |
| `agpb_bot_vel <idx> <x> <y> <z>` | **开发用**：下一 tick 直接写 `m_vecVelocity`（弹道跳原语） |
| `agpb_bot_testweapon <idx> <weapon_m4a1\|weapon_usp\|weapon_knife> [only]` | **sv_cheats 测试夹具**：仅活着且无路线的 bot 可用；公开 IBotController 创建 / 配备武器，`only` 先清空物品。仅用于构造武器测试条件，不参与 AI 决策 |

行走时会自动处理可见、符合筛选的可破坏障碍：使用持有枪械瞄准 / 开火 / 换弹，近处持刀可挥刀，
暂停移动但保留原路线，障碍消失或移出路径后自动续路。卡住时也会扩大近处探测。
远处持刀优先换有弹枪械，只有刀则继续接近；近处超出保守近战探测距离时也尝试换枪。
枪口 / 近战射线被玩家或其他物体挡住时不攻击；每个障碍最多处理 10 秒，连续六次攻击没有确认伤害或破坏则停止。
失败会终止被障碍挡住的当前任务 / 路线并输出原因；手动 `goto` / `roam` 停止，
自主模式保留开启并在短暂延迟后重新规划目标。`agpb_obstacle_debug` 只控制额外诊断，
自动破坏不依赖它；`agpb_bot_obstacle` 本身仍是只读查询。生命值可由公开只读 key 查询补读，
未暴露的受伤属性保持 `unknown`，候选判断不等于保证可破坏。

## 可选回归夹具

以下命令由手工加载的 SourceMod 测试夹具提供，不属于 AgPB 发布包。编译、加载与完整
M3a / M3b 采样流程见 [路点工作流](waypoint-workflow/README.md)，战斗专用夹具及流程见 [战斗验证](combat-tests/README.md)。
准备命令要求 `sv_cheats 1`，只用于构造测试条件；bot 的导航、视角和按键仍由核心 CUserCmd 控制。

| 命令 | 说明 |
|---|---|
| `agpb_wp_probe_state` | 只读玩家身份、实际队伍 / 存活、位置 / 速度 / 视角、按钮、真实冻结标志和事件计数 |
| `agpb_wp_probe_bomb` | 只读 C4 状态与真实安放 / 拆除 / 爆炸事件计数 |
| `agpb_wp_probe_lifecycle` | 只读生命周期计数及最近真实事件缓存；`replay=1` 保留原序号，不表示新事件 |
| `agpb_wp_probe_clearance_watch name\|ent value` | 独立clearance probe：按唯一targetname或实体idx锁定已有物件的完整SM EntRef，最多16个；需SDKHooks实际加载成功 |
| `agpb_wp_probe_clearance slot` | 查询所有被观测物件、该玩家真实武器状态、伤害 / 破坏 / 开火计数及64条真实事件缓存 |
| `agpb_wp_probe_clearance_clear` | 仅取消clearance观测与钩子，不删除或改变游戏物件 |
| `agpb_wp_probe_combat_watch subject_bot_slot human_slot [target_slot]` | 独立只读战斗探针：绑定实际身份与事件；主体 bot 与低 slot 真人保持同队，第三参数仅供另获明确授权的受控敌对场景 |
| `agpb_wp_probe_combat` | 读取已绑定玩家 / 武器、真实开火 / 伤害 / 死亡事件与射线观察 |
| `agpb_wp_probe_combat_clear` | 仅取消战斗观测，不改变玩家或游戏设置 |
| `agpb_reg_lifecycle_freeze bot_slot human_slot seconds [after_seconds]` | 同队存活 bot 的有限时 point_viewcontrol；持续 0.5..15 秒，可延迟 0..5 秒，实际冻结须读回确认 |
| `agpb_reg_lifecycle_kill bot_slot human_slot` | 同队 bot 的真实死亡准备；拒绝与点控重叠 |
| `agpb_reg_lifecycle_respawn bot_slot human_slot` | 同队已死 bot 的真实重生准备；拒绝与点控重叠 |
| `agpb_reg_lifecycle_team bot_slot human_slot SPEC\|SAME` | 仅场景准备：先观察者，再加入锚定真人所在队；核心换队回归使用 `agpb_bot_team` |
| `agpb_reg_clearance_arm stop\|death bot_slot human_slot core_idx` | fixture1.2：单个AgPB fake与活真人同队、core_idx=0；首次真实weapon_fire后跨tick记录快照，再标准stop / 真实自杀；10秒未执行则取消 |
| `agpb_reg_clear` | 取消真人输入、生命周期及fire-arm请求，并通过引擎 Disable 恢复存活点控对象；随后须 probe 确认 |

死亡、换队、卸载前先结束点控并确认 `frozen=0`、玩家视角恢复。真实回合切换通过游戏原生
`endround`，以真实事件和后续状态验收。
fire-arm 绑定双方userid / serial，clear、卸载、换图、断线或身份 / 同队条件失效均取消；`armed`不能证明实际中断。
常规测试仍要求所有 bot 与真人同队；战斗探针的绑定不构成敌对授权。正向敌对测试须另获用户单独明确授权，
并持续核对 Radio 兼容补丁 `active=1`、`intact=1`；工作流与离线重算入口见 [战斗验证](combat-tests/README.md)。

## ConVar

原生 ZBot 作为对手的多目标短测另用可选只读 [agpb_native_match_probe.sp](combat-tests/fixtures/agpb_native_match_probe.sp)：
`agpb_match_watch <subject_slot> <human_slot>`、`agpb_match_state`、`agpb_match_clear`。
该观察器要求真人与受测 AgPB 同队，并只观测另外两或三名假客户端；不会创建对手、发输入或修改伤害。
敌队场景仍须用户单独授权并核验匹配的 Radio guard；原生 ZBot 仅作对手，不替代 AgPB 行为核心。

可选三方假客户端兼容层：先按 [部署说明](compat/sourcemod/README.md) 构建 / 安装 SourceMod 补丁。

| 命令 / ConVar | 说明 |
|---|---|
| `sm_agpb_guard_status` | 只读查询补丁 active / intact，以及空 bot、空 profile、有效原生分支的实际命中计数；常规测试要求所有 bot 与真人同队；受控敌对测试另需用户明确授权及 active / intact 均有效 |
| `sm_agpb_fakeclient_guard` | 默认 1，开关当前 DLL 版本的 Radio 兼容补丁；有假客户端连接时拒绝关闭。卸载前先踢所有假客户端，关闭后原始崩溃风险恢复 |

| ConVar | 默认 | 说明 |
|---|---|---|
| `agpb_enable` | 1 | 0 = 不驱动 bot（路点编辑器 / 绘制 / 菜单不受影响） |
| `agpb_team` | 2 | `agpb_add` 不带队伍参数时用的默认队 |
| `agpb_mode` | normal | 运行期游戏模式：`normal`（原生）/ `zombie`（ZM-ZE，行为未实现）。按图在 cfg 里切 |
| `agpb_combat` | 1 | 普通模式本地战斗总开关；还须对应 bot 的战斗开关开启 |
| `agpb_difficulty` | 3 | 新建 bot 的 SyPB 难度 `0..4`；已有 bot 用 `agpb_bot_difficulty` 修改 |
| `agpb_combat_debug` | 0 | 输出战斗目标取得 / 释放诊断 |
| `agpb_bomb_blast_radius` | 0 | C4最终伤害半径覆盖值（单位）；0按地图初始实体配置读取，并显示来源。若管理员运行期用 `map_setbombradius` 改值，需同步设置为实际伤害半径；用 `map_showbombradius` 核对。不是地图原始damage参数 |
| `agpb_obstacle_debug` | 0 | 1 = 行走时每 0.25 秒输出路径障碍变化；卡住时输出扩大检测结果。只控制额外日志，自主破坏 / 续路不依赖此开关 |
| `agpb_wp_show` | 0 | 绘制路点 |
| `agpb_wp_labels` | 0 | 画下标 |
| `agpb_wp_alllinks` | 1 | 画所有连线 |
| `agpb_wp_thick` | 3 | 线宽（1..5，叠画遍数） |
| `agpb_wp_xray` | 1 | 连线穿墙可见 |
| `agpb_wp_maxjump` | 57 | 几何判定的最大跳跃高度（CS:S 站立跳） |
| `agpb_wp_hullmode` | 0 | 0=自动 / 1=老 CS:S 体积 / 2=CS:GO 风格体积 |
| `agpb_camp_min` / `agpb_camp_max` | 8 / 20 | 到 CAMP 点后原地蹲守的随机时长（秒） |

## 用法速查

**打点流程**

```
agpb_wp_type normal      // 站在 A 点打第一个点
// 走到 B 点
agpb_wp_type normal
agpb_wp_cache            // 把 B 存成目标点（也可以直接用准星指）
// 回到 A 点附近；最近点必须是 A，目标点才是 B
agpb_wp_connect both     // 连接最近点 <-> 目标点（双向）
agpb_wp_check
agpb_wp_save
```

**跳跃边**（照 EBot 的 `AddPath(type=1)`）：站在起跳点 → 准星指向落地点 →
`agpb_wp_connect jump`（或菜单「创建连线 → 跳跃连线」）。一条命令做三件事：

- 边打 `PATH_JUMP`，**单向**（起跳点 → 落地点）
- 起跳点打 `JUMP` 标志，半径压到 **4**（先站准再起跳）
- 反向也要跳就站到落地点再连一次

**蹲行点**（CROUCH 的语义是「必须蹲着才能到达」）：放在低矮通道内部、两端各一个；
蹲行点默认半径 0（精确到达），别手动调大，否则会提前算到达。

**绘图配色**

- 点：下半截=基础色（CAMP 青 / GOAL 紫 / LADDER 棕 / RESCUE 白 / AVOID 红 /
  FALLCHECK 灰 / USEBUTTON 蓝 / JUMP 黄 / CROUCH 紫罗兰 / LIFT 墨绿 / 其它绿），
  上半截=附加色（SNIPER 暗金 / T 红 / CT 蓝 / FALLRISK 粉）
- 连线：JUMP 红 / BOOST 蓝 / VISIBLE 通视绿、被挡橙 / 双向黄 / 单向白 / 单向入边墨绿
- 半径=蓝框；缓存点=黄线；准星指向点=白线

### 终点停稳与逃爆诊断

- 普通路线的中间节点仍连续穿行；最终节点用CUserCmd减速/制动，实际落地、水平误差在有效半径内、
  脚底高度误差不超过4、速度不超过5且连续稳定0.8秒才报告arrived。有效半径为max(人工计划radius,4)，
  控制目标另留0.5单位内侧余量，AGPW原始radius不改。跳跃到终点先完成落地，不在空中刹停。
- `agpb_objectives` 新增bomb-radius、radius-source、planted-observed；unknown保持不确定，不能视为安全。
- 普通模式逃爆沿现有手工图选择预计最早可达的安全点，预算包括入图、行走/蹲行/跳跃及余量；
  CT还比较进场和拆弹预算。紧急撤离不受普通目标失败的5秒冷却阻断。
- 逃爆日志有left、goal、entry、path、eta、radius/source与safe；`NO-best-effort`不表示已经安全。
  UNKNOWN观测保留避险任务，确认炸弹已解决或回合变化后才释放。最终成功仍需实际爆炸后存活证据。
- 行为基线为SyPB、EBot作对照；Source引擎仅提供公开接口、运动/伤害规则，不使用ZBot AI控制。
