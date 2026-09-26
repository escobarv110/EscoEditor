// =========================================================================
//  ecs_reshade.inc - EscoEditor as a ReShade add-on: ReShade's Dear ImGui for
//  the light editor, an "EscoEditor" tab in ReShade's menu, and EscoFlare.fx
//  drawn before the game's own menus
// =========================================================================
//  ReShade's build with add-on support (the one that loads .addon64 files;
//  FiveM runs it) exports a small C API that any DLL in the process may use.
//  EscoEditor.asi registers itself as an add-on while the plugin loads
//  (DllMain): ReShade is in the process before any .asi, and DllMain is
//  where ReShade documents registering - its event lists are plain vectors,
//  so registering later from another thread could race the renderer. If
//  ReShade only arrives later, the worker registers then.
//
//  THE FLARE PASS. A lens flare and a light leak are lens things: the engine
//  has no per-light control for either, so they are drawn as one small
//  ReShade effect (EscoFlare.fx, copied from beside the .asi into ReShade's
//  shader folder). The plugin owns it: the technique stays switched off in
//  ReShade's own list and EscoEditor renders it itself with
//  effect_runtime::render_technique - which works on a switched-off
//  technique, saves and restores the game's pipeline state around itself,
//  and fires begin/finish_effects so ReShade's depth add-on binds the depth
//  buffer for it.
//
//  WHERE THE MENUS START. It is drawn right before the game's UI, so the
//  editor's menus and timeline stay sharp (ReShade's own pass runs at
//  Present, after the UI - that is what blurred them in 2.7). Watching the
//  game's immediate context: the first draw into the swap chain's back buffer
//  each frame brings in the finished picture, and the next draw into the back
//  buffer is the first piece of UI. The flare goes in just before that draw,
//  once per frame. A frame in which that never happens (no UI on screen) gets
//  the flare from ReShade at Present instead, like any other effect.
//
//  Every value comes from the light editor: where each light of the clip is
//  on screen (projected here, with this runtime's own aspect ratio), how far
//  away it is, its colour, and that light's flare numbers. Nothing here hooks
//  the game.
// =========================================================================
namespace rs
{
	namespace ra = reshade::api;

	constexpr const char* kEffect    = "EscoFlare.fx";
	constexpr const char* kTechnique = "EscoFlare";

	enum Var { V_COUNT_, V_A, V_B, V_C, V_D, V_E, V_F, V_G, V_NEARCLIP, V_FARCLIP, V_TIME, V_VARS };
	const char* const kVarNames[V_VARS] = {
		"EE_FlareCount", "EE_FlareA", "EE_FlareB", "EE_FlareC", "EE_FlareD", "EE_FlareE", "EE_FlareF", "EE_FlareG",
		"EE_NearClip", "EE_FarClip", "EE_Time" };

	// --- one frame of back-buffer traffic (pure, so the self-test can run it) --
	enum { K_DRAW = 0, K_INDEXED, K_INDIRECT, K_COPY };
	const char* kindName(int k) { return k == K_DRAW ? "a draw" : k == K_INDEXED ? "an indexed draw" : k == K_INDIRECT ? "an indirect draw" : "a copy"; }

	struct Frame
	{
		bool     sceneIn, uiSeen, drawn;
		int      bbBinds, bbDraws, sceneKind, uiKind;
		uint32_t sceneCount, uiCount;
	};

	// A draw into the back buffer. True: draw the flare now, right before it.
	bool frameDraw(Frame& f, int kind, uint32_t count)
	{
		++f.bbDraws;
		if (f.drawn || f.uiSeen) return false;
		if (!f.sceneIn) { f.sceneIn = true; f.sceneKind = kind; f.sceneCount = count; return false; }
		f.uiSeen = true; f.uiKind = kind; f.uiCount = count;
		return true;
	}

	// The finished picture copied (or resolved) into the back buffer.
	void frameCopy(Frame& f)
	{
		if (!f.sceneIn && !f.uiSeen) { f.sceneIn = true; f.sceneKind = K_COPY; f.sceneCount = 0; }
	}

	// --- one entry per ReShade effect runtime (one per swap chain) -------------
	struct Entry
	{
		ra::effect_runtime*         rt = nullptr;
		ra::command_list*           immediate = nullptr;
		ra::resource                bb = {};
		uint32_t                    w = 0, h = 0;
		uint64_t                    area = 0;
		bool                        found = false, fallbackOn = false;
		int                         findLogs = 0;
		ULONGLONG                   nextFind = 0;
		ra::effect_technique        tech = {};
		ra::effect_uniform_variable var[V_VARS] = {};
		Frame                       fr = {};
		// EscoFocus: the depth in the middle of the picture, read back for the ENB option
		ra::effect_technique        focusTech = {};
		ra::effect_texture_variable focusTex = {};
		ra::resource                staging[3] = {};
		uint32_t                    focusFrame = 0;
		bool                        focusLooked = false, focusOn = false;
		ULONGLONG                   loadedAt = 0;      // ReShade said its effects are loaded (reshade_reloaded_effects)
		ULONGLONG                   foundAt = 0;       // EscoFlare.fx was first seen
	};
	constexpr int kMaxEntries = 4;
	Entry g_entries[kMaxEntries];

