// =========================================================================
//  ecs_menu.inc - rows in the editor's own menus
// =========================================================================
//  The marker menu (Camera, Depth of Field, Effects, Audio, Speed, ...) and
//  its submenus are Scaleform columns the game rebuilds through
//  CVideoEditorPlayback::Populate*Menu: one ADD_COLUMN_ITEM(_WITH_OPTIONS)
//  call per row, then ADD_COLUMN_HELP_TEXT. Hooking CScaleformMgr::
//  BeginMethod lets us slip our rows in just before that last call, so they
//  land at the bottom of the list. The game then treats them like any other
//  row: focus moves onto them and left/right/accept reach the marker-menu
//  input dispatcher, which is hooked so those presses act on our rows
//  instead of a stock option. UpdateItemTextValue rewrites a row's value in
//  place afterwards, and the help-text choke point is hooked so each row
//  explains itself on the line under the column.
//
//  Two columns carry our rows:
//    the marker menu       one "EscoEditor" header whose value is a page
//                          (Camera Speed, Copy & Paste, Time & Weather,
//                          Lights); left/right switches page and rebuilds
//                          the column
//    the Depth of Field    Copy DOF, Paste DOF and DOF To All under the
//    submenu               game's own DOF rows (ecs_dof.inc)
//
//  KEYBOARD ONLY. The Scaleform column turns mouse clicks into the same
//  accept / left / right codes; those are dropped on our rows and on the
//  Speed row (an input that arrives while the left mouse button is down, or
//  within a third of a second of a click).
//
//  Our rows carry option id 0x27 (MARKER_MENU_OPTION_HELP_TEXT): the last id
//  inside the game's own enum, inert if a press ever reaches stock code. A
//  row is recognised by its index AND that id, never by either alone.
//
//  The stock Speed row (option id 0x25) is also handled here: left/right on
//  it go to ecs_clipspeed.inc and the row is relabelled after each populate.
//
//  Mechanism after Rockstar Editor+ (GPL-3, github.com/crxhvrd/REPlus); the
//  patterns and interior offsets were verified on FiveM's b3407 image.
//  Legacy only: the Enhanced binary has a different shape here.
// =========================================================================
namespace menu
{
	using FnPopulate    = void (__fastcall*)(unsigned);
	using FnClear       = void (__fastcall*)();
	using FnMenuInput   = void (__fastcall*)(int);
	using FnBeginMethod = char (__fastcall*)(int, int, const char*, int, void*);
	using FnAddParamStr = void (__fastcall*)(const char*, bool);
	using FnEndMethod   = void (__fastcall*)(bool);
	using FnArrayGrow   = void*(__fastcall*)(void*, int);
	using FnUpdateText  = void (__fastcall*)(int, const char*);
	using FnUpdateHelp  = void (__fastcall*)(unsigned);

	struct MenuOption { unsigned id, restriction, toggleCount; };
	struct MenuArray  { MenuOption* data; unsigned short count, cap; };
	static_assert(sizeof(MenuOption) == 12, "MenuOption is 12 bytes");

	constexpr int      kVideoEditorClass = 9;     // SF_BASE_CLASS_VIDEO_EDITOR
	constexpr unsigned kOurOptionId      = 0x27;  // MARKER_MENU_OPTION_HELP_TEXT
	constexpr int      NAV_RIGHT = 0xBE, NAV_LEFT = 0xBD, NAV_ACCEPT = 0xC9;
	constexpr int      kColumnCapacity = 16;      // the Scaleform column draws no more

