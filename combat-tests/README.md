# 战斗验证

本目录负责普通模式战斗闭环、难度瞄准采样及原生 bot 对手短测。规则见 [AGENTS.md](../AGENTS.md)，命令见 [COMMANDS.md](../COMMANDS.md)，状态与验收结果见 [PROGRESS.md](../PROGRESS.md)。

## 工具

| 文件 | 用途 |
|---|---|
| [verify_combat.py](verify_combat.py) | 离线重算原四类最小战斗证据，或显式选择独立地面战斗移动 / 续路 / 停止样本契约 |
| [test_verify_combat.py](test_verify_combat.py) | 战斗证据验证器的离线回归 |
| [fixtures/agpb_combat_probe.sp](fixtures/agpb_combat_probe.sp) | 只读玩家身份、武器、射线及真实开火 / 伤害 / 死亡事件 |
| [fixtures/agpb_native_match_probe.sp](fixtures/agpb_native_match_probe.sp) | 只读多对手交锋事件、装备与计数，供探索性短测使用 |
| [fixtures/agpb_locomotion_fixture.sp](fixtures/agpb_locomotion_fixture.sp) | 一次性准备装备 / 位置，控制目标显隐，以及有限地面交战取样；主体执行仍由本地核心负责 |

控制台传输复用 [Invoke-GameConsole.ps1](../waypoint-workflow/Invoke-GameConsole.ps1)；身份与停点判断复用既有纯离线解析模块。agent 意图接口与客户端的用法集中在 COMMANDS。

## 实机取样

常规测试遵守同队要求。敌对场景须单独获得用户明确授权，并在准备、取样及收尾核对匹配引擎的 `sm_agpb_guard_status` 为 `active=1 intact=1`。切换场景先踢 bot；准备前记录设置原值、实际身份与资产哈希。

夹具用部署目标下 `addons/sourcemod/scripting/spcomp64.exe` 编译到本次系统 TEMP 子目录；编译成功后放 `addons/sourcemod/plugins/disabled/` 并手工加载。已有同名插件先核对加载状态及卸载，避免覆盖占用文件。两个观察器均不准备场景、修改伤害或控制 bot。

单个受控目标的示例，slot / idx 使用本次实际值：

```text
sm plugins load disabled/agpb_combat_probe
agpb_wp_probe_combat_watch 2 1
agpb_bot_info 0; agpb_bot_combat_info 0; agpb_wp_probe_combat; sm_agpb_guard_status
agpb_wp_probe_combat_clear
```

主体为与低 slot 活真人同队的存活 ACTIVE bot；已授权敌对场景可用 watch 的第三参数绑定一个既存目标 fake 的 slot。

两或三名对手的探索性短测另用：

```text
sm plugins load disabled/agpb_native_match_probe
agpb_match_watch 2 1
agpb_match_state
agpb_match_clear
```

以真实事件、userid / serial、地图 / 回合、探针 epoch / seq 核对归因。攻击命令、弹药减少、射线畅通和缓存重放都不能单独证明新伤害；固定装备短测的结果仅覆盖该次交锋。

## 离线复核

证据保留完整原始回执及 ACK 外事件，绑定计划、AGPW、BSP、DLL、实际探针 SMX 与 server 二进制哈希；记录授权、guard 状态与预期 CRC。每个 case 保留身份及原始 stages，字段约定见验证器模块说明和测试样本。

```powershell
py -B .\combat-tests\verify_combat.py (Join-Path $sessionDir 'combat-evidence.json')
py -B .\combat-tests\test_verify_combat.py
```

正向续路记录须同时证明真实伤害、战斗期间原 owner / goal / hop 保留、失敌后无新 goto 自动续路；停止记录须跨 tick 确认输入释放及无新攻击。缺证据保留 pending。验证器只认证提交的采样。

### 地面战斗移动样本

`agpb_locomotion_fixture` 仅用于已授权的受控敌方场景。准备前停止两个核心，原始快照确认主体释放输入；绑定活着、ACTIVE 的主体 / 低 slot 同队真人 / 敌方 fake 三个身份。`agpb_loco_prepare` 一次设置主体位置、枪械、弹药与双方测试生命值，随后仅目标输入受锁；`agpb_loco_hide` / `agpb_loco_show [x y z yaw]` 改变目标位置并打印真实读回。运行期不写主体位置、视角、血量或弹药。

