/**
 * AgPB - 运行期游戏模式（普通 / ZM-ZE）。
 *
 * 与 game_target.h 的区别：目标游戏（csczs/css/csgo）是编译期的；
 * 游戏模式是运行期的 —— 换图时在 cfg 里改 `agpb_mode` 即可切换。
 * 由用户手动设置，不做自动判定。
 *
 * 目前只落地开关；僵尸模式的行为（flag / 敌人判定 / camp）之后再做。
 */

#ifndef _INCLUDE_AGPB_GAME_MODE_H_
#define _INCLUDE_AGPB_GAME_MODE_H_

enum AgPBGameMode
{
	AgPB_MODE_NORMAL = 0,   // 原生模式（SyPB 血统）：炸弹 / 人质…
	AgPB_MODE_ZOMBIE = 1,   // ZM / ZE（EBot 血统）—— 行为未实现，先占位
};

/** 读 convar `agpb_mode`；未知值按 normal 处理。 */
AgPBGameMode AgPB_GameMode();

/** 模式名（给菜单 / 日志用）："normal" / "zombie"。 */
const char *AgPB_GameModeName( AgPBGameMode eMode );

#endif // _INCLUDE_AGPB_GAME_MODE_H_
