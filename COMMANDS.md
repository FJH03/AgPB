# 命令与 ConVar

> 控制台输出一律英文（控制台是 GBK）；HUD 菜单是中文。
> 命令用法速查见文末「打点流程 / 跳跃边 / 蹲行点」。

## bot / 调试

监听主机打开 `agpb_menu` 后按 `1` 进入「机器人管理」。页内 `1` 快速添加、`2` 指定添加、`3` 随机踢出、`4` 踢出全部、`5` 按名单分页踢出。
快速添加使用 `agpb_difficulty`，通过人数自动选队。指定添加依次选技能、性格、队伍和该队外观。
队伍页 `1` 选匪方、`2` 选反恐方、`5` 自动选队并随机外观。外观页 `1–4` 选标准外观，`5` 随机；csczs 的民兵 / 俄罗斯特种部队在 `6`。

主菜单 `3`「填满服务器」依次选队伍、难度和性格，按当前空位逐个添加 AgPB。专服也可在服务器控制台用 `agpb_fill`，或用 `agpb_fill normal` 按普通 40–60 填满。每个机器人在所选技能区间内独立抽取技能，外观随机。指定队伍时沿用 SyPB 的规则，将 `mp_limitteams` 和 `mp_autoteambalance` 设为 `0`。

在 `server.cfg` 设置 `agpb_auto_players 24` 可自动补位。默认等首位真人进游戏再添 bot，按真实连接和已有 bot 增减人数，24 槽保留一个可用槽给下一个真人。一次性填充队列完成后，自动补位才继续按目标人数工作。

创建 bot 时，从已配置的姓名表随机取一个未被占用的名字。优先读取 `addons/AgPB/configs/names.cfg`，缺少该文件时读取 `addons/AgPB/configs/defaults/names.cfg`。csczs 的新建 bot 默认使用 `[AgPB]` 战队标签，`agpb_nametag 0` 可关闭。姓名表为空或用尽时采用编号名。

| 命令 | 说明 |
|---|---|
| `agpb_add [team]` | 创建 bot：1=观察者 2=T 3=CT（默认用 `agpb_team`） |
| `agpb_fill [default\|normal] [auto\|t\|ct]` | 一次性排队填满当前空位。默认按 `agpb_difficulty` 并按人数选队。`normal` 为普通 40–60。 |
| `agpb_bot_team <idx> <team>` | 运行时切换队伍 |
| `agpb_list` | 列出所有 bot（队伍 / 血量 / 武器 / 坐标） |
| `agpb_mode_status` | 显示配置值、实际模式、ZR 接口状态和当前地图 |
| `agpb_bot_info [idx\|all]` | 只读生命周期快照，默认全部：身份、地图 / 回合代次、存活 / 队伍、任务 / 路线归属、终点计时、清理原因，以及最近实际提交的 CUserCmd |
| `agpb_bot_clearance [idx\|all]` | 独立只读清障快照，默认全部：完整目标 / 武器句柄、路线owner / goal、破坏任务 / 预算、武器状态、最近实际CUserCmd及累计提交攻击次数 |
| `agpb_kick <idx\|all>` | 移除 bot。`all` 同时取消填充队列并关闭自动补位。 |
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

新建 bot 默认开启自主任务。普通模式下按地图目标选择炸弹安放 / 拆除 / 拾取 / 逃离、人质带领 / 护送，或按性格、攻守职责和局势在目标、防守、巡逻与驻守之间选择。普通选点避开当前点和最近到访点，并考虑同队玩家及 bot 的目标占用。ZM 自主任务见下节。ZE 自主任务待实现。以下命令可以核对当前决策与地图观察。

| 命令 | 说明 |
|---|---|
| `agpb_bot_auto <idx\|all> [0\|1]` | 切换自主任务；不写第二个参数等于 `1`。切换时清当前任务和路线，关闭手动漫游；`0` 后 bot 待命，`1` 后重新规划 |
| `agpb_bot_tasks [idx\|all]` | 只读列出 bot 的 `autonomy`、地图类型 / 炸弹状态、当前路线所属任务，以及每个任务的 active / suspended、路点、优先级、截止时间、标识和目标实体；不带参数看全部 |
| `agpb_objectives` | 只读列出地图目标快照：爆破点 / 救援区 / 人质数量、炸弹状态与位置、每个人质的存活 / 获救 / 跟随状态；显示 `unknown` 时不能当成目标已完成 |

