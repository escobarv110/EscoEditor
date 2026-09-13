// =============================================================================
//  EscoEditor.asi - more control for the Rockstar Editor
// =============================================================================
//  Standalone. No ScriptHookV, no dependency on any other mod. Works in GTA V
//  Legacy and FiveM (declares every FiveM game build through FX_ASI_BUILD
//  resources); on GTA V Enhanced only the free camera speed keys work.
//
//  An "EscoEditor" row is added at the bottom of every marker's menu in the
//  editor. Left/right on it opens one of four pages (keyboard only):
//
//    Camera Speed    the free camera's speed in Edit Camera: Camera Speed,
//                    Up/Down Speed, Look Speed
//    Copy & Paste    From / To keyframe, Copy, Cut, Paste: whole markers
//                    (camera, blend, shake, effects, audio, speed), one or a
//                    range, into the same clip, another clip, or after a
//                    restart (the clipboard lives in EscoEditor.ini)
//    Time & Weather  play the clip at any time of day and in any weather,
//                    with the lighting following (not only the sky)
//    Lights          open the light editor, and pick the key that opens it
//
//  The stock Speed row is taken over as well: 5% steps from 5% to 1000%
//  instead of the game's 9 fixed choices, and the Depth of Field submenu
//  gets Copy DOF / Paste DOF / DOF To All. Scene lights are placed in the
//  picture with a 3D gizmo in the light editor, an ImGui window through
//  ReShade's add-on API. The editor's own camera path, transitions, shake
//  and depth of field are never touched.
//
//  The source is split into include files, one per feature, all inside one
//  anonymous namespace so the whole plugin is a single translation unit:
//    ecs_core.inc       paths, log, ini, presets, pattern scanning, the
//                       Edit Camera gate, the free camera metadata block, keys
//    ecs_game.inc       marker record, marker storage interface, quaternions,
//                       replay director / free camera layout, game functions
//    ecs_clipboard.inc  the keyframe clipboard
//    ecs_camfree.inc    Free Look: fly at a keyframe without changing its
//                       camera, and switch back to the camera it renders
//    ecs_scene.inc      time of day / weather override
//    ecs_clipspeed.inc  the 5% marker speed steps
//    ecs_dof.inc        depth of field: copy / paste / to all
//    ecs_reshade.inc    EscoEditor as a ReShade add-on: ReShade's ImGui for the
//                       light editor, and an EscoEditor tab in ReShade's menu
//    ecs_lights.inc     scene lights: each clip's lights and their keyframes,
//                       EscoEditor.lights.txt, and the game's scene light list
//    ecs_lightui.inc    the light editor: ImGui window and 3D gizmo (ReShade)
//    ecs_menu.inc       the rows in the marker menu
//    ecs_director.inc   the per-frame hook on the replay director
//    ecs_worker.inc     the worker thread
//
//  Techniques and offsets follow Rockstar Editor+ (GPL-3, github.com/crxhvrd/
//  REPlus) and were verified on FiveM's b3407 image. Code here is original.
//
//  Copyright (C) 2026. Licence: MIT (MinHook: BSD-2, see src/minhook).
// =============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <emmintrin.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <cmath>
#include <initializer_list>
#include <string>
#include <vector>
#include "minhook/include/MinHook.h"
// Dear ImGui 1.90.4 docking (MIT), header only: every call goes through the
// function table ReShade hands over, so these settings must be the ones
// ReShade built its ImGui with (deps/ImGui.props), or the structs disagree.
#define ImTextureID ImU64
#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui/imgui.h"
#include "reshade/reshade.hpp"      // ReShade add-on API, header-only (BSD-3 / MIT)

namespace
{
#include "ecs_core.inc"
#include "ecs_game.inc"
#include "ecs_clipboard.inc"
#include "ecs_camfree.inc"
#include "ecs_scene.inc"
#include "ecs_clipspeed.inc"
#include "ecs_dof.inc"
#include "ecs_lights.inc"
#include "ecs_lightui.inc"
#include "ecs_reshade.inc"
#include "ecs_menu.inc"
#include "ecs_director.inc"
#include "ecs_worker.inc"
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(hModule);
		g_self = hModule;

		GetModuleFileNameA(hModule, g_dir, MAX_PATH);
		if (char* s = strrchr(g_dir, '\\')) s[1] = '\0';
		strcpy_s(g_ini, g_dir); strcat_s(g_ini, "EscoEditor.ini");
		strcpy_s(g_log, g_dir); strcat_s(g_log, "EscoEditor.log");

		if (!isSupportedGame()) return TRUE;   // dropped into some other game: stay inert

		loadConfig();
		clipboard::load();
		dof::load();
		lights::init();
		if (FILE* f = nullptr; fopen_s(&f, g_log, "wb") == 0 && f) fclose(f);   // fresh log per session
		logf("EscoEditor %s attached to '%s'%s (build " __DATE__ " " __TIME__ ")", kVersion,
			exeName(), isFiveM() ? " [FiveM]" : isEnhanced() ? " [Enhanced]" : " [Legacy]");
		logf("files: %s", g_ini);
		rs::registerEarly();   // before ReShade starts rendering - see ecs_reshade.inc

		if (HANDLE h = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr)) CloseHandle(h);
		else logf("could not start the worker thread (%lu)", GetLastError());
	}
	else if (reason == DLL_PROCESS_DETACH)
	{
		lights::saveAtExit();
		scene::restore();
		restoreAll();
	}
	return TRUE;
}
