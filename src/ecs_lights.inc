// =========================================================================
//  ecs_lights.inc - scene lights for the Rockstar Editor: the lights of a
//  clip, their keyframes, the file they live in, and the engine side
// =========================================================================
//  ENGINE. The game keeps an immediate-mode list of scene lights: a growable
//  array of 0x1C0-byte CLightSource records (g_SceneLights: data +0, count
//  u16 +8, capacity u16 +0xA). AddSceneLight() hands out the next record
//  (b3407 rva 0x518240: lea rcx,[g_SceneLights]; mov edx,0x10; jmp
//  GrowAndAppend) and, once per frame, a consumer (rva 0x585E90) renders
//  every record whose type is not -1 and then zeroes the count. Hooking the
//  consumer and appending our lights right before it runs makes a light
//  exist for exactly that frame; doing it every frame keeps it there.
//  The technique and the record's layout follow Rockstar Editor+ (GPL-3,
//  github.com/crxhvrd/REPlus, lights/lightbuild.h), which decompiled
//  Legacy's CLightSource::Reset, SetCommon, SetDirTangent and SetSpotlight
//  and checked the result against lights captured in game. buildRecord()
//  writes the same fields from those facts; its comments say what each is.
//
//  SCOPE. Lights belong to one clip of one project. The project is the
//  editor's project object - the pointer GetCurrentClip reads first (b3407
//  rva 0x203D120) - whose montage sits at +0x320 with its name at +0x70. The
//  clip is the replay playback controller's current clip (the object the
//  project seek JumpToNonDilatedTimeMs loads at +0x40, vtable +0x50
//  GetCurrentClipIndex), which follows an export from clip to clip; the
//  edited clip index GetCurrentClip reads second is the fallback.
//
//  KEYFRAMES. A light is one look for its whole clip until it is animated.
//  Then it carries keys (clip time in ms -> every value that can move) and is
//  interpolated between them: position, colour, intensity, range, falloff,
//  cone, volume and scale in a straight line, the direction along the great
//  circle.
//  Before the first key and after the last it holds. Type, the switches and
//  on/off belong to the light, not to a key.
//
//  ATTACH. A light can follow the camera or an entity of the scene - a ped,
//  a vehicle or a prop. Its position and direction are then kept in that
//  parent's frame (X to its right, Y ahead, Z up) and put into the world
//  every frame from where the parent is now. Entities come from the game's
//  pools: peds and objects are generic pools (storage +0, flags +8 with bit 7
//  = free slot, count +0x10, item size +0x14), where slot i is the object at
//  storage + i * item size. The vehicle pool is not like that - it holds an
//  ARRAY OF POINTERS at +0, its capacity at +8 and the in-use bits at +0x30,
//  so slot i is the pointer array[i]. Reading it as a run of objects (3.0 to
//  3.11 did) gives rubbish and no vehicle can be attached to, so both
//  readings are tried and the one that yields readable entities is kept. An
//  entity is known by its pool, its model (archetype +0x20, hash +0x18) and
//  where it was last seen: the replay rebuilds entities when the playhead
//  jumps, so when the old one is gone the same model nearest to that place
//  is taken. Its world matrix is CEntity +0x60 .. +0x9C (right, forward,
//  up, position; as Rockstar Editor+ found it on both builds).
//
//  THREADS. The editor window (ecs_lightui.inc) runs on ReShade's render
//  thread; the scope, the camera and the evaluation on the game thread (the
//  director hook); the file on the worker. All three share one lock. The
//  engine hook reads a seqlocked copy of what to draw and never waits.
// =========================================================================
namespace lights
{
	constexpr int      kMaxLights = 32, kMaxKeys = 96, kParamFloats = 17;
	constexpr uint32_t kRecordSize = 0x1C0;
	constexpr float    kDeg = 0.017453292f;
	constexpr uint32_t kProjectMontage = 0x320, kMontageName = 0x70, kVtClipIndex = 0x50;
	constexpr uint32_t kDirNearClip = 0x1E0 + 0x74, kDirFarClip = 0x1E0 + 0x78;   // the director's copy of the rendered frame

	enum { T_POINT = 0, T_SPOT = 1 };
	// The light's switches (ours; engineFlags() turns them into the game's).
	enum : uint32_t { LF_SHADOWS = 1u << 0, LF_HIRES = 1u << 1, LF_VOLUME = 1u << 2, LF_NOSPEC = 1u << 3, LF_BLACKOUT = 1u << 4,
	                  LF_SUN = 1u << 5 };

	struct Params
	{
		float pos[3], dir[3], col[3];
		float intensity, range, falloff, inner, outer, volInt, volSize;
		float scale;                   // uniform: it reaches range x scale, a spot keeps its cone's angles
	};
	static_assert(sizeof(Params) == kParamFloats * sizeof(float), "Params is a flat run of floats");

	struct KeyFrame { float t; Params p; };

	// A frame to follow: right, forward, up (unit, at right angles) and origin.
	struct Frame { float a[3], b[3], c[3], d[3]; };

	enum { A_NONE = 0, A_CAMERA = 1, A_ENTITY = 2 };
	enum { E_PED = 0, E_VEHICLE = 1, E_OBJECT = 2, E_KINDS = 3 };

	// What a light follows. Saved with it; `last` finds the entity again.
	struct Attach { int mode; int kind; uint32_t model; float last[3]; };

	// A light's lens flare, drawn by EscoFlare.fx through ReShade
	// (ecs_reshade.inc). One per light rather than per key: its brightness
	// follows the light's own intensity, which IS keyed.
	// A flare, as one flat run of floats: the file writes them in this order and
	// the render thread reads them in it. The first nine are 3.3's; a 3.3 file
	// simply stops after them and flareFill() puts today's defaults in the rest,
	// so an old flare looks exactly as it did.
	struct Flare
	{
		float on, size, bright, streak, star, ghost, halo, chroma, leak;
		float tint[3], tintMix;        // a colour of its own, and how far it replaces the light's
		float angle, cross;            // the streak's angle in degrees, and a second streak across it
		float spokes;                  // how many spikes the star has, 2 .. 8
		float ghostCount, ghostSpread; // how many ghosts, and how far apart they sit
		float haloSize;                // the halo ring's radius
		float squeeze;                 // an oval core: 1 round, 2 twice as wide
		float flicker, flickerHz;      // how much it flickers, and how fast
	};
	constexpr int kFlareFloats = 22, kFlareFloats33 = 9;
	static_assert(sizeof(Flare) == kFlareFloats * sizeof(float), "Flare is a flat run of floats");

	// Zero is not a sensible default for the numbers 3.3 did not have - it would
	// mean a star with no spikes and a core with no width - so anything out of
	// range is set to what 3.3 drew.
	void flareFill(Flare& f)
	{
		if (!(f.tint[0] > 0.0f) && !(f.tint[1] > 0.0f) && !(f.tint[2] > 0.0f)) { f.tint[0] = 0.60f; f.tint[1] = 0.80f; f.tint[2] = 1.00f; }
		if (!(f.spokes      >= 2.0f))  f.spokes = 3.0f;
		if (!(f.ghostCount  >= 1.0f))  f.ghostCount = 4.0f;
		if (!(f.ghostSpread >= 0.02f)) f.ghostSpread = 0.35f;
		if (!(f.haloSize    >= 0.02f)) f.haloSize = 0.18f;
		if (!(f.squeeze     >= 0.05f)) f.squeeze = 1.0f;
		if (!(f.flickerHz   >= 0.05f)) f.flickerHz = 6.0f;
	}

	struct Light
	{
		uint32_t id;
		char     name[32];
		bool     enabled;
		int      type;
		uint32_t flags;
		Params   base;                 // the whole clip, while nkeys == 0
		int      nkeys;
		KeyFrame keys[kMaxKeys];       // sorted by time
		Attach   at;                   // what it follows: pos and dir are then in that frame
		Flare    fl;                   // its lens flare
		// the game thread's view, each frame (not saved)
		void*    ent;                  // the entity it was found on
		int      entSlot;              // and the pool slot it was in (not saved)
		Frame    parent;               // the frame it follows, as last seen
		bool     parentOk;             // ... seen this frame
		bool     frameKnown;           // parent holds a frame at all
	};

	// A clip's time of day and weather over its length. -1 in either field
	// means "leave that one as recorded" at that key.
	constexpr int kMaxSceneKeys = 32;
	struct SceneKey { float t; int minutes; int weather; };
	struct SceneTrack { int n; SceneKey k[kMaxSceneKeys]; };

	// Every option of ENB's depth of field, keyed over the clip for the ENB
	// option (ecs_enbdof.inc). Floats interpolate, whole numbers round, the
	// switches change at the key.
	constexpr int kEnbParams = 23, kMaxEnbKeys = 16;
	struct EnbKey { float t; uint32_t set; float v[kEnbParams]; };   // `set`: bit i = this key sets option i
	struct EnbTrack { int n; EnbKey k[kMaxEnbKeys]; };

	struct LightSet { int count; uint32_t nextId; Light l[kMaxLights]; SceneTrack sky; EnbTrack enb; };

	constexpr uint32_t kEnbAll = (1u << kEnbParams) - 1u;

	// `kinds`: 0 a float, 1 a whole number, 2 a switch. Each option is blended
	// on its own, between the keys that SET it: a keyframe that only sets the
	// blur size says nothing about the focus. Returns which options the keys
	// decided; the rest are left as they were in `out`.
	uint32_t evalEnb(const EnbTrack& s, float t, float* out, const uint8_t* kinds)
	{
		uint32_t filled = 0;
		for (int p = 0; p < kEnbParams; ++p)
		{
			const uint32_t bit = 1u << p;
			const EnbKey* a = nullptr;
			const EnbKey* b = nullptr;
			for (int i = 0; i < s.n; ++i)
			{
				const EnbKey& k = s.k[i];
				if (!(k.set & bit)) continue;
				if (k.t <= t) a = &k;
				else { b = &k; break; }
			}
			if (!a && !b) continue;
			filled |= bit;
			if (!a) { out[p] = b->v[p]; continue; }                  // before the first: it holds
			if (!b || kinds[p] == 2) { out[p] = a->v[p]; continue; } // after the last, or a switch
			float u = b->t > a->t ? (t - a->t) / (b->t - a->t) : 0.0f;
			if (u < 0.0f) u = 0.0f;
			if (u > 1.0f) u = 1.0f;
			const float x = a->v[p] + (b->v[p] - a->v[p]) * u;
			out[p] = kinds[p] == 1 ? floorf(x + 0.5f) : x;
		}
		return filled;
	}

	// A key of every option at `t` (the light editor's Key ENB here).
	int addEnbKey(EnbTrack& s, float t, const float* v)
	{
		for (int i = 0; i < s.n; ++i)
			if (fabsf(s.k[i].t - t) <= 5.0f) { memcpy(s.k[i].v, v, sizeof(s.k[i].v)); s.k[i].set = kEnbAll; return i; }
		if (s.n >= kMaxEnbKeys) return -1;
		int at = s.n;
		while (at > 0 && s.k[at - 1].t > t) { s.k[at] = s.k[at - 1]; --at; }
		s.k[at].t = t;
		s.k[at].set = kEnbAll;
		memcpy(s.k[at].v, v, sizeof(s.k[at].v));
		++s.n;
		return at;
	}

	// One option of the keyframe at `t` (the Depth of Field menu's rows). A new
	// key starts from `seed` - what the keyframe showed - with only this one
	// option set, so the rest keep following the keyframes around it.
	bool setEnbParam(EnbTrack& s, float t, int p, float value, const float* seed)
	{
		if (p < 0 || p >= kEnbParams) return false;
		for (int i = 0; i < s.n; ++i)
			if (fabsf(s.k[i].t - t) <= 5.0f) { s.k[i].v[p] = value; s.k[i].set |= 1u << p; return true; }
		if (s.n >= kMaxEnbKeys) return false;
		int at = s.n;
		while (at > 0 && s.k[at - 1].t > t) { s.k[at] = s.k[at - 1]; --at; }
		s.k[at].t = t;
		s.k[at].set = 1u << p;
		memcpy(s.k[at].v, seed, sizeof(s.k[at].v));
		s.k[at].v[p] = value;
		++s.n;
		return true;
	}

	// Does the keyframe at `t` set option p itself - and to what.
	bool enbKeyValue(const EnbTrack& s, float t, int p, float* out)
	{
		for (int i = 0; i < s.n; ++i)
			if (fabsf(s.k[i].t - t) <= 5.0f && (s.k[i].set & (1u << p))) { *out = s.k[i].v[p]; return true; }
		return false;
	}

	// Hand option p of the keyframe at `t` back to the keyframes around it. A
	// key that is left setting nothing goes.
	void clearEnbParam(EnbTrack& s, float t, int p)
	{
		for (int i = 0; i < s.n; ++i)
			if (fabsf(s.k[i].t - t) <= 5.0f)
			{
				s.k[i].set &= ~(1u << p);
				if (!s.k[i].set) { for (int j = i; j < s.n - 1; ++j) s.k[j] = s.k[j + 1]; --s.n; }
				return;
			}
	}

	void removeEnbKey(EnbTrack& s, int i)
	{
		if (i < 0 || i >= s.n) return;
		for (int j = i; j < s.n - 1; ++j) s.k[j] = s.k[j + 1];
		--s.n;
	}

	// Minutes from a to b the short way round the clock: 23:00 to 01:00 runs
	// forwards through midnight, not backwards through the whole day.
	float lerpClock(int a, int b, float u)
	{
		int d = b - a;
		if (d > 720) d -= 1440;
		if (d < -720) d += 1440;
		float m = (float)a + (float)d * u;
		while (m < 0.0f) m += 1440.0f;
		while (m >= 1440.0f) m -= 1440.0f;
		return m;
	}

