// =========================================================================
//  ecs_game.inc - the replay marker record, the marker storage interface,
//  quaternions, the replay director / free camera layout, and the game
//  functions and globals the features need.
// =========================================================================
//  Every offset below was verified on FiveM's b3407 image (decrypted
//  executable snapshot) against Rockstar Editor+'s documented layout and, for
//  the speed and marker code, by reading the game's own functions:
//
//    IReplayMarkerStorage::IncrementMarkerSpeed  (vtable +0x70, rva 0x55C604)
//        idx = marker+0x90 + delta; wraps 0..8; marker+0x00 |= 2; then
//        RecalcPositionsOnSpeedChange-style refresh
//    IReplayMarkerStorage vtable +0x80  (rva 0x557330)
//        GetMarkerSpeedAtTime(float t): finds the marker at t through vtable
//        +0x138, reads its +0x90 INDEX and converts it with a switch:
//        0->5 1->20 2->35 3->50 4->100 5->125 6->150 7->175 8->200, anything
//        else -> 100. It is the only conversion the playback uses, which is
//    CVideoEditorPlayback::GetCurrentEditMarker  (rva 0x17B54C)
//        GetCurrentClip()->vtable[+0xB0](g_EditMarkerIndex)
// =========================================================================

// ------------------------------------------------------------ hooking
// MinHook freezes threads to patch, and its thread snapshot can lose a race
// on a busy process (reported as MH_ERROR_MEMORY_ALLOC). Retry.
volatile LONG g_hookStalls = 0;         // hooks that could not be enabled yet
extern "C" BOOL g_mhFreezeOptional;     // MinHook (hook.c): hook without pausing threads

// Every hook we place, so they can all be taken out and put back when
// Rockstar Editor Plus needs the game's own bytes to find its signatures.
struct Placed { uintptr_t target; void* detour; void** orig; const char* what; };
Placed g_placed[16] = {};
int    g_placedCount = 0;

bool hookFn(uintptr_t target, void* detour, void** orig, const char* what)
{
	const MH_STATUS init = MH_Initialize();
	if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
	{
		logf("hook: MH_Initialize failed: %s", MH_StatusToString(init));
		return false;
	}
	const MH_STATUS cr = MH_CreateHook((void*)target, detour, orig);
	if (cr != MH_OK && cr != MH_ERROR_ALREADY_CREATED)   // already created = this is one of our own retries
	{
		logf("hook: MH_CreateHook(%s) failed: %s", what, MH_StatusToString(cr));
		return false;
	}
	MH_STATUS en = MH_OK;
	for (int tries = 0; tries < 40; ++tries)
	{
		en = MH_EnableHook((void*)target);
		if (en != MH_ERROR_MEMORY_ALLOC) break;
		Sleep(25);
	}
	if (en != MH_OK)
	{
		InterlockedIncrement(&g_hookStalls);
		if (en == MH_ERROR_MEMORY_ALLOC)
			logf("hook: %s is not in yet - the game would not hand over its thread list, which MinHook needs (it is still starting). "
				 "The worker keeps trying for two minutes.", what);
		else
			logf("hook: MH_EnableHook(%s) failed: %s", what, MH_StatusToString(en));
		return false;
	}
	if (g_placedCount < 16)
	{
		g_placed[g_placedCount++] = { target, detour, orig, what };
	}
	return true;
}

// All of our hooks out of the way, so another plugin scanning for the game's
// own first bytes finds them. Returns how many came out.
int unhookAll()
{
	int n = 0;
	for (int i = 0; i < g_placedCount; ++i)
	{
		MH_DisableHook((void*)g_placed[i].target);
		if (MH_RemoveHook((void*)g_placed[i].target) == MH_OK) ++n;
	}
	return n;
}

// And back in. Created afresh rather than merely re-enabled: whatever is at the
// target now - another plugin's jump, most likely - is what the new trampoline
// is built from, so calling the original runs their hook and then the game's
// code. Re-enabling the old one would write over their jump and silence them.
int rehookAll()
{
	int n = 0;
	for (int i = 0; i < g_placedCount; ++i)
	{
		const Placed& p = g_placed[i];
		if (MH_CreateHook((void*)p.target, p.detour, p.orig) != MH_OK) continue;
		if (MH_EnableHook((void*)p.target) == MH_OK) ++n;
	}
	return n;
}