本地任务按需求优先级中断：可恢复任务保留为 `suspended`，较高需求任务完成后恢复原目标；有效高层意图另行仲裁目标控制权。
清障期间原路线与下一跳也保留。任务的截止时间是游戏时间，挂起时仍会到期。
普通目标的远距离移动、逃爆和巡逻依赖**现有、手工连通**的 `.agpw` 路点图，
不会自动加点或连边。没有爆破图路点时，只能检查 bot 已在目标附近的局部交互：
携带 C4 且已进入爆破区时安放、靠近已安放 C4 且剩余时间足够时拆除、
靠近掉落 C4 时拾取；需要接近目标时，起点须在地面且短段可行走体积检查通过。
此时不能据此验收跨图寻路或逃爆。

## ZM 本地行为

`agpb_mode_status` 核对地图、实际模式和 ZR 接口。ZM 的人类／僵尸身份由 ZR 桥同步。身份未确认时，自主 bot 暂停选点和交战。人类 bot 优先选安全且有队友接应的空闲 CAMP，其次选其他安全 CAMP，再按路程和所在区域的拥挤度考虑其余空闲 CAMP。每个 CAMP 同时分给一只 bot。到点观察，CAMP + CROUCH 按作者标记蹲守。

可达的僵尸逼近时先沿安全路线撤退，走不通时就近拉开。逃生终点是临时停靠点。到达后重新选 CAMP，原点附近的威胁消失后也可重新选择。临时停靠时继续等 CAMP，受到新的威胁时照常撤退。ZR 确认为人类的自主 bot 按现有武器偏好、个人余额及原生购买时间和购买区购枪。`agpb_bot_buy_info` 可核对实际购买回执。

僵尸从可见敌人中优先选路点距离较短的目标，近距离直追并挥刀。自身没有有效敌情时采纳队友近期实际目击的坐标。目标暂时消失后继续走向已经确认的位置，目标明显移动并使当前终点落后时重算路线。按目标身份和位置记录已搜索的线索，收到目标移位的新目击后可以继续追。线索耗尽时按 CS-EBOT 规则抽取巡逻终点。存在可达的手工 T 巡逻点时，有三分之二的概率从该类点选，三分之一从全图选。没有可用 T 点时从全图可达终点选。梯子和跳边由现有导航完成，新敌情可在安全路段接管巡逻。

ZR Grenade Modes 在每颗 bot 手雷生成时，从该类雷支持的普通、碰撞、感应和绊线模式随机选择。抽到绊线时，从 bot 附近沿八个水平方向找 400 单位内可附着的墙，把弹体放到墙面后按现有流程布设激光。找不到合适的墙时，从其他模式重抽。真人可用右键切换自己的模式。ZR 插件负责模式及效果，AgPB 负责原生投掷。

`agpb_bot_tasks` 的 `zm` 行回显身份、目标、守点、撤退与追击状态。`pursuit-userid` 是当前追击路线绑定的目标，`sight-userid` 是最近亲眼看到的目标。`agpb_combat` 和 `agpb_bot_combat` 控制武器战斗，`agpb_bot_info` 回显实际提交的操作。ZE 的撤离阶段、等待区和载具逻辑待实现。

## 普通模式战斗与难度

`agpb_combat` 默认 `1`。已实现的本地战斗以 SyPB 普通模式机制为基线，EBot 作实现对照：
从实际拥有的枪械中按 SyPB 固定武器表和弹药选择，通过 CUserCmd 切枪并等待实际装备确认。交战优先用装弹武器，缺弹时优先找有备弹的主枪。副枪有弹时跳过喷子／狙击的距离过滤，否则使用 SyPB 的 750／300 单位阈值。安全时按主枪、副枪顺序给低于八成的弹匣补弹。高技能 bot 可在开局跑位和合适的近身战主动拔刀，部分手工跳边也会提前切刀。梯子、清障及地图目标任务优先执行。
地面战斗按武器与局势进退、横移或停步瞄准。射击与走位决定最多按 15 Hz 更新，决定之间把攻击按钮与走位提案锁存到每个原生 tick 的 CUserCmd；目标身份、实际指向、玩家遮挡、弹药、姿态与几何安全仍逐 tick 复核，必要时立即停火或停止移动。

普通模式下，各难度 bot 在敌人进入视野后都可主动以步速横移并开火，步枪距离小于 768 单位，手枪距离小于 544 单位。近身后撤、受压退避、换弹、持位和路点要求的蹲姿优先。横移方向仍按 0.5–2.5 秒重选，受阻时复用已有的安全换边。在 256 单位及以上，每段横移结束前的 0.30 秒释放移动，实际水平速度降到现有稳枪阈值后才放行这段停步中的开火。狙击枪继续沿用停稳射击逻辑。
普通 Fight 执行沿用 SyPB 调用顺序：先计算开火前的移动，再执行射击；只有 `skill>=60` 额外按本帧攻击状态重算移动。因此低技能、足够血量 / 进攻倾向的步枪 bot 可以移动开火，高技能会执行相应停枪 / 停步选择；CAMP 等有效持位任务仍保持位置。换弹清可选蹲伏计时，保留必要蹲姿与近距本帧请求的蹲姿。