`agpb_loco_sample core_index delay count interval` 定时执行四条只读诊断。完整闭环使用 `agpb_bot_goto core_index goal; agpb_loco_sequence core_index nonce reShowX reShowY reShowZ reShowYaw`：在真实导航后取 before，再按目标显隐取 run / resumed / reengaged，最后通过唯一一次原生 stop 取 quiet，共 26 组快照。阶段打印真正的 `AGPB_SESSION_BEGIN_<nonce><phase>` / END，保留原始调用、实际执行命令与全部 ACK 外字节；`agpb_loco_clear` 清理绑定并取消定时取样。准备参数完整语法由 `agpb_loco_prepare` 无参数回显。

旧证据默认使用 `legacy_minimum_combat_v1`；原 `visible_damage_resume` 仍要求战斗拥有期间实际三轴移动命令为零，不因新增移动行为放宽。新样本在 metadata 显式设置 `acceptance_contract: "sampled_combat_locomotion_v1"`，只放独立 `kind: "locomotion_damage_resume_stop"` case。该契约认证一份明确场景的样本即可，不要求同时提交旧四类或全部进退 / 横移 / 停枪矩阵。

每个新 case 保留原有六项 binding 与身份字段，另需 `goal`、`locomotion_policy: "sypb_ground_v1"` 及以下场景边界（示例值须换成本次实际范围）：

```json
{"locomotion": {"expected_motion": "approach", "weapon_class": "weapon_m4a1", "distance_range": [250, 700], "movement_context": "ground_walk"}}
```

`expected_motion` 选 `approach`、`retreat`、`strafe` 或 `precision_stop`；`distance_range` 是主体与目标实际 3D 距离的闭区间，单位为游戏单位，两个数须有限且满足 `0 <= min < max <= 8192`。该边界与当前武器类必须和每份拥有目标的快照一致，不能以场景标签代替采样。它只核对所声明动作确实执行，不证明该武器或距离下的决策规则已全面正确。

| stage | 必需原始证据 |
|---|---|
| `before` | 唯一一次原生 goto、当前 route / owner / goal / hop / node 与准备后实际身份 |
| `run` | 同一目标被战斗拥有；至少三份严格递增快照，跨至少 0.8 秒、相邻至多 1.5 秒；原 route / owner / goal / hop / node 保留；新真实开火及归属一致、独立 HP 降低的伤害 |
| `resumed` | 真实失敌 / 死亡 / 原生释放反馈；至少两份存活原 route 的运动快照，owner / goal 保留；不重发 goto |
| `reengaged` | 同一目标重新被拥有；保留恢复后那一刻的 route / owner / goal / hop / node；再次满足 `run` 的地面动作采样窗口，随后执行 stop |
| `quiet` | 明确原生 `agpb_bot_stop`，至少三份跨 0.8 秒快照；route / task / combat 清理，实际 CUserCmd 按键与三轴移动全释放，无新增真实开火 / 弹药变化 |

移动窗口使用实际 lifecycle `move` / `view` 将 CUserCmd 转成世界方向，再对照跨 tick 的玩家位置。进退须有朝向目标 / 背离目标的命令及至少 4 单位对应位移；横移须有侧向命令及至少 4 单位侧向位移。仅有非零命令、速度值或目标自身移动不能通过。命令 XY 长度不超过 450，位移不超过 `400 * dt + 4`，相邻位置高度差不超过 18；这些是本次地面取样边界，不扩展为跳跃 / 梯子 / 空中验收。`precision_stop` 要求三轴命令归零、实际速度至多 5、整个窗口位置在首样本 4 单位内，并有提交的主攻击命令。窗口均须接地、非冻结、无 jump / upmove / pending velocity；不允许用 setpos、noclip 或调试移动命令制造位移。

真实伤害、完整句柄身份、guard、资产哈希、原始回执、无友伤与无替代 goto 等原有断言继续生效。`run` 的伤害不能由后续重新交火替代；stop 在 `reengaged` 的战斗拥有后执行。进退 / 横移 / 停枪的其它场景、穿透与敌情调查均需另行取样，本契约不认证这些未提交范围。

精确停枪可用窗口内采到的 IN_ATTACK，或过滤后已提交攻击计数 `attackcmds` 的严格增长，补证定时快照可能漏掉的单帧按键。两段停稳窗口还各自要求身份 / 武器匹配的原生开火，以及由该窗 HP 样本独立夹证的真实伤害，均须落在首尾快照的 tick 范围内；窗口外或 resumed 的伤害不可替代。速度、位移及零移动命令限制保持不变。

