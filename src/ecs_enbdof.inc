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
//       of the playhead: a Custom keyframe with manual focus sets the focus
//       distance, its intensity (1..10) times a scale sets the aperture, and
//       a keyframe set to None means no blur at all.
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
	enum Kind : uint8_t { K_FLOAT = 0, K_INT = 1, K_BOOL = 2 };
	struct Def { const char* ui; Kind kind; float lo, hi; const char* label; };
	constexpr int N = lights::kEnbParams;
	const Def kDefs[N] = {
		{ "--> Click here to enable Mouse Focus",       K_BOOL,   0.0f,    1.0f, "Mouse focus" },
		{ "--> Blur the near field toggle",             K_BOOL,   0.0f,    1.0f, "Blur the near field" },
		{ "--> Near field intensity",                   K_FLOAT,  0.0f,   20.0f, "Near field intensity" },
		{ "--> Manual Focus : Distance",                K_FLOAT,  0.1f, 1000.0f, "Focus distance (m)" },
		{ "--> Manual Focus : Aperture",                K_FLOAT,  0.0f,   10.0f, "Aperture" },
		{ "--> Manual Focus : Near field power",        K_FLOAT,  0.0f,   10.0f, "Near field power" },
		{ "--> Auto-focus : Aperture",                  K_FLOAT,  0.0f,   10.0f, "Auto-focus aperture" },
		{ "--> Auto-focus : Speed",                     K_FLOAT,  0.0f,   20.0f, "Auto-focus speed" },
		{ "--> Auto-focus : Range boost",               K_FLOAT,  0.0f,   10.0f, "Auto-focus range boost" },
		{ "--> Auto-focus : Point of interestX",        K_FLOAT,  0.0f,    1.0f, "Auto-focus point X" },
		{ "--> Auto-focus : Point of interestY",        K_FLOAT,  0.0f,    1.0f, "Auto-focus point Y" },
		{ "--> Blur : Chromatic Spread",                K_FLOAT,  0.0f,   10.0f, "Chromatic spread" },
		{ "--> Blur : Size",                            K_FLOAT,  0.0f,   10.0f, "Blur size" },
		{ "--> Blur : Quality Steps",                   K_INT,    1.0f,   32.0f, "Blur quality steps" },
		{ "--> DOF : Maximum size",                     K_FLOAT,  0.0f,  100.0f, "Maximum size" },
		{ "--> DOF : Quality",                          K_FLOAT,  0.0f,    4.0f, "Quality" },
		{ "--> DOF : Performance scaler",               K_FLOAT,  0.0f,    4.0f, "Performance scaler" },
		{ "--> DOF : Anamorphic enable",                K_BOOL,   0.0f,    1.0f, "Anamorphic" },
		{ "--> DOF : Anamorphic stretch",               K_FLOAT,  0.0f,    5.0f, "Anamorphic stretch" },
		{ "--> DOF : Distribution",                     K_FLOAT,  0.0f,    5.0f, "Distribution" },
		{ "--> Lens Distortion : Distortion Amount",    K_FLOAT, -1.0f,    1.0f, "Lens distortion" },
		{ "--> Lens Distortion : Always zoomed",        K_BOOL,   0.0f,    1.0f, "Distortion: always zoomed" },
		{ "--> Lens Distortion : Zoom Amount",          K_FLOAT,  0.0f,    1.0f, "Distortion: zoom" },
	};
	enum { I_NEAR = 1, I_DIST = 3, I_APERTURE = 4, I_AUTO_APERTURE = 6 };

	uint8_t kinds[N] = {};                // for lights::evalEnb, filled on start

	// ---- state ----------------------------------------------------------------
	const char* s_file = nullptr;         // the filename / category pair ENB answers to
	const char* s_cat  = nullptr;
	bool  s_found = false;                // the SDK is exported
	bool  s_ready = false;                // ENB answered for the DOF shader
	bool  s_known[N]  = {};
	float s_base[N]   = {};               // ENB's own values, which are put back
	float s_manual[N] = {};               // this session's values, edited in the window
	float s_pushed[N] = {};
	bool  s_havePushed[N] = {};
	bool  s_dirty = false;                // something of ours is in ENB now
	volatile LONGLONG s_lastCb = 0;       // when ENB last called us
	int   s_cbLogs = 0, s_setFails = 0;
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
		case K_INT:   { p.type = P_INT; const int n = (int)floorf(v + 0.5f); memcpy(p.data, &n, 4); break; }
		case K_BOOL:  { p.type = P_BOOL; const BOOL b = v > 0.5f ? TRUE : FALSE; memcpy(p.data, &b, 4); break; }
		}
		return setRaw(kDefs[i].ui, &p);
	}

	bool same(int i, float a, float b)
	{
		if (kDefs[i].kind == K_FLOAT) return fabsf(a - b) <= 1e-4f * (1.0f + fabsf(a));
		return (int)floorf(a + 0.5f) == (int)floorf(b + 0.5f);
	}

	// ---- finding ENB and its DOF shader ----------------------------------------
	bool resolve()
	{
		if (s_found) return true;
		static const char* const kMods[] = { "d3d11.dll", "dxgi.dll", "d3d9.dll" };
		for (const char* name : kMods)
		{
			HMODULE h = GetModuleHandleA(name);
			if (!h) continue;
			FnSet set = (FnSet)GetProcAddress(h, "ENBSetParameter");
			FnGet get = (FnGet)GetProcAddress(h, "ENBGetParameter");
			if (!set || !get) continue;
			s_set = set; s_get = get;
			s_ver = (FnVer)GetProcAddress(h, "ENBGetSDKVersion");
			s_setCb = (FnSetCb)GetProcAddress(h, "ENBSetCallbackFunction");
			__try { s_sdk = s_ver ? s_ver() : 0; }
			__except (EXCEPTION_EXECUTE_HANDLER) { s_sdk = 0; }
			s_found = true;
			logf("enb: ENBSeries found in %s (SDK %ld) - the ENB option can drive its depth of field", name, s_sdk);
			return true;
		}
		return false;
	}

	void WINAPI onEnb(int type);

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
				s_known[i] = readValue(s_file, s_cat, i, &s_base[i]);
				if (s_known[i]) ++known;
				s_manual[i] = s_base[i];
				s_havePushed[i] = false;
			}
			s_ready = true;
			logf("enb: enbdepthoffield.fx answers as (%s, %s): %d of %d options known, focus distance %.2f m, aperture %.2f",
				s_file ? s_file : "null", s_cat ? s_cat : "null", known, N, s_base[I_DIST], s_base[I_APERTURE]);
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
			if (s_known[i]) readValue(s_file, s_cat, i, &s_base[i]);
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
	void applyEditor(const EdDof& a, const EdDof& b, float t, float scale, float* v, char* line, int cap)
	{
		if (a.mode == dof::MODE_NONE)
		{
			v[I_APERTURE] = 0.0f;
			v[I_AUTO_APERTURE] = 0.0f;
			if (line) snprintf(line, cap, "keyframe DOF None - no blur");
			return;
		}
		if (a.mode != dof::MODE_CUSTOM)
		{
			if (line) snprintf(line, cap, "keyframe DOF Default - ENB's own values");
			return;
		}
		float u = 0.0f;
		const bool blend = b.have && b.mode == dof::MODE_CUSTOM && b.t > a.t;
		if (blend) u = (t - a.t) / (b.t - a.t);
		if (!(u >= 0.0f)) u = 0.0f;
		if (u > 1.0f) u = 1.0f;
		const float inten = a.intensity + ((blend ? b.intensity : a.intensity) - a.intensity) * u;
		v[I_APERTURE] = inten * scale;
		v[I_AUTO_APERTURE] = inten * scale;
		if (a.focus == dof::FOCUS_MANUAL)
		{
			const bool blendDist = blend && b.focus == dof::FOCUS_MANUAL;
			const float d = a.dist + ((blendDist ? b.dist : a.dist) - a.dist) * u;
			if (d > 0.0f) v[I_DIST] = d;
			if (line) snprintf(line, cap, "keyframe DOF Custom, manual focus %.2f m, intensity %.1f -> aperture %.2f", v[I_DIST], inten, v[I_APERTURE]);
		}
		else if (line)
			snprintf(line, cap, "keyframe DOF Custom, %s focus, intensity %.1f -> aperture %.2f (focus left to ENB)",
				dof::focusName(a.focus), inten, v[I_APERTURE]);
	}

	// ---- one frame -------------------------------------------------------------
	// What ENB should hold, computed on the game thread and published; pushed
	// from ENB's thread. A snapshot older than a second is not trusted - an
	// export does not tick the replay director - and the push computes afresh.
	struct Want { float v[N]; ULONGLONG at; bool on; };
	Want s_want = {};
	volatile LONG s_wantSeq = 0;

	void compute(Want& w)
	{
		w.at = GetTickCount64();
		w.on = g_cfg.enbDof;
		memcpy(w.v, s_manual, sizeof(w.v));
		if (!w.on) return;
		const float t = game::replayTimeMs();
		{
			lights::Lock lk;
			if (lights::g_cur && lights::g_cur->enb.n > 0) lights::evalEnb(lights::g_cur->enb, t, w.v, kinds);
		}
		if (g_cfg.enbFollow)
		{
			EdDof a, b;
			if (dofAround(t, a, b)) applyEditor(a, b, t, g_cfg.enbScale, w.v, s_editorLine, sizeof(s_editorLine));
			else snprintf(s_editorLine, sizeof(s_editorLine), "no keyframes in this clip yet");
		}
	}

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
		for (int i = 0; i < N; ++i)
		{
			if (!s_known[i]) continue;
			if (s_havePushed[i] && same(i, s_pushed[i], w.v[i])) continue;
			if (writeValue(i, w.v[i])) { s_pushed[i] = w.v[i]; s_havePushed[i] = true; s_dirty = true; }
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

	// From the worker, about once a second.
	void poll(ULONGLONG now)
	{
		if (now < s_nextPoll) return;
		s_nextPoll = now + 1000;
		for (int i = 0; i < N; ++i) kinds[i] = (uint8_t)kDefs[i].kind;
		if (!resolve()) return;
		if (!s_ready) { calibrate(); return; }
		if (!g_cfg.enbDof) rereadBase();
	}

	void setOn(bool on)
	{
		g_cfg.enbDof = on;
		iniWriteInt("EnbDof", on ? 1 : 0);
		logf("enb: the ENB option is %s", on ? "on - the Rockstar Editor's depth of field is drawn by ENB" : "off - ENB's own values are back");
	}
}
