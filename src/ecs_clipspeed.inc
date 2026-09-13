// =========================================================================
//  ecs_clipspeed.inc - the marker Speed row in 5% steps, 5% .. 1000%
// =========================================================================
//  A marker stores its playback speed as an INDEX into a nine-entry list
//  (5, 20, 35, 50, 100, 125, 150, 175, 200 percent), and the playback asks
//  the marker storage for "the speed at time t" through one vtable method
//  (+0x80) that converts that index with a switch - any index it does not
//  know gives 100%. Nothing else in the game converts the index (the
//  timeline's fast/slow icons use the same nine values inline; the labels
//  are text keys VEUI_SPEED_<n>).
//
//  So the index space is simply extended: 0..8 stay the game's own values,
//  and 9 + n means n * 5 percent. That method is hooked to answer for the
//  extended indices, the stock Speed row is stepped by us in 5% steps, and
//  the row is relabelled after every populate (the game would show a blank
//  for an index it does not know). A project saved with extended speeds
//  and opened WITHOUT this plugin plays those markers at 100% - it never
//  crashes, because the game's own switch defaults for them.
// =========================================================================
namespace clipspeed
{
	constexpr int kStock[9] = { 5, 20, 35, 50, 100, 125, 150, 175, 200 };
	constexpr int kMin = 5, kMax = 1000, kStep = 5;
	constexpr unsigned kStockOptionId = 0x25;   // the marker menu's Speed row

	int indexToPercent(int idx)
	{
		if (idx >= 0 && idx <= 8) return kStock[idx];
		if (idx >= 9)
		{
			int p = (idx - 9) * kStep;
			return p < kMin ? kMin : p > kMax ? kMax : p;
		}
		return 100;
	}

	int percentToIndex(int pct)
	{
		for (int i = 0; i < 9; ++i) if (kStock[i] == pct) return i;
		return 9 + pct / kStep;
	}

	// --- the playback lookup hook -------------------------------------------
	using FnSpeedAt = unsigned (__fastcall*)(void*, float);
	FnSpeedAt origSpeedAt = nullptr;
	uintptr_t a_SpeedAt = 0;
	bool g_hooked = false, g_gaveUp = false;
	int  g_lastLoggedPct = -1;

	// Plain C body for the __try.
	unsigned extended(void* self, float t, unsigned stock)
	{
		__try
		{
			const int mi = st::findAt(self, t);
			const void* m = st::tryGet(self, mi);
			if (!m) return stock;
			const int idx = mk::i32(m, mk::OFF_Speed);
			if (idx < 9) return stock;
			return (unsigned)indexToPercent(idx);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return stock; }
	}

	unsigned __fastcall hkSpeedAt(void* self, float t)
	{
		const unsigned stock = origSpeedAt(self, t);
		return extended(self, t, stock);
	}

	// The method's prologue on b3407: push rbx / sub rsp,30 / mov rax,[rcx]
	// / xor r9d,r9d. A different shape means the slot moved - then the stock
	// nine steps stay and nothing is hooked.
	const uint8_t kPrologue[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0x01, 0x45, 0x33, 0xC9 };

	// From the worker, once a clip is loaded (the storage pointer is null
	// before that and its vtable is where the method lives).
	void poll()
	{
		if (g_hooked || g_gaveUp || !g_cfg.clipSpeed) return;
		void* s = game::replayStorage();
		if (!s) return;
		uintptr_t fn = 0;
		__try
		{
			fn = *(uintptr_t*)(*(uint8_t**)s + st::VT_SpeedAt);
			if (fn < g_modBase || fn >= g_modBase + g_modSize || memcmp((const void*)fn, kPrologue, sizeof(kPrologue)) != 0) fn = 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { fn = 0; }
		if (!fn)
		{
			g_gaveUp = true;
			logf("clipspeed: the storage's speed lookup (vtable +0x80) does not look as expected - the Speed row keeps the game's 9 steps");
			return;
		}
		a_SpeedAt = fn;
		if (!hookFn(fn, (void*)hkSpeedAt, (void**)&origSpeedAt, "GetMarkerSpeedAtTime")) { g_gaveUp = true; return; }
		g_hooked = true;
		logf("clipspeed: GetMarkerSpeedAtTime hooked (rva 0x%llX) - the Speed row now steps 5%% from 5%% to 1000%%", rva(fn));
	}

	bool active() { return g_hooked && g_cfg.clipSpeed; }

	// --- stepping the marker under the menu ---------------------------------
	int currentPercent()
	{
		const void* m = game::currentMarker();
		if (!m) return -1;
		__try { return indexToPercent(mk::i32(m, mk::OFF_Speed)); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
	}

	bool writeIndex(void* m, int idx)
	{
		__try
		{
			mk::put(m, mk::OFF_Speed, &idx, 4);
			mk::markEdited(m, mk::BIT_Speed);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	// Returns the new percent, or -1 when there was nothing to step.
	int step(int delta)
	{
		void* m = game::currentMarker();
		if (!m) return -1;
		const int cur = currentPercent();
		if (cur < 0) return -1;
		int pct = cur + kStep * delta;
		if (pct < kMin) pct = kMin;
		if (pct > kMax) pct = kMax;
		if (pct == cur) return cur;
		if (!writeIndex(m, percentToIndex(pct))) return -1;
		int idx = -1;
		if (void* s = game::storageFor(m, &idx)) st::recalc(s, idx);   // re-times the later markers, as the game's own step does
		if (pct != g_lastLoggedPct) { g_lastLoggedPct = pct; logf("clipspeed: marker %d -> %d%% (index %d)", idx, pct, percentToIndex(pct)); }
		return pct;
	}

	const char* label(int pct, char* buf, int cap)
	{
		snprintf(buf, cap, "%d%%", pct);
		return buf;
	}
}
