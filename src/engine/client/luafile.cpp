#include "luafile.h"
#include "lua.h"
#include "luabinding.h"

#include <game/client/gameclient.h>
#include <game/client/components/chat.h>

CLuaFile::CLuaFile(CLua *pLua, std::string Filename) : m_pLua(pLua), m_Filename(Filename)
{
	m_pLuaState = 0;
	m_State = LUAFILE_STATE_IDLE;
	Reset();
}

void CLuaFile::Reset(bool error)
{
	m_UID = rand()%0xFFFF;
	m_State = error ? LUAFILE_STATE_ERROR: LUAFILE_STATE_IDLE;

	mem_zero(m_aScriptTitle, sizeof(m_aScriptTitle));
	mem_zero(m_aScriptInfo, sizeof(m_aScriptInfo));

	m_ScriptHasSettings = false;

	//if(m_pLuaState)
	//	lua_close(m_pLuaState);
	//m_pLuaState = luaL_newstate();
}

void CLuaFile::Unload()
{
//	if(m_pLuaState)             // -- do not close it in order to prevent crashes
//		lua_close(m_pLuaState);
	lua_gc(m_pLuaState, LUA_GCCOLLECT, 0);
	Reset();
}

void CLuaFile::OpenLua()
{
	if(m_pLuaState)
		lua_close(m_pLuaState);

	m_pLuaState = luaL_newstate();

	lua_atpanic(m_pLuaState, CLua::Panic);
	lua_register(m_pLuaState, "errorfunc", CLua::ErrorFunc);

	luaL_openlibs(m_pLuaState);
	luaopen_base(m_pLuaState);
	luaopen_math(m_pLuaState);
	luaopen_string(m_pLuaState);
	luaopen_table(m_pLuaState);
	//luaopen_io(m_pLua);
	luaopen_os(m_pLuaState);
	//luaopen_package(m_pLua); // not sure whether we should load this
	luaopen_debug(m_pLuaState);
	luaopen_bit(m_pLuaState);
	luaopen_jit(m_pLuaState);
	luaopen_ffi(m_pLuaState); // don't know about this yet. could be a sand box leak.
}

void CLuaFile::Init()
{
	m_State = LUAFILE_STATE_IDLE;

	OpenLua();

	if(!LoadFile("data/luabase/events.lua")) // try the usual script file first
	{
		if(!LoadFile("data/lua/events.luac")) // try for the compiled file if script not found
			m_State = LUAFILE_STATE_ERROR;
		else
			RegisterLuaCallbacks();
	}
	else
		RegisterLuaCallbacks();

	if(m_State != LUAFILE_STATE_ERROR)
	{
		if(LoadFile(m_Filename.c_str()))
			m_State = LUAFILE_STATE_LOADED;
		else
			m_State = LUAFILE_STATE_ERROR;
	}

	// if we errored so far, don't go any further
	if(m_State == LUAFILE_STATE_ERROR)
	{
		Reset(true);
		return;
	}

	// gather basic global infos from the script
	lua_getglobal(m_pLuaState, "_g_ScriptTitle");
	if(lua_isstring(m_pLuaState, -1))
		str_copy(m_aScriptTitle, lua_tostring(m_pLuaState, -1), sizeof(m_aScriptTitle));
	lua_pop(m_pLuaState, -1);

	lua_getglobal(m_pLuaState, "_g_ScriptInfo");
	if(lua_isstring(m_pLuaState, -1))
		str_copy(m_aScriptInfo, lua_tostring(m_pLuaState, -1), sizeof(m_aScriptInfo));
	lua_pop(m_pLuaState, -1);

	m_ScriptHasSettings = ScriptHasSettings();

	// pass the uid to the script
	lua_pushinteger(m_pLuaState, m_UID);
	lua_setglobal(m_pLuaState, "_g_ScriptUID");

	// call the OnScriptInit function if we have one
	try
	{
		LuaRef func = GetFunc("OnScriptInit");
		if(func)
			if(!func().cast<bool>())
				Reset(true);
	}
	catch (std::exception& e)
	{
		printf("LUA EXCEPTION: %s\n", e.what());
		Reset(true);
	}
}