	// Where the track stands at `t` ms into the clip. Returns false when there
	// is nothing keyed. `from`/`to`/`blend` are the weather pair to hand the
	// game; `minutes` is the time of day, -1 for "as recorded".
	bool evalSky(const SceneTrack& s, float t, int* minutes, int* from, int* to, float* blend)
	{
		*minutes = -1; *from = -1; *to = -1; *blend = 0.0f;
		if (s.n <= 0) return false;
		int i = 0;
		while (i < s.n - 1 && s.k[i + 1].t <= t) ++i;
		const SceneKey& a = s.k[i];
		const bool last = (i >= s.n - 1) || t <= a.t;
		const SceneKey& b = last ? a : s.k[i + 1];
		float u = 0.0f;
		if (!last && b.t > a.t) u = (t - a.t) / (b.t - a.t);
		if (u < 0.0f) u = 0.0f;
		if (u > 1.0f) u = 1.0f;

		if (a.minutes >= 0)
			*minutes = (int)(b.minutes >= 0 ? lerpClock(a.minutes, b.minutes, u) + 0.5f : (float)a.minutes);
		if (a.weather >= 0)
		{
			*from = a.weather;
			*to = b.weather >= 0 ? b.weather : a.weather;
			*blend = (*to == *from) ? 0.0f : u;
		}
		return *minutes >= 0 || *from >= 0;
	}

	// Sorted by time, one key per moment.
	int addSkyKey(SceneTrack& s, float t, int minutes, int weather)
	{
		for (int i = 0; i < s.n; ++i)
			if (fabsf(s.k[i].t - t) <= 5.0f) { s.k[i].minutes = minutes; s.k[i].weather = weather; return i; }
		if (s.n >= kMaxSceneKeys) return -1;
		int at = s.n;
		while (at > 0 && s.k[at - 1].t > t) { s.k[at] = s.k[at - 1]; --at; }
		s.k[at] = { t, minutes, weather };
		++s.n;
		return at;
	}

	void removeSkyKey(SceneTrack& s, int i)
	{
		if (i < 0 || i >= s.n) return;
		for (int j = i; j < s.n - 1; ++j) s.k[j] = s.k[j + 1];
		--s.n;
	}

	struct Cam { float pos[3], right[3], fwd[3], up[3]; float fov, nearClip, farClip; };
	struct Ctx { bool valid; Cam cam; float now, marker; int markerIndex; ULONGLONG at; };
	struct DrawItem { uint32_t id; int type; uint32_t flags; Params p; };
	struct Draw { int count; ULONGLONG at; DrawItem it[kMaxLights]; };

	// --- vector maths ----------------------------------------------------------
	inline float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
	inline void  cross3(const float* a, const float* b, float* o)
	{
		const float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
		o[0] = x; o[1] = y; o[2] = z;
	}
	inline bool norm3(float* v)
	{
		const float l2 = dot3(v, v);
		if (!(l2 > 1e-12f)) return false;
		const float k = 1.0f / sqrtf(l2);
		v[0] *= k; v[1] *= k; v[2] *= k;
		return true;
	}
	inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
	inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

	// The scale as used (0.05 .. 20, 1 for anything that is not a number), and
	// how far the light reaches with it, in metres.
	inline float scaleOf(const Params& p) { return p.scale > 0.0f ? clampf(p.scale, 0.05f, 20.0f) : 1.0f; }
	inline float reachOf(const Params& p) { return clampf(p.range * scaleOf(p), 0.1f, 1000.0f); }

	// --- frames: what an attached light is kept in ------------------------------
	void toFrame(const Frame& F, const float w[3], float l[3])        // a world point into the frame
	{
		const float r[3] = { w[0] - F.d[0], w[1] - F.d[1], w[2] - F.d[2] };
		l[0] = dot3(r, F.a); l[1] = dot3(r, F.b); l[2] = dot3(r, F.c);
	}
	void fromFrame(const Frame& F, const float l[3], float w[3])      // a point of the frame into the world
	{
		const float v[3] = { l[0], l[1], l[2] };
		for (int i = 0; i < 3; ++i) w[i] = F.d[i] + F.a[i] * v[0] + F.b[i] * v[1] + F.c[i] * v[2];
	}
	void dirToFrame(const Frame& F, const float w[3], float l[3])
	{
		const float v[3] = { w[0], w[1], w[2] };
		l[0] = dot3(v, F.a); l[1] = dot3(v, F.b); l[2] = dot3(v, F.c);
	}
	void dirFromFrame(const Frame& F, const float l[3], float w[3])
	{
		const float v[3] = { l[0], l[1], l[2] };
		for (int i = 0; i < 3; ++i) w[i] = F.a[i] * v[0] + F.b[i] * v[1] + F.c[i] * v[2];
	}
	Params toWorld(const Frame& F, const Params& p) { Params o = p; fromFrame(F, p.pos, o.pos); dirFromFrame(F, p.dir, o.dir); return o; }
	Params toLocal(const Frame& F, const Params& p) { Params o = p; toFrame(F, p.pos, o.pos); dirToFrame(F, p.dir, o.dir); return o; }

	// Rows made unit and checked: false for anything that is not a rotation
	// (a slot holding garbage, an entity in the middle of being rebuilt).
	bool tidyFrame(Frame& f)
	{
		for (int i = 0; i < 3; ++i)
			if (!isfinite(f.a[i]) || !isfinite(f.b[i]) || !isfinite(f.c[i]) || !isfinite(f.d[i])) return false;
		if (!norm3(f.a) || !norm3(f.b) || !norm3(f.c)) return false;
		if (fabsf(dot3(f.a, f.b)) > 0.05f || fabsf(dot3(f.b, f.c)) > 0.05f || fabsf(dot3(f.a, f.c)) > 0.05f) return false;
		return fabsf(f.d[0]) < 50000.0f && fabsf(f.d[1]) < 50000.0f && fabsf(f.d[2]) < 50000.0f;
	}

	// v turned by `angle` radians about the unit `axis` (Rodrigues).
	void rotateAbout(float v[3], const float axis[3], float angle)
	{
		const float c = cosf(angle), s = sinf(angle), d = dot3(axis, v);
		float x[3];
		cross3(axis, v, x);
		for (int i = 0; i < 3; ++i) v[i] = v[i] * c + x[i] * s + axis[i] * d * (1.0f - c);
	}

	// From one unit direction to another along the great circle. Opposite
	// directions turn about an axis at right angles instead of passing
	// through zero halfway.
	void slerpDir(const float a[3], const float b[3], float t, float o[3])
	{
		const float d = clampf(dot3(a, b), -1.0f, 1.0f);
		if (d > 0.9995f)
		{
			for (int i = 0; i < 3; ++i) o[i] = lerpf(a[i], b[i], t);
			if (!norm3(o)) { o[0] = a[0]; o[1] = a[1]; o[2] = a[2]; }
			return;
		}
		if (d < -0.9995f)
		{
			const float z[3] = { 0, 0, 1 }, x[3] = { 1, 0, 0 };
			float axis[3];
			cross3(a, fabsf(a[2]) < 0.9f ? z : x, axis);
			norm3(axis);
			o[0] = a[0]; o[1] = a[1]; o[2] = a[2];
			rotateAbout(o, axis, 3.14159265f * t);
			return;
		}
		const float th = acosf(d), s = sinf(th);
		const float wa = sinf((1.0f - t) * th) / s, wb = sinf(t * th) / s;
		for (int i = 0; i < 3; ++i) o[i] = a[i] * wa + b[i] * wb;
		norm3(o);
	}

	// The game's convention: yaw 0 faces north (+Y), positive pitch looks up.
	void dirFromYawPitch(float yawDeg, float pitchDeg, float o[3])
	{
		const float y = yawDeg * kDeg, p = pitchDeg * kDeg;
		o[0] = -sinf(y) * cosf(p);
		o[1] = cosf(y) * cosf(p);
		o[2] = sinf(p);
	}
	void yawPitchFromDir(const float d[3], float* yawDeg, float* pitchDeg)
	{
		*pitchDeg = asinf(clampf(d[2], -1.0f, 1.0f)) / kDeg;
		*yawDeg   = atan2f(-d[0], d[1]) / kDeg;
	}

	// --- one light -------------------------------------------------------------
	Params defaultParams(int type)
	{
		Params p = {};
		p.dir[2] = -1.0f;
		p.col[0] = p.col[1] = p.col[2] = 1.0f;
		p.intensity = type == T_SPOT ? 20.0f : 10.0f;
		p.range     = type == T_SPOT ? 25.0f : 12.0f;
		p.falloff   = 8.0f;
		p.inner     = 15.0f;
		p.outer     = 45.0f;
		p.volInt    = 1.0f;
		p.volSize   = 1.0f;
		p.scale     = 1.0f;
		return p;
	}

	Params blend(const Params& a, const Params& b, float t)
	{
		Params o = a;
		for (int i = 0; i < 3; ++i)
		{
			o.pos[i] = lerpf(a.pos[i], b.pos[i], t);
			o.col[i] = lerpf(a.col[i], b.col[i], t);
		}
		o.intensity = lerpf(a.intensity, b.intensity, t);
		o.range     = lerpf(a.range, b.range, t);
		o.falloff   = lerpf(a.falloff, b.falloff, t);
		o.inner     = lerpf(a.inner, b.inner, t);
		o.outer     = lerpf(a.outer, b.outer, t);
		o.volInt    = lerpf(a.volInt, b.volInt, t);
		o.volSize   = lerpf(a.volSize, b.volSize, t);
		o.scale     = lerpf(a.scale, b.scale, t);
		slerpDir(a.dir, b.dir, t, o.dir);
		return o;
	}

	// What the light looks like at clip time `t` (ms).
	Params evalLight(const Light& L, float t)
	{
		if (L.nkeys <= 0) return L.base;
		if (t <= L.keys[0].t) return L.keys[0].p;
		const int last = L.nkeys - 1;
		if (t >= L.keys[last].t) return L.keys[last].p;
		for (int i = 0; i < last; ++i)
		{
			const KeyFrame& a = L.keys[i];
			const KeyFrame& b = L.keys[i + 1];
			if (t < b.t)
			{
				const float span = b.t - a.t;
				return blend(a.p, b.p, span > 0.0f ? (t - a.t) / span : 0.0f);
			}
		}
		return L.keys[last].p;
	}

	// The key within 5 ms of `t` (marker times are whole milliseconds), or -1.
	int keyAt(const Light& L, float t)
	{
		for (int i = 0; i < L.nkeys; ++i) if (fabsf(L.keys[i].t - t) <= 5.0f) return i;
		return -1;
	}

	// Sets the key at `t` (replacing one within 5 ms), keeping the keys in
	// time order. Returns its index, or -1 when the light is full.
	int insertKey(Light& L, float t, const Params& p)
	{
		const int k = keyAt(L, t);
		if (k >= 0) { L.keys[k].p = p; return k; }
		if (L.nkeys >= kMaxKeys) return -1;
		int at = L.nkeys;
		while (at > 0 && L.keys[at - 1].t > t) { L.keys[at] = L.keys[at - 1]; --at; }
		L.keys[at].t = t;
		L.keys[at].p = p;
		++L.nkeys;
		return at;
	}

	void deleteKey(Light& L, int i)
	{
		if (i < 0 || i >= L.nkeys) return;
		for (int j = i; j + 1 < L.nkeys; ++j) L.keys[j] = L.keys[j + 1];
		--L.nkeys;
	}

	// --- the engine's record ---------------------------------------------------
	inline void wf(uint8_t* o, uint32_t off, float v)    { memcpy(o + off, &v, 4); }
	inline void wu(uint8_t* o, uint32_t off, uint32_t v) { memcpy(o + off, &v, 4); }

	// The game's light flags (+0x64) for our switches, with SetCommon's
	// interior / exterior fixup applied exactly the way the game applies it.
	uint32_t engineFlags(uint32_t lf)
	{
		uint32_t f = 1u << 14;                                            // INT_AND_EXT: lit indoors and out
		if (lf & LF_SHADOWS)  f |= (1u << 6) | (1u << 7) | (1u << 8);     // CAST_SHADOWS, _STATIC_, _DYNAMIC_
		if (lf & LF_HIRES)    f |= 1u << 20;                              // CAST_HIGHER_RES_SHADOW: first pick of the 8 shadow slots
		if (lf & LF_VOLUME)   f |= 1u << 12;                              // VOLUME_DRAWING
		if (lf & LF_NOSPEC)   f |= 1u << 13;                              // NO_SPECULAR
		if (lf & LF_BLACKOUT) f |= 1u << 4;                               // FX: still drawn in a blackout
		if (lf & LF_SUN)      f |= 1u << 9;                               // CALC_FROM_SUNLIGHT: the engine takes the sun's colour and strength for the hour
		if ((f & 0x2004018u) == 0) f = (f & ~1u) | 2u; else f &= ~3u;
		return f;
	}