	HMODULE g_module = nullptr;
	char    g_reshadeDir[MAX_PATH] = {};
	char    g_shaderFile[MAX_PATH] = {};
	bool    g_registered = false, g_gaveUp = false, g_shaderInstalled = false;
	bool    g_armed  = false;   // a flare is wanted this frame: watch the game's draws
	bool    g_inCall = false;   // inside our own render_technique (it fires begin/finish_effects)
	Entry*  g_bound  = nullptr; // the entry whose back buffer the game has bound right now
	int     g_outcomeSeen = -1, g_outcomeHeld = 0;
	int     g_tries = 0, g_outcome = -1, g_outcomeLogs = 0, g_frameLogs = 0, g_viewFails = 0, g_armedFrames = 0;
	bool    g_noEventsLogged = false, g_sawBind = false;

	Entry* entryFor(ra::effect_runtime* rt, bool create)
	{
		for (Entry& e : g_entries) if (e.rt == rt) return &e;
		if (!create) return nullptr;
		for (Entry& e : g_entries) if (!e.rt) { e = Entry(); e.rt = rt; return &e; }
		return nullptr;
	}

	// The game's window: the biggest back buffer among the runtimes that have
	// EscoFlare loaded (FiveM also opens small helper swap chains).
	Entry* mainEntry()
	{
		Entry* best = nullptr;
		for (Entry& e : g_entries) if (e.rt && e.found && (!best || e.area > best->area)) best = &e;
		return best;
	}

	// --- the shader file -----------------------------------------------------
	// ReShade's first effect search path, made absolute against its folder.
	// Pure so the self-test can run it.
	void resolveShaderDir(const char* searchPaths, const char* base, char* out, size_t cap)
	{
		char first[MAX_PATH] = {};
		const char* p = searchPaths ? searchPaths : "";
		while (*p == ' ') ++p;
		size_t n = 0;
		while (p[n] && p[n] != ',' && n < sizeof(first) - 1) { first[n] = p[n]; ++n; }
		first[n] = '\0';
		// "..\Shaders\**" means recursive to ReShade; the folder itself is what we want
		while (n > 0 && (first[n - 1] == ' ' || first[n - 1] == '\\' || first[n - 1] == '/' || first[n - 1] == '*')) first[--n] = '\0';
		if (!first[0]) strcpy_s(first, ".\\reshade-shaders\\Shaders");
		const bool absolute = (first[0] && first[1] == ':') || (first[0] == '\\' && first[1] == '\\');
		if (absolute) snprintf(out, cap, "%s\\", first);
		else
		{
			const char* rel = first;
			if (rel[0] == '.' && (rel[1] == '\\' || rel[1] == '/')) rel += 2;
			snprintf(out, cap, "%s%s\\", base, rel);
		}
		for (char* c = out; *c; ++c) if (*c == '/') *c = '\\';
	}

	bool readAll(const char* path, char** data, size_t* size)
	{
		*data = nullptr; *size = 0;
		FILE* f = nullptr;
		if (fopen_s(&f, path, "rb") != 0 || !f) return false;
		fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
		if (n < 0 || n > 4 * 1024 * 1024) { fclose(f); return false; }
		char* d = (char*)malloc((size_t)n + 1);
		if (!d) { fclose(f); return false; }
		const size_t got = fread(d, 1, (size_t)n, f);
		fclose(f);
		d[got] = '\0';
		*data = d; *size = got;
		return true;
	}

	bool sameFile(const char* a, const char* b)
	{
		char* da; size_t na; char* db; size_t nb;
		if (!readAll(a, &da, &na)) return false;
		if (!readAll(b, &db, &nb)) { free(da); return false; }
		const bool same = na == nb && memcmp(da, db, na) == 0;
		free(da); free(db);
		return same;
	}

	void ensureDir(const char* dir)
	{
		char path[MAX_PATH];
		strcpy_s(path, dir);
		for (char* c = path + 3; *c; ++c)
		{
			if (*c == '\\') { *c = '\0'; CreateDirectoryA(path, nullptr); *c = '\\'; }
		}
		CreateDirectoryA(path, nullptr);
	}

	// The EscoFlare.fx built into the plugin (escoflare.rc).
	bool builtinShader(const char** data, size_t* size)
	{
		HRSRC r = FindResourceA(g_self, "ESCOFLARE", MAKEINTRESOURCEA(10));   // RT_RCDATA
		if (!r) return false;
		HGLOBAL h = LoadResource(g_self, r);
		const void* p = h ? LockResource(h) : nullptr;
		const DWORD n = SizeofResource(g_self, r);
		if (!p || !n) return false;
		*data = (const char*)p;
		*size = n;
		return true;
	}

