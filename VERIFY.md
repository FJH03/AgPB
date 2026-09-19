# AgPB 验证清单

> 所有验证都是**控制台命令**，仓库里不放任何测试脚本
> （唯一例外是 `agpb_testmove` 这个临时 ConCommand，M3 的 `control` 接管后删除）。
> 状态：M1 ✅ / M2 ✅ / **M2.5 ✅ 已实测通过** / **M3 路点系统 ✅ 已落地**
> 详细背景见 [`ARCHIVE.md`](ARCHIVE.md)。
>
> 输出里的 `s=` 是该字段的 `m_ElementStride`。标量恒为 **-1**（`SIZEOF_IGNORE`），
> 这是正常的 —— SendTable 里拿不到字段宽度，int 取值走引擎自己的 proxy。

## 准备

```
cstrike/addons/AgPB/bin/win64/agpb_mm.dll
cstrike/addons/metamod/AgPB.vdf
```

加载成功的标志：

```
[AgPB] loaded. gpGlobals=..., IBotManager=..., helpers=...
```

（`IBotManager` 必须是 `ok`，否则 `agpb_add` 不会工作。）

---

## 阶段 A —— 建 bot 并出生

```
agpb_kick all
agpb_add 3              // 3 = CT；不带参数时用 agpb_team 的默认值 2（T）
mp_restartgame 1
```

**★ 这里等 2 秒**。`mp_restartgame 1` 之后回合重启才会把 bot 刷进地图
（`CCSGameRules::FPlayerCanRespawn()` 的闸门，见 ARCHIVE §3.3）。

> 2026-09-19 实测：这次 `agpb_add 3` 之后**没打** `mp_restartgame`，
> `agpb_list` 就已经是 `hp=100` 且坐标有效 —— 所以 restart 不是每次都必需，
> 但只要列表里出现 `hp=0` / 坐标全 0，就补一次 `mp_restartgame 1`
> **⚠️ 同队测试的安全边界**（崩溃根因见 [`CRASH_REPORT.md`](CRASH_REPORT.md)）
>
> freezetime 结束时 `CCSGameRules::CheckFreezePeriodExpired()` 会让**每队第一个
> `STATE_ACTIVE` 玩家**喊一次无线电（`radio.go` 之类）；若那个玩家是假客户端，
> `CCSPlayer::Radio()` 里的 `dynamic_cast<CCSBot*>` 会得到 NULL 并崩溃。
>
> 所以测试期间必须保证：**bot 所在队伍里有一个 slot 更小、且处于 `STATE_ACTIVE` 的玩家**
> （通常是先连进来的人类玩家）。
>
> 会踩雷的操作（都会让 bot 变成"该队第一个活跃玩家"）：
> - 人类玩家**断线**
> - 人类切**观察者** / **换队**（`spectate`、`agpb_team` 之类）
> - 人类还没**进入游戏**就先让 bot 占住另一支队
>
> 安全做法：换场景前先 `agpb_kick all`，或者干脆让 bot 与人类始终同队。
> 崩溃本身目前**未修复**，属已知问题。
---

## 阶段 B —— 反射层全量检查

```
agpb_list
agpb_netlist 0 m_iHealth
agpb_netlist 0 m_iTeamNum
agpb_netlist 0 m_lifeState
agpb_netlist 0 m_vecOrigin
agpb_netlist 0 m_vecVelocity
agpb_netlist 0 m_angEyeAngles
agpb_netlist 0 m_iAmmo
agpb_netlist 0 m_iAccount
agpb_netlist 0 m_iClass
agpb_netlist 0 m_iShotsFired
agpb_netlist 0 m_flNextAttack
agpb_nethandle 0 m_hActiveWeapon
```

### 期望值与判读