	// One CLightSource, written from scratch into the slot the game handed out.
	void buildRecord(uint8_t* o, uint32_t id, int type, uint32_t lf, const Params& p)
	{
		memset(o, 0, kRecordSize);

		// What CLightSource::Reset leaves in the fields nothing else sets.
		wf(o, 0x28, 1.0f); wf(o, 0x34, 1.0f);                  // w lanes of the direction / tangent vec4s
		wf(o, 0x40, 1.0f); wf(o, 0x44, 1.0f); wf(o, 0x48, 1.0f); wf(o, 0x4C, 1.0f);
		wu(o, 0x50, ~0u); wu(o, 0x54, ~0u); wu(o, 0x58, ~0u); wu(o, 0x5C, ~0u);
		wu(o, 0x70, ~0u);
		wu(o, 0x90, ~0u);
		wu(o, 0x94, 0xFFFCFFFFu);                                // no interior location: no entity owns this light
		wf(o, 0xD4, 0.01f); wf(o, 0xDC, 1.0f); wf(o, 0xE0, 1.0f);

		// SetCommon: position and colour (vec4s, w left at 0), type, flags,
		// intensity and the hours it is lit.
		wf(o, 0x00, p.pos[0]); wf(o, 0x04, p.pos[1]); wf(o, 0x08, p.pos[2]);
		wf(o, 0x10, p.col[0] > 0.0f ? p.col[0] : 0.0f);
		wf(o, 0x14, p.col[1] > 0.0f ? p.col[1] : 0.0f);
		wf(o, 0x18, p.col[2] > 0.0f ? p.col[2] : 0.0f);
		wu(o, 0x60, type == T_SPOT ? 2u : 1u);                  // 1 point, 2 spot
		wu(o, 0x64, engineFlags(lf));
		wf(o, 0x68, p.intensity > 0.0f ? p.intensity : 0.0f);
		wu(o, 0x6C, 0x00FFFFFFu | (1u << 29));                   // every hour; bit 29: take the fade bytes below as they are
		const float range = reachOf(p);                         // the range times the scale
		wf(o, 0x98, range);
		wf(o, 0x9C, p.falloff > 0.01f ? p.falloff : 8.0f);      // falloff exponent

		// SetDirTangent + SetSpotlight. The tangent only has to be at right
		// angles to the direction; the angles are clamped the way the game
		// clamps them and stored as degrees and cosines.
		if (type == T_SPOT)
		{
			float d[3] = { p.dir[0], p.dir[1], p.dir[2] };
			if (!norm3(d)) { d[0] = 0.0f; d[1] = 0.0f; d[2] = -1.0f; }
			const float up[3] = { 0, 0, 1 }, ax[3] = { 1, 0, 0 };
			float tg[3];
			cross3(d, fabsf(d[2]) > 0.99f ? ax : up, tg);
			norm3(tg);
			wf(o, 0x20, d[0]);  wf(o, 0x24, d[1]);  wf(o, 0x28, d[2]);
			wf(o, 0x30, tg[0]); wf(o, 0x34, tg[1]); wf(o, 0x38, tg[2]);
			const float outer = clampf(p.outer, 2.0f, 89.0f), inner = clampf(p.inner, 1.0f, outer - 1.0f);
			const float co = cosf(outer * kDeg), ci = cosf(inner * kDeg), so = sqrtf(1.0f - co * co);
			wf(o, 0xB0, ci); wf(o, 0xB4, co); wf(o, 0xB8, inner); wf(o, 0xBC, outer);
			wf(o, 0xC0, so * range);                             // cone radius at full range
			wf(o, 0xC4, co * range);                             // cone depth
		}

		// Fade distances in metres for the light, its shadow, its volume and
		// its corona: 255 is the table's ceiling, i.e. never faded by distance.
		o[0xD0] = o[0xD1] = o[0xD2] = o[0xD3] = 0xFF;

		// Volume intensity / size / exponent; the game zeroes the first two on
		// a light that draws no volume.
		if (lf & LF_VOLUME) { wf(o, 0x78, p.volInt > 0.0f ? p.volInt : 0.0f); wf(o, 0x7C, p.volSize > 0.01f ? p.volSize : 0.01f); }
		else                { wf(o, 0x78, 0.0f); wf(o, 0x7C, 0.0f); }
		wf(o, 0x80, 1.0f);

		// The shadow owner token: the shadow manager only takes lights that have
		// one and matches a light to last frame's shadow slot by it, so it must
		// be the same every frame and different for every light.
		if (lf & LF_SHADOWS) { const uint64_t token = 0x1000ull + id; memcpy(o + 0x88, &token, 8); }

		o[0x67] |= 0x80;                                          // what the game's own producers set before appending
	}

	// --- the camera the game rendered, for the gizmo ---------------------------
	struct View { Cam c; float w, h, tanV, tanH; };

	View makeView(const Cam& c, float w, float h)
	{
		View v;
		v.c = c; v.w = w > 1.0f ? w : 1.0f; v.h = h > 1.0f ? h : 1.0f;
		v.tanV = tanf(clampf(c.fov, 1.0f, 130.0f) * 0.5f * kDeg);   // the game's fov is the VERTICAL one
		v.tanH = v.tanV * (v.w / v.h);
		return v;
	}

	// World point -> pixel. False when it is behind the camera.
	bool toScreen(const View& v, const float p[3], float out[2])
	{
		const float d[3] = { p[0] - v.c.pos[0], p[1] - v.c.pos[1], p[2] - v.c.pos[2] };
		const float z = dot3(d, v.c.fwd);
		if (z < 0.05f) return false;
		const float x = dot3(d, v.c.right), y = dot3(d, v.c.up);
		out[0] = (0.5f + 0.5f * x / (z * v.tanH)) * v.w;
		out[1] = (0.5f - 0.5f * y / (z * v.tanV)) * v.h;
		return true;
	}

	// Pixel -> the unit ray from the camera through it.
	void rayAt(const View& v, float sx, float sy, float dir[3])
	{
		const float nx = (sx / v.w) * 2.0f - 1.0f, ny = 1.0f - (sy / v.h) * 2.0f;
		for (int i = 0; i < 3; ++i) dir[i] = v.c.fwd[i] + v.c.right[i] * nx * v.tanH + v.c.up[i] * ny * v.tanV;
		norm3(dir);
	}

	// The parameter s of the point on the line o + a*s closest to the ray
	// ro + rd*u (a and rd unit). False when the two run parallel.
	bool axisParam(const float o[3], const float a[3], const float ro[3], const float rd[3], float* s)
	{
		const float w0[3] = { o[0] - ro[0], o[1] - ro[1], o[2] - ro[2] };
		const float b = dot3(a, rd), d = dot3(a, w0), e = dot3(rd, w0), den = 1.0f - b * b;
		if (den < 1e-4f) return false;
		*s = (b * e - d) / den;
		return true;
	}

	// Where the ray meets the plane through po with normal n (in front only).
	bool rayPlane(const float ro[3], const float rd[3], const float po[3], const float n[3], float out[3])
	{
		const float den = dot3(rd, n);
		if (fabsf(den) < 1e-5f) return false;
		const float w[3] = { po[0] - ro[0], po[1] - ro[1], po[2] - ro[2] };
		const float u = dot3(w, n) / den;
		if (u < 0.0f) return false;
		for (int i = 0; i < 3; ++i) out[i] = ro[i] + rd[i] * u;
		return true;
	}

	// --- shared state ----------------------------------------------------------
	const char* kAddSig      = "48 8D 0D ? ? ? ? BA 10 00 00 00 E9";
	const char* kConsumerSig = "48 89 5C 24 08 57 48 83 EC 20 33 FF EB 07 33 C9 E8";
	const char* kSeekSig     = "40 53 48 83 EC 40 0F 29 74 24 30 32 D2 83 3D ? ? ? ? 02 0F 29 7C 24 20 41 8B D8 0F 28 F1 75";
	constexpr uint8_t OP_MOV_RCX_MEM[] = { 0x48, 0x8B, 0x0D };

	using FnAdd  = void* (__fastcall*)();
	using FnVoid = void (__fastcall*)();
	uintptr_t a_Add = 0, a_Consumer = 0, a_List = 0, a_Controller = 0, a_Project = 0, a_EditClip = 0;
	FnVoid    origConsumer = nullptr;
	bool      g_ready = false;

	CRITICAL_SECTION g_cs;
	struct Lock
	{
		Lock()  { EnterCriticalSection(&g_cs); }
		~Lock() { LeaveCriticalSection(&g_cs); }
		Lock(const Lock&) = delete;
		Lock& operator=(const Lock&) = delete;
	};
	void init() { InitializeCriticalSection(&g_cs); }

	struct StoreEntry { std::string project; int clip; LightSet* set; };
	std::vector<StoreEntry> g_store;
	LightSet* g_cur = nullptr;          // the lights of the clip on screen
	char      g_curProject[128] = {};
	int       g_curClip = -1;
	bool      g_dirty = false;
	ULONGLONG g_dirtyAt = 0;
	char      g_path[MAX_PATH] = {};

	volatile LONG g_openReq = 0;        // the key, the menu row or ReShade's tab -> editor window: open
	volatile bool g_textInput = false;  // editor window -> worker: typing, so the key is just a letter
	volatile bool g_uiOpen = false;     // editor window -> worker: the camera hotkeys stand down
	volatile bool g_needRelease = false; // the key closed the editor: it opens it again once it has been up

	// The editor's key is watched twice over - by the worker with
	// GetAsyncKeyState, and by the window with ImGui, because ReShade hides
	// keys from the game while it blocks input and the worker would otherwise
	// never see them. When both act on one press the window opens and shuts
	// inside a frame or two, which looks like the key hardly working. So both
	// go through this gate: one change of state per press, near enough.
	volatile LONGLONG g_toggleAt = 0;
	constexpr ULONGLONG kToggleGapMs = 350;
	bool      g_haveWindow = false;      // ReShade handed over its ImGui (ecs_reshade.inc)
	bool      g_keyWasDown = false;
	int       g_lastKey = 0, g_noWindowLogs = 0;
	int       g_drawLogs = 0;

	Ctx  g_ctx  = {}; volatile LONG g_ctxSeq  = 0;
	Draw g_draw = {}; volatile LONG g_drawSeq = 0;

	template <class T> void seqWrite(T& dst, volatile LONG& seq, const T& v)
	{
		InterlockedIncrement(&seq);
		MemoryBarrier();
		dst = v;
		MemoryBarrier();
		InterlockedIncrement(&seq);
	}
	template <class T> T seqRead(const T& src, volatile LONG& seq)
	{
		for (int i = 0; i < 16; ++i)
		{
			const LONG s0 = seq;
			if (s0 & 1) { YieldProcessor(); continue; }
			MemoryBarrier();
			T v = src;
			MemoryBarrier();
			if (seq == s0) return v;
		}
		return src;
	}

	// The lock is held by every caller of the functions below.
	void markDirty() { g_dirty = true; g_dirtyAt = GetTickCount64(); }
	int  syncAllClips();               // below: every clip of the project gets the open clip's lights
	LightSet* cur()  { return g_cur; }
	Ctx ctx()        { return seqRead(g_ctx, g_ctxSeq); }

	LightSet* findSet(const char* project, int clip, bool create)
	{
		for (StoreEntry& e : g_store) if (e.clip == clip && e.project == project) return e.set;
		if (!create) return nullptr;
		LightSet* s = (LightSet*)calloc(1, sizeof(LightSet));
		if (!s) return nullptr;
		s->nextId = 1;
		g_store.push_back({ std::string(project), clip, s });
		return s;
	}

	// A whole clip's worth of lights onto another clip. The three fields the
	// game thread fills in (what the light was found on, and where that was)
	// do not travel: the other clip resolves its own.
	void copySet(LightSet& dst, const LightSet& src)
	{
		dst = src;
		for (int i = 0; i < dst.count; ++i)
		{
			dst.l[i].ent = nullptr;
			dst.l[i].entSlot = -1;
			dst.l[i].parentOk = false;
			dst.l[i].frameKnown = false;
		}
	}

	// What the lights file holds, for the "Copy from" picker: lights are kept
	// per project AND clip, so a file carried over from another machine (or a
	// project that was renamed) holds rigs under names this clip will never
	// match. This is how they are brought over. The lock is held.
	int storeCount() { return (int)g_store.size(); }

	bool storeAt(int i, const char** project, int* clip, int* count)
	{
		if (i < 0 || i >= (int)g_store.size()) return false;
		*project = g_store[i].project.c_str();
		*clip = g_store[i].clip;
		*count = g_store[i].set ? g_store[i].set->count : 0;
		return true;
	}

	// Adds another clip's lights to the open clip, keeping the ones already
	// here. Returns how many came over.
	int copyFrom(int storeIndex)
	{
		if (!g_cur || storeIndex < 0 || storeIndex >= (int)g_store.size()) return 0;
		const LightSet* src = g_store[storeIndex].set;
		if (!src || src == g_cur) return 0;
		int n = 0;
		for (int i = 0; i < src->count && g_cur->count < kMaxLights; ++i)
		{
			Light& L = g_cur->l[g_cur->count++];
			L = src->l[i];
			L.id = g_cur->nextId++;         // its own id in this clip
			L.ent = nullptr;
			L.entSlot = -1;
			L.parentOk = false;
			L.frameKnown = false;
			++n;
		}
		if (n) markDirty();
		return n;
	}

	// "Same lights in every clip": what the open clip has, every other clip of
	// the same project gets. Returns how many clips were written. The lock is
	// held by the callers.
	int syncAllClips()
	{
		if (!g_cfg.lightsAll || !g_cur || !g_curProject[0]) return 0;
		int n = 0;
		for (StoreEntry& e : g_store)
			if (e.set && e.set != g_cur && e.project == g_curProject) { copySet(*e.set, *g_cur); ++n; }
		return n;
	}

	// A clip opened for the first time while the switch is on starts with the
	// project's lights instead of empty.
	bool seedFromSiblings(LightSet* s, const char* project)
	{
		if (!g_cfg.lightsAll || !s || s->count > 0) return false;
		for (StoreEntry& e : g_store)
			if (e.set && e.set != s && e.project == project && e.set->count > 0) { copySet(*s, *e.set); return true; }
		return false;
	}

	void clearStore()
	{
		for (StoreEntry& e : g_store) free(e.set);
		g_store.clear();
		g_cur = nullptr;
		g_curProject[0] = '\0';
		g_curClip = -1;
	}

	int indexOf(const LightSet& s, uint32_t id)
	{
		for (int i = 0; i < s.count; ++i) if (s.l[i].id == id) return i;
		return -1;
	}

	// A new light, `cam` metres ahead of the camera when there is one; a spot
	// points where the camera looks. Returns its index or -1 when full.
	int addLight(LightSet& s, int type, const Cam* cam)
	{
		if (s.count >= kMaxLights) return -1;
		Light& L = s.l[s.count];
		memset(&L, 0, sizeof(L));
		if (s.nextId == 0) s.nextId = 1;
		L.id = s.nextId++;
		snprintf(L.name, sizeof(L.name), "%s %u", type == T_SPOT ? "Spot" : "Light", L.id);
		L.enabled = true;
		L.type = type == T_SPOT ? T_SPOT : T_POINT;
		L.base = defaultParams(L.type);
		if (cam)
		{
			const float ahead = L.type == T_SPOT ? 1.5f : 3.0f;
			for (int i = 0; i < 3; ++i) L.base.pos[i] = cam->pos[i] + cam->fwd[i] * ahead;
			if (L.type == T_SPOT) memcpy(L.base.dir, cam->fwd, sizeof(L.base.dir));
		}
		return s.count++;
	}