结束时清观测、踢本轮 bot，恢复并独立读回设置，卸载本轮夹具；证据交付到被 ignore 的 build 输出目录。临时资料仅列完整路径供用户自行删除。

## 5v5 观战演示

用户明确授权双方交战及主机观察者后，可使用 [agpb_showmatch.sp](fixtures/agpb_showmatch.sp)。先在零 bot 时让主机 `jointeam 1`，核验 Radio guard 为 `active=1 intact=1`；此演示例外不改变常规同队测试要求。夹具按上文编译到 TEMP，手动加载 `disabled/agpb_showmatch`，以 `sv_cheats 1` 和 `agpb_showmatch_enable 1` 显式启用。

CT 以 `agpb_difficulty 4` 创建五名 AgPB，T 以 `bot_difficulty 3` 和五次 `bot_add_t expert` 创建原生专家。观察者场景须设 `bot_join_after_player 0`、`bot_quota_mode normal`、`bot_auto_vacate 0`，关闭自动平衡及人数差限制；五次原生 add 后配额应为 5，不包含 AgPB。`bot_loadout "m4a1 vesthelm primammo"` 固定原生购买路径。双方 AI 保持开启，AgPB 默认使用本地 normal 自主行为。用户明确要求分区时，可选协调器只通过既有 scoped camp 意图指定手工路点目标；绑定地图及玩家身份，跨回合重新提交，停止续期后按 TTL 回退本地核心，不接管逐 tick 行为。

夹具仅在出生及回合准备阶段发放 M4A1、30/90 弹药、100 甲和头盔，并通过原生 `use` 命令选中 M4；保留 C4、地图目标与其它物品，不在交火中补弹、补甲或改伤害。`agpb_showmatch_state` 核对双方各五名 fake、真人 `team=1`、实际主副武器与装备，`agpb_bot_combat_info all` 核对五名 `difficulty=4 skill=100`；guard 完整性另读 `sm_agpb_guard_status`。`agpb_showmatch_focus <human_slot> <living_bot_slot>` 切第一人称，目标 0 恢复先前视角；事件及比分只作本次演示记录，不等于完整对局强弱验收。

结束时先停止可选分区协调器，再 `agpb_kick all; bot_kick`，独立核对零 fake，再 `agpb_showmatch_enable 0; sm plugins unload disabled/agpb_showmatch` 并恢复本次记录的设置原值，独立读回后按用户要求退出游戏。演示期间不换图或切主机队伍。临时资料仅列完整路径供用户自行删除。


## 已有武器选择

纯策略由 `tools/test_weapon_selection.cpp` 独立验证偏好、弹药 / 距离、等待确认、冷却、失败重试与状态重置。实机准备继续使用停住 bot 的 `agpb_bot_testweapon`，只在准备阶段用诊断入口设置弹药；运行时不可补弹或代替 AI 切枪。

同队冒烟用 `agpb_bot_weapon_info` 核对完整武器句柄、owner 对应的库存、弹夹 / 备弹和 pending；开启 `agpb_weapons_debug 1` 记录实际提交的 CUserCmd 与装备读回。应覆盖有弹无备弹切枪、空闲换弹、蹲行段切枪、原路线 / owner 续行和 stop 后跨 tick 释放输入。真实交战的备枪替换、距离选择与反复切换需另取样，不能由同队冒烟代替。

## 本地自主与控制层回归

`src/local_planner.h`、`src/search_policy.h` 与实际 `src/tasks.cpp` 的独立 C++ 回归分别由 `tools/test_local_planner.cpp`、`tools/test_enemy_search.cpp`、`tools/test_agent_intention.cpp` 覆盖。客户端 `tools/test_agpb_agent.py` 覆盖清障子任务期间的续租。编译产物放系统 TEMP；完整 DLL 构建及头文件消费者重编仍遵守 AGENTS。

无敌方的同队冒烟可以核对多 bot 自主选点、条件驻守 / 观察方向及短 TTL 高层意图回退；使用 `agpb_bot_tasks`、`agpb_bot_info`、`agpb_agent_state` 和 guard 回执交叉核验。开火 / 脚步调查、丢视野后搜索及紧急目标下高层执行的验收需要独立的已授权敌对场景，离线策略通过不能替代这些实机检查。