| 命令 | 已实测输出（2026-09-19 复核） | 在验证什么 |
|---|---|---|
| `agpb_list` | `[0] AgPB_01 slot=2 team=3 want=3 hp=100 wpn=weapon_usp pos=...` | 基线：队伍 / 血量 / 武器 / 坐标 |
| `m_iHealth` | `+364 s=-1 int = 100` | 标量 + `baseclass` 偏移递归 |
| `m_iTeamNum` | `+716 s=-1 int = 3` | 同上 |
| `m_lifeState` | `+368 s=-1 int = 0`（`LIFE_ALIVE`） | **int 宽度只能从 proxy 得到**，见 ARCHIVE §4 |
| `m_vecOrigin` | `+1076 s=-1 vec3`，**同名同偏移出现 3 次** | `DPT_Vector`；3 次不是 bug：`cs_player.cpp` 235/322/341 在三个玩家表里各注册一次 |
| `m_vecVelocity` | `[0] +888` / `[1] +892` / `[2] +896`，静止时全 `0.0000` | **负偏移 / `SENDPROP_VECTORELEM`** |
| `m_angEyeAngles` | `[0] +6984` / `[1] +6988`，静止时 `0.0000` | 同上（两项间隔 4 字节） |
| `m_iAmmo` | `+2136 array [32 x 4] elem=int`，`[8] = 100`，其余全 `0` | **`SendPropArray3` 合成表折叠** |
| `m_iAccount` | `+5384 s=-1 int = 800`（开局） | 经济系统 |
| `m_iClass` | `+6940 s=-1 int`，值落在 **6..10**（本次实测 `7` = `CS_CLASS_GSG_9`） | 值**每次会变**：`joinclass 0` 是让引擎在 CT 职业里挑一个（`cs_shareddefs.h:196-215`）—— 判据是"落在 6..10"，不是"等于 7" |
| `m_iShotsFired` | `+6504 s=-1 int = 0` | 标量 |
| `m_flNextAttack` | `+2044 s=-1 float`（实测 9.18 / 11.43，随对局时间变化） | **它在网络表里**，不需要 datamap |
| `agpb_nethandle 0 m_hActiveWeapon` | `raw=0x0000000007DF0061` → `entry=97 serial=32240` → `resolved=yes class=CWeaponUSP` | **EHANDLE 解包 + 实体解析**（本地 8 字节：entry 12 位 + serial 20 位） |

`class=` 里的武器名要和 `agpb_list` 的 `wpn=` 对得上：实测 `class=CWeaponUSP`（ServerClass 名）
对应 `wpn=weapon_usp`（`IPlayerInfo::GetWeaponName()` 返回的是类名的小写形式，**不是** `usp`）。

`agpb_nethandle` 会先把字段信息全打出来，所以失败时能立刻看出是
「字段不存在」还是「句柄为空」还是「serial 校验没过」。

（`entry` 稳定，`serial` **每次开服/换图都会变**（11330 / 32240 都见过）——
判据看 `entry` 和 `resolved=yes`，不要去对 `serial`。）

#### 为什么清单里没列 `agpb_netlist <EHANDLE 字段>`

`agpb_netlist` 读 EHANDLE 字段会走引擎 proxy，拿到的是**网络传输压缩值**，
不是本地句柄 —— **没有判据价值**（下面的实测对照就是证据），所以清单里只留 `agpb_nethandle`。

| 读法 | 实测值 | 打包方式 |
|---|---|---|
| `agpb_netlist`（走引擎 proxy） | `1015905` = `0x000F8061` | **网络传输用**：entry 低 **11** 位 + serial **9** 位（`SendProxy_EHandleToInt`） |
| `agpb_nethandle`（直读内存） | `0x0000000007DF0061` | 本地 `CBaseHandle` **8 字节**：entry 低 **12** 位 + serial **20** 位 |

两者可以互相验算（实测值完全吻合）：

```
本地句柄 0x07DF0061 → entry = 0x061 = 97，serial = 0x07DF0 = 32240
网络压缩值 = 97 | ((32240 & 0x1FF) << 11) = 97 + 496 × 2048 = 1015905  ✓
```

所以 **netlist 打印的 EHANDLE 数字不能用来判断句柄对不对** ——
`entry` / `serial` / 实体解析一律看 `agpb_nethandle`。
（`m_hOwnerEntity` / `m_hGroundEntity` 等 EHANDLE 字段同理，M3 里一律用 `agpb_nethandle` 查。）

---

## 阶段 C —— ucmd 注入（**最关键的一步**）

