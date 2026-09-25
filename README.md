# AgPB

Counter-Strike: Source（Source 1 / Win64）的 agent 控制 bot 插件
（插件内部名 / VDF alias：`AgPB`）。

- 不使用 SourceMod
- 不使用引擎自带的 `CCSBot` / `CCSBotManager`
- 由外部 agent（LLM）下发高层意图，插件内的反射层逐 tick 生成 `CUserCmd`

> **状态**：M1 / M2 / M2.5 已实测通过；M3 已走到「能自己打点、能看、能走」
> （无 AI 假客户端 + ucmd 注入驱动玩家 + SendTable 零索引反射层 + EHANDLE 解析
> + 手工路点图 / A* 寻路 + 中文 HUD 菜单编辑器 + EBot 配色的叠加层绘制
> + wayzone 自动半径 + 最小导航 `agpb_bot_goto`）
>
> 文档：[`ARCHIVE.md`](ARCHIVE.md) 完整设计与踩坑 ｜ [`VERIFY.md`](VERIFY.md) 四阶段验证清单
> ｜ 收工交接见 ARCHIVE **§10**（已实测结论 / 数字基线 / 明天从哪开始）

## 为什么能绕开 CCSBot

`server.dll` 通过 `CreateInterface` 暴露了 `"BotManager001"`（实现类 `CPluginBotManager`，
见 `game/server/playerinfomanager.cpp:133`）：

```cpp
edict_t *CPluginBotManager::CreateBot( const char *botname )
{
    edict_t *pEdict = engine->CreateFakeClient( botname );
    ...
    pPlayer->ClearFlags();
    pPlayer->AddFlag( FL_CLIENT | FL_FAKECLIENT );
    pPlayer->ChangeTeam( TEAM_UNASSIGNED );
    pPlayer->RemoveAllItems( true );
    pPlayer->Spawn();
    return pEdict;
}
```

它创建的是一个 **不带任何 AI 的假客户端**，因此引擎自带的 bot 逻辑完全不会介入。
`IBotManager::GetBotController()` 返回 `IBotController`（实现类 `CPlayerInfo`），
其中的 `RunPlayerMove(CBotCmd*)` 正是在 `CBasePlayer::PhysicsSimulate()` 里被引擎自己调用的那条路径：

```cpp
MoveHelperServer()->SetHost( m_pParent );
m_pParent->PlayerRunCommand( &cmd, MoveHelperServer() );
```

好处是：**不需要特征码扫描、不需要硬编码偏移、不需要链接 server.dll**。
插件只依赖公开的虚接口，构建时只要 SDK 头文件。

## 当前阶段

只保留"引擎适配层" + netvar 反射层 + 路点层：

```
引擎事件（GameFrame, 66 Hz）
  └─ CAgPB::Think()
       ├─ TryJoinTeam()                     入队 + 出生（ChangeTeam / joinclass）
       └─ IBotController::RunPlayerMove()   驱动引擎命令链

路点层（M3 第一步，见下）
  └─ CAgPBWaypoints（全局单例 BotWaypoints()）
       ├─ SetMapName()        换图时读 addons/AgPB/waypoints/<map>.agpw
       ├─ FindPath()          A*（沿连边，跳过 AVOID / 阵营不符的点）
       └─ PathDistance()      单源 Dijkstra（按路点图算距离）

反射层（任意时刻可读）
  └─ CNetVarRegistry::GetForEdict(edict)
       └─ ServerClass -> SendTable -> “名字 → 偏移 → 类型”
```

`Think()` 目前**不生成任何有意义的输入**（只有 `agpb_testmove` 这个临时开关会写
`forwardmove` / `viewangles.y`）——移动 / 瞄准 / 战斗将由移植过来的
EBot `control` / `navigate` / `combat` 模块填充。

### 路点系统（M3 第一步）

`.nav` 只描述「哪块地面能站」，「A 到 B 怎么走」要靠几何去推；推错了就会出现
「nav 说连通、物理上过不去」的连接（cs_office 上实测有相邻 area 中心差 85~108 单位、
却没标 `NAV_MESH_JUMP` 的）。所以改用**手工路点图**：每条连边都是人验证过的动作，
不存在推导这一步。

```
addons/AgPB/waypoints/<map>.agpw        # 每张图一个文件（按地图名自动读盘）
```

- 数据模型 1:1 照搬 EBot：每个路点 **定长 8 条连边**（`Const_MaxPathIndex`）、
  位标志 `AgPB_WP_*`（蹲 / 梯子 / 跳 / 连跳 / 阵营专用…）、连边标志 `AgPB_PATH_*`