对枪时，低血或低进攻性后撤使用 bot 自己的视野判断，敌人朝向用于选择横移方向。若斜移受阻，先检查另一侧斜移和两侧纯横移，再尝试保留原进退方向。交火中受伤会立即更新 bot 的进攻性。有近期目击的普通模式自主 bot 可为换弹进入 `seek_cover`。C4 尚未安装时，生命不超过 30 或对眼前敌人的进攻值低于 20 也可触发找掩体。它沿手工路点去能比敌人先到且有实体遮挡的位置，途中有视线且武器可用时仍可射击。进掩体停稳且无需换弹时，可根据近期目击安全投雷。拔环前会复查敌人是否仍被遮挡，投掷期间保留路点要求的蹲姿。短停、投雷、换弹或超时后恢复原任务。紧急目标和原有持位任务保留优先权。
射击采用 SyPB 的 `primaryFireHold` 分类：hold 类持续请求 `IN_ATTACK`，穿越原生武器冷却；非 hold 类按武器基础延时和 skill 随机延时决定下一次请求。实际射速仍由原生武器控制，按钮保持不等于每 tick 都发射。近距 `<256` 不增加这段 AI 延时，也绕过距离后坐力暂停；非 hold 的近距请求仍需释放上一攻击按钮。
购买按 SyPB 的性格偏好选枪。主攻击、Glock／FAMAS 三连发、开镜和消音器切换已接入。csczs 与 CS:S 的普通模式和 ZM 人类共用 HE、闪光和烟雾投掷。bot 根据近期目击选择库存中的手雷，检查原生飞行段、预估生效区域及队友距离。通过 CUserCmd 切雷、瞄准、拔环和松手，弹体由游戏生成。紧急目标、清障、梯子和跳边优先。`agpb_bot_weapon_info` 回显投掷阶段和引擎确认次数。
Glock 按 SyPB 原码在非连发且距离低于 300 时开启，已开启且距离达到 30 时关闭。FAMAS 在超过 400 时开启，400 及以内恢复自动。游戏处理每次连发，`agpb_bot_combat_info` 可读回活动武器的模式和剩余连发数。
狙击枪按 SyPB 距离判断开镜。150 单位及以内收镜，超过 150 开一档，超过 1500 继续开二档。已在二档时，距离大于 150 且不超过 1500 会维持当前镜位。AUG 和 SG552 在 `skill < 90` 且距离超过 800 时主动开镜，800 及以内可收镜。确认近敌后可在主射击条件未满足时收镜。普通模式和 ZM 人类 bot 脱战约一秒后，在可用的指令帧按原生镜档逐步收镜。狙击枪换弹或拉栓期间若由引擎暂时退镜，等回镜后继续检查。
USP 和 M4A1 按 ZBot 规则装卸消音器。新建 bot 时固定抽取一次 70% 偏好，`skill > 70` 的 bot 固定偏好消音。安全空档且距上次目视敌人至少 3.5 秒时才处理，换弹、切枪和交战有优先权。
自主 bot 在回合有效购买时间内按阵营、余额和所持装备购买。开局手枪局会按预算和偏好升级手枪，随后队伍经济不足时进入省钱模式。实际物品和扣款由原生购买处理。
失去视线时保留最后观测位置与时间，并释放攻击；本地可据近期目视或真实开火 / 脚步事件发起有期限的局部调查，沿已有手工图巡查，线索耗尽后续原任务。调查不刷新遮挡后敌人的坐标，不覆盖有效高层意图；空中和梯子阶段不接管导航。战斗保留原任务 / 路线归属；
逃爆及正在安放 / 拆除等优先交互仍由本地核心处理。开启战斗不会解除手动 stop。

| 命令 | 说明 |
|---|---|
| `agpb_bot_combat <idx\|all> [0\|1]` | 切换指定 bot 的本地战斗；省略开关等于 `1`，还须全局 `agpb_combat=1` |
| `agpb_bot_combat_info [idx\|all]` | 只读目标完整句柄 / userid、武器、瞄准、镜位、实际攻击命令 / 弹药消耗、任务与路线归属，以及 difficulty / skill / 反应和后坐力状态 |
| `agpb_bot_weapon_info [idx\|all]` | 只读列出实际拥有武器的完整句柄、弹匣 / 备弹、固定切枪顺序、当前装备与消音器状态，并回显等待确认、冷却 / 重试与累计切枪命令数 |
| `agpb_bot_buy_info [idx\|all]` | 只读查询购买阶段、账户余额、队伍经济和引擎确认的购买结果 |
| `agpb_bot_difficulty <idx\|all> <0..4>` | 给已有 bot 设置 SyPB 难度并在该档范围内重新选择 skill；清当前战斗目标，不改自主开关 |

