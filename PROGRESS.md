# PROGRESS — AgPB Agent 交接

更新于 2026-11-24。当前处于 **M3 普通模式导航与本地自主行为移植**。普通模式以 SyPB 为主、CS-EBOT 为对照。引擎源码用于核实公开接口、物理与伤害规则。 [项目约束](AGENTS.md)

| 接手任务 | 先读 |
|---|---|
| 项目约束、构建与部署 | [AGENTS.md](AGENTS.md) · [README.md](README.md) |
| 命令、菜单与回归验收 | [COMMANDS.md](COMMANDS.md) · [VERIFY.md](VERIFY.md) |
| 手工制图、原生控制台、TGA 与 3D 编辑 | [路点工作流](waypoint-workflow/README.md) · [3D 工具](tools/agpw_view3d_README.md) |
| 战斗、难度及原生 bot 对手验证 | [独立战斗验证](combat-tests/README.md) |
| csczs 假客户端无线电兼容 | [CRASH_REPORT.md](CRASH_REPORT.md) |

## 当前部署

csczs / Win64 本轮构建通过。同一份包的 AgPB DLL、默认配置、路点和 VDF 已覆盖专服和监听服，专服另已同步 ZR 桥。DLL 包含 ZM 守点、撤退与多目标追击调整、真人电台延迟、专服填满与自动补位，并取消梯口占用等待。本次修复挂梯接管、脱困和物理刷体破障。两端已停服同步并核对包内文件。DLL SHA-256 为 `CC24737391E0EC09ED4A1B7B16FA94B17DA81A39A2FA28FA2F698E9F3F1CCB1B`。部署后未启动游戏。专服已恢复 Advance-Noblock 2.1.0，ZR Grenade Modes 1.4.8 的 SMX 已同步到同层 ZR 仓库。自动补位待实机验收。先前专服版本在 `zm_westwood_final` 空场确认 `auto → zm`、`zr=available`、`IBotManager=ok` 和 `active=1 intact=1`。历史实机结果各自绑定验收用 DLL、地图及路点。

| 部署地图 | 当前 AGPW | 已验收及待办 |
|---|---|---|
| cs_office | v2 · 405 点 / 1,136 有向边 / 61 跳边 · `81F0A7F66F4AE51138CCF50E99FF0310F0E4AD09EAAF81B673840577F92CD5CD` | 用户游戏保存版。新版本逐项实走待补。 [仓库 AGPW](waypoints/cs_office.agpw) |
| de_dust2 | v2 · 209 点 / 599 有向边 / 14 跳边 · `F229F1528EBF3D37F3C0B78B0EAB8E1BF64ADB1F3EA2372AA28828CE0AC4F087` | 用户游戏保存版。待对照原生 NAV / BSP 复核分区、多层覆盖及实走。 [仓库 AGPW](waypoints/de_dust2.agpw) |
| zm_westwood_final | v2 · 396 点 / 914 有向边 / 27 CAMP · `11A51DCAB142F1F70D6D251DD1E9465DBB8D168A798289D24FBA209F66096C7E` | 用户更新版，监听服与专服现有文件同版。新版路线未验收。 [仓库 AGPW](waypoints/zm_westwood_final.agpw) |

此前监听服读到 Dust2 的 209 点和 Office 的 405 点，`IBotManager=ok`。

- office nav10 旧版有 405 点 / 1,080 条有向边，T / CT 有向图强连通，覆盖 14 个原生 NAV 命名区。两把梯子双向 4/4 通过（`926C`，同队 CT）。两处雨棚和泵坑深尾已布点，最高车库小箱因蹲姿净空不足排除。 [旧计划](build/waypoint-output/office-nav10-closeout-20261121/cs_office.plan.json) · [离线审计](build/waypoint-output/office-nav10-closeout-20261121/offline-audit.json) · [梯子实走](build/waypoint-output/office-nav10-closeout-20261121/ladder-verification.json) · [空间核对](build/waypoint-output/office-full-20261029-98d4da1b/office-spatial-review-nav08.md)
- Dust2 F4A8 旧版通过 175/175 终点、360/360 边和 A/B 的 T 与 CT 逃爆、CT 拆包六项。拾包及安放证据覆盖指定路线。 [冻结验收](build/waypoint-output/de_dust2-20261023-final/bundle-index.json)