- 连边**双向**存储（`AddLink` 同时写两端）；**没有自动连线** —— 自动连的边没人验证过
- 寻路照搬 EBot `RunAsyncAStar`，去掉线程 / 异步壳：点只有几百个，帧内同步跑完
- **不建 N×N 距离矩阵**：EBot 那张 `8192² × 2B = 128MB` 的表只是给 A* 当启发式，
  这里用单源 Dijkstra（`ComputeDistances` / `PathDistance`），最短路结果一样

### netvar 反射（M2）

数据全部来自引擎自己构建的 SendTable，整条链路都是编译器解析的成员访问 / 虚调用：

```
edict_t* -> GetNetworkable() -> IServerNetworkable*
         -> GetServerClass() -> ServerClass* -> m_pTable -> SendTable*
         -> m_pProps[i] (SendProp: 名字 / 偏移 / 类型 / 数组元素数 / stride)
```

**零硬编码 vtable 索引，零特征码。** 基类表以 `"baseclass"` + offset 0 挂在派生表里
（`public/dt_send.h:553`），所以统一用 `offset += pProp->GetOffset()` 递归展开。

用法：

```cpp
int hp = pBot->GetNetVarInt( "m_iHealth" );
Vector vel = pBot->GetNetVarVector( "m_vecVelocity" );
int ammo0 = pBot->GetNetVarArrayInt( "m_iAmmo", 0 );
```

> `CBaseEntity` 在公开头文件里只有前置声明，所以实体基址以不透明指针传递，
> 偏移加法在插件侧完成。

## 构建

需要 `E:\Plugins-Platform` 下的目录同处一层：

```
E:\Plugins-Platform\
  hl2sdk-css\            <- CS:S SDK（含 source-engine-czero）
  hl2sdk-manifests\
  metamod-source\
  AgPB\                    <- 本插件
```

```powershell
cd E:\Plugins-Platform\AgPB
mkdir build
cd build
cmd /c '"E:\vs\VC\Auxiliary\Build\vcvarsall.bat" amd64 && chcp 65001 && set PYTHONIOENCODING=utf-8 && py ../configure.py -s css --targets x86_64 --enable-optimize && ambuild'
```

产物：`build\agpb_mm\windows-x86_64\agpb_mm.dll`

> 注意：`hl2sdk-css\source-engine-czero\build\{tier0,vstdlib,mathlib,tier1}\*.lib`
> 必须先存在，否则链接会失败（Metamod:Source 本身也依赖这几个库）。

## 部署

```
cstrike/addons/AgPB/bin/win64/agpb_mm.dll
cstrike/addons/metamod/AgPB.vdf
```

`AgPB.vdf`：

```
"Metamod Plugin"
{
	"alias"		"AgPB"
	"file"		"addons/AgPB/bin/win64/agpb_mm"
}
```

启动后控制台出现下面这行即部署成功：

```
[AgPB] loaded. gpGlobals=..., IBotManager=..., helpers=...
```

另外建议在 `server.cfg` 里把引擎自带 bot 关掉，避免 `bot_quota` 干预：

```
bot_quota 0
bot_quota_mode normal
```

## 控制台命令

| 命令 | 说明 |
|---|---|
| `agpb_add [team]` | 创建一个 AgPB bot（1=观察者 2=T 3=CT） |
| `agpb_team <idx> <team>` | 运行时切换队伍：1=观察者 2=T 3=CT |
| `agpb_list` | 列出所有 bot（实际队伍 / 目标队伍 / 血量 / 武器 / 坐标） |
| `agpb_kick <idx\|all>` | 移除 bot |
| `agpb_netlist <idx> [filter]` | 打印该 bot 的 SendTable 字段表（名字 / 偏移 / 当前值） |
| `agpb_nethandle <idx> <field>` | 解包 EHANDLE 字段并解析回实体（entry / serial / class） |
| `agpb_testmove <idx> <fwd> [yaw]` | **【临时】** 注入 `forwardmove` / `viewangles.y`，验证 ucmd 注入链路 |

路点编辑器 / 寻路（`agpb_wp_*`，详见 [ARCHIVE §4](ARCHIVE.md)）：

