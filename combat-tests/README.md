# 战斗验证

本目录负责普通模式战斗闭环、难度瞄准采样及原生 bot 对手短测。规则见 [AGENTS.md](../AGENTS.md)，命令见 [COMMANDS.md](../COMMANDS.md)，状态与验收结果见 [PROGRESS.md](../PROGRESS.md)。

## 工具

| 文件 | 用途 |
|---|---|
| [verify_combat.py](verify_combat.py) | 离线重算同队拒绝、遮挡拒绝、真实伤害后续路、开火中停止四类证据 |
| [test_verify_combat.py](test_verify_combat.py) | 战斗证据验证器的离线回归 |
| [fixtures/agpb_combat_probe.sp](fixtures/agpb_combat_probe.sp) | 只读玩家身份、武器、射线及真实开火 / 伤害 / 死亡事件 |
| [fixtures/agpb_native_match_probe.sp](fixtures/agpb_native_match_probe.sp) | 只读多对手交锋事件、装备与计数，供探索性短测使用 |

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

结束时清观测、踢本轮 bot，恢复并独立读回设置，卸载本轮夹具；证据交付到被 ignore 的 build 输出目录。临时资料仅列完整路径供用户自行删除。

## 5v5 观战演示

用户明确授权双方交战及主机观察者后，可使用 [agpb_showmatch.sp](fixtures/agpb_showmatch.sp)。先在零 bot 时让主机 `jointeam 1`，核验 Radio guard 为 `active=1 intact=1`；此演示例外不改变常规同队测试要求。夹具按上文编译到 TEMP，手动加载 `disabled/agpb_showmatch`，以 `sv_cheats 1` 和 `agpb_showmatch_enable 1` 显式启用。

CT 以 `agpb_difficulty 4` 创建五名 AgPB，T 以 `bot_difficulty 3` 和五次 `bot_add_t expert` 创建原生专家。观察者场景须设 `bot_join_after_player 0`、`bot_quota_mode normal`、`bot_auto_vacate 0`，关闭自动平衡及人数差限制；五次原生 add 后配额应为 5，不包含 AgPB。`bot_loadout "m4a1 vesthelm primammo"` 固定原生购买路径。双方 AI 保持开启，AgPB 默认使用本地 normal 自主行为。用户明确要求分区时，可选协调器只通过既有 scoped camp 意图指定手工路点目标；绑定地图及玩家身份，跨回合重新提交，停止续期后按 TTL 回退本地核心，不接管逐 tick 行为。

夹具仅在出生及回合准备阶段发放 M4A1、30/90 弹药、100 甲和头盔，并通过原生 `use` 命令选中 M4；保留 C4、地图目标与其它物品，不在交火中补弹、补甲或改伤害。`agpb_showmatch_state` 核对双方各五名 fake、真人 `team=1`、实际主副武器与装备，`agpb_bot_combat_info all` 核对五名 `difficulty=4 skill=100`；guard 完整性另读 `sm_agpb_guard_status`。`agpb_showmatch_focus <human_slot> <living_bot_slot>` 切第一人称，目标 0 恢复先前视角；事件及比分只作本次演示记录，不等于完整对局强弱验收。

结束时先停止可选分区协调器，再 `agpb_kick all; bot_kick`，独立核对零 fake，再 `agpb_showmatch_enable 0; sm plugins unload disabled/agpb_showmatch` 并恢复本次记录的设置原值，独立读回后按用户要求退出游戏。演示期间不换图或切主机队伍。临时资料仅列完整路径供用户自行删除。


## 本地自主与控制层回归

`src/local_planner.h`、`src/search_policy.h` 与实际 `src/tasks.cpp` 的独立 C++ 回归分别由 `tools/test_local_planner.cpp`、`tools/test_enemy_search.cpp`、`tools/test_agent_intention.cpp` 覆盖。客户端 `tools/test_agpb_agent.py` 覆盖清障子任务期间的续租。编译产物放系统 TEMP；完整 DLL 构建及头文件消费者重编仍遵守 AGENTS。

无敌方的同队冒烟可以核对多 bot 自主选点、条件驻守 / 观察方向及短 TTL 高层意图回退；使用 `agpb_bot_tasks`、`agpb_bot_info`、`agpb_agent_state` 和 guard 回执交叉核验。开火 / 脚步调查、丢视野后搜索及紧急目标下高层执行的验收需要独立的已授权敌对场景，离线策略通过不能替代这些实机检查。
