// =========================================================================
//  ecs_worker.inc - the worker thread
// =========================================================================
// Rockstar Editor+ (RockstarEditorPlus.asi) hooks five of the same functions
// this plugin does - PopulateCameraMenu, PopulateMarkerMenu, BeginMethod,
// MenuInput, UpdateMenuHelpText - plus UpdateSmoothing, the scene light
// consumer and CPacketWeather::Extract, and it finds every one of them by its
// first bytes. Under FiveM it installs late: its own notes measure
// ScriptHookV's script thread starting 35 s after attach, while this plugin is
// ready in two. A hook of ours at a function's start therefore made its scans
// miss, and its menu, smooth blend and exporter never installed - which is why
// the two plugins looked mutually exclusive.
//
// So when it is loaded: scan first (reading bytes changes nothing), wait until
// its own hooks are in place, and only then install ours on top of them.
// MinHook copies whatever it finds into our trampoline, so calling the original
// runs its hook and then the game's code: both plugins do their work.
void waitForRePlus()
{
	struct Watch { const char* what; const uint8_t* at; };
	bool hooked = false;
	const Watch watch[] = {
		{ "PopulateCameraMenu", scanFnBytes(game::kPcm, &hooked) },
		{ "UpdateSmoothing",    scanFnBytes(director::kSig, &hooked) },
		{ "GetMaxDistanceAllowedFromPlayer", (const uint8_t*)g_maxDistFn },
	};
	logf("note: RockstarEditorPlus.asi is loaded. Waiting for it to install its own hooks before installing ours, "
		"so its scans still find the functions both plugins use (it looks for their first bytes).");
	const ULONGLONG start = GetTickCount64();
	int seen = -1;
	while (seen < 0 && GetTickCount64() - start < 150000)
	{
		for (int i = 0; i < (int)(sizeof(watch) / sizeof(watch[0])); ++i)
			if (watch[i].at && looksHooked(watch[i].at)) { seen = i; break; }
		if (seen < 0) Sleep(500);
	}
	if (seen >= 0)
	{
		logf("note: Rockstar Editor+ has hooked %s (after %llu s) - giving it three more seconds, then installing EscoEditor on top; both run.",
			watch[seen].what, (unsigned long long)((GetTickCount64() - start) / 1000));
		Sleep(3000);
	}
	else
		logf("note: none of the functions both plugins hook changed in 150 s - Rockstar Editor+ may have those features switched off in its ini. Installing EscoEditor now.");
}

// The four that hook the game, skipping whatever is already in. A hook can fail
// to go in at all while FiveM is still starting - the game refuses a thread list
// then and MinHook needs one - so the worker calls this again until it takes.
void installModules()
{
	if (!menu::g_ready)      menu::install();
	if (!g_rescale)          director::install();
	if (!scene::g_installed) scene::install();
	if (!lights::g_ready)    lights::install();
}