| 命令 | 说明 |
|---|---|
| `agpb_wp_add [x y z]` | 加一个路点；不给坐标就用**你**当前的位置 |
| `agpb_wp_del <idx>` | 删一个路点（其它点的下标会跟着修正） |
| `agpb_wp_link <from> <to> [flags]` | 连边（双向）；flags 是 `AgPB_PATH_*` 组合：1=跳 2=连跳 4=仅通视 |
| `agpb_wp_unlink <from> <to>` | 断开一条连边 |
| `agpb_wp_list [max]` | 打印路点与连边（`->邻居/连边标志`） |
| `agpb_wp_nearest` | 报告离你最近 / 最远的路点下标 |
| `agpb_wp_save` / `agpb_wp_load` | 存盘 / 重读 `addons/AgPB/waypoints/<map>.agpw` |
| `agpb_wp_clear` | 只清内存（不动文件） |
| `agpb_wp_path <to>` ｜ `<from> <to>` | 跑一遍 A* 并打印每一跳；单参数时起点取「离你最近的路点」 |
| `agpb_wp_dist <from> <to>` | 沿路点图的代价（梯子 / 蹲点位 ×2 权重） |

### 路点编辑器（菜单 + 可视化）

`agpb_menu` 打开菜单。菜单走 **"ShowMenu" usermessage**（`IVEngineServer::UserMessageBegin`），
也就是 CS:S 客户端 `CHudMenu` 渲染的左上角数字菜单（和 EBot 在 GoldSrc 上的观感一致，
**不是** VGUI 对话框）；编号由客户端发 `menuselect <n>` 回到服务器，
`agpb_menu <n>` 手输等价，数字键 `0` = 退出。

> 客户端菜单自己 **5 秒没输入就会收**（`MENU_SELECTION_TIMEOUT`），所以插件每 **3 秒**
> 重发一份一模一样的内容给它"续命" —— 菜单会一直挂在屏幕上，直到选了一项或按 `0` 退出。
> `0. Exit` 是唯一退出项（不再往 9 号塞 Exit）；选完一次动作要么重开同级菜单（多级操作），
> 要么把菜单收掉。
> 取不到 "ShowMenu" 消息 id 时会自动回落成控制台文本菜单（选项编号规则一样）。
> 文本里 `->N. 标题` 的 N 既是槽位号也是屏幕上那个编号（客户端约定）；
> 单条 usermessage 上限 255 字节，超过会按行**分段发送**，客户端自己拼。
> **菜单文字用中文**（UTF-8 直发：客户端 `ILocalize::ConvertANSIToUnicode` 走的是
> `CP_UTF8`，见 `tier1/ilocalize.cpp:23`）；**控制台输出仍保持英文** ——
> SRCDS / 客户端控制台是 GBK，中文进去会乱码。

| 命令 | 说明 |
|---|---|
| `agpb_menu [n]` | 打开主菜单 / 直接选第 n 项 |
| `agpb_wp_show [0\|1]` | 开关路点绘制（convar `agpb_wp_show`） |
| `agpb_wp_labels [0\|1]` | 每个点上方画下标 |
| `agpb_wp_alllinks [0\|1]` | 画所有连线 / 只画最近点的连线 |
| `agpb_wp_cache` | 把「离你最近的点」存成连线目标（EBot 的 Cache waypoint） |
| `agpb_wp_type <type>` | 在你脚下加一个指定类型的点：`normal` `t` `ct` `avoid` `rescue` `camp` `goal` `jump` `crouch` `ladder` `usebutton` `sniper` `lift` `fallcheck` `fallrisk` |
| `agpb_wp_flag <flag\|clear>` | 切换最近点上的标志（flag 名同上，`clear` 清空） |
| `agpb_wp_radius <0..255>` | 设最近点的到达半径 |
| `agpb_wp_wayzone [idx\|all]` | **自动算到达半径**（EBot 的 `CalculateWayzone`）：不给参数=准星/缓存/最近那个点，`all`=全部重算 |
| `agpb_wp_connect <out\|in\|both\|jump\|boost\|visible>` | 连线：起点 = 最近点，终点 = **准星指向的点**（没有就用缓存点） |
| `agpb_wp_cut` | 断开「最近点 ↔ 目标点」的连线 |
| `agpb_wp_teleport [idx]` | 传送自己到路点（`setpos`，需要 `sv_cheats 1`） |
| `agpb_wp_noclip` | 给自己开 / 关 noclip（需要 `sv_cheats 1`） |
| `agpb_wp_check` | 结构校验：越界下标 / 自连 / 孤立点 |
| `agpb_wp_stats` | 统计各类点与连线数 |
| `agpb_wp_legend` | 打印配色说明 |
| `agpb_bot_goto <idx> [wp]` | **最小导航**：让 bot 沿 A* 路线走过去（不给 wp = 准星指向 / 缓存的那个点） |
| `agpb_bot_stop <idx\|all>` | 停止行走 |

