// =============================================================================
//  ecs_camfree.inc - Free Look: fly the editor's camera at a keyframe without
//  leaving that keyframe's camera changed.
//
//  In the Rockstar Editor the free camera in Edit Camera IS the marker's
//  camera: every metre flown is written into the marker record. So to look
//  around without authoring anything, the marker's camera fields are copied
//  before the flight and put back after it - and where the free camera got to
//  is kept, so switching goes back and forth between the two: your own view,
//  and the camera the clip will actually render.
//
//  Nothing here touches the game's camera code. It reads and writes the marker
//  record - the same fields the keyframe clipboard copies - and then calls the
//  editor's own Recalculate and BlendUpdated for that marker, which is how the
//  clipboard's paste makes the director pick a change up.
//
//  The marker is addressed by storage + index, and the time stamp of the
//  marker is checked before anything is put back: if the editor has rebuilt
//  its list underneath us, the camera is left alone rather than written into
//  the wrong keyframe.
// =============================================================================
namespace camfree
{
	// What flying changes, and nothing else: no DOF, no time, no speed index.
	struct Field { uint32_t off, size; };
	constexpr Field kFields[] = {
		{ mk::OFF_Quat,       8 },   // orientation, packed
		{ mk::OFF_AttachQuat, 8 },
		{ mk::OFF_AttachPos, 12 },
		{ mk::OFF_Pos,       12 },   // world position
		{ mk::OFF_PosRel,    12 },
		{ mk::OFF_AttachId,   4 },
		{ mk::OFF_LookAtId,   4 },
		{ mk::OFF_CamType,    1 },
		{ mk::OFF_Fov,        1 },
		{ mk::OFF_CamValid,   1 } };
	constexpr int kFieldCount = (int)(sizeof(kFields) / sizeof(kFields[0]));

	struct Snap { bool have; float t; uint8_t b[mk::SIZE]; };

	Snap  g_authored = {};        // the keyframe's camera, as it was
	Snap  g_free = {};            // where the free camera was left
	void* g_storage = nullptr;    // the marker's storage ...
	int   g_index = -1;           // ... and its index in it
	bool  g_on = false;
	volatile LONG g_request = 0;  // the key or the menu row -> the game thread
	int   g_logs = 0;
	char  g_last[128] = "Off - the keyframe's own camera.";

	bool on() { return g_on; }
	void request() { InterlockedExchange(&g_request, 1); }

	void copyFields(uint8_t* dst, const uint8_t* src)
	{
		for (int i = 0; i < kFieldCount; ++i) memcpy(dst + kFields[i].off, src + kFields[i].off, kFields[i].size);
	}

	bool grab(Snap& s, const void* m)
	{
		__try { memcpy(s.b, m, mk::SIZE); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		float t;
		memcpy(&t, s.b + mk::OFF_TimeMs, 4);
		s.t = t;
		s.have = true;
		return true;
	}

	// Writes the camera fields of `s` into the marker, but only if that marker
	// is still the one the snapshot came from.
	bool apply(void* m, const Snap& s)
	{
		if (!m || !s.have) return false;
		__try
		{
			float t;
			memcpy(&t, (const uint8_t*)m + mk::OFF_TimeMs, 4);
			if (!(fabsf(t - s.t) < 0.5f)) return false;      // a different keyframe now
			copyFields((uint8_t*)m, s.b);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		return true;
	}

	// The editor's own "this marker changed" pair, so the director rebuilds the
	// camera from the record instead of from what it had blended before.
	void settle()
	{
		if (!g_storage || g_index < 0) return;
		st::recalc(g_storage, g_index);
		st::blendUpdated(g_storage, g_index);
	}

	void* markerNow() { return (g_storage && g_index >= 0) ? st::tryGet(g_storage, g_index) : nullptr; }

	// The keyframe being edited. GetCurrentEditMarker answers only while a
	// marker's MENU is open, and in Edit Camera - where the flying happens -
	// it is not: the two are mutually exclusive, which is why the key could
	// never find a keyframe. The editor's own marker index is what holds while
	// the camera is being flown, so that comes first; the menu's marker is the
	// fallback, for the Free Look row.
	void* markerForEdit(void** storage, int* index)
	{
		*storage = nullptr;
		*index = -1;
		void* s = game::replayStorage();
		const int idx = game::editMarkerIndex();
		if (s && idx >= 0)
			if (void* m = st::tryGet(s, idx)) { *storage = s; *index = idx; return m; }
		if (void* m = game::currentMarker())
		{
			int i = -1;
			void* s2 = game::storageFor(m, &i);
			if (s2 && i >= 0) { *storage = s2; *index = i; return m; }
		}
		return nullptr;
	}

	void stop(const char* why)
	{
		if (!g_on) return;
		void* m = markerNow();
		bool back = false;
		if (m)
		{
			grab(g_free, m);                       // keep the view for next time
			back = apply(m, g_authored);           // and put the keyframe's camera back
			if (back) settle();
		}
		g_on = false;
		g_storage = nullptr;
		g_index = -1;
		snprintf(g_last, sizeof(g_last), back ? "Off - the keyframe's own camera is back."
		                                      : "Off - the keyframe could not be found to put its camera back.");
		if (g_logs < 40)
		{
			++g_logs;
			logf("camera: free look off (%s) - %s", why, back ? "the keyframe's camera is back as it was"
			                                                  : "WARNING: the marker was gone, nothing written back");
		}
	}

	void start()
	{
		void* s = nullptr;
		int idx = -1;
		void* m = markerForEdit(&s, &idx);
		if (!m)
		{
			snprintf(g_last, sizeof(g_last), "Stand on a keyframe in the editor first.");
			if (g_logs < 40) { ++g_logs; logf("camera: free look found no keyframe - the editor is not on one"); }
			return;
		}
		if (!grab(g_authored, m)) return;
		g_storage = s;
		g_index = idx;
		g_on = true;

		// back to where the free camera was left, when that was this keyframe
		const bool resumed = g_free.have && apply(m, g_free);
		if (resumed) settle();
		snprintf(g_last, sizeof(g_last), resumed ? "On - your own view. Off puts the keyframe's camera back."
		                                         : "On - fly; the keyframe's camera is kept for when you switch off.");
		if (g_logs < 40)
		{
			++g_logs;
			logf("camera: free look on at keyframe %d (%.0f ms)%s - flying no longer changes it", idx, g_authored.t,
				resumed ? ", back to where you left off" : "");
		}
	}

	// From the replay director hook: the game thread, once per frame.
	void onFrame()
	{
		const bool req = InterlockedExchange(&g_request, 0) != 0;

		if (g_on)
		{
			// the editor moved to another keyframe, or out of one: put it back
			void* s = nullptr;
			int idx = -1;
			void* m = markerForEdit(&s, &idx);
			if (!m || s != g_storage || idx != g_index) stop("the editor left that keyframe");
		}
		if (!req) return;
		if (g_on) stop("switched off");
		else      start();
	}

	// The editor closed, or playback started: never leave a keyframe holding
	// the free camera.
	void leave(const char* why) { if (g_on) stop(why); }
}
