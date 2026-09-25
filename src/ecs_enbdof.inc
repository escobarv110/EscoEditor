// =============================================================================
//  ecs_enbdof.inc - "ENB": the Rockstar Editor's depth of field, drawn by ENB
// =============================================================================
//  ENBSeries exports a small SDK from its d3d11.dll - ENBGetParameter,
//  ENBSetParameter, ENBSetCallbackFunction - that reads and writes the same
//  shader parameters its own GUI does, by their UI names. This drives the
//  parameters of enbdepthoffield.fx from the clip:
//
//    1. what the user set here for the session (starts as ENB's own values)
//    2. keys over the clip, every ENB DOF option, interpolated like a light
//    3. the Rockstar Editor's own Depth of Field on the keyframes either side
//       of the playhead: a Custom keyframe's Focal Distance is where ENB
//       focuses (in metres, converted to NVE's own unit), its Intensity
//       (a percentage) sets the aperture, and None means no blur at all.
//
//  Values are pushed from ENB's own BeginFrame callback (the place the SDK is
//  meant to be used from), only when they change. Before ENB saves its
//  configuration they are put back to what ENB had, so the user's
//  enbdepthoffield.fx.ini never ends up holding a keyframe's numbers.
//
//  The UI names are NVE's enbdepthoffield.fx, taken from its .fx.ini. Each is
//  checked against ENB when it is found; a name ENB does not know is left out.
// =============================================================================
namespace enbdof
{
	// ---- the SDK --------------------------------------------------------------
	enum : int { P_NONE = 0, P_FLOAT = 1, P_INT = 2, P_HEX = 3, P_BOOL = 4 };
	struct Param { unsigned char data[16]; unsigned long size; int type; };
	using FnGet   = BOOL (*)(char*, char*, char*, Param*);
	using FnSet   = BOOL (*)(char*, char*, char*, Param*);
	using FnVer   = long (*)();
	using FnCb    = void (WINAPI*)(int);
	using FnSetCb = void (*)(FnCb);

	FnGet   s_get = nullptr;
	FnSet   s_set = nullptr;
	FnVer   s_ver = nullptr;
	FnSetCb s_setCb = nullptr;
	long    s_sdk = 0;

	// ---- the parameters -------------------------------------------------------
	enum Kind : uint8_t { K_FLOAT = 0, K_INT = 1, K_BOOL = 2, K_ENUM = 3 };   // K_ENUM: a choice, sent as an int, held key to key
	struct Def { const char* ui; Kind kind; float lo, hi; const char* label; };
	constexpr int N = lights::kEnbParams;
	// The ranges are the UIMin / UIMax NVE's compiled enbdepthoffield.fx.bin
	// gives each one. The focus distance is kept in metres here and converted
	// on its way to ENB (enbDistance): NVE's "Distance" is not metres.
	const Def kDefs[N] = {
		{ "--> Click here to enable Mouse Focus",       K_BOOL,   0.0f,    1.0f, "Mouse focus" },
		{ "--> Blur the near field toggle",             K_BOOL,   0.0f,    1.0f, "Near field blur" },
		{ "--> Near field intensity",                   K_FLOAT,  0.0f, 1000.0f, "Near field power" },
		{ "--> Manual Focus : Distance",                K_FLOAT,  0.1f, 2000.0f, "Focus distance (m)" },
		{ "--> Manual Focus : Aperture",                K_FLOAT,  0.0f,   10.0f, "Aperture" },
		{ "--> Manual Focus : Near field power",        K_FLOAT,  0.0f, 1000.0f, "Near field power" },
		{ "--> Auto-focus : Aperture",                  K_FLOAT,  0.0f,   10.0f, "Auto-focus aperture" },
		{ "--> Auto-focus : Speed",                     K_FLOAT,  0.01f, 100.0f, "Auto-focus speed" },
		{ "--> Auto-focus : Range boost",               K_FLOAT,  0.0f,  100.0f, "Auto-focus range boost" },
		{ "--> Auto-focus : Point of interestX",        K_FLOAT,  0.0f,    1.0f, "Auto-focus point X" },
		{ "--> Auto-focus : Point of interestY",        K_FLOAT,  0.0f,    1.0f, "Auto-focus point Y" },
		{ "--> Blur : Chromatic Spread",                K_FLOAT,  1.0f,   32.0f, "Chromatic spread" },
		{ "--> Blur : Size",                            K_FLOAT,  0.0f,    2.0f, "Blur size" },
		{ "--> Blur : Quality Steps",                   K_INT,    1.0f,   16.0f, "Blur quality steps" },
		{ "--> DOF : Maximum size",                     K_FLOAT,  2.0f,   20.0f, "Maximum blur" },
		{ "--> DOF : Quality",                          K_FLOAT,  0.1f,    2.0f, "Quality" },
		{ "--> DOF : Performance scaler",               K_FLOAT,  0.1f,    1.0f, "Performance scaler" },
		{ "--> DOF : Anamorphic enable",                K_BOOL,   0.0f,    1.0f, "Anamorphic" },
		{ "--> DOF : Anamorphic stretch",               K_FLOAT,  0.0f,    4.0f, "Anamorphic stretch" },
		{ "--> DOF : Distribution",                     K_FLOAT,  1.0f,    4.0f, "Distribution" },
		{ "--> Lens Distortion : Distortion Amount",    K_FLOAT,  0.0f,    1.0f, "Lens distortion" },
		{ "--> Lens Distortion : Always zoomed",        K_BOOL,   0.0f,    1.0f, "Distortion: always zoomed" },
		{ "--> Lens Distortion : Zoom Amount",          K_FLOAT,  0.0f,    1.0f, "Distortion: zoom" },
		// Six lines that look like separators in ENB's window. They ARE only
		// labels: the shader declares them (text0, textmanu, ...) and never
		// reads them. 4.9 took them for switches; they stay in the table so
		// keys written then still read, and are never sent to ENB.
		{ "-------------------------- DOF Near Field settings ------------", K_BOOL, 0.0f, 1.0f, "Near field section" },
		{ "-------------------------- Manual DOF -------------------------", K_BOOL, 0.0f, 1.0f, "Manual DOF" },
		{ "-------------------------- Auto-focus DOF ---------------------", K_BOOL, 0.0f, 1.0f, "Auto-focus DOF" },
		{ "-------------------------- Blur Settings ----------------------", K_BOOL, 0.0f, 1.0f, "Blur section" },
		{ "-------------------------- Quality Settings -------------------", K_BOOL, 0.0f, 1.0f, "Quality section" },
		{ "-------------------------- Misc. settings ---------------------", K_BOOL, 0.0f, 1.0f, "Misc section" },
		// Not a shader variable: which of NVE's techniques ENB runs, 0 its
		// "Gameplay DOF Auto-focus", 1 its "Gameplay DOF Manual". Per keyframe
		// this is Focus Mode Auto / Manual.
		{ "TECHNIQUE",                                  K_ENUM,   0.0f,    1.0f, "Focus mode" },
	};
	enum { I_MOUSE = 0, I_NEAR = 1, I_AUTO_NEARPOWER = 2, I_DIST = 3, I_APERTURE = 4, I_NEARPOWER = 5, I_AUTO_APERTURE = 6,
	       I_SEC_NEAR = 23, I_SEC_MANUAL = 24, I_SEC_AUTO = 25, I_SEC_BLUR = 26, I_SEC_QUALITY = 27, I_SEC_MISC = 28,
	       I_FOCUS = 29 };
	inline bool isLabel(int i) { return i >= I_SEC_NEAR && i <= I_SEC_MISC; }