	// Is `path` exactly these bytes?
	bool fileIs(const char* path, const char* data, size_t size)
	{
		char* d; size_t n;
		if (!readAll(path, &d, &n)) return false;
		const bool same = n == size && memcmp(d, data, n) == 0;
		free(d);
		return same;
	}

	bool writeAll(const char* path, const char* data, size_t size)
	{
		FILE* f = nullptr;
		if (fopen_s(&f, path, "wb") != 0 || !f) return false;
		const size_t put = fwrite(data, 1, size, f);
		fclose(f);
		return put == size;
	}

	// EscoFlare.fx into ReShade's shader folder. Since 4.17 the plugin carries
	// its own copy and writes it whenever the one there differs: 4.14 added a
	// pass to it, but only the copy next to the .asi was ever compared, so
	// updating just the .asi left an old EscoFlare.fx in place and ReShade
	// never had the pass (Auto focus measured nothing). The copy next to the
	// .asi is refreshed too, so it does not mislead.
	bool installShader()
	{
		char ini[MAX_PATH];
		snprintf(ini, sizeof(ini), "%sReShade.ini", g_reshadeDir);
		static char paths[4096];
		paths[0] = '\0';
		GetPrivateProfileStringA("GENERAL", "EffectSearchPaths", "", paths, sizeof(paths), ini);
		char dir[MAX_PATH];
		resolveShaderDir(paths, g_reshadeDir, dir, sizeof(dir));
		snprintf(g_shaderFile, sizeof(g_shaderFile), "%s%s", dir, kEffect);

		const char* data = nullptr;
		size_t size = 0;
		if (!builtinShader(&data, &size))
		{
			logf("reshade: this build carries no EscoFlare.fx of its own - no lens flares or Auto focus");
			return false;
		}
		char beside[MAX_PATH];
		snprintf(beside, sizeof(beside), "%s%s", g_dir, kEffect);
		if (GetFileAttributesA(beside) != INVALID_FILE_ATTRIBUTES && !fileIs(beside, data, size)) writeAll(beside, data, size);

		const bool haveDst = GetFileAttributesA(g_shaderFile) != INVALID_FILE_ATTRIBUTES;
		if (haveDst && fileIs(g_shaderFile, data, size)) { logf("reshade: %s is up to date in %s", kEffect, dir); return true; }
		ensureDir(dir);
		if (!writeAll(g_shaderFile, data, size))
		{
			logf("reshade: could not write %s to %s (error %lu) - no lens flares or Auto focus", kEffect, dir, GetLastError());
			return false;
		}
		logf("reshade: %s %s in %s (the plugin's own copy)", kEffect, haveDst ? "updated" : "installed", dir);
		return true;
	}

	// --- values --------------------------------------------------------------
	bool findAll(Entry& e)
	{
		e.tech = e.rt->find_technique(kEffect, kTechnique);
		if (e.tech.handle == 0) return false;
		for (int i = 0; i < V_VARS; ++i)
		{
			e.var[i] = e.rt->find_uniform_variable(kEffect, kVarNames[i]);
			if (e.var[i].handle == 0)
			{
				if (e.findLogs++ < 2) logf("reshade: EscoFlare.fx has no '%s' - an older EscoFlare.fx is loaded? Press Reload in ReShade.", kVarNames[i]);
				return false;
			}
		}
		return true;
	}

	inline void setf(Entry& e, int v, float x) { e.rt->set_uniform_value_float(e.var[v], &x, 1); }
	inline void seti(Entry& e, int v, int x)   { const int32_t y = x; e.rt->set_uniform_value_int(e.var[v], &y, 1); }