五档预设的 skill 范围为 `0=1–29`、`1=30–49`、`2=50–79`、`3=80–99`、`4=100`；
内部技能表有六行，按 `skill / 20` 选择反应与后坐力参数；六段后坐力阈值依次为 `50 / 40 / 35 / 30 / 23 / 20`。远距射击依据实际 punch 与距离计算是否超阈值，Godlike（skill 100）对应阈值 20、后坐力暂停 0.4 游戏秒；不是固定打 N 发就暂停。`agpb_difficulty` 默认 `3`，`-1` 表示新建 bot 各自在 1–100 内抽取技能。
瞄准同时区分反应、头部选择、低技能误差与弹道暂停；SyPB 的直接对准条件是已产生开火意图且
`skill >= 70`（狙击武器也有该分支），因此也覆盖档位 2 的高 skill 和档位 3，并非最高档独有。

普通巡逻到点后继续选择新目标；驻守只在有效 CAMP / SNIPER 点，或符合防守局势、占用和冷却条件的目标区执行。驻守会每 2–5 秒选择当前可通视的既有路点方向观察，不改变点、边或跳跃标志。`agpb_bot_tasks` 回显人格、战术、选点、观察点和控制层；`agpb_bot_combat_info` 同时回显目视 / 听觉线索身份、快照、期限及调查进度。

普通模式按 SyPB 规则约每 2.5 秒搜枪，在 400 单位内寻找最近的可见、优于同槽已有武器的枪，使用相同的性格偏好表。走近时准星转向仍可见的目标枪，移动继续沿原路线。靠近后原生丢弃旧枪并走上去拾取，实际入包后再装备。可在 `agpb_bot_tasks` 查看 `pickup_weapon` 任务。紧急地图目标和交战有优先权。

CAMP 任务保持位置，路点的 CROUCH 标志决定是否持续下蹲；CAMP 单独存在时可以站立驻守。按 SyPB，CAMP + CROUCH 在到达并进入驻守后才施加蹲姿，接近、途经或离开时不单凭该组合蹲行；普通 CROUCH 点继续要求蹲着通行，实际低矮空间仍由体积检查与原生蹲起约束处理。交战、失敌释放和切枪不会解除仍有效的任务姿态；Pause 与已到位的 agent camp 复用该规则。驻守 deadline 使用游戏时间，不因交战延长；到期、stop、死亡或冻结后不再施加原任务的蹲姿要求。

`agpb_bot_combat_info` 的 `combat movement` 行回显 `sypb_ground_v1` 策略、进退 / 横移 / 停留意图、三维距离、面向敌人的期望移动、几何约束命中及实际速度；最终提交输入仍以 `agpb_bot_info` 的 `move/view/buttons` 为准。`braking` 表示精确射击等待降速，`posture-settling` 表示蹲起视点过渡期间停火；这些状态不代表已实际开火或伤害。

`agpb_strafe_log -1` 取样所有 bot，正数按客户端 slot 取样，`0` 关闭。`strafe sample` 行的 `side` 依次记录策略、几何检查后和最终提交的敌人相对侧移，`delta-side` 是该次原生移动的侧向位移，`shots` 是同一武器在该指令上的弹药消耗。`phase` 和 `filter` 回显停步、距离、视野、几何回退等条件。
`combat fire` 行回显 `sypb_primary_v1` 的 `state`、hold 分类、下一 AI 射击期限 `next-ai`、决定编号 / 期限 `decision` / `decision-next`，以及 `holding-position`、`task-crouch` 和 `sniper-stop`；真实开火与伤害仍以原生事件和血量变化确认。

普通模式和 ZM 共用地面玩家避让，ZM 以 ZR 身份识别队友。见敌后保留 SyPB 的 48／56 单位预判，迎面或横穿的队友也可提前通过向前斜让避开。实际阻挡触发侧移或退步，碰撞关闭的玩家不参与让路。机器人让真人和原地持位的队友先行，两台 AgPB 对顶时按客户端编号决定谁等、谁让。绕行动作须通过玩家体积、墙、台阶和地面支撑检查。堵路持续时沿已有手工图重算到原目标，只接用能绕开当前一段的路线。bot 到梯口直接尝试挂梯，持续未推进时按梯子超时处理。`agpb_bot_info` 回显地面阻挡者、是否正在避让以及重规划次数。

## 可选 agent 意图

本地 SyPB / EBot 血统核心默认自主运行；已提供原生控制台意图接口及确定性 Python 客户端，尚未接入 LLM 战术层。
外部调用者只给路点目标，由现有任务仲裁、手工图导航和 CUserCmd 执行移动 / 瞄准 / 按键。

