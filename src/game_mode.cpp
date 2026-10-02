/**
 * AgPB - 运行期游戏模式开关，见 game_mode.h。
 *
 * 值：normal（默认）/ zombie；另外接受 zm / ze / 1 当 zombie 的简写。
 * 行为层还没按模式分叉 —— 先把开关和显示落地。
 */

#include <tier1/convar.h>
#include <tier1/strtools.h>

#include "game_mode.h"

static ConVar agpb_mode( "agpb_mode", "normal", FCVAR_GAMEDLL,
                         "AgPB game mode: normal (native) or zombie (ZM/ZE). Set it per map in cfg." );

AgPBGameMode AgPB_GameMode()
{
	const char *pszMode = agpb_mode.GetString();

	if ( pszMode != NULL &&
	     ( Q_stricmp( pszMode, "zombie" ) == 0 ||
	       Q_stricmp( pszMode, "zm" ) == 0 ||
	       Q_stricmp( pszMode, "ze" ) == 0 ||
	       Q_stricmp( pszMode, "1" ) == 0 ) )
	{
		return AgPB_MODE_ZOMBIE;
	}

	return AgPB_MODE_NORMAL;
}

const char *AgPB_GameModeName( AgPBGameMode eMode )
{
	return ( eMode == AgPB_MODE_ZOMBIE ) ? "zombie" : "normal";
}