void CLuaFile::RegisterLuaCallbacks() // LUABRIDGE!
{
	getGlobalNamespace(m_pLuaState)

		// system namespace
		.beginNamespace("_system")
			.addFunction("Import", &CLuaBinding::LuaImport)
		.endNamespace()

		// client namespace
		.beginNamespace("_client")
			.addFunction("Connect", &CLuaBinding::LuaConnect)
			.addFunction("GetTick", &CLuaBinding::LuaGetTick)
			// local info
			.addFunction("GetLocalCharacterID", &CLuaBinding::LuaGetLocalCharacterID)
			//.addFunction("GetLocalCharacterPos", &CLuaBinding::LuaGetLocalCharacterPos)
			.addFunction("GetLocalCharacterWeapon", &CLuaBinding::LuaGetLocalCharacterWeapon)
			.addFunction("GetLocalCharacterWeaponAmmo", &CLuaBinding::LuaGetLocalCharacterWeaponAmmo)
			.addFunction("GetLocalCharacterHealth", &CLuaBinding::LuaGetLocalCharacterHealth)
			.addFunction("GetLocalCharacterArmor", &CLuaBinding::LuaGetLocalCharacterArmor)
			.addFunction("GetFPS", &CLuaBinding::LuaGetFPS)
			// external info
			.addFunction("GetPlayerName", &CLuaBinding::LuaGetPlayerName)
			.addFunction("GetPlayerClan", &CLuaBinding::LuaGetPlayerClan)
			.addFunction("GetPlayerCountry", &CLuaBinding::LuaGetPlayerCountry)
			.addFunction("GetPlayerScore", &CLuaBinding::LuaGetPlayerScore)
			.addFunction("GetPlayerPing", &CLuaBinding::LuaGetPlayerPing)
		.endNamespace()

		// ui namespace
		.beginNamespace("_ui")
			.addFunction("SetUiColor", &CLuaBinding::LuaSetUiColor)
			.addFunction("DrawUiRect", &CLuaBinding::LuaDrawUiRect)
			.addFunction("DoButton_Menu", &CLuaBinding::LuaDoButton_Menu)
		.endNamespace()


		// components namespace
		.beginNamespace("_game")
			.beginNamespace("chat")
				.addFunction("Send", &CLuaBinding::LuaChatSend)
				.addFunction("Active", &CLuaBinding::LuaChatActive)
				.addFunction("AllActive", &CLuaBinding::LuaChatAllActive)
				.addFunction("TeamActive", &CLuaBinding::LuaChatTeamActive)
			.endNamespace()

			.beginNamespace("collision")
				.addFunction("GetMapWidth", &CLuaBinding::LuaColGetMapWidth)
				.addFunction("GetMapHeight", &CLuaBinding::LuaColGetMapHeight)
				.addFunction("GetTile", &CLuaBinding::LuaColGetTile)
			.endNamespace()

			.beginNamespace("emote")
				.addFunction("Send", &CLuaBinding::LuaEmoteSend)
			.endNamespace()

			.beginNamespace("controls")
				.addFunction("LockInput", &CLuaBinding::LuaLockInput)
				.addFunction("UnlockInput", &CLuaBinding::LuaUnlockInput)
				.addFunction("InputLocked", &CLuaBinding::LuaInputLocked)
				.addFunction("GetInput", &CLuaBinding::LuaGetInput)
				.addFunction("SetInput", &CLuaBinding::LuaSetInput)
				.addFunction("ResetInput", &CLuaBinding::LuaResetInput)
			.endNamespace()

			//.beginNamespace("players")
			//	.addFunction("GetPos", &CLuaBinding::LuaGetPlayerPos)
			//.endNamespace()
		.endNamespace()


		// graphics namespace
		.beginNamespace("_graphics")
			.addFunction("GetScreenWidth", &CLuaBinding::LuaGetScreenWidth)
			.addFunction("GetScreenHeight", &CLuaBinding::LuaGetScreenHeight)
			.addFunction("BlendNone", &CLuaBinding::LuaBlendNone)
			.addFunction("BlendNormal", &CLuaBinding::LuaBlendNormal)
			.addFunction("BlendAdditive", &CLuaBinding::LuaBlendAdditive)
			.addFunction("SetColor", &CLuaBinding::LuaSetColor)
			.addFunction("DrawLine", &CLuaBinding::LuaDrawLine)
			.addFunction("LoadTexture", &CLuaBinding::LuaLoadTexture)
			.addFunction("RenderTexture", &CLuaBinding::LuaRenderTexture)
		.endNamespace()

		// global types
		.beginClass< vector2_base<int> >("vec2")
			.addConstructor <void (*) (int, int)> ()
			.addData("x", &vector2_base<int>::x)
			.addData("y", &vector2_base<int>::y)
		.endClass()
		.beginClass< vector3_base<int> >("vec3")
			.addConstructor <void (*) (int, int, int)> ()
			.addData("x", &vector3_base<int>::x)
			.addData("y", &vector3_base<int>::y)
			.addData("z", &vector3_base<int>::z)
		.endClass()
		.beginClass< vector4_base<int> >("vec4")
			.addConstructor <void (*) (int, int, int, int)> ()
			.addData("r", &vector4_base<int>::r)
			.addData("g", &vector4_base<int>::g)
			.addData("b", &vector4_base<int>::b)
			.addData("a", &vector4_base<int>::a)
		.endClass()

		.beginClass< vector2_base<float> >("vec2f")
			.addConstructor <void (*) (float, float)> ()
			.addData("x", &vector2_base<float>::x)
			.addData("y", &vector2_base<float>::y)
		.endClass()
		.beginClass< vector3_base<float> >("vec3f")
			.addConstructor <void (*) (float, float, float)> ()
			.addData("x", &vector3_base<float>::x)
			.addData("y", &vector3_base<float>::y)
			.addData("z", &vector3_base<float>::z)
		.endClass()
		.beginClass< vector4_base<float> >("vec4f")
			.addConstructor <void (*) (float, float, float, float)> ()
			.addData("r", &vector4_base<float>::r)
			.addData("g", &vector4_base<float>::g)
			.addData("b", &vector4_base<float>::b)
			.addData("a", &vector4_base<float>::a)
		.endClass()
		
		//OOP BEGINS HERE
		
		.beginClass<CGameClient>("CGameClient")
			.addData("Chat", &CGameClient::m_pChat)
		.endClass()
		.beginClass<CChat>("CChat")
			.addFunction("Say", &CChat::Say)
			.addProperty("Mode", &CChat::GetMode)
		.endClass()

		.beginNamespace("Game")
			.addVariable("Client", &CLua::m_pCGameClient, false)	      //false means read only! important so noobs dont mess up the pointer		
			.addVariable("Chat", &CLua::m_pCGameClient->m_pChat, false)
		.endNamespace()
		
		.beginNamespace("Client")
			.beginNamespace("Local")
				.addVariable("CID", &CLua::m_pCGameClient->m_Snap.m_LocalClientID)
			.endNamespace()
						
			.beginClass<CConfigProperties>("Config")   // g_Config stuff...
				.addStaticProperty("PlayerName", &CConfigProperties::GetConfigPlayerName, &CConfigProperties::SetConfigPlayerName)
				.addStaticProperty("PlayerClan", &CConfigProperties::GetConfigPlayerClan, &CConfigProperties::SetConfigPlayerClan)  //char-arrays
				.addStaticData("PlayerCountry", &CConfigProperties::m_pConfig->m_PlayerCountry)  //ints
				
				.addStaticProperty("PlayerSkin", &CConfigProperties::GetConfigPlayerSkin, &CConfigProperties::SetConfigPlayerSkin)
				.addStaticData("PlayerColorBody", &CConfigProperties::m_pConfig->m_ClPlayerColorBody)
				.addStaticData("PlayerColorFeet", &CConfigProperties::m_pConfig->m_ClPlayerColorFeet)
				.addStaticData("PlayerUseCustomColor", &CConfigProperties::m_pConfig->m_ClPlayerUseCustomColor)
			.endClass()
		.endNamespace()
		
		//OOP ENDS HERE

	;
	dbg_msg("Lua", "Registering LuaBindings complete.");
}

