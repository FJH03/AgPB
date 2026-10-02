# AgPB 验证清单

> 只列命令与期望，原理看 [`PROGRESS.md`](PROGRESS.md)，命令参数看 [`COMMANDS.md`](COMMANDS.md)。
> 现状：M1 / M2 / M2.5 ✅ 已实测；M3 路点系统 / 菜单 / 最小导航 ✅ 已落地。
>
> ⚠️ **安全前提：bot 必须与人类同队**。bot 独占一队时，freezetime 结束会崩
> （`CCSPlayer::Radio()` 对非 `CCSBot` 的假客户端解引用空指针，**未修**，见
> [`CRASH_REPORT.md`](CRASH_REPORT.md)）。换场景前先 `agpb_kick all`。

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

## 阶段 A —— 建 bot 并出生

```
agpb_kick all
agpb_add 3              // 3 = CT；不带参数用 agpb_team 默认值 2（T）
mp_restartgame 1        // 不是必须；列表里 hp=0 / 坐标全 0 时补一次
agpb_list
```

期望：`[0] ... team=3 want=3 hp=100` 且坐标有效，武器是 `weapon_usp`。

## 阶段 B —— 反射层

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

## 阶段 C —— ucmd 注入（关键一步）

先打两个点（阶段 D），然后：

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
| 视角不变但速度非零 | ⚠️ 得另找设置朝向的途径 |
| 全 0、bot 不动 | ❌ `RunPlayerMove` 没生效，先解决这个 |

## 阶段 D —— 路点编辑器与寻路

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

## 阶段 E —— 菜单 / 绘制

```
agpb_menu
agpb_wp_show 1
agpb_wp_legend
agpb_wp_thick 4
agpb_wp_xray 0
agpb_wp_flag t
```

期望：

- 屏幕左上角出现**中文**数字菜单，`0` 退出；按数字键有英文控制台回显。
- 点按标志变色（`agpb_wp_flag t` 后上半截变红），最近点带信息文字，连线可辨类型。
- 菜单不会自己消失（每 3 秒续命）；弹不出菜单时控制台出现同样的文本菜单也算通过。

## 阶段 F —— bot 走路（最小导航）

准备（同队）：`agpb_add 2`，`agpb_list` 确认 hp / 坐标正常。

```
// 站在 A 点
agpb_wp_type normal
// 走到 B 点
agpb_wp_type normal
agpb_wp_cache
// 人站回 A 点附近
agpb_bot_goto 0
```

期望：

- 控制台先打印路线（`walking to #1, 2 hop(s): 0 1`）；
- bot 转身朝 B、以 ~250 的速度走过去；
- 到达后打印 `arrived at waypoint #1` 并停住；
- 撞墙 / 被卡住打印 `stuck at ...` 并停下（走不通 = 路点数据问题）。

跳跃：在起跳点用 `agpb_wp_connect jump` 连到落点，再 `agpb_bot_goto 0`。
期望是**站立跳**（57 高度）起跳；空中蹲由引擎自动完成，bot 不该在跳跃边按蹲。

蹲行：把 B 打在矮通道里、`agpb_wp_flag crouch`、`agpb_wp_wayzone`（蹲点应算出半径 0），
再 `agpb_bot_goto 0`：期望 bot 蹲着走进去、速度掉到 ~85、离开矮区前保持蹲姿。

## 阶段 G —— 几何判定 / 体积模式

```
agpb_wp_hullmode 0          // 0=自动
agpb_wp_show 1
sv_cs_use_legacy_viewvectors 1   // 老 CS:S：站立 62 / 蹲姿 45
agpb_wp_wayzone 0
sv_cs_use_legacy_viewvectors 0   // CS:GO 风格：站立 72 / 蹲姿 54
agpb_wp_wayzone 0
```

期望：切换体积模式后，点的竖线高度、wayzone 算出的半径、`agpb_wp_reach` 的可达性跟着变。
想固定用 `agpb_wp_hullmode 1`（CSS）或 `2`（CS:GO）。

```
agpb_wp_reach        // 站在 A 点、准星指 B
agpb_wp_check
```

期望：`reach` 输出 `-> #N: dist ..., reachable=YES/NO, must-jump=YES/NO` 和边是否存在；
`check` 额外给出「像要跳但没打标志」和「打了多余的跳标志」的边的数量。
**只提示、不改数据** —— 跳跃标志一律手工连。

## 已搁置

netvar 写入验收（原阶段 H）已搁置：写入层降级为开发 / 诊断工具，不再列入常规回归。
工具本身还能用：`agpb_netwrite` / `agpb_bot_vel`，用法见 [`COMMANDS.md`](COMMANDS.md)。