	const char* kPmm =
		"48 89 5C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 50 8B F9 E8";
	const char* kMi =
		"48 89 5C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 40 "
		"45 33 FF 0F 29 74 24 30 44 8B F1 41 8B DF E8 ? ? ? ? 4C 8B E8";
	const char* kUit =
		"48 89 5C 24 08 57 48 83 EC 30 41 83 C9 FF 8B F9 8B 0D ? ? ? ? 48 8B DA 4C 8D 05";
	const char* kUmh =
		"40 53 48 83 EC 30 0F B7 05 ? ? ? ? 85 C0 0F 8E ? ? ? ? 39 05 ? ? ? ? "
		"0F 8D ? ? ? ? E8 ? ? ? ? 48 85 C0 74 04 8B 08 EB 05 B9 28 00 00 00";
	// CVideoEditorPlayback::PopulateDofMenu opens with this (rax = rsp frame).
	constexpr uint8_t kDofPrologue[] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10 };

	uintptr_t a_PopulateMarkerMenu = 0, a_PopulateDofMenu = 0, a_ClearOptions = 0, a_MenuInput = 0;
	uintptr_t a_UpdateItemText = 0, a_UpdateMenuHelpText = 0;
	uintptr_t a_BeginMethod = 0, a_AddParamString = 0, a_EndMethod = 0, a_ArrayGrow = 0;
	uintptr_t a_MovieId = 0, a_MenuOptions = 0, a_FocusIndex = 0;

	FnPopulate    origPopulateMarker = nullptr;
	FnPopulate    origPopulateDof    = nullptr;
	FnBeginMethod origBeginMethod    = nullptr;
	FnMenuInput   origMenuInput      = nullptr;
	FnUpdateHelp  origUpdateHelp     = nullptr;

	bool g_ready = false, g_inMenu = false, g_inDof = false, g_injecting = false, g_injected = false;
	int  g_first = -1, g_count = 0, g_injectLogs = 0, g_mouseDrops = 0;

	// --- columns, pages and rows ---------------------------------------------
	enum Kind { K_MARKER = 0, K_DOF };
	enum Page { PAGE_CLOSED = 0, PAGE_CAMSPEED, PAGE_CLIPBOARD, PAGE_SCENE, PAGE_LIGHTS, PAGE_COUNT };
	enum Row  { R_HEADER = 0, R_SPEED, R_VSPEED, R_LOOK, R_FREE,
	            R_FROM, R_TO, R_COPY, R_CUT, R_PASTE, R_TIME, R_WEATHER,
	            R_DCOPY, R_DPASTE, R_DALL, R_LOPEN, R_LKEY, R_LALL, R_COUNT };

	int g_kind = K_MARKER;   // the column our rows are in right now
	int g_page = PAGE_CAMSPEED;
	int s_rows[12];
	int s_shown = 0;

	bool pageAvailable(int p)
	{
		switch (p)
		{
		case PAGE_CLOSED: case PAGE_CAMSPEED: return true;
		case PAGE_CLIPBOARD: return game::a_GetCurrentEditMarker != 0;
		case PAGE_SCENE:     return scene::g_installed;
		case PAGE_LIGHTS:    return lights::g_ready;
		default: return false;
		}
	}

	const char* pageName(int p)
	{
		switch (p)
		{
		case PAGE_CAMSPEED:  return "Camera Speed";
		case PAGE_CLIPBOARD: return "Copy & Paste";
		case PAGE_SCENE:     return "Time & Weather";
		case PAGE_LIGHTS:    return "Lights";
		default:             return "Closed";
		}
	}

	void buildRows()
	{
		s_shown = 0;
		if (g_kind == K_DOF)
		{
			s_rows[s_shown++] = R_DCOPY; s_rows[s_shown++] = R_DPASTE; s_rows[s_shown++] = R_DALL;
			return;
		}
		s_rows[s_shown++] = R_HEADER;
		switch (g_page)
		{
		case PAGE_CAMSPEED:
			s_rows[s_shown++] = R_SPEED; s_rows[s_shown++] = R_VSPEED; s_rows[s_shown++] = R_LOOK;
			if (game::a_GetCurrentEditMarker) s_rows[s_shown++] = R_FREE;
			break;
		case PAGE_CLIPBOARD:
			s_rows[s_shown++] = R_FROM; s_rows[s_shown++] = R_TO;
			s_rows[s_shown++] = R_COPY; s_rows[s_shown++] = R_CUT; s_rows[s_shown++] = R_PASTE;
			break;
		case PAGE_SCENE:
			s_rows[s_shown++] = R_TIME; s_rows[s_shown++] = R_WEATHER;
			break;
		case PAGE_LIGHTS:
			s_rows[s_shown++] = R_LOPEN; s_rows[s_shown++] = R_LKEY; s_rows[s_shown++] = R_LALL;
			break;
		default: break;
		}
	}

	bool isAction(int r) { return r == R_COPY || r == R_CUT || r == R_PASTE || r == R_DCOPY || r == R_DPASTE || r == R_DALL || r == R_LOPEN; }

	// --- Scaleform plumbing (Legacy shapes) ------------------------------
	bool sfBegin(const char* method)
	{
		return ((FnBeginMethod)a_BeginMethod)(*(int*)a_MovieId, kVideoEditorClass, method, -1,
		                                      (void*)(intptr_t)-1) != 0;
	}
	void sfParam(const char* s) { ((FnAddParamStr)a_AddParamString)(s, false); }
	void sfEnd()                { ((FnEndMethod)a_EndMethod)(false); }
	int  optionCount()          { return ((const MenuArray*)a_MenuOptions)->count; }

	void pushOption(int toggles)
	{
		auto* slot = (unsigned*)((FnArrayGrow)a_ArrayGrow)((void*)a_MenuOptions, 0x10);
		if (!slot) return;
		slot[0] = kOurOptionId;
		slot[1] = 0;
		slot[2] = (unsigned)toggles;
	}

	void addRow(const char* label, const char* value, int toggles)
	{
		if (!sfBegin(value ? "ADD_COLUMN_ITEM_WITH_OPTIONS" : "ADD_COLUMN_ITEM")) return;
		sfParam(label);
		if (value) sfParam(value);
		sfEnd();
		pushOption(toggles);
	}

	void emitHelp(const char* text)
	{
		if (!sfBegin("UPDATE_COLUMN_HELP_TEXT")) return;
		sfParam(text ? text : "");
		sfEnd();
	}

	void updateText(int idx, const char* text)
	{
		if (a_UpdateItemText) ((FnUpdateText)a_UpdateItemText)(idx, text);
	}

	// --- the rows ----------------------------------------------------------
	const char* rowLabel(int r)
	{
		switch (r)
		{
		case R_HEADER:  return "EscoEditor";
		case R_SPEED:   return "Camera Speed";
		case R_VSPEED:  return "Up/Down Speed";
		case R_LOOK:    return "Look Speed";
		case R_FREE:    return "Free Look";
		case R_FROM:    return "From Keyframe";
		case R_TO:      return "To Keyframe";
		case R_COPY:    return "Copy Keyframes";
		case R_CUT:     return "Cut Keyframes";
		case R_PASTE:   return "Paste Keyframes";
		case R_TIME:    return "Time of Day";
		case R_WEATHER: return "Weather";
		case R_DCOPY:   return "Copy DOF";
		case R_DPASTE:  return "Paste DOF";
		case R_DALL:    return "DOF To All";
		case R_LOPEN:   return "Open Light Editor";
		case R_LKEY:    return "Light Editor Key";
		case R_LALL:    return "Lights In Every Clip";
		default:        return "";
		}
	}

	const char* rowValue(int r, char* buf, int cap)
	{
		switch (r)
		{
		case R_HEADER:  return pageName(g_page);
		case R_SPEED:   snprintf(buf, cap, "%d%%", g_cfg.speed); return buf;
		case R_VSPEED:
			if (g_cfg.vertical <= 0) return "Same";
			snprintf(buf, cap, "%d%%", g_cfg.vertical); return buf;
		case R_LOOK:    snprintf(buf, cap, "%d%%", g_cfg.look); return buf;
		case R_FREE:    return camfree::on() ? "On" : "Off";
		case R_FROM:    return clipboard::rangeValue(true, buf, cap);
		case R_TO:      return clipboard::rangeValue(false, buf, cap);
		case R_TIME:    return scene::timeName(buf, cap);
		case R_WEATHER: return scene::weatherName();
		case R_LKEY:    return lights::g_capture ? "Press a key" : keyName(g_cfg.keyLights, buf, cap);
		case R_LALL:    return g_cfg.lightsAll ? "On" : "Off";
		default:        return "";
		}
	}

	int rowToggles(int r)
	{
		switch (r)
		{
		case R_HEADER:  return PAGE_COUNT;
		case R_SPEED: case R_LOOK: return kPresetCount;
		case R_FREE:    return 2;
		case R_VSPEED:  return kPresetCount + 1;
		case R_FROM: case R_TO: return 64;
		case R_TIME:    return 97;
		case R_WEATHER: return scene::kWeatherCount + 1;
		case R_LKEY:    return lights::kCycleCount;
		case R_LALL:    return 2;
		default:        return -1;
		}
	}

	// One short line each - the row name says most of it.
	const char* rowHelp(int r)
	{
		static char buf[200];
		char s[48];
		switch (r)
		{
		case R_HEADER:  return "Left/right: Camera Speed, Copy & Paste, Time & Weather, Lights.";
		case R_SPEED:   return "Free camera speed in Edit Camera. 100% = normal.";
		case R_VSPEED:  return "Page Up / Page Down speed.";
		case R_LOOK:    return "Mouse and stick turn speed.";
		case R_FREE:
			snprintf(buf, sizeof(buf), "Fly without changing this keyframe's camera; off puts it back. %s also does it. %s",
				keyName(g_cfg.keyFreeLook, s, sizeof(s)), camfree::g_last);
			return buf;
		case R_FROM:    return "First keyframe to copy.";
		case R_TO:      return "Last keyframe to copy.";
		case R_COPY:    snprintf(buf, sizeof(buf), "Copy the keyframes. Clipboard: %s. %s", clipboard::summary(s, sizeof(s)), clipboard::g_last); return buf;
		case R_CUT:     snprintf(buf, sizeof(buf), "Copy, then remove them when you paste. %s", clipboard::g_last); return buf;
		case R_PASTE:   snprintf(buf, sizeof(buf), "Paste onto this keyframe, the rest follow. %s", clipboard::g_last); return buf;
		case R_TIME:    return "Clip time of day. As Recorded = the clip's own.";
		case R_WEATHER: return "Clip weather. As Recorded = the clip's own.";
		case R_DCOPY:   snprintf(buf, sizeof(buf), "Copy this keyframe's DOF. %s", dof::g_last); return buf;
		case R_DPASTE:  snprintf(buf, sizeof(buf), "Paste the copied DOF here. %s", dof::g_last); return buf;
		case R_DALL:    snprintf(buf, sizeof(buf), "Put this DOF on every keyframe. %s", dof::g_last); return buf;
		case R_LOPEN:
			if (!lightui::g_imgui) return "Needs ReShade with add-on support: the light editor is a ReShade window.";
			snprintf(buf, sizeof(buf), "Place lights with a 3D gizmo. %s opens and closes it too.", keyName(g_cfg.keyLights, s, sizeof(s)));
			return buf;
		case R_LALL:    return "On: this clip's lights are the whole project's - every edit lands in every clip.";
		case R_LKEY:
			if (lights::g_capture || lights::since(GetTickCount64(), lights::g_keyMsgAt) < 6000) return lights::g_keyMsg;
			return "Left/right: pick a key. Enter: then press any key.";
		default: return "";
		}
	}

	int nextPage(int from, int delta)
	{
		int p = from;
		for (int i = 0; i < PAGE_COUNT; ++i)
		{
			p = (p + delta + PAGE_COUNT) % PAGE_COUNT;
			if (pageAvailable(p)) return p;
		}
		return PAGE_CLOSED;
	}

	// Left/right on a row. Returns true when the column must be rebuilt.
	bool adjust(int r, int delta)
	{
		const ULONGLONG now = GetTickCount64();
		switch (r)
		{
		case R_HEADER:
			g_page = nextPage(g_page, delta);
			iniWriteInt("MenuPage", g_page);
			return true;
		case R_SPEED:
			step(delta, now);
			break;
		case R_VSPEED:
		{
			const int cur = g_cfg.vertical <= 0 ? 0 : 1 + nearestPreset(g_cfg.vertical);
			int next = cur + delta;
			if (next < 0) next = 0;
			if (next > kPresetCount) next = kPresetCount;
			g_cfg.vertical = next > 0 ? kPresets[next - 1] : 0;
			iniWriteInt("VerticalSpeed", g_cfg.vertical);
			break;
		}
		case R_FREE:    camfree::request(); break;
		case R_LOOK:
		{
			int i = nearestPreset(g_cfg.look) + delta;
			if (i < 0) i = 0;
			if (i >= kPresetCount) i = kPresetCount - 1;
			g_cfg.look = kPresets[i];
			iniWriteInt("LookSpeed", g_cfg.look);
			break;
		}
		case R_FROM:    clipboard::stepRange(true, delta);  return true;
		case R_TO:      clipboard::stepRange(false, delta); return true;
		case R_TIME:    scene::stepTime(delta);    break;
		case R_WEATHER: scene::stepWeather(delta); break;
		case R_LKEY:    lights::stepShortcut(delta); break;
		case R_LALL:    lights::toggleAllClips(); break;
		default: break;
		}
		return false;
	}

	// Accept on a row. Returns true when the column must be rebuilt.
	bool accept(int r)
	{
		switch (r)
		{
		case R_FREE:   camfree::request(); return false;
		case R_LALL:   lights::toggleAllClips(); return false;
		case R_COPY:   clipboard::copy(false); return false;
		case R_CUT:    clipboard::copy(true);  return false;
		case R_PASTE:  return clipboard::paste();
		case R_DCOPY:  dof::copy();            return false;
		case R_DPASTE: return dof::paste();          // the stock DOF rows show the new values after a rebuild
		case R_DALL:   dof::applyToAll();      return false;
		case R_LOPEN:  if (lightui::g_imgui) InterlockedExchange(&lights::g_openReq, 1); return false;
		case R_LKEY:   lights::beginCapture(lights::CAP_MENU); return false;
		default: return false;
		}
	}

	// --- ownership -------------------------------------------------------
	bool ownsRow(int idx)
	{
		if (g_count <= 0 || g_first < 0 || idx < g_first || idx >= g_first + g_count) return false;
		const auto* arr = (const MenuArray*)a_MenuOptions;
		if (!arr->data || idx >= (int)arr->count) return false;
		return arr->data[idx].id == kOurOptionId;
	}

	int logicalAt(int idx)
	{
		const int i = idx - g_first;
		return (i >= 0 && i < s_shown) ? s_rows[i] : R_HEADER;
	}

	bool isStockSpeedRow(int idx)
	{
		if (idx < 0 || ownsRow(idx)) return false;
		const auto* arr = (const MenuArray*)a_MenuOptions;
		if (!arr->data || idx >= (int)arr->count) return false;
		return arr->data[idx].id == clipspeed::kStockOptionId;
	}

	int stockSpeedRow()
	{
		const auto* arr = (const MenuArray*)a_MenuOptions;
		if (!arr->data) return -1;
		for (int i = 0; i < (int)arr->count; ++i)
			if (arr->data[i].id == clipspeed::kStockOptionId && !ownsRow(i)) return i;
		return -1;
	}

	const char* speedHelp(int pct)
	{
		static char buf[96];
		snprintf(buf, sizeof(buf), "Clip speed %d%%. 5%% steps, 5%% to 1000%%.", pct);
		return buf;
	}

	bool fromMouse()
	{
		if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) return true;
		return GetTickCount64() - g_lastClick < 300;
	}

	void injectRows(int kind)
	{
		g_kind = kind;
		if (kind == K_MARKER)
		{
			if (!pageAvailable(g_page)) g_page = PAGE_CAMSPEED;
			if (g_page == PAGE_CLIPBOARD) clipboard::resetRange();
		}
		g_first = optionCount();
		buildRows();
		char v[64];
		for (int i = 0; i < s_shown; ++i)
		{
			const int r = s_rows[i];
			addRow(rowLabel(r), isAction(r) ? nullptr : rowValue(r, v, sizeof(v)), rowToggles(r));
		}
		g_count = optionCount() - g_first;

		if (g_injectLogs < 8)
		{
			++g_injectLogs;
			const int total = g_first + g_count;
			logf("menu: %d row(s) added at index %d (%s), %d in the column%s", g_count, g_first,
				kind == K_DOF ? "Depth of Field menu" : pageName(g_page), total,
				total > kColumnCapacity ? " - MORE THAN THE COLUMN DRAWS, the last will be hidden" : "");
		}
	}

	void refreshRow(int idx, int r)
	{
		char v[64];
		if (!isAction(r)) updateText(idx, rowValue(r, v, sizeof(v)));
		emitHelp(rowHelp(r));
	}

	void refreshPage(int focus)
	{
		char v[64];
		for (int i = 0; i < s_shown; ++i)
			if (!isAction(s_rows[i])) updateText(g_first + i, rowValue(s_rows[i], v, sizeof(v)));
		if (ownsRow(focus)) emitHelp(rowHelp(logicalAt(focus)));
	}

	// The marker menu does not clear the option array itself (its caller
	// does); the DOF menu is rebuilt the way the game rebuilds it after a
	// focus-mode change: clear, then populate.
	void rebuild(int focus)
	{
		if (g_kind == K_DOF)
		{
			if (!a_PopulateDofMenu) return;
			if (a_ClearOptions) ((FnClear)a_ClearOptions)();
			else if (auto* arr = (MenuArray*)a_MenuOptions) arr->count = 0;
			((FnPopulate)a_PopulateDofMenu)((unsigned)focus);
			return;
		}
		if (auto* arr = (MenuArray*)a_MenuOptions) arr->count = 0;
		((FnPopulate)a_PopulateMarkerMenu)((unsigned)focus);
	}

	void relabelSpeedRow()
	{
		if (!clipspeed::active()) return;
		const int row = stockSpeedRow();
		if (row < 0) return;
		const int pct = clipspeed::currentPercent();
		if (pct < 0) return;
		char b[16];
		updateText(row, clipspeed::label(pct, b, sizeof(b)));
	}

	// --- hooks -------------------------------------------------------------
	void __fastcall hkPopulateMarker(unsigned focusToRestore)
	{
		const bool outer = !g_inMenu && !g_inDof;
		if (outer) { g_inMenu = true; g_injected = false; }
		origPopulateMarker(focusToRestore);
		if (outer) { g_inMenu = false; relabelSpeedRow(); }
	}

	void __fastcall hkPopulateDof(unsigned focusToRestore)
	{
		const bool outer = !g_inMenu && !g_inDof;
		if (outer) { g_inDof = true; g_injected = false; }
		origPopulateDof(focusToRestore);
		if (outer) g_inDof = false;
	}

	char __fastcall hkBeginMethod(int movie, int cls, const char* method, int a, void* b)
	{
		if ((g_inMenu || g_inDof) && !g_injecting && !g_injected && method &&
		    strcmp(method, "ADD_COLUMN_HELP_TEXT") == 0)
		{
			g_injected  = true;
			g_injecting = true;
			injectRows(g_inDof ? K_DOF : K_MARKER);
			g_injecting = false;
		}
		return origBeginMethod(movie, cls, method, a, b);
	}

	void __fastcall hkUpdateHelp(unsigned idx)
	{
		const bool ours = ownsRow((int)idx);
		const int  r = ours ? logicalAt((int)idx) : -1;
		// A pick begun on the Light Editor Key row ends when the focus leaves the row.
		if (lights::g_capture && lights::g_captureSrc == lights::CAP_MENU && r != R_LKEY) lights::cancelCapture(lights::END_CANCELLED, "Cancelled");
		if (ours) { if (r == R_LKEY) refreshRow((int)idx, r); else emitHelp(rowHelp(r)); return; }
		if (clipspeed::active() && isStockSpeedRow((int)idx))
		{
			const int pct = clipspeed::currentPercent();
			if (pct >= 0) { emitHelp(speedHelp(pct)); return; }
		}
		origUpdateHelp(idx);
	}

	void __fastcall hkMenuInput(int nav)
	{
		const int  focus = *(int*)a_FocusIndex;
		const bool lr    = nav == NAV_RIGHT || nav == NAV_LEFT;
		const bool acc   = nav == NAV_ACCEPT;
		if (ownsRow(focus))
		{
			if (!lr && !acc) { origMenuInput(nav); return; }
			if (fromMouse())
			{
				if (g_mouseDrops++ < 3) logf("menu: mouse press on our row ignored (keyboard only)");
				return;
			}
			const int r = logicalAt(focus);
			if (lr)
			{
				const bool rangeRow = r == R_FROM || r == R_TO;
				if (adjust(r, nav == NAV_RIGHT ? 1 : -1) && !rangeRow) rebuild(focus);
				else if (rangeRow) refreshPage(focus);
				else refreshRow(focus, r);
				return;
			}
			if (accept(r)) rebuild(focus);
			else refreshRow(focus, r);
			return;
		}
		if ((lr || acc) && clipspeed::active() && isStockSpeedRow(focus))
		{
			if (fromMouse()) return;
			if (lr)
			{
				const int pct = clipspeed::step(nav == NAV_RIGHT ? 1 : -1);
				if (pct >= 0)
				{
					char b[16];
					updateText(focus, clipspeed::label(pct, b, sizeof(b)));
					emitHelp(speedHelp(pct));
					return;
				}
			}
		}
		origMenuInput(nav);
	}

	// --- the light editor's key, caught up on the game thread ----------------------
	// A key picked for the light editor arrives from the worker or from
	// ReShade's thread. The director hook calls this every frame on the game's
	// thread, like the menu itself, and the Light Editor Key row and its help
	// line are redrawn when the focus is still on that row. A pick that was
	// cancelled or ran out is left alone: Esc and Backspace also leave the menu.
	LONG s_keySeen = 0;

	void tick()
	{
		const LONG seq = lights::g_captureSeq;
		if (seq == s_keySeen) return;
		s_keySeen = seq;
		const LONG end = lights::g_captureEnd;
		if (!g_ready || !editorOpen() || g_kind != K_MARKER || (end != lights::END_SAVED && end != lights::END_REFUSED)) return;
		const int focus = *(int*)a_FocusIndex;
		if (ownsRow(focus) && logicalAt(focus) == R_LKEY) refreshRow(focus, R_LKEY);
	}

	// --- install -----------------------------------------------------------
	bool install()
	{
		if (!g_cfg.menuRows) { logf("menu: MenuRows=0 - no rows, keys only"); return false; }
		if (isEnhanced())    { logf("menu: GTA V Enhanced is not mapped for the menu rows - keys only"); return false; }

		g_page = g_cfg.menuPage;
		if (rePlusLoaded() && g_page != PAGE_CLOSED)
		{
			g_page = PAGE_CLOSED;
			logf("menu: Rockstar Editor+ is loaded, so the EscoEditor row starts Closed - the game's column draws %d rows and both plugins share it. "
				"Left/right on the row opens a page as usual.", kColumnCapacity);
		}
		logf("menu: resolving the menu functions");
		const uintptr_t pcm = game::a_PopulateCameraMenu ? game::a_PopulateCameraMenu : game::scanFn(game::kPcm, "PopulateCameraMenu");
		a_PopulateMarkerMenu = game::scanFn(kPmm, "PopulateMarkerMenu");
		a_MenuInput          = game::scanFn(kMi,  "MenuInput");
		a_UpdateItemText     = game::scanFn(kUit, "UpdateItemTextValue");
		a_UpdateMenuHelpText = game::scanFn(kUmh, "UpdateMenuHelpText");
		if (!pcm || !a_PopulateMarkerMenu || !a_MenuInput)
		{
			logf("menu: patterns missing on this build - no menu rows (the keys still work)");
			return false;
		}

		a_MovieId        = derive(pcm, 0x91,  0x93,  OP_MOV_ECX, 2, "g_MovieId");
		a_BeginMethod    = derive(pcm, 0xA9,  0xAA,  OP_CALL,    1, "BeginMethod");
		a_AddParamString = derive(pcm, 0xC6,  0xC7,  OP_CALL,    1, "AddParamString");
		a_EndMethod      = derive(pcm, 0xDB,  0xDC,  OP_CALL,    1, "EndMethod");
		a_MenuOptions    = derive(pcm, 0x186, 0x189, OP_LEA_RCX, 3, "g_MenuOptions");
		a_ArrayGrow      = derive(pcm, 0x197, 0x198, OP_CALL,    1, "ArrayGrow");
		a_FocusIndex     = derive(a_MenuInput, 0x3C, 0x3E, OP_MOV_EAX, 2, "g_MenuFocusIndex");
		if (!a_MovieId || !a_BeginMethod || !a_AddParamString || !a_EndMethod ||
		    !a_MenuOptions || !a_ArrayGrow || !a_FocusIndex)
		{
			logf("menu: an interior offset did not verify - no menu rows (the keys still work)");
			return false;
		}

		// The Depth of Field submenu: the dispatcher rebuilds it after a
		// focus-mode change with ClearOptions() then PopulateDofMenu(0).
		a_ClearOptions    = derive(a_MenuInput, 0x92A, 0x92B, OP_CALL, 1, "ClearMenuOptions");
		a_PopulateDofMenu = derive(a_MenuInput, 0x931, 0x932, OP_CALL, 1, "PopulateDofMenu");
		if (a_PopulateDofMenu && memcmp((const void*)a_PopulateDofMenu, kDofPrologue, sizeof(kDofPrologue)) != 0 &&
		    !looksHooked((const uint8_t*)a_PopulateDofMenu))
		{
			logf("menu: PopulateDofMenu does not look as expected - the DOF menu gets no rows");
			a_PopulateDofMenu = 0;
		}

		if (!hookFn(a_PopulateMarkerMenu, (void*)hkPopulateMarker, (void**)&origPopulateMarker, "PopulateMarkerMenu")) return false;
		if (!hookFn(a_BeginMethod,        (void*)hkBeginMethod,    (void**)&origBeginMethod,    "BeginMethod"))        return false;
		if (!hookFn(a_MenuInput,          (void*)hkMenuInput,      (void**)&origMenuInput,      "MenuInput"))          return false;
		if (a_UpdateMenuHelpText &&
		    !hookFn(a_UpdateMenuHelpText, (void*)hkUpdateHelp,     (void**)&origUpdateHelp,     "UpdateMenuHelpText"))
			logf("menu: help line unavailable - rows will show a blank help text");
		if (a_PopulateDofMenu &&
		    !hookFn(a_PopulateDofMenu,    (void*)hkPopulateDof,    (void**)&origPopulateDof,    "PopulateDofMenu"))
			a_PopulateDofMenu = 0;

		g_ready = true;
		logf("menu: rows installed - marker menu: EscoEditor at the bottom%s",
			a_PopulateDofMenu ? "; Depth of Field menu: Copy DOF, Paste DOF, DOF To All" : "");
		return true;
	}
}
