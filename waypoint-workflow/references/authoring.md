# 路点制作与交付

## 适用场景

为新地图制图、补足已保存地图的覆盖，或调整手工跳边。先按[地图类型](map-types.md)列出本图要覆盖的路线及目标，再按[实走验收](route-validation.md)验证候选。

## 勘察与手工编辑

1. 从 [AGENTS.md](../../AGENTS.md) 读取部署位置。记录目标地图、模式、DLL / BSP / AGPW 哈希和待修改设置原值。已有图先备份到当次 TEMP 会话，并核对备份哈希。
2. 在游戏查看每个分区、楼层间连接、普通往返、窄口、门、梯子、跳落和目标区。对照原生 NAV / BSP 的可达区域，记录未覆盖区域、不可达处及原因。图连通、实走覆盖和地图空间覆盖分别记录。
3. 每个点明确 `label`、XYZ、`radius` 和 `flags`，每条有向边明确 `from`、`to` 和 `flags`。反向另列。每点最多 8 个出边。T / CT 位限制通行。flag 命令为切换操作。双向连接用 `agpb_wp_list` 检查两边，已有 A→B 时可明确选 B 为起点再连 A。`agpb_wp_path` 的默认结果忽略队伍限制，应以实际同队 bot 再核实。
4. 菜单和无参数 `agpb_wp_add` 取脚底 `GetAbsOrigin()`，显式 XYZ 与计划导出保留输入。核对 Z 时在同一落地点读 `agpb_wp_nearest`、`getpos 2`、默认 `getpos` 和计划 Z。默认 `getpos` 可读视点，游戏路点柱向上绘制，连边在半身高度，3D 工具用存储 XYZ。分开验证准星反馈和存储坐标。禁止仅凭显示偏移整体抬高路点。
5. 用 `agpb_wp_show 1` 查看准星白线和 `TARGET #编号`，菜单可查看最近 / 指向 / 缓存。`agpb_wp_xray 0` 要求目标柱体通视，`1` 可选穿墙显示的点。开始编辑前阅读[命令用法](../../COMMANDS.md#路点菜单与编辑器)及[独立 3D 工具](../../tools/agpw_view3d_README.md)。

## 导出与部署候选

从仓库根目录运行。先按[会话准备](route-validation.md#控制台与观察)创建 `$sessionDir`，将 `$plan` 和候选地图名换为本轮值。

```powershell
$plan = '<本轮人工计划.json>'
$candidate = Join-Path $sessionDir '<地图名>.agpw'
py -B .\waypoint-workflow\export_manual_plan.py $plan $candidate
if ($LASTEXITCODE -ne 0) { throw 'Plan export failed' }
py -B .\waypoint-workflow\audit_waypoints.py compare $plan $candidate
if ($LASTEXITCODE -ne 0) { throw 'Plan comparison failed' }
```

- `plan_version` 为 1。`agpw_version` 可显式指定 1 或 2。省略时，零向量计划输出 v1，非零速度输出 v2。给旧图做追加保全时沿用原磁盘版本。每个起点的出边可全写 `slot: 0..7` 保留空洞，或全部省略并按顺序分配。
- `compare` 核对字段、八个连接槽、方向、速度及分队有向连通，单列实际和预期磁盘版本。全零 v1 / v2 可语义等价。版本或内容变化会改变哈希，并要求重新绑定实走证据。
- 离线核对通过后，将候选放到实际目标地图的 AGPW 路径，再用 `agpb_wp_load` 读取并核对内存。游戏内编辑可按区原生保存再加载核对。`agpb_wp_save` 直接覆盖文件，`agpb_wp_clear` 仅清内存，删点会重排后续编号。使用匹配当前备份的计划或增量清单，保全用户已保存的编辑。

## 既有图增量验收

增量验收要求原磁盘版本不变。新点追加在末尾，现有节点只在清单列出的空出边槽追加。旧坐标、半径、标志、mesh、gravity、已占用连接槽和速度须按字节保全。新点出边也要显式列入清单。

```json
{"schema_version":1,"additions":[{"from":148,"to":149,"slot":2},{"from":149,"to":148,"slot":0}]}
```

```powershell
py -B .\waypoint-workflow\audit_incremental.py compare `
  --original $original --candidate $candidate --allowed-additions $additions
py -B .\waypoint-workflow\audit_incremental.py coverage `
  --original $original --candidate $candidate --allowed-additions $additions `
  --plan $plan --records $walkRecords --bsp $bsp --dll $dll --jump-probe $jumpProbeSmx
```

`coverage` 使用与候选一致的完整计划和本轮实走绑定，只要求本轮新增普通终点和有向边。若涉及梯子，传入本轮实际加载的探针 SMX 并按[梯子连续证据](route-validation.md#梯子连续证据)取样。旧边可在路线中经过。仅原生保存确实改了 author 时，才按需要加 `--allow-author-change`。

## 手工弹道跳设计

1. 先打开游戏原生 TGA 或其转换出的 PNG，实际查看起点、终点和中间场景，从侧面或斜上方核对障碍、头顶净空、支撑和落地缓冲空间。禁止仅凭两点 XYZ 或高差决定初速度。证据留在本轮会话。点位不合适时记录具体位置、遮挡或落脚问题及重布方案，供用户删除原点后重做。 [截图入口](route-validation.md#控制台与观察)
2. 必要时用只读 `agpb_jump_probe_sweep <bot-slot> sx sy sz tx ty tz [height]` 沿候选弧线分段扫掠，在障碍、弧顶和落点加密。核对实际姿态、玩家体积、原生跳跃冲量和重力。可选 `height` 是不超过实际玩家高度的试探体积，不会改变实际姿态。非零速度要求起跳时有静态世界地面支撑。
3. 沿用 SyPB 机制，在明确标记 `PATH_JUMP` 的现有有向边填写 `jumpVelocity: [x, y, z]`，或用 `agpb_wp_jumpvel` / 3D 工具编辑。各分量为有限 float32，范围 `[-2000, 2000]`，单位 units/s。缺省 `[0, 0, 0]` 沿用普通跳跃。非零向量表示起跳前世界速度，Z 随后叠加原生跳跃冲量。起跳瞬间视角须对准跳跃方向。反向独立设计，保留点的半径、`gravity` 和其他标志。
4. 每个方向先验证单跳，再从邻接路线自然接近、经过落点并继续后续段，最后验证往返。可选准备夹具只能在路线和输入已释放时摆放一次起点。运行阶段只给一次目标，运动由核心 CUserCmd 与原生物理执行。用只读跳跃探针保留事件、逐 tick 轨迹和落地，以及核心单次非零注入的 edge / XYZ / owner。普通落地终点按[严格停点](route-validation.md#实走与严格停点)判定。
5. 保存并重读后，对照最终文件的 XYZ、半径、标志、槽位和速度。把实际 TGA、必要扫掠、实走、DLL / BSP / AGPW 及探针或夹具哈希绑定到最终版本。

## 交付与收尾

1. 原生 `agpb_wp_save` 后确认成功，再 `agpb_wp_load` 读回。再次 `compare` 并核对最终文件哈希。若原生保存仅改变 author，显式使用 `--allow-author-change`，其余差异逐字段核对。
2. 交付 AGPW、人工计划或明确的手工增量清单、空间覆盖、方向及终点实走、玩法目标、未通过项和版本哈希。按需用 `audit_waypoints.py verify-bundle <manifest> --agpw <文件> --bsp <文件> --dll <文件>` 核对交付清单及实际部署。
3. 按[会话收尾](route-validation.md#收尾)恢复设置、卸载本轮夹具及整理证据。当前结论及证据入口更新到 [PROGRESS.md](../../PROGRESS.md)。

## 离线自检

从仓库根目录运行。Python 离线工具使用标准库，TGA 转换需已有 Pillow。

```powershell
py -B -m unittest discover -s .\waypoint-workflow -p 'test_*.py'
.\waypoint-workflow\test_console_transport.ps1
```