	// Three rows stand for more than one ENB option each (4.13):
	//   Aperture         a percentage. NVE's two techniques read two different
	//                    apertures on two different scales - Manual's is spent
	//                    by about 0.1, Auto-focus's wants 2 to 10 - so the same
	//                    percentage is converted for whichever runs.
	//   Near Field Blur  Off, or how much: the near field switch and its power.
	//   Anamorphic       Off, or how stretched: the switch and the stretch.
	enum { V_APERTURE = 100, V_NEAR = 101, V_ANAMORPHIC = 102 };
	inline bool isVirtual(int p) { return p >= V_APERTURE && p <= V_ANAMORPHIC; }
	inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
	// Aperture is THE blur amount (4.15): 0% no blur at all, 100% the most NVE
	// draws. NVE's blur is three numbers together - its aperture (how soon
	// things go soft past the focus), DOF : Maximum size (the radius the blur
	// can reach, 2..20) and Blur : Size (a softening pass over it, 0..2) - so
	// the one percentage sets all three (blurSizesFor), and the menu has no
	// separate size rows. Stored as NVE's aperture: 0.12 at 100%.
	constexpr float kApertureFull = 0.12f;
	inline float manualFromPct(float pc) { return kApertureFull * clampf(pc, 0.0f, 100.0f) / 100.0f; }
	inline float pctManual(float a)      { return clampf(a / kApertureFull, 0.0f, 1.0f) * 100.0f; }
	// Maximum size and Blur size for an aperture, as the percentage says.
	inline void blurSizesFor(float a, float* maxSize, float* blurSize)
	{
		const float f = clampf(a / kApertureFull, 0.0f, 1.0f);
		*maxSize = 2.0f + 18.0f * f;
		*blurSize = 2.0f * f;
	}
	// Auto-focus compares raw depths: its blur also shrinks as the focus moves off.
	inline float autoFromPct(float pc)   { return clampf(pc, 0.0f, 100.0f) / 10.0f; }
	inline float pctAuto(float a)        { return clampf(a * 10.0f, 0.0f, 100.0f); }
	// The near field power divides the near blur: 100% = as much as behind.
	inline float powerFromPct(float pc)  { return 100.0f / clampf(pc, 1.0f, 100.0f); }
	inline float nearPct(float power)    { return power <= 1.0f ? 100.0f : clampf(100.0f / power, 1.0f, 100.0f); }

	// NVE's two techniques. "Gameplay DOF Manual" focuses at Manual Focus :
	// Distance; "Gameplay DOF Auto-focus" measures the depth at its point of
	// interest itself. Each reads its own aperture and near field power.
	enum { TECH_AUTO = 0, TECH_MANUAL = 1 };
	int s_tech = -1;                      // -1 not known yet: taken as Manual
	inline bool wantsCentre();

	uint8_t kinds[N] = {};                // for lights::evalEnb, filled on start

	// ---- state ----------------------------------------------------------------
	const char* s_file = nullptr;         // the filename / category pair ENB answers to
	const char* s_cat  = nullptr;
	bool  s_found = false;                // the SDK is exported
	bool  s_ready = false;                // ENB answered for the DOF shader
	bool  s_known[N]  = {};
	bool  s_focusFromIni = false;        // ENB does not answer for TECHNIQUE: its ini says what it runs
	float s_base[N]   = {};               // ENB's own values, which are put back
	float s_manual[N] = {};               // this session's values, edited in the window
	float s_pushed[N] = {};
	bool  s_havePushed[N] = {};
	bool  s_dirty = false;                // something of ours is in ENB now
	volatile LONGLONG s_lastCb = 0;       // when ENB last called us
	int   s_cbLogs = 0, s_setFails = 0, s_checkLogs = 0;
	int   s_saveCalls = 0;
	ULONGLONG s_saveWindow = 0;
	bool  s_saveIsNoise = false;
	char  s_editorLine[160] = "";          // what the Rockstar Editor asks for at the playhead
	ULONGLONG s_nextPoll = 0;