### zm_westwood_final 制作检查点

此前从 348 点版手工追加 35 点和 70 条有向边，含 14 条弹道跳边和 5 个箱顶 CAMP。接通警长屋顶至银行西侧吊桥、前后两组箱顶与撤回路线，并接入中央高箱到前排深色箱及西侧高箱的双向横跳。已有点位和已占用边槽字节保全，T / CT 有向图均强连通。

383 点历史版原生保存、重读后同步两端。普通模式同队 CT bot 实走分三阶段验收全部新增点和边，并从前排深色箱经中央高箱跳到西南箱顶再返回。原生结构检查 0 错误、0 孤立点，有 143 条只读几何提示。 [本轮计划和覆盖](build/waypoint-output/westwood-20261123-sheriff-bridge-crates/coverage.md)

348 点版从用户保存的 341 点版追加 7 点、16 条有向边并通过增量验收。同队 CT bot 在木梁完好时走通理发店至北屋的吊桥 `#336 ↔ #335 ↔ #221`，跨街绳 `#340 ↔ #338 ↔ #339 ↔ #337` 连同两端屋顶也通过双向实走。 [348 点历史证据](build/waypoint-output/westwood-20261123-rope-bridge/coverage.md)

325 点历史版本的 31 个严格停点包含中南楼、理发店、西酒馆、警长办公室北檐和谷仓外屋顶。中央外梯、旅馆二楼、南楼、西屋、谷仓阁楼和银行后廊木板有早期往返记录。旅馆正门在回合重置后需先打开。 [325 点历史证据](build/waypoint-output/westwood-20261122-roof-holds/coverage.md) · [249 点基线](build/waypoint-output/westwood-20261122-multilevel/coverage.md)

386 点版保留用户新增的三个屋顶点，追加四条弹道跳和一条普通落边。同队 CT bot 通过理发店至北屋的往返和 #221 → #385 → #271。可破坏木梁只标 #335 为 FALLCHECK。离线排除该点后两队均能走备用路线。 [本轮计划和覆盖](build/waypoint-output/westwood-20261124-barber-roof-backup/coverage.md)

银行屋面其他支路、可破坏木梁损毁后的绕行、深水和单向传送待核。全图有向边与梯子连续证据、专服感染模式的多 bot 守点和撤退待验。

## 接手清单

路点空间覆盖、图连通和逐项实走分别验收。严格停稳要求 XY≤max(原半径, 4)、Z≤4、接地、速度≤5，连续达到 0.8 秒。 [验收标准](VERIFY.md)

- **底座与导航** 无 AI 假客户端、SendTable 标量 / 数组 / 向量 / EHANDLE、CUserCmd、手工 AGPW 编辑和寻路。行走、蹲行、梯子、地面视角与移动解耦。单 bot 手动导航七项生命周期通过（DA6B）。

  **接续与待验收** 攻击中换队 / 回合 / 换图、冻结与死亡重叠、驻守与多 bot 生命周期，扩大梯子实测。门行为待补，中间点切换的一 tick 空输入仍待复核。

- **弹道跳与编辑** 手工有向跳边支持 SyPB 起跳前世界 XYZ 初速度，默认零走普通跳跃，起跳对准跳跃方向。非零注入仅覆盖静态世界支撑。AGPW v1/v2 兼容，3D 工具可改半径、标志和现有跳边 XYZ。准星反馈已有用户截图确认。

  **接续与待验收** 同一落地点核对 `agpb_wp_nearest`、`getpos 2`、默认 `getpos` 视点及计划 Z，继续验证关闭 xray、密集和多层选点。游戏内移动点与撤销待补，3D 工具的位置与连线仍只读。