	// The clip's lights as this runtime sees them. Returns how many flares are
	// on screen: 0 means there is nothing to draw this frame.
	int pushValues(Entry& e, const lights::FlareSet& fs)
	{
		constexpr int kN = lights::kMaxFlares;
		constexpr float kRad = 3.14159265f / 180.0f;
		float A[4 * kN] = {}, B[4 * kN] = {}, C[4 * kN] = {}, D[4 * kN] = {};
		float E[4 * kN] = {}, F[4 * kN] = {}, G[4 * kN] = {};
		const float aspect = (e.w > 0 && e.h > 0) ? (float)e.w / (float)e.h : 16.0f / 9.0f;
		int n = 0;
		for (int i = 0; i < fs.count && n < kN; ++i)
		{
			const lights::FlareSrc& s = fs.it[i];
			float u, v;
			if (!lights::flareUv(s.cs, fs.fov, aspect, &u, &v)) continue;
			if (u < -0.6f || u > 1.6f || v < -0.6f || v > 1.6f) continue;   // too far outside the frame to matter
			A[n * 4 + 0] = u;          A[n * 4 + 1] = v;           A[n * 4 + 2] = s.dist;     A[n * 4 + 3] = s.bright;
			B[n * 4 + 0] = s.col[0];   B[n * 4 + 1] = s.col[1];    B[n * 4 + 2] = s.col[2];   B[n * 4 + 3] = s.size;
			C[n * 4 + 0] = s.streak;   C[n * 4 + 1] = s.star;      C[n * 4 + 2] = s.ghost;    C[n * 4 + 3] = s.halo;
			D[n * 4 + 0] = s.chroma;   D[n * 4 + 1] = s.leak;      D[n * 4 + 2] = s.angle * kRad; D[n * 4 + 3] = s.cross;
			E[n * 4 + 0] = s.spokes;   E[n * 4 + 1] = s.gcount;    E[n * 4 + 2] = s.gspread;  E[n * 4 + 3] = s.halosize;
			F[n * 4 + 0] = s.squeeze;  F[n * 4 + 1] = s.flicker;   F[n * 4 + 2] = s.flickerHz; F[n * 4 + 3] = s.tintMix;
			G[n * 4 + 0] = s.tint[0];  G[n * 4 + 1] = s.tint[1];   G[n * 4 + 2] = s.tint[2];  G[n * 4 + 3] = s.phase;
			++n;
		}
		seti(e, V_COUNT_, n);
		e.rt->set_uniform_value_float(e.var[V_A], A, 4 * kN);
		e.rt->set_uniform_value_float(e.var[V_B], B, 4 * kN);
		e.rt->set_uniform_value_float(e.var[V_C], C, 4 * kN);
		e.rt->set_uniform_value_float(e.var[V_D], D, 4 * kN);
		e.rt->set_uniform_value_float(e.var[V_E], E, 4 * kN);
		e.rt->set_uniform_value_float(e.var[V_F], F, 4 * kN);
		e.rt->set_uniform_value_float(e.var[V_G], G, 4 * kN);
		setf(e, V_NEARCLIP, fs.nearClip > 0.001f && fs.nearClip < 5.0f ? fs.nearClip : 0.1f);
		setf(e, V_FARCLIP,  fs.farClip > 50.0f && fs.farClip < 100000.0f ? fs.farClip : 1000.0f);
		// the clip's clock, wrapped to an hour so a float keeps its precision
		setf(e, V_TIME, fs.time - 3600.0f * floorf(fs.time / 3600.0f));
		return n;
	}

	// Does the light editor want flares on screen right now?
	bool wantNow(lights::FlareSet& F)
	{
		F = lights::flares();
		const ULONGLONG now = GetTickCount64();
		return editorOpen() && !game::exporting() && F.count > 0 && F.at != 0 && (now - F.at) < 5000;
	}

	// --- drawing it before the menus ------------------------------------------
	void drawBeforeUi(Entry& e, ra::command_list* cmd)
	{
		lights::FlareSet F;
		if (!wantNow(F) || !e.rt->get_effects_state()) return;
		if (pushValues(e, F) <= 0) return;
		ra::device* dev = cmd->get_device();
		const ra::resource_desc d = dev->get_resource_desc(e.bb);
		if (d.texture.samples > 1) return;
		ra::resource_view v = {}, vs = {};
		if (!dev->create_resource_view(e.bb, ra::resource_usage::render_target, ra::resource_view_desc(ra::format_to_default_typed(d.texture.format, 0)), &v))
		{
			if (g_viewFails++ < 2) logf("reshade: could not make a render target view of the back buffer (format %u) - the flare stays at the end of the frame", (unsigned)d.texture.format);
			return;
		}
		if (!dev->create_resource_view(e.bb, ra::resource_usage::render_target, ra::resource_view_desc(ra::format_to_default_typed(d.texture.format, 1)), &vs))
			vs = {};
		g_inCall = true;
		e.rt->render_technique(e.tech, cmd, v, vs.handle ? vs : v);
		g_inCall = false;
		if (vs.handle) dev->destroy_resource_view(vs);
		dev->destroy_resource_view(v);
		e.fr.drawn = true;
	}

	void onBindTargets(ra::command_list* cmd, uint32_t count, const ra::resource_view* rtvs, ra::resource_view)
	{
		if (!g_armed || g_inCall) return;
		Entry* m = mainEntry();
		if (!m || cmd != m->immediate) return;
		Entry* hit = nullptr;
		if (count >= 1 && rtvs && rtvs[0].handle && cmd->get_device()->get_resource_from_view(rtvs[0]) == m->bb) hit = m;
		g_bound = hit;
		if (hit) { ++m->fr.bbBinds; g_sawBind = true; }
	}

	void atDraw(ra::command_list* cmd, int kind, uint32_t count)
	{
		if (!g_armed || g_inCall || !g_bound || cmd != g_bound->immediate) return;
		if (frameDraw(g_bound->fr, kind, count)) drawBeforeUi(*g_bound, cmd);
	}

	bool onDraw(ra::command_list* cmd, uint32_t vertices, uint32_t, uint32_t, uint32_t) { atDraw(cmd, K_DRAW, vertices); return false; }
	bool onDrawIndexed(ra::command_list* cmd, uint32_t indices, uint32_t, uint32_t, int32_t, uint32_t) { atDraw(cmd, K_INDEXED, indices); return false; }
	bool onDrawIndirect(ra::command_list* cmd, ra::indirect_command type, ra::resource, uint64_t, uint32_t, uint32_t)
	{
		if (type == ra::indirect_command::draw || type == ra::indirect_command::draw_indexed) atDraw(cmd, K_INDIRECT, 0);
		return false;
	}