	int duplicateLight(LightSet& s, int i)
	{
		if (i < 0 || i >= s.count || s.count >= kMaxLights) return -1;
		Light& L = s.l[s.count];
		L = s.l[i];
		L.id = s.nextId++;
		char nm[32];
		snprintf(nm, sizeof(nm), "%.24s copy", s.l[i].name);
		strcpy_s(L.name, nm);
		L.base.pos[0] += 0.5f;
		for (int k = 0; k < L.nkeys; ++k) L.keys[k].p.pos[0] += 0.5f;
		return s.count++;
	}

	void removeLight(LightSet& s, int i)
	{
		if (i < 0 || i >= s.count) return;
		for (int j = i; j + 1 < s.count; ++j) s.l[j] = s.l[j + 1];
		--s.count;
	}

	// --- reading the game (no C++ objects in these: they carry __try) ----------
	bool inModule(uintptr_t p) { return p >= g_modBase && p < g_modBase + g_modSize; }

	bool readProjectName(char* out, int cap)
	{
		out[0] = '\0';
		if (!a_Project) return false;
		__try
		{
			const uint8_t* proj = *(const uint8_t* const*)a_Project;
			if (!proj) return false;
			const uint8_t* mont = *(const uint8_t* const*)(proj + kProjectMontage);
			if (!mont) return false;
			const char* s = (const char*)(mont + kMontageName);
			int n = 0;
			for (; n < cap - 1 && n < 255; ++n)
			{
				const unsigned char ch = (unsigned char)s[n];
				if (!ch) break;
				if (ch < 0x20 || ch == 0x7F) { out[0] = '\0'; return false; }
				out[n] = (char)ch;
			}
			out[n] = '\0';
			return n > 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; return false; }
	}

