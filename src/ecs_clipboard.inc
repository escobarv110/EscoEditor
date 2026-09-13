// =========================================================================
//  ecs_clipboard.inc - keyframe (marker) clipboard
// =========================================================================
//  Copy / Cut / Paste whole markers: camera, blend, shake, depth of field,
//  effects, audio, speed - the complete record. A range of markers goes in
//  one go (From .. To), and the clipboard is kept in EscoEditor.ini, so it
//  survives a clip change, a project change and a restart.
//
//  Paste lands ON the marker whose menu is open: that marker takes the
//  first copied marker's settings, and the remaining copied markers are
//  added after it with their original spacing (the game's own AddMarker,
//  which refuses a spot too close to another marker or the clip end - those
//  are reported, never forced). So "move keyframes somewhere else" is: add
//  a marker where they should start, open its menu, Paste. Cut is Copy plus
//  a note to remove the originals once the paste has landed (same clip
//  only - in another clip there is nothing to remove).
//
//  Pasting into ANOTHER clip resets the attach / look-at / focus target ids:
//  those are entity ids of the clip the markers were copied from and mean
//  something else, or nothing, elsewhere. Everything else carries over.
// =========================================================================
namespace clipboard
{
	constexpr int kMax = 32;

	struct Item
	{
		uint8_t rec[mk::SIZE];   // the whole sReplayMarkerInfo
		float   offsetMs;        // time after the first copied marker
	};

	Item  g_items[kMax];
	int   g_count = 0;
	void* g_sourceStorage = nullptr;   // where the items came from (clip identity)
	bool  g_cut = false;               // remove the originals on paste
	void* g_cutMarkers[kMax];          // the originals, by pointer
	int   g_cutCount = 0;

	// The range the menu shows, as marker indices; -1 = "the marker under
	// the menu" (resolved at copy time).
	int  g_from = -1, g_to = -1;
	char g_last[128] = {};

	constexpr const char* kSection = "Keyframes";

	// --- ini ---------------------------------------------------------------
	void save()
	{
		char key[16], buf[mk::SIZE * 2 + 40];
		char n[16]; sprintf_s(n, "%d", g_count);
		WritePrivateProfileStringA(kSection, "Count", n, g_ini);
		for (int i = 0; i < kMax; ++i)
		{
			sprintf_s(key, "M%d", i + 1);
			if (i >= g_count) { WritePrivateProfileStringA(kSection, key, nullptr, g_ini); continue; }
			int p = sprintf_s(buf, "%.1f ", g_items[i].offsetMs);
			for (int b = 0; b < (int)mk::SIZE; ++b) p += sprintf_s(buf + p, sizeof(buf) - p, "%02x", g_items[i].rec[b]);
			WritePrivateProfileStringA(kSection, key, buf, g_ini);
		}
	}

	void load()
	{
		g_count = 0;
		const int n = GetPrivateProfileIntA(kSection, "Count", 0, g_ini);
		for (int i = 0; i < n && i < kMax; ++i)
		{
			char key[16], buf[mk::SIZE * 2 + 40] = {};
			sprintf_s(key, "M%d", i + 1);
			GetPrivateProfileStringA(kSection, key, "", buf, sizeof(buf), g_ini);
			float off = 0.0f; int consumed = 0;
			if (sscanf_s(buf, "%f %n", &off, &consumed) != 1) break;
			const char* hex = buf + consumed;
			if (strlen(hex) < mk::SIZE * 2) break;
			Item& it = g_items[g_count];
			it.offsetMs = off;
			bool ok = true;
			for (int b = 0; b < (int)mk::SIZE && ok; ++b)
			{
				unsigned v = 0;
				if (sscanf_s(hex + b * 2, "%2x", &v) != 1) ok = false;
				it.rec[b] = (uint8_t)v;
			}
			if (!ok) break;
			++g_count;
		}
		g_sourceStorage = nullptr;   // another session: the source clip is unknown, treat every paste as cross-clip
		g_cut = false; g_cutCount = 0;
	}

