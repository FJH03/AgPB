# 假客户端Radio兼容约束

本文件说明当前仍需遵守的引擎兼容条件。项目状态见[PROGRESS.md](PROGRESS.md)，测试始终遵守[AGENTS.md](AGENTS.md)的同队要求。

## 适用范围

当前csczs引擎的`CCSPlayer::Radio()`会把`IsBot()`为真的玩家当作原生`CCSBot`使用。AgPB创建的是无原生AI的假客户端，插件核心不靠伪装真人或改用CCSBot解决这个引擎假设。

独立的 SourceMod 插件为无线电代码中的空 bot / profile 分支提供保护。

AgPB 的 csczs 普通、ZM 和 ZE 电台播报通过公开客户端命令入口执行原生无线电，使用现有已启用的 Guard。引擎按当前角色模型选音，ZM 和 ZE 根据 ZR 身份限制该次播报的接收者。

- 源码：[agpb_fakeclient_guard.sp](compat/sourcemod/agpb_fakeclient_guard.sp)。
- 补丁配置：[agpb.fakeclient.guard.txt](configs/sourcemod/gamedata/agpb.fakeclient.guard.txt)。
- csczs Win64 补丁按无线电代码定位原指令、共用清理路径及预留空间。链接时变化的相对地址用通配处理。安装或升级无需登记 `server.dll` 的 CRC、MD5 或哈希。

## 部署与启停

游戏目录以 AGENTS 中的部署目标为准。用游戏已有的 `addons/sourcemod/scripting/spcomp64.exe` 将源文件编译到系统 TEMP。将 SMX、源文件和 gamedata 一起同步到游戏目录和 `build/package/addons/sourcemod/` 的对应位置。已有同名插件运行时，替换前踢出假客户端并卸载。

- `sm_agpb_guard_status`只读检查当前补丁；使用时要求`active=1 intact=1`。
- `sm_agpb_fakeclient_guard 0|1`切换补丁；有假客户端时关闭会被拒绝。
- 关闭、卸载或重载前先`agpb_kick all`，并确认其它假客户端也已离开。关闭补丁后，引擎原有兼容限制恢复。
- 补丁状态不改变本项目同队测试要求。真人退出、换队或切观察者之前，以及换图 / 场景之前，先踢bot。

引擎更新后用 `sm_agpb_guard_status` 查看实际启用状态。若插件加载失败，原因在 SourceMod 错误日志中。
