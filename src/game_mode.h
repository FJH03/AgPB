/**
 * AgPB - 运行期玩法模式。目标引擎仍由 game_target.h 在编译期选择。
 */

#ifndef _INCLUDE_AGPB_GAME_MODE_H_
#define _INCLUDE_AGPB_GAME_MODE_H_

enum AgPBGameMode
{
	AgPB_MODE_NORMAL = 0,
	AgPB_MODE_ZM,
	AgPB_MODE_ZE,
};

enum class AgPBZRRole
{
	Unknown,
	Human,
	Zombie,
};

/** Returns the latest live ZR role for this server userid, or Unknown. */
AgPBZRRole AgPB_ZRRoleForUserId( int userId );

/** 刷新当前地图与 ZR 状态，返回是否需要清空正在执行的任务。 */
bool AgPB_UpdateGameMode();
void AgPB_ResetGameMode();

AgPBGameMode AgPB_GameMode();
const char *AgPB_RequestedGameMode();
bool AgPB_IsZRAvailable();

/** 模式名（给日志用）："normal" / "zm" / "ze"。 */
const char *AgPB_GameModeName( AgPBGameMode eMode );

#endif // _INCLUDE_AGPB_GAME_MODE_H_