- **清障与避让** 枪刀、连续障碍、受控超时及攻击中 stop / death 的代表矩阵通过（28BC）。普通模式和 ZM 共用本地玩家让行。ZM 按 ZR 身份判定队友，以实际地面碰撞排除 NoBlock 玩家。补了迎面／横穿预判、停步原移动探测范围内的让行和持续阻挡的绕路，保持原目标。玩家让行和物件避障离线回归通过，同队让行历史三项通过（414C8）。

  **破障代码检查** 普通模式和 ZM 人类、僵尸沿路点行进时共用破障逻辑。ZM 的贴身直追和就地撤退可绕过路点执行，已开始破障时暂停移动，保留原路线。普通模式先处理对敌战斗，战斗占用当帧指令时跳过破障。只有刀时须在近战距离内，近处探测命中但实际刀距不足时可能原地等到超时。

  **Westwood 挂梯与破障修复** 以引擎实际挂梯状态接管相邻的手工梯道。意外挂梯或自主路线已丢失时，通过原生跳跃指令脱梯后继续任务。确认有正生命值且可受常规伤害的 `func_physbox` 和 `func_physbox_multiplayer` 进入现有破障逻辑。CS:CZS Win64 构建与梯子离线回归通过，含 Westwood 梯脚提前挂梯的距离边界。第 5 回合复现场景为 #257 梯脚和 #139 ↔ #138 两扇各 50 血的木门。新版自然对局实走、刀伤和破障后续路由用户验收。修复前快照在系统 TEMP 的 `agpb-zm-ladder-watch-20261124-225945/`。

  **接续与待验收** 当前让行策略待 Dust2 同队回归。物件避障曾在 7742／office nav03 代表场景走通，当前部署待回归。清障待验自然堵枪、六次无伤及换弹 / 换枪，继续补初始重叠、近身接触、狭道、拥挤交战和不同物件类型。