M1/M2 只验证了「能创建 bot、能读它的数据」，而**「`IBotController::RunPlayerMove()`
真的驱动了玩家」才是整个架构（不走 CCSBot、自己注入 usercmd）的前提** ——
所以这一步单独拉出来打，已于 2026-09-19 实测通过（记录见本节末尾）。

```
agpb_testmove 0 400 90
```

**★ 这里等 2 秒**，同时看着游戏里的 bot 是否开始往前走。

```
agpb_netlist 0 m_vecVelocity
agpb_netlist 0 m_angEyeAngles
agpb_netlist 0 m_vecOrigin
agpb_testmove 0 0 0
agpb_netlist 0 m_vecVelocity
```

`agpb_testmove` 的输入是**持续生效**的（值存在 bot 对象里，每 tick 写进 `CUserCmd`），
直到下一条 `agpb_testmove` 把它改掉，所以不需要掉时间。

### ✅ 已实测通过（2026-09-19）

```
agpb_testmove 0 400 90
  m_vecVelocity[0] = -0.0000
  m_vecVelocity[1] = 250.0000     <-- yaw=90 即 +Y 方向；250 正好是 USP 跑速上限
  m_vecVelocity[2] =  0.0000
  m_angEyeAngles[1] = 90.0000     <-- 我给的 yaw 原样出现在视角字段里
agpb_testmove 0 0 0
```

方向、大小、速度上限**三个都对**：

```
Think() -> IBotController::RunPlayerMove() -> CPlayerMove::RunCommand -> PM_Move
```

**两个结论：**

1. `RunPlayerMove` 真的驱动玩家 —— 整个架构的前提成立，M3 的 control/navigate 有落脚点。
2. **假客户端的视角跟随 `CUserCmd`** —— `m_angEyeAngles[0]/[1]`（+6984/+6988）
   就是可靠的当前朝向来源，EBot 的 `IsInViewCone` 直接用。

下表留作以后回归验证的参照。

### 判读

| 现象 | 结论 | 下一步 |
|---|---|---|
| `m_vecVelocity` 变成非零、游戏里看到 bot 走动 | ✅ **ucmd 注入通了**，架构前提成立 | 直接开 M3 |
| `m_angEyeAngles[1] ≈ 90` | ✅ 视角跟随 ucmd | EBot 的 `IsInViewCone` 可以直接用 |
| `m_angEyeAngles[1]` 仍是 0，但速度非零 | ⚠️ 假客户端的视角**不从 `CUserCmd` 推导** | 得另找设置朝向的途径（直接写角度字段 / 另发消息）；`m_angEyeAngles` 不能作为朝向来源 |
| `m_vecVelocity` 全 0，bot 不动 | ❌ `RunPlayerMove` 没生效 | **必须优先解决**，否则 M3 的 navigate/control 没有落脚点 |

---

## 阶段 D —— 路点编辑器与寻路（M3 第一步）

路点系统是**自包含**的：不依赖 bot，也不依赖 `.nav`。整张图存在
`cstrike/addons/AgPB/waypoints/<map>.agpw`，换图时按地图名自动读盘。

### D.1 打点 / 连线 / 存盘

```
agpb_wp_add                     // 在「你」的位置加一个点（listen server 上第一个真人就是你）
agpb_wp_add 1024 -512 0         // 也可以显式给坐标
agpb_wp_nearest                 // 报告最近 / 最远路点下标，方便拿 idx
agpb_wp_link 0 1                // 连边（双向）；flags：1=跳 2=连跳 4=仅通视
agpb_wp_list 10                 // 核对（->邻居/连边标志）
agpb_wp_save                    // 落盘
```

判读：

| 命令 | 期望输出 |
|---|---|
| `agpb_wp_add` | `[AgPB] waypoint N added at x y z (M total, K links)` |
| `agpb_wp_nearest` | `nearest = N at ...`；一个点都没有时是 `nearest = none (no waypoints yet)` |
| `agpb_wp_link 0 1` | `[AgPB] link 0 <-> 1 flags=0x0 (added); K links total` |
| `agpb_wp_list N` | 每行 `[  i] x y z flags=0x.. r=.. ->邻居/0x..` |
| `agpb_wp_save` | `[AgPB] wp_save ok: saved M point(s), K link(s) -> addons/AgPB/waypoints/<map>.agpw` |

