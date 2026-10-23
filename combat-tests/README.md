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