	int readClipIndex()
	{
		__try
		{
			if (a_Controller)
			{
				void* c = (void*)a_Controller;
				const uintptr_t* vt = *(const uintptr_t* const*)c;
				if (vt && inModule((uintptr_t)vt) && inModule(vt[kVtClipIndex / 8]))
				{
					using Fn = int (__fastcall*)(void*);
					const int i = ((Fn)vt[kVtClipIndex / 8])(c);
					if (i >= 0) return i;
				}
			}
			if (a_EditClip) return *(const int*)a_EditClip;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
		return -1;
	}

	bool readCam(const uint8_t* dir, Cam& c)
	{
		__try
		{
			memcpy(c.right, dir + rd::DIR_RenderRowA, 12);
			memcpy(c.fwd,   dir + rd::DIR_RenderRowB, 12);
			memcpy(c.up,    dir + rd::DIR_RenderRowC, 12);
			memcpy(c.pos,   dir + rd::DIR_RenderPos, 12);
			memcpy(&c.fov,  dir + rd::DIR_RenderFov, 4);
			memcpy(&c.nearClip, dir + kDirNearClip, 4);
			memcpy(&c.farClip,  dir + kDirFarClip, 4);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		if (!(c.fov >= 1.0f && c.fov <= 130.0f)) return false;
		for (int i = 0; i < 3; ++i)
			if (!isfinite(c.pos[i]) || !isfinite(c.fwd[i]) || !isfinite(c.right[i]) || !isfinite(c.up[i])) return false;
		return fabsf(dot3(c.fwd, c.fwd) - 1.0f) < 0.05f;
	}

	float markerTime(const void* m)
	{
		__try { float t; memcpy(&t, (const uint8_t*)m + mk::OFF_TimeMs, 4); return isfinite(t) ? t : -1.0f; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return -1.0f; }
	}

	unsigned listCount()
	{
		if (!a_List) return 0;
		__try { uint16_t n; memcpy(&n, (const void*)(a_List + 8), 2); return n; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
	}

	// --- lens flares (EscoFlare.fx through ReShade, ecs_reshade.inc) ---------------
	//  A flare and a leak are lens things - the engine has no per-light control
	//  for either - so the light editor's numbers are published here with where
	//  each light is as the camera sees it, and the render thread turns that into
	//  a screen position with the back buffer's own aspect ratio.
	constexpr int kMaxFlares = 8;
	struct FlareSrc
	{
		float cs[3], dist, col[3], bright, size, streak, star, ghost, halo, chroma, leak;
		float tint[3], tintMix, angle, cross, spokes, gcount, gspread, halosize, squeeze, flicker, flickerHz, phase;
	};
	struct FlareSet { int count; ULONGLONG at; float fov, nearClip, farClip, time; FlareSrc it[kMaxFlares]; };
	FlareSet g_flares = {}; volatile LONG g_flareSeq = 0;
	bool g_flareLive = false;   // ReShade has EscoFlare.fx loaded (ecs_reshade.inc)

	FlareSet flares() { return seqRead(g_flares, g_flareSeq); }

	// Where a camera-space point (right, forward, up) lands on screen, 0 .. 1.
	// False when it is behind the lens.
	bool flareUv(const float cs[3], float fov, float aspect, float* u, float* v)
	{
		if (!(cs[1] > 0.05f)) return false;
		const float tanV = tanf(clampf(fov, 1.0f, 130.0f) * 0.5f * kDeg);
		const float tanH = tanV * (aspect > 0.1f ? aspect : 1.7778f);
		*u = 0.5f + 0.5f * cs[0] / (cs[1] * tanH);
		*v = 0.5f - 0.5f * cs[2] / (cs[1] * tanV);
		return isfinite(*u) && isfinite(*v);
	}

	// The looks the light editor offers; every number is a slider as well.
	enum { FP_OFF = 0, FP_SOFT, FP_ANAMORPHIC, FP_STAR, FP_CINEMATIC, FP_LEAK, FP_BLUE, FP_DIRTY, FP_COUNT };

	const char* flareName(int p)
	{
		switch (p)
		{
		case FP_SOFT:       return "Soft";
		case FP_ANAMORPHIC: return "Anamorphic";
		case FP_STAR:       return "Star";
		case FP_CINEMATIC:  return "Cinematic";
		case FP_LEAK:       return "Leak";
		case FP_BLUE:       return "Blue";
		case FP_DIRTY:      return "Dirty";
		default:            return "Off";
		}
	}

	Flare flarePreset(int p)
	{
		Flare f = {};
		switch (p)
		{   //                on    size  bright streak star  ghost halo  chroma leak   tint                  mix    angle cross spokes gN    gSpread halo  squeeze flick hz
		case FP_SOFT:       f = { 1.0f, 1.0f, 1.0f,  0.0f, 0.0f, 0.0f, 0.0f, 0.30f, 0.0f, { 0.60f, 0.80f, 1.00f }, 0.00f,  0.0f, 0.0f, 3.0f, 4.0f, 0.35f, 0.18f, 1.0f, 0.00f, 6.0f }; break;
		case FP_ANAMORPHIC: f = { 1.0f, 1.0f, 1.2f,  1.0f, 0.0f, 0.2f, 0.2f, 0.40f, 0.0f, { 0.55f, 0.75f, 1.00f }, 0.15f,  0.0f, 0.0f, 3.0f, 4.0f, 0.35f, 0.18f, 1.6f, 0.00f, 6.0f }; break;
		case FP_STAR:       f = { 1.0f, 0.9f, 1.0f,  0.2f, 1.0f, 0.0f, 0.1f, 0.30f, 0.0f, { 0.60f, 0.80f, 1.00f }, 0.00f,  0.0f, 0.0f, 6.0f, 4.0f, 0.35f, 0.18f, 1.0f, 0.00f, 6.0f }; break;
		case FP_CINEMATIC:  f = { 1.0f, 1.1f, 1.2f,  0.8f, 0.3f, 0.8f, 0.7f, 0.60f, 0.2f, { 0.70f, 0.85f, 1.00f }, 0.10f, -6.0f, 0.25f, 4.0f, 5.0f, 0.30f, 0.20f, 1.3f, 0.08f, 4.0f }; break;
		case FP_LEAK:       f = { 1.0f, 1.0f, 1.0f,  0.3f, 0.0f, 0.0f, 0.0f, 0.20f, 1.0f, { 1.00f, 0.85f, 0.70f }, 0.20f,  0.0f, 0.0f, 3.0f, 4.0f, 0.35f, 0.18f, 1.2f, 0.00f, 6.0f }; break;
		case FP_BLUE:       f = { 1.0f, 1.2f, 1.3f,  1.4f, 0.1f, 0.3f, 0.3f, 0.50f, 0.3f, { 0.35f, 0.60f, 1.00f }, 0.70f,  0.0f, 0.35f, 4.0f, 4.0f, 0.32f, 0.22f, 2.2f, 0.00f, 6.0f }; break;
		case FP_DIRTY:      f = { 1.0f, 1.0f, 1.1f,  0.5f, 0.4f, 1.3f, 1.0f, 0.80f, 0.6f, { 1.00f, 0.80f, 0.55f }, 0.35f,  8.0f, 0.15f, 5.0f, 6.0f, 0.22f, 0.26f, 1.1f, 0.15f, 9.0f }; break;
		default: break;
		}
		return f;
	}

	// One light's flare source for this frame (the lock is held). False when the
	// light has no flare, is behind the lens, or would be invisible anyway.
	bool flareOf(const Light& L, const Params& p, const Cam& cam, FlareSrc& s)
	{
		if (L.fl.on <= 0.5f) return false;
		const float rel[3] = { p.pos[0] - cam.pos[0], p.pos[1] - cam.pos[1], p.pos[2] - cam.pos[2] };
		s.cs[0] = dot3(rel, cam.right); s.cs[1] = dot3(rel, cam.fwd); s.cs[2] = dot3(rel, cam.up);
		if (!(s.cs[1] > 0.05f)) return false;
		s.dist = sqrtf(dot3(rel, rel));
		memcpy(s.col, p.col, sizeof(s.col));
		s.bright = L.fl.bright * clampf(p.intensity / 10.0f, 0.0f, 4.0f);
		if (!(s.bright > 0.001f)) return false;
		s.size   = L.fl.size > 0.02f ? L.fl.size : 1.0f;
		s.streak = L.fl.streak; s.star = L.fl.star; s.ghost = L.fl.ghost;
		s.halo   = L.fl.halo; s.chroma = L.fl.chroma; s.leak = L.fl.leak;

		// 3.5's numbers, with a 3.3 flare (or a zeroed one) filled in first, and
		// every one clamped here so the shader can trust them.
		Flare f = L.fl;
		flareFill(f);
		memcpy(s.tint, f.tint, sizeof(s.tint));
		s.tintMix   = clampf(f.tintMix, 0.0f, 1.0f);
		s.angle     = clampf(f.angle, -180.0f, 180.0f);
		s.cross     = clampf(f.cross, 0.0f, 2.0f);
		s.spokes    = clampf(f.spokes, 2.0f, 8.0f);
		s.gcount    = clampf(f.ghostCount, 1.0f, 6.0f);
		s.gspread   = clampf(f.ghostSpread, 0.02f, 1.5f);
		s.halosize  = clampf(f.haloSize, 0.02f, 0.8f);
		s.squeeze   = clampf(f.squeeze, 0.05f, 4.0f);
		s.flicker   = clampf(f.flicker, 0.0f, 1.0f);
		s.flickerHz = clampf(f.flickerHz, 0.05f, 30.0f);
		// a phase of its own from the light's id, so two lights never flicker in step
		s.phase     = (float)((L.id * 2654435761u >> 16) & 0xFFFFu) * (6.2831853f / 65535.0f);
		return true;
	}

	// --- what a light can follow: the camera, or an entity of the scene -----------
	Frame frameOf(const Cam& c)
	{
		Frame f;
		memcpy(f.a, c.right, sizeof(f.a)); memcpy(f.b, c.fwd, sizeof(f.b)); memcpy(f.c, c.up, sizeof(f.c)); memcpy(f.d, c.pos, sizeof(f.d));
		return f;
	}

	const char* kPoolSig[E_KINDS] = {
		"48 8B 05 ? ? ? ? 41 0F BF C8 0F BF 40 10",   // peds:     mov rax,[pool]; movsx ecx,r8w; movsx eax,word [rax+10h]
		"48 8B 05 ? ? ? ? F3 0F 59 F6 48 8B 08",      // vehicles: mov rax,[pool]; mulss xmm6,xmm6; mov rcx,[rax]
		"48 8B 05 ? ? ? ? 8B 78 10 85 FF" };          // objects:  mov rax,[pool]; mov edi,[rax+10h]; test edi,edi
	const char* kPoolName[E_KINDS] = { "g_PedPool", "g_VehiclePool", "g_ObjectPool" };
	constexpr uint8_t  OP_MOV_RAX_MEM[] = { 0x48, 0x8B, 0x05 };
	constexpr uint32_t ENT_Archetype = 0x20, ARCH_Hash = 0x18, ENT_Matrix = 0x60;
	uintptr_t a_Pool[E_KINDS] = {};
	bool      g_canPick = false;   // at least one pool resolved
	int       g_attachLogs = 0;

	const char* kindName(int k) { return k == E_PED ? "Ped" : k == E_VEHICLE ? "Vehicle" : k == E_OBJECT ? "Prop" : "Entity"; }
	inline float dist2(const float* a, const float* b) { const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2]; return x * x + y * y + z * z; }

	// storage + item: the generic pools, where an entity sits inline at
	// storage + i * item. slots: the vehicle pool's array of pointers, where
	// an entity is the pointer slots[i]. flags or bits says which are in use.
	struct PoolView { uint8_t* storage; const uint64_t* slots; const uint8_t* flags; const uint32_t* bits; int size; uint64_t item; };

	enum { VEH_UNKNOWN = 0, VEH_POINTERS = 1, VEH_INLINE = 2 };
	int g_vehMode = VEH_UNKNOWN;   // how this build's vehicle pool reads
	int g_vehLogs = 0;

	// A pool the way the game's own loops read it (see ATTACH at the top).
	// `vehMode` picks how the vehicle pool is read; poolView() below chooses.
	bool poolViewAs(int kind, int vehMode, PoolView& v)
	{
		v.storage = nullptr; v.slots = nullptr; v.flags = nullptr; v.bits = nullptr; v.size = 0; v.item = 0;
		if (kind < 0 || kind >= E_KINDS || !a_Pool[kind]) return false;
		__try
		{
			const uint8_t* p = *(const uint8_t* const*)a_Pool[kind];
			if (!p) return false;
			(void)vehMode;
			v.storage = *(uint8_t* const*)p;
			v.flags = *(const uint8_t* const*)(p + 8);
			v.size = *(const int32_t*)(p + 0x10);
			v.item = *(const uint32_t*)(p + 0x14);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		if (!(v.size > 0 && v.size <= 200000)) return false;
		if (v.slots) return v.bits != nullptr;
		return v.storage && (v.flags || v.bits) && v.item >= 0x40 && v.item <= 0x100000;
	}

	// The entity in slot i, or nullptr for a free slot.
	void* slotAt(const PoolView& v, int i)
	{
		if (i < 0 || i >= v.size) return nullptr;
		__try
		{
			if (v.bits) { if (!((v.bits[i >> 5] >> (i & 31)) & 1u)) return nullptr; }
			else if (v.flags[i] & 0x80) return nullptr;
			if (v.slots)
			{
				const uint64_t a = v.slots[i];                       // the vehicle pool: a pointer per slot
				return (a > 0x10000 && (a & 7) == 0) ? (void*)a : nullptr;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
		return v.storage + (uint64_t)i * v.item;
	}

	uint32_t modelOf(const void* e);          // both are just below
	bool     frameOfEntity(const void* e, Frame& f);

	// How many of the first `cap` slots hold something that really looks like
	// an entity: a model hash and a world matrix can be read from it.
	int entitiesIn(const PoolView& v, int cap)
	{
		int found = 0;
		const int n = v.size < cap ? v.size : cap;
		for (int i = 0; i < n; ++i)
		{
			void* e = slotAt(v, i);
			if (!e || !modelOf(e)) continue;
			Frame f;
			if (!frameOfEntity(e, f)) continue;
			// inside the map: a stray pointer that happens to read as a matrix
			// almost never lands somewhere a car could be
			if (fabsf(f.d[0]) > 20000.0f || fabsf(f.d[1]) > 20000.0f || fabsf(f.d[2]) > 5000.0f) continue;
			++found;
		}
		return found;
	}

	// ---- the vehicle pool, found rather than assumed --------------------------
	//  Peds and props are generic pools and read the same on every build. The
	//  vehicle pool is the odd one out and 3.0 - 3.12 read it wrongly, so it is
	//  worked out here in the process: every candidate layout is tried and the
	//  one that yields real entities is kept. Cheap enough to do once - a few
	//  tens of thousands of guarded reads - and it reports what it found.
	struct VehLayout { int hops; bool pointers; uint32_t arrOff, sizeOff, bitsOff, itemOff; };
	VehLayout g_vehLay = {};
	volatile LONG g_vehReady = 0;   // 1 = g_vehLay is worked out and usable
	ULONGLONG g_vehTriedAt = 0;     // the worker's last attempt
	int  g_vehTries = 0;
	int  g_vehLogs2 = 0;

	const uint8_t* vehBase(int hops)
	{
		__try
		{
			const uint8_t* p = *(const uint8_t* const*)a_Pool[E_VEHICLE];
			if (hops == 0) return p;
			return p ? *(const uint8_t* const*)p : nullptr;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
	}

	// A view built from one candidate layout. False when anything in it is not
	// readable or not plausible.
	bool viewFrom(const uint8_t* p, const VehLayout& L, PoolView& v)
	{
		v.storage = nullptr; v.slots = nullptr; v.flags = nullptr; v.bits = nullptr; v.size = 0; v.item = 0;
		if (!p) return false;
		__try
		{
			const uint32_t size = *(const uint32_t*)(p + L.sizeOff);
			if (!(size > 0 && size <= 200000)) return false;
			const uint32_t* bits = *(const uint32_t* const*)(p + L.bitsOff);
			if (!bits) return false;
			volatile uint32_t touchBits = bits[0];
			(void)touchBits;
			if (L.pointers)
			{
				v.slots = *(const uint64_t* const*)(p + L.arrOff);
				if (!v.slots) return false;
				volatile uint64_t touch = v.slots[0];
				(void)touch;
			}
			else
			{
				v.storage = *(uint8_t* const*)(p + L.arrOff);
				if (!v.storage) return false;
				v.item = *(const uint64_t*)(p + L.itemOff);
				if (!(v.item >= 0x40 && v.item <= 0x100000)) return false;
				volatile uint64_t touch = *(const uint64_t*)v.storage;
				(void)touch;
			}
			v.bits = bits;
			v.size = (int)size;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		return true;
	}

	// Sixteen qwords of whatever the global points at, so the fields can be
	// placed by hand when nothing in the grid fits.
	void dumpVehPool()
	{
		for (int hops = 0; hops < 2; ++hops)
		{
			const uint8_t* p = vehBase(hops);
			char line[420];
			int n = snprintf(line, sizeof(line), "lights: vehicle pool %d hop(s) -> %p:", hops + 1, (const void*)p);
			for (int i = 0; i < 16 && p && n < (int)sizeof(line) - 22; ++i)
			{
				uint64_t q = 0;
				__try { q = *(const uint64_t*)(p + i * 8); }
				__except (EXCEPTION_EXECUTE_HANDLER) { q = 0xBADBADBADBADBADull; }
				n += snprintf(line + n, sizeof(line) - (size_t)n, " [%02X]=%llX", i * 8, (unsigned long long)q);
			}
			logf("%s", line);
		}
	}

	bool tryLayout(const VehLayout& c, int sample, int* found)
	{
		*found = 0;
		PoolView v;
		if (!viewFrom(vehBase(c.hops), c, v)) return false;
		*found = entitiesIn(v, sample);
		return *found > 0;
	}

	ULONGLONG g_vehCheckAt = 0;     // when what was settled on was last re-checked
	int g_vehDropped = 0;

	void commitLayout(const VehLayout& c, int found)
	{
		g_vehLay = c;
		g_vehCheckAt = GetTickCount64();
		InterlockedExchange(&g_vehReady, 1);
		logf("lights: the vehicle pool reads as %s - %d hop(s), array +0x%X, count +0x%X, in-use bits +0x%X%s: %d vehicle(s) in it",
			c.pointers ? "an array of pointers" : "objects one after another", c.hops + 1,
			c.arrOff, c.sizeOff, c.bitsOff, c.pointers ? "" : ", item size +0x18", found);
	}

	// ONLY from the worker thread. Most of the reads below fault and are caught,
	// and every one of those costs a trip through the kernel - on the game's own
	// thread that is a freeze, which is what 3.13 did.
	void probeVehiclePool(ULONGLONG now)
	{
		// What was settled on is re-checked while the replay runs: a layout
		// picked during a loading screen can read as a pool and hold nothing,
		// and 3.18 proved that a wrong one, once kept, stays for the session.
		if (g_vehReady)
		{
			if (replayMode() == 0 || now < g_vehCheckAt + 10000) return;
			g_vehCheckAt = now;
			PoolView v;
			int alive = 0;
			if (viewFrom(vehBase(g_vehLay.hops), g_vehLay, v)) alive = entitiesIn(v, 512);
			if (alive > 0) return;
			InterlockedExchange(&g_vehReady, 0);
			g_vehTriedAt = 0;
			if (g_vehDropped < 4)
			{
				++g_vehDropped;
				logf("lights: the vehicle pool layout that was settled on holds nothing any more - dropping it and looking again");
			}
			return;
		}
		if (!a_Pool[E_VEHICLE]) return;
		// Only once the replay is running. At the menus and on a loading screen
		// the world holds no vehicles at all, and 3.14 - 3.16 spent their
		// attempts there and gave up before the editor was ever opened. Any
		// replay mode will do - playback has cars in it as much as editing.
		if (replayMode() == 0) return;
		if (g_vehTriedAt && now > g_vehTriedAt && now - g_vehTriedAt < 5000) return;   // since() is declared further down
		g_vehTriedAt = now;
		++g_vehTries;

		// The two shapes GTA V really uses - one or two pointer hops, the array
		// at +0, the count at +8, the in-use bits at +0x30 - tried first and
		// sampled DEEP. The replay puts its cars in whatever pool slots are
		// free, so a shallow sample finds an empty stretch and moves on: 3.13
		// looked at 256 slots and found 48 cars, 3.14 - 3.17 looked at 16 to 48
		// and found none. That, and nothing else, is why vehicles came back and
		// went away again.
		// The four shapes the game is known to use - the array (or the run of
		// objects) at +0, the count at +8, the in-use bits at +0x30, one or two
		// pointer hops away. One real car in 512 slots is enough to believe any
		// of these; anything else is a guess and has to earn it below.
		static const VehLayout kKnown[] = {
			{ 0, true,  0x00, 0x08, 0x30, 0x18 },
			{ 1, true,  0x00, 0x08, 0x30, 0x18 },   // b3751 reads this way: 48 cars
			{ 0, false, 0x00, 0x08, 0x30, 0x18 },
			{ 1, false, 0x00, 0x08, 0x30, 0x18 } };
		int found = 0;
		for (const VehLayout& known : kKnown)
			if (tryLayout(known, 512, &found) && found >= 1) { commitLayout(known, found); return; }

		static const uint32_t kSizeOff[] = { 0x08, 0x0C, 0x10, 0x14, 0x18, 0x1C, 0x20, 0x28, 0x30, 0x38, 0x40, 0x50, 0x60, 0x68, 0x70 };
		static const uint32_t kBitsOff[] = { 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x58, 0x60, 0x68, 0x70 };
		static const uint32_t kArrOff[]  = { 0x00, 0x08 };
		// A guessed layout has to show real weight: eight entities, not two.
		// Two is what a wrong one found on 3.18 while the replay was still
		// loading and the real shape had nothing in it yet.
		VehLayout best = {};
		int bestCount = 7;
		for (int hops = 0; hops < 2; ++hops)
		{
			if (!vehBase(hops)) continue;
			for (int ai = 0; ai < 2; ++ai)
				for (int pointers = 1; pointers >= 0; --pointers)
					for (uint32_t so : kSizeOff)
						for (uint32_t bo : kBitsOff)
						{
							if (bo == so || bo == kArrOff[ai]) continue;
							const VehLayout cand = { hops, pointers != 0, kArrOff[ai], so, bo, 0x18 };
							int n = 0;
							if (tryLayout(cand, 64, &n) && n > bestCount) { bestCount = n; best = cand; }
						}
		}
		if (bestCount > 7)
		{
			int wide = 0;
			tryLayout(best, 4096, &wide);          // the whole pool before trusting a guess
			if (wide >= 8) { commitLayout(best, wide); return; }
		}
		// It keeps looking for as long as the clip is open - a clip can be
		// standing in an empty field. Only the first failure and then every
		// twelfth (about a minute apart) are worth a line.
		if (g_vehTries == 1)
		{
			PoolView probe;
			const VehLayout known = { 0, true, 0x00, 0x08, 0x30, 0x18 };
			const bool shaped = viewFrom(vehBase(0), known, probe);
			logf("lights: nothing in the vehicle pool at rva 0x%llX yet - 512 slots of each known shape were read and none held a car "
				 "(one hop %s%d slots). Looking again every 5 s while the replay runs. What it points at:",
				rva(a_Pool[E_VEHICLE]), shaped ? "says it holds " : "does not read as a pool, ", shaped ? probe.size : 0);
			dumpVehPool();
		}
		else if (g_vehTries % 12 == 0 && g_vehLogs2 < 6)
		{
			++g_vehLogs2;
			logf("lights: still nothing in the vehicle pool after %d tries - peds and props can be attached to meanwhile", g_vehTries);
		}
	}

	// The game thread: never probes, only reads what the worker settled on.
	bool poolView(int kind, PoolView& v)
	{
		if (kind != E_VEHICLE) return poolViewAs(kind, 0, v);
		if (!g_vehReady) return false;
		return viewFrom(vehBase(g_vehLay.hops), g_vehLay, v);
	}

	uint32_t modelOf(const void* e)
	{
		__try
		{
			const uint8_t* arch = *(const uint8_t* const*)((const uint8_t*)e + ENT_Archetype);
			return arch ? *(const uint32_t*)(arch + ARCH_Hash) : 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
	}

	bool frameOfEntity(const void* e, Frame& f)
	{
		__try
		{
			const uint8_t* m = (const uint8_t*)e + ENT_Matrix;
			memcpy(f.a, m, 12); memcpy(f.b, m + 0x10, 12); memcpy(f.c, m + 0x20, 12); memcpy(f.d, m + 0x30, 12);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		return tidyFrame(f);
	}

	// The entity a light follows this frame: the one it was on while that slot
	// still holds the same model, else the same model nearest to where it was
	// last seen; nullptr when there is none in the scene.
	// `slot` is the pool slot this light's entity was in last time, in and out.
	//
	// It used to work that slot out from the pointer instead: (cached -
	// storage) / item size. The vehicle pool is an array of pointers, so its
	// storage is null and its item size is ZERO - and the frame after a light
	// was put on a car, that divided by zero and took the game down with an
	// unhandled exception (3.13, "escoeditor.asi+18024"). One read of the
	// remembered slot answers the same question for every kind of pool.
	void* findEntity(const Attach& at, void* cached, int* slot = nullptr)
	{
		PoolView v;
		if (!poolView(at.kind, v)) return nullptr;
		if (cached && slot && *slot >= 0 && *slot < v.size && slotAt(v, *slot) == cached && modelOf(cached) == at.model)
			return cached;
		if (!at.model) return nullptr;
		void* best = nullptr;
		int   bestSlot = -1;
		float bestD = 0.0f;
		for (int i = 0; i < v.size; ++i)
		{
			void* e = slotAt(v, i);
			if (!e || modelOf(e) != at.model) continue;
			Frame f;
			if (!frameOfEntity(e, f)) continue;
			const float d = dist2(f.d, at.last);
			if (!best || d < bestD) { best = e; bestSlot = i; bestD = d; }
		}
		if (slot) *slot = bestSlot;
		return best;
	}

	// The entities near the camera, published for the editor window while it
	// is picking what a light follows: the nearest kMaxScene within 150 m.
	constexpr int kMaxScene = 64;
	struct SceneItem { void* ent; int kind; uint32_t model; float dist; Frame f; };
	struct Scene { int count; ULONGLONG at; SceneItem it[kMaxScene]; };
	Scene g_scene = {}; volatile LONG g_sceneSeq = 0;
	volatile bool g_wantScene = false;   // editor window -> game thread: it is picking

	Scene scene() { return seqRead(g_scene, g_sceneSeq); }

	void gatherScene(const Cam& cam, ULONGLONG now)
	{
		Scene s;
		s.count = 0;
		s.at = now;
		for (int k = 0; k < E_KINDS; ++k)
		{
			PoolView v;
			if (!poolView(k, v)) continue;
			for (int i = 0; i < v.size; ++i)
			{
				void* e = slotAt(v, i);
				if (!e) continue;
				SceneItem it;
				if (!frameOfEntity(e, it.f)) continue;
				it.dist = sqrtf(dist2(it.f.d, cam.pos));
				if (it.dist > 150.0f) continue;
				it.ent = e; it.kind = k; it.model = modelOf(e);
				if (s.count < kMaxScene) { s.it[s.count++] = it; continue; }
				int worst = 0;
				for (int j = 1; j < kMaxScene; ++j) if (s.it[j].dist > s.it[worst].dist) worst = j;
				if (it.dist < s.it[worst].dist) s.it[worst] = it;
			}
		}
		seqWrite(g_scene, g_sceneSeq, s);
	}

	// Where an attached light's parent is this frame (the lock is held).
	void resolveParent(Light& L, const Ctx& c)
	{
		L.parentOk = false;
		if (L.at.mode == A_CAMERA)
		{
			if (!c.valid) return;
			L.parent = frameOf(c.cam);
			L.parentOk = L.frameKnown = true;
		}
		else if (L.at.mode == A_ENTITY)
		{
			void* e = findEntity(L.at, L.ent, &L.entSlot);
			Frame f;
			if (!e || !frameOfEntity(e, f)) { L.ent = nullptr; L.entSlot = -1; return; }
			if (e != L.ent && g_attachLogs < 20)
			{
				++g_attachLogs;
				logf("lights: '%s' found its %s %08X%s", L.name, kindName(L.at.kind), L.at.model, L.ent ? " again (the replay rebuilt it)" : "");
			}
			L.ent = e;
			L.parent = f;
			L.parentOk = L.frameKnown = true;
			memcpy(L.at.last, f.d, sizeof(L.at.last));
		}
	}

	// A light (its whole-clip look and every key) moved from one frame into
	// another, staying where it is now. nullptr means the world.
	void reframe(Light& L, const Frame* from, const Frame* to)
	{
		auto move = [&](Params& p) { const Params w = from ? toWorld(*from, p) : p; p = to ? toLocal(*to, w) : w; };
		move(L.base);
		for (int k = 0; k < L.nkeys; ++k) move(L.keys[k].p);
	}

	// The frame a light's numbers are in now: nullptr for the world, or when
	// what it follows was never seen - then `fallback` stands in for it.
	const Frame* frameNow(const Light& L, const Frame* fallback)
	{
		if (L.at.mode == A_NONE) return nullptr;
		return L.frameKnown ? &L.parent : fallback;
	}

	bool attachTo(Light& L, int mode, const Frame& f, void* ent, int kind, uint32_t model, const Frame* fallback)
	{
		if (mode != A_CAMERA && mode != A_ENTITY) return false;
		reframe(L, frameNow(L, fallback), &f);
		L.at.mode = mode;
		L.at.kind = mode == A_ENTITY ? kind : 0;
		L.at.model = mode == A_ENTITY ? model : 0;
		memcpy(L.at.last, f.d, sizeof(L.at.last));
		L.ent = ent;
		L.entSlot = -1;
		L.parent = f;
		L.parentOk = L.frameKnown = true;
		return true;
	}

	// How far above the middle of a ped, vehicle or prop a light lands when it
	// starts following it: a car's own origin is inside its bodywork, and a
	// ped's is at the feet, so both would bury the light.
	constexpr float kJumpUp = 1.0f;

	// Puts the light on what it follows. The whole clip and every key shift by
	// the same amount, so keyed movement is kept - the light as a whole moves.
	// `now` is the moment the editor is showing, in ms.
	void jumpToParent(Light& L, float now)
	{
		if (L.at.mode == A_NONE) return;
		const Params at = evalLight(L, now);
		const float d[3] = { -at.pos[0], -at.pos[1], kJumpUp - at.pos[2] };
		auto shift = [&](Params& p) { p.pos[0] += d[0]; p.pos[1] += d[1]; p.pos[2] += d[2]; };
		shift(L.base);
		for (int k = 0; k < L.nkeys; ++k) shift(L.keys[k].p);
	}

	void detach(Light& L, const Frame* fallback)
	{
		reframe(L, frameNow(L, fallback), nullptr);
		memset(&L.at, 0, sizeof(L.at));
		L.ent = nullptr;
		L.entSlot = -1;
		L.parentOk = L.frameKnown = false;
	}

	// --- the game thread: once per replay frame ---------------------------------
	void syncScope()
	{
		char name[128];
		const bool haveName = readProjectName(name, sizeof(name));
		const int clip = haveName ? readClipIndex() : -1;
		Lock lk;
		if (!haveName || clip < 0)
		{
			if (g_cur) { g_cur = nullptr; g_curClip = -1; g_curProject[0] = '\0'; logf("lights: no clip open - scene lights off"); }
			return;
		}
		if (g_cur && clip == g_curClip && strcmp(name, g_curProject) == 0) return;
		g_cur = findSet(name, clip, true);
		strcpy_s(g_curProject, name);
		g_curClip = clip;
		const bool seeded = seedFromSiblings(g_cur, name);
		if (seeded) markDirty();
		logf("lights: clip %d of '%s' - %d light(s)%s", clip + 1, name, g_cur ? g_cur->count : 0,
			seeded ? " (the project's lights, same in every clip)" : "");
	}

	// From the director hook: publish the camera and what to draw this frame.
	volatile LONG g_heldCount = 0;   // frames an attached light held the last place it saw its parent

	// The lights of the open clip as the game wants them: world positions,
	// types and flags. It lives here, and not inside the director hook, so the
	// light consumer can build it itself when nothing else has - which is what
	// an export needs, where the director tick is not guaranteed to come round
	// between the frames being encoded. `fs` may be null: the flares need a
	// camera, and an export has no ReShade pass to draw them in.
	void buildDraw(const Ctx& c, Draw& d, FlareSet* fs)
	{
		d.count = 0;
		d.at = GetTickCount64();
		if (fs)
		{
			fs->count = 0;
			fs->at = d.at;
			fs->fov = c.cam.fov;
			fs->nearClip = c.cam.nearClip;
			fs->farClip = c.cam.farClip;
			fs->time = c.now * 0.001f;   // the clip's own clock: a flicker falls on the same frame every playback
		}
		Lock lk;
		if (!g_cur) return;
		for (int i = 0; i < g_cur->count; ++i)
		{
			Light& L = g_cur->l[i];
			resolveParent(L, c);        // an entity needs no camera; A_CAMERA keeps its last frame without one
			if (!L.enabled || d.count >= kMaxLights) continue;
			Params p = evalLight(L, c.now);
			if (L.at.mode != A_NONE)
			{
				// The replay only keeps entities alive around the camera, so from
				// a distance the car a light follows winks in and out of the pools.
				// Dropping the light on those frames is what made it - and its
				// flare - flicker. It holds the last place it saw instead.
				if (!L.parentOk && !L.frameKnown) continue;   // never seen it at all
				if (!L.parentOk) InterlockedIncrement(&g_heldCount);
				p = toWorld(L.parent, p);
			}
			DrawItem& it = d.it[d.count++];
			it.id = L.id; it.type = L.type; it.flags = L.flags;
			it.p = p;
			if (fs && c.valid && fs->count < kMaxFlares && flareOf(L, p, c.cam, fs->it[fs->count])) ++fs->count;
		}
	}

	void onDirectorFrame(uint8_t* dir)
	{
		if (!g_ready || !dir) return;
		Ctx c = {};
		c.at = GetTickCount64();
		c.valid = readCam(dir, c.cam);
		c.now = game::replayTimeMs();
		c.marker = -1.0f;
		c.markerIndex = -1;
		const int ei = game::editMarkerIndex();
		if (ei >= 0)
			if (const void* m = game::currentMarker())
			{
				c.marker = markerTime(m);
				if (c.marker >= 0.0f) c.markerIndex = ei;
			}
		seqWrite(g_ctx, g_ctxSeq, c);

		syncScope();

		// The clip's own time and weather, handed to ecs_scene.inc for the
		// packet hook. Published every frame and stale after a second, so a
		// clip without keys falls straight back to the menu rows.
		{
			Lock lk;
			int mins = -1, from = -1, to = -1;
			float blend = 0.0f;
			if (g_cur && evalSky(g_cur->sky, c.now, &mins, &from, &to, &blend))
			{
				scene::g_animTime = mins;
				scene::g_animFrom = from;
				scene::g_animTo = to;
				scene::g_animBlend = blend;
				InterlockedExchange64((volatile LONGLONG*)&scene::g_animAt, (LONGLONG)c.at);
			}
		}

		Draw d;
		FlareSet fs;
		buildDraw(c, d, &fs);
		seqWrite(g_draw, g_drawSeq, d);
		seqWrite(g_flares, g_flareSeq, fs);
		if (g_wantScene && c.valid) gatherScene(c.cam, c.at);
	}

	// --- the engine hook ----------------------------------------------------------
	int  g_cmpLogs = 0;                          // 0 none yet, 1 theirs written, 2 both
	void compareRecords(const uint8_t* mine);    // defined with inject() below

	bool addOne(const DrawItem& it)
	{
		__try
		{
			void* slot = ((FnAdd)a_Add)();
			if (!slot) return false;
			buildRecord((uint8_t*)slot, it.id, it.type, it.flags, it.p);
			if (g_cmpLogs == 1) { compareRecords((const uint8_t*)slot); g_cmpLogs = 2; }
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	// A flickering light is one that is in the frame's light list on some
	// frames and not on others, and there are only three ways that happens:
	// the game never calls the consumer that frame, it calls it and the list
	// will take no more, or our own snapshot went stale. Each second of this
	// is counted, and a line is written whenever the picture changes - which
	// says which of the three it is instead of leaving it to guesswork.
	struct Tally { int calls, injects, wanted, got, refused, stale, rebuilt; unsigned listLo, listHi; };
	Tally g_tally = {};
	ULONGLONG g_tallyAt = 0;
	int g_tallyLogs = 0;
	char g_tallyLast[96] = {};

	void tallyReset() { g_tally = {}; g_tally.listLo = 0xFFFFFFFFu; }

	void tallyLine(ULONGLONG now)
	{
		if (!g_tallyAt) { g_tallyAt = now; tallyReset(); return; }
		if (now - g_tallyAt < 2000) return;
		const Tally t = g_tally;
		g_tallyAt = now;
		tallyReset();
		if (!t.calls && !t.injects) return;
		char sig[96];
		snprintf(sig, sizeof(sig), "%d|%d|%d|%d|%d", t.wanted, t.got, t.refused, t.stale, (int)g_presented > 0 ? 1 : 0);
		const bool same = strcmp(sig, g_tallyLast) == 0;
		strcpy_s(g_tallyLast, sig);
		if (same || g_tallyLogs >= 30) return;
		++g_tallyLogs;
		const int frames = (int)InterlockedExchange(&g_presented, 0);
		logf("lights: in the last two seconds the game drew %d frame(s) and asked for its lights %d time(s); %d of %d went in, %d refused (the list was full), %d skipped as stale, %d built here because nothing else had; the game's own list held %u to %u. %s",
			frames, t.calls, t.got, t.wanted, t.refused, t.stale, t.rebuilt,
			t.listLo == 0xFFFFFFFFu ? 0u : t.listLo, t.listHi,
			t.refused ? "A refusal is the game's light list being full."
			          : t.stale ? "Stale means the replay director did not run that frame."
			          : (frames > 0 && t.calls < frames - frames / 8)
			                    ? "FEWER ASKS THAN FRAMES: the light is only in some of the frames, which is the flicker."
			                    : "Every light went in, in every frame the game drew.");
		const LONG held = InterlockedExchange(&g_heldCount, 0);
		if (held)
			logf("lights: an attached light held its last place in %ld frame(s) of those two seconds - the thing it follows was not in the scene then (the replay drops what is far from the camera). It stays put now instead of blinking.", held);
	}

	// One of the game's OWN scene light records, side by side with one of ours.
	// Everything of ours goes into the list every frame (the tally proves it),
	// so a light that blinks in the distance is the engine deciding not to draw
	// it - and the difference has to be in the record. This is written once,
	// the first time the game has a light of its own in the list.
	void compareRecords(const uint8_t* mine)
	{
		if (g_cmpLogs || !a_List) return;
		const uint8_t* theirs = nullptr;
		__try
		{
			const uint8_t* storage = *(const uint8_t* const*)a_List;
			uint16_t n = 0;
			memcpy(&n, (const void*)(a_List + 8), 2);
			if (storage && n > 0) theirs = storage;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { theirs = nullptr; }
		if (!theirs) return;
		++g_cmpLogs;
		for (int pass = 0; pass < 2; ++pass)
		{
			const uint8_t* r = pass ? mine : theirs;
			if (!r) continue;
			char line[520];
			int k = snprintf(line, sizeof(line), "lights: %s record 0x60..0xEF:", pass ? "our own" : "the game's own");
			__try
			{
				for (uint32_t off = 0x60; off < 0xF0 && k < (int)sizeof(line) - 10; off += 4)
				{
					uint32_t w = 0;
					memcpy(&w, r + off, 4);
					k += snprintf(line + k, sizeof(line) - (size_t)k, " %02X=%08X", off, w);
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { k += snprintf(line + k, sizeof(line) - (size_t)k, " <unreadable>"); }
			logf("%s", line);
		}
	}

	void inject()
	{
		++g_tally.calls;
		Draw d = seqRead(g_draw, g_drawSeq);
		// An export is driven frame by frame by whatever is exporting, and the
		// replay director's tick - which is what publishes the snapshot above -
		// is not guaranteed to come round between those frames. So rather than
		// hand the game a stale list, or none at all, build it here from the
		// clip's own lights and the replay clock. No camera: only flares want one.
		if (d.count <= 0 || GetTickCount64() - d.at > 1000)
		{
			Ctx here = {};
			here.valid = false;
			here.now = game::replayTimeMs();
			here.marker = -1.0f;
			here.markerIndex = -1;
			here.at = GetTickCount64();
			buildDraw(here, d, nullptr);
			if (d.count > 0) ++g_tally.rebuilt;
		}
		if (d.count <= 0) return;
		const unsigned before = listCount();
		if (before < g_tally.listLo) g_tally.listLo = before;
		if (before > g_tally.listHi) g_tally.listHi = before;
		if (before > 0 && !g_cmpLogs) compareRecords(nullptr);   // theirs first, while the list still holds only theirs
		int n = 0;
		for (int i = 0; i < d.count; ++i) { if (!addOne(d.it[i])) break; ++n; }
		++g_tally.injects;
		g_tally.wanted += d.count;
		g_tally.got += n;
		g_tally.refused += d.count - n;
		if (n && g_drawLogs < 2)
		{
			++g_drawLogs;
			logf("lights: %d scene light(s) handed to the game this frame (its list now holds %u)", n, listCount());
		}
	}

	void __fastcall hkConsumer()
	{
		// No mode test at all. A clip's lights exist only while that clip is
		// open, so having one IS the gate - and an export runs in whatever mode
		// the thing driving it chose (the game's own Export button opens the
		// playback as BAKE; Rockstar Editor Plus rewrites that to a full-project
		// preview so it can capture the frames itself). Asking about the mode is
		// how the lights kept missing from exports.
		if (g_ready && g_cur) { inject(); tallyLine(GetTickCount64()); }
		origConsumer();
	}

	// --- the file -------------------------------------------------------------------
	constexpr const char* kFileHeader = "EscoEditorLights 2";   // 1 = 2.8, without the scale

	void appendParams(std::string& o, const char* tag, const float* t, const Params& p)
	{
		float f[kParamFloats];
		memcpy(f, &p, sizeof(f));
		char b[768];
		int n = snprintf(b, sizeof(b), "%s", tag);
		if (t) n += snprintf(b + n, sizeof(b) - n, " %.9g", *t);
		for (int i = 0; i < kParamFloats && n < (int)sizeof(b) - 24; ++i) n += snprintf(b + n, sizeof(b) - n, " %.9g", f[i]);
		o.append(b, (size_t)n);
		o.push_back('\n');
	}

	std::string serialize()
	{
		std::string o = kFileHeader;
		o += "\n";
		char b[256];
		for (const StoreEntry& e : g_store)
		{
			if (!e.set) continue;
			snprintf(b, sizeof(b), "scope %d %s\n", e.clip, e.project.c_str());
			o += b;
			for (int i = 0; i < e.set->count; ++i)
			{
				const Light& L = e.set->l[i];
				char nm[sizeof(L.name)];
				strcpy_s(nm, L.name);
				for (char* c = nm; *c; ++c) if (*c == '\r' || *c == '\n') *c = ' ';
				snprintf(b, sizeof(b), "light %u %d %d 0x%X %s\n", L.id, L.enabled ? 1 : 0, L.type, L.flags, nm);
				o += b;
				if (L.at.mode != A_NONE)
				{
					snprintf(b, sizeof(b), "a %d %d 0x%08X %.9g %.9g %.9g\n", L.at.mode, L.at.kind, L.at.model, L.at.last[0], L.at.last[1], L.at.last[2]);
					o += b;
				}
				if (L.fl.on > 0.5f)
				{
					float g[kFlareFloats];
					memcpy(g, &L.fl, sizeof(g));
					int n = snprintf(b, sizeof(b), "f");
					for (int j = 0; j < kFlareFloats && n < (int)sizeof(b) - 24; ++j) n += snprintf(b + n, sizeof(b) - n, " %.9g", g[j]);
					o.append(b, (size_t)n);
					o.push_back('\n');
				}
				appendParams(o, "b", nullptr, L.base);
				for (int k = 0; k < L.nkeys; ++k) appendParams(o, "k", &L.keys[k].t, L.keys[k].p);
			}
			for (int k = 0; k < e.set->enb.n; ++k)
			{
				const EnbKey& q = e.set->enb.k[k];
				std::string line = "e";
				char num[32];
				snprintf(num, sizeof(num), " %.9g %u", q.t, (unsigned)q.set);
				line += num;
				for (int p = 0; p < kEnbParams; ++p) { snprintf(num, sizeof(num), " %.9g", q.v[p]); line += num; }
				o += line;
				o += "\n";
			}
			for (int k = 0; k < e.set->sky.n; ++k)
			{
				const SceneKey& s = e.set->sky.k[k];
				snprintf(b, sizeof(b), "t %.9g %d %d\n", s.t, s.minutes, s.weather);
				o += b;
			}
		}
		return o;
	}

	// As many floats as the line actually holds, up to n, and how many that was.
	// A file written by an older build holds fewer.
	int parseSome(const char* s, float* out, int n)
	{
		int i = 0;
		for (; i < n; ++i)
		{
			char* end = nullptr;
			const float v = strtof(s, &end);
			if (end == s || !isfinite(v)) break;
			out[i] = v;
			s = end;
		}
		return i;
	}

	bool parseFloats(const char* s, float* out, int n)
	{
		for (int i = 0; i < n; ++i)
		{
			char* end = nullptr;
			out[i] = strtof(s, &end);
			if (end == s || !isfinite(out[i])) return false;
			s = end;
		}
		return true;
	}

	// Reads a whole file's worth into the store. Returns the lights read.
	int parse(const std::string& text)
	{
		size_t pos = 0;
		bool header = false;
		int  nf = kParamFloats;      // the values on a b line (a k line has the time first)
		LightSet* set = nullptr;
		Light* L = nullptr;
		int lights = 0;
		while (pos < text.size())
		{
			size_t eol = text.find('\n', pos);
			if (eol == std::string::npos) eol = text.size();
			std::string line = text.substr(pos, eol - pos);
			pos = eol + 1;
			if (!line.empty() && line.back() == '\r') line.pop_back();
			const char* s = line.c_str();
			if (!header)
			{
				if (strncmp(s, "EscoEditorLights", 16) != 0) { logf("lights: %s is not an EscoEditor lights file - ignored", g_path); return 0; }
				header = true;
				nf = atoi(s + 16) >= 2 ? kParamFloats : 16;   // file 1 (2.8) had no scale
				continue;
			}
			if (!strncmp(s, "scope ", 6))
			{
				const int clip = atoi(s + 6);
				const char* sp = strchr(s + 6, ' ');
				set = (sp && sp[1]) ? findSet(sp + 1, clip, true) : nullptr;
				L = nullptr;
				continue;
			}
			if (!strncmp(s, "e ", 2))
			{
				float g[2 + kEnbParams] = {};
				const int got = set ? parseSome(s + 2, g, 2 + kEnbParams) : 0;
				if (got == 2 + kEnbParams)                       // 4.7: time, which options, values
				{
					const int at = addEnbKey(set->enb, g[0], g + 2);
					if (at >= 0) set->enb.k[at].set = (uint32_t)g[1] & kEnbAll;
				}
				else if (got == 1 + kEnbParams)                  // 4.6: time and every option
					addEnbKey(set->enb, g[0], g + 1);
				continue;
			}
			if (!strncmp(s, "t ", 2))
			{
				float t = 0.0f;
				int mins = -1, wx = -1;
				if (set && sscanf_s(s + 2, "%f %d %d", &t, &mins, &wx) == 3) addSkyKey(set->sky, t, mins, wx);
				continue;
			}
			if (!strncmp(s, "light ", 6))
			{
				L = nullptr;
				if (!set || set->count >= kMaxLights) continue;
				unsigned id = 0, flags = 0;
				int on = 1, type = 0, used = 0;
				if (sscanf_s(s + 6, "%u %d %d %x %n", &id, &on, &type, &flags, &used) < 4) continue;
				L = &set->l[set->count++];
				memset(L, 0, sizeof(*L));
				L->id = id ? id : set->nextId;
				L->enabled = on != 0;
				L->type = type == T_SPOT ? T_SPOT : T_POINT;
				L->flags = flags;
				strncpy_s(L->name, s + 6 + used, _TRUNCATE);
				L->base = defaultParams(L->type);
				if (L->id >= set->nextId) set->nextId = L->id + 1;
				++lights;
				continue;
			}
			if (L && s[0] == 'a' && s[1] == ' ')
			{
				int mode = 0, kind = 0;
				unsigned model = 0;
				float x = 0, y = 0, z = 0;
				if (sscanf_s(s + 2, "%d %d %x %f %f %f", &mode, &kind, &model, &x, &y, &z) == 6 &&
					(mode == A_CAMERA || mode == A_ENTITY) && kind >= 0 && kind < E_KINDS && isfinite(x) && isfinite(y) && isfinite(z))
				{
					L->at.mode = mode; L->at.kind = kind; L->at.model = model;
					L->at.last[0] = x; L->at.last[1] = y; L->at.last[2] = z;
				}
				continue;
			}
			if (L && s[0] == 'f' && s[1] == ' ')
			{
				float g[kFlareFloats] = {};
				const int got = parseSome(s + 2, g, kFlareFloats);
				if (got >= kFlareFloats33)          // 3.3 wrote nine of them, 3.5 writes them all
				{
					memcpy(&L->fl, g, (size_t)got * sizeof(float));
					flareFill(L->fl);
				}
				continue;
			}
			if (L && s[0] == 'b' && s[1] == ' ')
			{
				float f[kParamFloats];
				if (parseFloats(s + 2, f, nf)) memcpy(&L->base, f, nf * sizeof(float));
				continue;
			}
			if (L && s[0] == 'k' && s[1] == ' ' && L->nkeys < kMaxKeys)
			{
				float f[kParamFloats + 1];
				if (parseFloats(s + 2, f, nf + 1))
				{
					Params p = defaultParams(L->type);
					memcpy(&p, f + 1, nf * sizeof(float));
					insertKey(*L, f[0], p);
				}
				continue;
			}
		}
		return lights;
	}

	void load()
	{
		snprintf(g_path, sizeof(g_path), "%sEscoEditor.lights.txt", g_dir);
		FILE* f = nullptr;
		if (fopen_s(&f, g_path, "rb") != 0 || !f) { logf("lights: no saved lights yet (%s)", g_path); return; }
		std::string text;
		char buf[4096];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
		fclose(f);
		Lock lk;
		const int lights = parse(text);
		logf("lights: %d light(s) in %d clip(s) read from %s", lights, (int)g_store.size(), g_path);
	}

	void save()
	{
		std::string text;
		{
			Lock lk;
			syncAllClips();          // before it is written out, so the file and memory agree
			text = serialize();
			g_dirty = false;
		}
		char tmp[MAX_PATH];
		snprintf(tmp, sizeof(tmp), "%s.tmp", g_path);
		FILE* f = nullptr;
		bool ok = fopen_s(&f, tmp, "wb") == 0 && f;
		if (ok)
		{
			ok = fwrite(text.data(), 1, text.size(), f) == text.size();
			fclose(f);
		}
		if (ok) ok = MoveFileExA(tmp, g_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
		if (!ok)
		{
			logf("lights: could not write %s (error %lu) - will try again", g_path, GetLastError());
			Lock lk;
			markDirty();
		}
	}

	// Process exit: never wait on a lock a killed thread may still hold.
	void saveAtExit()
	{
		if (!g_ready || !TryEnterCriticalSection(&g_cs)) return;
		const bool dirty = g_dirty;
		LeaveCriticalSection(&g_cs);
		if (dirty) save();
	}

	// --- the light editor's key -------------------------------------------------------
	//  KeyLights opens the editor. The Lights page of the EscoEditor menu, the
	//  editor window and the EscoEditor tab in ReShade's menu change it in
	//  game: left/right on the menu row walk a list of free keys, and a pick
	//  takes whatever key is pressed next (one already down when the pick
	//  began has to go up first). Two watchers, and the first to see a key
	//  wins: the worker (GetAsyncKeyState, fine while nothing holds the input)
	//  and the editor window through ReShade's ImGui (ReShade hides the keys
	//  from GetAsyncKeyState while its windows hold the input). Esc or
	//  Backspace cancel, 15 s without a key gives up, and a key the editor,
	//  its menus, the free camera or the speed keys already use is refused.
	static_assert(ImGuiKey_9 - ImGuiKey_0 == 9 && ImGuiKey_Z - ImGuiKey_A == 25 && ImGuiKey_F12 - ImGuiKey_F1 == 11 &&
			ImGuiKey_Keypad9 - ImGuiKey_Keypad0 == 9, "ImGui's digits, letters, F keys and keypad digits run in order");

	// The ImGui key for a Windows virtual-key code. ImGuiKey_None when there is
	// none - and then the key cannot be seen while the editor holds the input.
	ImGuiKey imguiKeyFor(int vk)
	{
		if (vk >= '0' && vk <= '9') return (ImGuiKey)(ImGuiKey_0 + (vk - '0'));
		if (vk >= 'A' && vk <= 'Z') return (ImGuiKey)(ImGuiKey_A + (vk - 'A'));
		if (vk >= VK_F1 && vk <= VK_F12) return (ImGuiKey)(ImGuiKey_F1 + (vk - VK_F1));
		if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return (ImGuiKey)(ImGuiKey_Keypad0 + (vk - VK_NUMPAD0));
		switch (vk)
		{
		case VK_TAB:        return ImGuiKey_Tab;
		case VK_LEFT:       return ImGuiKey_LeftArrow;
		case VK_RIGHT:      return ImGuiKey_RightArrow;
		case VK_UP:         return ImGuiKey_UpArrow;
		case VK_DOWN:       return ImGuiKey_DownArrow;
		case VK_PRIOR:      return ImGuiKey_PageUp;
		case VK_NEXT:       return ImGuiKey_PageDown;
		case VK_HOME:       return ImGuiKey_Home;
		case VK_END:        return ImGuiKey_End;
		case VK_INSERT:     return ImGuiKey_Insert;
		case VK_DELETE:     return ImGuiKey_Delete;
		case VK_BACK:       return ImGuiKey_Backspace;
		case VK_SPACE:      return ImGuiKey_Space;
		case VK_RETURN:     return ImGuiKey_Enter;
		case VK_ESCAPE:     return ImGuiKey_Escape;
		case VK_LCONTROL:   return ImGuiKey_LeftCtrl;
		case VK_LSHIFT:     return ImGuiKey_LeftShift;
		case VK_LMENU:      return ImGuiKey_LeftAlt;
		case VK_LWIN:       return ImGuiKey_LeftSuper;
		case VK_RCONTROL:   return ImGuiKey_RightCtrl;
		case VK_RSHIFT:     return ImGuiKey_RightShift;
		case VK_RMENU:      return ImGuiKey_RightAlt;
		case VK_RWIN:       return ImGuiKey_RightSuper;
		case VK_APPS:       return ImGuiKey_Menu;
		case VK_OEM_7:      return ImGuiKey_Apostrophe;
		case VK_OEM_COMMA:  return ImGuiKey_Comma;
		case VK_OEM_MINUS:  return ImGuiKey_Minus;
		case VK_OEM_PERIOD: return ImGuiKey_Period;
		case VK_OEM_2:      return ImGuiKey_Slash;
		case VK_OEM_1:      return ImGuiKey_Semicolon;
		case VK_OEM_PLUS:   return ImGuiKey_Equal;
		case VK_OEM_4:      return ImGuiKey_LeftBracket;
		case VK_OEM_5:      return ImGuiKey_Backslash;
		case VK_OEM_6:      return ImGuiKey_RightBracket;
		case VK_OEM_3:      return ImGuiKey_GraveAccent;
		case VK_CAPITAL:    return ImGuiKey_CapsLock;
		case VK_SCROLL:     return ImGuiKey_ScrollLock;
		case VK_NUMLOCK:    return ImGuiKey_NumLock;
		case VK_SNAPSHOT:   return ImGuiKey_PrintScreen;
		case VK_PAUSE:      return ImGuiKey_Pause;
		case VK_DECIMAL:    return ImGuiKey_KeypadDecimal;
		case VK_DIVIDE:     return ImGuiKey_KeypadDivide;
		case VK_MULTIPLY:   return ImGuiKey_KeypadMultiply;
		case VK_SUBTRACT:   return ImGuiKey_KeypadSubtract;
		case VK_ADD:        return ImGuiKey_KeypadAdd;
		default:            return ImGuiKey_None;
		}
	}

	int g_reshadeKey = 0;   // the key that opens ReShade's menu (ecs_reshade.inc reads it), 0 = unknown

	// Why `vk` cannot be the light editor's key, or nullptr when it can.
	const char* shortcutProblem(int vk)
	{
		if (imguiKeyFor(vk) == ImGuiKey_None) return "the editor window could not see it.";
		switch (vk)
		{
		case VK_LCONTROL: case VK_RCONTROL: case VK_LSHIFT: case VK_RSHIFT: case VK_LMENU: case VK_RMENU:
		case VK_LWIN: case VK_RWIN: case VK_APPS:
			return "Shift, Ctrl, Alt and the Windows keys are taken.";
		case VK_RETURN: case VK_SPACE: case VK_BACK: case VK_ESCAPE: case VK_TAB:
		case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
			return "the editor's menus use it.";
		case VK_PRIOR: case VK_NEXT: case 'A': case 'S': case 'D':
			return "the free camera flies with it.";
		case 'W': case 'E': case 'R': case VK_DELETE:
			return "the light editor uses it (W move, E rotate, R scale, Del delete).";
		case VK_CAPITAL: case VK_NUMLOCK: case VK_SCROLL: case VK_SNAPSHOT: case VK_PAUSE:
			return "lock keys, Print Screen and Pause do not work as a shortcut.";
		default: break;
		}
		if (vk == VK_F8 && isFiveM()) return "it opens FiveM's console.";
		if (vk == g_reshadeKey)       return "it opens ReShade's menu.";
		if (vk == g_cfg.keySlower)    return "it makes the camera slower (KeySlower).";
		if (vk == g_cfg.keyFaster)    return "it makes the camera faster (KeyFaster).";
		if (vk == g_cfg.keyReset)     return "it resets the camera speed (KeyReset).";
		if (vk == g_cfg.keyBoost)     return "it is the speed boost key (KeyBoost).";
		if (vk == g_cfg.keySlow)      return "it is the slow key (KeySlow).";
		return nullptr;
	}

	// Left/right on the menu row walk these, skipping the ones taken.
	const int kCycle[] = {
		'L', 'K', 'J', 'H', 'U', 'I', 'O', 'P', 'N', 'M', 'B',
		VK_F1, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6, VK_F7, VK_F8, VK_F9, VK_F10, VK_F11, VK_F12,
		VK_INSERT, VK_HOME, VK_END };
	constexpr int kCycleCount = (int)(sizeof(kCycle) / sizeof(kCycle[0]));

	// The free key after (delta > 0) or before `from` in that list; `from` when none is free.
	int nextShortcut(int from, int delta)
	{
		int at = -1;
		for (int i = 0; i < kCycleCount; ++i) if (kCycle[i] == from) { at = i; break; }
		if (at < 0) at = delta > 0 ? kCycleCount - 1 : 0;   // a key off the list: the first step lands on an end
		for (int n = 0; n < kCycleCount; ++n)
		{
			at = (at + (delta > 0 ? 1 : kCycleCount - 1)) % kCycleCount;
			if (!shortcutProblem(kCycle[at])) return kCycle[at];
		}
		return from;
	}

	// The pick: its state, and the line the menu row and the windows show.
	constexpr ULONGLONG kCaptureGraceMs = 250, kCaptureTimeoutMs = 15000;
	enum { CAP_NONE = 0, CAP_MENU, CAP_WINDOW, CAP_RESHADE };
	enum { END_NONE = 0, END_SAVED, END_CANCELLED, END_TIMEOUT, END_REFUSED };
	volatile LONG g_capture = 0;         // 1 while a new key is awaited
	volatile LONG g_captureSeq = 0;      // bumped whenever the pick or its line changes
	volatile LONG g_captureEnd = END_NONE;
	int           g_captureSrc = CAP_NONE;
	ULONGLONG     g_captureStart = 0, g_captureAt = 0, g_keyMsgAt = 0;
	char          g_keyMsg[128] = {};    // its last byte stays 0: no line written is longer
	bool          g_held[256] = {};      // the worker's view of the keys during a pick
	bool          g_scanning = false;

	// Milliseconds from t to now; 0 when t is later (read from another thread's clock).
	inline ULONGLONG since(ULONGLONG now, ULONGLONG t) { return now > t ? now - t : 0; }

	bool toggleDue(ULONGLONG now) { return since(now, (ULONGLONG)g_toggleAt) >= kToggleGapMs; }
	void toggleDone(ULONGLONG now) { InterlockedExchange64(&g_toggleAt, (LONGLONG)now); }

	void keyMessage(LONG end, const char* fmt, ...)
	{
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(g_keyMsg, sizeof(g_keyMsg), fmt, ap);
		va_end(ap);
		g_keyMsgAt = GetTickCount64();
		InterlockedExchange(&g_captureEnd, end);
		InterlockedIncrement(&g_captureSeq);
	}

	void setShortcut(int vk)
	{
		g_cfg.keyLights = vk;
		iniWriteInt("KeyLights", vk);
		char k[32];
		logf("lights: the light editor key is now %s (0x%02X)", keyName(vk, k, sizeof(k)), vk);
	}

	void beginCapture(int src)
	{
		g_captureSrc = src;
		g_captureStart = g_captureAt = GetTickCount64();
		InterlockedExchange(&g_capture, 1);
		keyMessage(END_NONE, "Press the new key. Esc cancels.");
	}

	void cancelCapture(LONG end, const char* what)
	{
		if (InterlockedCompareExchange(&g_capture, 0, 1) != 1) return;
		char k[32];
		keyMessage(end, "%s - still %s.", what, keyName(g_cfg.keyLights, k, sizeof(k)));
	}

	// A key went down during a pick (from either watcher). True when the pick is over.
	bool offerKey(int vk)
	{
		if (g_capture != 1) return true;
		if (vk == VK_ESCAPE || vk == VK_BACK) { cancelCapture(END_CANCELLED, "Cancelled"); return true; }
		char k[32];
		keyName(vk, k, sizeof(k));
		if (const char* why = shortcutProblem(vk))
		{
			g_captureAt = GetTickCount64();   // the time to pick another starts again
			keyMessage(END_REFUSED, "%s can't be used: %s Press another key.", k, why);
			return false;
		}
		if (InterlockedCompareExchange(&g_capture, 0, 1) != 1) return true;   // the other watcher had it
		setShortcut(vk);
		keyMessage(END_SAVED, "Saved: %s opens the light editor.", k);
		return true;
	}

	// Left/right on the menu row.
	void stepShortcut(int delta)
	{
		InterlockedExchange(&g_capture, 0);
		const int vk = nextShortcut(g_cfg.keyLights, delta);
		if (vk != g_cfg.keyLights) setShortcut(vk);
		char k[32];
		keyMessage(END_SAVED, "Saved: %s opens the light editor.", keyName(vk, k, sizeof(k)));
	}

	// The worker's watcher: every key the editor window could see as well.
	void scanCapture(ULONGLONG now)
	{
		if (g_capture != 1) { g_scanning = false; return; }
		if (since(now, g_captureAt) > kCaptureTimeoutMs) { cancelCapture(END_TIMEOUT, "No key pressed"); return; }
		const bool fg = gameInForeground();
		const bool fresh = !g_scanning || !fg || since(now, g_captureStart) < kCaptureGraceMs;   // only note what is down
		g_scanning = fg;
		for (int vk = 1; vk < 255; ++vk)
		{
			if (imguiKeyFor(vk) == ImGuiKey_None) continue;
			const bool d = (GetAsyncKeyState(vk) & 0x8000) != 0;
			const bool edge = d && !g_held[vk];
			g_held[vk] = d;
			if (edge && !fresh && offerKey(vk)) return;
		}
	}

	// From the menu row (the game thread, no lock held).
	void toggleAllClips()
	{
		g_cfg.lightsAll = !g_cfg.lightsAll;
		iniWriteInt("LightsAllClips", g_cfg.lightsAll ? 1 : 0);
		int n = 0;
		{
			Lock lk;
			if (g_cfg.lightsAll) n = syncAllClips();
			markDirty();
		}
		logf("lights: same lights in every clip %s%s", g_cfg.lightsAll ? "on" : "off",
			n ? " - this clip's lights went to the project's other clips" : "");
	}

	// --- the worker -------------------------------------------------------------------
	void tick(ULONGLONG now)
	{
		if (!g_ready) return;
		scanCapture(now);
		probeVehiclePool(now);   // the worker's job: it is too expensive for the game thread

		// The key opens the editor; the editor closes itself on it (ecs_lightui.inc).
		const int vk = g_cfg.keyLights;
		if (vk != g_lastKey) { g_lastKey = vk; g_keyWasDown = true; }   // a new key counts once it has been up
		const bool down = isDown(vk);
		if (down && !g_keyWasDown && !g_needRelease && !g_uiOpen && g_capture == 0 && !g_textInput && toggleDue(now) && editorOpen() && gameInForeground())
		{
			if (g_haveWindow) { InterlockedExchange(&g_openReq, 1); toggleDone(now); }
			else if (g_noWindowLogs < 2)
			{
				++g_noWindowLogs;
				char k[32];
				logf("lights: %s pressed - the light editor window needs ReShade with add-on support (the lights still draw)", keyName(vk, k, sizeof(k)));
			}
		}
		g_keyWasDown = down;

		bool due = false;
		{
			Lock lk;
			due = g_dirty && now - g_dirtyAt >= 700;
		}
		if (due) save();
	}

	void install()
	{
		if (!g_cfg.lights) { logf("lights: Lights=0 in the ini - no scene lights"); return; }
		if (isEnhanced())  { logf("lights: GTA V Enhanced is not mapped - no scene lights"); return; }
		logf("lights: resolving the scene light list");
		a_Add      = game::scanFn(kAddSig, "AddSceneLight");
		a_Consumer = game::scanFn(kConsumerSig, "SceneLightConsumer");
		if (a_Add) a_List = derive(a_Add, 0, 3, OP_LEA_RCX, 3, "g_SceneLights");
		if (const uintptr_t seek = game::scanFn(kSeekSig, "JumpToNonDilatedTimeMs"))
			a_Controller = derive(seek, 0x40, 0x43, OP_LEA_RCX, 3, "g_PlaybackController");
		if (game::a_GetCurrentClip)
		{
			a_Project  = derive(game::a_GetCurrentClip, 0x04, 0x07, OP_MOV_RCX_MEM, 3, "g_EditProject");
			a_EditClip = derive(game::a_GetCurrentClip, 0x12, 0x14, game::OP_MOV_EDX, 2, "g_EditClipIndex");
		}
		for (int k = 0; k < E_KINDS; ++k)
			if (const uintptr_t at = game::scanFn(kPoolSig[k], kPoolName[k])) a_Pool[k] = derive(at, 0, 3, OP_MOV_RAX_MEM, 3, kPoolName[k]);
		g_canPick = a_Pool[E_PED] || a_Pool[E_VEHICLE] || a_Pool[E_OBJECT];
		load();
		if (!a_Add || !a_Consumer || !a_List || !a_Project)
		{
			logf("lights: the scene light list or the project did not resolve on this build - no scene lights");
			return;
		}
		if (!hookFn(a_Consumer, (void*)hkConsumer, (void**)&origConsumer, "SceneLightConsumer")) return;
		g_ready = true;
		char k[24];
		logf("lights: installed - a clip's lights are drawn in playback, the menus and the export; %s opens the light editor%s",
			keyName(g_cfg.keyLights, k, sizeof(k)), a_Controller ? "" : " (no playback controller: an export uses the edited clip's lights)");
		logf("lights: a light can follow the camera%s%s%s%s", g_canPick ? ", or " : " (the entity lists did not resolve: no peds, vehicles or props)",
			a_Pool[E_PED] ? "peds " : "", a_Pool[E_VEHICLE] ? "vehicles " : "", a_Pool[E_OBJECT] ? "props" : "");
	}
}
