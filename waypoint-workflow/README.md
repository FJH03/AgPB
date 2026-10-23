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
| [fixtures/agpb_waypoint_probe.sp](fixtures/agpb_waypoint_probe.sp) | 可选只读玩家 / C4状态及真实事件探针 |
| [fixtures/agpb_behavior_fixture.sp](fixtures/agpb_behavior_fixture.sp) | 仅操纵真人的场景准备夹具 |
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

probe只读玩家 / C4状态及真实`bomb_planted` / `bomb_defused` / `bomb_exploded`事件。先记事件计数基线；按新增事件、userid、同回合、同颗C4验收。缺属性为-1，不能当0；爆炸后须另取更晚tick的同身份存活样本。C4原点在爆炸时可能变化，只在ticking阶段要求位置固定。

真人夹具只作场景准备，要求sv_cheats、拒绝fake client，不写bot或伪造物品 / 事件：

| 命令 | 作用 |
|---|---|
| `agpb_reg_place_human slot x y z yaw` | 摆放真人 |
| `agpb_reg_human_plant slot` | 真人须已持真实C4、位于真实包区，通过真人CUserCmd安放 |
| `agpb_reg_drop_c4 slot x y z` | 真人正常丢弃已有C4后摆放落地武器 |
| `agpb_reg_human_team slot T\|CT` | 零playing fake时换真人队并重生 |
| `agpb_reg_clear` | 取消夹具输入请求，卸载也清理 |

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

## 交付与收尾

- `agpb_wp_save` / `load`后再次compare；只有author变化时才显式`--allow-author-change`。2D核对拓扑，3D核对几何与层次，使用`tools/agpw_view.py`及`tools/agpw_view3d.py`，各自能力见工具说明。
- 交付包含AGPW、人工计划、分区覆盖、实走 / 目标结果、未通过项和版本哈希。`audit_waypoints.py verify-bundle <manifest> --agpw <文件> --bsp <文件> --dll <文件>`核对清单与部署；只有全图实际覆盖和所需行为通过才能对应结项。
- 停止写入后再整理证据。踢bot、`agpb_reg_clear`、释放真人按键、关闭本轮noclip，恢复并独立读回记录过的设置，卸载本轮加载的disabled夹具并停止日志。关闭日志 / quit可能没有END；核对实际进程和唯一退出标记，未知退出码保留未知。
- 清理文件按AGENTS及本轮用户授权执行；TEMP和两个游戏junction是临时映射，正式AGPW不在清理范围。无需重写已冻结的历史交付包。

PROGRESS只保留当前状态、未完成项、必要验收结论和证据入口。记录地图 / 游戏目标 / 模式 / 范围、计划及三资产哈希、真人与bot身份、节点与方向覆盖、严格终点及真实目标结果、save/load比较、原设置与恢复值、退出状态及清理路径。已修复bug的排查和尝试过程不在日常文档累积。

## 离线自检

```powershell
py -B .\waypoint-workflow\test_audit_waypoints.py
py -B .\waypoint-workflow\test_verify_routes.py
py -B .\waypoint-workflow\test_stop_policy.py
py -B .\waypoint-workflow\test_verify_objectives.py
.\waypoint-workflow\test_console_transport.ps1
```

这些测试不启动游戏、不部署插件、不修改正式路点；临时输出放系统TEMP。
