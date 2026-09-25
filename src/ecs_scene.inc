// =========================================================================
//  ecs_scene.inc - time of day and weather override
// =========================================================================
//  A clip records the clock and the weather every frame and replays both,
//  so setting either from outside does not survive a frame. The replay
//  hands each frame's weather packet to CPacketWeather::Extract, which only
//  READS from it: the hook gives it a patched stack copy instead. The
//  frame's clock packet sits 20 bytes in front of the weather packet
//  (validated by id, size and guard word every time); the play pass reads it
//  inline, so that one is patched in the buffer and put back at the start of
//  the next call, after the game consumed it.
//
//  Lighting: a clip also records the fully resolved timecycle (the ~430
//  lighting variables) and copies it back over the live one, variable by
//  variable, gated on a per-variable flag in the timecycle table. With the
//  flags cleared the lighting is evaluated from the live clock and weather
//  again, so the sun really moves and rain really darkens the sky. The flags
//  are put back the moment the editor is left: the recording path reads them
//  too. The technique is Rockstar Editor+'s (scene.cpp).
// =========================================================================
namespace scene
{
	using FnExtract = void (__fastcall*)(const void*);
	FnExtract origExtract = nullptr;
	bool g_installed = false;

	// Session-only overrides (-1 = as recorded). Not saved to the ini on
	// purpose: a forgotten override would silently colour every later clip.
	volatile int g_time    = -1;    // minutes since midnight, 0..1439
	volatile int g_weather = -1;    // index into kWeather

	// A clip with keys of its own drives these instead of the two rows above.
	// Published once a frame by ecs_lights.inc, which owns the per-clip store
	// and knows where the playhead is; read here, inside the packet hook.
	// Stale after a second, so a clip that stops being played falls straight
	// back to the fixed rows.
	volatile int   g_animTime  = -1;     // minutes since midnight
	volatile int   g_animFrom  = -1;     // weather blending from ...
	volatile int   g_animTo    = -1;     // ... to
	volatile float g_animBlend = 0.0f;   // 0 .. 1 between them
	volatile LONGLONG g_animAt = 0;

	bool animLive()
	{
		const ULONGLONG now = GetTickCount64();
		const ULONGLONG at = (ULONGLONG)g_animAt;
		return at && now >= at && now - at < 1000;
	}

	int animTime()    { return animLive() ? (int)g_animTime : -1; }
	int animWeather() { return animLive() ? (int)g_animFrom : -1; }

	constexpr unsigned kGuard = 0xAAAAAAAAu;
	constexpr unsigned short kIdClock = 5, kIdWeather = 6;
	constexpr int kClockSize = 20, kWeatherSize = 288;
	constexpr int kClockHours = 12, kClockMinutes = 13, kClockSeconds = 14;
	constexpr int kWeatherOld = 0x0C, kWeatherNew = 0x10, kWeatherInterp = 0x14;

	// weather.xml load order (stock file). The packet stores an index, the
	// names are only for the menu.
	const char* const kWeather[] = {
		"Extra Sunny", "Clear", "Clouds", "Smog", "Foggy", "Overcast", "Rain", "Thunder",
		"Clearing", "Neutral", "Snow", "Blizzard", "Light Snow", "Xmas", "Halloween" };
	constexpr int kWeatherCount = (int)(sizeof(kWeather) / sizeof(kWeather[0]));

	unsigned char* s_clockPkt = nullptr;
	unsigned char  s_clockWas[3] = {};
	int  s_badWarned = 0;

	// --- timecycle flags ---------------------------------------------------
	constexpr int kTcStride = 32, kTcReplayOff = 0x18, kTcMax = 1024;
	unsigned char s_tcWas[kTcMax];
	int  s_tcCount = -1;        // -1 not walked yet, 0 unusable
	bool s_tcCleared = false;

