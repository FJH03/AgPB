/**
 * AgPB - agent-controlled bots for Counter-Strike: Source.
 *
 * 设计要点：完全不使用引擎自带的 CCSBot / CCSBotManager。
 * 假客户端由 server.dll 暴露的 IBotManager ("BotManager001") 创建，
 * 逐 tick 的 usercmd 由 IBotController::RunPlayerMove() 注入。
 */

#ifndef _INCLUDE_AGPB_PLUGIN_H_
#define _INCLUDE_AGPB_PLUGIN_H_

#include <ISmmPlugin.h>
#include <igameevents.h>
#include <eiface.h>
#include <edict.h>
#include <icvar.h>
#include <tier1/convar.h>
#include <game/server/iplayerinfo.h>
#include <engine/iserverplugin.h>

#include "version_gen.h"

class AgPBPlugin : public ISmmPlugin, public IMetamodListener
{
public:
	bool Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late);
	bool Unload(char *error, size_t maxlen);
	bool Pause(char *error, size_t maxlen);
	bool Unpause(char *error, size_t maxlen);
	void AllPluginsLoaded();

public: // IMetamodListener
	void OnLevelShutdown();

public: // hooks
	void Hook_GameFrame(bool simulating);
	void Hook_ClientDisconnect(edict_t *pEntity);

public: // plugin metadata
	const char *GetAuthor() { return PLUGIN_AUTHOR; }
	const char *GetName() { return PLUGIN_DISPLAY_NAME; }
	const char *GetDescription() { return PLUGIN_DESCRIPTION; }
	const char *GetURL() { return PLUGIN_URL; }
	const char *GetLicense() { return PLUGIN_LICENSE; }
	const char *GetVersion() { return PLUGIN_FULL_VERSION; }
	const char *GetDate() { return __DATE__; }
	const char *GetLogTag() { return PLUGIN_LOGTAG; }
};

extern AgPBPlugin g_AgPBPlugin;

PLUGIN_GLOBALVARS();

#endif // _INCLUDE_AGPB_PLUGIN_H_