// ---------------------------------------------------- sReplayMarkerInfo
namespace mk
{
	constexpr uint32_t OFF_Edited      = 0x00;  // u64 edited-property bits
	constexpr uint32_t OFF_Quat        = 0x08;  // u64 packed free-camera orientation
	constexpr uint32_t OFF_AttachQuat  = 0x10;
	constexpr uint32_t OFF_AttachPos   = 0x18;
	constexpr uint32_t OFF_Pos         = 0x24;  // float[3] free-camera world position
	constexpr uint32_t OFF_PosRel      = 0x30;
	constexpr uint32_t OFF_TimeMs      = 0x70;
	constexpr uint32_t OFF_AttachId    = 0x78;  // s32, -1 none
	constexpr uint32_t OFF_LookAtId    = 0x80;  // s32, -1 none
	constexpr uint32_t OFF_Speed       = 0x90;  // s32 INDEX into the speed list (see above)
	constexpr uint32_t OFF_Type        = 0xA0;  // u8, 2 = anchor
	constexpr uint32_t OFF_CamType     = 0xA1;  // u8, 6 = free camera
	constexpr uint32_t OFF_Fov         = 0xA3;  // u8 degrees
	constexpr uint32_t OFF_CamValid    = 0xB2;  // bool: pos/quat/fov above are authored
	constexpr uint32_t SIZE            = 0xB8;

	// A marker keeps its playback speed as an index into these nine values.
	// The game's own switch turns the index into a percentage; anything it
	// does not know plays at 100.
	constexpr int kSpeedPercent[9] = { 5, 20, 35, 50, 100, 125, 150, 175, 200 };
	inline int speedPercent(int idx) { return (idx >= 0 && idx < 9) ? kSpeedPercent[idx] : 100; }

	constexpr int     BIT_Speed   = 1;
	constexpr int     BIT_CamType = 10;
	constexpr uint8_t CAM_FREE    = 6;
	constexpr uint8_t TYPE_ANCHOR = 2;

	inline int32_t  i32(const void* m, uint32_t off) { int32_t v; memcpy(&v, (const uint8_t*)m + off, 4); return v; }
	inline uint8_t  u8 (const void* m, uint32_t off) { return *((const uint8_t*)m + off); }
	inline uint64_t u64(const void* m, uint32_t off) { uint64_t v; memcpy(&v, (const uint8_t*)m + off, 8); return v; }
	inline void     put(void* m, uint32_t off, const void* src, size_t n) { memcpy((uint8_t*)m + off, src, n); }
	inline void     markEdited(void* m, int bit) { uint64_t f = u64(m, OFF_Edited); f |= (1ull << bit); put(m, OFF_Edited, &f, 8); }
}

// ------------------------------------------------ IReplayMarkerStorage
// A pure interface; slots confirmed against real call sites (see the file
// header and RE+'s marker.h). Every call is SEH-wrapped: a wrong slot must be
// a log line, not a dead game.
namespace st
{
	constexpr uint32_t VT_AddAtTime  = 0x20;   // bool (bool sort, bool inherit, float timeMs, int* outIdx) - slot 3 (+0x18) is
	                                           // this with the replay clock as the time; verified at rva 0x5186A8
	constexpr uint32_t VT_Remove     = 0x28;   // void (u32 idx) - refuses idx 0, the last one, and a clip with <= 2 markers
	constexpr uint32_t VT_CanPlace   = 0x38;   // bool (float timeMs, bool ignoreBoundary)
	constexpr uint32_t VT_IncSpeed   = 0x70;   // void (u32 idx, s32 delta)
	constexpr uint32_t VT_SpeedAt    = 0x80;   // u32 (float timeMs)          the playback's speed lookup
	constexpr uint32_t VT_Recalc     = 0x88;   // void (u32 fromIdx)
	constexpr uint32_t VT_TryGet     = 0xA8;   // sReplayMarkerInfo* (u32 idx)
	constexpr uint32_t VT_Count      = 0x108;  // s32 ()
	constexpr uint32_t VT_FindAt     = 0x138;  // s32 (float t, int mode, float* out, void* exclude, bool skipAnchors)
	constexpr uint32_t VT_BlendUpd   = 0x168;  // void (u32 idx)