	// --- the range -----------------------------------------------------------
	// Ordinary (non-anchor) marker indices of the clip under the menu, in order.
	int listMarkers(void* s, int* out, int cap)
	{
		int n = 0;
		const int count = st::count(s);
		for (int i = 0; i < count && n < cap; ++i)
		{
			const void* m = st::tryGet(s, i);
			if (!m) break;
			if (mk::u8(m, mk::OFF_Type) == mk::TYPE_ANCHOR) continue;
			out[n++] = i;
		}
		return n;
	}

	// The current marker's storage and index, and its ordinal among the
	// ordinary markers - what the From / To rows count in.
	struct Here { void* s = nullptr; int idx = -1; int ord = -1; int list[256]; int n = 0; };

	bool here(Here& h)
	{
		void* m = game::currentMarker();
		if (!m) return false;
		h.s = game::storageFor(m, &h.idx);
		if (!h.s) return false;
		h.n = listMarkers(h.s, h.list, 256);
		h.ord = -1;
		for (int i = 0; i < h.n; ++i) if (h.list[i] == h.idx) { h.ord = i; break; }
		return h.ord >= 0;
	}

	// From / To as ordinals. -1 means "this marker".
	void resolveRange(const Here& h, int* a, int* b)
	{
		int from = g_from < 0 ? h.ord : g_from;
		int to   = g_to   < 0 ? h.ord : g_to;
		if (from > h.n - 1) from = h.n - 1;
		if (to   > h.n - 1) to   = h.n - 1;
		if (from < 0) from = 0;
		if (to < from) to = from;
		*a = from; *b = to;
	}

	const char* rangeValue(bool fromRow, char* buf, int cap)
	{
		Here h;
		if (!here(h)) { snprintf(buf, cap, "-"); return buf; }
		int a, b; resolveRange(h, &a, &b);
		const int ord = fromRow ? a : b;
		const void* m = st::tryGet(h.s, h.list[ord]);
		float tf = 0.0f; if (m) memcpy(&tf, (const uint8_t*)m + mk::OFF_TimeMs, 4);
		snprintf(buf, cap, "%d of %d%s (%.1fs)", ord + 1, h.n, ord == h.ord ? " - this one" : "", tf / 1000.0f);
		return buf;
	}

	void stepRange(bool fromRow, int delta)
	{
		Here h;
		if (!here(h)) return;
		int a, b; resolveRange(h, &a, &b);
		if (fromRow) { a += delta; if (a < 0) a = 0; if (a > h.n - 1) a = h.n - 1; if (b < a) b = a; }
		else         { b += delta; if (b < 0) b = 0; if (b > h.n - 1) b = h.n - 1; if (a > b) a = b; }
		g_from = a; g_to = b;
	}

	void resetRange() { g_from = -1; g_to = -1; }