| 命令 | 说明 |
|---|---|
| `agpb_agent_combat_override 0\|1` | 默认 `0`。设为 `1` 后允许外部提交和续期 `move` / `camp`。切回 `0` 立即释放外部意图及其所属任务，交回本地核心。 |
| `agpb_agent_state` | 只读当前地图代次 `mapserial`、回合代次 `round`、实际 bot `userid`、租约及本地任务状态。scope 行回显 `combat_override` 和 `chat_override`。 |
| `agpb_agent_intent <mapserial> <round> <userid> move\|camp <wp> <ttl>` | 在当前地图 / 回合和该 bot 身份下提交目标；TTL 为有限的 `0.5..30` 游戏秒，仅接受普通模式、存活且 `auto=1` 的 bot |
| `agpb_agent_intent <mapserial> <round> <userid> clear` | 释放该 bot 的意图及其拥有的路线 / 清障子任务，交回本地任务 |

关闭外部战斗干预时，状态查询和意图 `clear` 仍可用。提交目标前在服务器设置 `agpb_agent_combat_override 1`，再查询当前 scope 和实际 userid。
有效意图拥有最高层目标仲裁权，独立本地目标 / 逃爆任务不会静默覆盖它；行为执行仍由核心处理。清障以绑定意图 token 的子任务运行，完成后继续原目标；状态会显示 `clearing`。任务数值 desire 只用于本地任务需求排序，不代表控制权层级。
同目标、同模式续期保留 token 与当前 hop；TTL 在挂起和战斗期间继续计时。
`move` 到达即释放，`camp` 到达后保持至租约到期；过期、失败、清除或生命周期变化后自动回到本地核心，
释放只清理该意图拥有的路线及执行子任务，随后回退本地自治。没有后续续期时按 TTL 释放；接口不改变自主开关，
手动 `agpb_bot_stop` 后仍保持停止，须用户显式重新开启 auto。

`tools/agpb_agent.py` 向已运行的原生控制台会话提交有限时长的目标并续期；须显式 `--run`，
使用实际 `--userid`、`--mode move|camp`、`--waypoint`、`--session-name` 和 `--session-directory`。
默认续期 10 墙钟秒、TTL 为 3 游戏秒；客户端不打开 auto、不接管既有租约，退出后停止续期，
保留最后期限供本地核心自动回退。原始回执保留在它打印的系统 TEMP 目录。

## 聊天与 Radio

| 命令 | 说明 |
|---|---|
| `agpb_chat 0\|1` | 默认 `1`。启用自主 bot 的关键词回复、低频公开闲聊及入队、击杀、死亡和安放 C4 话术。回复沿用原消息的公开或队伍频道及生死状态。 |
| `agpb_radio 0\|1` | 默认 `1`。启用同队电台播报和命令。用原生电台菜单发送跟随、掩护、持位、支援、朝前推进、后撤或汇报。条件满足时由本地任务执行；高层目标、紧急地图任务和当前交战优先。 |
| `agpb_agent_chat_override 0\|1` | 默认 `0`，直接使用本地话术。启用后给外部 harness 最多 2.5 游戏秒决策，到期回退本地话术。切回 `0` 清除外部决策，仍有效的回复按原有本地节奏处理，无需等外部超时。 |
| `agpb_agent_chat_events` | 在服务器控制台轮询尚待外部决策的聊天事件。事件包含 `id`、`map`、`round`、`userid`、`bot`、`botuserid`、队伍与频道、`ttl`、`cue` 和 UTF-8 `text_hex`。 |
| `agpb_agent_chat_reply <id> <map> <round> <userid> <botuserid> <text_hex\|->` | 接受与最新事件身份匹配的限时回复。正文为最多 120 字节的 UTF-8 十六进制，`-` 明确取消本次发言。重复、过期或身份不匹配的提交会拒绝。 |

在普通模式，抵达守点、首次发现敌人、低血量遇敌及击杀会触发有冷却的电台呼叫。csczs 与 CS:S 通过原生电台命令播放语音，csczs 由引擎按角色模型选择音频，CS:S 由引擎播放原版音频。普通模式使用原生队伍投递及 `ignorerad`。跟随最多两名 bot，持位由下达过跟随命令的队友继续下达，逃离现场和重新集合复用炸弹任务。 [Radio 兼容约束](CRASH_REPORT.md)

普通、ZM 和 ZE 中，回复真人电台要等 2–3 游戏秒。任务执行和 bot 主动呼叫沿用原有节奏。

