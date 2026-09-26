	constexpr const char* kVersion = "4.22";

	// ------------------------------------------------------------------ paths
	char    g_dir[MAX_PATH]  = {};   // folder of the .asi, trailing backslash
	char    g_ini[MAX_PATH]  = {};
	char    g_log[MAX_PATH]  = {};
	HMODULE g_self           = nullptr;
	bool    g_logOn          = true;

	void logf(const char* fmt, ...)
	{
		if (!g_logOn || !g_log[0]) return;
		FILE* f = nullptr;
		if (fopen_s(&f, g_log, "ab") != 0 || !f) return;
		SYSTEMTIME st; GetLocalTime(&st);
		fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
		va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
		fputc('\n', f);
		fclose(f);
	}

	// ----------------------------------------------------------------- config
	constexpr const char* kSection = "EscoEditor";

	struct Config
	{
		int   speed     = 100;    // % of stock, walking
		int   vertical  = 0;      // % for Page Up/Down; 0 = same as speed
		int   look      = 100;    // % of stock turn speed (stick response + mouse multiplier)
		int   response  = 1;      // 0 stock ramp, 1 ramp scales with speed, 2 instant
		int   hotkeys   = 2;      // 0 off, 1 step keys, 2 + hold keys, 3 + wheel
		int   keySlower = 0x46;   // F
		int   keyFaster = 0x47;   // G
		int   keyReset  = 0x30;   // 0
		int   keyBoost  = 0xA0;   // Left Shift
		int   keySlow   = 0xA4;   // Left Alt
		float boostMul  = 3.0f;
		float slowMul   = 0.25f;
		bool  anywhere  = false;  // keys also in the editor menus, not only Edit Camera
		bool  menuRows  = true;   // rows in the editor's marker menu (needs 4 code hooks)
		bool  log       = true;
		int   menuPage  = 1;      // EscoEditor page shown by default (0 closed, 1 Camera Speed, 2 Copy & Paste, 3 Time & Weather, 4 Lights)
   // take over the stock Speed row: 5% steps, 5% .. 1000%
		bool  scene     = true;   // Time of Day / Weather rows
		bool  lights    = true;   // scene lights, and the light editor on KeyLights
		int   keyLights = 0x4C;   // L
		int   keyFreeLook = 0x4B; // K
		bool  lightsAll = false;  // the lights of one clip are the lights of every clip
		bool  enbDof = false;     // ENB draws the Rockstar Editor's depth of field
		bool  enbFollow = false;  // ... a Custom keyframe's Focal Distance / Intensity drive ENB (opt-in since 4.11)
		float enbStrength = 1.0f; // times the aperture a keyframe's intensity gives (100% = NVE's 0.5)
		bool  enbAll = false;     // 4.18: a change on an ENB row goes to every keyframe of the clip
		bool  enbMeasure = true;  // 4.19: EscoFocus measures the middle of the picture (Auto focus, Enter on Focus Distance)
	} g_cfg;

	// When the left mouse button was last seen down (worker clock); the menu
	// drops presses that arrive within a third of a second of it.
	volatile ULONGLONG g_lastClick = 0;

	const char* kDefaultIni =
		"; EscoEditor - more control for the Rockstar Editor (standalone .asi).\r\n"
		"; Keys are Windows virtual-key codes, decimal or 0x hex:\r\n"
		";   https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes\r\n"
		"; An inline \"; comment\" after a number is fine.\r\n"
		"\r\n"
		"[EscoEditor]\r\n"
		"Speed=100            ; % of the game's free camera speed (100 = the stock 10 m/s). 5..1000\r\n"
		"VerticalSpeed=0      ; % for Page Up / Page Down. 0 = same as Speed\r\n"
		"LookSpeed=100        ; % of the stock turn (look) speed, stick and mouse\r\n"
		"Response=1           ; 0 stock ramp, 1 ramp scales with the speed, 2 instant start/stop\r\n"
		"MenuRows=1           ; 1 = the EscoEditor rows in the editor's marker menu, 0 = keys only\r\n"
	"MenuPage=1           ; page EscoEditor opens on: 0 closed, 1 Camera Speed, 2 Copy & Paste, 3 Time & Weather, 4 Lights\r\n"
	"Scene=1              ; 1 = Time of Day / Weather rows, 0 = off\r\n"
	"Lights=1             ; 1 = scene lights in the Rockstar Editor, placed in the light editor (its window needs ReShade with add-on support)\r\n"
	"KeyLights=0x4C       ; L - opens / closes the light editor inside a clip (the Lights page of the menu changes it)\r\n"
		"Hotkeys=2            ; 0 off, 1 step keys, 2 step + hold keys, 3 step + hold + mouse wheel\r\n"
		"KeySlower=0x46       ; F      - one preset slower\r\n"
		"KeyFaster=0x47       ; G      - one preset faster\r\n"
		"KeyReset=0x30        ; 0      - back to 100%\r\n"
		"KeyBoost=0xA0        ; Left Shift - hold for BoostMul x\r\n"
		"BoostMul=3\r\n"
		"KeySlow=0xA4         ; Left Alt   - hold for SlowMul x\r\n"
		"SlowMul=0.25\r\n"
		"HotkeysAnywhere=0    ; 1 = keys also count in the editor menus, not only in Edit Camera\r\n"
		"Log=1                ; write EscoEditor.log next to the .asi\r\n";

	void stripComment(char* s)
	{
		if (char* semi = strchr(s, ';')) *semi = '\0';
		size_t n = strlen(s);
		while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r' || s[n-1] == '\n')) s[--n] = '\0';
	}

	int iniInt(const char* key, int def, int lo, int hi)
	{
		char b[64] = {}, d[32] = {};
		sprintf_s(d, "%d", def);
		GetPrivateProfileStringA(kSection, key, d, b, sizeof(b), g_ini);
		stripComment(b);
		char* end = nullptr;
		long v = strtol(b, &end, 0);      // base 0: decimal or 0x hex
		if (end == b) v = def;
		if (v < lo) v = lo;
		if (v > hi) v = hi;
		return (int)v;
	}

	float iniFloat(const char* key, float def, float lo, float hi)
	{
		char b[64] = {}, d[32] = {};
		sprintf_s(d, "%g", def);
		GetPrivateProfileStringA(kSection, key, d, b, sizeof(b), g_ini);
		stripComment(b);
		char* end = nullptr;
		double v = strtod(b, &end);
		if (end == b || !(v == v)) v = def;
		if (v < lo) v = lo;
		if (v > hi) v = hi;
		return (float)v;
	}

	// A key setting this version has and the ini does not: written in, so it is
	// visible and editable there instead of being an invisible default.
	// Same, for a plain number.
	void iniEnsureInt(const char* key, int v)
	{
		char b[16] = {};
		GetPrivateProfileStringA(kSection, key, "?", b, sizeof(b), g_ini);
		if (strcmp(b, "?") != 0) return;
		char s[16];
		snprintf(s, sizeof(s), "%d", v);
		WritePrivateProfileStringA(kSection, key, s, g_ini);
	}

	void iniEnsureKey(const char* key, int vk)
	{
		char b[16] = {};
		GetPrivateProfileStringA(kSection, key, "?", b, sizeof(b), g_ini);
		if (strcmp(b, "?") != 0) return;
		char v[16];
		snprintf(v, sizeof(v), "0x%02X", vk);
		WritePrivateProfileStringA(kSection, key, v, g_ini);
	}

	void iniWriteInt(const char* key, int v)
	{
		char b[32]; sprintf_s(b, "%d", v);
		WritePrivateProfileStringA(kSection, key, b, g_ini);
	}

	// The settings of an EditorCamSpeed.ini (the plugin's old name) are
	// carried over the first time: same keys, the section is renamed.
	void migrateOldIni()
	{
		char old[MAX_PATH];
		strcpy_s(old, g_dir); strcat_s(old, "EditorCamSpeed.ini");
		FILE* in = nullptr;
		if (fopen_s(&in, old, "rb") != 0 || !in) return;
		FILE* out = nullptr;
		if (fopen_s(&out, g_ini, "wb") != 0 || !out) { fclose(in); return; }
		char line[1024];
		while (fgets(line, sizeof(line), in))
		{
			if (strncmp(line, "[EditorCamSpeed]", 16) == 0) fputs("[EscoEditor]\r\n", out);
			else fputs(line, out);
		}
		fclose(in); fclose(out);
	}

	// Keys of what is gone (2.5 - 2.8's EscoDOF and its Depth of Field rows),
	// taken out of the ini so it lists only what still does something.
	void dropRetiredKeys()
	{
		static const char* const kRetired[] = {
			"ClipSpeed", "EnbDofScale", "EnbDofFollow", "EscoDOF", "DofAmount", "DofSmooth", "DofNear", "DofQuality", "DofBokeh", "DofAnamorphic",
			"DofAnamorphicAmount", "DofStretch", "DofChroma", "DofRadial", "DofHighlight", "DofPage", "DofEngine" };
		for (const char* key : kRetired)
		{
			char b[8] = {};
			GetPrivateProfileStringA(kSection, key, "?", b, sizeof(b), g_ini);
			if (strcmp(b, "?") != 0) WritePrivateProfileStringA(kSection, key, nullptr, g_ini);
		}
	}

	void loadConfig()
	{
		if (GetFileAttributesA(g_ini) == INVALID_FILE_ATTRIBUTES)
		{
			migrateOldIni();
			if (GetFileAttributesA(g_ini) == INVALID_FILE_ATTRIBUTES)
			{
				FILE* f = nullptr;
				if (fopen_s(&f, g_ini, "wb") == 0 && f) { fputs(kDefaultIni, f); fclose(f); }
			}
		}
		g_cfg.speed     = iniInt("Speed",           g_cfg.speed,     1, 2000);
		g_cfg.vertical  = iniInt("VerticalSpeed",   g_cfg.vertical,  0, 2000);
		g_cfg.look      = iniInt("LookSpeed",       g_cfg.look,      1, 2000);
		g_cfg.response  = iniInt("Response",        g_cfg.response,  0, 2);
		g_cfg.hotkeys   = iniInt("Hotkeys",         g_cfg.hotkeys,   0, 3);
		g_cfg.keySlower = iniInt("KeySlower",       g_cfg.keySlower, 0, 0xFE);
		g_cfg.keyFaster = iniInt("KeyFaster",       g_cfg.keyFaster, 0, 0xFE);
		g_cfg.keyReset  = iniInt("KeyReset",        g_cfg.keyReset,  0, 0xFE);
		g_cfg.keyBoost  = iniInt("KeyBoost",        g_cfg.keyBoost,  0, 0xFE);
		g_cfg.keySlow   = iniInt("KeySlow",         g_cfg.keySlow,   0, 0xFE);
		g_cfg.boostMul  = iniFloat("BoostMul",      g_cfg.boostMul,  0.01f, 50.0f);
		g_cfg.slowMul   = iniFloat("SlowMul",       g_cfg.slowMul,   0.01f, 50.0f);
		g_cfg.anywhere  = iniInt("HotkeysAnywhere", 0, 0, 1) != 0;
		g_cfg.menuRows  = iniInt("MenuRows",        1, 0, 1) != 0;
		g_cfg.log       = iniInt("Log",             1, 0, 1) != 0;
		g_cfg.menuPage  = iniInt("MenuPage",        1, 0, 4);
		g_cfg.scene     = iniInt("Scene",           1, 0, 1) != 0;
		g_cfg.lights    = iniInt("Lights",          1, 0, 1) != 0;
		g_cfg.keyLights = iniInt("KeyLights",       g_cfg.keyLights, 0, 0xFE);
		g_cfg.keyFreeLook = iniInt("KeyFreeLook",   g_cfg.keyFreeLook, 0, 0xFE);
		g_cfg.lightsAll = iniInt("LightsAllClips",  0, 0, 1) != 0;
		g_cfg.enbDof    = iniInt("EnbDof",          0, 0, 1) != 0;
		g_cfg.enbFollow = iniInt("EnbDofFollowGame", 0, 0, 1) != 0;
		g_cfg.enbStrength = iniFloat("EnbDofStrength", 1.0f, 0.0f, 20.0f);
		g_cfg.enbAll    = iniInt("EnbDofAllKeyframes", 0, 0, 1) != 0;
		g_cfg.enbMeasure = iniInt("EnbDofMeasure", 1, 0, 1) != 0;
		g_logOn = g_cfg.log;
		iniEnsureKey("KeyFreeLook", g_cfg.keyFreeLook);   // new in 3.9
		iniEnsureInt("LightsAllClips", 0);                // new in 3.11
		iniEnsureInt("EnbDof", 0);                        // new in 4.6
		iniEnsureInt("EnbDofFollowGame", 0);            // 4.11: replaces EnbDofFollow, off
		iniEnsureInt("EnbDofAllKeyframes", 0);          // new in 4.18
		iniEnsureInt("EnbDofMeasure", 1);               // new in 4.19
		{
			char b[8] = {};
			GetPrivateProfileStringA(kSection, "EnbDofStrength", "?", b, sizeof(b), g_ini);   // new in 4.10
			if (!strcmp(b, "?")) WritePrivateProfileStringA(kSection, "EnbDofStrength", "1.0", g_ini);
		}
		dropRetiredKeys();
	}

	void saveSpeed() { iniWriteInt("Speed", g_cfg.speed); }

	// ---------------------------------------------------------------- presets
	const int kPresets[] = { 5, 10, 15, 20, 25, 30, 40, 50, 60, 75, 100,
	                         125, 150, 200, 250, 300, 400, 500, 750, 1000 };
	constexpr int kPresetCount = (int)(sizeof(kPresets) / sizeof(kPresets[0]));

	int nearestPreset(int pct)
	{
		int best = 0;
		for (int i = 1; i < kPresetCount; ++i)
			if (abs(kPresets[i] - pct) < abs(kPresets[best] - pct)) best = i;
		return best;
	}

	// -------------------------------------------------------------- key names
	// ASCII only: on a French layout the "0" key is named "à", which is one
	// byte of ANSI that reads as garbage in a UTF-8 log.
	const char* keyName(int vk, char* buf, int cap)
	{
		if (cap <= 0) return "";
		buf[0] = '\0';
		if (vk <= 0) { snprintf(buf, cap, "none"); return buf; }
		UINT sc = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
		switch (vk)
		{
		case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT:
		case VK_END: case VK_HOME: case VK_INSERT: case VK_DELETE: case VK_DIVIDE:
		case VK_NUMLOCK: case VK_RCONTROL: case VK_RMENU:
			sc |= 0x100; break;
		default: break;
		}
		if (sc && GetKeyNameTextA((LONG)(sc << 16), buf, cap) > 0)
		{
			bool ascii = true;
			for (const char* c = buf; *c; ++c) if ((unsigned char)*c >= 0x80) { ascii = false; break; }
			if (ascii) return buf;
		}
		if (vk >= '0' && vk <= '9') { snprintf(buf, cap, "%c", (char)vk); return buf; }
		snprintf(buf, cap, "0x%02X", vk);
		return buf;
	}

	// ------------------------------------------------------------ the process
	const char* exeName()
	{
		static char name[MAX_PATH] = {};
		if (!name[0])
		{
			char path[MAX_PATH] = {};
			GetModuleFileNameA(nullptr, path, MAX_PATH);
			const char* s = strrchr(path, '\\');
			strcpy_s(name, s ? s + 1 : path);
		}
		return name;
	}

	bool isEnhanced() { return _stricmp(exeName(), "GTA5_Enhanced.exe") == 0; }

	bool isFiveM()
	{
		return GetModuleHandleA("asi-five.dll") || GetModuleHandleA("citizen-game-main.dll") ||
		       strstr(exeName(), "GTAProcess") != nullptr;
	}

	bool isSupportedGame()
	{
		return isFiveM() || _stricmp(exeName(), "GTA5.exe") == 0 || isEnhanced();
	}

	// ------------------------------------------------------- pattern scanning
	struct Pattern { uint8_t b[64]; uint8_t m[64]; int n; };

	Pattern parsePattern(const char* s)
	{
		Pattern p = {};
		for (const char* c = s; *c && p.n < 64; )
		{
			if (*c == ' ') { ++c; continue; }
			if (*c == '?') { while (*c == '?') ++c; p.b[p.n] = 0; p.m[p.n] = 0; ++p.n; continue; }
			auto hex = [](char h) -> int {
				if (h >= '0' && h <= '9') return h - '0';
				if (h >= 'a' && h <= 'f') return h - 'a' + 10;
				if (h >= 'A' && h <= 'F') return h - 'A' + 10;
				return -1;
			};
			const int hi = hex(c[0]); const int lo = hex(c[1]);
			if (hi < 0 || lo < 0) { ++c; continue; }
			p.b[p.n] = (uint8_t)((hi << 4) | lo); p.m[p.n] = 1; ++p.n; c += 2;
		}
		return p;
	}

	// Scans one committed, readable range. Plain C on purpose: the __try must
	// not share a frame with anything that needs unwinding.
	const uint8_t* scanRange(const uint8_t* base, size_t size, const Pattern& p)
	{
		__try
		{
			if (size < (size_t)p.n) return nullptr;
			const uint8_t first = p.b[0];
			const size_t last = size - (size_t)p.n;
			for (size_t i = 0; i <= last; ++i)
			{
				if (p.m[0] && base[i] != first) continue;
				int j = 1;
				for (; j < p.n; ++j)
					if (p.m[j] && base[i + j] != p.b[j]) break;
				if (j == p.n) return base + i;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
		return nullptr;
	}

	uintptr_t g_modBase = 0, g_modSize = 0;

	// The main module, page by page (the game image has holes and guard pages).
	const uint8_t* scanModule(const Pattern& p)
	{
		const auto* base = (const uint8_t*)GetModuleHandleA(nullptr);
		if (!base) return nullptr;
		const auto* dos = (const IMAGE_DOS_HEADER*)base;
		if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
		const auto* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
		g_modBase = (uintptr_t)base;
		g_modSize = nt->OptionalHeader.SizeOfImage;

		uintptr_t addr = g_modBase;
		const uintptr_t end = g_modBase + g_modSize;
		while (addr < end)
		{
			MEMORY_BASIC_INFORMATION mbi;
			if (!VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi))) break;
			const uintptr_t rb = (uintptr_t)mbi.BaseAddress;
			const uintptr_t re = rb + mbi.RegionSize;
			if (re <= addr) break;
			const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
				(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
				                PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY));
			if (readable)
			{
				const uintptr_t lo = addr > rb ? addr : rb;
				const uintptr_t hi = re < end ? re : end;
				if (hi > lo)
					if (const uint8_t* hit = scanRange((const uint8_t*)lo, hi - lo, p)) return hit;
			}
			addr = re;
		}
		return nullptr;
	}

	inline unsigned long long rva(uintptr_t a) { return (unsigned long long)(a ? a - g_modBase : 0); }

	// A RIP-relative operand inside a scanned function: the opcode at `insn`
	// is checked first, the displacement at `disp` is relative to disp+4, and
	// the result must land inside the module.
	uintptr_t derive(uintptr_t base, int insn, int disp, const uint8_t* op, int opLen, const char* what)
	{
		if (!base) return 0;
		if (memcmp((const void*)(base + insn), op, (size_t)opLen) != 0)
		{
			logf("  %s: opcode mismatch at +0x%X - build drifted, not used", what, insn);
			return 0;
		}
		int32_t d = 0; memcpy(&d, (const void*)(base + disp), 4);
		const uintptr_t a = base + disp + 4 + (intptr_t)d;
		if (a < g_modBase || a >= g_modBase + g_modSize)
		{
			logf("  %s: derived %p is outside the module - not used", what, (void*)a);
			return 0;
		}
		logf("  %-22s rva 0x%llX", what, rva(a));
		return a;
	}

	constexpr uint8_t OP_CALL[]    = { 0xE8 };
	constexpr uint8_t OP_MOV_EAX[] = { 0x8B, 0x05 };
	constexpr uint8_t OP_MOV_ECX[] = { 0x8B, 0x0D };
	constexpr uint8_t OP_LEA_RCX[] = { 0x48, 0x8D, 0x0D };

	// --------------------------------------------- sharing the game with others
	// Another plugin's hook is a jump written over a function's first bytes, so
	// a pattern that starts at the function's entry stops matching. MinHook
	// needs at most 14 of them, so: try the pattern, then try it again from 16
	// bytes in and step back. The function is found either way and our own hook
	// chains on top of the other plugin's.
	bool looksHooked(const uint8_t* at)
	{
		__try
		{
			return at && (at[0] == 0xE9 || at[0] == 0xEB || (at[0] == 0xFF && at[1] == 0x25) || at[0] == 0x68);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	const uint8_t* scanFnBytes(const char* sig, bool* hooked)
	{
		*hooked = false;
		const Pattern p = parsePattern(sig);
		if (const uint8_t* hit = scanModule(p)) return hit;
		if (p.n <= 26) return nullptr;              // too short to be unique without its head
		Pattern t = {};
		t.n = p.n - 16;
		memcpy(t.b, p.b + 16, (size_t)t.n);
		memcpy(t.m, p.m + 16, (size_t)t.n);
		if (!t.m[0]) return nullptr;                // a wildcard first byte is no anchor
		const uint8_t* tail = scanModule(t);
		if (!tail || !looksHooked(tail - 16)) return nullptr;
		*hooked = true;
		return tail - 16;
	}

	// Rockstar Editor+ in the process. It hooks five of the same functions we
	// do, and finds them by their first bytes.
	// Rockstar Editor Plus, whatever its file has been renamed to: the loaded
	// module list is walked for a name that contains "rockstareditorplus".
	// Walked out of the PEB so this needs no import of its own - and the file
	// is routinely renamed (RockstarEditorPlus-Custom.asi and the like), which
	// a single GetModuleHandleA would miss, leaving us hooked in front of it.
	bool rePlusLoaded()
	{
		__try
		{
			const uint8_t* peb = (const uint8_t*)__readgsqword(0x60);
			const uint8_t* ldr = *(const uint8_t* const*)(peb + 0x18);
			if (!ldr) return false;
			const LIST_ENTRY* head = (const LIST_ENTRY*)(ldr + 0x20);      // InMemoryOrderModuleList
			for (const LIST_ENTRY* e = head->Flink; e && e != head; e = e->Flink)
			{
				const uint8_t* entry = (const uint8_t*)e - 0x10;            // LDR_DATA_TABLE_ENTRY
				// UNICODE_STRING lives in winternl.h, which clashes with more than
				// it is worth here; the shape is Length, MaximumLength, then the
				// pointer at +8 after the natural padding.
				struct NtStr { uint16_t Length, MaximumLength; const wchar_t* Buffer; };
				const NtStr* nm = (const NtStr*)(entry + 0x58);   // BaseDllName
				if (!nm->Buffer || nm->Length < 2) continue;
				char low[80];
				int n = 0;
				for (int i = 0; i < nm->Length / 2 && n < (int)sizeof(low) - 1; ++i)
				{
					wchar_t w = nm->Buffer[i];
					if (w >= L'A' && w <= L'Z') w = (wchar_t)(w + 32);
					low[n++] = (w < 128) ? (char)w : '_';
				}
				low[n] = 0;
				if (strstr(low, "rockstareditorplus")) return true;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		return false;
	}

	// Looked up once and kept, because the render thread asks every frame.
	// Rockstar Editor Plus rewrites the free camera's response blocks from its
	// own per-frame tick, so ours have to be written after its, every frame, or
	// Look Speed and Response quietly do nothing.
	bool g_rePlus = false;

	// --------------------------------------------------------- game globals
	// camReplayDirector::GetMaxDistanceAllowedFromPlayer opens with a compare
	// of CReplayMgr's mode against 2 (EDIT) and closes with a compare of the
	// director's current marker against the index being camera-edited:
	//
	//   Legacy   83 3D d32 02          cmp [rip+d], 2         -> mode
	//            ...
	//            39 05 d32             cmp [rip+d], eax       -> editing marker index
	//   Enhanced 48 8B 81 F0 02 00 00  mov rax,[rcx+2F0]
	//            F3 0F 10 40 38        movss xmm0,[rax+38]
	//            83 3D d32 02          cmp [rip+d], 2         -> mode
	//            ...
	//            39 0D d32             cmp [rip+d], ecx       -> editing marker index
	//
	// Verified on the decrypted FiveM b3258 / b3407 / b3570 / b3717 images and
	// live on FiveM b3407 (index -1 -> 0 the moment Edit Camera opened).
	struct Sig { const char* pat; int modeDisp, modeNext, idxDisp, idxNext; };

	const Sig kSigLegacy = {
		"83 3D ? ? ? ? 02 4C 8B 81 F0 02 00 00 F3 41 0F 10 40 38 75 1D 84 D2 74 19 "
		"8B 81 30 06 00 00 83 F8 FF 74 0E 39 05",
		2, 7, 39, 43 };
	const Sig kSigEnhanced = {
		"48 8B 81 F0 02 00 00 F3 0F 10 40 38 83 3D ? ? ? ? 02 75 1C 84 D2 74 18 "
		"8B 89 30 06 00 00 83 F9 FF 74 0D 39 0D",
		14, 19, 38, 42 };

	uintptr_t g_maxDistFn = 0;                    // the function the two globals were read from
	volatile const int* g_replayMode = nullptr;   // 2 = editor open
	volatile const int* g_editIndex  = nullptr;   // -1 = not in Edit Camera

	bool resolveGlobals()
	{
		const Sig& sig = isEnhanced() ? kSigEnhanced : kSigLegacy;
		bool hooked = false;
		const uint8_t* hit = scanFnBytes(sig.pat, &hooked);
		if (!hit) return false;
		g_maxDistFn = (uintptr_t)hit;
		if (hooked) logf("globals: GetMaxDistanceAllowedFromPlayer is already hooked by another plugin - read past its jump");

		int32_t d1 = 0, d2 = 0;
		memcpy(&d1, hit + sig.modeDisp, 4);
		memcpy(&d2, hit + sig.idxDisp, 4);
		const uintptr_t a1 = (uintptr_t)(hit + sig.modeNext) + (intptr_t)d1;
		const uintptr_t a2 = (uintptr_t)(hit + sig.idxNext)  + (intptr_t)d2;
		const bool ok1 = a1 >= g_modBase && a1 < g_modBase + g_modSize;
		const bool ok2 = a2 >= g_modBase && a2 < g_modBase + g_modSize;

		logf("globals: GetMaxDistanceAllowedFromPlayer rva 0x%llX; mode %s rva 0x%llX, edit-index %s rva 0x%llX",
			rva((uintptr_t)hit), ok1 ? "at" : "REJECTED", rva(a1), ok2 ? "at" : "REJECTED", rva(a2));

		if (ok1) g_replayMode = (volatile const int*)a1;
		if (ok2) g_editIndex  = (volatile const int*)a2;
		return ok1;
	}

	inline int  replayMode()  { return g_replayMode ? *g_replayMode : -1; }
	inline int  editIndex()   { return g_editIndex ? *g_editIndex : -2; }
	inline bool editorOpen()  { return replayMode() == 2; }
	inline bool editingCam()
	{
		const int i = editIndex();
		return i == -2 ? editorOpen() : i != -1;
	}

	// ----------------------------------------------------- the metadata block
	constexpr uint32_t kNameHash    = 0xCD5692CCu;   // "replay_free_camera"
	constexpr uint32_t OFF_Name     = 0x08;
	constexpr uint32_t OFF_CollRef  = 0x14;
	constexpr uint32_t OFF_DofRef   = 0x18;
	constexpr uint32_t OFF_NearClip = 0x28;
	constexpr uint32_t OFF_RespH    = 0x30;
	constexpr uint32_t OFF_RespV    = 0x48;
	constexpr uint32_t OFF_RespLook = 0x60;
	constexpr uint32_t OFF_MaxPitch = 0x168;
	constexpr uint32_t OFF_MinFov   = 0x16C;
	constexpr uint32_t OFF_MaxFov   = 0x170;
	constexpr uint32_t OFF_DefFov   = 0x174;
	constexpr uint32_t OFF_Capsule  = 0x178;
	constexpr uint32_t OFF_MouseMul = 0x18C;         // RotationMouseInputMultiplier
	constexpr uint32_t kMetaSize    = 0x220;

	constexpr uint32_t RF_Power = 0x08, RF_Accel = 0x0C, RF_Decel = 0x10, RF_Speed = 0x14;

	struct Resp { float power, accel, decel, speed; };
	struct Meta
	{
		uint8_t* p;
		Resp     h, v, look;   // stock values
		float    mouse;        // stock RotationMouseInputMultiplier
	};

	Meta  g_meta[4] = {};
	int   g_metaCount = 0;

	inline float rdf(const uint8_t* m, uint32_t off) { float f; memcpy(&f, m + off, 4); return f; }
	inline uint32_t rdu(const uint8_t* m, uint32_t off) { uint32_t u; memcpy(&u, m + off, 4); return u; }

	Resp readResp(const uint8_t* m, uint32_t off)
	{
		return { rdf(m, off + RF_Power), rdf(m, off + RF_Accel), rdf(m, off + RF_Decel), rdf(m, off + RF_Speed) };
	}

	// The fingerprint rests ONLY on fields nothing rewrites at runtime: the
	// object's name and reference hashes, the near clip, the eight input curve
	// exponents and the default FOV. Deliberately NOT MinFov, MaxFov, MaxPitch,
	// the collision radius, the accelerations or the speeds: NoEditorRestrictions
	// rewrites the first four when the free camera is created, Rockstar Editor+
	// rewrites the FOV pair every frame, and this plugin rewrites the rest.
	bool fingerprint(const uint8_t* m)
	{
		if (rdu(m, OFF_Name) != kNameHash) return false;
		if (rdu(m, OFF_CollRef) == 0 || rdu(m, OFF_DofRef) == 0) return false;
		const float nearClip = rdf(m, OFF_NearClip);
		if (!(nearClip > 0.0f && nearClip <= 2.0f)) return false;
		for (uint32_t off = OFF_RespH; off <= 0xD8; off += 0x18)
		{
			const float pw = rdf(m, off + RF_Power);
			if (!(pw > 0.0f && pw <= 16.0f)) return false;
		}
		const float defFov = rdf(m, OFF_DefFov);
		return defFov >= 1.0f && defFov <= 130.0f;
	}

	// First sight only: the numbers about to become "stock" must make sense.
	bool acceptable(const uint8_t* m)
	{
		if (!fingerprint(m)) return false;
		const Resp r[3] = { readResp(m, OFF_RespH), readResp(m, OFF_RespV), readResp(m, OFF_RespLook) };
		for (const Resp& x : r)
			if (!(x.accel > 0.0f && x.accel <= 1e7f && x.decel > 0.0f && x.decel <= 1e7f &&
			      x.speed > 0.0f && x.speed <= 10000.0f)) return false;
		const float mouse = rdf(m, OFF_MouseMul);
		return mouse > 0.0f && mouse <= 1000.0f;
	}

	// What the block looks like right now - for the log, so a rejection or a
	// fight with another mod is diagnosable from the file alone.
	const char* describe(const uint8_t* m, char* buf, int cap)
	{
		snprintf(buf, cap,
			"name %08X coll %08X dof %08X nearClip %.3f | move h %.2f (accel %.0f decel %.0f pw %.1f) "
			"v %.2f | look %.2f | mouse %.2f | MaxPitch %.1f MinFov %.2f MaxFov %.1f DefFov %.1f Capsule %.3f",
			rdu(m, OFF_Name), rdu(m, OFF_CollRef), rdu(m, OFF_DofRef), rdf(m, OFF_NearClip),
			rdf(m, OFF_RespH + RF_Speed), rdf(m, OFF_RespH + RF_Accel), rdf(m, OFF_RespH + RF_Decel), rdf(m, OFF_RespH + RF_Power),
			rdf(m, OFF_RespV + RF_Speed), rdf(m, OFF_RespLook + RF_Speed), rdf(m, OFF_MouseMul),
			rdf(m, OFF_MaxPitch), rdf(m, OFF_MinFov), rdf(m, OFF_MaxFov), rdf(m, OFF_DefFov), rdf(m, OFF_Capsule));
		return buf;
	}

	// Candidates inside one heap region. The name hash sits at block+8 and the
	// block is 16-aligned in its PSO page, so the hash lands 8 past a 16-byte
	// boundary. Every 8-aligned dword is examined (lanes 0 and 2 of each
	// 16-byte chunk), four chunks per iteration, with SSE2 - which is what
	// turns a 7-second walk of a 14 GB process into a fraction of that.
	int scanRegionForMeta(uint8_t* base, size_t size, uint8_t** out, int cap, int have)
	{
		__try
		{
			if (size < kMetaSize) return have;
			const __m128i key = _mm_set1_epi32((int)kNameHash);
			uint8_t* p   = (uint8_t*)(((uintptr_t)base + 15) & ~(uintptr_t)15);
			uint8_t* end = base + size;
			for (; p + 64 <= end && have < cap; p += 64)
			{
				const int m0 = _mm_movemask_epi8(_mm_cmpeq_epi32(_mm_load_si128((const __m128i*)(p)),      key));
				const int m1 = _mm_movemask_epi8(_mm_cmpeq_epi32(_mm_load_si128((const __m128i*)(p + 16)), key));
				const int m2 = _mm_movemask_epi8(_mm_cmpeq_epi32(_mm_load_si128((const __m128i*)(p + 32)), key));
				const int m3 = _mm_movemask_epi8(_mm_cmpeq_epi32(_mm_load_si128((const __m128i*)(p + 48)), key));
				if (((m0 | m1 | m2 | m3) & 0x0F0F) == 0) continue;
				const int masks[4] = { m0, m1, m2, m3 };
				for (int c = 0; c < 4 && have < cap; ++c)
				{
					for (int lane = 0; lane < 16; lane += 8)
					{
						if (((masks[c] >> lane) & 0xF) != 0xF) continue;
						uint8_t* q    = p + c * 16 + lane;      // the dword that matched
						uint8_t* cand = q - OFF_Name;
						if (cand < base || cand + kMetaSize > end) continue;
						if (acceptable(cand)) out[have++] = cand;
					}
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
		return have;
	}

	// Walk every committed private read-write region of the process. The
	// metadata lives in the game's heap; DLL images and file mappings are
	// skipped, which also keeps the plugin from matching its own constants.
	int findMetadata()
	{
		uint8_t* found[4] = {};
		int n = 0;
		uintptr_t addr = 0;
		for (;;)
		{
			MEMORY_BASIC_INFORMATION mbi;
			if (!VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi))) break;
			const uintptr_t rb = (uintptr_t)mbi.BaseAddress;
			const uintptr_t re = rb + mbi.RegionSize;
			if (re <= addr) break;
			if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
			    !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
			    (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)))
			{
				n = scanRegionForMeta((uint8_t*)rb, mbi.RegionSize, found, 4, n);
				if (n >= 4) break;
			}
			addr = re;
			if (addr >= 0x00007FFFFFFF0000ull) break;
		}

		g_metaCount = 0;
		for (int i = 0; i < n; ++i)
		{
			Meta& m = g_meta[g_metaCount++];
			m.p     = found[i];
			m.h     = readResp(m.p, OFF_RespH);
			m.v     = readResp(m.p, OFF_RespV);
			m.look  = readResp(m.p, OFF_RespLook);
			m.mouse = rdf(m.p, OFF_MouseMul);
			char d[512];
			logf("metadata: free camera block %d at %p - stock move %.2f m/s (h) / %.2f (v), "
			     "look %.2f, mouse x%.2f; accel %.0f, decel %.0f", i + 1, (void*)m.p,
				m.h.speed, m.v.speed, m.look.speed, m.mouse, m.h.accel, m.h.decel);
			logf("metadata: block %d now reads: %s", i + 1, describe(m.p, d, sizeof(d)));
			if (fabsf(m.h.speed - 10.0f) > 0.01f)
				logf("metadata: note - the stock speed is not the game's 10 m/s, so another mod "
				     "changed it first (NoEditorRestrictions max_*_free_cam_speed?). That value is "
				     "what 100%% means here.");
		}
		return g_metaCount;
	}

	bool metaStillValid(char* why, int cap)
	{
		for (int i = 0; i < g_metaCount; ++i)
		{
			__try
			{
				if (!fingerprint(g_meta[i].p)) { describe(g_meta[i].p, why, cap); return false; }
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { snprintf(why, cap, "access violation reading %p", (void*)g_meta[i].p); return false; }
		}
		return g_metaCount > 0;
	}

	// One response block: MaxSpeed follows the multiplier; how the ramp follows
	// is Response.
	//   0 stock    only the speed changes
	//   1 scaled   the ramp scales with multipliers above 1, so a 5x camera still
	//              reaches full speed in the stock third of a second; below 1 the
	//              stock ramp stays, it is already near-instant there
	//   2 instant  no ramp: the camera stops the moment you let go
	void writeResp(uint8_t* m, uint32_t off, const Resp& o, float mul, int response)
	{
		float accel = o.accel, decel = o.decel, speed = o.speed * mul;
		if (response == 1) { const float k = mul > 1.0f ? mul : 1.0f; accel = o.accel * k; decel = o.decel * k; }
		else if (response == 2) { accel = 100000.0f; decel = 100000.0f; }

		float* pa = (float*)(m + off + RF_Accel);
		float* pd = (float*)(m + off + RF_Decel);
		float* ps = (float*)(m + off + RF_Speed);
		if (*pa != accel) *pa = accel;
		if (*pd != decel) *pd = decel;
		if (*ps != speed) *ps = speed;
	}

	// The hold-key multiplier this tick (worker writes, director hook reads),
	// and whether the director hook is driving the speed (see namespace director).
	volatile float g_holdMul = 1.0f;
	volatile bool  g_rescale = false;

	// Fight detection: if the horizontal speed we wrote last tick is not what
	// the block holds now, something else wrote it in between. Once is a mod
	// applying its settings when the camera is created (harmless, we win the
	// next tick); every tick is a mod that rewrites it per frame, and then the
	// camera sees its value more often than ours - worth one loud log line.
	float g_lastWrittenH = -1.0f;
	int   g_fights = 0;
	ULONGLONG g_fightWindow = 0;
	int   g_fightsInWindow = 0;

	// The ramp for one axis, from the effective multiplier `mul` and the ratio
	// `k` between what we want and what the block's MaxSpeed currently says.
	// In the fallback mode k is 1 (we wrote MaxSpeed ourselves); in rescale mode
	// the camera runs at the block's speed and the director hook scales its
	// movement by k, so the internal ramp has to be 1/k steeper to feel the same.
	//   Stock    the effective ramp stays the game's own
	//   Scaled   the ramp keeps its stock LENGTH at any speed
	//   Instant  no ramp at all
	void writeRamp(uint8_t* m, uint32_t off, const Resp& o, float mul, float k, int response)
	{
		if (!(k > 0.0001f)) k = 1.0f;
		float accel = o.accel, decel = o.decel;
		if (response == 2) { accel = 100000.0f; decel = 100000.0f; }
		else
		{
			const float g = (response == 1 && mul > 1.0f) ? mul : 1.0f;
			accel = o.accel * g / k;
			decel = o.decel * g / k;
			if (accel > 100000.0f) accel = 100000.0f;
			if (decel > 100000.0f) decel = 100000.0f;
		}
		float* pa = (float*)(m + off + RF_Accel);
		float* pd = (float*)(m + off + RF_Decel);
		if (*pa != accel) *pa = accel;
		if (*pd != decel) *pd = decel;
	}

	void applyAll(float hold, ULONGLONG now)
	{
		const float mulH = (float)g_cfg.speed / 100.0f * hold;
		const float mulV = (float)(g_cfg.vertical > 0 ? g_cfg.vertical : g_cfg.speed) / 100.0f * hold;
		const float mulL = (float)g_cfg.look / 100.0f;
		for (int i = 0; i < g_metaCount; ++i)
		{
			Meta& m = g_meta[i];
			__try
			{
				if (g_rescale)
				{
					// The director hook owns the speed. MaxSpeed is left to
					// whoever else writes it (the game, NoEditorRestrictions);
					// only the ramps, the look response and the mouse
					// multiplier are ours here.
					const float curH = rdf(m.p, OFF_RespH + RF_Speed);
					const float curV = rdf(m.p, OFF_RespV + RF_Speed);
					const float kH = curH > 0.0f ? (m.h.speed * mulH) / curH : 1.0f;
					const float kV = curV > 0.0f ? (m.v.speed * mulV) / curV : 1.0f;
					writeRamp(m.p, OFF_RespH, m.h, mulH, kH, g_cfg.response);
					writeRamp(m.p, OFF_RespV, m.v, mulV, kV, g_cfg.response);
					writeResp(m.p, OFF_RespLook, m.look, mulL, 1);
					float* pm = (float*)(m.p + OFF_MouseMul);
					const float mouse = m.mouse * mulL;
					if (*pm != mouse) *pm = mouse;
					continue;
				}

				if (i == 0 && g_lastWrittenH >= 0.0f)
				{
					const float cur = rdf(m.p, OFF_RespH + RF_Speed);
					if (cur != g_lastWrittenH)
					{
						++g_fights;
						if (now - g_fightWindow > 5000) { g_fightWindow = now; g_fightsInWindow = 0; }
						++g_fightsInWindow;
						if (g_fights == 1 || g_fights == 10 || g_fights == 100 || g_fights == 1000)
							logf("fight: another mod wrote the free camera speed (%.2f m/s) over ours (%.2f) - %d time(s) so far, %d in the last 5 s%s",
								cur, g_lastWrittenH, g_fights, g_fightsInWindow,
								g_fightsInWindow >= 50 ? ". That is every frame: set NoEditorRestrictions' max_horizontal/vertical_free_cam_speed to 10 or remove them." : "");
					}
				}
				writeResp(m.p, OFF_RespH,    m.h,    mulH, g_cfg.response);
				writeResp(m.p, OFF_RespV,    m.v,    mulV, g_cfg.response);
				writeResp(m.p, OFF_RespLook, m.look, mulL, 1);
				float* pm = (float*)(m.p + OFF_MouseMul);
				const float mouse = m.mouse * mulL;
				if (*pm != mouse) *pm = mouse;
				if (i == 0) g_lastWrittenH = rdf(m.p, OFF_RespH + RF_Speed);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { g_metaCount = 0; return; }
		}
	}

	void restoreAll()
	{
		for (int i = 0; i < g_metaCount; ++i)
		{
			Meta& m = g_meta[i];
			__try
			{
				writeResp(m.p, OFF_RespH,    m.h,    1.0f, 0);
				writeResp(m.p, OFF_RespV,    m.v,    1.0f, 0);
				writeResp(m.p, OFF_RespLook, m.look, 1.0f, 0);
				*(float*)(m.p + OFF_MouseMul) = m.mouse;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}
	}

	// ------------------------------------------------------------------ input
	struct Key { bool down = false; ULONGLONG t = 0, rep = 0; };
	Key g_kSlower, g_kFaster, g_kReset, g_kFree;

	inline bool isDown(int vk) { return vk > 0 && (GetAsyncKeyState(vk) & 0x8000) != 0; }

	// Edge on press, then auto-repeat after 400 ms at ~9 Hz.
	bool pressed(Key& k, int vk, ULONGLONG now)
	{
		const bool d = isDown(vk);
		if (!d) { k.down = false; return false; }
		if (!k.down) { k.down = true; k.t = now; k.rep = now; return true; }
		if (now - k.t >= 400 && now - k.rep >= 110) { k.rep = now; return true; }
		return false;
	}

	// Edge on press and nothing more. A switch (Free Look) must not take the
	// auto-repeat above, or holding the key would flip it nine times a second.
	bool tapped(Key& k, int vk)
	{
		const bool d = isDown(vk);
		const bool edge = d && !k.down;
		k.down = d;
		return edge;
	}

	void track(Key& k, int vk) { k.down = isDown(vk); }

	bool gameInForeground()
	{
		HWND fg = GetForegroundWindow();
		if (!fg) return false;
		DWORD pid = 0;
		GetWindowThreadProcessId(fg, &pid);
		return pid == GetCurrentProcessId();
	}

	bool inputLive()
	{
		if (g_cfg.hotkeys <= 0 || !gameInForeground() || !editorOpen()) return false;
		return g_cfg.anywhere ? true : editingCam();
	}

	// --- mouse wheel (optional) -----------------------------------------------
	volatile LONG g_wheel = 0;
	volatile LONG g_presented = 0;   // frames the game has presented (counted in ecs_reshade.inc)
	HHOOK  g_wheelHook = nullptr;
	HANDLE g_wheelThread = nullptr;

	LRESULT CALLBACK wheelProc(int code, WPARAM wp, LPARAM lp)
	{
		if (code == HC_ACTION && wp == WM_MOUSEWHEEL && g_cfg.hotkeys >= 3 && inputLive())
		{
			const short delta = (short)HIWORD(((MSLLHOOKSTRUCT*)lp)->mouseData);
			if (delta > 0) InterlockedIncrement(&g_wheel);
			else if (delta < 0) InterlockedDecrement(&g_wheel);
		}
		return CallNextHookEx(nullptr, code, wp, lp);
	}

	DWORD WINAPI wheelThread(LPVOID)
	{
		g_wheelHook = SetWindowsHookExW(WH_MOUSE_LL, wheelProc, g_self, 0);
		if (!g_wheelHook) { logf("wheel: hook failed (%lu) - mouse wheel disabled", GetLastError()); return 1; }
		logf("wheel: mouse wheel hook installed");
		MSG msg;
		while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
		UnhookWindowsHookEx(g_wheelHook);
		return 0;
	}

	void startWheel()
	{
		if (g_wheelThread) return;
		g_wheelThread = CreateThread(nullptr, 0, wheelThread, nullptr, 0, nullptr);
	}

	// ------------------------------------------------------------- speed steps
	int  g_lastLogged = -1;
	bool g_dirty = false;
	ULONGLONG g_dirtyAt = 0;

	float stockH() { return g_metaCount ? g_meta[0].h.speed : 10.0f; }

	void announce()
	{
		if (g_cfg.speed == g_lastLogged) return;
		g_lastLogged = g_cfg.speed;
		logf("speed: %d%% (%.2f m/s)%s", g_cfg.speed, stockH() * g_cfg.speed / 100.0f,
			g_metaCount ? "" : "  [block not found yet - nothing written]");
	}

	void setSpeed(int pct, ULONGLONG now)
	{
		if (pct < 1) pct = 1;
		if (pct > 2000) pct = 2000;
		if (pct == g_cfg.speed) return;
		g_cfg.speed = pct;
		g_dirty = true; g_dirtyAt = now;
		announce();
	}

	void step(int delta, ULONGLONG now)
	{
		int i = nearestPreset(g_cfg.speed) + delta;
		if (i < 0) i = 0;
		if (i >= kPresetCount) i = kPresetCount - 1;
		setSpeed(kPresets[i], now);
	}
