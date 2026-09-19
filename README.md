# AgPB

Counter-Strike: Source（Source 1 / Win64）的 agent 控制 bot 插件
（插件内部名 / VDF alias：`AgPB`）。

- 不使用 SourceMod
- 不使用引擎自带的 `CCSBot` / `CCSBotManager`
- 由外部 agent（LLM）下发高层意图，插件内的反射层逐 tick 生成 `CUserCmd`

> **状态**：M1 / M2 / M2.5 已实测通过；M3 第一步「路点系统」已落地
> （无 AI 假客户端 + ucmd 注入驱动玩家 + SendTable 零索引反射层 + EHANDLE 解析
> + 手工路点图 / A* 寻路）
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

> 打点命令不带坐标时取「第一个非假客户端玩家」的位置 ——
> MMS 的 ConCommand 回调**不带 client**，listen server 上这就是你自己。

ConVar：`agpb_enable`（默认 1）、`agpb_team`（默认 2）。

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
| ↳ 替身层 | `entvars_t` / `Entity` / `Client` / `Engine`（实施顺序：`Engine` 最先，见 [ARCHIVE §7](ARCHIVE.md)） | ⬜ |
| ↳ `control` / `navigate` / `combat` | 跟随路点走、脱困、战斗（EBot 的弹道跳要改写，见 ARCHIVE §4「还没做」） | ⬜ |
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
