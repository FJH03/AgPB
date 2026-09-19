# AgPB 验证清单

> 所有验证都是**控制台命令**，源码里不放任何测试脚本。
> 状态：M1 ✅ / M2 ✅ / **M2.5 ✅ 已实测通过**
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
agpb_add 3
mp_restartgame 1
```

**★ 这里等 2 秒**。`mp_restartgame 1` 之后回合重启才会把 bot 刷进地图
（`CCSGameRules::FPlayerCanRespawn()` 的闸门，见 ARCHIVE §3.3）。

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
agpb_netlist 0 m_hActiveWeapon
agpb_netlist 0 m_flNextAttack
agpb_nethandle 0 m_hActiveWeapon
```

### 期望值与判读

| 命令 | 已实测的期望输出 | 在验证什么 |
|---|---|---|
| `agpb_list` | `[0] AgPB_xx slot=.. team=3 want=3 hp=100 wpn=usp` | 基线：队伍/血量/武器 |
| `m_iHealth` | `+364 int = 100` | 标量 + `baseclass` 偏移递归 |
| `m_iTeamNum` | `+716 int = 3` | 同上 |
| `m_lifeState` | `int = 0` | `LIFE_ALIVE` |
| `m_vecOrigin` | `vec3 = x y z` | `DPT_Vector` |
| `m_vecVelocity` | `[0] +888` / `[1] +892` / `[2] +896`，静止时全 `0.0000` | **负偏移 / `SENDPROP_VECTORELEM`** |
| `m_angEyeAngles` | `[0] +6984` / `[1] +6988` | 同上（两项间隔 4 字节） |
| `m_iAmmo` | `+2136 array [32 x 4] elem=int`，`[8] = 100` | **`SendPropArray3` 合成表折叠** |
| `m_iAccount` | `int`（实测开局 800） | 经济系统 |
| `m_flNextAttack` | `float`（实测 11.43，= 可再次攻击的时间点） | **它在网络表里**，不需要 datamap |
| `m_vecOrigin` | **会出现 3 次**，同名同偏移 | 不是 bug：`cs_player.cpp` 235/322/341 在三个玩家表里各注册一次 |
| `m_lifeState` | `+368 int = 0`（`LIFE_ALIVE`） | **int 宽度只能从 proxy 得到**，见 ARCHIVE §4 |
| `agpb_nethandle 0 m_hActiveWeapon` | `stride=-1` → `raw=0x...` → `entry=N` → **`resolved=yes class=CWeaponUSP`**（实测） | **EHANDLE 解包 + 实体解析** |

`class=` 里的武器名要和 `agpb_list` 的 `wpn=` 对得上（一个是 ServerClass 名，一个是 `IPlayerInfo` 的说法）。

`agpb_nethandle` 会先把字段信息全打出来，所以失败时能立刻看出是
「字段不存在」还是「句柄为空」还是「serial 校验没过」。

---

## 阶段 C —— ucmd 注入（**最关键的一步**）

到目前为止，M1/M2 只验证了「能创建 bot、能读它的数据」。
**「`IBotController::RunPlayerMove()` 真的驱动了玩家」这件事一次都没验证过** ——
而整个架构（不走 CCSBot、自己注入 usercmd）的前提就是它。

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
| bot 走动但方向不对 / 撞墙 | 正常 | `forwardmove` 是相对 yaw 的，yaw=90 时朝向会变 |

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

ConVar：`agpb_enable`（默认 1）、`agpb_team`（默认 2）。