luabridge::LuaRef CLuaFile::GetFunc(const char *pFuncName)
{
//	bool nostate = m_pLuaState == 0;
//	if(nostate)                       // SOOO MUCH HACK
//		m_pLuaState = luaL_newstate();

	LuaRef func = getGlobal(m_pLuaState, pFuncName);

//	if(nostate)
//		Unload(); // MUCH MUCH HACK

	if(func == 0)
		dbg_msg("Lua", "Error: Function '%s' not found.", pFuncName);

	return func;  // return 0 if the function is not found!
}

void CLuaFile::CallFunc(const char *pFuncName)
{
	if(!m_pLuaState)
		return;

	// TODO : Find a way to pass the LuaFunction up to 8 arguments (no matter of the type)
}

bool CLuaFile::LoadFile(const char *pFilename)
{
	if(!pFilename || pFilename[0] == '\0' || str_length(pFilename) <= 4 ||
			(str_comp_nocase(&pFilename[str_length(pFilename)]-4, ".lua") &&
			str_comp_nocase(&pFilename[str_length(pFilename)]-7, ".config")) || !m_pLuaState)
		return false;

    int Status = luaL_loadfile(m_pLuaState, pFilename);
    if (Status)
    {
        // does this work? -- I don't think so, Henritees.
        CLua::ErrorFunc(m_pLuaState);
        return false;
    }

    Status = lua_pcall(m_pLuaState, 0, LUA_MULTRET, 0);
    if (Status)
    {
    	CLua::ErrorFunc(m_pLuaState);
        return false;
    }

    return true;
}

bool CLuaFile::ScriptHasSettings()
{
	LuaRef func1 = GetFunc("OnScriptRenderSettings");
	LuaRef func2 = GetFunc("OnScriptSaveSettings");
	if(func1.cast<bool>() && func2.cast<bool>())
		return true;
	return false;
}