	void atCopy(ra::command_list* cmd, ra::resource dst)
	{
		if (!g_armed || g_inCall) return;
		Entry* m = mainEntry();
		if (!m || cmd != m->immediate || dst != m->bb) return;
		frameCopy(m->fr);
	}

	bool onCopyResource(ra::command_list* cmd, ra::resource, ra::resource dst) { atCopy(cmd, dst); return false; }
	bool onCopyRegion(ra::command_list* cmd, ra::resource, uint32_t, const ra::subresource_box*, ra::resource dst, uint32_t, const ra::subresource_box*, ra::filter_mode)
	{
		atCopy(cmd, dst); return false;
	}
	bool onResolve(ra::command_list* cmd, ra::resource, uint32_t, const ra::subresource_box*, ra::resource dst, uint32_t, int32_t, int32_t, int32_t, ra::format)
	{
		atCopy(cmd, dst); return false;
	}

	// --- the depth in the middle of the picture (ENB option, Auto focus) ---------
	// EscoFocus writes it into a 1x1 R32F texture during ReShade's effects.
	// Right after them it is copied into one of three CPU-readable textures,
	// and the one written two frames ago is read: by then the GPU is done
	// with it, so reading never stalls the frame.
	int g_focusLogs = 0;

	void readFocus(Entry& e, ra::command_list* cmd)
	{
		ra::device* dev = e.rt->get_device();
		ra::resource_view srv = {}, srvS = {};
		e.rt->get_texture_binding(e.focusTex, &srv, &srvS);
		if (!srv.handle) return;
		const ra::resource src = dev->get_resource_from_view(srv);
		if (!src.handle) return;
		for (ra::resource& st : e.staging)
			if (!st.handle &&
				!dev->create_resource(ra::resource_desc(1, 1, 1, 1, ra::format::r32_float, 1, ra::memory_heap::gpu_to_cpu, ra::resource_usage::copy_dest),
					nullptr, ra::resource_usage::copy_dest, &st))
			{
				if (g_focusLogs++ < 2) logf("reshade: could not make a readable texture for EscoFocus - the ENB option's Auto focus has no measurement");
				return;
			}
		const uint32_t w = e.focusFrame % 3;
		const ra::resource_usage from = ra::resource_usage::shader_resource, to = ra::resource_usage::copy_source;
		cmd->barrier(1, &src, &from, &to);
		cmd->copy_resource(src, e.staging[w]);
		cmd->barrier(1, &src, &to, &from);
		if (e.focusFrame >= 2)
		{
			const ra::resource r = e.staging[(e.focusFrame + 1) % 3];      // written two frames ago
			ra::subresource_data data = {};
			if (dev->map_texture_region(r, 0, nullptr, ra::map_access::read_only, &data) && data.data)
			{
				float d = 0.0f;
				memcpy(&d, data.data, 4);
				dev->unmap_texture_region(r, 0);
				enbdof::setCentre(d);
				if (g_focusLogs < 1) { ++g_focusLogs; logf("reshade: EscoFocus reads the middle of the picture - raw depth %.5f", d); }
			}
		}
		++e.focusFrame;
	}

	void dropStaging(Entry& e)
	{
		ra::device* dev = e.rt ? e.rt->get_device() : nullptr;
		for (ra::resource& st : e.staging)
			if (st.handle && dev) { dev->destroy_resource(st); st = {}; }
		e.focusFrame = 0;
	}

	// --- ReShade's own frame ---------------------------------------------------
	void refreshLive()
	{
		lights::g_flareLive = mainEntry() != nullptr;
	}

