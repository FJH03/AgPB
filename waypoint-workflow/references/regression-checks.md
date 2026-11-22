# 地图目标与导航回归

按本次改动选择爆破、生命周期或清障场景。运行命令前完成[实走准备](route-validation.md)，按需编译并加载对应的 [SourceMod 夹具](route-validation.md#准备夹具)。以下示例在仓库根目录执行，bot 编号和玩家 slot 使用当次实际值。人质护送和无图近距离交互见[验证清单](../../VERIFY.md)。

## 爆破目标

普通模式 `agpb_mode normal` 的拾包、A/B 安放与拆包、T/CT 逃爆。自动记录使用 `agpb_waypoint_probe` 和 `agpb_behavior_fixture`。前者只读，后者的准备命令要求 `sv_cheats 1`。没有 SourceMod 时可记录原生实际事件和状态进行人工验收，缺可靠观测的项目标为未验证。

1. 每个场景先踢除 bot 并确认零 bot，再准备真人队伍，随后建立同队场景。记录设置原值，核对 `sv_cheats 1`、`bot_quota 0`、`mp_ignore_round_win_conditions 1`，关闭自动平衡与闲置踢人。用 `map_showbombradius` 和 `agpb_objectives` 核对伤害半径及来源，运行中改动时同步对应覆盖设置。
2. 用 `agpb_wp_probe_state` 核对玩家，用 `agpb_wp_probe_bomb` 记录 `bomb_planted`、`bomb_defused`、`bomb_exploded` 的基线。人工准备可用下表的真人接口，掉包位置须与 bot 出生点留出足够距离以采到真实落地状态。
3. 对 A 和 B 分别执行 `T_escape`、`CT_escape`、`CT_defuse`，每次使用新的 `--name`。默认场景标签和坐标适用于 de_dust2，其他地图以 `--help` 中的场景参数指定本图标签和位置。

| 准备命令 | 用法 |
|---|---|
| `agpb_reg_place_human slot x y z yaw` | 摆放真人 |
| `agpb_reg_human_plant slot` | 真人持真实 C4 到真实包区，通过真人 CUserCmd 安放 |
| `agpb_reg_drop_c4 slot x y z` | 真人正常丢弃持有的 C4，再摆放落地武器 |
| `agpb_reg_human_team slot T\|CT` | 零 playing fake 时换真人队并重生 |
| `agpb_reg_clear` | 取消夹具请求并恢复存活点控对象，随后读回确认 |

```powershell
py -B .\waypoint-workflow\verify_objectives.py --plan $plan `
  --session-name $session --session-directory $sessionDir `
  --mode T_escape --site A --name objective-T-A-01
```

| 场景 | 通过证据 |
|---|---|
| 拾包及携包接近 | 无人持有的落地 C4 转为 bot 所有，沿手工路线到指定区域时仍持包 |
| A/B 安放 | 新安放事件归于实际 bot userid，`planted_c4` 在真实包区或 GOAL 邻域计时 |
| A/B 拆包 | 真人安放后在零 bot 时转 CT，再加入同队 bot。新拆包事件归于 bot，同一颗 C4 停表且无爆炸事件 |
| T/CT 逃爆 | 实际撤离及真实爆炸，且更晚 tick 的同回合、同身份样本仍存活。剩时客观不足单列 |

使用同回合、同一颗 C4 的实际状态及新增事件核对结果。缺失属性返回 `-1`，记录为未知。只在 C4 正计时时要求位置固定。任务完成回显、GOAL 标志、夹具 armed 或 JSON 中的通过标记均需真实证据印证。安放后保持自主任务和真实炸弹，等待实际事件自然发生。拆包等待同时受 15 秒游戏时钟和 60 秒墙钟限制，分别记录。

## 生命周期回归（M3a）

适用于变更停止、冻结恢复、死亡、换队、回合或换图清理后的回归。加载 `agpb_waypoint_probe` 和 `agpb_behavior_fixture`。关闭受测 bot 的 auto 和 roam，每个场景先取有路线及 owner / goal 的 `before`。

```text
agpb_bot_info 0; agpb_wp_probe_state; agpb_wp_probe_lifecycle
```

干预后跨至少两个 tick 再取样，按游戏时间、tick 和命令序号递增记录。每个 `stopped` 或 `frozen` 窗口至少 3 个样本、跨度至少 0.8 游戏秒、相邻间隔至多 1.5 秒。停止在实际运动且朝向明显非零时触发。除换图恢复外保持原客户端身份。

| `kind` | `stages` 顺序 | 通过证据 |
|---|---|---|
| `stop` | `before` / `stopped` | `agpb_bot_stop` 清除任务、路线和输入，保持朝向 |
| `freeze_resume` | `before` / `frozen` / `resumed` | 真实冻结时释放输入，保持路线归属，解冻后无新 goto 自动续行 |
| `freeze_settle` | `before` / `frozen` / `resumed` | 冻结前 `0 ≤ settleage < 0.8`，冻结清计时，解冻后重新完整计时并到达原目标 |
| `death_respawn` | `before` / `dead` / `respawned` / `recovered` | 同身份真实死亡及重生，状态已清理，再给原目标并实际移动 |
| `team` | `before` / `changed` / `returned` / `recovered` | 通过 `agpb_bot_team` 切观察者再返原队，核实真实队伍事件及清理，再给原目标 |
| `round` | `before` / `cleared` / `recovered` | 原生 `endround` 后核实实际回合事件、代次及清理，新回合再给原目标 |
| `map` | `before` / `absent` / `recovered` | 踢至零 bot，转场并保持零 bot，回原图后检查新 bot 首帧干净，再给原目标 |

- 冻结用 `agpb_reg_lifecycle_freeze bot_slot human_slot seconds [after_seconds]`，持续 0.5–15 秒、延迟 0–5 秒。它通过原生 `point_viewcontrol` 冻结。用 `freeze_begin` 的 `camera_owner=bot_slot`、`frozen=1`、`view_ent=camera` 与核心快照交叉确认，待 `frozen=0` 且玩家视角恢复后再做其他场景。核心原始冻结位是 64，SourceMod 标准化冻结位是 32。冻结和自动续行之间保持任务命令不变。
- `agpb_reg_lifecycle_kill bot_slot human_slot` 和 `agpb_reg_lifecycle_respawn bot_slot human_slot` 仅用于准备真实死亡和重生。操作前确认点控已释放。需换真人队时，先将所有 playing bot 切到观察者并核对零 playing fake，再换真人队，最后让 bot 回到真人当前队。核心换队回归使用 `agpb_bot_team`。

保留原始 console、完整 BEGIN/END 回执及 ACK 外字节。ACK 外事件可放入 `observed_log`，其中保留完整原 `log`。按 `(epoch, seq)` 去除探针缓存重放，依据新增事件时间、userid、serial 和地图代次归因。换队事件读 `newteam`，再核对后续实际队伍。

证据 JSON 使用 `schema_version: 1`，顶层含 `metadata` 和 `cases`。在 `metadata` 中记 `map`、`bot_slot`、`bot_userid`、`bot_name`、`human_slot`、`human_userid`，以及六项 SHA-256 字段 `plan_sha256`、`agpw_sha256`、`bsp_sha256`、`dll_sha256`、`probe_sha256`、`fixture_sha256`。最后两项对应实际加载的 SMX。每个 case 带 `kind`、相同六项哈希的 `binding` 和 `stages`，每个阶段值为原始回执数组。换图另填 `recovery_identity`，可填 `target={index,x,y,z,radius}` 核对最终严格停点。资产有改动时另建证据。

```powershell
py -B .\waypoint-workflow\verify_lifecycle.py (Join-Path $sessionDir 'lifecycle-evidence.json') `
  --output (Join-Path $sessionDir 'lifecycle-report.json')
```

报告路径须位于 TEMP 且尚不存在。验证器离线重算七类场景，依实际事件、身份和资产绑定判定提交的样本。

## 清障回归（M3b）

适用于验证既有手工路线的枪刀清障、续路及中断。加载 `agpb_waypoint_probe`、`agpb_behavior_fixture` 和 `agpb_clearance_probe`，用 `sm exts list` 确认 SDKHooks 已加载。

1. 每个场景先踢除并确认零 bot，再重载地图恢复原生物件。加入同队 bot 并关闭 auto 和 roam。测试 bot 自主走到起点后准备代表武器。需移动物件时在准备阶段移动已有原生物件，读回实体身份、坐标和实际 HP，保留原 HP。
2. 锁定待观测物件，确认 `damage_hooked=1`，再给一次单程目标。分别记录正反方向实际路线。沿途由核心完成移动、清障和到达，观察期间禁止重发目标、传送、人工攻击或移除物件。
3. 有限退出或实际中断后取 `quiet`，检查输入、任务、路线、break/use 已清理，`attackcmds` 不再增长。按生命周期的 3 样本、0.8 秒跨度与 1.5 秒最大间隔取样。刀有待结算的 Smack 时，从结算结束后开始安静窗口。

```text
agpb_wp_probe_clearance_watch name radio
agpb_wp_probe_clearance_watch name phone7
agpb_bot_info 0; agpb_bot_clearance 0; agpb_wp_probe_state; agpb_wp_probe_clearance 2
```

`watch name|ent value` 可绑定最多 16 个已有物件。保留完整 SM EntRef、观测 generation 和核心 EHANDLE，以同 tick 的实体编号和类名建立对应。按探针 `(epoch, seq)` 去重并保留新增事件。真实伤害需同身份、武器归因，并有独立 HP 变化，或同 `damage_id` 的伤害上下文与实际 break/destroy 闭环。仅有回显的 damage 值、弹药减少或物件消失不构成通过证据。刀按实际武器及 Smack 判定。

| `kind` | `stages` 顺序 | 通过证据 |
|---|---|---|
| `gun` / `knife` | `before` / `run` | 对应武器真实伤害及破坏，同 owner / goal 自动续路并严格停在原终点 |
| `chain` | `before` / `run` | 至少两个不同原 EntRef 逐件处理，同目标实际续路并到达 |
| `bounded_exit` | `before` / `run` / `quiet` | 六次无确认伤害的攻击或 10 秒预算耗尽后退出，清理输入、任务及路线 |
| `stop_interrupt` | `before` / `run` / `quiet` | 真实攻击且清障仍活动时执行标准 stop，随后无新攻击 |
| `death_interrupt` | `before` / `run` / `quiet` | 活动清障中同身份真实死亡，随后无新攻击 |

捕获短暂攻击状态时，在给目标前执行 `agpb_reg_clearance_arm stop|death bot_slot human_slot core_idx`。夹具要求唯一 AgPB、活真人同队和 `core_idx=0`，按双方 userid/serial 绑定。真实 `weapon_fire` 后通过 RequestFrame 先打印四项快照，再执行标准 stop 或真实死亡。核对 `armed/triggered/before/done` 的 tick 和身份，以及动作前实际存在的清障状态。到第 10 游戏秒仍未完成，或清除、卸载、换图、断线、身份条件失效时取消。点控期间先结束冻结。

证据格式沿用 `schema_version: 1`、同组玩家身份及原始回执约定。保留同一回执中动作前后的状态块，ACK 外内容保留 `observed_log` 和连续字节 offset。此处的七项哈希为 `waypoint_snapshot_sha256`、`agpw_sha256`、`bsp_sha256`、`dll_sha256`、`player_probe_sha256`、`fixture_sha256`、`clearance_probe_sha256`。后三项对应实际加载的 SMX。

- `waypoint_snapshot` 保存既有 AGPW 的节点、半径、标志及有向边，标记 `source_kind=existing_agpw_readonly_observation`、`manual_authorship=false`，用 `source_sha256` 绑定原 AGPW。可用 `snapshot_target(rawbytes, metadata, index)` 读取终点。
- 每个 case 带 `kind`、相同七项哈希的 `binding`、`goal`、原始 `target_refs` 及 `stages`。成功场景另填 `target={index,x,y,z,radius}`。`bounded_exit` 补 `expected_exit=timeout|no_damage_budget`。各场景的新玩家身份可写入 case 的 `identity`，资产哈希保持一致。

```powershell
py -B .\waypoint-workflow\verify_clearance.py (Join-Path $sessionDir 'clearance-evidence.json')
```

验证器离线重算提交的样本，六类都通过时退出 0。场景不齐或证据不足时输出 `pending_kinds` 并退出 1。完成后先 `agpb_reg_clear`，再用 `agpb_wp_probe_clearance_clear` 清观测，按[主流程](../README.md)收尾。
