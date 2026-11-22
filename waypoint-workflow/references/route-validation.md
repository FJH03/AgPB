# 路点实走验收

## 适用场景

在已加载对应地图和候选路点的游戏里，验证同队 bot 实际走过的节点、有向边、普通终点停稳及梯子连续穿越。人工计划和部署见 [制作](authoring.md)，地图目标与场景回归见 [专项回归](regression-checks.md)。

## 强制规则

- 操作前后检查 `git status --short`。部署目标以 [AGENTS.md](../../AGENTS.md) 为准。所有上场测试 bot 与真人同队，真人存活、处于 ACTIVE 且 slot 小于 bot。换图、换真人队或换场景前先 `agpb_kick all`，读回确认零 bot。单独验证受测 bot 的队伍生命周期时，按[专项回归](regression-checks.md#生命周期回归m3a)切到观察者再返回原队。
- 使用普通 shell 和游戏原生控制台。每个游戏只由一个操作者串行发命令。游戏截图使用 TGA，禁用 `jpeg`。会话 CFG、原始日志、截图、记录和编译出的 SMX 放系统 TEMP，正式交付见 [制作](authoring.md)。

## 最小导航

按 [控制台与观察](#控制台与观察) 准备新会话，按 [准备夹具](#准备夹具) 加载只读探针，再执行 [实走与严格停点](#实走与严格停点)。涉及梯子时同时采集 [梯子连续证据](#梯子连续证据)。

## 控制台与观察

从仓库根目录运行。先确认只有一个目标游戏的 `hl2_launcher.exe` 在运行。每次选用新的 `agpb_wp_` 开头的会话名，后缀用 ASCII 字母、数字或下划线，控制台命令也使用 ASCII。记录当前地图、游戏模式、计划及 DLL／BSP／AGPW 哈希与将修改的设置原值，已有 AGPW 先备份到本次 TEMP 会话。

```powershell
$session = 'agpb_wp_dust2_session01'
$sessionDir = Join-Path $env:TEMP $session
New-Item -ItemType Directory -Path $sessionDir -ErrorAction Stop | Out-Null
.\waypoint-workflow\Invoke-GameConsole.ps1 -SessionName $session `
  -SessionDirectory $sessionDir -Commands 'agpb_list; agpb_wp_list 0' -Screenshot overview
```

[控制台入口](../Invoke-GameConsole.ps1) 将游戏的 `cfg/$session` 和 `screenshots/$session` 映射到同一 TEMP 会话。`-Listen` 逐行接收 `{"commands":"agpb_list","delay":500}` 这样的 JSON。`READY` 表示可收新请求，单独发送 `END` 只关闭 helper。日志中的 `BEGIN`／`END` 和 `CONSOLE_ACK` 表示 CFG 执行到标记。地图加载、出生、换队及导航结果要另外读回。遇到超时，先确认游戏的实际状态再考虑重发。`-AckTimeoutSeconds` 和 `-ScreenshotTimeoutSeconds` 分别限制回执和截图等待。截图转换需要已有 Pillow，可用 `-PythonExe` 选择对应解释器。

`agpb_wp_nearest` 以脚底定位。核对布点 Z 时，真人先落地，再对照 `agpb_wp_nearest`、`getpos 2` 和默认 `getpos` 的视点高度。真人可用 `noclip` 观察，验证路线时应远离 bot 通路，以 bot 实际轨迹为准。把 `+forward` 一类按键命令写入 CFG，`+exec` 则是启动器参数。每次按下都要配对释放，停止或中断时释放本轮所有按键。可用 `hidepanel all; hideconsole; gameui_hide` 收起面板。

## 准备夹具

自动实走先编译和加载 [agpb_waypoint_probe.sp](../fixtures/agpb_waypoint_probe.sp)，它只读玩家状态、C4 与原生事件。梯子再加 `agpb_jump_probe`，其它场景按对应章节追加所需夹具。无 SourceMod 时，用原生命令和事件日志作人工验收，缺少可靠采样的项目标为未验证。

从仓库根目录使用部署目标已有的 `spcomp64.exe`。先查看 `sm plugins list`，同名夹具已加载时先结束其场景、卸载并读回。核对 `disabled` 下的同名文件再决定是否替换，保留本轮实际加载的 SMX 及哈希。

```powershell
$requirements = Get-Content -LiteralPath .\AGENTS.md -Raw -Encoding utf8
$gameDir = [regex]::Match($requirements, '(?m)^- 部署目标：\s*`([^`]+)`').Groups[1].Value
if (!$gameDir) { throw 'Deployment target missing' }
$scripting = Join-Path $gameDir 'addons\sourcemod\scripting'
$disabled = Join-Path $gameDir 'addons\sourcemod\plugins\disabled'
$fixtures = @('agpb_waypoint_probe')
foreach ($name in $fixtures) {
  $source = Join-Path '.\waypoint-workflow\fixtures' ($name + '.sp')
  $smx = Join-Path $sessionDir ($name + '.smx')
  & (Join-Path $scripting 'spcomp64.exe') $source ('-i' + (Join-Path $scripting 'include')) ('-o' + $smx)
  if ($LASTEXITCODE -ne 0) { throw ('Fixture compilation failed: ' + $name) }
  $installed = Join-Path $disabled ($name + '.smx')
  Copy-Item -LiteralPath $smx -Destination $installed -ErrorAction Stop
  Get-FileHash -Algorithm SHA256 -LiteralPath $installed
}
```

在游戏控制台手动加载本轮夹具。`disabled/` 避免启动时自动加载。下例先加载实走探针，再读取实际状态。其它夹具按同样方式加载对应名称。

```text
sm plugins load disabled/agpb_waypoint_probe
sm plugins list
agpb_wp_probe_state
```

### 收尾

1. 若使用场景夹具，先停止相应观测并完成恢复。例如 `agpb_reg_clear` 取消行为夹具请求，要读回确认仍存活的点控对象已恢复。跳跃探针用 `agpb_jump_probe_clear`，清障探针用 `agpb_wp_probe_clearance_clear`，玩家取样用 `agpb_players_clear`。若准备阶段移动过 prop，先用 `agpb_prop_watch_stop` 停止取样。在同一身份下确认 bot 的输入和路线已释放，再用 `agpb_prop_restore` 恢复物件并读回。
2. 执行 `agpb_kick all`，核对零 bot，释放真人按键，关闭本轮 `noclip`，恢复并逐项读回本轮修改的设置。
3. `sm plugins unload disabled/agpb_waypoint_probe`，按相同方式卸载本轮其它已加载的夹具，用 `sm plugins list` 确认。用原生控制台 `con_logfile ""` 停止会话日志。停止日志或按要求 `quit` 时可能缺少 `END`。涉及退出时核对原生退出标记和实际进程，未知的退出码记为未知。
4. 停止写入后整理证据，按 [AGENTS.md](../../AGENTS.md) 列出待清理文件和游戏内两个临时 junction 的完整路径。备份和正式部署的 AGPW 按本轮交付保管。

## 实走与严格停点

1. 先用 `agpb_list; agpb_wp_probe_state` 核对 bot index 与 slot、队伍、双方存活及 userid，待武器就绪后关闭 bot 自主任务。`agpb_bot_auto <idx> 0` 同时清路线和手动漫游。以下索引、队伍和目标标签仅为示例，按计划与当前会话实际值填写。

   ```powershell
   $plan = '<本轮人工计划.json>'
   .\waypoint-workflow\Invoke-GameConsole.ps1 -SessionName $session `
     -SessionDirectory $sessionDir -Commands 'agpb_bot_auto 0 0'
   py -B .\waypoint-workflow\verify_routes.py --plan $plan `
     --session-name $session --session-directory $sessionDir `
     --bot-index 0 --bot-slot 2 --human-slot 1 --team CT `
     --targets '<本轮目标标签>' --require-precise-stop --output (Join-Path $sessionDir 'walk-01.json')
   ```

2. `--targets` 使用计划中的 label，多个目标用逗号分隔。记录器核对内存路点、相同 userid 及实际队伍，再逐目标提交一次 `agpb_bot_goto`，捕获原始 walking／arrival 回执与停点样本。移动由 AgPB 的 CUserCmd 执行。失败或中断会尝试停住同一 bot，失败与重试用不同输出文件。完整覆盖要求分别验证普通节点作为请求终点，并核对全部有向边。梯子节点按下节处理。
3. [严格终点政策](../stop_policy.py) 要求 XY 偏差 ≤ `max(原 radius, 4)`、Z 偏差 ≤ 4、接地且三维速度 ≤ 5 units/s。最后一个同身份连续窗口需 ≥ 0.8 游戏秒、至少 3 份样本、相邻间隔 ≤ 1.5 秒。中间点按穿行验收。严格停点需有位置、速度、接地、身份数据和对应政策版本。

## 梯子连续证据

给一次以普通出口为终点的 goto，连续经过下端和上端 `LADDER` 节点，到普通出口按原停点政策停稳。下降方向单独观察。提前按 [夹具准备](#准备夹具) 加载 `agpb_jump_probe`，在预期穿越前用 `agpb_jump_probe_arm <bot-slot> <human-slot> [seconds]` 限时采集，默认 5 秒，允许 `0 < seconds ≤ 10`。在爬梯附近用 `agpb_bot_info <idx>` 保留实际 goal 和非零 owner。

- 在同一条 record 的原始 `log`、`observed_log` 或 `ladder_log` 保留同次 walking／arrival、逐 tick 探针样本和核心快照。metadata 写入实际加载探针 SMX 的 SHA-256 至 `jump_probe_sha256`，给 [既有图增量验收](authoring.md#既有图增量验收) 的 `--jump-probe` 传入该 SMX 验证。
- 在该请求 `before`／`after` 范围内采到同一地图代次、观测代次、双方身份及队伍的连续窗口。至少 3 份样本，tick 递增、样本号连续、相邻时间差 `0 < Δt ≤ 0.2` 秒。在梯节点容差 XY ≤ `max(radius, 24)`、Z ≤ 32 内按路线顺序实际采到 `movetype=9`，上下端之间持续为梯子状态。
- 相邻梯节点计划高差绝对值至少 8 单位，实际沿相同方向进展至少 `max(8, |计划高差| - 64)` 单位。活跃路线的 goal 和 owner 一致，至少一次 owner 采样在梯节点实测时刻的 1.5 秒内。随后采到脱梯、有效地面句柄和接地标志，最后在普通出口严格停稳。梯子容差用于中间穿越。

## 输出要求

在同一计划和同一组实际部署资产下汇总实走记录，使用本轮 AGPW、BSP、DLL 文件核对哈希。可依次传入多份记录。改变计划、点位、半径、标志、边或顺序后重新取证。

```powershell
py -B .\waypoint-workflow\audit_waypoints.py coverage $plan (Join-Path $sessionDir 'walk-01.json') `
  --require-precise-stop --agpw '<正式AGPW>' --bsp '<当前BSP>' --dll '<当前DLL>'
```

保留原始控制台回执、记录 JSON 及审核报告。路线日志被截断时用 `--console-log` 补交同次原始控制台日志，`--allow-legacy` 只用于显式降级的历史审核。用 `missing_nodes` 和 `missing_directed_edges` 核对图内遍历缺口，再把 `endpoint_coverage.precise_endpoint_ids` 与全部普通节点逐项对照，另查梯子逐方向的连续证据。`precise_stop_certified` 仅覆盖已提交的停点样本。审核退出 1 时查具体缺口，输入错误退出 2。最后按[制作](authoring.md)核查可玩区和多层通路的空间覆盖。