	void onBeginEffects(ra::effect_runtime* rt, ra::command_list*, ra::resource_view, ra::resource_view)
	{
		if (!rt || g_inCall) return;   // our own render_technique fires this too
		Entry* e = entryFor(rt, true);
		if (!e) return;
		e->bb = rt->get_current_back_buffer();
		{
			const ra::resource_desc d = rt->get_device()->get_resource_desc(e->bb);
			e->w = d.texture.width; e->h = d.texture.height;
			e->area = (uint64_t)d.texture.width * d.texture.height;
		}
		if (!e->immediate)
			if (ra::command_queue* q = rt->get_command_queue()) e->immediate = q->get_immediate_command_list();

		const ULONGLONG now = GetTickCount64();
		if (!e->found)
		{
			if (now < e->nextFind) return;
			e->nextFind = now + 1000;
			e->found = findAll(*e);
			if (e->found && !e->foundAt) e->foundAt = now;
			if (e->found)
			{
				logf("reshade: EscoFlare.fx is loaded for a %ux%u window - EscoEditor draws the lens flares from now on", e->w, e->h);
				refreshLive();
			}
			else if (e->findLogs++ == 0)
			{
				logf("reshade: EscoFlare.fx is not among the loaded effects (expected at %s) - press Reload in the ReShade overlay. "
					"If it never appears: ReShade.ini needs PerformanceMode=0 and SkipLoadingDisabledEffects=0", g_shaderFile);
			}
			if (!e->found) return;
		}

		Entry* m = mainEntry();

		// EscoFocus runs in ReShade's own pass while the ENB option is on
		if (!e->focusLooked)
		{
			e->focusLooked = true;
			e->focusTech = rt->find_technique(kEffect, "EscoFocus");
			e->focusTex = rt->find_texture_variable(kEffect, "EE_FocusTex");
			if (e == m && (!e->focusTech.handle || !e->focusTex.handle))
				logf("reshade: the loaded EscoFlare.fx has no EscoFocus - an older copy? Press Reload in ReShade (the ENB option's Auto focus needs it)");
		}
		// Whether EscoFocus runs this frame is decided after the last frame
		// finished (focusSwitch, from reshade_present), never here: switching a
		// technique on while ReShade is still loading its effects made it draw
		// a pass whose resources did not exist yet - a null read inside
		// ReShade, dxgi.dll+1131CC, on every start with the ENB option on (4.19).
		// An export starting between frames: switched off before ReShade draws
		// (switching off is safe here; it is switching ON mid-frame that is not)
		if (game::exporting() && e->focusTech.handle && rt->get_technique_state(e->focusTech)) rt->set_technique_state(e->focusTech, false);
		e->focusOn = e->focusTech.handle && e->focusTex.handle && rt->get_technique_state(e->focusTech);

		lights::FlareSet F;
		const bool want = wantNow(F);
		if (e == m) g_armed = want && rt->get_effects_state();

		// The technique stays off in ReShade: EscoEditor draws it before the
		// menus. Only a frame where that did not happen gets it from ReShade,
		// here at Present (switched on for this one pass of the effect list).
		const bool fallback = e == m && want && !e->fr.drawn;
		if (rt->get_technique_state(e->tech) != fallback) rt->set_technique_state(e->tech, fallback);
		e->fallbackOn = fallback;
		if (fallback && pushValues(*e, F) <= 0) { rt->set_technique_state(e->tech, false); e->fallbackOn = false; }
		else if (!want) seti(*e, V_COUNT_, 0);

		if (e == m)
		{
			const int outcome = !want ? 0 : e->fr.drawn ? 1 : 2;
			// The pre-UI draw finds the game's own menu draw on most frames but
			// not every one, so only a state that has held for half a second is
			// worth a line - the log used to flip-flop frame after frame.
			if (outcome == g_outcomeSeen) ++g_outcomeHeld;
			else { g_outcomeSeen = outcome; g_outcomeHeld = 1; }
			if (g_outcomeHeld == 30 && outcome != g_outcome && g_outcomeLogs < 12)
			{
				++g_outcomeLogs;
				logf("reshade: lens flares %s", outcome == 0 ? "off" : outcome == 1 ? "drawn before the game's menus"
												   : "drawn by ReShade at the end of the frame (no menu draw found this frame)");
				g_outcome = outcome;
			}
		}
	}

	void onFinishEffects(ra::effect_runtime* rt, ra::command_list* cmd, ra::resource_view, ra::resource_view)
	{
		if (g_inCall) return;
		Entry* e = entryFor(rt, false);
		if (e && e->found && e->fallbackOn) { rt->set_technique_state(e->tech, false); e->fallbackOn = false; }
		if (e && e->focusOn && cmd) readFocus(*e, cmd);
	}

	// EscoFocus on or off for the next frame, decided once this one is done.
	// Only while a replay runs, with the ENB option on, and only once
	// ReShade has said its effects are loaded - three seconds after that, or
	// ten after EscoFlare.fx was first seen if that word never comes.
	int g_focusSwitchLogs = 0;

	// Extended Video Export takes an export's frames at Present, after ReShade.
	// If the game's Export cannot be recognised on this build, EscoFocus stays
	// off altogether while EVE is there - no measuring beats a black export.
	bool eveLoaded()
	{
		static ULONGLONG s_next = 0;
		static bool s_eve = false;
		const ULONGLONG now = GetTickCount64();
		if (now >= s_next) { s_next = now + 5000; s_eve = GetModuleHandleA("ExtendedVideoExport.asi") != nullptr; }
		return s_eve;
	}
	bool focusBlocked()
	{
		static bool s_logged = false;
		const bool blocked = eveLoaded() && !game::a_PlaybackType;
		if (blocked && !s_logged)
		{
			s_logged = true;
			logf("reshade: Extended Video Export is loaded and the game's Export could not be recognised on this build - "
				 "EscoFocus stays off (Auto focus and Enter on Focus Distance have nothing to measure; Manual and Player work)");
		}
		return blocked;
	}