在监听服设置 `agpb_agent_chat_override 1` 后，使用 `tools/agpb_agent.py --run --chat-only --session-name <name> --session-directory <path> --chat-provider <executable> [args...]` 连接自定义 LLM 提供程序。提供程序在标准输入接收一行 JSON（`version` 和 `event`），并在标准输出返回 `{"reply":"文本"}`、`{"reply":null}` 或 `{"suppress":true}`。带目标的运行需另行启用 `agpb_agent_combat_override 1`，并可在参数末尾附加同一个 `--chat-provider`。模型或进程超时、退出后，游戏侧按期限继续本地聊天。服务器负责发送消息并校验队伍、频道、地图、回合和玩家身份。

普通模式和 ZM 共用全服闲聊节奏。每隔 18–30 游戏秒尝试选一名存活、无交战的 bot 公开发一条闲聊，需有存活真人在线。未命中关键词的真人消息有 35% 概率收到一条通用回复。

ZM 双方 bot 首次确认发现对手时都能报告敌情，人类 bot 到达守点或开始撤退也会回报。ZM 与 ZE 的 bot 电台按 ZR 身份投递原生输出给同阵营存活真人，电台同时限制单 bot、同阵营和同一句的频率。两种感染模式都可用 `Report In, Team` 请求同阵营状态报告。ZM 存活队聊回复继续按 ZR 身份投递。

## 路点：基础操作

| 命令 | 说明 |
|---|---|
| `agpb_wp_add [x y z]` | 加路点；不给坐标就用「你」的位置 |
| `agpb_wp_del <idx>` | 删路点（后面下标前移，连边自动修正） |
| `agpb_wp_link <from> <to> [flags]` | 连边（双向）；flags：1=跳 2=连跳 4=仅通视 |
| `agpb_wp_unlink <from> <to>` | 断开连边 |
| `agpb_wp_jumpvel [from to] [x y z]` | 查询 / 设置现有有向跳边的三维初速度；省略 from/to 时使用最近点 → 准星目标 |
| `agpb_wp_list [max]` | 打印路点与连边 |
| `agpb_wp_nearest` | 离你最近 / 最远的路点下标 |
| `agpb_wp_save` / `agpb_wp_load` | 存盘 / 重读 `addons/AgPB/waypoints/<map>.agpw` |
| `agpb_wp_clear` | 只清内存，不动文件 |
| `agpb_wp_path <to>` ｜ `<from> <to>` | 跑 A* 并打印每一跳（单参数时起点取离你最近的点） |
| `agpb_wp_dist <from> <to>` | 沿路点图的路径代价（梯子 / 蹲点位 ×2） |

## 复用 SyPB 配置

将旧 SyPB 的 `language/en_names.cfg` 等姓名文件复制为游戏目录下的 `addons/AgPB/configs/names.cfg`。使用 UTF-8，每行一个名字，新建 bot 时读取。分发包在 `configs/defaults/` 提供默认姓名表。

旧 SyPB 的 `general.cfg` 可以原样放进 `addons/AgPB/configs/`，插件启动时读取 `PersonalityNormal`、`PersonalityRusher`、`PersonalityCareful`、`GrenadePercent`、`GrenadeMoney`。数字编号和顺序沿用 SyPB。另有同格式的 `weapons.cfg` 覆盖入口，其优先级高于 `general.cfg`。分发包仅部署 `configs/defaults/` 下的默认文件。

## 路点：菜单与编辑器

`agpb_menu` 打开 HUD 菜单（`ShowMenu` usermessage，左上角数字菜单）。按 `2`「功能」→ `2`「路点菜单」进入编辑。
菜单每 3 秒自动续命，通常按 `0` 退出。机器人名单按客户端槽位每页显示八格，`9` 翻页，`0` 回上一页或机器人管理。取不到消息 id 时回落成控制台文本菜单。
菜单里的「起点」= 离你最近的路点（75 单位内），「终点」= 准星指向的点（没有就用缓存点）。

| 菜单 | 键位 |
|---|---|
| 路点操作第 1 页 | `1` 显示 / 隐藏、`2` 缓存、`3` 创建连线、`4` 删除连线、`5` 添加路点、`6` 删除路点、`8` 设置半径、`9` 下一页 |
| 路点操作第 2 页 | `1` 统计、`2` 编辑辅助（传送、连线显示、配色说明）、`3` 设置标志、`5` 直接保存、`6` 加载、`7` 检查、`8` 穿墙、`9` 上一页 |
| 创建连线 | `1` 出向、`2` 入向、`3` 双向、`4` 删除连线、`5` 跳跃、`6` 叠罗汉、`7` 仅通视、`8` 返回、`9` 添加路点 |

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

可破坏桥面可用 `agpb_wp_flag fallcheck` 标记 75 单位内最近的已有桥面点，再用 `agpb_wp_save` 存盘。普通、ZM 和 ZE 的寻路与行进都会检查带标记端点的普通步行边沿线是否还有地面支撑。标出真正踩在可破坏面上的点。两端都未标记的直连边需手工调整，确保跨桥路线经过标记点。桥头安全点可继续用作备用路线的分岔点。跳边、梯子和电梯使用各自的通行规则。