**颜色语义照抄 EBot**（`IVDebugOverlay` 逐帧重画）：

**跳跃边怎么打（照 EBot 的 `AddPath(type=1)` 约定）**：

- 站在**起跳点**上 → 准星指向（或先 `agpb_wp_cache`）**落地点** → 菜单 `创建连线... → 跳跃连线`
  （等价命令 `agpb_wp_connect jump`）
- 这一条命令做三件事：边打 `PATH_JUMP`（**单向** 起跳点 → 落地点）、**起跳点**打 `跳跃点`（JUMP）、
  起跳点半径压到 **4**（机器人得先站准再起跳）
- 落地那侧不需要 jump 属性；窄台 / 有落差建议给它补 `需要看脚下`（叠加层会往下探 60 单位：有地面蓝、悬空红）
- 反向也要跳，就站到落地点再 `agpb_wp_connect jump` 补一条反向边（跳跃边默认不对称）
- `PATH_JUMP` 目前只影响显示与数据；等最小导航层接上后，机器人走到这条边会自动按跳

**到达半径（wayzone）**：默认 `agpb_wp_autowayzone 1` —— 加点时按 EBot 的
`CalculateWayzone` 扫地形算（32→112 逐级、18 个方向、头高体积 + 前后落地点 + 头顶空间，
撞到就 -16；门直接给 0；LADDER/GOAL/CAMP/RESCUE/CROUCH 点或邻点带 LADDER/JUMP 的给 0）。
想看/回落到 EBot 加点菜单那套固定值（Normal/T/CT/Rescue=64、Camp=32、Avoid=0）就
`agpb_wp_autowayzone 0`。已经打好的点不用重打：`agpb_wp_wayzone all` 然后 `agpb_wp_save`。

**蹲行点（CROUCH）怎么设计**：EBot 的 `WAYPOINT_CROUCH` 语义是"**必须蹲着才能到达这个点**"，
不是"站在这里蹲着"。

- 放在**低矮通道 / 管道 / 矮洞的内部**（入口内侧），两端各一个更好（进、出都有）
- 蹲点会被 wayzone 自动算成 **半径 0 = 精确到达**，最小导航会一路蹲着贴到点上，
  并且在离开矮区前（离上一个 CROUCH 点 64 单位内）保持蹲姿，不会站起来顶天花板
- 到达判定照 EBot：`radius >= 50` → 进圈就算到；否则 → 走到 `max(radius, 4)` 以内（带速度外推，
  避免高速擦过被判"没到"）。所以别给蹲点手动设大半径，否则会提前算到达
- Source 里蹲行速度上限 ≈ `maxspeed * 0.34`（USP 250 → ~85），这是引擎设计；
  EBot 在 GoldSrc 上还要手动把速度顶上去（`navigate.cpp:459-475`），Source 不需要
- 只想"蹲在这儿守着"用 `蹲守(CAMP)`；要蹲着钻又要守，就 CROUCH + CAMP 都打

- 点：下半截 = 基础色，上半截 = 附加标志色
  - 基础色：CAMP=青 GOAL=紫 LADDER=棕 RESCUE=白 AVOID=红 FALLCHECK/FALLRISK=灰
    USEBUTTON=蓝；扩展：JUMP=黄 CROUCH=紫罗兰 LIFT=墨绿；其它 = 绿
  - 附加色：SNIPER=暗金 T=红 CT=蓝 FALLRISK=粉
- 连线（以最近点为中心）：JUMP=红 BOOST=蓝 VISIBLE=通视绿 / 被挡橙 双向=黄
  只有出边=白 只有入边=墨绿 其它点的连线=蓝
- 半径 = 蓝框（≤4 画小叉）；缓存点 = 黄线、准星指向点 = 白线（拉到你身上）
- FALLCHECK / FALLRISK 点：往下探 60 单位，有地面 = 蓝、悬空 = 红
- 线的粗细与可见性：`agpb_wp_thick <1..5>`（默认 3）—— Source 的 debug overlay
  没有线宽参数，所以每根线会沿**观察者视角的 right/up** 做世界空间小偏移叠画 N 遍；
  `agpb_wp_xray <0|1>`（默认 1）决定要不要穿墙画；非最近点的连线按 110/255 压暗，
  让"当前操作的点和它的连线"跳出来

