# 命令与 ConVar

> 控制台输出一律英文（控制台是 GBK）；HUD 菜单是中文。
> 命令用法速查见文末「打点流程 / 跳跃边 / 蹲行点」。

## bot / 调试

| 命令 | 说明 |
|---|---|
| `agpb_add [team]` | 创建 bot：1=观察者 2=T 3=CT（默认用 `agpb_team`） |
| `agpb_team <idx> <team>` | 运行时切换队伍 |
| `agpb_list` | 列出所有 bot（队伍 / 血量 / 武器 / 坐标） |
| `agpb_kick <idx\|all>` | 移除 bot |
| `agpb_netlist <bot-idx> [filter]` ｜ `agpb_netlist ent <edict-idx> [filter]` | 打印实体 SendTable 字段（名字 / 偏移 / 当前值） |
| `agpb_nethandle <idx> <field>` | 解包 EHANDLE 并解析回实体（entry / serial / class） |
| `agpb_ents [class-filter]` | 列出世界实体（edict 下标 / 类名 / 血量 / 坐标） |
| `agpb_netwrite <bot-idx> <field> <value> [element]` ｜ `... ent <edict-idx> ...` | **开发 / 诊断工具**：写网络字段并打印邻字段对照；不作为 bot 的行为手段（[`PROGRESS.md`](PROGRESS.md) 问题 14） |

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
| `agpb_bot_goto <idx> [wp]` | 让 bot 沿 A* 路线走过去（不给 wp = 准星指向 / 缓存点） |
| `agpb_bot_stop <idx\|all>` | 停止行走 |
| `agpb_bot_roam <idx\|all> [0\|1]` | **随机漫游**：没路线时随机挑一个可达点走过去，到了再挑下一个；会避开别的 bot 的目标点，正前方有队友时按序号错身让路。不加参数 = 开，`0` = 关（`agpb_bot_stop` 也会关） |
| `agpb_bot_vel <idx> <x> <y> <z>` | **开发用**：下一 tick 直接写 `m_vecVelocity`（弹道跳原语） |

## ConVar

| ConVar | 默认 | 说明 |
|---|---|---|
| `agpb_enable` | 1 | 0 = 不驱动 bot（路点编辑器 / 绘制 / 菜单不受影响） |
| `agpb_team` | 2 | `agpb_add` 不带队伍参数时用的默认队 |
| `agpb_mode` | normal | 运行期游戏模式：`normal`（原生）/ `zombie`（ZM-ZE，行为未实现）。按图在 cfg 里切 |
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