	void focusSwitch(Entry& e)
	{
		if (!e.found || !e.focusTech.handle || !e.focusTex.handle) return;
		const ULONGLONG now = GetTickCount64();
		const bool settled = e.loadedAt ? now - e.loadedAt > 3000 : (e.foundAt && now - e.foundAt > 10000);
		// Never during the game's Export: Extended Video Export takes the frame at
		// Present, after ReShade, and with EscoFocus on ReShade ran its whole
		// effect pass in the middle of that - the export came out black (4.23).
		// Auto focus holds its last measurement through an export instead.
		const bool want = &e == mainEntry() && settled && g_cfg.enbMeasure && enbdof::wantsCentre() && replayMode() > 0 && !game::exporting() && !focusBlocked() && e.rt->get_effects_state();
		if (e.rt->get_technique_state(e.focusTech) == want) return;
		e.rt->set_technique_state(e.focusTech, want);
		if (g_focusSwitchLogs < 6) { ++g_focusSwitchLogs; logf("reshade: EscoFocus %s", want ? "on - measuring the middle of the picture for the ENB focus" : "off"); }
	}

	// After ReShade's Present work: report what the frame looked like, then
	// start the next one.
	void onPresentDone(ra::effect_runtime* rt)
	{
		Entry* e = entryFor(rt, false);
		if (!e) return;
		focusSwitch(*e);
		if (e == mainEntry()) InterlockedIncrement(&g_presented);   // one per frame the game draws
		if (e == mainEntry() && g_armed)
		{
			++g_armedFrames;
			const Frame& f = e->fr;
			if (g_frameLogs < 4 || (g_frameLogs < 12 && f.drawn != (g_outcome == 1)))
			{
				++g_frameLogs;
				if (!f.bbBinds && !f.sceneIn)
					logf("reshade: frame - the game never drew into the back buffer this frame; the flare was left to ReShade at the end of the frame");
				else if (f.uiSeen)
					logf("reshade: frame - back buffer bound %d times, %d draws into it; the picture came in by %s (%u), the menus start with %s (%u); flare %s",
						f.bbBinds, f.bbDraws, kindName(f.sceneKind), f.sceneCount, kindName(f.uiKind), f.uiCount,
						f.drawn ? "drawn just before them" : "not drawn there (not wanted at that moment)");
				else
					logf("reshade: frame - back buffer bound %d times, %d draws into it; the picture came in by %s (%u) and nothing was drawn after it; flare left to ReShade at the end of the frame",
						f.bbBinds, f.bbDraws, f.sceneIn ? kindName(f.sceneKind) : "nothing seen", f.sceneCount);
			}
			if (!g_noEventsLogged && g_armedFrames > 120 && !g_sawBind)
			{
				g_noEventsLogged = true;
				logf("reshade: no draw events reach EscoEditor (a ReShade without full add-on support?) - the flare stays at the end of the frame, over the menus");
			}
		}
		e->fr = Frame();
	}

	void onReloaded(ra::effect_runtime* rt)
	{
		if (Entry* e = entryFor(rt, true))
		{
			e->found = false; e->nextFind = 0; e->fallbackOn = false; e->focusLooked = false; e->focusOn = false; e->focusFrame = 0;
			e->loadedAt = GetTickCount64();       // all of ReShade's effects are loaded now
		}
		if (g_bound && g_bound->rt == rt) g_bound = nullptr;
		refreshLive();
	}

	void onDestroyRuntime(ra::effect_runtime* rt)
	{
		if (Entry* e = entryFor(rt, false))
		{
			if (g_bound == e) g_bound = nullptr;
			dropStaging(*e);
			*e = Entry();
		}
		refreshLive();
	}

	// ReShade present but without the add-on API: its log sits next to it.
	// Did a ReShade write its log during THIS run of the game? The log sits
	// next to the plugin, so one left by an earlier run says nothing - that is
	// the case where ReShade simply did not load (another wrapper in front of
	// it, a renamed dll, an update gone wrong), which is a different problem
	// from a ReShade built without add-on support.
	bool logFrom(bool thisSession)
	{
		char p[MAX_PATH];
		snprintf(p, sizeof(p), "%sReShade.log", g_dir);
		WIN32_FILE_ATTRIBUTE_DATA fa;
		if (!GetFileAttributesExA(p, GetFileExInfoStandard, &fa)) return false;
		FILETIME started, exited, kernel, user;
		if (!GetProcessTimes(GetCurrentProcess(), &started, &exited, &kernel, &user)) return false;
		ULARGE_INTEGER lg, st;
		lg.LowPart = fa.ftLastWriteTime.dwLowDateTime;  lg.HighPart = fa.ftLastWriteTime.dwHighDateTime;
		st.LowPart = started.dwLowDateTime;             st.HighPart = started.dwHighDateTime;
		return thisSession ? lg.QuadPart > st.QuadPart : lg.QuadPart <= st.QuadPart;
	}

