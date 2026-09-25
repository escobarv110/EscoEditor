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
		{ "--> Manual Focus : Distance",                K_FLOAT,  0.3f,  200.0f, "Focus distance (m)" },
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
		{ "TECHNIQUE",                                  K_ENUM,   0.0f,    2.0f, "Focus mode" },
		// Not ENB's either: this keyframe's blur on (1) or off (0), blending
		// from keyframe to keyframe like the rest (4.20). Never sent to ENB.
		{ "",                                           K_FLOAT,  0.0f,    1.0f, "Keyframe DOF" },
	};
	enum { I_MOUSE = 0, I_NEAR = 1, I_AUTO_NEARPOWER = 2, I_DIST = 3, I_APERTURE = 4, I_NEARPOWER = 5, I_AUTO_APERTURE = 6,
	       I_SEC_NEAR = 23, I_SEC_MANUAL = 24, I_SEC_AUTO = 25, I_SEC_BLUR = 26, I_SEC_QUALITY = 27, I_SEC_MISC = 28,
	       I_FOCUS = 29, I_ENABLED = 30,
	       // The five label slots after them hold the character a keyframe's
	       // Player focus follows (4.20): its model hash in two halves, and where
	       // it was when picked. They were never sent to ENB and still are not.
	       I_TGT_HI = 23, I_TGT_LO = 24, I_TGT_X = 25, I_TGT_Y = 26, I_TGT_Z = 27 };
	inline bool isOurs(int i) { return i == I_ENABLED; }
	inline bool isLabel(int i) { return i >= I_SEC_NEAR && i <= I_SEC_MISC; }
	// Focus Mode, per keyframe (EscoEditor's own - NVE always runs its Manual
	// technique): the middle of the picture, a distance, or the player.
	enum { FM_AUTO = 0, FM_MANUAL = 1, FM_PLAYER = 2 };
	inline int focusModeOf(float v) { const int m = (int)floorf(v + 0.5f); return m < FM_AUTO ? FM_AUTO : m > FM_PLAYER ? FM_PLAYER : m; }

	// Three rows stand for more than one ENB option each (4.13):
	//   Aperture         a percentage. NVE's two techniques read two different
	//                    apertures on two different scales - Manual's is spent
	//                    by about 0.1, Auto-focus's wants 2 to 10 - so the same
	//                    percentage is converted for whichever runs.
	//   Near Field Blur  Off, or how much: the near field switch and its power.
	//   Anamorphic       Off, or how stretched: the switch and the stretch.
	enum { V_APERTURE = 100, V_NEAR = 101, V_ANAMORPHIC = 102, V_TARGET = 103 };
	inline bool isVirtual(int p) { return p >= V_APERTURE && p <= V_TARGET; }
	inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
	// Aperture is THE blur amount: 0% no blur at all, 100% the most. NVE's own
	// aperture is spent by about 0.1 - past that, everything behind the focus
	// is already as blurred as it gets - so the blur you see is set by two more
	// numbers: DOF : Maximum size (how far the blur reaches, 2..20) and Blur :
	// Size (a softening over it, 0..2). Since 4.18 those two rows are what the
	// blur is at 100%, and the Aperture scales all three together, so it works
	// whatever the size rows say (4.17's "Auto" sizes stopped following it as
	// soon as a keyframe had sizes of its own - then Aperture did next to
	// nothing). Stored as NVE's aperture: 0.12 at 100%.
	constexpr float kApertureFull = 0.12f;
	inline float manualFromPct(float pc) { return kApertureFull * clampf(pc, 0.0f, 100.0f) / 100.0f; }
	inline float pctManual(float a)      { return clampf(a / kApertureFull, 0.0f, 1.0f) * 100.0f; }
	inline void blurSizesFor(float a, float userMax, float userBlur, float* maxSize, float* blurSize)
	{
		const float f = clampf(a / kApertureFull, 0.0f, 1.0f);
		*maxSize = 2.0f + (clampf(userMax, 2.0f, 20.0f) - 2.0f) * f;
		*blurSize = clampf(userBlur, 0.0f, 2.0f) * f;
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
	float     s_autoClip = -1.0f;         // the clip clock at the last ease (4.18)
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
				s_known[i] = !isLabel(i) && !isOurs(i) && readValue(s_file, s_cat, i, &s_base[i]);
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
			s_known[I_ENABLED] = true;                // ours: this keyframe's blur on or off
			s_base[I_ENABLED] = s_manual[I_ENABLED] = 1.0f;
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
			if (s_known[i] && !isOurs(i) && !(i == I_FOCUS && s_focusFromIni)) readValue(s_file, s_cat, i, &s_base[i]);
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

	// ---- the player (Focus Mode Player, 4.18) ------------------------------------
	// Which ped is the player during a replay? Two ways, and the log says which:
	//  1. the ped that carries player data. A player's ped points to its player
	//     record and the record points back to the ped; other peds have nothing
	//     there. Where those two pointers sit is learned from the pool itself:
	//     of every (ped+A -> record, record+B -> ped) pair, the one exactly one
	//     ped has, with several peds around. A ped far from the camera is not
	//     it (the real player waits, hidden, while the replay plays).
	//  2. otherwise the character nearest the middle of the picture - and once
	//     picked, the same one, as long as it stays in view.
	// The focus is its chest, measured along the camera's view.
	volatile float     s_playerMetres = 0.0f;
	volatile ULONGLONG s_playerAt = 0;
	void*    s_plPed = nullptr;
	uint32_t s_plModel = 0;
	int      s_linkA = -1, s_linkB = -1;       // learned: ped+A -> player data, data+B -> the ped
	int      s_linkTries = 0;
	int      s_plLogs = 0, s_plHow = 0;          // 0 none, 1 player data, 2 nearest the middle
	ULONGLONG s_nextLearn = 0;

	bool playerFresh() { return s_playerAt && GetTickCount64() - s_playerAt < 750; }

	inline bool plausiblePtr(uintptr_t v) { return v > 0x10000 && v < 0x00007FFFFFFFFFFFull && (v & 7) == 0; }

	// Does ped+a point to something that points back to the ped at +b?
	bool linkHolds(const uint8_t* ped, int a, int b)
	{
		__try
		{
			const uintptr_t r = *(const uintptr_t*)(ped + a);
			if (!plausiblePtr(r)) return false;
			return *(const uintptr_t*)(r + b) == (uintptr_t)ped;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	// Learn the player-data link from these peds (pure enough to be tested):
	// the (A, B) pair exactly one of them has. Needs at least four peds, or
	// every link of a lone ped would look unique.
	constexpr int kLinkA0 = 0x1000, kLinkA1 = 0x1200, kLinkB0 = 0x100, kLinkB1 = 0x400;
	constexpr int kLinkNA = (kLinkA1 - kLinkA0) / 8, kLinkNB = (kLinkB1 - kLinkB0) / 8;
	uint16_t s_linkCount[kLinkNA][kLinkNB];
	void*    s_linkFirst[kLinkNA][kLinkNB];

	// Every pair one ped has: ped+A read once, then the record scanned for the ped.
	void countLinks(const uint8_t* ped)
	{
		for (int a = 0; a < kLinkNA; ++a)
		{
			uintptr_t r = 0;
			__try { r = *(const uintptr_t*)(ped + kLinkA0 + a * 8); }
			__except (EXCEPTION_EXECUTE_HANDLER) { r = 0; }
			if (!plausiblePtr(r)) continue;
			if (r >= (uintptr_t)ped && r < (uintptr_t)ped + 0x2000) continue;     // a pointer inside the ped itself
			__try
			{
				for (int b = 0; b < kLinkNB; ++b)
					if (*(const uintptr_t*)(r + kLinkB0 + b * 8) == (uintptr_t)ped && s_linkCount[a][b]++ == 0) s_linkFirst[a][b] = (void*)ped;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}
	}

	bool learnLink(void* const* peds, int n, int* outA, int* outB, void** who)
	{
		if (n < 4) return false;
		memset(s_linkCount, 0, sizeof(s_linkCount));
		for (int i = 0; i < n; ++i) countLinks((const uint8_t*)peds[i]);
		int bestA = -1, bestB = -1;
		void* only = nullptr;
		for (int a = 0; a < kLinkNA; ++a)
			for (int b = 0; b < kLinkNB; ++b)
			{
				if (s_linkCount[a][b] != 1) continue;
				if (!only) { only = s_linkFirst[a][b]; bestA = kLinkA0 + a * 8; bestB = kLinkB0 + b * 8; }
				else if (s_linkFirst[a][b] != only) return false;    // two peds, each with a link of its own: no answer
			}
		if (!only) return false;
		*outA = bestA; *outB = bestB; if (who) *who = only;
		return true;
	}

	// The character nearest the middle of the picture (pure): the smallest angle
	// off the view, in front of the camera, within 120 m. -1 when none.
	int nearestToCentre(const float* cam, const float* fwd, const float (*pos)[3], int n, float* zOut)
	{
		int best = -1;
		float bestT = 0.0f;
		for (int i = 0; i < n; ++i)
		{
			const float v[3] = { pos[i][0] - cam[0], pos[i][1] - cam[1], pos[i][2] - cam[2] };
			const float z = v[0] * fwd[0] + v[1] * fwd[1] + v[2] * fwd[2];
			if (z < 0.3f || z > 120.0f) continue;
			const float l2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2] - z * z;
			const float tanA = sqrtf(l2 > 0.0f ? l2 : 0.0f) / z;
			if (tanA > 0.7f) continue;                         // about 35 degrees off the middle
			if (best < 0 || tanA < bestT) { best = i; bestT = tanA; if (zOut) *zOut = z; }
		}
		return best;
	}

	// Its chest, and how far that is along the view.
	bool chestOf(void* ped, const lights::Cam& cam, float* z, float* at)
	{
		lights::Frame f;
		if (!lights::frameOfEntity(ped, f)) return false;
		for (int k = 0; k < 3; ++k) at[k] = f.d[k] + f.c[k] * 0.45f;
		const float v[3] = { at[0] - cam.pos[0], at[1] - cam.pos[1], at[2] - cam.pos[2] };
		*z = v[0] * cam.fwd[0] + v[1] * cam.fwd[1] + v[2] * cam.fwd[2];
		return true;
	}

	// The people in view, left to right on screen - the order the Character
	// row steps through them in.
	struct Person { void* ped; float chest[3]; float sx; float z; };
	int s_peopleCount = 0;

	int peopleOnScreen(const lights::Cam& cam, Person* out, int cap)
	{
		lights::PoolView v;
		if (!lights::poolView(lights::E_PED, v) || v.size <= 0) return 0;
		int n = 0;
		for (int i = 0; i < v.size && n < cap; ++i)
		{
			void* e = lights::slotAt(v, i);
			if (!e) continue;
			Person p;
			if (!chestOf(e, cam, &p.z, p.chest) || p.z < 0.3f || p.z > 150.0f) continue;
			const float d[3] = { p.chest[0] - cam.pos[0], p.chest[1] - cam.pos[1], p.chest[2] - cam.pos[2] };
			p.sx = (d[0] * cam.right[0] + d[1] * cam.right[1] + d[2] * cam.right[2]) / p.z;
			const float sy = (d[0] * cam.up[0] + d[1] * cam.up[1] + d[2] * cam.up[2]) / p.z;
			if (fabsf(p.sx) > 2.0f || fabsf(sy) > 1.2f) continue;            // off the picture
			p.ped = e;
			int at = n++;
			while (at > 0 && out[at - 1].sx > p.sx) { out[at] = out[at - 1]; --at; }
			out[at] = p;
		}
		return n;
	}

	// A picked character is its model hash (two 16-bit halves: a float holds
	// each exactly) and where it was. 0/1 left in those slots by 4.9's section
	// switches is not a pick.
	inline uint32_t modelFrom(float hi, float lo)
	{
		if (!(hi >= 0.0f && hi < 65536.0f && lo >= 0.0f && lo < 65536.0f)) return 0;
		const uint32_t m = ((uint32_t)hi << 16) | (uint32_t)lo;
		return m > 0x10001u ? m : 0;
	}
	inline void modelTo(uint32_t m, float* hi, float* lo) { *hi = (float)(m >> 16); *lo = (float)(m & 0xFFFFu); }

	void*    s_tgtPed = nullptr;               // the picked character, as last found
	uint32_t s_tgtModel = 0;
	float    s_tgtPick[3] = {};                // where it was picked: a new pick starts the search there
	float    s_tgtLast[3] = {};                // where it was last seen
	volatile float     s_markPos[3] = {};      // the followed character's chest, for the marker
	volatile ULONGLONG s_markAt = 0;
	volatile bool      s_markerOn = false;     // the menu's focus is on the Character row

	void publishFollowed(const float* at, float z)
	{
		if (z >= 0.3f) { s_playerMetres = z; s_playerAt = GetTickCount64(); }
		for (int k = 0; k < 3; ++k) s_markPos[k] = at[k];
		s_markAt = GetTickCount64();
	}

	// Every frame on the game thread while the ENB option is on. `want` is
	// what the keyframes ask for at the playhead, the pick included.
	void trackPlayer(const float* want)
	{
		const lights::Ctx c = lights::ctx();
		if (!c.valid || GetTickCount64() - c.at > 500) return;

		// A character picked on the Character row: the same one, frame to frame.
		const uint32_t pick = modelFrom(want[I_TGT_HI], want[I_TGT_LO]);
		if (pick)
		{
			const float at0[3] = { want[I_TGT_X], want[I_TGT_Y], want[I_TGT_Z] };
			if (pick != s_tgtModel || memcmp(at0, s_tgtPick, sizeof(at0)) != 0)
			{
				s_tgtModel = pick;
				memcpy(s_tgtPick, at0, sizeof(at0));
				memcpy(s_tgtLast, at0, sizeof(at0));
				s_tgtPed = nullptr;
			}
			lights::PoolView v;
			if (!lights::poolView(lights::E_PED, v)) return;
			void* found = nullptr;
			float bestD = 0.0f;
			for (int i = 0; i < v.size; ++i)
			{
				void* e = lights::slotAt(v, i);
				if (!e || lights::modelOf(e) != pick) continue;
				if (e == s_tgtPed) { found = e; break; }                       // still the one it was
				lights::Frame f;
				if (!lights::frameOfEntity(e, f)) continue;
				const float d = lights::dist2(f.d, s_tgtLast);
				if (!found || d < bestD) { found = e; bestD = d; }
			}
			if (!found) return;
			float z, at[3];
			if (!chestOf(found, c.cam, &z, at)) return;
			if (found != s_tgtPed && s_plLogs < 12) { ++s_plLogs; logf("enb: Player focus follows the character picked for this keyframe, %.1f m away", z); }
			s_tgtPed = found;
			memcpy(s_tgtLast, at, sizeof(at));
			s_plPed = found; s_plModel = pick; s_plHow = 3;
			publishFollowed(at, z);
			return;
		}

		// No pick: the player - the ped with player data, else the character
		// nearest the middle of the picture, kept while it stays in view.
		lights::PoolView v;
		if (!lights::poolView(lights::E_PED, v) || v.size <= 0) return;
		constexpr int kMax = 256;
		void* peds[kMax];
		float pos[kMax][3];
		int n = 0;
		for (int i = 0; i < v.size && n < kMax; ++i)
		{
			void* e = lights::slotAt(v, i);
			if (!e) continue;
			float z, at[3];
			if (!chestOf(e, c.cam, &z, at)) continue;
			peds[n] = e;
			memcpy(pos[n], at, sizeof(at));
			++n;
		}
		if (n <= 0) return;

		const ULONGLONG now = GetTickCount64();
		if (s_linkA < 0 && s_linkTries < 3 && now >= s_nextLearn && n >= 4)
		{
			// a few tries, 20 s apart: the scene may hold too few peds at first
			++s_linkTries;
			s_nextLearn = now + 20000;
			void* who = nullptr;
			if (learnLink(peds, n, &s_linkA, &s_linkB, &who))
				logf("enb: Player focus - one ped of %d carries player data (ped+0x%X -> its record, record+0x%X -> the ped); that is the player", n, s_linkA, s_linkB);
			else if (s_linkTries == 1)
				logf("enb: Player focus - no ped of %d carries player data during the replay; the player is the character nearest the middle of the picture", n);
		}
		void* found = nullptr;
		int how = 0;
		if (s_linkA >= 0)
		{
			int holders = 0;
			for (int i = 0; i < n; ++i)
				if (linkHolds((const uint8_t*)peds[i], s_linkA, s_linkB)) { ++holders; found = peds[i]; }
			if (holders > 1) { found = nullptr; s_linkA = -1; logf("enb: Player focus - %d peds share that link, so it is not player data; looking again", holders); }
			float z = 0.0f, at[3];
			if (found && (!chestOf(found, c.cam, &z, at) || z < 0.3f || z > 300.0f)) found = nullptr;   // the hidden real player, far away
			if (found) how = 1;
		}
		if (!found && s_plPed && lights::modelOf(s_plPed) == s_plModel)
		{
			for (int i = 0; i < n; ++i)
				if (peds[i] == s_plPed)
				{
					float z, at[3];
					if (chestOf(s_plPed, c.cam, &z, at) && z > 0.3f && z < 150.0f) { found = s_plPed; how = 2; }
					break;
				}
		}
		if (!found)
		{
			const int k = nearestToCentre(c.cam.pos, c.cam.fwd, (const float(*)[3])pos, n, nullptr);
			if (k >= 0) { found = peds[k]; how = 2; }
		}
		if (!found) return;
		float z, at[3];
		if (!chestOf(found, c.cam, &z, at) || z < 0.3f) return;
		if (found != s_plPed || how != s_plHow)
		{
			if (s_plLogs < 12) { ++s_plLogs; logf("enb: Player focus follows %s, %.1f m away", how == 1 ? "the ped with player data" : "the character nearest the middle of the picture", z); }
			s_plPed = found;
			s_plModel = lights::modelOf(found);
			s_plHow = how;
		}
		publishFollowed(at, z);
	}

	// ---- one frame -------------------------------------------------------------
	// What ENB should hold, computed on the game thread and published; pushed
	// from ENB's thread. A snapshot older than a second is not trusted - an
	// export does not tick the replay director - and the push computes afresh.
	struct Want { float v[N]; ULONGLONG at; bool on; uint32_t filled; };   // filled: options the keyframes set themselves
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
		w.filled = filled;
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
	bool s_techPush = true;                  // ENB takes TECHNIQUE (the nudge to its Manual technique)

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
		// Auto: NVE's Distance follows what is in the middle of the picture;
		// Player: the player's chest (the middle of the picture while no player
		// is found). Eased, so the focus pulls rather than jumps (about a
		// quarter second).
		const int fm = s_known[I_FOCUS] ? focusModeOf(w.v[I_FOCUS]) : FM_MANUAL;
		const bool autoF = fm == FM_AUTO || fm == FM_PLAYER;
		float autoD = 0.0f;
		const bool byPlayer = fm == FM_PLAYER && playerFresh();
		if (byPlayer || (autoF && centreFresh()))
		{
			const float target = byPlayer ? enbDistance(clampf(s_playerMetres, 0.3f, 200.0f), n) : distanceForRaw(s_centreRaw);
			// Eased on the CLIP's clock, so an export - the editor's, or Rockstar
			// Editor+'s frame-by-frame render at whatever speed the PC manages -
			// pulls focus exactly as the playback did. A jump (a seek, the next
			// clip) snaps; a paused clip settles on the real clock instead.
			const ULONGLONG t = GetTickCount64();
			const float clip = game::replayTimeMs();
			float dt;
			if (s_autoClip >= 0.0f && clip != s_autoClip)
			{
				const float d = fabsf(clip - s_autoClip);
				dt = d > 500.0f ? 1.0f : d * 0.001f;
			}
			else dt = s_autoAt ? (float)(t - s_autoAt) * 0.001f : 1.0f;
			s_autoClip = clip;
			s_autoAt = t;
			const float k = dt >= 0.25f ? 1.0f : dt / 0.25f;
			s_autoD = s_autoD > 0.0f ? s_autoD + (target - s_autoD) * k : target;
			autoD = s_autoD;
			if (s_autoLogs < 3 && !byPlayer) { ++s_autoLogs; logf("enb: Auto focus - the middle of the picture is at depth %.5f, about %.1f m: NVE's Distance %.2f", s_centreRaw, centreMetres(), target); }
		}
		else if (autoF && s_autoLogs < 3 && GetTickCount64() > 60000)
		{
			s_autoLogs = 3;
			logf("enb: Auto focus has nothing measured - it needs ReShade with EscoFlare.fx (its EscoFocus pass). The keyframe's Focus Distance is used meanwhile.");
		}
		float sizeMax = 0.0f, sizeBlur = 0.0f;
		// Keyframe DOF Off takes the blur away (fading between keyframes).
		const float apEff = clampf(w.v[I_APERTURE], 0.0f, kApertureFull) * clampf(w.v[I_ENABLED], 0.0f, 1.0f);
		blurSizesFor(apEff, w.v[14], w.v[12], &sizeMax, &sizeBlur);
		for (int i = 0; i < N; ++i)
		{
			if (!s_known[i]) continue;
			if (i == I_FOCUS && !s_techPush) continue;
			if (isOurs(i)) continue;
			// NVE always runs its Manual technique (TECHNIQUE 1); Auto and Player
			// are ours. The sizes are the keyframe's, scaled by the Aperture.
			const float out = i == I_FOCUS ? (float)TECH_MANUAL
							: i == I_MOUSE ? 0.0f
							: i == 14 ? sizeMax
							: i == 12 ? sizeBlur
							: i == I_APERTURE ? apEff                            // an old key's 1.38 is 100%, not a paper-thin focus
							: i == I_DIST ? (autoD > 0.0f ? autoD : enbDistance(clampf(w.v[i], 0.3f, 200.0f), n))   // metres -> NVE's Distance
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
				s_techPush = false;             // Focus Mode is ours; only the nudge to Manual is dropped
				logf("enb: ENB would not take TECHNIQUE through the SDK - make sure its technique is \"Gameplay DOF Manual\" in ENB's window");
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
		if (g_cfg.enbDof) trackPlayer(w.v);
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
	int focusModeHere()
	{
		bool own;
		if (s_known[I_FOCUS]) return focusModeOf(valueHere(I_FOCUS, &own));
		return techAuto() ? FM_AUTO : FM_MANUAL;
	}
	bool focusAutoHere() { return focusModeHere() == FM_AUTO; }

	void buildList()
	{
		s_listN = 0;
		auto add = [&](int p) { if ((isVirtual(p) || s_known[p]) && s_listN < 16) s_list[s_listN++] = p; };
		add(I_ENABLED);
		{
			bool o;
			if (valueHere(I_ENABLED, &o) < 0.5f) return;      // Off: nothing else to set on this keyframe
		}
		const int fm = focusModeHere();
		add(I_FOCUS);
		if (fm == FM_MANUAL) add(I_DIST);
		if (fm == FM_PLAYER) add(V_TARGET);
		if (s_known[I_APERTURE]) add(V_APERTURE);
		add(14);                                          // DOF : Maximum size  - what the blur reaches at 100%
		add(12);                                          // Blur : Size         - its softening at 100%
		add(I_NEAR);                                      // Near Field Blur: On / Off
		{
			bool o;
			if (s_known[I_NEARPOWER] && valueHere(I_NEAR, &o) > 0.5f) add(V_NEAR);   // and how much, while it is on
		}
		add(11);                                          // Blur : Chromatic Spread
		if (s_known[17] && s_known[18]) add(V_ANAMORPHIC);
	}
	int listCount() { return s_listN; }
	int listParam(int k) { return (k >= 0 && k < s_listN) ? s_list[k] : -1; }
	// Changing these adds or removes rows.
	bool reshapes(int p) { return p == I_FOCUS || p == I_NEAR || p == I_ENABLED; }

	const char* menuLabel(int p)
	{
		switch (p)
		{
		case I_ENABLED:         return "Keyframe DOF";
		case V_TARGET:          return "Character";
		case I_FOCUS:           return "Focus Mode";
		case V_APERTURE:        return "Aperture";
		case V_NEAR:            return "Near Field Amount";
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
			r = nearPct(valueHere(a ? I_AUTO_NEARPOWER : I_NEARPOWER, &o1));
			break;
		case V_ANAMORPHIC:
		{
			const float on = valueHere(17, &o1);
			const float st = valueHere(18, &o2);
			r = on > 0.5f ? (st > 0.05f ? st : 0.05f) : 0.0f;
			break;
		}
		case V_TARGET:
		{
			// -1 no pick (the player is found by itself), -2 picked but not in
			// view, else which of the people on screen, left to right
			const uint32_t pick = modelFrom(valueHere(I_TGT_HI, &o1), valueHere(I_TGT_LO, &o2));
			const lights::Ctx c = lights::ctx();
			Person ppl[64];
			const int n = c.valid ? peopleOnScreen(c.cam, ppl, 64) : 0;
			s_peopleCount = n;
			r = -1.0f;
			if (pick)
			{
				r = -2.0f;
				for (int i = 0; i < n; ++i) if (ppl[i].ped == s_tgtPed) { r = (float)i; break; }
			}
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
		if (p < 0 || p >= N) return 0.0f;
		if (!markerTimeNow(&t)) return p == I_DIST ? clampf(s_manual[p], kDefs[p].lo, kDefs[p].hi) : s_manual[p];
		{
			lights::Lock lk;
			float v;
			if (lights::g_cur && lights::enbKeyValue(lights::g_cur->enb, t, p, &v)) { *own = true; return p == I_DIST ? clampf(v, kDefs[p].lo, kDefs[p].hi) : v; }
		}
		Want w;
		computeAt(t, w);
		return p == I_DIST ? clampf(w.v[p], kDefs[p].lo, kDefs[p].hi) : w.v[p];
	}

	// `delta` is +-1 a press, more while it is held.
	// A Focus Distance step that suits where it is: 5 cm under a metre, 10 m
	// past a hundred. 4.17 moved it by 10% of itself, and a held key took it
	// to hundreds of metres in a second - where it stayed, behind everything.
	inline float distStep(float x)
	{
		return x < 1.0f ? 0.05f : x < 3.0f ? 0.1f : x < 10.0f ? 0.25f : x < 30.0f ? 1.0f : x < 100.0f ? 5.0f : 10.0f;
	}

	float stepped(int p, float cur, int delta)
	{
		const Def& d = kDefs[p];
		float v;
		if (p == I_ENABLED) return cur > 0.5f ? 0.0f : 1.0f;
		if (p == I_DIST)
		{
			float x = clampf(cur, d.lo, d.hi);
			const int dir = delta > 0 ? 1 : -1;
			for (int k = 0, n = delta > 0 ? delta : -delta; k < n; ++k)
			{
				const float st = distStep(dir > 0 ? x : x - 0.001f);
				x = floorf((x + (float)dir * st) / st + 0.5f) * st;
				x = clampf(x, d.lo, d.hi);
			}
			return floorf(x * 100.0f + 0.5f) / 100.0f;
		}
		if (d.kind == K_ENUM)
		{
			// a choice cycles, one a press however long the key is held
			const int count = (int)(d.hi - d.lo + 1.5f);
			const int c = (int)floorf(cur - d.lo + 0.5f);
			return d.lo + (float)(((c + (delta > 0 ? 1 : -1)) % count + count) % count);
		}
		if (d.kind == K_BOOL) v = cur > 0.5f ? 0.0f : 1.0f;
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

	int s_keyFullLogs = 0;

	// The keyframes a change goes to: this one, or with ENB set to All
	// Keyframes every keyframe of the clip (4.18).
	int markerTimes(float* out, int cap)
	{
		void* st = game::replayStorage();
		const int n = st ? st::count(st) : 0;
		if (n <= 0 || n > 4096) return 0;
		int k = 0;
		for (int i = 0; i < n && k < cap; ++i)
		{
			const void* m = st::tryGet(st, i);
			if (!m) continue;
			EdDof d;
			readDof(m, d);
			if (d.have) out[k++] = d.t;
		}
		return k;
	}

	int targetsFor(float t, float* out, int cap)
	{
		if (g_cfg.enbAll)
		{
			const int n = markerTimes(out, cap);
			if (n > 0) return n;
		}
		out[0] = t;
		return 1;
	}

	// Left / right on an option row: this keyframe gets its own value - or,
	// with All Keyframes, every keyframe gets the same.
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
		float targets[256];
		const int nt = targetsFor(t, targets, 256);
		int misses = 0;
		auto put = [&](int q, float v) {
			for (int k = 0; k < nt; ++k)
				if (lights::setEnbParam(tr, targets[k], q, v, w.v)) ch = true; else ++misses;
		};
		switch (p)
		{
		case V_APERTURE:
		{
			const float pc = clampf(floorf(cur + 0.5f) + (float)delta, 0.0f, 100.0f);
			put(a ? I_AUTO_APERTURE : I_APERTURE, a ? autoFromPct(pc) : manualFromPct(pc));
			break;
		}
		case V_NEAR:
			put(a ? I_AUTO_NEARPOWER : I_NEARPOWER, powerFromPct(clampf(floorf(cur + 0.5f) + 5.0f * (float)delta, 1.0f, 100.0f)));
			break;
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
		case V_TARGET:
		{
			// Left / right: the next person on screen that way. The first press
			// takes the one followed now, so nothing jumps.
			const lights::Ctx c = lights::ctx();
			Person ppl[64];
			const int n = c.valid ? peopleOnScreen(c.cam, ppl, 64) : 0;
			if (n <= 0) break;
			int curI = -1;
			for (int i = 0; i < n; ++i) if (ppl[i].ped == s_plPed) { curI = i; break; }
			int nx;
			if (cur <= -1.0f && cur > -1.5f && curI >= 0) nx = curI;
			else if (curI < 0) nx = delta > 0 ? 0 : n - 1;
			else nx = ((curI + (delta > 0 ? 1 : -1)) % n + n) % n;
			const uint32_t m = lights::modelOf(ppl[nx].ped);
			if (!m) break;
			float hi, lo;
			modelTo(m, &hi, &lo);
			put(I_TGT_HI, hi); put(I_TGT_LO, lo);
			put(I_TGT_X, ppl[nx].chest[0]); put(I_TGT_Y, ppl[nx].chest[1]); put(I_TGT_Z, ppl[nx].chest[2]);
			s_tgtPed = ppl[nx].ped; s_tgtModel = m;
			memcpy(s_tgtPick, ppl[nx].chest, sizeof(s_tgtPick)); memcpy(s_tgtLast, ppl[nx].chest, sizeof(s_tgtLast));
			s_plPed = ppl[nx].ped;
			publishFollowed(ppl[nx].chest, ppl[nx].z);
			break;
		}
		case I_FOCUS:
		{
			const float nv = stepped(p, cur, delta);
			put(I_FOCUS, nv);
			// Into Manual: start the distance where the focus was just now -
			// the player, or the middle of the picture - not somewhere unknown.
			if (focusModeOf(nv) == FM_MANUAL)
			{
				const float m = focusModeOf(cur) == FM_PLAYER && playerFresh() ? s_playerMetres : centreFresh() ? centreMetres() : 0.0f;
				if (m > 0.0f) put(I_DIST, clampf(floorf(m * 100.0f + 0.5f) / 100.0f, kDefs[I_DIST].lo, kDefs[I_DIST].hi));
			}
			break;
		}
		default:
			put(p, stepped(p, cur, delta));
			break;
		}
		if (misses && s_keyFullLogs++ < 3) logf("enb: %d keyframe(s) could not take the change - a clip holds ENB values on at most %d keyframes", misses, lights::kMaxEnbKeys);
		if (ch) lights::markDirty();
	}

	// Enter on an option row: back to the keyframes around it.
	void clearHere(int p)
	{
		float t;
		if (!markerTimeNow(&t)) return;
		lights::Lock lk;
		if (!lights::g_cur) return;
		float targets[256];
		const int nt = targetsFor(t, targets, 256);
		auto clr = [&](int q) { for (int k = 0; k < nt; ++k) lights::clearEnbParam(lights::g_cur->enb, targets[k], q); };
		if (p == I_DIST && centreFresh())
		{
			// Enter on Focus Distance: focus on what is in the middle of the picture
			Want w;
			computeAt(t, w);
			const float m = clampf(floorf(centreMetres() * 100.0f + 0.5f) / 100.0f, kDefs[I_DIST].lo, kDefs[I_DIST].hi);
			for (int k = 0; k < nt; ++k) lights::setEnbParam(lights::g_cur->enb, targets[k], I_DIST, m, w.v);
			lights::markDirty();
			return;
		}
		switch (p)
		{
		case V_APERTURE:   clr(I_APERTURE); clr(I_AUTO_APERTURE); break;
		case V_NEAR:       clr(I_NEARPOWER); clr(I_AUTO_NEARPOWER); break;
		case V_ANAMORPHIC: clr(17); clr(18); break;
		case V_TARGET:     clr(I_TGT_HI); clr(I_TGT_LO); clr(I_TGT_X); clr(I_TGT_Y); clr(I_TGT_Z); break;
		default:           if (p >= 0 && p < N) clr(p); else return; break;
		}
		lights::markDirty();
	}

	// ---- Copy DOF / Paste DOF / DOF To All, for the ENB rows (4.16) -------------
	// The three rows under the game's own DOF rows copied only the game's
	// fields. They carry the keyframe's ENB values too: exactly what it set
	// itself, so a pasted keyframe is set the same and falls back to the same
	// defaults for the rest - it looks the same.
	struct EnbClip { bool used; uint32_t own; float v[N]; };
	EnbClip g_enbClip = {};

	bool copyEnbHere()
	{
		float t;
		if (!s_ready || !markerTimeNow(&t)) return false;
		lights::Lock lk;
		if (!lights::g_cur) return false;
		g_enbClip.own = 0;
		for (int p = 0; p < N; ++p)
		{
			float x;
			if (lights::enbKeyValue(lights::g_cur->enb, t, p, &x)) { g_enbClip.own |= 1u << p; g_enbClip.v[p] = x; }
		}
		g_enbClip.used = true;
		logf("enb: copied keyframe at %.0f ms - %s", t, g_enbClip.own ? "its own ENB values" : "no ENB values of its own (the defaults)");
		return true;
	}

	// The copied ENB values onto the keyframe at `t` (the lock is held).
	bool putEnbAt(float t)
	{
		lights::EnbTrack& tr = lights::g_cur->enb;
		Want w;
		computeAt(t, w);
		bool ok = true;
		for (int p = 0; p < N; ++p)
		{
			if (g_enbClip.own & (1u << p)) { if (!lights::setEnbParam(tr, t, p, g_enbClip.v[p], w.v)) ok = false; }
			else lights::clearEnbParam(tr, t, p);
		}
		return ok;
	}

	bool pasteEnbHere()
	{
		float t;
		if (!s_ready || !g_enbClip.used || !markerTimeNow(&t)) return false;
		lights::Lock lk;
		if (!lights::g_cur) return false;
		const bool ok = putEnbAt(t);
		lights::markDirty();
		logf("enb: pasted onto the keyframe at %.0f ms%s", t, ok ? "" : " - some values did not fit (too many keyframes with values of their own)");
		return ok;
	}

	// This keyframe's ENB values onto every other keyframe of the clip.
	int enbToAll()
	{
		float t;
		if (!s_ready || !copyEnbHere() || !markerTimeNow(&t)) return -1;
		void* st = game::replayStorage();
		const int n = st ? st::count(st) : 0;
		if (n <= 0 || n > 4096) return -1;
		lights::Lock lk;
		if (!lights::g_cur) return -1;
		int done = 0, full = 0;
		for (int i = 0; i < n; ++i)
		{
			const void* m = st::tryGet(st, i);
			if (!m) continue;
			EdDof d;
			readDof(m, d);
			if (!d.have || fabsf(d.t - t) <= 5.0f) continue;
			if (putEnbAt(d.t)) ++done; else ++full;
		}
		lights::markDirty();
		logf("enb: this keyframe's ENB values put on %d other keyframe(s)%s", done, full ? " - some did not fit" : "");
		return done;
	}

	const char* format(int p, float v, char* buf, int cap)
	{
		if (p == V_APERTURE) { snprintf(buf, cap, "%.0f%%", v); return buf; }
		if (p == V_NEAR) { snprintf(buf, cap, "%.0f%%", v); return buf; }
		if (p == V_ANAMORPHIC) { if (v <= 0.0f) return "Off"; snprintf(buf, cap, "%.2fx", v); return buf; }
		if (p == V_TARGET)
		{
			if (v > -1.5f && v < -0.5f) return "Auto";
			if (v < -1.5f) return "Picked (not in view)";
			snprintf(buf, cap, "Person %d of %d", (int)v + 1, s_peopleCount);
			return buf;
		}
		if (p == I_ENABLED) return v > 0.5f ? "On" : "Off";
		if (p < 0 || p >= N) return "";
		const Def& d = kDefs[p];
		if (p == I_FOCUS)
		{
			const int fm = focusModeOf(v);
			if (fm == FM_MANUAL) return "Manual";
			if (fm == FM_PLAYER)
			{
				if (!playerFresh()) return "Player";
				const float m = s_playerMetres;
				snprintf(buf, cap, m < 10.0f ? "Player (%.1f m)" : "Player (%.0f m)", m);
				return buf;
			}
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

	// One short line on what an option does, for the help line under the column.
	const char* hint(int p)
	{
		switch (p)
		{
		case I_ENABLED:         return "Blur on this keyframe. Off = no blur here.";
		case I_FOCUS:           return "Auto: the middle of the screen. Manual: a distance. Player: a person.";
		case V_TARGET:          return "Who to focus on. Left/right: the next person. The marker shows who.";
		case I_DIST:            return centreFresh() ? "Focus distance in metres. Enter: focus on the middle." : "Focus distance in metres.";
		case V_APERTURE:        return "How much blur. 0% none, 100% the most.";
		case V_NEAR:            return "How much blur in front of the focus.";
		case V_ANAMORPHIC:      return "Oval bokeh. Off, or how stretched.";
		case I_APERTURE:
		case I_AUTO_APERTURE:   return "How much blur.";
		case I_NEAR:            return "Blur things in front of the focus too.";
		case I_NEARPOWER:
		case I_AUTO_NEARPOWER:  return "Higher = less blur in front of the focus.";
		case 7:  return "How fast the auto-focus follows.";
		case 8:  return "Makes the auto-focus reach further.";
		case 9:  return "Where the auto-focus looks, left to right.";
		case 10: return "Where the auto-focus looks, top to bottom.";
		case 11: return "Colour fringes in the blur.";
		case 12: return "How soft the blur is at 100%.";
		case 13: return "More steps, smoother blur, slower.";
		case 14: return "How big the blur gets at 100%.";
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

	// Per Keyframe or All Keyframes: where a change on an ENB row goes.
	void setAll(bool all)
	{
		if (g_cfg.enbAll == all) return;
		g_cfg.enbAll = all;
		iniWriteInt("EnbDofAllKeyframes", all ? 1 : 0);
		logf("enb: ENB row changes now go to %s", all ? "every keyframe of the clip" : "the keyframe they are made on");
	}
}