`agpb_wp_show 1` 中的蓝／红标线显示单点有无支撑。用 `agpb_wp_path <from> <to>` 核对当前能选到哪些路。已在走的路线遇到支撑消失时，bot 会停下并尝试按原目标重新寻路。

加点默认到达半径（固定值，自动算已删）：精确类（`AVOID` / `LADDER` / `GOAL` / `RESCUE` / `CROUCH`）= **0**，
`CAMP` = **32**，`JUMP` 起跳点 = **4**，其它（普通 / T / CT）= **64**；要改就 `agpb_wp_radius <n>`。

跳跃初速度沿用 SyPB 的**起跳前世界坐标 XYZ 速度**，单位 units/s，每分量 -2000..2000，默认 `0 0 0` 保留普通跳跃与原有动量。非零向量在该次连接起跳时只注入一次；Z 随后叠加游戏原生的竖直跳跃冲量。起跳视角直接对准向量的水平朝向；零向量或纯竖直向量使用落点方向。

```text
agpb_wp_jumpvel                 // 查询最近点 -> 准星目标的已有跳边
agpb_wp_jumpvel 0 480 60        // 设置这条跳边的世界 XYZ 初速度
agpb_wp_jumpvel 109 110 0 480 60 // 显式指定起点、终点与 XYZ
agpb_wp_save
```

