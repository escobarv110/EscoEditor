// =========================================================================
//  ecs_dof.inc - depth of field: copy / paste / apply to all
// =========================================================================
//  A keyframe's Depth of Field lives in its marker record:
//    +0xAF mode        0 Default, 1 Custom, 2 None. The order is the game's:
//                      PopulateDofMenu (rva 0x1737FC) adds the Intensity row
//                      for 0 and 1, the focus rows for 1 only and nothing
//                      for 2, and its labels are VEUI_DOF_DEFAULT, _CUSTOM,
//                      _NONE in that order.
//    +0xB0 focus mode  0 Auto, 1 Manual, 2 Target (the Focal Distance row
//                      appears for 1, the Target row for 2)
//    +0x5C focal distance, +0x60 intensity 1..10, +0x88 the focus target (an
//    entity id of this clip). The game's menu sets edited bits 26 mode,
//    27 focus mode, 28 focal distance, 30 intensity. Every keyframe keeps
//    its own, saved with the project, and the game renders it as it is:
//    nothing in EscoEditor changes how depth of field looks.
//
//  COPY / PASTE / TO ALL move those fields between keyframes: the three
//  rows at the bottom of the Depth of Field submenu (ecs_menu.inc). The
//  copied values are kept in EscoEditor.ini, [DepthOfField] Clip=.
// =========================================================================
namespace dof
{
	constexpr uint32_t OFF_FocalDist = 0x5C, OFF_Intensity = 0x60, OFF_TargetId = 0x88, OFF_TargetIdx = 0x8C;
	constexpr uint32_t OFF_Mode = 0xAF, OFF_FocusMode = 0xB0;
	constexpr int BIT_Mode = 26, BIT_FocusMode = 27, BIT_FocalDist = 28, BIT_Intensity = 30;
	constexpr uint8_t MODE_DEFAULT = 0, MODE_CUSTOM = 1, MODE_NONE = 2;
	constexpr uint8_t FOCUS_AUTO = 0, FOCUS_MANUAL = 1, FOCUS_TARGET = 2;

	const char* modeName(uint8_t m)  { return m == MODE_DEFAULT ? "Default" : m == MODE_CUSTOM ? "Custom" : m == MODE_NONE ? "None" : "unknown"; }
	const char* focusName(uint8_t f) { return f == FOCUS_AUTO ? "auto" : f == FOCUS_MANUAL ? "manual" : f == FOCUS_TARGET ? "target" : "unknown"; }

	// --- the copied DOF -------------------------------------------------------
	struct Clip { bool used = false; uint8_t mode = 0, focusMode = 0; float focalDist = 0, intensity = 0; int32_t targetId = -1, targetIdx = -1; };
	Clip  g_clip;
	void* g_clipStorage = nullptr;
	char  g_last[96] = {};
	constexpr const char* kSection = "DepthOfField";

	void save()
	{
		char b[128];
		if (!g_clip.used) { WritePrivateProfileStringA(kSection, "Clip", nullptr, g_ini); return; }
		sprintf_s(b, "%d %d %.4f %.4f", g_clip.mode, g_clip.focusMode, g_clip.focalDist, g_clip.intensity);
		WritePrivateProfileStringA(kSection, "Clip", b, g_ini);
	}

	void load()
	{
		char b[128] = {};
		GetPrivateProfileStringA(kSection, "Clip", "", b, sizeof(b), g_ini);
		int m, f; float dd, i;
		if (sscanf_s(b, "%d %d %f %f", &m, &f, &dd, &i) == 4 && isfinite(dd) && isfinite(i))
		{
			g_clip.used = true; g_clip.mode = (uint8_t)m; g_clip.focusMode = (uint8_t)f; g_clip.focalDist = dd; g_clip.intensity = i;
			g_clip.targetId = -1; g_clip.targetIdx = -1;
		}
	}

	bool readFrom(const void* m, Clip& c)
	{
		__try
		{
			c.mode = mk::u8(m, OFF_Mode); c.focusMode = mk::u8(m, OFF_FocusMode);
			memcpy(&c.focalDist, (const uint8_t*)m + OFF_FocalDist, 4);
			memcpy(&c.intensity, (const uint8_t*)m + OFF_Intensity, 4);
			c.targetId = mk::i32(m, OFF_TargetId); c.targetIdx = mk::i32(m, OFF_TargetIdx);
			c.used = isfinite(c.focalDist) && isfinite(c.intensity);
			return c.used;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	bool writeTo(void* m, const Clip& c, bool withTarget)
	{
		__try
		{
			mk::put(m, OFF_Mode, &c.mode, 1);           mk::markEdited(m, BIT_Mode);
			mk::put(m, OFF_FocusMode, &c.focusMode, 1); mk::markEdited(m, BIT_FocusMode);
			mk::put(m, OFF_FocalDist, &c.focalDist, 4); mk::markEdited(m, BIT_FocalDist);
			mk::put(m, OFF_Intensity, &c.intensity, 4); mk::markEdited(m, BIT_Intensity);
			const int32_t id = withTarget ? c.targetId : -1, ix = withTarget ? c.targetIdx : -1;
			mk::put(m, OFF_TargetId, &id, 4); mk::put(m, OFF_TargetIdx, &ix, 4);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	bool copy()
	{
		void* m = game::currentMarker();
		if (!m) { strcpy_s(g_last, "No keyframe."); return false; }
		int idx = -1;
		void* s = game::storageFor(m, &idx);
		Clip c;
		if (!readFrom(m, c)) { strcpy_s(g_last, "Could not read it."); return false; }
		g_clip = c; g_clipStorage = s;
		save();
		strcpy_s(g_last, "Copied.");
		logf("dof: copied from marker %d (mode %s focus %s dist %.2f int %.2f target %d)", idx, modeName(c.mode), focusName(c.focusMode), c.focalDist, c.intensity, c.targetId);
		return true;
	}

	bool paste()
	{
		if (!g_clip.used) { strcpy_s(g_last, "Nothing copied yet."); return false; }
		void* m = game::currentMarker();
		if (!m) { strcpy_s(g_last, "No keyframe."); return false; }
		int idx = -1;
		void* s = game::storageFor(m, &idx);
		const bool sameClip = s && s == g_clipStorage;
		if (!writeTo(m, g_clip, sameClip)) { strcpy_s(g_last, "Could not write it."); return false; }
		strcpy_s(g_last, "Pasted.");
		logf("dof: pasted onto marker %d (%s clip)", idx, sameClip ? "same" : "other");
		return true;
	}

	bool applyToAll()
	{
		void* m = game::currentMarker();
		if (!m) { strcpy_s(g_last, "No keyframe."); return false; }
		int idx = -1;
		void* s = game::storageFor(m, &idx);
		if (!s) { strcpy_s(g_last, "No clip."); return false; }
		Clip c;
		if (!readFrom(m, c)) { strcpy_s(g_last, "Could not read it."); return false; }
		int n = 0;
		const int count = st::count(s);
		for (int i = 0; i < count; ++i)
		{
			void* o = st::tryGet(s, i);
			if (!o || o == m || mk::u8(o, mk::OFF_Type) == mk::TYPE_ANCHOR) continue;
			if (writeTo(o, c, true)) ++n;
		}
		snprintf(g_last, sizeof(g_last), "Done: %d keyframe%s.", n, n == 1 ? "" : "s");
		logf("dof: applied marker %d's DOF to %d others", idx, n);
		return true;
	}
}
