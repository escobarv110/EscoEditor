// =========================================================================
//  ecs_director.inc - the per-frame hook on the replay director
// =========================================================================
//  camReplayDirector::UpdateSmoothing runs once per replay frame, after the
//  free camera has integrated this frame's movement and before the frame is
//  rendered - the function Rockstar Editor+ hooks to own the camera's
//  output. On the way out:
//
//  1. The camera speed. NoEditorRestrictions rewrites the metadata's
//     MaxSpeed fields on EVERY frame the free camera runs, so a plugin that
//     only rewrites the metadata loses. Instead the free camera's movement
//     for this frame is rescaled by k = desired / the MaxSpeed the block
//     held: the correction goes into the camera's own accumulator
//     (cam+0x480, where the stick integrates), its frame, and the director's
//     rendered and authoring frames, so what you see, what the next frame
//     starts from and what a new marker records all agree. Only in Edit
//     Camera, only for the free camera (its metadata pointer is our block),
//     only when it is not mounted on an entity, and never across a jump of
//     more than 50 m in one frame.
//  2. The director pointer is remembered for the other features.
//  3. The scene lights read the camera and publish what to draw (ecs_lights.inc).
//  4. The Lights page's key row catches up with a key picked for it (ecs_menu.inc).
// =========================================================================
namespace director
{
	using FnUpdate = void (__fastcall*)(void*);

	// camReplayDirector::UpdateSmoothing(this), Legacy. Prologue free of
	// RIP-relative operands; the only wildcard is the frame size. Verified
	// on the b3407 image (rva 0x2A5534, as RE+ logged live).
	const char* kSig =
		"48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 41 54 41 55 41 56 41 57 "
		"48 8D 6C 24 80 48 81 EC ? ? ? ? 0F 29 70 C8 0F 29 78 B8 44 0F 29 40 A8 "
		"48 8B D9 48 8B 89 C0 02 00 00";

	uintptr_t a_UpdateSmoothing = 0;
	FnUpdate  origUpdate = nullptr;

	uint8_t* s_prevCam = nullptr;
	float    s_prev[3] = {};
	bool     s_have    = false;
	int      s_logs    = 0;
	float    s_lastK   = 1.0f;

	inline bool finite3(const float* v) { return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]); }

	void addTo(uint8_t* at, const float* d)
	{
		float v[3]; memcpy(v, at, 12);
		if (!finite3(v)) return;
		v[0] += d[0]; v[1] += d[1]; v[2] += d[2];
		memcpy(at, v, 12);
	}

	// Plain C body: __try must not share a frame with anything that unwinds.
	void rescale(uint8_t* dir)
	{
		__try
		{
			uint8_t* cam = *(uint8_t**)(dir + rd::DIR_ActiveCamera);
			if (!cam || !g_metaCount || !editingCam()) { s_have = false; return; }

			uint8_t* meta = *(uint8_t**)(cam + rd::CAM_Metadata);
			int mi = -1;
			for (int i = 0; i < g_metaCount; ++i) if (g_meta[i].p == meta) { mi = i; break; }
			if (mi < 0 || *(void**)(cam + rd::CAM_AttachEntity) != nullptr) { s_have = false; return; }

			float pos[3]; memcpy(pos, cam + rd::CAM_Position, 12);
			if (!finite3(pos)) { s_have = false; return; }

			if (s_have && s_prevCam == cam)
			{
				const float d[3] = { pos[0] - s_prev[0], pos[1] - s_prev[1], pos[2] - s_prev[2] };
				const float len2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
				if (len2 > 0.0f && len2 < 50.0f * 50.0f)
				{
					const Meta& m  = g_meta[mi];
					const float hold = g_holdMul;
					const float curH = rdf(meta, OFF_RespH + RF_Speed);
					const float curV = rdf(meta, OFF_RespV + RF_Speed);
					const float desH = m.h.speed * (float)g_cfg.speed / 100.0f * hold;
					const float desV = m.v.speed * (float)(g_cfg.vertical > 0 ? g_cfg.vertical : g_cfg.speed) / 100.0f * hold;
					const float kH = curH > 0.0f ? desH / curH : 1.0f;
					const float kV = curV > 0.0f ? desV / curV : 1.0f;

					if (fabsf(kH - 1.0f) > 0.002f || fabsf(kV - 1.0f) > 0.002f)
					{
						const float corr[3] = { d[0] * (kH - 1.0f), d[1] * (kH - 1.0f), d[2] * (kV - 1.0f) };
						addTo(cam + rd::CAM_Position, corr);
						addTo(cam + rd::CAM_FramePos, corr);
						addTo(dir + rd::DIR_RenderPos, corr);
						addTo(dir + rd::DIR_AuthorPos, corr);
						pos[0] += corr[0]; pos[1] += corr[1]; pos[2] += corr[2];

						if (s_logs < 4 && fabsf(kH - s_lastK) > 0.01f)
						{
							++s_logs;
							s_lastK = kH;
							logf("director: camera runs at %.1f m/s, you asked for %.1f -> movement x%.3f (block owned by %s)",
								curH, desH, kH, fabsf(curH - m.h.speed) < 0.01f ? "the game" : "another mod");
						}
					}
				}
			}
			memcpy(s_prev, pos, 12);
			s_prevCam = cam;
			s_have    = true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { s_have = false; }
	}

	void __fastcall hkUpdateSmoothing(void* self)
	{
		origUpdate(self);
		if (!self) return;
		rd::g_lastDirector = (uint8_t*)self;
		rescale((uint8_t*)self);
		// After RE+'s own tick, which runs inside origUpdate above: the ramps,
		// the look response and the mouse multiplier are written again here so
		// the numbers on our rows are the ones the frame uses. Without this,
		// whoever wrote last won - and that is RE+, every frame.
		if (g_rePlus && g_metaCount) applyAll(g_holdMul, GetTickCount64());
		lights::onDirectorFrame((uint8_t*)self);
		camfree::onFrame();
		enbdof::fromGame();      // only when ENB is not calling back itself
		menu::tick();
	}

	bool install()
	{
		if (isEnhanced()) { logf("director: GTA V Enhanced is not mapped - metadata writes only"); return false; }
		bool hooked = false;
		a_UpdateSmoothing = (uintptr_t)scanFnBytes(kSig, &hooked);
		logf("director: UpdateSmoothing %s rva 0x%llX%s", a_UpdateSmoothing ? "at" : "NOT FOUND -", rva(a_UpdateSmoothing),
			hooked ? "  (already hooked by another plugin - ours chains on top)" : "");
		if (!a_UpdateSmoothing) return false;
		game::resolveReplayTime(a_UpdateSmoothing);
		if (!hookFn(a_UpdateSmoothing, (void*)hkUpdateSmoothing, (void**)&origUpdate, "UpdateSmoothing"))
			return false;
		g_rescale = true;
		logf("director: hooked - camera speed applied to the movement every frame; clip clock %s",
			game::a_ReplayTimeMs ? "found" : "not found (animated lights cannot follow the playhead)");
		return true;
	}
}