	using FnGet    = void* (__fastcall*)(void*, unsigned);
	using FnCount  = int   (__fastcall*)(void*);
	using FnIdx    = void  (__fastcall*)(void*, unsigned);
	using FnFindAt = int   (__fastcall*)(void*, float, int, float*, void*, bool);
	using FnAddAt  = bool  (__fastcall*)(void*, bool, bool, float, int*);
	using FnCanAt  = bool  (__fastcall*)(void*, float, bool);

	template <typename Fn> Fn vfn(void* s, uint32_t off)
	{
		return (Fn)(*(uintptr_t*)(*(uint8_t**)s + off));
	}

	void* tryGet(void* s, int idx)
	{
		if (!s || idx < 0) return nullptr;
		__try { return vfn<FnGet>(s, VT_TryGet)(s, (unsigned)idx); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
	}

	int count(void* s)
	{
		if (!s) return 0;
		__try { return vfn<FnCount>(s, VT_Count)(s); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
	}

	bool recalc(void* s, int idx)
	{
		if (!s || idx < 0) return false;
		__try { vfn<FnIdx>(s, VT_Recalc)(s, (unsigned)idx); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	bool blendUpdated(void* s, int idx)
	{
		if (!s || idx < 0) return false;
		__try { vfn<FnIdx>(s, VT_BlendUpd)(s, (unsigned)idx); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	int findAt(void* s, float t)
	{
		if (!s) return -1;
		__try { return vfn<FnFindAt>(s, VT_FindAt)(s, t, 1, nullptr, nullptr, true); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
	}

	// Index of a marker pointer inside a storage, -1 if it is not there.
	int indexOf(void* s, const void* m)
	{
		const int n = count(s);
		if (n <= 0 || n > 4096) return -1;
		for (int i = 0; i < n; ++i) if (tryGet(s, i) == m) return i;
		return -1;
	}

	bool canPlaceAt(void* s, float timeMs)
	{
		if (!s) return false;
		__try { return vfn<FnCanAt>(s, VT_CanPlace)(s, timeMs, false); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	// Adds a marker at `timeMs` with default settings, sorted into place.
	// Returns its index, -1 when the game refused (too close to another
	// marker or the clip bounds, or the marker limit).
	int addAt(void* s, float timeMs)
	{
		if (!s) return -1;
		int idx = -1;
		bool ok = false;
		__try { ok = vfn<FnAddAt>(s, VT_AddAtTime)(s, true, false, timeMs, &idx); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
		return ok ? idx : -1;
	}

	bool remove(void* s, int idx)
	{
		if (!s || idx <= 0) return false;
		__try { vfn<FnIdx>(s, VT_Remove)(s, (unsigned)idx); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	// The next / previous ordinary marker (anchors skipped), -1 at the end.
	int neighbour(void* s, int from, int dir)
	{
		const int n = count(s);
		for (int i = from + dir; i >= 0 && i < n; i += dir)
		{
			const void* m = tryGet(s, i);
			if (!m) return -1;
			if (mk::u8(m, mk::OFF_Type) == mk::TYPE_ANCHOR) continue;
			return i;
		}
		return -1;
	}
}

// ---------------------------------------------------------- quaternions
// The game's smallest-three packing (2-bit largest index, three 20-bit
// components over [-1/sqrt2, +1/sqrt2]) and the conversions to and from the
// RAGE Matrix34 basis rows (a = right, b = forward, c = up). Ported from
// RE+'s quat.h, which round-tripped them against real marker values.
namespace rq
{
	struct Quat { float x, y, z, w; };

	inline Quat normalized(Quat q)
	{
		const float n = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
		if (n < 1e-8f) return { 0, 0, 0, 1 };
		const float inv = 1.0f / n;
		return { q.x * inv, q.y * inv, q.z * inv, q.w * inv };
	}

	inline Quat decode(uint64_t packed)
	{
		constexpr uint64_t MASK20 = (1ull << 20) - 1;
		const float k = 1.0f / sqrtf(2.0f);
		auto deq = [&](uint64_t v) { return (float)v / (float)((1u << 20) - 1u) * (2.0f * k) - k; };
		const uint64_t largestIdx = packed >> 60;
		float o[3] = { deq((packed >> 40) & MASK20), deq((packed >> 20) & MASK20), deq(packed & MASK20) };
		const float t2 = o[0] * o[0] + o[1] * o[1] + o[2] * o[2];
		const float largest = sqrtf(t2 >= 1.0f ? 0.0f : 1.0f - t2);
		switch (largestIdx)
		{
		case 0:  return { largest, o[0], o[1], o[2] };
		case 1:  return { o[0], largest, o[1], o[2] };
		case 2:  return { o[0], o[1], largest, o[2] };
		default: return { o[0], o[1], o[2], largest };
		}
	}

	inline uint64_t encode(Quat in)
	{
		const Quat q = normalized(in);
		const float c[4] = { q.x, q.y, q.z, q.w };
		int largest = 0; float la = fabsf(c[0]);
		for (int i = 1; i < 4; ++i) if (fabsf(c[i]) > la) { la = fabsf(c[i]); largest = i; }
		const float sign = c[largest] < 0.0f ? -1.0f : 1.0f;
		const float k = 1.0f / sqrtf(2.0f);
		auto quant = [&](float v) -> uint64_t {
			float f = (v * sign + k) / (2.0f * k) * (float)((1u << 20) - 1u);
			if (f < 0.0f) f = 0.0f;
			if (f > (float)((1u << 20) - 1u)) f = (float)((1u << 20) - 1u);
			return (uint64_t)f;
		};
		float others[3]; int n = 0;
		for (int i = 0; i < 4; ++i) if (i != largest) others[n++] = c[i];
		uint64_t out = (uint64_t)largest << 60;
		out |= quant(others[0]) << 40;
		out |= quant(others[1]) << 20;
		out |= quant(others[2]);
		return out;
	}

	inline void toRows(Quat q, float* a, float* b, float* c)
	{
		const Quat n = normalized(q);
		const float xx = n.x * n.x, yy = n.y * n.y, zz = n.z * n.z;
		const float xy = n.x * n.y, xz = n.x * n.z, yz = n.y * n.z;
		const float wx = n.w * n.x, wy = n.w * n.y, wz = n.w * n.z;
		a[0] = 1.0f - 2.0f * (yy + zz); a[1] = 2.0f * (xy + wz);        a[2] = 2.0f * (xz - wy);
		b[0] = 2.0f * (xy - wz);        b[1] = 1.0f - 2.0f * (xx + zz); b[2] = 2.0f * (yz + wx);
		c[0] = 2.0f * (xz + wy);        c[1] = 2.0f * (yz - wx);        c[2] = 1.0f - 2.0f * (xx + yy);
	}

	inline Quat fromRows(const float* a, const float* b, const float* c)
	{
		const float trace = a[0] + b[1] + c[2];
		if (trace > 0.0f)
		{
			const float s = 2.0f * sqrtf(trace + 1.0f);
			return normalized({ (b[2] - c[1]) / s, (c[0] - a[2]) / s, (a[1] - b[0]) / s, 0.25f * s });
		}
		if (a[0] > b[1] && a[0] > c[2])
		{
			const float s = 2.0f * sqrtf(1.0f + a[0] - b[1] - c[2]);
			return normalized({ 0.25f * s, (a[1] + b[0]) / s, (a[2] + c[0]) / s, (b[2] - c[1]) / s });
		}
		if (b[1] > c[2])
		{
			const float s = 2.0f * sqrtf(1.0f - a[0] + b[1] - c[2]);
			return normalized({ (a[1] + b[0]) / s, 0.25f * s, (b[2] + c[1]) / s, (c[0] - a[2]) / s });
		}
		const float s = 2.0f * sqrtf(1.0f - a[0] - b[1] + c[2]);
		return normalized({ (a[2] + c[0]) / s, (b[2] + c[1]) / s, 0.25f * s, (a[1] - b[0]) / s });
	}

	// Is this a rotation basis? A guard on OUR offsets, not on the game's data.
	inline bool validBasis(const float* a, const float* b, const float* c)
	{
		auto fin = [](const float* v) { return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]); };
		auto len2 = [](const float* v) { return v[0] * v[0] + v[1] * v[1] + v[2] * v[2]; };
		auto dot = [](const float* u, const float* v) { return u[0] * v[0] + u[1] * v[1] + u[2] * v[2]; };
		if (!fin(a) || !fin(b) || !fin(c)) return false;
		for (const float* v : { a, b, c }) { const float l = len2(v); if (l < 0.98f || l > 1.02f) return false; }
		return fabsf(dot(a, b)) < 0.02f && fabsf(dot(b, c)) < 0.02f && fabsf(dot(a, c)) < 0.02f;
	}
}

// ------------------------------------- camReplayDirector / free camera
namespace rd
{
	constexpr uint32_t DIR_AuthorPos    = 0x060;  // authoring camFrame position
	constexpr uint32_t DIR_RenderRowA   = 0x1F0;  // rendered camFrame basis rows
	constexpr uint32_t DIR_RenderRowB   = 0x200;
	constexpr uint32_t DIR_RenderRowC   = 0x210;
	constexpr uint32_t DIR_RenderPos    = 0x220;
	constexpr uint32_t DIR_RenderFov    = 0x250;
	constexpr uint32_t DIR_ActiveCamera = 0x2C0;
	constexpr uint32_t DIR_FreeCam0     = 0x4D0;
	constexpr uint32_t DIR_FreeCam1     = 0x4D8;
	constexpr uint32_t DIR_CurMarker    = 0x630;  // s32

	constexpr uint32_t CAM_RowA         = 0x030;  // camera frame basis rows (right / forward / up)
	constexpr uint32_t CAM_RowB         = 0x040;
	constexpr uint32_t CAM_RowC         = 0x050;
	constexpr uint32_t CAM_FramePos     = 0x060;
	constexpr uint32_t CAM_Fov          = 0x090;
	constexpr uint32_t CAM_MarkerIndex  = 0x238;  // s32
	constexpr uint32_t CAM_Metadata     = 0x250;
	constexpr uint32_t CAM_AttachRowA   = 0x3E0;  // m_AttachEntityMatrix: the parent's frame as the game built it
	constexpr uint32_t CAM_AttachRowB   = 0x3F0;  //   for this marker's attach type (full / heading / position only)
	constexpr uint32_t CAM_AttachRowC   = 0x400;
	constexpr uint32_t CAM_AttachPos    = 0x410;
	constexpr uint32_t CAM_Position     = 0x480;  // the camera's own position (world when not attached)
	constexpr uint32_t CAM_AttachEntity = 0x4C0;
	constexpr uint32_t CAM_LookAtEntity = 0x4C8;

	constexpr uint32_t DIR_AuthorRowA   = 0x030;  // authoring camFrame basis rows and fov
	constexpr uint32_t DIR_AuthorRowB   = 0x040;
	constexpr uint32_t DIR_AuthorRowC   = 0x050;
	constexpr uint32_t DIR_AuthorFov    = 0x090;

	constexpr uint32_t ENT_RowA         = 0x060;  // CEntity world matrix rows
	constexpr uint32_t ENT_RowB         = 0x070;
	constexpr uint32_t ENT_RowC         = 0x080;
	constexpr uint32_t ENT_Position     = 0x090;

	// The director the UpdateSmoothing hook last ran on (see ecs_director.inc).
	uint8_t* volatile g_lastDirector = nullptr;

	inline float* f3(uint8_t* p, uint32_t off) { return (float*)(p + off); }

	// The director keeps two free cameras and blends between them; the one
	// for a marker is found by its marker index, as the game's own steppers do.
	uint8_t* freeCamForMarker(uint8_t* dir, int markerIdx)
	{
		if (!dir || markerIdx < 0) return nullptr;
		for (uint32_t off : { DIR_FreeCam0, DIR_FreeCam1 })
		{
			uint8_t* cam = *(uint8_t**)(dir + off);
			if (cam && *(int32_t*)(cam + CAM_MarkerIndex) == markerIdx) return cam;
		}
		return nullptr;
	}

	uint8_t* anyFreeCam(uint8_t* dir)
	{
		if (!dir) return nullptr;
		for (uint32_t off : { DIR_FreeCam0, DIR_FreeCam1 })
			if (uint8_t* cam = *(uint8_t**)(dir + off)) return cam;
		return nullptr;
	}
}

// ------------------------------------------------- functions & globals
namespace game
{
	using FnGetMarker = void* (__fastcall*)();
	using FnGetClip   = void* (__fastcall*)();

	// CVideoEditorPlayback::PopulateCameraMenu - the function the Scaleform
	// plumbing (ecs_menu.inc) and GetCurrentEditMarker are derived out of.
	const char* kPcm =
		"48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 54 41 55 41 56 41 57 "
		"48 8B EC 48 83 EC 50 8B 05 ? ? ? ? 41 83 CC FF 8B F1 44 39 25";
	// camReplayDirector::GetNextMarkerIndex - its prologue loads
	// g_ReplayMarkerStorage (mov rdi,[rip+d] at +0x19) and calls
	// GetMarkerCount through the vtable at +0x108, which pins the interface.
	const char* kGnmi =
		"48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 20 "
		"48 8B 3D ? ? ? ? 44 8B F1 48 8B 07 48 8B CF FF 90 08 01 00 00";
	// CPacketWeather::Extract - one call per played frame with the frame's
	// weather packet (see ecs_scene.inc).
	const char* kWeather =
		"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 60 "
		"8A 05 ? ? ? ? 0F 57 C0 48 8B D9 0F 28 C8 38 41 07 72 10 "
		"F3 0F 10 81 14 01 00 00";
	// The timecycle variable table (data): entry 0 light_dir_col_r 0.89 and
	// entry 1 light_dir_col_g 0.675, 32 bytes each, +0x18 replay-override flag.
	const char* kTcTable =
		"00 00 00 00 00 00 00 00 ? ? ? ? ? ? ? ? 0A D7 63 3F 04 00 00 00 "
		"01 00 00 00 02 00 00 00 01 00 00 00 00 00 00 00 ? ? ? ? ? ? ? ? "
		"CD CC 2C 3F 00 00 00 00 01 00 00 00 02 00 00 00";

	constexpr uint8_t OP_MOV_EDX[]     = { 0x8B, 0x15 };
	constexpr uint8_t OP_MOV_RDI_MEM[] = { 0x48, 0x8B, 0x3D };
	constexpr uint8_t OP_CVTTSS2SI[]   = { 0xF3, 0x48, 0x0F, 0x2C, 0x0D };

	// CVideoEditorPlayback::Open(type, clip) opens by storing "a playback is
	// active" and its type; type 1 is BAKE - the game's own Export, the one
	// Extended Video Export captures at Present. Pattern and offsets after
	// Rockstar Editor+'s notes; the opcodes are checked on the running image.
	const char* kPlaybackOpen =
		"40 53 48 83 EC 20 48 8B 05 ? ? ? ? 89 15 ? ? ? ? 4C 8D 05 ? ? ? ? 48 F7 D8 C6 05 ? ? ? ? 01 89 0D";
	constexpr uint8_t OP_MOV_MEM_ECX[]  = { 0x89, 0x0D };
	constexpr uint8_t OP_MOV_MEM_BYTE[] = { 0xC6, 0x05 };
	constexpr int     PLAYBACK_BAKE = 1;
	uintptr_t a_PlaybackOpen = 0, a_PlaybackType = 0, a_PlaybackActive = 0;

	uintptr_t a_PopulateCameraMenu = 0, a_GetCurrentEditMarker = 0, a_GetCurrentClip = 0;
	uintptr_t a_EditMarkerIndex = 0, a_GetNextMarkerIndex = 0, a_MarkerStorage = 0;
	uintptr_t a_WeatherExtract = 0, a_TcTable = 0, a_ReplayTimeMs = 0;
	bool g_resolved = false;

	// The replay clock (float ms, non-dilated), read inside UpdateSmoothing as
	// `cvttss2si rcx,[rip+d]` at +0x12C; the same address the game's own
	// AddMarker(here) passes as the time. Set by director::install().
	void resolveReplayTime(uintptr_t updateSmoothing)
	{
		if (a_ReplayTimeMs || !updateSmoothing) return;
		a_ReplayTimeMs = derive(updateSmoothing, 0x12C, 0x131, OP_CVTTSS2SI, 5, "g_ReplayTimeMs");
	}

	float replayTimeMs()
	{
		if (!a_ReplayTimeMs) return -1.0f;
		__try { return *(volatile float*)a_ReplayTimeMs; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return -1.0f; }
	}

	uintptr_t scanFn(const char* sig, const char* what)
	{
		bool hooked = false;
		const uint8_t* hit = scanFnBytes(sig, &hooked);
		logf("  %-22s %s rva 0x%llX%s", what, hit ? "at" : "NOT FOUND -", rva((uintptr_t)hit),
			hooked ? "  (already hooked by another plugin - ours chains on top)" : "");
		return (uintptr_t)hit;
	}

	void resolve()
	{
		if (g_resolved || isEnhanced()) return;
		g_resolved = true;
		logf("game: resolving the marker functions");
		a_PopulateCameraMenu = scanFn(kPcm, "PopulateCameraMenu");
		if (a_PopulateCameraMenu)
		{
			a_GetCurrentEditMarker = derive(a_PopulateCameraMenu, 0x49, 0x4A, OP_CALL, 1, "GetCurrentEditMarker");
			if (a_GetCurrentEditMarker)
			{
				a_GetCurrentClip   = derive(a_GetCurrentEditMarker, 0x04, 0x05, OP_CALL,    1, "GetCurrentClip");
				a_EditMarkerIndex  = derive(a_GetCurrentEditMarker, 0x16, 0x18, OP_MOV_EDX, 2, "g_EditMarkerIndex");
			}
		}
		a_GetNextMarkerIndex = scanFn(kGnmi, "GetNextMarkerIndex");
		if (a_GetNextMarkerIndex)
			a_MarkerStorage = derive(a_GetNextMarkerIndex, 0x19, 0x1C, OP_MOV_RDI_MEM, 3, "g_ReplayMarkerStorage");
		a_WeatherExtract = scanFn(kWeather, "CPacketWeather::Extract");
		a_TcTable        = scanFn(kTcTable, "TimecycleVarTable");
		a_PlaybackOpen   = scanFn(kPlaybackOpen, "PlaybackOpen");
		if (a_PlaybackOpen)
		{
			a_PlaybackType = derive(a_PlaybackOpen, 0x24, 0x26, OP_MOV_MEM_ECX, 2, "g_PlaybackType");
			// `mov byte [rip+d], 1`: the displacement is followed by the 1, so the
			// next instruction - what d counts from - starts one byte later
			if (!memcmp((const void*)(a_PlaybackOpen + 0x1D), OP_MOV_MEM_BYTE, 2) && *(const uint8_t*)(a_PlaybackOpen + 0x23) == 1)
			{
				int32_t d = 0;
				memcpy(&d, (const void*)(a_PlaybackOpen + 0x1F), 4);
				const uintptr_t a = a_PlaybackOpen + 0x24 + (intptr_t)d;
				if (a >= g_modBase && a < g_modBase + g_modSize) { a_PlaybackActive = a; logf("  %-22s rva 0x%llX", "g_PlaybackActive", rva(a)); }
			}
		}
	}

	// The game's own Export is running: its playback is open as BAKE.
	bool exporting()
	{
		if (!a_PlaybackType) return false;
		__try
		{
			const bool active = a_PlaybackActive ? *(volatile const uint8_t*)a_PlaybackActive != 0 : true;
			return active && *(volatile const int*)a_PlaybackType == PLAYBACK_BAKE;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	// The marker whose menu is open (top-level marker menu or a submenu).
	void* currentMarker()
	{
		if (!a_GetCurrentEditMarker) return nullptr;
		__try { return ((FnGetMarker)a_GetCurrentEditMarker)(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
	}

	int editMarkerIndex()
	{
		if (!a_EditMarkerIndex) return -1;
		__try { return *(volatile int*)a_EditMarkerIndex; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
	}

	// The storage the replay director works with (the clip being edited).
	void* replayStorage()
	{
		if (!a_MarkerStorage) return nullptr;
		__try { return *(void* volatile*)a_MarkerStorage; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
	}

	void* currentClip()
	{
		if (!a_GetCurrentClip) return nullptr;
		__try { return ((FnGetClip)a_GetCurrentClip)(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
	}

	// The storage that holds `m`, and m's index in it. The replay storage is
	// tried first (that is the object the director and the speed lookup use),
	// the clip the menu is editing second; the index from the editor's own
	// global is trusted only when it really points at m.
	void* storageFor(const void* m, int* idx)
	{
		*idx = -1;
		if (!m) return nullptr;
		void* cands[2] = { replayStorage(), currentClip() };
		for (void* s : cands)
		{
			if (!s) continue;
			const int gi = editMarkerIndex();
			if (gi >= 0 && st::tryGet(s, gi) == m) { *idx = gi; return s; }
			const int i = st::indexOf(s, m);
			if (i >= 0) { *idx = i; return s; }
		}
		return nullptr;
	}
}