- **普通战斗** 保留 SyPB 五档 difficulty 和 Godlike，按武器 / 距离 / 局势选进退、横移或停枪。低血后撤改用 bot 自己的视野检测，敌人朝向仍用于定侧移方向。斜移受阻时检查两个纯横向备选，记住实际通过安全检查的反向。逐枪主攻击与驻守有历史验收。CAMP 保持位置，CAMP+CROUCH 到位后才蹲，交战按原期限。

  **主动横移** 普通模式各难度均可使用。敌人在视野内时，步枪在距离小于 768 单位、手枪在距离小于 544 单位时可边开火边以步速横移。沿用 0.5–2.5 秒的方向保持及原有安全换边。在 256 单位及以上，每段末尾约 0.30 秒释放移动，在实测速度降至稳枪阈值后放行停步段的开火。狙击停稳、近身退避、换弹和掩体的优先级沿用原策略。`agpb_strafe_log` 可核对侧移提案、几何过滤、最终指令、位移和实际弹药消耗。 [命令与行为](COMMANDS.md#普通模式战斗与难度)

  **横移实机** 11 月 22 日经授权在 Dust2 监听服用菜单「普通」加满 23 个 AgPB，技能 40–60。日志确认手枪 3 次、步枪 11 次在同一指令上有有效侧移、同向实际位移和弹药消耗，另确认手枪停步出弹 2 次。已关闭取样开关，保留对局。原始回执快照位于系统 TEMP 的 `agpb_wp_strafe_20261122_0uwvmvvc/strafe-observation.log`。

  **掩体** 交火受伤时即时更新进攻性。普通模式自主 bot 可为换弹找掩体，C4 未安装时，生命不超过 30 或对眼前敌人的进攻值低于 20 也可触发。选点使用手工路网、双方到达距离，以及敌情快照与相邻路点的墙体和静态道具遮挡。途中有视线且武器可用时仍可开火。到位停稳且无需换弹时可用近期目击尝试投雷，拔环前复查遮挡和弧线，提交投掷时保留蹲姿。霰弹枪逐发装填期间不切雷。短停、投掷、换弹或超时后恢复先前任务，紧急目标、原有持位、梯子和跳边保持原优先级。

  **已核对** csczs Win64 构建及两端部署文件校验通过。当前离线通过步枪和手枪横移、停步、方向计时、开火策略、武器选择、持位姿态、巡逻、敌情搜索和 39 项战斗证据验证器测试。先前离线通过掩体任务优先级、巡逻恢复和手雷弧线检查。

  **待验收** 其他地图和难度的实战横移节奏、步枪停步出弹及命中、对枪中的脱战频率、抵达掩体后的换弹和实际投掷。掩体候选目前在通用寻路后排除跳边和梯道，存在捷径时仍需验证同一终点的可用地面绕路。VIP 专用撤退、受伤离开营地和 SyPB 经验危险评分尚未对齐。

  **接续与待验收** 更多武器、友军遮挡、攻击中断与多 bot。完整刀法、致盲射击及最后位置压制待补。穿透和敌情调查按用户安排后置，待验遮挡部位、不同墙体 / 动态物件实伤、失敌和真实声源。

- **武器与经济** SyPB 固定选枪、空闲换弹、近战拔刀、三种性格购买与拾枪。Glock/FAMAS 三连发、SyPB 开镜和 ZBot 消音器偏好已接。11 月 22 日同队 CT 监听局有钱购买通过，SG550 等六笔确认，余额 16000→9350。

  **收镜** 普通模式及 ZM 人类 bot 在脱战约一秒后的可用指令帧通过原生副攻击收镜。近敌收镜和主射击资格独立，AUG、SG552 用 800 单位边界，狙击枪用 150 单位边界。狙击枪按原生镜档循环，换弹或拉栓造成的临时 FOV 变化不取消收镜检查。Win64 构建、镜档与距离边界、时间回归和两端文件校验通过。 [武器行为](COMMANDS.md#普通模式战斗与难度)

  **接续与待验收** 经济局、实际交战切枪、主动拾枪和拾枪视角，以及三连发、近敌或失敌后的实际退镜和消音器效果待实机核对。

- **菜单与平台** 机器人管理五项及排队填满服务器已接。首版同队 CT 核验了指定性格、技能 100、csczs class 10 和三种踢人。csczs 每队五个外观，CS:S 四个，CS:GO 暂随机。

  **专服补位** `agpb_fill` 复用已有的排队填充，`agpb_fill normal` 为普通 40–60。`agpb_auto_players` 默认关闭，启用后等待真人进游戏，按占槽数增减 AgPB 并留一槽接入。自动添加和不指定难度的填充读取 `agpb_difficulty`，新增 `-1` 在 1–100 内抽取，原默认保持 3。踢出全部同时清空队列并关闭自动补位。 [命令与配置](COMMANDS.md#bot--调试)

  **接续与待验收** 待用户验收新键位、快速添加自动选队、专服填满及自动补位的真人进出和换图行为。待验 CS:S 外观及 CS:GO SDK。与 SyPB 相比仍缺结束回合、武器模式、默认性格、调试切换、双人跳协助、丢弃 C4／武器、检查后保存、人质禁行及驻守朝向终点。

- **地图目标与模式** 普通模式有 C4 拾取、安放、拆除、逃爆及人质跟随 / 护送。`agpb_mode` 默认 `auto`。检测到可用的 Zombie:Reloaded 接口后，`ze_` 地图进入 ZE，`zm_` 及其他感染地图进入 ZM。无 ZR 时进入普通模式，管理员可显式覆盖。目标引擎仍在编译期指定。隔离专服通过自动选择、手动覆盖、ZR 卸载、换图等 9 项检查。

  **接续与待验收** office 曾确认两名人质实际获救，更多人质路线及地图待验。携包选点、全图搜包、SyPB 经验和危险评分待补。ZE 阶段、逃生及载具自主行为待实现。 [按地图类型规划](waypoint-workflow/references/map-types.md)

- **ZM 自主行为** ZR 桥按 userid 同步存活玩家身份。人类按 CS-EBOT 的守点和逃生顺序行动。一点一只 bot，按队友支援、已知威胁、路线距离和邻近拥挤度选择手工 CAMP。可达威胁逼近即撤离，比较僵尸到实际候选路线前两点的距离后再接用路线，走不通时就近拉开。撤到临时点后重新挑 CAMP，原守点仍按当前威胁参与选择。僵尸追击自身或队友近期实际目击的位置，近身用刀。无线索时走完当前路线再选巡逻终点。有可达手工 T 点时以 2/3 概率优先，其余从全图可达终点抽取。梯子、跳边和清障保留导航控制权。人类 bot 使用个人余额和原生购买命令，重生时重置购买流程。 [行为与诊断](COMMANDS.md#zm-本地行为)

  **守点与逃生对齐** CS:CZS 和 CS:S Win64 构建通过。离线场景覆盖有支援的守点、占用避让、普通点逃生、临时经过已预留 CAMP、拦截风险拒绝、临时点停住和威胁消失后回原守点。CS:CZS DLL 已部署到专服和监听服，Westwood 实机待验。

  **多守点追击** ZM 僵尸初次选敌优先比较可达路点距离。有效追击路线绑定目标身份并保持到最后确认的位置，只有目标明显移位且原终点落后时提前重算。已搜索和无法起路的线索按目标身份和位置分别记录，新位置重新参与选敌，仍可对眼前目标执行近战。CS:CZS 和 CS:S Win64 构建及选敌、追击保持、目标失效、终点重算、按身份记忆和战斗视野离线回归通过。CS:CZS DLL 已部署两端，多守点追击待实机核对。

  **构建与待验收** Win64 构建及普通模式离线技能、开火、战斗移动、武器选择、本地规划和敌情搜索共 6 项通过。ZR 身份快照离线验证覆盖定向撤销、过期、换图及模式识别。专服热重载后 23 个 bot 均使用配置姓名表，抽读到人类 bot 的原生购买及扣款回执。姓名配置文件与购买 cvar 沿用现有部署。让行表现、Westwood 多 bot 选点和占位、感染与换轮、追击、真伤害、封梯撤退及特殊通路待游戏内体验。ZE 暂仅识别模式。

  **Westwood 巡逻图核对** 325 个路点没有 T 标记。从南楼 `#249`、`#250` 或北屋 `#252` 出发，各有 303 个可达巡逻终点，其中 292–293 个需经原有梯道离开屋顶。Win64 构建通过，热更新后自然对局抽查到无线索的三只僵尸分别去往 `#198`、`#183` 和 `#255`。连续跨守点和目击后汇合待实战验收。

- **手雷投掷与模式** csczs 和 CS:S 的普通模式和 ZM 人类复用 SyPB、CS-EBOT 的选雷思路。依据库存和近期实际目击选择 HE、闪光或烟雾，核对原生投掷飞行段及预估生效区域。切雷、瞄准、拔环和松手通过 CUserCmd。两端都由游戏生成弹体，仅在提交 bot 投掷命令期间抑制原生投掷 Radio，随后还原控制台变量。

  用户提供的 GoldSrc NadeModes 11.2（Nomexous & OT，`nademodes.sma`）在 bot 投出手雷时随机选模式。抽到绊线时，插件沿八个水平方向找 400 单位内的可附着墙面，找到后把弹体移到墙边，找不到就重抽别的模式。ZR Grenade Modes 1.4.8 在 Source 中复用该流程与现有激光探测。真人右键选择和原有触碰附着继续由 ZR 插件处理。

  **已核对** csczs、CS:S Win64 构建通过，原生短弧、高弧和碰撞离线检查通过，战斗验证器 39 项通过。Grenade Modes 1.4.8 编译并热载到专服，编译器回报原有的四条未使用参数警告。**待验收** 游戏内实际投掷、绊线附着、无墙重抽和各模式触发。CS:GO 留待后续。

- **聊天与无线电** 普通模式和 ZM 在有存活真人时每隔 18–30 游戏秒尝试由一名空闲存活 bot 公开闲聊，真人回复优先，未命中关键词的通用回复概率为 35%。普通模式抵达有效守点时播报到位。ZM 双方首次见敌都能播报，人类抵达守点、开始撤退或低血量求援时也会呼叫。csczs / CS:S 的普通、ZM 和 ZE 电台共用原生游戏命令。csczs 引擎按角色模型选音，调用使用现有 Radio Guard。普通模式使用原生接收范围。ZM、ZE 电台按 ZR 身份投给同阵营存活真人，均支持同阵营报告请求。ZM 的存活队聊保留 ZR 过滤。外部聊天默认关闭，启用后由 harness 优先决策，超时回退本地话术。 [用法及限制](COMMANDS.md#聊天与-radio)

  **已核对** csczs 和 CS:S Win64 构建通过。csczs 普通模式监听服捕获 AgPB 的 `Radio.SectorClear.ct_gsg9` 和 `Radio.SectorClear.ct_sas` 原生选音。Python harness 先前通过 5 项检查，Westwood 满员服热更新后真人客户端收到自然触发的公开闲聊。

  **真人电台回复** 普通、ZM 和 ZE 在 2–3 游戏秒后才尝试语音回复。普通模式的指令执行和 bot 主动播报沿用原有节奏。状态汇报在开口时取最新状态。csczs Win64 构建通过，已部署到专服和监听端。

  **待验收** 真人电台回复间隔、ZM／ZE 电台接收范围、CS:S 实机电台、满员局的闲聊节奏、队聊和客户端声效，以及外部聊天开关与实际模型调用。

- **玩家碰撞** 专服由 Advance-Noblock 2.1.0 管理。检测到 AgPB 注册的 `agpb_enable` 后，关闭存活玩家之间的碰撞并暂管 ZR 防卡。AgPB 卸载后恢复碰撞组和 ZR 防卡，双按计分板可临时穿透。

  **已核对** 原版 Advance-Noblock 2.1.0 源码和 SMX 已恢复，专服 SMX 与备份一致。AgPB 的 CS:S 和 CS:CZS Win64 编译通过，产物已移除自带碰撞过滤。CS:CZS DLL 已在两端停服时替换，待启动后验收。

- **自主决策与 agent** 本地按感知、人格、攻守职责和占用自主选任务、巡逻、驻守与调查。外部战斗干预默认关闭。启用后接受有期限的 `move` / `camp`，绑定地图、回合及 userid，TTL 为 0.5–30 游戏秒。相同意图续期保留 token / 路线，关闭、失联或到期均释放意图及其所属子任务，交回本地。手动 stop 保持停止。

  **接续与待验收** 外部战斗开关、高层清障应对真实紧急目标与长期多 bot 待验。M4 仍需扩展意图与观察协议，地图与 ZM/ZE 可并行。M5 高层任务仲裁待接入，动作仍由本地 CUserCmd 执行。

导航及生命周期见 [bot.cpp](src/bot.cpp)，本地任务和观察见 [bot_tasks.cpp](src/bot_tasks.cpp)、[bot_autonomy.cpp](src/bot_autonomy.cpp)、[bot_search.cpp](src/bot_search.cpp)。高层意图见 [bot_intention.cpp](src/bot_intention.cpp) 和 [Python 客户端](tools/agpb_agent.py)。战斗见 [bot_combat.cpp](src/bot_combat.cpp)，选枪、购买与拾枪分别见 [bot_weapons.cpp](src/bot_weapons.cpp)、[bot_purchase.cpp](src/bot_purchase.cpp)、[bot_pickup.cpp](src/bot_pickup.cpp)。

### 阶段衔接

M3a 单 bot 生命周期、M3b 必要清障、M3c 最小战斗已有代表验收，后续按本次改动复跑。一般动态阻挡可与 M3c 并行。M3d 分批补梯子、门和地图目标，M3e 目前保留模式开关。M4 可与地图工作并行，M5 依赖 M4 协议与本地执行、恢复闭环。 [各阶段验收标准](VERIFY.md#主线阶段验收标准)

## 开放限制与构建提示

- **Radio** 补丁 1.0.2 已同步到分发包与游戏目录，适配 11 月 7 日版 csczs。匹配当前引擎的 `active=1 intact=1` 和实机触发仍需按兼容说明核对。测试常规保持同队，真人退出、换队、切观察者或换场景前先执行 `agpb_kick all`。受控敌对场景需逐次获得用户授权。 [兼容说明](CRASH_REPORT.md)
- **诊断** `agpb_ents` 异常退出根因待查，先用单实体只读查询。部分路点 Add / Load、调试速度和 netvar 写入仍缺有限值检查。原生 `jpeg` 会触发 engine.dll 崩溃，暂不修复，截图用 TGA。几何体检的跳跃提示仅供只读复核。
- **游戏差异** csczs 站立 / 蹲姿体积随 `sv_cs_use_legacy_viewvectors` 取 62/45 或 72/54，CS:S 固定 62/45，CS:GO 固定 72/54。入口为 [AgPB_RefreshHullSize](src/waypoint.cpp)。差异按 AGENTS 规定显式分支。
- **构建** [AMBuilder](AMBuilder) 登记 `src/` 头文件并按内容变化触发 AgPB C++ 重编。新增 `.cpp` 要登记，构建脚本保持 ASCII。服务器占用 DLL 时先停服。
- **3D 路点预览** 支持通过 `game+download` 导入地图，也可直接指定 `download/maps` 下的 BSP。索引时跳过 BSP 内嵌 ZIP 的无效路径，正常资源仍按挂载顺序读取。实测 Westwood 原版 BSP 和监听服 AGPW 导入 249 点、535 条有向边。 [3D 工具](tools/agpw_view3d_README.md)
- **测试工具** 旧游戏夹具已移出，现行夹具可按路点和战斗工作流从源码重编。日常查看与属性编辑使用独立 3D 工具。

## 冻结验收索引

证据位于本机 Git 忽略的 `build/`。复跑前核对 AGPW、BSP、DLL 和对应夹具版本。

| 基线 | 结论与证据 |
|---|---|
| M3a / M3b / M3c | DA6B 单 bot 生命周期 7 项，28BC 必要清障 6 类，030B 最小战斗 4 项与意图续期。 [生命周期](build/waypoint-output/m3a-lifecycle-20261023-final/acceptance.json) · [清障](build/waypoint-output/m3b-clearance-20261023-final/acceptance.json) · [最小战斗](build/waypoint-output/m3c-combat-20261023-final/acceptance.json) |
| 跳跃 | 9611 旧图的 34 条跳边与 9 段连续路线曾通过。局部调整分别绑定 414C8 / DAA326，144↔148 反向自然接近、连续往返和最终回存为旧图留待核对。 [跳边矩阵](build/waypoint-output/jump-config-20261024-final/acceptance-summary.json) · [蹲跳 52→62](build/waypoint-output/crouch-jump-20261024-daa326df/acceptance-summary.json) · [83↔86](build/waypoint-output/office83-86-bidirectional-20261024-d28984cc/acceptance-summary.json) · [144↔148](build/waypoint-output/office144-148-20261024-7d20ffdf/acceptance-summary.json) |
| 让行与驻守 | 414C8 三种让行及低 / 高技能 M4 对照，14062 驻守接近、到位和离开姿态。 [玩家让行](build/waypoint-output/player-yield-20261024-414c8b46/acceptance-summary.json) · [M4 调用顺序](build/combat-output/fire-frame-20261024-414c8b46/acceptance-summary.json) · [驻守姿态](build/waypoint-output/camp-approach-20261024-14062a05/acceptance-summary.json) |
| 战斗与武器 | 0979 四项移动交火和武器切换冒烟，6D985 十三项射击节奏 / 走位 / 驻守。 [战斗移动](build/combat-output/ground-movement-20261024-09797eea/acceptance-summary.json) · [武器选择](build/combat-output/owned-weapons-20261024-09797eea/smoke-summary.json) · [射击与驻守](build/combat-output/primary-camp-20261024-6d985fc7/acceptance-summary.json) |
| 本地自主 | 133D 同队五 bot 自主选点、驻守巡视及高层意图到期后回退。 [自主冒烟](build/combat-output/local-autonomy-20261023-133ddc3f/smoke-summary.json) |
