# 命令与 ConVar

> 控制台输出一律英文（控制台是 GBK）；HUD 菜单是中文。
> 命令用法速查见文末「打点流程 / 跳跃边 / 蹲行点」。

## bot / 调试

| 命令 | 说明 |
|---|---|
| `agpb_add [team]` | 创建 bot：1=观察者 2=T 3=CT（默认用 `agpb_team`） |
| `agpb_bot_team <idx> <team>` | 运行时切换队伍 |
| `agpb_list` | 列出所有 bot（队伍 / 血量 / 武器 / 坐标） |
| `agpb_kick <idx\|all>` | 移除 bot |
| `agpb_netlist <bot-idx> [filter]` ｜ `agpb_netlist ent <edict-idx> [filter]` | 打印实体 SendTable 字段（名字 / 偏移 / 当前值） |
| `agpb_nethandle <idx> <field>` | 解包 EHANDLE 并解析回实体（entry / serial / class） |
| `agpb_obstacle_entity <targetname>` | **只读诊断**：通过公开 server tools 查询一个命名实体的位置、模型索引、碰撞组、生命值与候选分类 |
| `agpb_ents [class-filter]` | 列出世界实体（edict 下标 / 类名 / 血量 / 坐标） |
| `agpb_netwrite <bot-idx> <field> <value> [element]` ｜ `... ent <edict-idx> ...` | **开发 / 诊断工具**：写网络字段并打印邻字段对照；不作为 bot 的行为手段（[`PROGRESS.md`](PROGRESS.md) 问题 14） |

## 普通模式任务

新建 bot 默认开启自主任务；`agpb_mode normal` 下按地图目标选择炸弹安放 / 拆除 / 拾取 / 逃离、
人质带领 / 护送，或在无目标时沿手工路点巡逻、蹲守。`zombie` 目前只保留模式开关，
不执行这些普通模式自主行为。可用以下命令控制并核对当前决策与地图观察：

| 命令 | 说明 |
|---|---|
| `agpb_bot_auto <idx\|all> [0\|1]` | 切换普通模式自主任务；不写第二个参数等于 `1`。切换时清当前任务和路线，关闭手动漫游；`0` 后 bot 待命，`1` 后重新规划 |
| `agpb_bot_tasks [idx\|all]` | 只读列出 bot 的 `autonomy`、地图类型 / 炸弹状态、当前路线所属任务，以及每个任务的 active / suspended、路点、优先级、截止时间、标识和目标实体；不带参数看全部 |
| `agpb_objectives` | 只读列出地图目标快照：爆破点 / 救援区 / 人质数量、炸弹状态与位置、每个人质的存活 / 获救 / 跟随状态；显示 `unknown` 时不能当成目标已完成 |

任务按优先级中断：可恢复任务保留为 `suspended`，高优先级任务完成后恢复原目标；
清障期间原路线与下一跳也保留。任务的截止时间是游戏时间，挂起时仍会到期。
普通目标的远距离移动、逃爆和巡逻依赖**现有、手工连通**的 `.agpw` 路点图，
不会自动加点或连边。没有爆破图路点时，只能检查 bot 已在目标附近的局部交互：
携带 C4 且已进入爆破区时安放、靠近已安放 C4 且剩余时间足够时拆除、
靠近掉落 C4 时拾取；需要接近目标时，起点须在地面且短段可行走体积检查通过。
此时不能据此验收跨图寻路或逃爆。

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

## ConVar

可选三方假客户端兼容层：先按 [部署说明](compat/sourcemod/README.md) 构建 / 安装 SourceMod 补丁。

| 命令 / ConVar | 说明 |
|---|---|
| `sm_agpb_guard_status` | 只读查询补丁 active / intact，以及空 bot、空 profile、有效原生分支的实际命中计数；本轮测试始终遵守 bot 与真人同队的前置 |
| `sm_agpb_fakeclient_guard` | 默认 1，开关当前 DLL 版本的 Radio 兼容补丁；有假客户端连接时拒绝关闭。卸载前先踢所有假客户端，关闭后原始崩溃风险恢复 |

| ConVar | 默认 | 说明 |
|---|---|---|
| `agpb_enable` | 1 | 0 = 不驱动 bot（路点编辑器 / 绘制 / 菜单不受影响） |
| `agpb_team` | 2 | `agpb_add` 不带队伍参数时用的默认队 |
| `agpb_mode` | normal | 运行期游戏模式：`normal`（原生）/ `zombie`（ZM-ZE，行为未实现）。按图在 cfg 里切 |
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
