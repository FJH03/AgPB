# 路点制作与验收

可复用入口集中在本目录。项目约束见 [AGENTS.md](../AGENTS.md)，游戏命令见 [COMMANDS.md](../COMMANDS.md)，当前进度和结果只写 [PROGRESS.md](../PROGRESS.md)。

## 保留的工具

| 文件 | 用途 |
|---|---|
| [Invoke-GameConsole.ps1](Invoke-GameConsole.ps1) | 向已运行游戏发送原生控制台命令，取得本次回执及TGA/PNG截图 |
| [ConsoleTransport.ps1](ConsoleTransport.ps1)、[convert_tga.py](convert_tga.py) | 控制台回执与完整截图读取；由主入口调用 |
| [export_manual_plan.py](export_manual_plan.py) | 将明确人工决定的节点和有向边导出为AGPW |
| [audit_waypoints.py](audit_waypoints.py) | `compare`核对字段与分队有向连通，`coverage`核对实走覆盖，`verify-bundle`核对交付哈希 |
| [verify_routes.py](verify_routes.py)、[stop_policy.py](stop_policy.py) | 同队实走记录与严格终点判定 |
| [verify_objectives.py](verify_objectives.py) | A/B真实安放、逃爆及拆包场景记录 |
| [verify_lifecycle.py](verify_lifecycle.py) | 离线重算M3a生命周期采样、身份、真实事件与版本绑定 |
| [verify_clearance.py](verify_clearance.py) | 离线重算M3b清障、续路、有限退出及攻击中断证据 |
| [fixtures/agpb_waypoint_probe.sp](fixtures/agpb_waypoint_probe.sp) | 可选只读玩家 / C4状态及真实事件探针 |
| [fixtures/agpb_clearance_probe.sp](fixtures/agpb_clearance_probe.sp) | 独立只读物件 / 武器及SDKHooks真实伤害事件探针 |
| [fixtures/agpb_behavior_fixture.sp](fixtures/agpb_behavior_fixture.sp) | 真人场景准备及显式cheat-gated生命周期准备夹具 |
| [maps/de_dust2.plan.json](maps/de_dust2.plan.json) | 可重建正式de_dust2路点的人工计划 |
| `test_*.py`、`test_console_transport.ps1` | 离线回归：导出、回执、身份、版本、停稳、目标及清单判定 |

Python工具使用标准库；截图转换另需已有Pillow，不自动安装依赖。临时CFG、截图、日志、记录及SMX均放系统TEMP；交付产物放被ignore的`build/waypoint-output/`。

## 制作

1. 操作前后检查`git status --short`。从AGENTS读取游戏目录，记录地图、模式、DLL/BSP/AGPW哈希及待修改设置的原值；已有AGPW先备份到本次TEMP会话。
2. 检查现有游戏进程，使用对应地图。换场景前先`agpb_kick all`，确认零bot；真人存活、ACTIVE、slot小于bot，测试bot始终与真人同队。
3. 分区核实真实通路，逐条决定点、半径、标志和有向边。de_dust2覆盖T后场、A大 / 坑、A小、中路、上下B洞、B区 / 窗口 / 后场、CT连接及A/B目标区；不同高度层分别布点。
4. 导出候选并`compare`；备份后部署候选，通过`agpb_wp_load`读取。普通往返、跳落、窄口、门、分支和目标区逐段正反实走，按实际缺失方向补测。
5. 每区通过后原生`agpb_wp_save`，确认成功再`agpb_wp_load`并比较字段。save直接覆盖文件，clear仅清内存，均不能代替备份 / 撤销。删点会重排后续编号。

```powershell
$plan = '.\waypoint-workflow\maps\de_dust2.plan.json'
$session = 'agpb_wp_dust2_session01'   # 每次使用新的会话名
$sessionDir = Join-Path $env:TEMP $session
New-Item -ItemType Directory -Path $sessionDir -ErrorAction Stop | Out-Null
$candidate = Join-Path $sessionDir 'de_dust2.agpw'
py -B .\waypoint-workflow\export_manual_plan.py $plan $candidate
if ($LASTEXITCODE -ne 0) { throw 'Plan export failed' }
py -B .\waypoint-workflow\audit_waypoints.py compare $plan $candidate
if ($LASTEXITCODE -ne 0) { throw 'Plan comparison failed' }
```