	// The key that opens ReShade's own menu (ReShade.ini, [INPUT] KeyOverlay =
	// key, ctrl, shift, alt): the light editor's key must not be that one.
	void readOverlayKey()
	{
		char ini[MAX_PATH], b[64] = {};
		snprintf(ini, sizeof(ini), "%sReShade.ini", g_reshadeDir);
		GetPrivateProfileStringA("INPUT", "KeyOverlay", "", b, sizeof(b), ini);
		const int vk = atoi(b);
		lights::g_reshadeKey = (vk > 0 && vk <= 0xFE) ? vk : 0;
		char k[32];
		if (lights::g_reshadeKey) logf("reshade: its menu opens with %s", keyName(lights::g_reshadeKey, k, sizeof(k)));
	}

	bool finishRegister(const char* when)
	{
		char mod[MAX_PATH] = {};
		GetModuleFileNameA(g_module, mod, MAX_PATH);
		strcpy_s(g_reshadeDir, mod);
		if (char* s = strrchr(g_reshadeDir, '\\')) s[1] = '\0';
		logf("reshade: found at %s (%s)", mod, when);
		readOverlayKey();

		// By hand rather than with reshade::register_addon: with imgui.h in the
		// build that helper would refuse a ReShade built with another ImGui
		// outright; this way only the window is missing and the log says why.
		using FnRegister = bool (*)(HMODULE, uint32_t);
		const FnRegister reg = (FnRegister)GetProcAddress(g_module, "ReShadeRegisterAddon");
		if (!reg || !reg(g_self, RESHADE_API_VERSION))
		{
			g_gaveUp = true;
			logf("reshade: ReShadeRegisterAddon refused us (we ask for API %d) - the light editor window and the lens flares need ReShade 6.3 or newer "
				"with add-on support, and EscoEditor must not be disabled in ReShade's Add-ons tab. The lights themselves still draw.", RESHADE_API_VERSION);
			return false;
		}
		g_registered = true;
		g_shaderInstalled = installShader();

		using FnImGui = const imgui_function_table* (*)(uint32_t);
		if (const FnImGui table = (FnImGui)GetProcAddress(g_module, "ReShadeGetImGuiFunctionTable"))
			imgui_function_table_instance() = table(IMGUI_VERSION_NUM);
		lightui::g_imgui = imgui_function_table_instance() != nullptr;
		lights::g_haveWindow = lightui::g_imgui;
		if (lightui::g_imgui)
		{
			reshade::register_event<reshade::addon_event::reshade_overlay>(&lightui::onOverlay);
			reshade::register_overlay("EscoEditor", &lightui::onReshadeTab);
		}
		else
			logf("reshade: this ReShade has no ImGui %d table - no light editor window (the lights still draw)", IMGUI_VERSION_NUM);

		reshade::register_event<reshade::addon_event::reshade_begin_effects>(&onBeginEffects);
		reshade::register_event<reshade::addon_event::reshade_finish_effects>(&onFinishEffects);
		reshade::register_event<reshade::addon_event::reshade_present>(&onPresentDone);
		reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(&onReloaded);
		reshade::register_event<reshade::addon_event::destroy_effect_runtime>(&onDestroyRuntime);
		reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(&onBindTargets);
		reshade::register_event<reshade::addon_event::draw>(&onDraw);
		reshade::register_event<reshade::addon_event::draw_indexed>(&onDrawIndexed);
		reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(&onDrawIndirect);
		reshade::register_event<reshade::addon_event::copy_resource>(&onCopyResource);
		reshade::register_event<reshade::addon_event::copy_texture_region>(&onCopyRegion);
		reshade::register_event<reshade::addon_event::resolve_texture_region>(&onResolve);
		logf("reshade: registered EscoEditor as an add-on (API %d, %s)%s%s", RESHADE_API_VERSION, when,
			lightui::g_imgui ? " - the light editor can open and ReShade's menu has an EscoEditor tab" : "",
			g_shaderInstalled ? "; lens flares ready" : "; EscoFlare.fx missing, no lens flares");
		return true;
	}

	// From DllMain: ReShade is normally loaded before any .asi.
	void registerEarly()
	{
		g_module = reshade::internal::get_reshade_module_handle();
		if (g_module) finishRegister("while the plugin loads");
		else logf("reshade: not in the process yet - will look again from the worker");
	}

	// From the worker, every half second, until registered or given up.
	void poll()
	{
		if (g_registered || g_gaveUp) return;
		if (!g_module) g_module = reshade::internal::get_reshade_module_handle();
		if (!g_module)
		{
			if (++g_tries >= 120)
			{
				g_gaveUp = true;
				if (logFrom(true))
					logf("reshade: a ReShade is running but exports no add-on API - the light editor window and the lens flares need the 'with full add-on support' build (the lights still draw)");
				else if (logFrom(false))
					logf("reshade: ReShade is installed here but did not load into the game this time - its ReShade.log is older than this session. "
						"Check that its dll (dxgi.dll) is still in the plugins folder and that nothing else took the same name; ENB's d3d11.dll can live "
						"alongside it. Until then: no light editor window and no lens flares, everything else works");
				else
					logf("reshade: not in the process after 60 s - no light editor window and no lens flares (both need ReShade with add-on support); everything else works");
			}
			return;
		}
		finishRegister("late - if EscoFlare.fx was just installed, press Reload in ReShade");
	}
}