DWORD WINAPI Worker(LPVOID)
{
	Sleep(2000);

	bool globals = false;
	for (int attempt = 0; attempt < 90 && !globals; ++attempt)
	{
		globals = resolveGlobals();
		if (!globals) Sleep(2000);
	}
	if (!globals)
		logf("globals: pattern not found on this build - the speed from the ini still "
		     "applies, but the hotkeys are OFF (no way to tell the editor is open)");
	else
		logf("globals: edit-camera gate %s", g_editIndex ? "resolved" : "unresolved - keys accepted whenever the editor is open");

	{
		char a[24], b[24], c[24], d[24], e[24];
		logf("config: speed %d%%, vertical %s, look %d%%, response %s, hotkeys %s; "
		     "slower %s, faster %s, reset %s, hold %s x%.2f, hold %s x%.2f%s",
			g_cfg.speed, g_cfg.vertical > 0 ? "own" : "same", g_cfg.look,
			g_cfg.response == 2 ? "instant" : g_cfg.response == 1 ? "scaled" : "stock",
			g_cfg.hotkeys == 0 ? "off" : g_cfg.hotkeys == 1 ? "keys" : g_cfg.hotkeys == 2 ? "keys + hold" : "keys + hold + wheel",
			keyName(g_cfg.keySlower, a, sizeof(a)), keyName(g_cfg.keyFaster, b, sizeof(b)),
			keyName(g_cfg.keyReset, c, sizeof(c)),
			keyName(g_cfg.keyBoost, d, sizeof(d)), g_cfg.boostMul,
			keyName(g_cfg.keySlow, e, sizeof(e)), g_cfg.slowMul,
			g_cfg.anywhere ? " (keys accepted anywhere in the editor)" : " (keys in Edit Camera only)");
		logf("config: menu page %s, scene %s, lights %s; keyframe clipboard holds %d keyframe(s)",
			menu::pageName(g_cfg.menuPage), g_cfg.scene ? "on" : "off", g_cfg.lights ? "on" : "off", clipboard::g_count);
	}

	if (globals)
	{
		game::resolve();
		if (rePlusLoaded()) waitForRePlus();
		installModules();
	}
	if (!g_rescale)
		logf("mode: fallback - MaxSpeed is written into the metadata directly; a mod that "
		     "rewrites it every frame (NoEditorRestrictions) will win over this plugin");
	if (g_cfg.hotkeys >= 3) startWheel();

	ULONGLONG nextScan = GetTickCount64() + 45000, nextPoll = 0;
	ULONGLONG nextHookTry = GetTickCount64() + 5000;
	int hookTries = 0;
	int  lastEdit = -12345, editLogs = 0, lastMode = -12345;
	bool wasLive = false, warnedRePlus = false;

	for (;;)
	{
		Sleep(10);
		const ULONGLONG now = GetTickCount64();

		// The menu drops presses that came from the mouse; this is the clock
		// it compares against.
		if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) g_lastClick = now;

		// --- the block --------------------------------------------------
		if (g_metaCount)
		{
			char why[512];
			if (!metaStillValid(why, sizeof(why)))
			{
				logf("metadata: block no longer validates - dropped, will rescan. It read: %s", why);
				g_metaCount = 0;
				g_lastWrittenH = -1.0f;
				nextScan = now + 3000;
			}
		}

		// A hook that could not go in while the game was starting. Two minutes
		// of trying, and after one of them without pausing the game's threads.
		if (g_hookStalls && now >= nextHookTry && hookTries < 24)
		{
			++hookTries;
			nextHookTry = now + 5000;
			InterlockedExchange(&g_hookStalls, 0);
			if (hookTries == 12 && !g_mhFreezeOptional)
			{
				g_mhFreezeOptional = TRUE;
				logf("hook: a minute of trying and the game still hands over no thread list - putting the rest of the hooks in "
					 "without pausing its threads. That is what most plugins do; it is a hair riskier and the alternative is no "
					 "menu rows, no smooth blend and no lights.");
			}
			installModules();
			if (!g_hookStalls) logf("hook: everything that was waiting is in now (after %d %s)", hookTries, hookTries == 1 ? "try" : "tries");
			else if (hookTries >= 24) logf("hook: gave up after two minutes - EscoEditor.log above says which hooks never went in");
		}

		const int mode = replayMode(), idx = editIndex();
		if (mode != lastMode)
		{
			logf("state: replay mode %d -> %d", lastMode, mode);
			camfree::leave("the editor changed mode");   // never leave a keyframe holding the free camera
			if (mode != 0 && !g_metaCount && lastMode != -12345) nextScan = now;
			lastMode = mode;
		}
		if (idx != lastEdit)
		{
			if (editLogs < 12) { ++editLogs; logf("state: edit-marker index %d -> %d", lastEdit, idx); }
			lastEdit = idx;
		}

		if (!g_metaCount && now >= nextScan)
		{
			const ULONGLONG t0 = GetTickCount64();
			const int n = findMetadata();
			logf("metadata: scan took %llu ms, %d block(s)%s", (unsigned long long)(GetTickCount64() - t0), n,
				n ? "" : " - not in memory yet, will retry");
			nextScan = now + (n ? 0 : (mode > 0 ? 15000 : 60000));
			g_lastWrittenH = -1.0f;
			if (n && !warnedRePlus)
			{
				warnedRePlus = true;
				if (rePlusLoaded())
					logf("note: Rockstar Editor+ is loaded. Everything else runs side by side, but the FREE CAMERA SPEED is written by both - leave one of them at 100%% (EscoEditor: Camera Speed 100%%, or Hotkeys=0 and Speed=100 in EscoEditor.ini).");
			}
		}

		if (globals && now >= nextPoll)
		{
			nextPoll = now + 500;
			rs::poll();
		}
		scene::tick();
		lights::tick(now);

		// --- keys -------------------------------------------------------
		const bool live = inputLive() && !lights::g_uiOpen;   // typing in the light editor is not a speed key
		if (live != wasLive) { wasLive = live; logf("keys: %s", live ? "active (Edit Camera)" : "inactive"); }
		if (live)
		{
			if (pressed(g_kSlower, g_cfg.keySlower, now)) step(-1, now);
			if (pressed(g_kFaster, g_cfg.keyFaster, now)) step(+1, now);
			if (pressed(g_kReset,  g_cfg.keyReset,  now)) setSpeed(100, now);
			if (tapped(g_kFree,    g_cfg.keyFreeLook)) camfree::request();

			LONG notches = InterlockedExchange(&g_wheel, 0);
			if (g_cfg.hotkeys >= 3 && notches != 0)
			{
				if (notches >  3) notches =  3;
				if (notches < -3) notches = -3;
				while (notches > 0) { step(+1, now); --notches; }
				while (notches < 0) { step(-1, now); ++notches; }
			}
		}
		else
		{
			track(g_kSlower, g_cfg.keySlower);
			track(g_kFaster, g_cfg.keyFaster);
			track(g_kReset,  g_cfg.keyReset);
			track(g_kFree,   g_cfg.keyFreeLook);
			InterlockedExchange(&g_wheel, 0);
		}

		float hold = 1.0f;
		if (live && g_cfg.hotkeys >= 2)
		{
			if (isDown(g_cfg.keyBoost)) hold *= g_cfg.boostMul;
			if (isDown(g_cfg.keySlow))  hold *= g_cfg.slowMul;
		}
		g_holdMul = hold;

		if (g_metaCount) applyAll(hold, now);

		if (g_dirty && now - g_dirtyAt >= 700) { g_dirty = false; saveSpeed(); }
	}
}
