# 假客户端Radio兼容约束

本文件说明当前仍需遵守的引擎兼容条件。项目状态见[PROGRESS.md](PROGRESS.md)，测试始终遵守[AGENTS.md](AGENTS.md)的同队要求。

## 适用范围

当前csczs引擎的`CCSPlayer::Radio()`会把`IsBot()`为真的玩家当作原生`CCSBot`使用。AgPB创建的是无原生AI的假客户端，插件核心不靠伪装真人或改用CCSBot解决这个引擎假设。

已有独立SourceMod兼容补丁检查空bot / profile分支，只在匹配的server.dll上生效：

- 源码：[agpb_fakeclient_guard.sp](compat/sourcemod/agpb_fakeclient_guard.sp)。
- 版本及字节校验数据：[agpb.fakeclient.guard.txt](configs/sourcemod/gamedata/agpb.fakeclient.guard.txt)。
- 当前配置限csczs Win64、2026-10-11 server.dll、CRC `770BAE64`；未知版本或字节不匹配时拒绝安装。它不修改磁盘DLL，也不替代AgPB的CUserCmd行为。

## 部署与启停

游戏目录以AGENTS中的部署目标为准。用游戏既有`addons/sourcemod/scripting/spcomp64.exe`编译上述SP到系统TEMP；编译成功后，将SMX放`addons/sourcemod/plugins/agpb_fakeclient_guard.smx`，gamedata放`addons/sourcemod/gamedata/agpb.fakeclient.guard.txt`。已有同名插件先检查加载状态，替换前踢所有假客户端并卸载。

- `sm_agpb_guard_status`只读检查当前补丁；使用时要求`active=1 intact=1`。
- `sm_agpb_fakeclient_guard 0|1`切换补丁；有假客户端时关闭会被拒绝。
- 关闭、卸载或重载前先`agpb_kick all`，并确认其它假客户端也已离开。关闭补丁后，引擎原有兼容限制恢复。
- 补丁状态不改变本项目同队测试要求。真人退出、换队或切观察者之前，以及换图 / 场景之前，先踢bot。

引擎或server.dll更新后重新核对gamedata及实际状态；不能把这个版本的兼容结论推广到其它DLL或所有引擎路径。
