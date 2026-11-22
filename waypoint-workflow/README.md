# AgPB 路点制作

适用于 `de_`、`cs_`、`zm_`、`ze_` 地图的新图制作、补图及验收。交付范围是用户指定地图的全部可玩区域，包含多层通路。局部两点试走只验证链路。

## 按任务查阅

| 当前任务 | 读哪里 |
|---|---|
| 新图、补图、手工跳边或保存交付 | [路点制作](references/authoring.md) |
| 判断本图应覆盖的区域、路线和玩法目标 | [地图类型](references/map-types.md) |
| 原生控制台、TGA 观察、同队实走与梯子 | [实走验收](references/route-validation.md) |
| 爆破目标、导航生命周期或路线清障回归 | [专项回归](references/regression-checks.md) |

## 通用流程

1. 核对地图、游戏版本和模式，记录待改设置并备份当前 AGPW。根据实地图勘察，以及原生 NAV / BSP 的只读对照，列出分区、楼层、出入口和单向通路。
2. 按分区手工决定脚底位置、半径、标志和每条有向边。逐项核对路线方向、队伍通行及转接，弹道跳先实看起点、落点和中间场景的原生 TGA。
3. 离线核对候选 AGPW，再在目标地图同队实走。分别记录可玩区域覆盖、节点停稳 / 梯子穿越、全部有向边和本图有条件验证的玩法目标。
4. 原生保存、重读并复核文件。交付 AGPW、人工计划、分区覆盖、实走及目标结果、待验项和版本哈希。当前结论写 [PROGRESS.md](../PROGRESS.md)。

## 操作约束

- 操作前后检查 `git status --short`。用普通 shell 和游戏原生控制台，截图用 TGA，禁用 `jpeg` 和 computer-use。临时数据放系统 TEMP，正式证据放版本对应的 `build/waypoint-output/`。
- 换场景前先 `agpb_kick all` 并确认零 bot，实走时 bot 与真人同队。路点一律手工布点、连边和标记跳跃。原生 NAV 与几何体检只读。
- 独立查看与属性编辑使用 [3D 工具](../tools/agpw_view3d_README.md)。项目规则见 [AGENTS.md](../AGENTS.md)，控制台命令见 [COMMANDS.md](../COMMANDS.md)，战斗验证见 [战斗工作流](../combat-tests/README.md)。