	// --- copy / cut ------------------------------------------------------------
	bool copy(bool cut)
	{
		Here h;
		if (!here(h)) { strcpy_s(g_last, "No marker under the menu."); return false; }
		int a, b; resolveRange(h, &a, &b);
		g_count = 0; g_cutCount = 0;
		float t0 = 0.0f;
		for (int o = a; o <= b && g_count < kMax; ++o)
		{
			const void* m = st::tryGet(h.s, h.list[o]);
			if (!m) break;
			__try
			{
				Item& it = g_items[g_count];
				memcpy(it.rec, m, mk::SIZE);
				float t; memcpy(&t, it.rec + mk::OFF_TimeMs, 4);
				if (g_count == 0) t0 = t;
				it.offsetMs = t - t0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { break; }
			if (cut) g_cutMarkers[g_cutCount++] = (void*)m;
			++g_count;
		}
		if (!g_count) { strcpy_s(g_last, "Nothing copied."); return false; }
		g_sourceStorage = h.s;
		g_cut = cut;
		save();
		snprintf(g_last, sizeof(g_last), "%s %d keyframe%s (%d to %d).%s", cut ? "Cut" : "Copied", g_count, g_count == 1 ? "" : "s",
			a + 1, b + 1, cut ? " They are removed when you Paste." : "");
		logf("clipboard: %s %d marker(s) %d..%d from storage %p", cut ? "cut" : "copied", g_count, a, b, h.s);
		return true;
	}

	// --- paste -----------------------------------------------------------------
	void writeRecord(void* m, const Item& it, float timeMs, bool sameClip)
	{
		uint8_t rec[mk::SIZE];
		memcpy(rec, it.rec, mk::SIZE);
		memcpy(rec + mk::OFF_TimeMs, &timeMs, 4);
		if (!sameClip)
		{
			const int32_t none = -1;
			for (uint32_t off : { 0x78u, 0x7Cu, 0x80u, 0x84u, 0x88u, 0x8Cu }) memcpy(rec + off, &none, 4);
		}
		rec[mk::OFF_Type] = mk::u8(m, mk::OFF_Type);   // never turn a marker into an anchor or back
		memcpy(m, rec, mk::SIZE);
	}

	bool paste()
	{
		if (!g_count) { strcpy_s(g_last, "Clipboard is empty - Copy first."); return false; }
		Here h;
		if (!here(h)) { strcpy_s(g_last, "No marker under the menu."); return false; }
		void* target = st::tryGet(h.s, h.idx);
		if (!target) { strcpy_s(g_last, "No marker under the menu."); return false; }
		const bool sameClip = (h.s == g_sourceStorage);
		float tBase = 0.0f; memcpy(&tBase, (const uint8_t*)target + mk::OFF_TimeMs, 4);

		int placed = 0, refused = 0;
		void* pasted[kMax] = {};
		__try
		{
			writeRecord(target, g_items[0], tBase, sameClip);
			pasted[placed++] = target;
			// Farthest first: each added marker lands in an empty stretch, and
			// the game sorts it into place.
			for (int i = g_count - 1; i >= 1; --i)
			{
				const float t = tBase + g_items[i].offsetMs;
				const int idx = st::canPlaceAt(h.s, t) ? st::addAt(h.s, t) : -1;
				if (idx < 0) { ++refused; continue; }
				void* m = st::tryGet(h.s, idx);
				if (!m) { ++refused; continue; }
				writeRecord(m, g_items[i], t, sameClip);
				pasted[placed++] = m;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { strcpy_s(g_last, "Paste failed."); return false; }

		// Cut: the originals go now (same clip only). Their pointers are
		// looked up again - adding markers may have moved indices, never the
		// records themselves. The target keeps its new contents even if it
		// was one of the originals.
		int removed = 0;
		if (g_cut && sameClip)
		{
			for (int i = g_cutCount - 1; i >= 0; --i)
			{
				void* m = g_cutMarkers[i];
				if (m == target) continue;
				const int idx = st::indexOf(h.s, m);
				if (idx <= 0 || idx >= st::count(h.s) - 1) continue;   // first / last are the game's, never removed
				if (st::remove(h.s, idx)) ++removed;
			}
			g_cut = false; g_cutCount = 0;
		}

		// The editor's own notifications: later markers re-timed for the new
		// speeds, and the director told about every pasted marker's blend.
		int tIdx = st::indexOf(h.s, target);
		if (tIdx < 0) tIdx = h.idx;
		st::recalc(h.s, tIdx > 0 ? tIdx : 1);
		const int count = st::count(h.s);
		for (int i = 0; i < count; ++i)
		{
			const void* m = st::tryGet(h.s, i);
			for (int k = 0; k < placed; ++k) if (pasted[k] == m) { st::blendUpdated(h.s, i); break; }
		}

		snprintf(g_last, sizeof(g_last), "Pasted %d of %d keyframe%s%s%s.", placed, g_count, g_count == 1 ? "" : "s",
			refused ? " - the rest did not fit (too close to a marker or the clip end)" : "",
			removed ? " - originals removed" : "");
		logf("clipboard: pasted %d/%d onto marker %d (%s clip), %d refused, %d originals removed",
			placed, g_count, h.idx, sameClip ? "same" : "other", refused, removed);
		return true;
	}

	const char* summary(char* buf, int cap)
	{
		if (!g_count) snprintf(buf, cap, "empty");
		else snprintf(buf, cap, "%d keyframe%s%s", g_count, g_count == 1 ? "" : "s", g_cut ? " (cut)" : "");
		return buf;
	}
}