命令仅修改已有 `PATH_JUMP` 方向，不创建连线或补跳标志。`0 0 0` 解除该方向的自定义初速度。保存非零速度时升级为 AGPW v2；旧 v1 自动读取为零，v2 保存不降级。3D 工具也可按目标编号编辑跳边 XYZ。普通 CROUCH 可与跳跃同存，不因已蹲或蹲起过渡拒绝起跳；原生引擎依据实际姿态选择跳跃冲量。当前非零注入仅覆盖静态世界地面支撑；冻结、水中及实体 / 移动支撑等待或按原有限恢复退出。每次起跳按需临时调整当前 bot 的限速字段，提交后立即恢复；全局连跳设置保持原值。设计与验收顺序见[手工弹道跳设计](waypoint-workflow/references/authoring.md#手工弹道跳设计)：先实际看多视角截图和碰撞，再手工调参及逐向实走。

## 最小导航

| 命令 | 说明 |
|---|---|
| `agpb_bot_goto <idx> [wp]` | 切到手动模式并让 bot 沿现有手工路点图的 A* 路线走到目标（不给 wp = 准星指向 / 缓存点）；到达后若目标点带 CAMP 标志会驻守，另有 CROUCH 标志时持续下蹲 |
| `agpb_bot_stop <idx\|all>` | 清任务和路线，关闭自主任务与漫游，停止行走；恢复自主须显式执行 `agpb_bot_auto <idx> 1` |
| `agpb_bot_obstacle <idx> [wp\|-1] [stuck=0\|1]` | **只读诊断**：按 ZBot 思路扫路径体积；默认检查当前路线，显式 wp 则从 bot 当前坐标直扫该点（不寻路）。`stuck=1` 扩大近处检测体积；停止后需显式给 wp。输出实体、碰撞组、位置、可见性与可破坏候选，未知受伤属性报 `unknown` |
| `agpb_bot_roam <idx\|all> [0\|1]` | 切到手动模式并清当前任务 / 路线；`1` 沿现有路点图随机漫游（无路线时选可达点，到达后再选），`0` 停止。会避开别的 bot 的目标点，地面移动共用玩家避碰执行；不加参数 = 开 |
| `agpb_bot_vel <idx> <x> <y> <z>` | **开发用**：下一 tick 直接覆盖 `m_vecVelocity`；不代替正式跳边的 XYZ 初速度执行路径 |
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
M3a / M3b 采样流程见 [导航专项回归](waypoint-workflow/references/regression-checks.md)，战斗专用夹具及流程见 [战斗验证](combat-tests/README.md)。
准备命令要求 `sv_cheats 1`，只用于构造测试条件；bot 的导航、视角和按键仍由核心 CUserCmd 控制。

| 命令 | 说明 |
|---|---|
| `agpb_wp_probe_state` | 只读玩家身份、实际队伍 / 存活、位置 / 速度 / 视角、按钮、真实冻结标志和事件计数 |
| `agpb_wp_probe_bomb` | 只读 C4 状态与真实安放 / 拆除 / 爆炸事件计数 |
| `agpb_wp_probe_lifecycle` | 只读生命周期计数及最近真实事件缓存；`replay=1` 保留原序号，不表示新事件 |
| `agpb_wp_probe_clearance_watch name\|ent value` | 独立clearance probe：按唯一targetname或实体idx锁定已有物件的完整SM EntRef，最多16个；需SDKHooks实际加载成功 |
| `agpb_wp_probe_clearance slot` | 查询所有被观测物件、该玩家真实武器状态、伤害 / 破坏 / 开火计数及64条真实事件缓存 |
| `agpb_wp_probe_clearance_clear` | 仅取消clearance观测与钩子，不删除或改变游戏物件 |
| `agpb_jump_probe_arm <bot-slot> <human-slot> [seconds]` / `agpb_jump_probe_clear` | 只读跳跃探针：绑定同队身份，限时观察真实跳跃、逐tick位置 / 速度 / 限速与落地 |
| `agpb_jump_probe_sweep <bot-slot> sx sy sz tx ty tz` | 只读实际玩家体积扫掠及五层高度射线，打印碰撞位置 / 实体 |
| `agpb_jump_prepare <bot-slot> <human-slot> x y z [yaw]` | 独立准备夹具，需sv_cheats=1、唯一同队AgPB且输入释放；仅把bot放到明确起点并清速度，后续两帧只读回报 |
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

可选三方假客户端兼容层的构建、安装和启停见[兼容补丁部署](CRASH_REPORT.md#部署与启停)。

| 命令 / ConVar | 说明 |
|---|---|
| `sm_agpb_guard_status` | 只读查询补丁 active / intact，以及空 bot、空 profile、有效原生分支的实际命中计数；常规测试要求所有 bot 与真人同队；受控敌对测试另需用户明确授权及 active / intact 均有效 |
| `sm_agpb_fakeclient_guard` | 默认 1，开关当前 DLL 版本的 Radio 兼容补丁；有假客户端连接时拒绝关闭。卸载前先踢所有假客户端，关闭后原始崩溃风险恢复 |

| ConVar | 默认 | 说明 |
|---|---|---|
| `agpb_enable` | 1 | 0 = 不驱动 bot（路点编辑器 / 绘制 / 菜单不受影响） |
| `agpb_team` | 2 | `agpb_add` 不带队伍参数时用的默认队 |
| `agpb_auto_players` | -1 | `-1` 关闭。其他值为总人数目标，计入已连接和仍在连接的客户端，最多补到槽数减一。只增减 AgPB。 |
| `agpb_join_after_player` | 1 | 自动补位等至少一个真人进入游戏才添加 bot。设为 `0` 可空服补位。 |
| `agpb_nametag` | 1 | csczs 新建 bot 是否使用 `[AgPB]` 战队标签 |
| `agpb_mode` | auto | ZR 接口可用时，`ze_` 进 ZE，`zm_` 和其他前缀进 ZM。无 ZR 时进入普通模式。可显式指定 `normal`、`zm` 或 `ze`。旧值 `zombie` 和 `1` 表示强制感染模式并按地图前缀分流 |
| `agpb_combat` | 1 | 普通模式本地战斗总开关；还须对应 bot 的战斗开关开启 |
| `agpb_difficulty` | 3 | 新建 bot 的 SyPB 难度 `0..4`，`-1` 在 1–100 内随机。已有 bot 用 `agpb_bot_difficulty` 修改。 |
| `agpb_combat_debug` | 0 | 输出战斗目标取得 / 释放诊断 |
| `agpb_weapons_debug` | 0 | 输出实际提交的切枪、换弹及次攻击命令，回显任务归属和提交后的实际装备 |
| `agpb_bomb_blast_radius` | 0 | C4最终伤害半径覆盖值（单位）；0按地图初始实体配置读取，并显示来源。若管理员运行期用 `map_setbombradius` 改值，需同步设置为实际伤害半径；用 `map_showbombradius` 核对。不是地图原始damage参数 |
| `agpb_obstacle_debug` | 0 | 1 = 行走时每 0.25 秒输出路径障碍变化；卡住时输出扩大检测结果。只控制额外日志，自主破坏 / 续路不依赖此开关 |
| `agpb_wp_show` | 0 | 绘制路点 |
| `agpb_wp_labels` | 0 | 画下标 |
| `agpb_wp_alllinks` | 1 | 画所有连线 |
| `agpb_wp_thick` | 3 | 线宽（1..5，叠画遍数） |
| `agpb_wp_xray` | 1 | 连线穿墙可见 |
| `agpb_wp_maxjump` | 57 | 几何判定的最大跳跃高度（CS:S 站立跳） |
| `agpb_wp_hullmode` | 0 | 0=自动 / 1=老 CS:S 体积 / 2=CS:GO 风格体积 |
| `agpb_camp_min` / `agpb_camp_max` | 8 / 20 | 到 CAMP 点后驻守的随机时长（游戏秒），不因交战延长 |

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