	int walkTcTable()
	{
		if (!game::a_TcTable) return 0;
		int n = 0;
		__try
		{
			const uint8_t* base = (const uint8_t*)game::a_TcTable;
			for (; n < kTcMax; ++n)
			{
				int id; memcpy(&id, base + n * kTcStride, 4);
				if (id != n) break;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
		if (n < 64 || n >= kTcMax)
		{
			logf("scene: timecycle table walk gave %d entries - not touched, the lighting stays as recorded", n);
			return 0;
		}
		logf("scene: timecycle table has %d variables (rva 0x%llX)", n, rva(game::a_TcTable));
		return n;
	}

	void setTcFlags(bool clear)
	{
		if (s_tcCount < 0) s_tcCount = walkTcTable();
		if (s_tcCount <= 0) return;
		__try
		{
			uint8_t* base = (uint8_t*)game::a_TcTable;
			for (int i = 0; i < s_tcCount; ++i)
			{
				uint8_t* flag = base + i * kTcStride + kTcReplayOff;
				if (clear) { s_tcWas[i] = *flag; *flag = 0; }
				else       { *flag = s_tcWas[i]; }
			}
			s_tcCleared = clear;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { s_tcCount = 0; }
	}

	// --- packets -------------------------------------------------------------
	inline unsigned pktSize(const unsigned char* p) { unsigned w; memcpy(&w, p + 4, 4); return w & 0xFFFFFF; }

	bool isPacket(const unsigned char* p, unsigned short id, int size)
	{
		unsigned short pid; unsigned guard;
		memcpy(&pid, p, 2); memcpy(&guard, p + 8, 4);
		return pid == id && guard == kGuard && pktSize(p) == (unsigned)size;
	}

	void putBackClock()
	{
		if (!s_clockPkt) return;
		__try
		{
			if (isPacket(s_clockPkt, kIdClock, kClockSize))
			{
				s_clockPkt[kClockHours]   = s_clockWas[0];
				s_clockPkt[kClockMinutes] = s_clockWas[1];
				s_clockPkt[kClockSeconds] = s_clockWas[2];
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
		s_clockPkt = nullptr;
	}

	bool overridesActive() { return g_installed && (g_time >= 0 || g_weather >= 0 || animTime() >= 0 || animWeather() >= 0); }

	void extractBody(unsigned char* pkt)
	{
		if (!isPacket(pkt, kIdWeather, kWeatherSize))
		{
			if (s_badWarned++ == 0)
				logf("scene: weather packet at %p is not the 288-byte layout (size %u) - overrides off", (void*)pkt, pktSize(pkt));
			origExtract(pkt);
			return;
		}
		const int anim = animTime();
		const int t = anim >= 0 ? anim : g_time;     // a keyed clip wins over the row
		if (t >= 0)
		{
			unsigned char* clk = pkt - kClockSize;
			if (isPacket(clk, kIdClock, kClockSize))
			{
				s_clockWas[0] = clk[kClockHours]; s_clockWas[1] = clk[kClockMinutes]; s_clockWas[2] = clk[kClockSeconds];
				s_clockPkt = clk;
				clk[kClockHours]   = (unsigned char)(t / 60);
				clk[kClockMinutes] = (unsigned char)(t % 60);
				clk[kClockSeconds] = 0;
			}
			else if (s_badWarned++ == 0)
				logf("scene: no clock packet in front of the weather packet - time of day left as recorded");
		}
		// The packet carries an old type, a new type and how far between them
		// the weather is - the game's own way of changing weather. A fixed row
		// sets both ends the same; a keyed clip sets the two ends of the key
		// pair it is between, and the blend does the rest.
		const int keyedFrom = animWeather();
		int from = g_weather, to = g_weather;
		float interp = 0.0f;
		if (keyedFrom >= 0)
		{
			from = keyedFrom;
			to = (int)g_animTo >= 0 ? (int)g_animTo : keyedFrom;
			interp = g_animBlend;
			if (!(interp >= 0.0f)) interp = 0.0f;
			if (interp > 1.0f) interp = 1.0f;
		}
		if (from < 0) { origExtract(pkt); return; }
		if (from >= kWeatherCount) from = kWeatherCount - 1;
		if (to < 0 || to >= kWeatherCount) to = from;
		alignas(16) unsigned char copy[kWeatherSize];
		memcpy(copy, pkt, kWeatherSize);
		memcpy(copy + kWeatherOld, &from, 4);
		memcpy(copy + kWeatherNew, &to, 4);
		memcpy(copy + kWeatherInterp, &interp, 4);
		origExtract(copy);
	}

	void __fastcall hkExtract(const void* packet)
	{
		putBackClock();
		if (!packet || !overridesActive()) { origExtract(packet); return; }
		if (!s_tcCleared && replayMode() != 0) setTcFlags(true);
		__try { extractBody((unsigned char*)packet); }
		__except (EXCEPTION_EXECUTE_HANDLER) { origExtract(packet); }
	}

	// From the worker: the lighting flags go back the moment the editor is
	// closed or both rows are back on As Recorded.
	void tick()
	{
		// Same reach as the lights: an export is not the editor being open.
		const bool want = overridesActive() && replayMode() != 0;
		if (want == s_tcCleared) return;
		setTcFlags(want);
	}

	void restore()
	{
		putBackClock();
		if (s_tcCleared) setTcFlags(false);
	}

	bool install()
	{
		if (!g_cfg.scene) { logf("scene: Scene=0 - no Time & Weather rows"); return false; }
		if (!game::a_WeatherExtract) { logf("scene: CPacketWeather::Extract not found - no Time & Weather rows"); return false; }
		if (!hookFn(game::a_WeatherExtract, (void*)hkExtract, (void**)&origExtract, "CPacketWeather::Extract")) return false;
		g_installed = true;
		logf("scene: hooked - Time of Day / Weather rows are live%s",
			game::a_TcTable ? "" : " (timecycle table not found: the sky changes but the lighting stays as recorded)");
		return true;
	}

	const char* timeName(char* buf, int cap)
	{
		const int keyed = animTime();
		if (keyed >= 0) { snprintf(buf, cap, "%02d:%02d (keyed)", keyed / 60, keyed % 60); return buf; }
		const int t = g_time;
		if (t < 0) return "As Recorded";
		snprintf(buf, cap, "%02d:%02d", t / 60, t % 60);
		return buf;
	}

	const char* weatherName()
	{
		const int keyed = animWeather();
		if (keyed >= 0 && keyed < kWeatherCount) return kWeather[keyed];
		const int w = g_weather;
		return w < 0 ? "As Recorded" : w < kWeatherCount ? kWeather[w] : "?";
	}

	// Left/right: As Recorded, then the day in 15-minute steps, wrapping.
	void stepTime(int delta)
	{
		int t = g_time;
		if (t < 0) t = delta > 0 ? 0 : 23 * 60 + 45;
		else
		{
			t += 15 * delta;
			if (t < 0 || t >= 24 * 60) t = -1;
		}
		g_time = t;
	}

	void stepWeather(int delta)
	{
		int w = g_weather + delta;
		if (w < -1) w = kWeatherCount - 1;
		if (w >= kWeatherCount) w = -1;
		g_weather = w;
	}
}