人工计划必须显式填写每点label / XYZ / radius / flags和每条有向边from / to / flags，反向边也单列。NAV/BSP仅作只读参考；不导入NAV图、不按几何自动连边或补跳。几何体检提示不能直接改图；弱连通、0 isolated和路线穿行不能代替分队有向可达或真实行为验收。

编辑细节按COMMANDS执行：已有A→B时，`link A B`不保证补反向边，须list核对；缓存B后回A再connect。jump为有向动作；每点最多8个出边。flag是切换操作，T/CT位是队伍通行限制，并非出生标签。不要扩大半径掩盖卡点。`agpb_wp_path`默认忽略队伍限制，须另用实际同队bot验证。

## 控制台与观察

只用普通shell及游戏原生控制台，不使用computer-use；截图只用TGA，禁用`jpeg`。

```powershell
.\waypoint-workflow\Invoke-GameConsole.ps1 -SessionName $session `
  -SessionDirectory $sessionDir -Commands 'agpb_list; agpb_wp_list 0' -Screenshot overview
```

单个游戏只由一个操作者写入。`-Listen`逐行接收`{"commands":"agpb_list","delay":500}`，`END`仅关闭helper。BEGIN/END表示本次CFG执行到标记；READY表示helper可以接收请求。地图加载、出生、换队及导航结果必须另读状态；超时先核实，避免重复有副作用命令。回执和截图分别受`-AckTimeoutSeconds`、`-ScreenshotTimeoutSeconds`限制。

`agpb_wp_nearest`读脚底，本游戏`getpos`可能读视点；布点须落地核对Z。真人可用noclip观察，但其穿越轨迹不能证明bot可通，真人实体也应远离bot路线。把`+forward`等输入写入CFG，区别于启动参数`+exec`；每次按下均要释放，停止或中断时释放全部本轮按键。面板可用`hidepanel all; hideconsole; gameui_hide`收起。

## 实走与严格停点

先等待`agpb_list` / probe显示双方实际同队、存活、武器就绪，再关闭bot自主任务。下列idx / slot只是示例，使用本次实际值；无probe时可人工逐段验收，不生成缺少真实采样的自动通过结果。

```powershell
.\waypoint-workflow\Invoke-GameConsole.ps1 -SessionName $session `
  -SessionDirectory $sessionDir -Commands 'agpb_list; agpb_wp_probe_state; agpb_bot_auto 0 0'
py -B .\waypoint-workflow\verify_routes.py --plan $plan `
  --session-name $session --session-directory $sessionDir `
  --bot-index 0 --bot-slot 2 --human-slot 1 --team CT `
  --targets C_spawn --require-precise-stop --output (Join-Path $sessionDir 'walk-01.json')
```

目标必须是计划中的label；单个目标只作链路检查，完整验收需每个节点分别作为终点，并另核对全部有向边。实走器不启停游戏、不加bot、不换队、不传送、不改路点；bot运动由AgPB的CUserCmd执行。失败 / 中断会尽力停bot；失败与重试使用不同输出文件。

严格终点政策：XY≤max(原radius,4)、Z误差≤4、接地、速度≤5，最终连续窗口≥0.8秒、至少3份样本、相邻间隔≤1.5秒。原始半径不改，中间点只验穿行；缺字段不能当零速度，缺新政策版本的旧记录不追认证。

```powershell
# 用本次实际文件路径替换占位符；多个路线记录依次列出。
py -B .\waypoint-workflow\audit_waypoints.py coverage $plan (Join-Path $sessionDir 'walk-01.json') `
  --require-precise-stop --agpw '<正式AGPW>' --bsp '<当前BSP>' --dll '<当前DLL>'
```

记录绑定计划及AGPW/BSP/DLL哈希，混合版本拒绝合并；改点、半径、标志、边或顺序后重验。`endpoint_coverage`分别统计请求终点及严格通过终点；`precise_stop_certified`只认证已提交样本，不能单独证明全图。`--allow-legacy`仅显式降级；路线被截断时另给`--console-log`，不据旧证据认证新版本。任何audit非零退出均需查看具体报告，制作中缺覆盖而退出1是未完成状态。

## 真实地图目标

自动目标记录需手工加载两份可选SourceMod夹具；AgPB核心不依赖SourceMod。没有SourceMod时保留原生命令与真实事件日志作人工验收，缺可靠状态标为未验证。使用游戏既有编译器，输出放TEMP，检查编译退出码。同名SMX已存在时先核对并卸载，不覆盖占用文件。