> 打点命令不带坐标时取「第一个非假客户端玩家」的位置 ——
> MMS 的 ConCommand 回调**不带 client**，listen server 上这就是你自己。

ConVar：`agpb_enable`（默认 1）、`agpb_team`（默认 2）、`agpb_wp_show`（默认 0）、
`agpb_wp_labels`（默认 0）、`agpb_wp_alllinks`（默认 1）、`agpb_wp_thick`（默认 3）、
`agpb_wp_xray`（默认 1）、`agpb_wp_autowayzone`（默认 1）。

## 验收

完整的四阶段验证清单（含期望输出与判读表）见 [`VERIFY.md`](VERIFY.md)。

最关键的结论已经实测通过：

```
agpb_testmove 0 400 90
  m_vecVelocity[1] = 250.0000      <-- yaw=90 即 +Y 方向；250 正好是 USP 跑速上限
  m_angEyeAngles[1] = 90.0000      <-- 注入的 yaw 原样出现在视角字段里
```

即 `Think() -> IBotController::RunPlayerMove() -> CPlayerMove::RunCommand -> PM_Move`
这条链真的驱动了玩家，而且假客户端的视角会跟随 `CUserCmd`。

## 路线图

| 阶段 | 内容 | 状态 |
|---|---|---|
| **M1** | 假客户端 + usercmd 注入 + 队伍切换 | ✅ |
| **M2** | netvar 反射层（`Entity` 底座） | ✅ |
| **M2.5** | ucmd 注入端到端验证（`RunPlayerMove` 真的驱动玩家） | ✅ 已实测 |
| **M3** | EBot 移植 | 🟡 |
| ↳ 路点系统 | 数据模型 + 编辑器命令 + A* 寻路 / 路径距离（`waypoint.h` / `waypoint.cpp`） | ✅ 已落地（自研，非照抄） |
| ↳ 编辑器 / 可视化 | 中文 HUD 菜单（`ShowMenu` usermessage）+ `agpb_wp_*` 动作 + EBot 配色叠加层（`menu.*` / `wpedit.*` / `wpdraw.*`） | ✅ 已实测（菜单、绘制、颜色） |
| ↳ wayzone 自动半径 | `CalculateWayzone`（EBot 移植）+ `agpb_wp_wayzone [idx\|all]` | ✅ 已落地 |
| ↳ 最小导航（验证用） | `agpb_bot_goto`：转向 + 前进 + 跳 / 蹲 + 卡住停（不是 EBot 的 control） | ✅ 已落地，待路点数据实测 |
| ↳ 替身层 | `entvars_t` / `Entity` / `Client` / `Engine`（实施顺序：`Engine` 最先，见 [ARCHIVE §7](ARCHIVE.md)） | ⬜ |
| ↳ `control` / `navigate` / `combat` | 跟随路点走、脱困、战斗（EBot 的弹道跳要改写，见 ARCHIVE §7「2026-09-25 进度」） | ⬜ |
| **M4** | UDP 桥 + Python agent | ⬜ |
| **M5** | LLM 战术层 | ⬜ |

## 致谢 / 参考实现

本项目的 `waypoint` / `control` / `navigate` / `combat` 模块移植自 **CS-EBOT**，
它是一支血统悠久的开源 bot 的延续（POD-Bot → YaPB → SyPB → CS-EBOT）：

- [EfeDursun125/CS-EBOT](https://github.com/EfeDursun125/CS-EBOT) —— 直接参考实现（CS 1.6）
- [CCNHsK-Dev/SyPB](https://github.com/CCNHsK-Dev/SyPB) —— CS-EBOT 的上游
- [yapb/yapb](https://github.com/yapb/yapb) —— YaPB，SyPB 的上游

`Bot::FindFriendsAndEnemiens()`、按 waypoint **路径距离**选最近敌人、
用一条 TraceLine 做可见性判定等核心设计均直接来自上述项目。

宿主与 SDK：

- [Metamod:Source](https://www.metamodsource.net/) —— 插件宿主（MMS 2.0，Source 1）
- CS:S SDK（`hl2sdk-css`，含 `source-engine-czero`）—— 构建期头文件来源

## 许可证

[**GPL-3.0**](LICENSE)。

之所以不用 MIT：M3 阶段会移植 EBot 的代码，而 EBot 的上游 SyPB 是 GPL-3.0，
因此分发包含这些代码的二进制时必须一并提供对应源码。