	// ---- one parameter --------------------------------------------------------
	bool getRaw(const char* file, const char* cat, const char* ui, Param* p)
	{
		memset(p, 0, sizeof(*p));
		__try { return s_get && s_get((char*)file, (char*)cat, (char*)ui, p) != FALSE; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	bool setRaw(const char* ui, Param* p)
	{
		__try { return s_set && s_set((char*)s_file, (char*)s_cat, (char*)ui, p) != FALSE; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	bool readValue(const char* file, const char* cat, int i, float* out)
	{
		Param p;
		if (!getRaw(file, cat, kDefs[i].ui, &p)) return false;
		switch (p.type)
		{
		case P_FLOAT: { float f; memcpy(&f, p.data, 4); if (!isfinite(f)) return false; *out = f; return true; }
		case P_INT: case P_HEX: { int v; memcpy(&v, p.data, 4); *out = (float)v; return true; }
		case P_BOOL: { BOOL b; memcpy(&b, p.data, 4); *out = b ? 1.0f : 0.0f; return true; }
		default: return false;
		}
	}

	bool writeValue(int i, float v)
	{
		Param p = {};
		p.size = 4;
		switch (kDefs[i].kind)
		{
		case K_FLOAT: p.type = P_FLOAT; memcpy(p.data, &v, 4); break;
		case K_INT: case K_ENUM: { p.type = P_INT; const int n = (int)floorf(v + 0.5f); memcpy(p.data, &n, 4); break; }
		case K_BOOL:  { p.type = P_BOOL; const BOOL b = v > 0.5f ? TRUE : FALSE; memcpy(p.data, &b, 4); break; }
		}
		return setRaw(kDefs[i].ui, &p);
	}

	bool same(int i, float a, float b)
	{
		if (kDefs[i].kind == K_FLOAT) return fabsf(a - b) <= 1e-4f * (1.0f + fabsf(a));
		return (int)floorf(a + 0.5f) == (int)floorf(b + 0.5f);
	}

	// ---- NVE's focus distance ----------------------------------------------------
	// NVE's manual pass focuses where 1 - 1/(Distance^2 * depth) meets the
	// depth, and GTA's depth is reversed (near / z). Solved for z, the plane in
	// focus sits at z = Distance^2 * near * (1 - near / z): NVE's "7.0" is about
	// 4.9 m with the Rockstar Editor camera's 0.1 m near clip, and "51.6" - a
	// keyframe's 51.6 m sent as it was until 4.9 - was nearly 270 m. So the
	// distance stays in metres everywhere here and is converted on its way out.
	float s_near = 0.1f;
	float nearClip()
	{
		const lights::Ctx c = lights::ctx();
		if (c.valid && c.cam.nearClip > 0.01f && c.cam.nearClip < 2.0f) s_near = c.cam.nearClip;
		return s_near;
	}
	inline float enbDistance(float metres, float n)   // metres -> NVE's Distance
	{
		if (!(metres > 2.0f * n)) metres = 2.0f * n;
		return metres / sqrtf(n * (metres - n));
	}
	inline float metresOf(float d, float n)           // NVE's Distance -> metres
	{
		if (!(d > 2.0f)) return 2.0f * n;
		return n * (d * d + d * sqrtf(d * d - 4.0f)) * 0.5f;
	}

	// ---- what is in the middle of the picture ------------------------------------
	// Auto focus (4.14). Asking ENB to switch NVE to its Auto-focus technique
	// was taken - ENB said so - and changed nothing on screen. So EscoEditor
	// measures instead: EscoFlare.fx's EscoFocus pass writes the raw depth at
	// the centre into one pixel, ecs_reshade.inc reads it back (two frames
	// late, never waiting on the GPU), and NVE stays on its Manual technique
	// with its Distance set where that depth is sharp. NVE's manual pass is
	// sharp where 1 - 1/(Distance^2 x depth) equals the depth, so for a raw
	// depth d that is Distance = 1 / sqrt(d (1 - d)) - exactly, whatever the
	// camera's near clip.
	volatile float     s_centreRaw = 0.0f;
	volatile ULONGLONG s_centreAt = 0;
	float s_autoD = 0.0f;                 // the smoothed Distance Auto sends
	ULONGLONG s_autoAt = 0;
	int   s_autoLogs = 0;

	void setCentre(float d)
	{
		if (!(d > 0.0f && d < 1.0f)) return;
		s_centreRaw = d;
		s_centreAt = GetTickCount64();
	}
	bool centreFresh() { return s_centreAt && GetTickCount64() - s_centreAt < 750; }
	inline float distanceForRaw(float d)
	{
		if (d < 1e-6f) d = 1e-6f;
		if (d > 0.75f) d = 0.75f;
		return 1.0f / sqrtf(d * (1.0f - d));
	}
	float nearClip();
	// For showing only: GTA's depth is reversed (near / z).
	float centreMetres() { const float d = s_centreRaw; return d > 1e-6f ? nearClip() / d : 0.0f; }

	// ---- finding ENB and its DOF shader ----------------------------------------
	// The SDK is part of ENB's own d3d11.dll - NVE's has it - and nothing has
	// to be installed. Finding that DLL is the whole problem: ENB loads the
	// SYSTEM d3d11.dll too, under the same name; FiveM's game process loads
	// well over a hundred modules before ENB's (4.7 gave up after 96); and a
	// packed ENB can keep its export table out of GetProcAddress's reach. So
	// there are three ways in, each on its own, and the log says what it saw.
	int  s_modsSeen = 0;
	char s_d3dSeen[4][300] = {};    // the d3d11 / dxgi modules found, for the log
	int  s_d3dSeenCount = 0;

	wchar_t s_enbIni[MAX_PATH] = {};   // ENB's enbseries\enbdepthoffield.fx.ini, next to its d3d11.dll

	bool take(HMODULE h, FnSet set, FnGet get, const char* how)
	{
		if (GetModuleFileNameW(h, s_enbIni, MAX_PATH))
		{
			if (wchar_t* slash = wcsrchr(s_enbIni, L'\\')) { slash[1] = 0; wcscat_s(s_enbIni, L"enbseries\\enbdepthoffield.fx.ini"); }
			else s_enbIni[0] = 0;
		}
		s_set = set;
		s_get = get;
		s_ver   = (FnVer)GetProcAddress(h, "ENBGetSDKVersion");
		s_setCb = (FnSetCb)GetProcAddress(h, "ENBSetCallbackFunction");
		__try { s_sdk = s_ver ? s_ver() : 0; }
		__except (EXCEPTION_EXECUTE_HANDLER) { s_sdk = 0; }
		s_found = true;
		logf("enb: ENB found %s at %p - its SDK (version %ld) is there; the ENB option can drive its depth of field", how, (void*)h, s_sdk);
		return true;
	}

	bool tryModule(HMODULE h, const char* how)
	{
		if (!h) return false;
		FnSet set = (FnSet)GetProcAddress(h, "ENBSetParameter");
		FnGet get = (FnGet)GetProcAddress(h, "ENBGetParameter");
		return set && get && take(h, set, get, how);
	}

	// The RVA of an export, read from the DLL's file on disk, or 0.
	uint32_t fileExportRva(const wchar_t* path, const char* want, uint32_t* sizeOfImage)
	{
		HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
		if (f == INVALID_HANDLE_VALUE) return 0;
		const DWORD size = GetFileSize(f, nullptr);
		uint8_t* d = (size > 0x1000 && size < (64u << 20)) ? (uint8_t*)VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) : nullptr;
		DWORD got = 0;
		const bool ok = d && ReadFile(f, d, size, &got, nullptr) && got == size;
		CloseHandle(f);
		uint32_t rva = 0;
		if (ok)
		{
			__try
			{
				const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)d;
				const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(d + dos->e_lfanew);
				*sizeOfImage = nt->OptionalHeader.SizeOfImage;
				const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
				const int nsec = nt->FileHeader.NumberOfSections;
				auto toOff = [&](uint32_t r) -> int64_t {
					for (int i = 0; i < nsec; ++i)
					{
						const uint32_t va = sec[i].VirtualAddress, len = sec[i].Misc.VirtualSize > sec[i].SizeOfRawData ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
						if (r >= va && r < va + len) return (int64_t)sec[i].PointerToRawData + (r - va);
					}
					return -1;
				};
				const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
				const int64_t eo = toOff(dir.VirtualAddress);
				if (dir.VirtualAddress && eo > 0)
				{
					const IMAGE_EXPORT_DIRECTORY* ex = (const IMAGE_EXPORT_DIRECTORY*)(d + eo);
					const int64_t no = toOff(ex->AddressOfNames), oo = toOff(ex->AddressOfNameOrdinals), fo = toOff(ex->AddressOfFunctions);
					for (DWORD i = 0; no > 0 && oo > 0 && fo > 0 && i < ex->NumberOfNames; ++i)
					{
						const int64_t so = toOff(((const uint32_t*)(d + no))[i]);
						if (so <= 0 || strcmp((const char*)(d + so), want) != 0) continue;
						const uint16_t ord = ((const uint16_t*)(d + oo))[i];
						rva = ((const uint32_t*)(d + fo))[ord];
						break;
					}
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { rva = 0; }
		}
		if (d) VirtualFree(d, 0, MEM_RELEASE);
		return rva;
	}

	// Way 3: Windows will not hand the export out, so read it from the file and
	// find it at the same offset in memory - once the loaded image is the same
	// size as the file's, so it is the same DLL.
	bool tryFromFile(HMODULE h, const wchar_t* path, uint32_t loadedSize)
	{
		uint32_t fileSize = 0;
		const uint32_t rs = fileExportRva(path, "ENBSetParameter", &fileSize);
		uint32_t fileSize2 = 0;
		const uint32_t rg = fileExportRva(path, "ENBGetParameter", &fileSize2);
		if (!rs || !rg || fileSize != loadedSize || rs >= loadedSize || rg >= loadedSize) return false;
		FnSet set = (FnSet)((uint8_t*)h + rs);
		FnGet get = (FnGet)((uint8_t*)h + rg);
		return take(h, set, get, "(its exports read from the file, the loaded image hides them)");
	}

	bool resolve()
	{
		if (s_found) return true;
		int n = 0;
		s_d3dSeenCount = 0;
		HMODULE d3d[4] = {};
		wchar_t d3dPath[4][300] = {};
		uint32_t d3dSize[4] = {};

		// Way 1: every module in the loader's list. No cap worth the name.
		__try
		{
			const uint8_t* peb = (const uint8_t*)__readgsqword(0x60);
			const uint8_t* ldr = *(const uint8_t* const*)(peb + 0x18);
			const LIST_ENTRY* head = (const LIST_ENTRY*)(ldr + 0x20);        // InMemoryOrderModuleList
			for (const LIST_ENTRY* e = head->Flink; e && e != head && n < 4096; e = e->Flink, ++n)
			{
				const uint8_t* entry = (const uint8_t*)e - 0x10;              // LDR_DATA_TABLE_ENTRY
				struct NtStr { uint16_t Length, MaximumLength; const wchar_t* Buffer; };
				const HMODULE base = *(HMODULE const*)(entry + 0x30);         // DllBase
				const uint32_t imageSize = *(const uint32_t*)(entry + 0x40);  // SizeOfImage
				const NtStr* full = (const NtStr*)(entry + 0x48);             // FullDllName
				const NtStr* nm   = (const NtStr*)(entry + 0x58);             // BaseDllName
				char low[48] = {};
				for (int i = 0, k = 0; nm->Buffer && i < nm->Length / 2 && k < 47; ++i)
				{
					wchar_t w = nm->Buffer[i];
					if (w >= L'A' && w <= L'Z') w = (wchar_t)(w + 32);
					low[k++] = w < 128 ? (char)w : '_';
				}
				if ((!strcmp(low, "d3d11.dll") || !strcmp(low, "dxgi.dll")) && s_d3dSeenCount < 4)
				{
					const int j = s_d3dSeenCount++;
					d3d[j] = base;
					d3dSize[j] = imageSize;
					const int len = full->Buffer ? (full->Length / 2 < 299 ? full->Length / 2 : 299) : 0;
					for (int i = 0; i < len; ++i) { d3dPath[j][i] = full->Buffer[i]; s_d3dSeen[j][i] = full->Buffer[i] < 128 ? (char)full->Buffer[i] : '_'; }
					d3dPath[j][len] = 0;
					s_d3dSeen[j][len] = 0;
				}
				if (tryModule(base, "in the game's module list")) return true;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
		s_modsSeen = n;

		// Way 2: by path - the game's folder, where ENB's own files are, and
		// next to this plugin, FiveM's plugins folder. Both places ENB is put.
		wchar_t path[MAX_PATH];
		if (GetCurrentDirectoryW(MAX_PATH - 16, path))
		{
			wcscat_s(path, L"\\d3d11.dll");
			if (tryModule(GetModuleHandleW(path), "in the game's folder")) return true;
		}
		if (GetModuleFileNameW(g_self, path, MAX_PATH))
		{
			if (wchar_t* slash = wcsrchr(path, L'\\')) { slash[1] = 0; wcscat_s(path, L"d3d11.dll"); }
			if (tryModule(GetModuleHandleW(path), "next to this plugin")) return true;
		}

		// Way 3: a d3d11.dll that is not the system's, whose file has the SDK.
		for (int j = 0; j < s_d3dSeenCount; ++j)
		{
			if (!d3d[j] || wcsstr(d3dPath[j], L"\\System32\\") || wcsstr(d3dPath[j], L"\\system32\\") || wcsstr(d3dPath[j], L"\\SysWOW64\\")) continue;
			if (tryFromFile(d3d[j], d3dPath[j], d3dSize[j])) return true;
		}
		return false;
	}

	void WINAPI onEnb(int type);

	// Which of NVE's techniques ENB runs: asked of ENB, else read from its
	// enbdepthoffield.fx.ini (the one next to ENB's own d3d11.dll).
	void readTechnique()
	{
		int t = -1;
		Param p;
		if (getRaw(s_file, s_cat, "TECHNIQUE", &p) && (p.type == P_INT || p.type == P_HEX)) memcpy(&t, p.data, 4);
		if (t < 0 && s_enbIni[0]) t = (int)GetPrivateProfileIntW(L"ENBDEPTHOFFIELD.FX", L"TECHNIQUE", -1, s_enbIni);
		if (t != TECH_AUTO && t != TECH_MANUAL) t = -1;
		if (t == s_tech) return;
		s_tech = t;
		logf("enb: ENB runs NVE's %s", t == TECH_AUTO ? "\"Gameplay DOF Auto-focus\" - it measures the focus itself; the keyframe's focus distance is not used"
			: t == TECH_MANUAL ? "\"Gameplay DOF Manual\" - the keyframe's focus distance is where it focuses"
			: "technique (not readable) - taken as \"Gameplay DOF Manual\"");
	}
	inline bool techAuto() { return s_tech == TECH_AUTO; }
	inline bool wantsCentre() { return s_ready && g_cfg.enbDof; }

	// Which filename / category pair ENB wants for a shader parameter is not
	// written down anywhere reliable, so each plausible one is asked for a value
	// that must exist, and the one that answers is kept.
	bool calibrate()
	{
		struct Form { const char* file; const char* cat; };
		static const Form kForms[] = {
			{ "enbdepthoffield.fx", "ENBDEPTHOFFIELD.FX" },
			{ "enbdepthoffield.fx", nullptr },
			{ nullptr,              "ENBDEPTHOFFIELD.FX" },
			{ "enbdepthoffield.fx", "" },
			{ "enbseries\\enbdepthoffield.fx", "ENBDEPTHOFFIELD.FX" } };
		for (const Form& f : kForms)
		{
			float v = 0.0f;
			if (!readValue(f.file, f.cat, I_DIST, &v)) continue;
			s_file = f.file;
			s_cat = f.cat;
			int known = 0;
			for (int i = 0; i < N; ++i)
			{
				s_known[i] = !isLabel(i) && readValue(s_file, s_cat, i, &s_base[i]);
				if (s_known[i]) ++known;
				s_manual[i] = s_base[i];
				s_havePushed[i] = false;
			}
			s_manual[I_DIST] = metresOf(s_base[I_DIST], nearClip());
			// NVE's ini keeps the two apertures on their own scales (its 0.5 and
			// 0.5 are "everything blurred" and "almost nothing"); the session
			// starts with Auto at the same strength as Manual.
			// 4.15: Aperture is the blur amount; the session starts at 50%.
			// NVE's own 0.5 would read as 100% - more blur than anyone asked for.
			if (s_known[I_APERTURE]) s_manual[I_APERTURE] = manualFromPct(50.0f);
			if (s_known[I_APERTURE] && s_known[I_AUTO_APERTURE]) s_manual[I_AUTO_APERTURE] = autoFromPct(pctManual(s_manual[I_APERTURE]));
			if (s_known[I_NEARPOWER] && s_known[I_AUTO_NEARPOWER]) s_manual[I_AUTO_NEARPOWER] = s_manual[I_NEARPOWER];
			readTechnique();
			if (!s_known[I_FOCUS])
			{
				// ENB does not say which technique it runs through the SDK; its
				// ini does. Switching is still tried - the first push tells.
				s_focusFromIni = true;
				s_known[I_FOCUS] = true;
				s_base[I_FOCUS] = s_manual[I_FOCUS] = techAuto() ? 0.0f : 1.0f;
			}
			s_ready = true;
			logf("enb: enbdepthoffield.fx answers as (%s, %s): %d options known, focus distance %.2f (about %.1f m), aperture %.2f",
				s_file ? s_file : "null", s_cat ? s_cat : "null", known, s_base[I_DIST], s_manual[I_DIST], s_base[I_APERTURE]);
			logf("enb: ENB %s which technique it runs through its SDK", s_focusFromIni ? "does not say" : "says");
			if (s_setCb)
			{
				__try { s_setCb(onEnb); logf("enb: ENB calls EscoEditor back every frame from now on"); }
				__except (EXCEPTION_EXECUTE_HANDLER) { logf("enb: ENBSetCallbackFunction failed - values go in from the game's frame instead"); }
			}
			return true;
		}
		return false;
	}

	// ENB's own values back, as they were before anything of ours went in.
	void restoreBase()
	{
		if (!s_ready) return;
		for (int i = 0; i < N; ++i)
			if (s_known[i] && s_havePushed[i]) writeValue(i, s_base[i]);
		for (int i = 0; i < N; ++i) s_havePushed[i] = false;
		s_dirty = false;
	}

	// While the option is off, ENB's values are the user's and may change in
	// ENB's own window: follow them, so switching on starts from what they see.
	void rereadBase()
	{
		if (!s_ready || s_dirty) return;
		for (int i = 0; i < N; ++i)
			if (s_known[i] && !(i == I_FOCUS && s_focusFromIni)) readValue(s_file, s_cat, i, &s_base[i]);
		if (s_focusFromIni) s_base[I_FOCUS] = techAuto() ? 0.0f : 1.0f;
	}

	// The session values back to ENB's own (the focus distance in metres).
	void resetManual()
	{
		for (int i = 0; i < N; ++i) s_manual[i] = s_base[i];
		s_manual[I_DIST] = metresOf(s_base[I_DIST], nearClip());
	}

	// ---- the Rockstar Editor's Depth of Field at the playhead -----------------
	struct EdDof { bool have; uint8_t mode, focus; float dist, intensity, t; };

	void readDof(const void* m, EdDof& d)
	{
		d.have = false;
		__try
		{
			d.mode = mk::u8(m, dof::OFF_Mode);
			d.focus = mk::u8(m, dof::OFF_FocusMode);
			memcpy(&d.dist, (const uint8_t*)m + dof::OFF_FocalDist, 4);
			memcpy(&d.intensity, (const uint8_t*)m + dof::OFF_Intensity, 4);
			memcpy(&d.t, (const uint8_t*)m + mk::OFF_TimeMs, 4);
			d.have = isfinite(d.dist) && isfinite(d.intensity) && isfinite(d.t) && mk::u8(m, mk::OFF_Type) != mk::TYPE_ANCHOR;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { d.have = false; }
	}

	// The keyframes either side of `t`: the one at or before it, the next after.
	bool dofAround(float t, EdDof& a, EdDof& b)
	{
		a.have = b.have = false;
		void* s = game::replayStorage();
		if (!s) return false;
		const int n = st::count(s);
		if (n <= 0 || n > 4096) return false;
		for (int i = 0; i < n; ++i)
		{
			const void* m = st::tryGet(s, i);
			if (!m) continue;
			EdDof d;
			readDof(m, d);
			if (!d.have) continue;
			if (d.t <= t) { if (!a.have || d.t >= a.t) a = d; }
			else if (!b.have || d.t < b.t) b = d;
		}
		if (!a.have && b.have) { a = b; b.have = false; }   // before the first keyframe: it holds
		return a.have;
	}

	// What the Rockstar Editor's keyframes ask of ENB at `t`. Pure enough to be
	// tested: works on the two keyframes it is given.
	//
	// The game's intensity is a percentage (a keyframe reads 50.0 at 50%); one
	// stored as 0..1 is taken as a fraction. NVE's Manual aperture is NOT a
	// blur amount: its circle of confusion is |focus - depth| x aperture x 36,
	// capped at 1.5, so above about 0.1 everything past the focus is already
	// as blurred as it gets. Its Auto-focus aperture compares raw depths and
	// wants far bigger numbers. So 100% is 0.5 for Manual and 2.0 for Auto.
	inline float intensityPct(float x)
	{
		if (!(x > 0.0f)) return 0.0f;
		const float p = x <= 1.0f ? x * 100.0f : x;
		return p > 100.0f ? 100.0f : p;
	}
	inline float apertureOf(float pct, float strength) { return manualFromPct(pct * strength); }
	inline float autoApertureOf(float pct, float strength) { return 2.0f * (pct / 100.0f) * strength; }
	int s_rawLogs = 0;

	// Only with "Follow the Rockstar Editor DOF" (off by default since 4.11):
	// a Custom keyframe's Focal Distance and Intensity drive ENB's focus and
	// aperture - unless the keyframe set them itself in the ENB rows
	// (`filled`). Default and None change nothing: ENB is its own DOF, and
	// None is how the game's own blur is kept from adding to it.
	void applyEditor(const EdDof& a, const EdDof& b, float t, float strength, float* v, char* line, int cap, uint32_t filled = 0)
	{
		if (s_rawLogs < 1 && a.have) { ++s_rawLogs; logf("enb: the keyframe's stored DOF intensity reads %.3f (mode %s)", a.intensity, dof::modeName(a.mode)); }
		if (a.mode != dof::MODE_CUSTOM)
		{
			if (line) snprintf(line, cap, "keyframe DOF %s - the ENB rows decide", dof::modeName(a.mode));
			return;
		}
		float u = 0.0f;
		const bool blend = b.have && b.mode == dof::MODE_CUSTOM && b.t > a.t;
		if (blend) u = (t - a.t) / (b.t - a.t);
		if (!(u >= 0.0f)) u = 0.0f;
		if (u > 1.0f) u = 1.0f;
		const float pa = intensityPct(a.intensity), pb = blend ? intensityPct(b.intensity) : pa;
		const float pct = pa + (pb - pa) * u;
		if (!(filled & (1u << I_APERTURE)))      v[I_APERTURE] = apertureOf(pct, strength);
		if (!(filled & (1u << I_AUTO_APERTURE))) v[I_AUTO_APERTURE] = autoApertureOf(pct, strength);
		const bool blendDist = blend && b.dist > 0.0f;
		const float d = a.dist + ((blendDist ? b.dist : a.dist) - a.dist) * u;
		if (d > 0.0f && !(filled & (1u << I_DIST))) v[I_DIST] = d;
		if (line) snprintf(line, cap, "keyframe DOF Custom: focal distance %.2f m, intensity %.0f%%", d, pct);
	}

	// ---- one frame -------------------------------------------------------------
	// What ENB should hold, computed on the game thread and published; pushed
	// from ENB's thread. A snapshot older than a second is not trusted - an
	// export does not tick the replay director - and the push computes afresh.
	struct Want { float v[N]; ULONGLONG at; bool on; };
	Want s_want = {};
	volatile LONG s_wantSeq = 0;

	// What ENB should hold at `t` ms into the clip: the session values, then the
	// options this clip's keyframes set themselves, then the game's own DOF of
	// the keyframes either side for whatever those did not set.
	// Is there a Rockstar Editor keyframe at `t` (within 5 ms)?
	bool markerAt(float t)
	{
		void* st = game::replayStorage();
		if (!st) return false;
		const int n = st::count(st);
		if (n <= 0 || n > 4096) return false;
		for (int i = 0; i < n; ++i)
		{
			const void* m = st::tryGet(st, i);
			if (!m) continue;
			EdDof d;
			readDof(m, d);
			if (d.have && fabsf(d.t - t) <= 5.0f) return true;
		}
		return false;
	}

	// The ENB options keyframe by keyframe (4.15), the way the game's own DOF
	// goes: each Rockstar Editor keyframe has its own values - the ones set in
	// its ENB rows, and the defaults for the rest - and the picture blends from
	// one keyframe to the next. Until 4.15 a value held until another keyframe
	// changed it, so a blurred keyframe kept blurring the keyframes after it.
	// Keys the light editor animated at other moments still shape the defaults.
	uint32_t evalByMarker(const lights::EnbTrack& tr, float t, float* out)
	{
		EdDof a, b;
		if (!dofAround(t, a, b)) return lights::evalEnb(tr, t, out, kinds);   // no keyframes: the keys as they are
		lights::EnbTrack loose;
		loose.n = 0;
		for (int i = 0; i < tr.n; ++i)
			if (!markerAt(tr.k[i].t)) loose.k[loose.n++] = tr.k[i];
		uint32_t filled = 0;
		float va[N], vb[N];
		memcpy(va, out, sizeof(va));
		if (loose.n) filled |= lights::evalEnb(loose, a.t, va, kinds);
		for (int p = 0; p < N; ++p) { float x; if (lights::enbKeyValue(tr, a.t, p, &x)) { va[p] = x; filled |= 1u << p; } }
		float u = 0.0f;
		if (b.have && b.t > a.t)
		{
			memcpy(vb, out, sizeof(vb));
			if (loose.n) filled |= lights::evalEnb(loose, b.t, vb, kinds);
			for (int p = 0; p < N; ++p) { float x; if (lights::enbKeyValue(tr, b.t, p, &x)) { vb[p] = x; filled |= 1u << p; } }
			u = (t - a.t) / (b.t - a.t);
			if (!(u >= 0.0f)) u = 0.0f;
			if (u > 1.0f) u = 1.0f;
		}
		for (int p = 0; p < N; ++p)
		{
			if (u <= 0.0f || kinds[p] >= 2) { out[p] = va[p]; continue; }   // a switch changes at the keyframe
			const float x = va[p] + (vb[p] - va[p]) * u;
			out[p] = kinds[p] == 1 ? floorf(x + 0.5f) : x;
		}
		return filled;
	}

	void computeAt(float t, Want& w)
	{
		w.at = GetTickCount64();
		w.on = g_cfg.enbDof;
		memcpy(w.v, s_manual, sizeof(w.v));
		uint32_t filled = 0;
		{
			lights::Lock lk;
			if (lights::g_cur && lights::g_cur->enb.n > 0) filled = evalByMarker(lights::g_cur->enb, t, w.v);
		}
		if (g_cfg.enbFollow)
		{
			EdDof a, b;
			if (dofAround(t, a, b)) applyEditor(a, b, t, g_cfg.enbStrength, w.v, s_editorLine, sizeof(s_editorLine), filled);
			else snprintf(s_editorLine, sizeof(s_editorLine), "no keyframes in this clip yet");
		}
	}

	void compute(Want& w) { computeAt(game::replayTimeMs(), w); }

	ULONGLONG s_nextVerify = 0;
	int s_resetLogs = 0;

	void push()
	{
		if (!s_ready) return;
		Want w = lights::seqRead(s_want, s_wantSeq);
		if (!w.at || GetTickCount64() - w.at > 1000) compute(w);
		if (!w.on)
		{
			if (s_dirty) restoreBase();
			return;
		}
		// ENB can put its own values back behind our back - a technique switch
		// reloads the shader - and a value only goes in when it changes, so
		// once a second what ENB holds is compared with what was sent.
		const ULONGLONG nowTick = GetTickCount64();
		if (nowTick >= s_nextVerify)
		{
			s_nextVerify = nowTick + 1000;
			for (int i = 0; i < N; ++i)
			{
				if (!s_known[i] || !s_havePushed[i]) continue;
				float back;
				if (!readValue(s_file, s_cat, i, &back) || same(i, back, s_pushed[i])) continue;
				s_havePushed[i] = false;
				if (s_resetLogs < 6) { ++s_resetLogs; logf("enb: ENB had \"%s\" back at %.3f (not %.3f) - sent again", kDefs[i].label, back, s_pushed[i]); }
			}
		}
		const float n = nearClip();
		// Auto: NVE's Distance follows what is in the middle of the picture,
		// eased so the focus pulls rather than jumps (about a quarter second).
		const bool autoF = s_known[I_FOCUS] ? w.v[I_FOCUS] < 0.5f : false;
		float autoD = 0.0f;
		if (autoF && centreFresh())
		{
			const float target = distanceForRaw(s_centreRaw);
			const ULONGLONG t = GetTickCount64();
			const float dt = s_autoAt ? (float)(t - s_autoAt) * 0.001f : 1.0f;
			s_autoAt = t;
			const float k = dt >= 0.25f ? 1.0f : dt / 0.25f;
			s_autoD = s_autoD > 0.0f ? s_autoD + (target - s_autoD) * k : target;
			autoD = s_autoD;
			if (s_autoLogs < 3) { ++s_autoLogs; logf("enb: Auto focus - the middle of the picture is at depth %.5f, about %.1f m: NVE's Distance %.2f", s_centreRaw, centreMetres(), target); }
		}
		else if (autoF && s_autoLogs < 3 && GetTickCount64() > 60000)
		{
			s_autoLogs = 3;
			logf("enb: Auto focus has nothing measured - it needs ReShade with EscoFlare.fx (its EscoFocus pass). The keyframe's Focus Distance is used meanwhile.");
		}
		float sizeMax = 0.0f, sizeBlur = 0.0f;
		blurSizesFor(w.v[I_APERTURE], &sizeMax, &sizeBlur);
		for (int i = 0; i < N; ++i)
		{
			if (!s_known[i]) continue;
			// NVE always runs its Manual technique (TECHNIQUE 1); Auto is ours.
			// Maximum size and Blur size are the Aperture's (4.15).
			const float out = i == I_FOCUS ? (float)TECH_MANUAL
							: i == I_MOUSE ? 0.0f
							: i == 14 ? sizeMax
							: i == 12 ? sizeBlur
							: i == I_DIST ? (autoD > 0.0f ? autoD : enbDistance(w.v[i], n))   // metres -> NVE's Distance
							: w.v[i];
			if (s_havePushed[i] && same(i, s_pushed[i], out)) continue;
			if (writeValue(i, out))
			{
				if (i == I_FOCUS && s_havePushed[i])
					for (int j = 0; j < N; ++j) if (j != I_FOCUS) s_havePushed[j] = false;   // a technique change: all of it again
				s_pushed[i] = out;
				s_havePushed[i] = true;
				s_dirty = true;
				// The first few pushes of what matters are asked back, so the log
				// shows plainly whether ENB's shader took them.
				if ((i == I_DIST || i == I_APERTURE || i == I_FOCUS) && s_checkLogs < 8 && !(i == I_DIST && autoD > 0.0f))
				{
					++s_checkLogs;
					float back = -12345.0f;
					const bool read = readValue(s_file, s_cat, i, &back);
					logf("enb: \"%s\" set to %.3f%s - ENB reports %s%.3f%s", kDefs[i].label, out, i == I_DIST ? " (NVE's units)" : "",
						read ? "" : "(could not read back) ", back, read && same(i, back, out) ? ", taken" : ", NOT TAKEN");
				}
			}
			else if (i == I_FOCUS)
			{
				s_known[I_FOCUS] = false;
				logf("enb: ENB would not switch its technique through the SDK - Focus Mode is out of the menu; "
					 "pick Auto-focus or Manual in ENB's own window (TECHNIQUE of enbdepthoffield.fx)");
			}
			else if (s_setFails++ < 3) logf("enb: ENB would not take \"%s\"", kDefs[i].ui);
		}
	}

	// ENB's callback. The numbering is the SDK's (1 end of frame, 2 start of
	// frame, 3 before it saves its settings, 4 after it loads them); the first
	// few are logged so a different numbering would show at once.
	void WINAPI onEnb(int type)
	{
		InterlockedExchange64(&s_lastCb, (LONGLONG)GetTickCount64());
		if (s_cbLogs < 6) { ++s_cbLogs; logf("enb: callback %d", type); }
		if (type == 1 || type == 2) { push(); return; }
		if (type == 3 && !s_saveIsNoise)
		{
			// "ENB is about to save": put its own values back first, so what it
			// writes is the user's configuration and not this keyframe's numbers.
			// If "3" turns out to arrive constantly, it is not that and is ignored.
			const ULONGLONG now = GetTickCount64();
			if (now - s_saveWindow > 2000) { s_saveWindow = now; s_saveCalls = 0; }
			if (++s_saveCalls > 5) { s_saveIsNoise = true; logf("enb: callback 3 arrives every frame - not treated as a save"); return; }
			restoreBase();
			return;
		}
		if (type == 4) { s_dirty = false; for (int i = 0; i < N; ++i) s_havePushed[i] = false; rereadBase(); }
	}

	// From the game's frame (the replay director hook): compute and publish;
	// push too when ENB is not calling back itself.
	void fromGame()
	{
		if (!s_ready) return;
		Want w;
		compute(w);
		lights::seqWrite(s_want, s_wantSeq, w);
		const ULONGLONG last = (ULONGLONG)s_lastCb;
		if (!last || GetTickCount64() - last > 1000) push();
	}

	// ---- the ENB rows in each keyframe's Depth of Field menu --------------------
	//  Only what matters, in the order ENB's own window has it: the focus
	//  mode, where it focuses, how strong, the near field, the blur and the
	//  bokeh. A row whose switch is off hides what depends on it, and Manual
	//  and Auto show their own aperture and near field power, as ENB does.
	//  The game's column draws 16 rows; the list scrolls if it must: down on
	//  its last row brings the next option in, up on its first the one before.
	int s_list[16] = {};
	int s_listN = 0;
	int g_scroll = 0;                     // the first option shown

	float valueHere(int p, bool* own);
	bool focusAutoHere()
	{
		bool own;
		if (s_known[I_FOCUS]) return valueHere(I_FOCUS, &own) < 0.5f;
		return techAuto();
	}

	void buildList()
	{
		const bool autoF = focusAutoHere();
		s_listN = 0;
		auto add = [&](int p) { if ((isVirtual(p) || s_known[p]) && s_listN < 16) s_list[s_listN++] = p; };
		add(I_FOCUS);
		if (!autoF) add(I_DIST);
		if (s_known[I_APERTURE]) add(V_APERTURE);
		if (s_known[I_NEAR]) add(V_NEAR);
		add(11);                                          // Blur : Chromatic Spread (the sizes follow Aperture)
		if (s_known[17] && s_known[18]) add(V_ANAMORPHIC);
	}
	int listCount() { return s_listN; }
	int listParam(int k) { return (k >= 0 && k < s_listN) ? s_list[k] : -1; }
	// Changing these adds or removes rows.
	bool reshapes(int p) { return p == I_FOCUS; }

	const char* menuLabel(int p)
	{
		switch (p)
		{
		case I_FOCUS:           return "Focus Mode";
		case V_APERTURE:        return "Aperture";
		case V_NEAR:            return "Near Field Blur";
		case V_ANAMORPHIC:      return "Anamorphic";
		case I_DIST:            return "Focus Distance";
		case I_APERTURE:
		case I_AUTO_APERTURE:   return "Aperture";
		case I_NEAR:            return "Near Field Blur";
		case I_NEARPOWER:
		case I_AUTO_NEARPOWER:  return "Near Field Power";
		case 12: return "Blur Size";
		case 14: return "Maximum Size";
		case 11: return "Chromatic Spread";
		case 17: return "Anamorphic";
		case 18: return "Anamorphic Stretch";
		default: return p >= 0 && p < N ? kDefs[p].label : "";
		}
	}

	// One press of left or right. 0 = this option moves by 10% of itself a
	// press, so it covers small values and large ones alike. Held, a press
	// counts several times (the menu's hold acceleration, ecs_menu.inc).
	const float kStep[N] = { 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.5f, 0.01f, 0.01f,
							 0.5f, 0.05f, 1.0f, 0.5f, 0.05f, 0.05f, 1.0f, 0.05f, 0.05f, 0.01f, 1.0f, 0.01f,
							 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };

	bool markerTimeNow(float* t)
	{
		const void* m = game::currentMarker();
		if (!m) return false;
		__try { memcpy(t, (const uint8_t*)m + mk::OFF_TimeMs, 4); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		return isfinite(*t);
	}

	float valueHere(int p, bool* own);

	// A virtual row's value: Aperture 0..100 (%), Near Field Blur -1 (Off) or
	// 1..100 (%), Anamorphic 0 (Off) or its stretch.
	float virtualHere(int p, bool* own)
	{
		bool o1 = false, o2 = false;
		const bool a = false;                // 4.14: NVE runs its Manual technique, Auto included
		float r = 0.0f;
		switch (p)
		{
		case V_APERTURE:
			r = a ? pctAuto(valueHere(I_AUTO_APERTURE, &o1)) : pctManual(valueHere(I_APERTURE, &o1));
			break;
		case V_NEAR:
		{
			const float on = valueHere(I_NEAR, &o1);
			const float pw = valueHere(a ? I_AUTO_NEARPOWER : I_NEARPOWER, &o2);
			r = on > 0.5f ? nearPct(pw) : -1.0f;
			break;
		}
		case V_ANAMORPHIC:
		{
			const float on = valueHere(17, &o1);
			const float st = valueHere(18, &o2);
			r = on > 0.5f ? (st > 0.05f ? st : 0.05f) : 0.0f;
			break;
		}
		}
		*own = o1 || o2;
		return r;
	}

	// This keyframe's value of option p, and whether it is its own.
	float valueHere(int p, bool* own)
	{
		*own = false;
		if (isVirtual(p)) return virtualHere(p, own);
		float t;
		if (p < 0 || p >= N || !markerTimeNow(&t)) return p >= 0 && p < N ? s_manual[p] : 0.0f;
		{
			lights::Lock lk;
			float v;
			if (lights::g_cur && lights::enbKeyValue(lights::g_cur->enb, t, p, &v)) { *own = true; return v; }
		}
		Want w;
		computeAt(t, w);
		return w.v[p];
	}

	// `delta` is +-1 a press, more while it is held.
	float stepped(int p, float cur, int delta)
	{
		const Def& d = kDefs[p];
		float v;
		if (d.kind == K_BOOL || d.kind == K_ENUM) v = cur > 0.5f ? 0.0f : 1.0f;
		else if (kStep[p] == 0.0f)
		{
			const float small = (d.hi - d.lo) * 0.0005f;    // where 10% of itself stops meaning anything
			const float f = powf(1.1f, (float)(delta > 0 ? delta : -delta));
			if (delta > 0) v = cur < d.lo + small ? d.lo + small : cur * f;
			else { v = cur / f; if (v < d.lo + small) v = d.lo; }
		}
		else v = cur + kStep[p] * (float)delta;
		if (v < d.lo) v = d.lo;
		if (v > d.hi) v = d.hi;
		if (d.kind == K_INT) v = floorf(v + 0.5f);
		else if (d.kind == K_FLOAT) v = floorf(v * 10000.0f + 0.5f) / 10000.0f;
		return v;
	}

	// Left / right on an option row: this keyframe gets its own value.
	void stepHere(int p, int delta)
	{
		float t;
		if (!markerTimeNow(&t) || !(isVirtual(p) || (p >= 0 && p < N))) return;
		lights::Lock lk;                     // re-entrant: the reads below take it too
		if (!lights::g_cur) return;
		lights::EnbTrack& tr = lights::g_cur->enb;
		Want w;
		computeAt(t, w);                     // what the keyframe showed: the seed of a new key
		bool own;
		const bool a = false;                // 4.14: NVE runs its Manual technique, Auto included
		const float cur = valueHere(p, &own);
		bool ch = false;
		auto put = [&](int q, float v) { if (lights::setEnbParam(tr, t, q, v, w.v)) ch = true; };
		switch (p)
		{
		case V_APERTURE:
		{
			const float pc = clampf(floorf(cur + 0.5f) + (float)delta, 0.0f, 100.0f);
			put(a ? I_AUTO_APERTURE : I_APERTURE, a ? autoFromPct(pc) : manualFromPct(pc));
			break;
		}
		case V_NEAR:
		{
			if (cur < 0.0f)                                   // Off
			{
				if (delta > 0) { put(I_NEAR, 1.0f); put(a ? I_AUTO_NEARPOWER : I_NEARPOWER, powerFromPct(10.0f)); }
				break;
			}
			const float pc = cur + 5.0f * (float)delta;
			if (pc < 1.0f) { put(I_NEAR, 0.0f); break; }
			put(I_NEAR, 1.0f);
			put(a ? I_AUTO_NEARPOWER : I_NEARPOWER, powerFromPct(pc > 100.0f ? 100.0f : pc));
			break;
		}
		case V_ANAMORPHIC:
		{
			if (cur <= 0.0f)                                  // Off
			{
				if (delta > 0) { bool o; const float st = valueHere(18, &o); put(17, 1.0f); put(18, st >= 1.0f ? st : 1.5f); }
				break;
			}
			const float st = floorf((cur + 0.1f * (float)delta) * 100.0f + 0.5f) / 100.0f;
			if (st < 1.0f) { put(17, 0.0f); break; }
			put(17, 1.0f);
			put(18, st > 4.0f ? 4.0f : st);
			break;
		}
		case I_FOCUS:
			put(I_FOCUS, stepped(p, cur, delta));
			break;
		default:
			put(p, stepped(p, cur, delta));
			break;
		}
		if (ch) lights::markDirty();
	}

	// Enter on an option row: back to the keyframes around it.
	void clearHere(int p)
	{
		float t;
		if (!markerTimeNow(&t)) return;
		lights::Lock lk;
		if (!lights::g_cur) return;
		auto clr = [&](int q) { lights::clearEnbParam(lights::g_cur->enb, t, q); };
		if (p == I_DIST && centreFresh())
		{
			// Enter on Focus Distance: focus on what is in the middle of the picture
			Want w;
			computeAt(t, w);
			if (lights::setEnbParam(lights::g_cur->enb, t, I_DIST, floorf(centreMetres() * 100.0f + 0.5f) / 100.0f, w.v)) lights::markDirty();
			return;
		}
		switch (p)
		{
		case V_APERTURE:   clr(I_APERTURE); clr(I_AUTO_APERTURE); break;
		case V_NEAR:       clr(I_NEAR); clr(I_NEARPOWER); clr(I_AUTO_NEARPOWER); break;
		case V_ANAMORPHIC: clr(17); clr(18); break;
		default:           if (p >= 0 && p < N) clr(p); else return; break;
		}
		lights::markDirty();
	}

	const char* format(int p, float v, char* buf, int cap)
	{
		if (p == V_APERTURE) { snprintf(buf, cap, "%.0f%%", v); return buf; }
		if (p == V_NEAR) { if (v < 0.0f) return "Off"; snprintf(buf, cap, "%.0f%%", v); return buf; }
		if (p == V_ANAMORPHIC) { if (v <= 0.0f) return "Off"; snprintf(buf, cap, "%.2fx", v); return buf; }
		if (p < 0 || p >= N) return "";
		const Def& d = kDefs[p];
		if (p == I_FOCUS)
		{
			if (v > 0.5f) return "Manual";
			if (!centreFresh()) return "Auto";
			const float m = centreMetres();
			snprintf(buf, cap, m < 10.0f ? "Auto (%.1f m)" : "Auto (%.0f m)", m);
			return buf;
		}
		if (d.kind == K_BOOL) return v > 0.5f ? "On" : "Off";
		if (d.kind == K_INT) { snprintf(buf, cap, "%d", (int)floorf(v + 0.5f)); return buf; }
		if (p == I_DIST) { snprintf(buf, cap, v < 10.0f ? "%.2f m" : "%.1f m", v); return buf; }
		snprintf(buf, cap, v < 1.0f ? "%.3f" : v < 100.0f ? "%.2f" : "%.1f", v);
		return buf;
	}

	// One line on what an option does, for the help line under the column.
	const char* hint(int p)
	{
		switch (p)
		{
		case I_FOCUS:           return "Auto: the focus follows what is in the middle of the picture, as the clip plays. Manual: at Focus Distance.";
		case V_APERTURE:        return "How much blur: 0% none at all, 100% the most. Hold left/right to go faster.";
		case V_NEAR:            return "Blur in front of the focus too: Off, or how much of the blur behind it.";
		case V_ANAMORPHIC:      return "Oval bokeh, like an anamorphic lens: Off, or how stretched.";
		case I_DIST:            return centreFresh() ? "Where the picture is sharp, in metres. Enter: focus on what is in the middle of the picture." : "Where the picture is sharp, in metres from the camera.";
		case I_APERTURE:        return "How fast the blur comes in past the focus. Small numbers: gentle.";
		case I_AUTO_APERTURE:   return "How strong the blur is.";
		case I_NEAR:            return "Blur what is closer than the focus too.";
		case I_NEARPOWER:
		case I_AUTO_NEARPOWER:  return "Higher = less blur in front of the focus.";
		case 7:  return "How fast the auto-focus follows.";
		case 8:  return "Makes the auto-focus reach further.";
		case 9:  return "Where the auto-focus looks, left to right.";
		case 10: return "Where the auto-focus looks, top to bottom.";
		case 11: return "Colour fringes at the edges of the blur. They grow with the blur, so they show more at a higher Aperture.";
		case 12: return "How big the blur is.";
		case 13: return "More steps, smoother blur, slower.";
		case 14: return "The largest the blur can get.";
		case 15: return "Bokeh quality.";
		case 16: return "Lower renders the blur at less resolution - faster.";
		case 17: return "Oval bokeh, like an anamorphic lens.";
		case 18: return "How oval.";
		case 19: return "How the blur spreads between near and far.";
		case 20: return "Barrel distortion of the lens.";
		case 21: return "Zoom in so the distortion shows no edges.";
		case 22: return "How far it zooms.";
		default: return "";
		}
	}

	// From the worker, about once a second.
	void poll(ULONGLONG now)
	{
		if (now < s_nextPoll) return;
		s_nextPoll = now + 1000;
		for (int i = 0; i < N; ++i) kinds[i] = (uint8_t)kDefs[i].kind;
		if (!resolve())
		{
			static int s_missLogs = 0;
			if (now > 30000 && s_missLogs++ == 0)
			{
				logf("enb: ENB not found in any of the %d modules in the game (nor by path, nor from a file)", s_modsSeen);
				for (int j = 0; j < s_d3dSeenCount; ++j) logf("enb:   loaded: %s", s_d3dSeen[j]);
				if (!s_d3dSeenCount) logf("enb:   no d3d11.dll or dxgi.dll in the module list at all");
			}
			return;
		}
		if (!s_ready) { calibrate(); return; }
		if (!g_cfg.enbDof) rereadBase();
		static ULONGLONG s_nextTech = 0;
		if (now >= s_nextTech) { s_nextTech = now + 3000; readTechnique(); }   // it can be switched in ENB's window
	}

	void setOn(bool on)
	{
		g_cfg.enbDof = on;
		iniWriteInt("EnbDof", on ? 1 : 0);
		logf("enb: the ENB option is %s", on ? "on - the Rockstar Editor's depth of field is drawn by ENB" : "off - ENB's own values are back");
	}
}