```powershell
$requirements = Get-Content .\AGENTS.md -Raw -Encoding utf8
$gameDir = [regex]::Match($requirements, '(?m)^- 部署目标：\s*`([^`]+)`').Groups[1].Value
if (!$gameDir) { throw 'Deployment target missing' }
$scripting = Join-Path $gameDir 'addons\sourcemod\scripting'
foreach ($name in @('agpb_waypoint_probe', 'agpb_behavior_fixture')) {
  & (Join-Path $scripting 'spcomp64.exe') ('.\waypoint-workflow\fixtures\' + $name + '.sp') `
    ('-i' + (Join-Path $scripting 'include')) ('-o' + (Join-Path $sessionDir ($name + '.smx')))
  if ($LASTEXITCODE -ne 0) { throw ('Fixture compilation failed: ' + $name) }
}
```

确认后将SMX放`addons/sourcemod/plugins/disabled/`，避免启动时自动加载；通过控制台执行：

```text
sm plugins load disabled/agpb_waypoint_probe
sm plugins load disabled/agpb_behavior_fixture
agpb_wp_probe_state
agpb_wp_probe_bomb
```

probe只读玩家 / C4状态、真实炸弹及生命周期事件。炸弹验收先记`bomb_planted` / `bomb_defused` / `bomb_exploded`计数基线；按新增事件、userid、同回合、同颗C4验收。缺属性为-1，不能当0；爆炸后须另取更晚tick的同身份存活样本。C4原点在爆炸时可能变化，只在ticking阶段要求位置固定。

夹具只作场景准备，全部准备命令要求`sv_cheats 1`，不伪造物品或事件。下列真人接口仍拒绝fake client；bot生命周期准备另走下节的显式命令。

| 命令 | 作用 |
|---|---|
| `agpb_reg_place_human slot x y z yaw` | 摆放真人 |
| `agpb_reg_human_plant slot` | 真人须已持真实C4、位于真实包区，通过真人CUserCmd安放 |
| `agpb_reg_drop_c4 slot x y z` | 真人正常丢弃已有C4后摆放落地武器 |
| `agpb_reg_human_team slot T\|CT` | 零playing fake时换真人队并重生 |
| `agpb_reg_clear` | 取消夹具请求并恢复存活点控对象，随后读回确认 |

记录器要求游戏及地图已就绪、两夹具已加载。记录设置原值后核对`sv_cheats 1`、`bot_quota 0`、`mp_ignore_round_win_conditions 1`并关闭自动平衡 / 闲置踢人；用`map_showbombradius`及`agpb_objectives`核对伤害半径与来源，运行期改值须同步override。掉包应离bot出生点有足够距离，避免丢失真实落地采样。

```powershell
py -B .\waypoint-workflow\verify_objectives.py --plan $plan `
  --session-name $session --session-directory $sessionDir `
  --mode T_escape --site A --name objective-T-A-01
```

依次运行A/B × `T_escape`、`CT_escape`、`CT_defuse`六项，使用不同`--name`。默认场景标签 / 位置用于de_dust2，其它地图显式设置CLI场景参数。每项先踢bot、读回零bot后准备真人队，再创建同队bot。

| 项目 | 通过证据 |
|---|---|
| 拾包、携包接近 | 真实无人持有的落地C4变为bot所有；bot沿手工路线到指定区域并仍持包 |
| A/B安放 | 新真实安放事件userid属于bot，planted_c4正在计时且位置在实际包区 / GOAL邻域 |
| A/B拆包 | 真人真实安放后，在零bot时切CT并加入同队bot；新拆包事件属于bot，同颗C4停表、未爆炸 |
| T/CT逃爆 | 实际撤离、真实爆炸、同身份且同回合的爆炸后存活；剩时客观不足的场景单列 |

`task complete`、GOAL标志、夹具armed回显或JSON的passed都不能单独证明目标完成。指定路线安放只认证指定路线，出生附近拾包只认证该场景。安放后关auto、提前结束回合或删炸弹不能认证逃爆；未知 / 超时 / 身份矛盾如实记未通过。拆包确认最多15秒游戏时间、60秒墙钟时间，分别记录触发时钟。

## 生命周期回归（M3a）

此流程检查手动导航的停止、冻结及生命周期清理；验收结果另写PROGRESS，不由工作流说明宣告通过。使用上节编译 / 加载的两份夹具；记录设置原值并保持真人存活、ACTIVE、与所有playing bot同队，关闭测试bot的auto / roam。所有行为输入仍由核心CUserCmd执行。

每份状态回执同时执行下列只读命令，idx以本次实际值为准：

```text
agpb_bot_info 0; agpb_wp_probe_state; agpb_wp_probe_lifecycle
```

`agpb_bot_info [idx|all]`默认全部；`cmdnum/cmdtick/buttons/move/view`是最近实际提交的CUserCmd，可能早于当前tick。变更后跨至少两个tick再采样；同阶段样本须按游戏时间、tick及命令序号递增。`stopped` / `frozen`至少3份样本、连续≥0.8游戏秒、相邻间隔≤1.5秒。每个场景的`before`须采到有效路线及owner / goal，非换图过程保留同客户端身份；停止须在运动且朝向明显非零时触发。

| kind | stages（依次） | 核对内容 |
|---|---|---|
| `stop` | `before` / `stopped` | `agpb_bot_stop`后任务 / 路线清理、实际输入归零、朝向保持 |
| `freeze_resume` | `before` / `frozen` / `resumed` | 真FL_FROZEN期间释放输入并保留路线归属，解除后无新goto自动续走 |
| `freeze_settle` | `before` / `frozen` / `resumed` | 冻结前采到`0≤settleage<0.8`的有效计时，冻结清计时，解除后重新完整计时并真实到达原goal |
| `death_respawn` | `before` / `dead` / `respawned` / `recovered` | 同身份真实死亡 / 出生事件与死活状态、清理；再显式goto原goal并实际移动 |
| `team` | `before` / `changed` / `returned` / `recovered` | 核心`agpb_bot_team`切观察者再回原测试队，真实队伍事件 / 清理；重新goto原goal |
| `round` | `before` / `cleared` / `recovered` | 原生`endround`、真实回合事件 / 代次及清理；下一回合重新goto原goal |
| `map` | `before` / `absent` / `recovered` | 先踢零bot，去不同地图且保持零bot，再回原图；新身份首帧干净后goto原goal |

冻结准备使用`agpb_reg_lifecycle_freeze bot_slot human_slot seconds [after_seconds]`，持续0.5..15秒、延迟0..5秒，watchdog每0.1秒检查。它给指定bot启用带有限wait的`point_viewcontrol`，不写冻结netvar。`freeze_armed`只表示请求；须观察`freeze_begin`的`camera_owner=bot_slot`、`frozen=1`、`view_ent=camera`，并以核心和probe独立采样确认。核心CSS原始`m_fFlags`的冻结位为64，SourceMod标准化flags的冻结位为32，不能直接比较整数字段。解除后须采到`frozen=0`及玩家视角恢复；冻结和续行阶段不插入goto / stop / auto / roam。

`agpb_reg_lifecycle_kill bot_slot human_slot`、`agpb_reg_lifecycle_respawn bot_slot human_slot`只准备真实死亡 / 重生。点控结束且probe确认恢复后，才可死亡、换队或卸载夹具；引擎Disable只给存活目标恢复控制，不组合冻结与强制死亡。换真人队时先用核心命令把所有playing bot转观察者并读回零playing fake，再`agpb_reg_human_team`换真人，最后让bot加入真人当前队；全程不得出现playing bot与真人敌对。`agpb_reg_lifecycle_team ... SPEC|SAME`仅为场景准备，不能代替核心换队回归。换图仍须先`agpb_kick all`。

保存完整原始console及每次BEGIN/END回执；ACK外真实事件可追加在`observed_log`，原`log`及ACK不剪裁、不改写。probe记录真实death / spawn / team / round事件及map forward，查询会重放最近64条缓存；按`(epoch,seq)`去重，`replay=1`不能算新事件。事件必须晚于基线且属于目标userid / serial / 地图代次；`player_team`事件的目标队看`newteam`，再由后续STATE确认实际队伍，不能仅凭计数增长归因。

证据JSON为`schema_version: 1`，包含`metadata`与`cases`。metadata记`map`、`bot_slot/bot_userid/bot_name`、`human_slot/human_userid`，以及`plan_sha256`、`agpw_sha256`、`bsp_sha256`、`dll_sha256`、`probe_sha256`、`fixture_sha256`六项实际文件哈希；后两项绑定加载的SMX。每个case的`binding`重复同六hash，`kind`及`stages`按上表，每阶段值为原始回执数组。换图case另记`recovery_identity`的新bot身份；可选`target={index,x,y,z,radius}`追加最终严格停点核对。修改DLL、路点、计划或SMX后新建证据，不混用版本。

```powershell
py -B .\waypoint-workflow\verify_lifecycle.py (Join-Path $sessionDir 'lifecycle-evidence.json') `
  --output (Join-Path $sessionDir 'lifecycle-report.json')
```

验证器只离线读取，不操控游戏；`--output`必须是尚不存在的TEMP路径。缺字段、身份 / 版本矛盾、缺真实事件或缺七类任一项均不整体通过。报告只认证提供的采样，不证明未采样时段，也不替代全图路点或目标验收。

## 清障回归（M3b）

复用M3a的同队、跨tick及原始回执规则。每个场景先`agpb_kick all`、读回零bot，再重载地图恢复原生物件；真人存活、ACTIVE且slot更小，准备后只加入同队bot，关闭auto / roam。先自主走到起点并准备代表武器，验收单程只给一次目标，不重发路线、不人工攻击 / 删物或传送bot。世界原生物件可在准备阶段移动，移动后读回身份 / 坐标 / 实际HP；不改HP，模型配置不能代替实机HP。正反方向分别记录，不能把A*捷径当成同一路段的反向验收。

原两份夹具的编译 / 加载方法保持不变；攻击中断需重新编译并加载fixture1.2。另按同一TEMP输出规则编译独立探针：

```powershell
& (Join-Path $scripting 'spcomp64.exe') '.\waypoint-workflow\fixtures\agpb_clearance_probe.sp' `
  ('-i' + (Join-Path $scripting 'include')) ('-o' + (Join-Path $sessionDir 'agpb_clearance_probe.smx'))
if ($LASTEXITCODE -ne 0) { throw 'Clearance probe compilation failed' }
```

按上节规则放入disabled并执行`sm plugins load disabled/agpb_clearance_probe`，用`sm exts list`核对SDKHooks加载成功；它只是可选诊断依赖，AgPB核心不依赖它。绑定物件后`damage_hooked=1`才有伤害钩子证据，属性缺失保持unknown。示例name / idx / slot均须替换为本次实际值：

```text
agpb_wp_probe_clearance_watch name radio
agpb_wp_probe_clearance_watch name phone7
agpb_bot_info 0; agpb_bot_clearance 0; agpb_wp_probe_state; agpb_wp_probe_clearance 2
```

`watch name|ent value`最多锁定16个已有物件，完整SM EntRef及观测generation不追随复用idx；`agpb_wp_probe_clearance slot`同时查询目标、武器、计数和64条事件缓存，`agpb_wp_probe_clearance_clear`仅取消观测。核心raw EHANDLE与SM EntRef各自完整保留，通过同tick的ent / class快照建立对应关系，不能直接比较两种句柄整数。按探针自身`(epoch,seq)`去重缓存，保留所有新增序号。真实伤害须有攻击者身份 / 武器归因及HP变化，或同damage_id的伤害上下文与真实break / destroy闭环；输入damage值、弹药减少、单独消失或醒来触发Break均不能单独证明伤害。刀按真实武器与Smack上下文判读，不假定伤害类型必为DMG_SLASH。

| kind | stages | 核对内容 |
|---|---|---|
| `gun` | `before` / `run` | 真实枪械伤害 / 破坏后，同owner / goal自动续路并严格停于原终点 |
| `knife` | `before` / `run` | 真实刀伤害 / 破坏及同目标续路、严格停点 |
| `chain` | `before` / `run` | 至少两个不同原始EntRef逐件处理，不换目标指令，最后实际续路 / 到达 |
| `bounded_exit` | `before` / `run` / `quiet` | 六次无确认伤害攻击或10秒预算内明确退出，清理输入 / 任务 / 路线 |
| `stop_interrupt` | `before` / `run` / `quiet` | 已真实攻击且仍有活动清障状态时标准stop，随后无新攻击 |
| `death_interrupt` | `before` / `run` / `quiet` | 活动清障中的真实死亡及同身份死亡事件，随后无新攻击 |

为捕获短暂攻击状态，可在给目标前执行`agpb_reg_clearance_arm stop|death bot_slot human_slot core_idx`。fixture1.2限定唯一AgPB fake、活真人同队、core_idx=0，绑定双方userid / serial；首次匹配的真实weapon_fire后通过RequestFrame跨tick，先同步打印上面的四项状态，再执行标准`agpb_bot_stop`或ForcePlayerSuicide。记录`armed/triggered/before/done`的真实tick与身份；若核心已释放清障，照实记录，不能据armed或触发承诺认证攻击中断。10游戏秒未完成、`agpb_reg_clear`、卸载、换图、断线或安全条件失效均取消；不与点控冻结重叠。

`quiet`至少3份连续≥0.8游戏秒、相邻间隔≤1.5秒，核对按钮 / 移动、任务 / 路线、break / use清理及`attackcmds`不再增长；刀已排队Smack可延后结算，安静窗口从它结束后计算。原始同回执内可能含动作前后两个状态块，原样保留，由工具按真实tick / 命令序号分期，不剪改或重复提交回执。ACK外事件及自动快照留在`observed_log`并记录连续字节offset。

证据JSON为`schema_version: 1`，metadata记录地图及真人 / bot身份，并绑定`waypoint_snapshot_sha256`、`agpw_sha256`、`bsp_sha256`、`dll_sha256`、`player_probe_sha256`、`fixture_sha256`、`clearance_probe_sha256`七项哈希；后三项为实际加载的SMX。`waypoint_snapshot`明确是既有AGPW只读观测快照：`source_kind=existing_agpw_readonly_observation`、`manual_authorship=false`、`source_sha256`绑定AGPW，保留原节点 / 半径 / 标志 / 有向边，不当成新人工计划或生成连通关系。API `snapshot_target(rawbytes, metadata, index)`可从此快照核对终点。

每个case含上表`kind`、相同七hash的`binding`、`goal`、原始`target_refs`及`stages`，每阶段为原始回执数组；成功case另含`target={index,x,y,z,radius}`。`bounded_exit`增加`expected_exit=timeout|no_damage_budget`；换场景的新玩家身份可在case的`identity`中记录，不覆盖资产哈希。改DLL、AGPW或SMX后新建证据；六类未齐或证据不满足时保留pending，不据记录中的passed字段通过。

```powershell
py -B .\waypoint-workflow\verify_clearance.py (Join-Path $sessionDir 'clearance-evidence.json')
```

验证器只离线读取，stdout输出重算JSON，六类全部通过才退出0，其余退出1并列出`pending_kinds`。范围仅限所提交采样，不替代一般阻挡等待 / 避让或全图验收。结束先`agpb_reg_clear`、清观测，再按统一收尾恢复设置并卸载本轮夹具。

## 交付与收尾

- `agpb_wp_save` / `load`后再次compare；只有author变化时才显式`--allow-author-change`。2D核对拓扑，3D核对几何与层次，使用`tools/agpw_view.py`及`tools/agpw_view3d.py`，各自能力见工具说明。
- 交付包含AGPW、人工计划、分区覆盖、实走 / 目标结果、未通过项和版本哈希。`audit_waypoints.py verify-bundle <manifest> --agpw <文件> --bsp <文件> --dll <文件>`核对清单与部署；只有全图实际覆盖和所需行为通过才能对应结项。
- 停止写入后再整理证据。先`agpb_reg_clear`并确认存活点控对象恢复，再踢bot、释放真人按键、关闭本轮noclip，恢复并独立读回记录过的设置，卸载本轮加载的disabled夹具并停止日志。关闭日志 / quit可能没有END；核对实际进程和唯一退出标记，未知退出码保留未知。
- 清理文件按AGENTS及本轮用户授权执行；TEMP和两个游戏junction是临时映射，正式AGPW不在清理范围。无需重写已冻结的历史交付包。

PROGRESS只保留当前状态、未完成项、必要验收结论和证据入口。记录地图 / 游戏目标 / 模式 / 范围、计划及三资产哈希、真人与bot身份、节点与方向覆盖、严格终点及真实目标结果、save/load比较、原设置与恢复值、退出状态及清理路径。已修复bug的排查和尝试过程不在日常文档累积。

## 离线自检

```powershell
py -B .\waypoint-workflow\test_audit_waypoints.py
py -B .\waypoint-workflow\test_verify_routes.py
py -B .\waypoint-workflow\test_stop_policy.py
py -B .\waypoint-workflow\test_verify_objectives.py
py -B .\waypoint-workflow\test_verify_lifecycle.py
py -B .\waypoint-workflow\test_verify_clearance.py
.\waypoint-workflow\test_console_transport.ps1
```

这些测试不启动游戏、不部署插件、不修改正式路点；临时输出放系统TEMP。