`agpb_wp_save` 失败时 `Status()` 会说明原因：`no IFileSystem` / `no map name` /
`WriteFile failed (see console for FS errors)`。写不进去先看服务端控制台里
`IFileSystem` 自己的报错。

⚠️ `agpb_wp_del <idx>` 会让**它后面的下标整体前移**，删完记得 `agpb_wp_list` 重新确认；
已存的连边会自动修正（指向被删点的被清掉）。

### D.2 换图后自动重载

```
changelevel cs_office
agpb_wp_list
```

期望：`[AgPB] map=cs_office file=addons/AgPB/waypoints/cs_office.agpw`，
然后是 `loaded M point(s), K link(s) from ...`。

没打过点的图会是 `no waypoint file for this map yet (use agpb_wp_save to create one)` ——
这是**正常**的，不是错误。

### D.3 寻路（不需要起 bot）

```
agpb_wp_path 5                  // 从「离你最近的路点」走到 5
agpb_wp_path 0 5                // 显式指定起点
agpb_wp_dist 0 5                // 沿图的代价（梯子 / 蹲点位 x2 权重）
```

期望输出：

```
[AgPB] path 0 -> 5: 4 hop(s)
   0. [  0] ...  flags=0x0  leg=0    link=0x0
   1. [  3] ...  flags=0x0  leg=210  link=0x0
   ...
[AgPB] path distance = 634 (straight line = 402)
```

| 现象 | 结论 |
|---|---|
| 列出完整路径，且 `path distance` 明显大于 `straight line` | ✅ 图连通，且代价确实沿连边走（不是直线距离） |
| `no path 0 -> 5 (M point(s), K link(s) in graph)` | 图不连通 —— 用 `agpb_wp_list` 检查是不是漏了 `agpb_wp_link` |
| `agpb_wp_dist` 回 `unreachable` | 同上 |
| 路径绕开了某个点 | 那点带了 `AVOID`（或阵营不符）—— 设计如此 |

> `PATH_DOUBLE`（需要队友叠罗汉）的边一律当不通；`PATH_JUMP` / `PATH_VISIBLE`
> 目前不影响寻路（要 M3 的动作层才会用到）。

---

## 相关命令一览

| 命令 | 说明 |
|---|---|
| `agpb_add [team]` | 创建 bot（1=观察者 2=T 3=CT） |
| `agpb_kick <idx\|all>` | 移除 bot |
| `agpb_list` | 列出所有 bot |
| `agpb_team <idx> <team>` | 运行时切换队伍 |
| `agpb_netlist <idx> [filter]` | 展开 SendTable 字段表（字段名 / 偏移 / 当前值） |
| `agpb_nethandle <idx> <field>` | 解包 EHANDLE 并解析回实体 |
| `agpb_testmove <idx> <fwd> [yaw]` | **【临时】** 注入 `forwardmove` / `viewangles.y`；M3 的 `control` 模块接管后删除。值存在 bot 对象里，**持续生效**直到被改掉 |
| `agpb_wp_add [x y z]` | 加路点（不给坐标就用你的位置） |
| `agpb_wp_del <idx>` | 删路点（后续下标前移，连边自动修正） |
| `agpb_wp_link <from> <to> [flags]` | 连边（双向）；flags：1=跳 2=连跳 4=仅通视 |
| `agpb_wp_unlink <from> <to>` | 断开连边 |
| `agpb_wp_list [max]` | 打印路点与连边 |
| `agpb_wp_nearest` | 最近 / 最远路点 |
| `agpb_wp_save` / `agpb_wp_load` | 存盘 / 重读 `<map>.agpw` |
| `agpb_wp_clear` | 只清内存（不动文件） |
| `agpb_wp_path <to>` ｜ `<from> <to>` | A* 寻路，打印每一跳 |
| `agpb_wp_dist <from> <to>` | 沿路点图的路径代价 |

ConVar：`agpb_enable`（默认 1）、`agpb_team`（默认 2）。
