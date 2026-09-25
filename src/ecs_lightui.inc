// =========================================================================
//  ecs_lightui.inc - the light editor: an ImGui window through ReShade and
//  a 3D gizmo in the viewport
// =========================================================================
//  ReShade hands add-ons its Dear ImGui - the same windows as its own menu -
//  and fires reshade_overlay every frame, whether its menu is open or not.
//  The light editor opens on its key (KeyLights, L by default, inside a
//  clip), from the Lights page of the EscoEditor menu, or from the
//  EscoEditor tab in ReShade's menu. While it is open this module
//    - asks ReShade to keep the game's input for one more frame, every frame
//      (block_input_next_frame), so clicks and keys stay in the editor,
//    - lays an invisible window over the whole screen, so the mouse belongs
//      to ImGui wherever it is, and draws the light markers and the gizmo on
//      the background draw list (behind every window),
//    - draws the "EscoEditor Lights" window: the clip's lights, the selected
//      light's transform (what it follows, position, rotation, scale), its
//      values and switches, and its keyframes.
//  The gizmo projects with the camera the game rendered (lights::View).
//  A light that follows the camera or an entity keeps its numbers in that
//  frame (lights::Frame): the markers and the gizmo work in the world and
//  every edit is turned back into the frame. "Pick in the scene" shows the
//  peds, vehicles and props near the camera (lights::gatherScene, on the
//  game thread) and a click on one attaches the selected light to it.
//  While ReShade holds the input it also hides the keys from the game's
//  GetAsyncKeyState, so the editor's own keys - its key to close it, Esc,
//  W E R, Del, and a new key being picked - are read from ImGui.
//  Everything here runs on ReShade's render thread; the light data is only
//  touched holding lights::g_cs.
// =========================================================================
namespace lightui
{
	bool     g_imgui = false;        // ReShade gave us its ImGui function table
	bool     g_open  = false;
	bool     g_closeArmed = false;   // the editor's key has been up since it opened
	bool     g_picking = false;      // the next click in the picture picks what the light follows
	bool     g_showList = false;     // the scene list is open in the attach section
	bool     g_kindOn[lights::E_KINDS] = { true, true, true };
	bool     g_jump = true;          // a light lands on what it starts following
	int      g_listHover = -1;       // the row the mouse is on, boxed in the picture
	int      g_copyPick = 0;         // the clip the "Copy from" picker is on
	uint32_t g_sel   = 0;            // selected light (id), 0 = none
	int      g_mode  = 0;            // gizmo: 0 move, 1 rotate, 2 scale
	int      g_drag  = 0;            // the handle being dragged
	uint32_t g_dragId = 0;
	float    g_p0[3] = {}, g_dir0[3] = {}, g_s0 = 0.0f, g_x0[3] = {}, g_v0[3] = {}, g_sc0 = 1.0f;
	ImVec2   g_grab;
	int      g_rtLogs = 0;
	bool     g_openLogged = false;

	enum { H_NONE = 0, H_X, H_Y, H_Z, H_PLANE, H_RX, H_RY, H_RZ, H_SCALE };
	const float  kAxis[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
	const ImU32  kAxisCol[3] = { IM_COL32(235, 70, 70, 255), IM_COL32(90, 210, 90, 255), IM_COL32(80, 140, 255, 255) };
	const ImU32  kHot  = IM_COL32(255, 215, 60, 255);
	const ImU32  kBeam = IM_COL32(255, 170, 60, 230);
	const ImU32  kPick = IM_COL32(120, 220, 255, 230);
	const ImVec4 kHotText(1.0f, 0.84f, 0.24f, 1.0f);

	float segDist(const ImVec2& p, const ImVec2& a, const ImVec2& b)
	{
		const float abx = b.x - a.x, aby = b.y - a.y, apx = p.x - a.x, apy = p.y - a.y;
		const float l2 = abx * abx + aby * aby;
		float t = l2 > 0.0f ? (apx * abx + apy * aby) / l2 : 0.0f;
		t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
		const float dx = apx - abx * t, dy = apy - aby * t;
		return sqrtf(dx * dx + dy * dy);
	}

	float pixDist(const ImVec2& a, const ImVec2& b) { const float dx = a.x - b.x, dy = a.y - b.y; return sqrtf(dx * dx + dy * dy); }

	ImU32 colorOf(const float c[3], int alpha)
	{
		auto b = [](float v) { v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; return (int)(v * 255.0f + 0.5f); };
		return IM_COL32(b(c[0]), b(c[1]), b(c[2]), alpha);
	}

	// The time an edit of an animated light goes to: the open marker, else the playhead.
	float keyTime(const lights::Ctx& c, bool* atMarker)
	{
		*atMarker = c.marker >= 0.0f;
		return *atMarker ? c.marker : c.now;
	}

	// What the window shows for a light right now (in its own frame).
	lights::Params shownParams(const lights::Light& L, const lights::Ctx& c)
	{
		if (L.nkeys == 0) return L.base;
		bool m;
		const float t = keyTime(c, &m);
		const int k = lights::keyAt(L, t);
		return k >= 0 ? L.keys[k].p : lights::evalLight(L, t);
	}

	// Where an edit lands: the whole clip, or the key at the key time - made
	// from what the light looks like there when there is none yet.
	lights::Params* editTarget(lights::Light& L, const lights::Ctx& c)
	{
		if (L.nkeys == 0) return &L.base;
		bool m;
		const float t = keyTime(c, &m);
		int k = lights::keyAt(L, t);
		if (k < 0) k = lights::insertKey(L, t, lights::evalLight(L, t));
		return k >= 0 ? &L.keys[k].p : nullptr;
	}

	// `p` (in the light's frame) in the world right now. False when what the
	// light follows is not in the scene at this moment.
	bool worldOf(const lights::Light& L, const lights::Params& p, lights::Params& w)
	{
		if (L.at.mode == lights::A_NONE) { w = p; return true; }
		if (!L.parentOk) return false;
		w = lights::toWorld(L.parent, p);
		return true;
	}

	// The frame the light's numbers are kept in: nullptr for the world. `ok`
	// is false when it follows something not in the scene right now - then
	// nothing can be placed from the world.
	const lights::Frame* editFrame(const lights::Light& L, bool* ok)
	{
		*ok = true;
		if (L.at.mode == lights::A_NONE) return nullptr;
		if (!L.parentOk) { *ok = false; return nullptr; }
		return &L.parent;
	}

	void putPos(lights::Params& p, const lights::Frame* F, const float w[3])
	{
		if (F) lights::toFrame(*F, w, p.pos);
		else memcpy(p.pos, w, sizeof(p.pos));
	}

	void putDir(lights::Params& p, const lights::Frame* F, const float w[3])
	{
		if (F) lights::dirToFrame(*F, w, p.dir);
		else memcpy(p.dir, w, sizeof(p.dir));
	}

	void arrowHead(ImDrawList* dl, const ImVec2& from, const ImVec2& to, ImU32 col)
	{
		float ux = to.x - from.x, uy = to.y - from.y;
		const float l = sqrtf(ux * ux + uy * uy);
		if (l < 1.0f) return;
		ux /= l; uy /= l;
		dl->AddTriangleFilled(ImVec2(to.x + ux * 11.0f, to.y + uy * 11.0f), ImVec2(to.x - uy * 5.0f, to.y + ux * 5.0f),
			ImVec2(to.x + uy * 5.0f, to.y - ux * 5.0f), col);
	}

	// Every light as a dot with its name; returns the id of the one under the
	// mouse. The selected one, when it follows an entity, is tied to it by a line.
	uint32_t drawMarkers(ImDrawList* dl, const lights::View& v, const lights::LightSet& S, const lights::Ctx& c, const ImVec2& mouse, bool hover)
	{
		uint32_t under = 0;
		float best = 13.0f;
		for (int i = 0; i < S.count; ++i)
		{
			const lights::Light& L = S.l[i];
			const bool sel = L.id == g_sel;
			lights::Params P;
			if (!worldOf(L, sel ? shownParams(L, c) : lights::evalLight(L, c.now), P)) continue;
			float s2[2];
			if (!lights::toScreen(v, P.pos, s2)) continue;
			const ImVec2 s(s2[0], s2[1]);
			if (sel && L.at.mode == lights::A_ENTITY)
			{
				float o2[2];
				if (lights::toScreen(v, L.parent.d, o2))
				{
					dl->AddLine(s, ImVec2(o2[0], o2[1]), IM_COL32(255, 215, 60, 140), 1.5f);
					dl->AddRect(ImVec2(o2[0] - 5.0f, o2[1] - 5.0f), ImVec2(o2[0] + 5.0f, o2[1] + 5.0f), IM_COL32(255, 215, 60, 200), 0.0f, 0, 1.5f);
				}
			}
			dl->AddCircleFilled(s, sel ? 7.0f : 6.0f, L.enabled ? colorOf(P.col, 255) : IM_COL32(90, 90, 90, 200));
			dl->AddCircle(s, sel ? 7.5f : 6.5f, IM_COL32(0, 0, 0, 220), 0, 1.5f);
			if (sel) dl->AddCircle(s, 12.0f, kHot, 0, 2.0f);
			dl->AddText(ImVec2(s.x + 11.0f, s.y - 8.0f), L.enabled ? IM_COL32(255, 255, 255, 235) : IM_COL32(160, 160, 160, 200), L.name);
			if (hover) { const float d = pixDist(mouse, s); if (d < best) { best = d; under = L.id; } }
		}
		return under;
	}

	// While picking: every ped, vehicle and prop near the camera as a box with
	// its kind and distance. Returns the one under the mouse, or -1.
	// `only` >= 0 draws just that one (the row the mouse is on in the list);
	// -1 draws them all, which is what picking in the picture wants.
	int drawScene(ImDrawList* dl, const lights::View& v, const lights::Scene& sc, const ImVec2& mouse, bool hover, int only)
	{
		int under = -1;
		float best = 16.0f;
		ImVec2 at[lights::kMaxScene];
		bool on[lights::kMaxScene];
		for (int i = 0; i < sc.count; ++i)
		{
			float s2[2];
			on[i] = lights::toScreen(v, sc.it[i].f.d, s2);
			at[i] = ImVec2(s2[0], s2[1]);
			if (on[i] && hover) { const float d = pixDist(mouse, at[i]); if (d < best) { best = d; under = i; } }
		}
		for (int i = 0; i < sc.count; ++i)
		{
			if (!on[i]) continue;
			if (only >= 0 && i != only) continue;
			const ImVec2 s = at[i];
			const bool hot = i == under || i == only;
			const ImU32 col = hot ? kHot : kPick;
			dl->AddRect(ImVec2(s.x - 7.0f, s.y - 7.0f), ImVec2(s.x + 7.0f, s.y + 7.0f), col, 0.0f, 0, hot ? 3.0f : 2.0f);
			char label[48];
			snprintf(label, sizeof(label), "%s  %.0f m", lights::kindName(sc.it[i].kind), sc.it[i].dist);
			dl->AddText(ImVec2(s.x + 11.0f, s.y - 8.0f), hot ? kHot : IM_COL32(230, 245, 255, 235), label);
		}
		return under;
	}

	// The gizmo of the selected light (world numbers). Returns the handle under the mouse.
	int drawGizmo(ImDrawList* dl, const lights::View& v, const lights::Params& P, bool spot, const ImVec2& mouse, bool hover)
	{
		float c2[2];
		if (!lights::toScreen(v, P.pos, c2)) return H_NONE;
		const ImVec2 c(c2[0], c2[1]);
		const float d[3] = { P.pos[0] - v.c.pos[0], P.pos[1] - v.c.pos[1], P.pos[2] - v.c.pos[2] };
		const float len = sqrtf(lights::dot3(d, d)) * 0.22f * v.tanV;   // about a tenth of the screen height
		int hot = H_NONE;
		float best = 1e9f;

		// the beam of a spot, in every mode
		if (spot)
		{
			const float tip[3] = { P.pos[0] + P.dir[0] * len * 1.8f, P.pos[1] + P.dir[1] * len * 1.8f, P.pos[2] + P.dir[2] * len * 1.8f };
			float t2[2];
			if (lights::toScreen(v, tip, t2)) { dl->AddLine(c, ImVec2(t2[0], t2[1]), kBeam, 2.0f); dl->AddCircleFilled(ImVec2(t2[0], t2[1]), 4.0f, kBeam); }
		}

		if (g_mode == 2)
		{
			// scale: three arms ending in squares, and the centre square. Any
			// of them scales the whole light.
			ImVec2 tips[3];
			bool ok[3];
			bool onIt = hover && pixDist(mouse, c) < 10.0f;
			for (int a = 0; a < 3; ++a)
			{
				const float tip[3] = { P.pos[0] + kAxis[a][0] * len * 0.8f, P.pos[1] + kAxis[a][1] * len * 0.8f, P.pos[2] + kAxis[a][2] * len * 0.8f };
				float t2[2];
				ok[a] = lights::toScreen(v, tip, t2);
				tips[a] = ImVec2(t2[0], t2[1]);
				if (ok[a] && hover && (segDist(mouse, c, tips[a]) < 8.0f || pixDist(mouse, tips[a]) < 9.0f)) onIt = true;
			}
			if (onIt) hot = H_SCALE;
			const bool lit = g_drag == H_SCALE || (!g_drag && hot == H_SCALE);
			for (int a = 0; a < 3; ++a)
			{
				if (!ok[a]) continue;
				const ImU32 col = lit ? kHot : kAxisCol[a];
				dl->AddLine(c, tips[a], col, 3.0f);
				dl->AddRectFilled(ImVec2(tips[a].x - 5.0f, tips[a].y - 5.0f), ImVec2(tips[a].x + 5.0f, tips[a].y + 5.0f), col);
			}
			dl->AddRectFilled(ImVec2(c.x - 6.0f, c.y - 6.0f), ImVec2(c.x + 6.0f, c.y + 6.0f), lit ? kHot : IM_COL32(240, 240, 240, 230));
			dl->AddRect(ImVec2(c.x - 6.0f, c.y - 6.0f), ImVec2(c.x + 6.0f, c.y + 6.0f), IM_COL32(0, 0, 0, 200));
			// how far a point light reaches: its sphere as the camera sees it
			if (!spot)
			{
				const float z = lights::dot3(d, v.c.fwd);
				if (z > 0.05f)
				{
					const float r = lights::reachOf(P) / z * (v.h * 0.5f) / v.tanV;
					if (r > 2.0f && r < 20000.0f) dl->AddCircle(c, r, IM_COL32(255, 255, 255, 110), 0, 1.5f);
				}
			}
			return hot;
		}

		if (!spot || g_mode == 0)
		{
			ImVec2 tips[3];
			bool ok[3];
			for (int a = 0; a < 3; ++a)
			{
				const float tip[3] = { P.pos[0] + kAxis[a][0] * len, P.pos[1] + kAxis[a][1] * len, P.pos[2] + kAxis[a][2] * len };
				float t2[2];
				ok[a] = lights::toScreen(v, tip, t2);
				tips[a] = ImVec2(t2[0], t2[1]);
				if (ok[a] && hover)
				{
					const float dd = segDist(mouse, c, tips[a]);
					if (dd < 8.0f && dd < best) { best = dd; hot = H_X + a; }
				}
			}
			if (hover && pixDist(mouse, c) < 10.0f) hot = H_PLANE;   // the centre wins
			for (int a = 0; a < 3; ++a)
			{
				if (!ok[a]) continue;
				const ImU32 col = (g_drag == H_X + a || (!g_drag && hot == H_X + a)) ? kHot : kAxisCol[a];
				dl->AddLine(c, tips[a], col, 3.0f);
				arrowHead(dl, c, tips[a], col);
			}
			const ImU32 pc = (g_drag == H_PLANE || (!g_drag && hot == H_PLANE)) ? kHot : IM_COL32(240, 240, 240, 230);
			dl->AddRectFilled(ImVec2(c.x - 6.0f, c.y - 6.0f), ImVec2(c.x + 6.0f, c.y + 6.0f), pc);
			dl->AddRect(ImVec2(c.x - 6.0f, c.y - 6.0f), ImVec2(c.x + 6.0f, c.y + 6.0f), IM_COL32(0, 0, 0, 200));
			return hot;
		}

		// rotate: a ring about each world axis
		for (int a = 0; a < 3; ++a)
		{
			const float* ax = kAxis[a];
			const float helper[3] = { 0.0f, 0.0f, fabsf(ax[2]) > 0.9f ? 0.0f : 1.0f };
			const float helperX[3] = { 1.0f, 0.0f, 0.0f };
			float u[3], w[3];
			lights::cross3(ax, fabsf(ax[2]) > 0.9f ? helperX : helper, u);
			lights::norm3(u);
			lights::cross3(ax, u, w);
			ImVec2 prev;
			bool havePrev = false;
			float dmin = 1e9f;
			ImVec2 pts[49];
			bool okp[49];
			for (int s = 0; s <= 48; ++s)
			{
				const float th = (float)s / 48.0f * 6.2831853f, cs = cosf(th), sn = sinf(th);
				const float p[3] = { P.pos[0] + (u[0] * cs + w[0] * sn) * len, P.pos[1] + (u[1] * cs + w[1] * sn) * len, P.pos[2] + (u[2] * cs + w[2] * sn) * len };
				float t2[2];
				okp[s] = lights::toScreen(v, p, t2);
				pts[s] = ImVec2(t2[0], t2[1]);
				if (okp[s] && havePrev) { const float dd = segDist(mouse, prev, pts[s]); if (dd < dmin) dmin = dd; }
				prev = pts[s];
				havePrev = okp[s];
			}
			if (hover && dmin < 7.0f && dmin < best) { best = dmin; hot = H_RX + a; }
			const ImU32 col = (g_drag == H_RX + a || (!g_drag && hot == H_RX + a)) ? kHot : kAxisCol[a];
			for (int s = 1; s <= 48; ++s) if (okp[s] && okp[s - 1]) dl->AddLine(pts[s - 1], pts[s], col, 2.5f);
		}
		dl->AddCircleFilled(c, 4.0f, IM_COL32(240, 240, 240, 230));
		return hot;
	}

	// A drag works on world numbers and stores them back in the light's frame.
	void beginDrag(int h, lights::Light& L, const lights::Ctx& c, const lights::View& v, const ImVec2& mouse)
	{
		bool fok;
		const lights::Frame* F = editFrame(L, &fok);
		if (!fok) return;
		lights::Params* T = editTarget(L, c);
		if (!T) return;
		const lights::Params W = F ? lights::toWorld(*F, *T) : *T;
		float rdir[3];
		lights::rayAt(v, mouse.x, mouse.y, rdir);
		memcpy(g_p0, W.pos, sizeof(g_p0));
		memcpy(g_dir0, W.dir, sizeof(g_dir0));
		bool ok = false;
		if (h >= H_X && h <= H_Z) ok = lights::axisParam(g_p0, kAxis[h - H_X], v.c.pos, rdir, &g_s0);
		else if (h == H_PLANE)    ok = lights::rayPlane(v.c.pos, rdir, g_p0, v.c.fwd, g_x0);
		else if (h >= H_RX && h <= H_RZ)
		{
			float x[3];
			ok = lights::rayPlane(v.c.pos, rdir, g_p0, kAxis[h - H_RX], x);
			if (ok)
			{
				for (int i = 0; i < 3; ++i) g_v0[i] = x[i] - g_p0[i];
				ok = lights::norm3(g_v0);
			}
		}
		else if (h == H_SCALE) { g_sc0 = lights::scaleOf(*T); g_grab = mouse; ok = true; }
		if (ok) { g_drag = h; g_dragId = L.id; }
	}

	void updateDrag(lights::Light& L, const lights::Ctx& c, const lights::View& v, const ImVec2& mouse, bool snap, float mult)
	{
		bool fok;
		const lights::Frame* F = editFrame(L, &fok);
		if (!fok) return;
		lights::Params* T = editTarget(L, c);
		if (!T) return;
		float rdir[3];
		lights::rayAt(v, mouse.x, mouse.y, rdir);
		if (g_drag >= H_X && g_drag <= H_Z)
		{
			const float* a = kAxis[g_drag - H_X];
			float s;
			if (!lights::axisParam(g_p0, a, v.c.pos, rdir, &s)) return;
			float d = (s - g_s0) * mult;
			if (snap) d = roundf(d / 0.25f) * 0.25f;
			float wp[3];
			for (int i = 0; i < 3; ++i) wp[i] = g_p0[i] + a[i] * d;
			putPos(*T, F, wp);
		}
		else if (g_drag == H_PLANE)
		{
			float x[3];
			if (!lights::rayPlane(v.c.pos, rdir, g_p0, v.c.fwd, x)) return;
			float wp[3];
			for (int i = 0; i < 3; ++i)
			{
				float d = (x[i] - g_x0[i]) * mult;
				if (snap) d = roundf(d / 0.25f) * 0.25f;
				wp[i] = g_p0[i] + d;
			}
			putPos(*T, F, wp);
		}
		else if (g_drag >= H_RX && g_drag <= H_RZ)
		{
			const float* a = kAxis[g_drag - H_RX];
			float x[3], w[3], cr[3];
			if (!lights::rayPlane(v.c.pos, rdir, g_p0, a, x)) return;
			for (int i = 0; i < 3; ++i) w[i] = x[i] - g_p0[i];
			if (!lights::norm3(w)) return;
			lights::cross3(g_v0, w, cr);
			float ang = atan2f(lights::dot3(cr, a), lights::dot3(g_v0, w)) * mult;
			if (snap) { const float st = 15.0f * lights::kDeg; ang = roundf(ang / st) * st; }
			float d[3] = { g_dir0[0], g_dir0[1], g_dir0[2] };
			lights::rotateAbout(d, a, ang);
			lights::norm3(d);
			putDir(*T, F, d);
		}
		else if (g_drag == H_SCALE)
		{
			// right or up grows it, left or down shrinks it: twice as big every 150 pixels
			float s = g_sc0 * exp2f(((mouse.x - g_grab.x) - (mouse.y - g_grab.y)) * mult / 150.0f);
			if (snap) s = roundf(s * 10.0f) / 10.0f;
			T->scale = lights::clampf(s, 0.05f, 20.0f);
		}
	}

	// How far the light is from the camera right now.
	float distToCam(const lights::Light& L, const lights::Ctx& c)
	{
		lights::Params W;
		if (!worldOf(L, shownParams(L, c), W)) return 10.0f;
		const float d = sqrtf(lights::dist2(W.pos, c.cam.pos));
		return d > 0.25f ? d : 0.25f;
	}

	// Put the light on the ray under the cursor, `dist` metres from the camera.
	// Up close the arrows move a light by centimetres per screen pixel - this is
	// the way to throw it across the scene without flying the camera back first.
	bool placeOnRay(lights::Light& L, const lights::Ctx& c, const lights::View& v, const ImVec2& mouse, float dist)
	{
		bool fok;
		const lights::Frame* F = editFrame(L, &fok);
		if (!fok) return false;
		lights::Params* T = editTarget(L, c);
		if (!T) return false;
		float rdir[3], wp[3];
		lights::rayAt(v, mouse.x, mouse.y, rdir);
		for (int i = 0; i < 3; ++i) wp[i] = c.cam.pos[i] + rdir[i] * dist;
		putPos(*T, F, wp);
		return true;
	}

	// The wheel pushes the light away from the camera or pulls it closer, along
	// its own line of sight.
	bool pushPull(lights::Light& L, const lights::Ctx& c, float notches, bool fast, bool snap)
	{
		lights::Params W;
		if (!worldOf(L, shownParams(L, c), W)) return false;
		float dir[3];
		for (int i = 0; i < 3; ++i) dir[i] = W.pos[i] - c.cam.pos[i];
		const float dist = sqrtf(lights::dot3(dir, dir));
		if (!lights::norm3(dir)) return false;
		const float step = (dist * 0.04f > 0.05f ? dist * 0.04f : 0.05f) * (fast ? 5.0f : 1.0f);
		float target = dist + notches * step;
		if (snap) target = roundf(target / 0.25f) * 0.25f;
		if (target < 0.1f) target = 0.1f;
		bool fok;
		const lights::Frame* F = editFrame(L, &fok);
		if (!fok) return false;
		lights::Params* T = editTarget(L, c);
		if (!T) return false;
		float wp[3];
		for (int i = 0; i < 3; ++i) wp[i] = c.cam.pos[i] + dir[i] * target;
		putPos(*T, F, wp);
		return true;
	}

	void flagBox(const char* label, lights::Light& L, uint32_t bit, const char* what)
	{
		bool on = (L.flags & bit) != 0;
		if (ImGui::Checkbox(label, &on)) { L.flags = on ? (L.flags | bit) : (L.flags & ~bit); lights::markDirty(); }
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", what);
	}

	void selectAdded(lights::LightSet& S, int i)
	{
		if (i < 0) return;
		g_sel = S.l[i].id;
		g_mode = 0;
		g_picking = false;
		lights::markDirty();
	}

	// n drag fields on one row ("X 1.00  Y 2.00  Z 3.00"), then the label.
	struct Axis { const char* fmt; float lo, hi; };

	bool dragAxes(int id, const char* label, float* v, int n, float speed, const Axis* ax)
	{
		ImGui::PushID(id);
		const float gap = ImGui::GetStyle().ItemInnerSpacing.x;
		const float w = (ImGui::CalcItemWidth() - gap * (float)(n - 1)) / (float)n;
		bool changed = false;
		for (int i = 0; i < n; ++i)
		{
			if (i) ImGui::SameLine(0.0f, gap);
			ImGui::PushID(i);
			ImGui::SetNextItemWidth(w);
			if (ImGui::DragFloat("##v", &v[i], speed, ax[i].lo, ax[i].hi, ax[i].fmt)) changed = true;
			ImGui::PopID();
		}
		ImGui::SameLine(0.0f, gap);
		ImGui::TextUnformatted(label);
		ImGui::PopID();
		return changed;
	}

	// "Light editor key: L  [Change]", or the prompt while a new one is picked.
	void shortcutUi(int src)
	{
		char k[32];
		const bool picking = lights::g_capture != 0;
		ImGui::AlignTextToFramePadding();
		if (picking)
		{
			ImGui::TextColored(kHotText, "Press the new key...");
			ImGui::SameLine();
			if (ImGui::Button("Cancel##key")) lights::cancelCapture(lights::END_CANCELLED, "Cancelled");
		}
		else
		{
			ImGui::Text("Light editor key: %s", keyName(g_cfg.keyLights, k, sizeof(k)));
			ImGui::SameLine();
			if (ImGui::Button("Change##key")) lights::beginCapture(src);
		}
		if (picking || lights::since(GetTickCount64(), lights::g_keyMsgAt) < 6000) ImGui::TextDisabled("%s", lights::g_keyMsg);
	}

	// Every key and mouse action in the editor, and what it does. The window is
	// the only place to learn them, so they are listed rather than hinted at.
	void keysUi()
	{
		if (!ImGui::CollapsingHeader("Keys & mouse", ImGuiTreeNodeFlags_DefaultOpen)) return;
		char k[32], kf[32];
		keyName(g_cfg.keyLights, k, sizeof(k));
		keyName(g_cfg.keyFreeLook, kf, sizeof(kf));
		struct Row { const char* key; const char* what; };
		const Row rows[] = {
			{ k,                    "opens and closes this editor, while a clip is open in the Rockstar Editor" },
			{ "Esc",                "closes the editor - or stops 'Pick in the scene' first" },
			{ "left click",         "selects the light under the cursor, or takes hold of the gizmo handle under it" },
			{ "W",                  "Move: drag an arrow to slide the light along X, Y or Z, the white square to move it across the screen" },
			{ "E",                  "Rotate: drag a ring to aim a spot (a point light has no direction)" },
			{ "R",                  "Scale: drag right or up to make the light bigger, left or down smaller" },
			{ "Alt + click",        "puts the light under the cursor keeping its distance, and keeps dragging it - the way to place a light while the camera is close by" },
			{ "mouse wheel",        "pushes the light away from the camera, or pulls it closer" },
			{ "Shift, dragging",    "6x faster - for crossing the scene" },
			{ "Alt, dragging",      "finer - for the last few centimetres" },
			{ "Ctrl, dragging",     "snaps to 25 cm, 15 degrees, or 0.1 of scale" },
			{ "Ctrl + click a box", "types an exact number into that field" },
			{ "Del",                "deletes the selected light" },
			{ kf,                   "Free Look, in Edit Camera: fly without changing this keyframe's camera. Press it again and the keyframe's own camera is back, so you see the shot as it will render. It is also the Free Look row on the EscoEditor menu's Camera Speed page." },
		};
		if (ImGui::BeginTable("##keyshelp", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthStretch);
			for (const Row& r : rows)
			{
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextColored(kHotText, "%s", r.key);
				ImGui::TableNextColumn();
				ImGui::TextWrapped("%s", r.what);
			}
			ImGui::EndTable();
		}
	}

	void helpLine()
	{
		char k[32];
		ImGui::TextDisabled("%s or Esc closes this editor.", keyName(g_cfg.keyLights, k, sizeof(k)));
		keysUi();
	}

	// An animated light's keys: one row each with its time and, at the right
	// edge, a Delete button; the key the edits go to is marked. The box grows
	// by dragging its bottom edge.
	void keyList(lights::Light& L, float kt)
	{
		int kill = -1;
		const float boxH = ImGui::GetFrameHeightWithSpacing() * 4.0f + ImGui::GetStyle().WindowPadding.y * 2.0f;
		ImGui::BeginChild("##keys", ImVec2(0.0f, boxH), ImGuiChildFlags_Border | ImGuiChildFlags_ResizeY);
		if (ImGui::BeginTable("##keytable", 2, ImGuiTableFlags_RowBg))
		{
			ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("delete", ImGuiTableColumnFlags_WidthFixed);
			for (int k = 0; k < L.nkeys; ++k)
			{
				ImGui::PushID(k);
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::AlignTextToFramePadding();
				const float t = L.keys[k].t / 1000.0f;
				if (fabsf(L.keys[k].t - kt) <= 5.0f) ImGui::TextColored(kHotText, "Key %d    %.2f s    editing", k + 1, t);
				else ImGui::Text("Key %d    %.2f s", k + 1, t);
				ImGui::TableNextColumn();
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.78f, 0.20f, 0.20f, 0.90f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.62f, 0.12f, 0.12f, 1.00f));
				if (ImGui::Button("Delete")) kill = k;
				ImGui::PopStyleColor(3);
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
		ImGui::EndChild();
		if (kill >= 0)
		{
			const lights::Params last = L.keys[kill].p;
			lights::deleteKey(L, kill);
			if (L.nkeys == 0) L.base = last;
			lights::markDirty();
		}
	}

	void tip(const char* text)
	{
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", text);
	}

	// What the light follows, and the buttons that change it. Changing it keeps
	// the light where it is now (the whole clip and every key move into the
	// new frame).
	void attachUi(lights::Light& L, const lights::Ctx& c, bool haveCam)
	{
		char what[96];
		if (L.at.mode == lights::A_CAMERA) snprintf(what, sizeof(what), "the camera");
		else if (L.at.mode == lights::A_ENTITY)
		{
			if (L.parentOk)
				snprintf(what, sizeof(what), "%s %08X, %.1f m away", lights::kindName(L.at.kind), L.at.model, sqrtf(lights::dist2(L.parent.d, c.cam.pos)));
			else
				snprintf(what, sizeof(what), "%s %08X - not in the scene here", lights::kindName(L.at.kind), L.at.model);
		}
		else snprintf(what, sizeof(what), "nothing (a fixed place)");
		ImGui::AlignTextToFramePadding();
		ImGui::Text("Follows: %s", what);

		const lights::Frame camF = lights::frameOf(c.cam);
		const lights::Frame* fallback = haveCam ? &camF : nullptr;
		ImGui::BeginDisabled(L.at.mode == lights::A_NONE);
		if (ImGui::Button("World"))
		{
			lights::detach(L, fallback);
			lights::markDirty();
			logf("lights: '%s' follows nothing now", L.name);
		}
		ImGui::EndDisabled();
		tip("Stop following: the light stays where it is now, fixed in the world.");
		ImGui::SameLine();
		ImGui::BeginDisabled(!haveCam || L.at.mode == lights::A_CAMERA);
		if (ImGui::Button("Camera"))
			if (lights::attachTo(L, lights::A_CAMERA, camF, nullptr, 0, 0, fallback))
			{
				lights::markDirty();
				logf("lights: '%s' follows the camera now", L.name);
			}
		ImGui::EndDisabled();
		tip("Follow the camera: the light moves and turns with it.");
		ImGui::SameLine();
		if (g_picking)
		{
			if (ImGui::Button("Cancel pick")) g_picking = false;
		}
		else
		{
			ImGui::BeginDisabled(!haveCam || !lights::g_canPick);
			if (ImGui::Button("Pick in the scene")) g_picking = true;
			ImGui::EndDisabled();
			tip(lights::g_canPick ? "Then click a ped, a vehicle or a prop in the picture: the light follows it."
								  : "The game's entity lists were not found on this build (see EscoEditor.log).");
		}
		ImGui::SameLine();
		if (ImGui::Button(g_showList ? "Hide the list" : "From a list")) g_showList = !g_showList;
		tip("Lists what the game has near the camera - peds, peds in cars, vehicles and props - nearest first, so a vehicle can be picked without hunting for it in the picture.");
		if (g_picking) ImGui::TextColored(kHotText, "Click a ped, vehicle or prop in the picture. Esc cancels.");

		ImGui::Checkbox("Jump to it", &g_jump);
		tip("On: the light lands a metre above the middle of the ped, vehicle or prop it starts following - the whole clip and every key move together, so keyed movement is kept. Off: the light stays where it is in the world and follows from there.");

		if (g_showList)
		{
			const lights::Scene sc = lights::scene();
			const bool fresh = lights::since(GetTickCount64(), sc.at) < 1000;
			int n[lights::E_KINDS] = {};
			for (int i = 0; i < sc.count; ++i)
				if (sc.it[i].kind >= 0 && sc.it[i].kind < lights::E_KINDS) ++n[sc.it[i].kind];
			ImGui::TextDisabled("Within 150 m: %d ped(s), %d vehicle(s), %d prop(s)%s",
				n[lights::E_PED], n[lights::E_VEHICLE], n[lights::E_OBJECT], fresh ? "" : "  (waiting for the game thread)");
			for (int k = 0; k < lights::E_KINDS; ++k)
			{
				if (k) ImGui::SameLine();
				ImGui::Checkbox(k == lights::E_PED ? "Peds" : k == lights::E_VEHICLE ? "Vehicles" : "Props", &g_kindOn[k]);
			}
			tip("Which kinds the list shows. The game keeps peds, vehicles and props in three separate lists.");

			// nearest first, so the one just in front is the top row
			int order[lights::kMaxScene];
			int m = 0;
			for (int i = 0; i < sc.count; ++i)
				if (sc.it[i].kind >= 0 && sc.it[i].kind < lights::E_KINDS && g_kindOn[sc.it[i].kind]) order[m++] = i;
			for (int a = 1; a < m; ++a)
			{
				const int cur = order[a];
				int b = a - 1;
				while (b >= 0 && sc.it[order[b]].dist > sc.it[cur].dist) { order[b + 1] = order[b]; --b; }
				order[b + 1] = cur;
			}

			g_listHover = -1;
			ImGui::BeginChild("##scene", ImVec2(0.0f, 120.0f), ImGuiChildFlags_Border | ImGuiChildFlags_ResizeY);
			for (int a = 0; a < m; ++a)
			{
				const lights::SceneItem& it = sc.it[order[a]];
				char row[96];
				snprintf(row, sizeof(row), "%-8s %08X   %5.1f m", lights::kindName(it.kind), it.model, it.dist);
				ImGui::PushID(a);
				const bool mine = L.at.mode == lights::A_ENTITY && it.ent == L.ent;
				if (ImGui::Selectable(row, mine))
				{
					if (lights::attachTo(L, lights::A_ENTITY, it.f, it.ent, it.kind, it.model, fallback))
					{
						bool am = false;
						if (g_jump) lights::jumpToParent(L, keyTime(c, &am));
						lights::markDirty();
						logf("lights: '%s' follows %s %08X now (from the list), %.1f m from the camera%s",
							L.name, lights::kindName(it.kind), it.model, it.dist, g_jump ? " - moved onto it" : "");
					}
				}
				if (ImGui::IsItemHovered()) g_listHover = order[a];
				ImGui::PopID();
			}
			if (m == 0)
				ImGui::TextDisabled(sc.count ? "Nothing of the ticked kinds is near the camera."
											 : "Nothing yet - play or scrub the clip once, so the game fills its lists.");
			ImGui::EndChild();
			ImGui::TextDisabled("A row is remembered by its model, so the replay rebuilding the scene still finds it.");
		}
	}

	// The clip's time of day and weather: the same two overrides the EscoEditor
	// menu offers, in reach while the lights are being set - lighting work is
	// mostly a question of which hour it is. Both last for the session only and
	// are deliberately never saved, so a forgotten override cannot colour a
	// later clip.
	void sceneUi(const lights::Ctx* ctx)
	{
		if (!ImGui::CollapsingHeader("Time & weather")) return;
		if (!scene::g_installed)
		{
			ImGui::TextDisabled("Not hooked on this game build - EscoEditor.log says why.");
			return;
		}

		int t = scene::g_time;
		bool onTime = t >= 0;
		if (ImGui::Checkbox("Set the time of day", &onTime))
		{
			scene::g_time = onTime ? (t < 0 ? 12 * 60 : t) : -1;
			t = scene::g_time;
		}
		tip("Off plays the clip at the hour it was recorded. On holds the whole clip at the hour you pick - the same as Time of Day on the EscoEditor menu, and it is in the game's own export too.");
		if (t >= 0)
		{
			char stamp[16];
			snprintf(stamp, sizeof(stamp), "%02d:%02d", t / 60, t % 60);
			float hours = (float)t / 60.0f;
			if (ImGui::DragFloat("Time", &hours, 0.02f, 0.0f, 23.9833f, stamp))
			{
				int m = (int)(hours * 60.0f + 0.5f);
				scene::g_time = m < 0 ? 0 : m > 24 * 60 - 1 ? 24 * 60 - 1 : m;
			}
			tip("Drag for the hour and the minute. Ctrl+click to type it as hours.");
			struct Hour { const char* name; int at; };
			static const Hour kHours[] = { { "Dawn", 6 * 60 + 15 }, { "Noon", 12 * 60 }, { "Golden", 19 * 60 + 15 }, { "Night", 23 * 60 } };
			for (int i = 0; i < 4; ++i)
			{
				if (i) ImGui::SameLine();
				if (ImGui::Button(kHours[i].name)) scene::g_time = kHours[i].at;
				tip("Straight to the light of that hour: first light, midday, the golden hour and the middle of the night.");
			}
		}

		ImGui::Spacing();
		int w = scene::g_weather;
		bool onW = w >= 0;
		if (ImGui::Checkbox("Set the weather", &onW)) scene::g_weather = onW ? (w < 0 ? 1 : w) : -1;
		tip("Off plays the clip's own weather. On holds this one - the same as the Weather row on the EscoEditor menu.");
		w = scene::g_weather;
		if (w >= 0)
		{
			if (ImGui::BeginCombo("Weather", scene::weatherName()))
			{
				for (int i = 0; i < scene::kWeatherCount; ++i)
					if (ImGui::Selectable(scene::kWeather[i], i == w)) scene::g_weather = i;
				ImGui::EndCombo();
			}
			tip("The sky, the clouds and the light of that weather. Rain and Thunder also wet the road; Foggy and Smog sit nicely behind a volumetric light.");
		}
		// ---- keyed over the clip, the way a light is ----------------------
		lights::LightSet* S = ctx ? lights::cur() : nullptr;
		if (!S)
		{
			ImGui::TextDisabled("Both last for this session and are not saved. Open a clip to key them over its length.");
			return;
		}

		ImGui::Spacing();
		ImGui::Separator();
		bool atMarker = false;
		const float kt = keyTime(*ctx, &atMarker);
		bool anim = S->sky.n > 0;
		if (ImGui::Checkbox("Animate over the clip", &anim))
		{
			if (anim) lights::addSkyKey(S->sky, kt, scene::g_time, scene::g_weather);
			else      S->sky.n = 0;
			lights::markDirty();
			logf("scene: time and weather %s for clip %d of '%s'",
				anim ? "keyed over the clip" : "back to one setting for the session",
				lights::g_curClip + 1, lights::g_curProject);
		}
		tip("On: the clip carries its own time and weather, keyed over its length - sunset arriving across a shot, clear turning to thunder.\n"
			"The time runs in minutes and takes the short way round midnight. The weather uses the game's own blend between one key and the next, so it does not step.\n"
			"The keys are saved with the clip's lights, and Copy from brings them over with everything else.");

		if (S->sky.n <= 0)
		{
			ImGui::TextDisabled("Not keyed: the two settings above hold for the whole session.");
			return;
		}

		ImGui::TextDisabled("Keys go in at the %s (%.2f s). The two settings above are what a new key holds.",
			atMarker ? "open marker" : "playhead", kt / 1000.0f);
		if (ImGui::Button("Key here"))
		{
			lights::addSkyKey(S->sky, kt, scene::g_time, scene::g_weather);
			lights::markDirty();
		}
		tip("Puts the time and the weather set above into a key at this moment. A key already at this moment is replaced.");
		ImGui::SameLine();
		if (ImGui::Button("Clear keys")) { S->sky.n = 0; lights::markDirty(); }
		tip("Removes every key, leaving the two settings above to hold for the session.");

		ImGui::BeginChild("##skykeys", ImVec2(0.0f, 120.0f), ImGuiChildFlags_Border | ImGuiChildFlags_ResizeY);
		int kill = -1;
		for (int i = 0; i < S->sky.n; ++i)
		{
			const lights::SceneKey& k = S->sky.k[i];
			char hhmm[16] = "as recorded";
			if (k.minutes >= 0) snprintf(hhmm, sizeof(hhmm), "%02d:%02d", k.minutes / 60, k.minutes % 60);
			char what[128];
			snprintf(what, sizeof(what), "%6.2f s    %s    %s", k.t / 1000.0f, hhmm,
				(k.weather >= 0 && k.weather < scene::kWeatherCount) ? scene::kWeather[k.weather] : "weather as recorded");
			ImGui::PushID(1000 + i);
			ImGui::AlignTextToFramePadding();
			if (fabsf(k.t - kt) <= 5.0f) ImGui::TextColored(kHotText, "%s   <- here", what);
			else                         ImGui::TextUnformatted(what);
			ImGui::SameLine();
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.78f, 0.20f, 0.20f, 0.90f));
			if (ImGui::Button("Delete")) kill = i;
			ImGui::PopStyleColor(2);
			ImGui::PopID();
		}
		ImGui::EndChild();
		if (kill >= 0) { lights::removeSkyKey(S->sky, kill); lights::markDirty(); }
	}

	// ENB's depth of field, driven by the clip: the switch, how it follows the
	// Rockstar Editor, every ENB DOF option, and keys over the clip.
	void enbUi(const lights::Ctx* ctx)
	{
		if (!ImGui::CollapsingHeader("ENB depth of field")) return;
		if (!enbdof::s_found)
		{
			ImGui::TextWrapped("ENBSeries with its SDK is not in the game (an ENB d3d11.dll exports ENBSetParameter). Without it there is nothing to drive.");
			return;
		}
		if (!enbdof::s_ready)
		{
			ImGui::TextWrapped("ENB found (SDK %ld) - waiting for enbdepthoffield.fx to load. Turn EnableDepthOfField on in ENB's [EFFECT] section.", enbdof::s_sdk);
			return;
		}

		bool on = g_cfg.enbDof;
		if (ImGui::Checkbox("ENB", &on)) enbdof::setOn(on);
		tip("On: ENB draws the depth of field, and it changes with the clip - every ENB DOF option below, keyed if you like, and the focus and strength of each Rockstar Editor keyframe.\n"
			"Off: ENB's own settings are put back exactly as they were.\n"
			"The same switch is the ENB row at the bottom of the Depth of Field menu. ENB's EnableDepthOfField has to be on.");

		bool follow = g_cfg.enbFollow;
		if (ImGui::Checkbox("Follow the Rockstar Editor DOF", &follow))
		{
			g_cfg.enbFollow = follow;
			iniWriteInt("EnbDofFollow", follow ? 1 : 0);
		}
		tip("Each keyframe's own Depth of Field drives ENB, blended from one keyframe to the next:\n"
			"Custom with manual focus sets the focus distance; its intensity (1 to 10) sets the aperture; None means no blur; Default leaves ENB's values alone.");
		if (g_cfg.enbFollow)
		{
			ImGui::SetNextItemWidth(160.0f);
			if (ImGui::DragFloat("Aperture per intensity step", &g_cfg.enbScale, 0.005f, 0.0f, 5.0f, "%.3f"))
			{
				char b[32];
				snprintf(b, sizeof(b), "%.4f", g_cfg.enbScale);
				WritePrivateProfileStringA(kSection, "EnbDofScale", b, g_ini);
			}
			tip("The keyframe's intensity times this is ENB's aperture: 0.25 makes intensity 2 an aperture of 0.5 and intensity 10 an aperture of 2.5.");
			if (enbdof::s_editorLine[0]) ImGui::TextDisabled("Now: %s", enbdof::s_editorLine);
		}

		// every option, grouped the way ENB's own window groups them
		ImGui::Spacing();
		ImGui::TextDisabled("ENB's options for this clip (what a new key holds):");
		for (int i = 0; i < enbdof::N; ++i)
		{
			const enbdof::Def& d = enbdof::kDefs[i];
			if (!enbdof::s_known[i]) continue;
			ImGui::PushID(2000 + i);
			float& v = enbdof::s_manual[i];
			if (d.kind == enbdof::K_BOOL)
			{
				bool b = v > 0.5f;
				if (ImGui::Checkbox(d.label, &b)) v = b ? 1.0f : 0.0f;
			}
			else if (d.kind == enbdof::K_INT)
			{
				int n = (int)floorf(v + 0.5f);
				if (ImGui::DragInt(d.label, &n, 0.1f, (int)d.lo, (int)d.hi)) v = (float)n;
			}
			else
			{
				const float speed = (d.hi - d.lo) > 50.0f ? 0.05f : 0.005f;
				ImGui::DragFloat(d.label, &v, speed, d.lo, d.hi, "%.3f");
			}
			if (g_cfg.enbFollow && (i == enbdof::I_DIST || i == enbdof::I_APERTURE || i == enbdof::I_AUTO_APERTURE))
				tip("Set by the Rockstar Editor keyframe while Follow is on (a Custom keyframe; focus distance only with manual focus).");
			ImGui::PopID();
		}
		if (ImGui::Button("Back to ENB's own values"))
			for (int i = 0; i < enbdof::N; ++i) enbdof::s_manual[i] = enbdof::s_base[i];
		tip("Every option above back to what ENB had before - the values in its enbdepthoffield.fx.ini.");

		// keys over the clip
		lights::LightSet* S = ctx ? lights::cur() : nullptr;
		if (!S) return;
		ImGui::Separator();
		bool atMarker = false;
		const float kt = keyTime(*ctx, &atMarker);
		bool anim = S->enb.n > 0;
		if (ImGui::Checkbox("Animate ENB over the clip", &anim))
		{
			if (anim) lights::addEnbKey(S->enb, kt, enbdof::s_manual);
			else      S->enb.n = 0;
			lights::markDirty();
		}
		tip("On: every ENB option above is keyed over the clip and blends from key to key - the switches change at the key. The Rockstar Editor's focus and strength still win while Follow is on.");
		if (S->enb.n <= 0) return;
		ImGui::TextDisabled("Keys go in at the %s (%.2f s).", atMarker ? "open marker" : "playhead", kt / 1000.0f);
		if (ImGui::Button("Key ENB here")) { lights::addEnbKey(S->enb, kt, enbdof::s_manual); lights::markDirty(); }
		tip("Puts every ENB option above into a key at this moment. A key already here is replaced.");
		ImGui::SameLine();
		if (ImGui::Button("Clear ENB keys")) { S->enb.n = 0; lights::markDirty(); }
		int kill = -1;
		ImGui::BeginChild("##enbkeys", ImVec2(0.0f, 100.0f), ImGuiChildFlags_Border | ImGuiChildFlags_ResizeY);
		for (int i = 0; i < S->enb.n; ++i)
		{
			const lights::EnbKey& k = S->enb.k[i];
			ImGui::PushID(3000 + i);
			ImGui::AlignTextToFramePadding();
			char what[128];
			int nset = 0;
			for (int b = 0; b < enbdof::N; ++b) if (k.set & (1u << b)) ++nset;
			snprintf(what, sizeof(what), "%6.2f s    %d option(s)    focus %.2f m    aperture %.2f", k.t / 1000.0f, nset,
				k.v[enbdof::I_DIST], k.v[enbdof::I_APERTURE]);
			if (fabsf(k.t - kt) <= 5.0f) ImGui::TextColored(kHotText, "%s   <- here", what);
			else                         ImGui::TextUnformatted(what);
			ImGui::SameLine();
			if (ImGui::Button("Load")) memcpy(enbdof::s_manual, k.v, sizeof(k.v));
			tip("Puts this key's values into the options above, to change and key again.");
			ImGui::SameLine();
			if (ImGui::Button("Delete")) kill = i;
			ImGui::PopID();
		}
		ImGui::EndChild();
		if (kill >= 0) { lights::removeEnbKey(S->enb, kill); lights::markDirty(); }
	}

	// The "EscoEditor Lights" window.
	void panel(lights::LightSet& S, const lights::Ctx& c, bool haveCam)
	{
		ImGui::SetNextWindowPos(ImVec2(40.0f, 90.0f), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(420.0f, 760.0f), ImGuiCond_FirstUseEver);
		bool keep = true;
		char title[72];
		snprintf(title, sizeof(title), "EscoEditor Lights %s###EscoEditorLights", kVersion);
		if (!ImGui::Begin(title, &keep, ImGuiWindowFlags_NoCollapse))
		{
			ImGui::End();
			if (!keep) g_open = false;
			return;
		}

		bool atMarker;
		const float kt = keyTime(c, &atMarker);
		ImGui::TextDisabled("Clip %d of %s", lights::g_curClip + 1, lights::g_curProject);
		if (atMarker) ImGui::TextDisabled("Marker %d at %.2f s", c.markerIndex, kt / 1000.0f);
		else          ImGui::TextDisabled("Playhead at %.2f s (no marker menu open)", kt / 1000.0f);
		if (!haveCam) ImGui::TextDisabled("No camera yet: play or scrub the clip once.");

		const lights::Cam* cam = haveCam ? &c.cam : nullptr;
		if (ImGui::Button("Add point")) selectAdded(S, lights::addLight(S, lights::T_POINT, cam));
		tip("A new light 3 m in front of the camera. A point light shines in every direction.");
		ImGui::SameLine();
		if (ImGui::Button("Add spot")) selectAdded(S, lights::addLight(S, lights::T_SPOT, cam));
		tip("A new light 1.5 m in front of the camera, aimed where the camera looks.");
		int si = lights::indexOf(S, g_sel);
		ImGui::SameLine();
		if (ImGui::Button("Duplicate") && si >= 0) selectAdded(S, lights::duplicateLight(S, si));
		tip("A copy of the selected light, half a metre to its right, with its keys.");
		ImGui::SameLine();
		if (ImGui::Button("Delete") && si >= 0) { lights::removeLight(S, si); g_sel = 0; g_picking = false; lights::markDirty(); }
		tip("Removes the selected light. The Del key does the same.");

		bool all = g_cfg.lightsAll;
		if (ImGui::Checkbox("Same lights in every clip", &all))
		{
			g_cfg.lightsAll = all;
			iniWriteInt("LightsAllClips", all ? 1 : 0);
			const int n = all ? lights::syncAllClips() : 0;
			lights::markDirty();
			logf("lights: same lights in every clip %s%s", all ? "on" : "off",
				n ? " - this clip's lights went to the project's other clips" : "");
		}
		tip("On: this clip's lights are the whole project's. Every edit here lands in every other clip of the project, and a clip opened for the first time starts with them - handy for a light rig that should not change from shot to shot.\nOff: each clip keeps its own lights.");

		// a rig from another clip or another project, including one that came in
		// with somebody else's lights file
		const int sets = lights::storeCount();
		if (sets > 1)
		{
			if (g_copyPick >= sets) g_copyPick = 0;
			const char* pn = nullptr;
			int pc = 0, pk = 0;
			char shown[168] = "";
			if (lights::storeAt(g_copyPick, &pn, &pc, &pk)) snprintf(shown, sizeof(shown), "%s - clip %d (%d light(s))", pn, pc + 1, pk);
			ImGui::SetNextItemWidth(260.0f);
			if (ImGui::BeginCombo("##copyfrom", shown))
			{
				for (int i = 0; i < sets; ++i)
				{
					const char* n2 = nullptr;
					int c2 = 0, k2 = 0;
					if (!lights::storeAt(i, &n2, &c2, &k2)) continue;
					char row[168];
					snprintf(row, sizeof(row), "%s - clip %d (%d light(s))", n2, c2 + 1, k2);
					if (ImGui::Selectable(row, i == g_copyPick)) g_copyPick = i;
				}
				ImGui::EndCombo();
			}
			tip("Every project and clip in EscoEditor.lights.txt. Lights are kept per project AND clip, so a file brought from another machine - or a project that was renamed - holds rigs this clip will never match by itself. Pick one here and copy it over.");
			ImGui::SameLine();
			if (ImGui::Button("Copy here"))
			{
				const int n = lights::copyFrom(g_copyPick);
				logf("lights: %d light(s) copied into clip %d of '%s'", n, lights::g_curClip + 1, lights::g_curProject);
			}
			tip("Adds that clip's lights to this one, keeping the lights already here. Their keyframes, flares and what they follow come with them.");
		}

		// The clip's lights. Dragging the box's bottom edge makes it taller.
		ImGui::BeginChild("##lights", ImVec2(0.0f, 130.0f), ImGuiChildFlags_Border | ImGuiChildFlags_ResizeY);
		for (int i = 0; i < S.count; ++i)
		{
			lights::Light& L = S.l[i];
			ImGui::PushID((int)L.id);
			if (ImGui::Checkbox("##on", &L.enabled)) lights::markDirty();
			ImGui::SameLine();
			char label[96];
			snprintf(label, sizeof(label), "%s  (%s%s%s)", L.name, L.type == lights::T_SPOT ? "spot" : "point", L.nkeys ? ", animated" : "",
				L.at.mode == lights::A_CAMERA ? ", on the camera" : L.at.mode == lights::A_ENTITY ? ", attached" : "");
			if (ImGui::Selectable(label, L.id == g_sel)) g_sel = L.id;
			ImGui::PopID();
		}
		if (S.count == 0) ImGui::TextDisabled("No lights in this clip yet.");
		ImGui::EndChild();

		si = lights::indexOf(S, g_sel);
		if (si < 0)
		{
			ImGui::TextDisabled("Add a light, or click a dot in the picture.");
			ImGui::Separator();
			sceneUi(&c);
			enbUi(&c);
			shortcutUi(lights::CAP_WINDOW);
			helpLine();
			ImGui::End();
			if (!keep) g_open = false;
			return;
		}
		lights::Light& L = S.l[si];
		const bool spot = L.type == lights::T_SPOT;
		ImGui::Separator();
		if (ImGui::InputText("Name", L.name, sizeof(L.name))) lights::markDirty();
		tip("Only a name, so you can tell your lights apart in the list and in the picture.");
		if (ImGui::RadioButton("Point", !spot)) { L.type = lights::T_POINT; lights::markDirty(); }
		tip("Shines in every direction from where it is, like a bulb.");
		ImGui::SameLine();
		if (ImGui::RadioButton("Spot", spot)) { L.type = lights::T_SPOT; lights::markDirty(); }
		tip("Shines in a cone you can aim: Rotation below, or the Rotate gizmo (E).");

		// --- what it follows, where it is, where it points, how big it is
		ImGui::SeparatorText("Transform");
		attachUi(L, c, haveCam);
		const bool attached = L.at.mode != lights::A_NONE;
		bool fok;
		const lights::Frame* F = editFrame(L, &fok);

		lights::Params e = shownParams(L, c);
		const lights::Params before = e;

		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("Gizmo");
		ImGui::SameLine();
		if (ImGui::RadioButton("Move (W)", g_mode == 0 || (g_mode == 1 && !spot))) g_mode = 0;
		tip("Arrows in the picture move the light along X, Y and Z; the white square moves it across the screen.");
		ImGui::SameLine();
		ImGui::BeginDisabled(!spot);
		if (ImGui::RadioButton("Rotate (E)", g_mode == 1 && spot)) g_mode = 1;
		tip("Rings in the picture aim a spot. A point light has no direction.");
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::RadioButton("Scale (R)", g_mode == 2)) g_mode = 2;
		tip("Drag the gizmo right or up to make the light bigger, left or down to make it smaller.");

		static const Axis kPos[3] = { { "X %.2f", 0.0f, 0.0f }, { "Y %.2f", 0.0f, 0.0f }, { "Z %.2f", 0.0f, 0.0f } };
		const float posStep = haveCam ? lights::clampf(distToCam(L, c) * 0.004f, 0.02f, 1.0f) : 0.05f;
		dragAxes(1, attached ? "Offset" : "Position", e.pos, 3, posStep, kPos);
		tip(attached ? "Metres from what it follows: X to its right, Y ahead of it, Z above it.\nCtrl+click a field to type a number."
					 : "World position in metres: X east, Y north, Z up.\nCtrl+click a field to type a number.");
		if (spot)
		{
			float yaw, pitch;
			lights::yawPitchFromDir(e.dir, &yaw, &pitch);
			float rot[2] = { pitch, yaw };
			static const Axis kRot[2] = { { "X %.1f deg", -89.9f, 89.9f }, { "Z %.1f deg", -180.0f, 180.0f } };
			if (dragAxes(2, "Rotation", rot, 2, 0.5f, kRot)) lights::dirFromYawPitch(rot[1], rot[0], e.dir);
			tip(attached ? "Turned like what it follows: X tilts the beam up and down, Z turns it round (0 = straight ahead).\nA round cone looks the same rolled, so there is no Y."
						 : "X tilts the beam up and down, Z turns it round.\nA round cone looks the same rolled, so there is no Y.");
		}
		else ImGui::TextDisabled("Rotation: a point light shines every way (a Spot can be aimed).");
		float sc = lights::scaleOf(e);
		if (ImGui::DragFloat("Scale", &sc, 0.01f, 0.05f, 20.0f, "%.2f x")) e.scale = lights::clampf(sc, 0.05f, 20.0f);
		tip("Makes the whole light bigger or smaller: it reaches Range x Scale.\nA spot keeps its cone's angles.");
		ImGui::SameLine();
		ImGui::TextDisabled("reaches %.1f m", lights::reachOf(e));

		// placed from where the camera is: world numbers, into the light's frame
		ImGui::BeginDisabled(!haveCam || !fok);
		if (ImGui::Button("To camera"))
		{
			putPos(e, F, c.cam.pos);
			if (spot) putDir(e, F, c.cam.fwd);
		}
		ImGui::SameLine();
		if (ImGui::Button("3 m ahead"))
		{
			float wp[3];
			for (int i = 0; i < 3; ++i) wp[i] = c.cam.pos[i] + c.cam.fwd[i] * 3.0f;
			putPos(e, F, wp);
		}
		if (spot)
		{
			ImGui::SameLine();
			if (ImGui::Button("Aim like the camera")) putDir(e, F, c.cam.fwd);
		}
		ImGui::EndDisabled();

		// --- what it looks like
		ImGui::SeparatorText("Light");
		ImGui::ColorEdit3("Colour", e.col);
		tip("The colour of the light. Click the square for the full picker.");
		ImGui::DragFloat("Intensity", &e.intensity, 0.1f, 0.0f, 2000.0f, "%.1f");
		tip("How bright it is. A room light is around 5 - 20, a stage light hundreds.");
		ImGui::DragFloat("Range", &e.range, 0.1f, 0.1f, 500.0f, "%.1f m");
		tip("How far it reaches at Scale 1.");
		ImGui::DragFloat("Falloff", &e.falloff, 0.05f, 0.1f, 64.0f, "%.2f");
		tip("How fast it dims towards its range. Low = a soft even wash, high = a tight pool of light.");
		if (spot)
		{
			ImGui::DragFloat("Cone inner", &e.inner, 0.2f, 1.0f, 88.0f, "%.1f deg");
			tip("The bright middle of the beam. Close to the outer angle = a hard edge.");
			ImGui::DragFloat("Cone outer", &e.outer, 0.2f, 2.0f, 89.0f, "%.1f deg");
			tip("How wide the beam is. 89 degrees is nearly a half sphere.");
		}
		if (L.flags & lights::LF_VOLUME)
		{
			ImGui::DragFloat("Volume intensity", &e.volInt, 0.01f, 0.0f, 20.0f, "%.2f");
			tip("How strong the haze in the air is.");
			ImGui::DragFloat("Volume size", &e.volSize, 0.01f, 0.01f, 20.0f, "%.2f");
			tip("How big that haze is next to the light itself.");
		}
		if (memcmp(&e, &before, sizeof(e)) != 0)
			if (lights::Params* T = editTarget(L, c)) { *T = e; lights::markDirty(); }

		flagBox("Shadows", L, lights::LF_SHADOWS, "The light casts shadows. The game shadows only 8 lights in view at a time.");
		ImGui::SameLine();
		flagBox("Sharper shadow", L, lights::LF_HIRES, "Gives this light first pick of those 8 shadow slots, at a higher resolution.");
		flagBox("Volumetric", L, lights::LF_VOLUME, "Draws the light in the air - the visible beam or haze - and adds its two values above.");
		ImGui::SameLine();
		flagBox("No reflections", L, lights::LF_NOSPEC, "No shiny highlight on wet roads, cars and glass from this light.");
		ImGui::SameLine();
		flagBox("Lit in a blackout", L, lights::LF_BLACKOUT, "Keeps the light on while the game has the city lights switched off.");
		flagBox("From the sun", L, lights::LF_SUN, "The engine works this light's colour and brightness out from the sun at the clip's hour, so it follows sunrise, noon and dusk by itself - a little sun of its own. Time of Day on the EscoEditor menu drives it.");

		// --- the lens flare, drawn by EscoFlare through ReShade
		ImGui::SeparatorText("Flare (lens)");
		bool fon = L.fl.on > 0.5f;
		if (ImGui::Checkbox("Lens flare", &fon))
		{
			if (fon && L.fl.size <= 0.02f) L.fl = lights::flarePreset(lights::FP_CINEMATIC);
			L.fl.on = fon ? 1.0f : 0.0f;
			lights::markDirty();
		}
		tip("A lens flare for this light: a glow, an anamorphic streak, a star, ghosts, a halo and a light leak.\nDrawn by EscoFlare through ReShade, before the editor's own menus, so nothing here goes blurry.");
		if (fon)
		{
			lights::flareFill(L.fl);   // a flare from a 3.3 file has none of 3.5's numbers yet
			if (!lights::g_flareLive)
				ImGui::TextDisabled("Waiting for ReShade to load EscoFlare.fx - EscoEditor.log says where it went.");
			for (int p = lights::FP_SOFT; p < lights::FP_COUNT; ++p)
			{
				if (p > lights::FP_SOFT && p != lights::FP_BLUE) ImGui::SameLine();
				if (ImGui::Button(lights::flareName(p))) { L.fl = lights::flarePreset(p); lights::markDirty(); }
			}
			tip("A starting point: it fills in the numbers below, which are all still yours to change.");
			bool ch = false;
			if (ImGui::DragFloat("Size", &L.fl.size, 0.01f, 0.2f, 4.0f, "%.2f")) ch = true;
			tip("How big the whole flare is on screen.");
			if (ImGui::DragFloat("Brightness", &L.fl.bright, 0.01f, 0.0f, 3.0f, "%.2f")) ch = true;
			tip("How strong it is. The light's own Intensity counts too, so keying the intensity keys the flare.");
			if (ImGui::DragFloat("Streak", &L.fl.streak, 0.01f, 0.0f, 2.0f, "%.2f")) ch = true;
			tip("The anamorphic line across the frame - the streak of a cine lens.");
			if (ImGui::DragFloat("Star", &L.fl.star, 0.01f, 0.0f, 2.0f, "%.2f")) ch = true;
			tip("Crossed spikes, the way a stopped-down photo lens draws a bright point.");
			if (ImGui::DragFloat("Ghosts", &L.fl.ghost, 0.01f, 0.0f, 2.0f, "%.2f")) ch = true;
			tip("The row of circles marching from the light through the middle of the frame.");
			if (ImGui::DragFloat("Halo", &L.fl.halo, 0.01f, 0.0f, 2.0f, "%.2f")) ch = true;
			tip("A ring on the opposite side of the frame's centre.");
			if (ImGui::DragFloat("Colour fringe", &L.fl.chroma, 0.01f, 0.0f, 1.0f, "%.2f")) ch = true;
			tip("How far the ghosts and the halo drift from the light's own colour.");
			if (ImGui::DragFloat("Light leak", &L.fl.leak, 0.01f, 0.0f, 2.0f, "%.2f")) ch = true;
			tip("Light creeping in from the edge of the frame, strongest when the light is near it or just past it - a film light leak. It shows even when the light itself is off screen.");

			// --- the shape of it
			ImGui::Spacing();
			if (ImGui::DragFloat("Angle", &L.fl.angle, 0.5f, -90.0f, 90.0f, "%.0f deg")) ch = true;
			tip("Turns the streak and the star. 0 is a level streak across the frame; real anamorphic lenses sit a degree or two off level.");
			if (ImGui::DragFloat("Cross streak", &L.fl.cross, 0.01f, 0.0f, 2.0f, "%.2f")) ch = true;
			tip("A second streak square across the first, for a cross rather than a single line. Needs Streak above.");
			if (ImGui::DragFloat("Star spikes", &L.fl.spokes, 0.05f, 2.0f, 8.0f, "%.0f")) ch = true;
			tip("How many spikes the star has - the number of blades in the iris. Needs Star above.");
			if (ImGui::DragFloat("Ghost count", &L.fl.ghostCount, 0.05f, 1.0f, 6.0f, "%.0f")) ch = true;
			tip("How many ghost circles march through the middle of the frame. Needs Ghosts above.");
			if (ImGui::DragFloat("Ghost spacing", &L.fl.ghostSpread, 0.01f, 0.05f, 1.5f, "%.2f")) ch = true;
			tip("How far apart they sit along the line from the light through the centre. Small = a tight cluster near the middle.");
			if (ImGui::DragFloat("Halo size", &L.fl.haloSize, 0.005f, 0.02f, 0.8f, "%.3f")) ch = true;
			tip("The halo ring's radius, as a share of the frame's height. Needs Halo above.");
			if (ImGui::DragFloat("Squeeze", &L.fl.squeeze, 0.02f, 0.2f, 4.0f, "%.2f")) ch = true;
			tip("Stretches the core sideways: 1 is a round glow, 2 an oval twice as wide - the bokeh of an anamorphic lens. Follows Angle.");

			// --- its own colour
			ImGui::Spacing();
			if (ImGui::ColorEdit3("Flare colour", L.fl.tint)) ch = true;
			tip("A colour for the flare itself, whatever the light's colour is. Click the square for the full picker; it only shows as far as Tint below.");
			if (ImGui::DragFloat("Tint", &L.fl.tintMix, 0.01f, 0.0f, 1.0f, "%.2f")) ch = true;
			tip("0 keeps the light's own colour, 1 uses the flare colour above, in between mixes them. The classic blue cine streak is a blue flare colour at about 0.7.");

			// --- and whether it sits still
			ImGui::Spacing();
			if (ImGui::DragFloat("Flicker", &L.fl.flicker, 0.01f, 0.0f, 1.0f, "%.2f")) ch = true;
			tip("Makes the flare breathe - an arc lamp, a fire, a failing bulb. It follows the clip's own clock, not the wall clock, so the same frame always looks the same and it holds still while you scrub.");
			if (ImGui::DragFloat("Flicker speed", &L.fl.flickerHz, 0.05f, 0.2f, 30.0f, "%.1f Hz")) ch = true;
			tip("How fast it breathes. Two lights with the same numbers still flicker out of step.");
			if (ch) lights::markDirty();
		}

		if (ImGui::CollapsingHeader("Keyframes", ImGuiTreeNodeFlags_DefaultOpen))
		{
			bool anim = L.nkeys > 0;
			const bool wasAnim = anim;
			if (ImGui::Checkbox("Animate over the clip", &anim))
			{
				if (anim) lights::insertKey(L, kt, L.base);
				else { L.base = lights::evalLight(L, kt); L.nkeys = 0; }
				lights::markDirty();
			}
			tip(wasAnim ? "Untick to go back to one look for the whole clip; what it looks like at this moment is kept."
			            : "Tick to key the light over the clip: every edit then goes to a key at the open marker, or at the playhead, and the light moves smoothly from key to key.");
			if (L.nkeys > 0)
			{
				ImGui::TextDisabled("Edits go to the key at the %s (%.2f s).", atMarker ? "open marker" : "playhead", kt / 1000.0f);
				keyList(L, kt);
				if (lights::keyAt(L, kt) < 0)
				{
					if (ImGui::Button("Key here")) { lights::insertKey(L, kt, lights::evalLight(L, kt)); lights::markDirty(); }
					tip("Adds a key at this moment, holding the light exactly as it looks now.");
				}
			}
			else ImGui::TextDisabled("One look for the whole clip. Tick Animate to key it per marker.");
		}
		ImGui::Separator();
		sceneUi(&c);
		enbUi(&c);
		shortcutUi(lights::CAP_WINDOW);
		helpLine();
		ImGui::End();
		if (!keep) g_open = false;
	}

	// FiveM can run more than one ReShade runtime (helper swap chains beside
	// the game's window), and each one fires reshade_overlay with its own ImGui.
	// The editor lives in the one on the window in front - the biggest when that
	// does not decide; one that stops calling hands over after 2 s.
	reshade::api::effect_runtime* g_mainRt = nullptr;
	float     g_mainArea = 0.0f;
	ULONGLONG g_mainSeen = 0;
	bool      g_mainFront = false;

	bool isMainRuntime(reshade::api::effect_runtime* rt, const ImVec2& size)
	{
		const ULONGLONG now = GetTickCount64();
		const float area = size.x * size.y;
		const HWND hw = (HWND)rt->get_hwnd(), fg = GetForegroundWindow();
		const bool front = hw && fg && GetAncestor(hw, GA_ROOT) == GetAncestor(fg, GA_ROOT);
		if (rt == g_mainRt) { g_mainArea = area; g_mainSeen = now; g_mainFront = front; return true; }
		const bool stale  = !g_mainRt || now - g_mainSeen > 2000;
		const bool better = (front && !g_mainFront) || (front == g_mainFront && area > g_mainArea);
		if (!stale && !better) return false;
		g_mainRt = rt; g_mainArea = area; g_mainSeen = now; g_mainFront = front;
		if (g_rtLogs < 4) { ++g_rtLogs; logf("lights: the light editor draws in the %.0fx%.0f window%s", size.x, size.y, front ? "" : " (not in front)"); }
		return true;
	}

	// A new key is being picked (from the menu, this window or ReShade's tab):
	// ImGui sees the keys even while ReShade keeps them from the game. True on
	// the frame the pick ended.
	bool captureKeys()
	{
		if (lights::g_capture != 1 || lights::since(GetTickCount64(), lights::g_captureStart) < lights::kCaptureGraceMs) return false;
		for (int vk = 1; vk < 255; ++vk)
		{
			const ImGuiKey k = lights::imguiKeyFor(vk);
			if (k != ImGuiKey_None && ImGui::IsKeyPressed(k, false) && lights::offerKey(vk)) return true;
		}
		return false;
	}

	void closeState()
	{
		lights::g_uiOpen = false;
		lights::g_wantScene = false;
		g_picking = false;
		g_showList = false;
		g_listHover = -1;
		g_drag = 0;
		g_openLogged = false;
	}

	// reshade_overlay: every frame, on ReShade's render thread.
	void onOverlay(reshade::api::effect_runtime* rt)
	{
		ImGuiIO& io = ImGui::GetIO();
		if (!isMainRuntime(rt, io.DisplaySize)) return;
		lights::g_textInput = io.WantTextInput;

		// The key that just closed the editor opens it again only once it is up.
		const ImGuiKey sk = lights::imguiKeyFor(g_cfg.keyLights);
		if (lights::g_needRelease && (sk == ImGuiKey_None || !ImGui::IsKeyDown(sk))) lights::g_needRelease = false;

		if (InterlockedExchange(&lights::g_openReq, 0) && !g_open)
		{
			g_open = true;
			g_closeArmed = false;
			lights::toggleDone(GetTickCount64());   // the same press must not close it again
		}
		const bool picked = captureKeys();

		if (g_open && (!lights::g_ready || !editorOpen())) g_open = false;
		if (!g_open) { closeState(); return; }

		lights::Lock lk;
		lights::LightSet* S = lights::cur();
		if (!S) { g_open = false; closeState(); return; }
		if (!g_openLogged) { g_openLogged = true; logf("lights: light editor opened (clip %d, %d light(s))", lights::g_curClip + 1, S->count); }

		rt->block_input_next_frame();
		io.MouseDrawCursor = true;
		const lights::Ctx c = lights::ctx();
		const bool haveCam = c.valid && GetTickCount64() - c.at < 1000;
		const lights::View v = lights::makeView(c.cam, io.DisplaySize.x, io.DisplaySize.y);
		const ImVec2 mouse = io.MousePos;

		// The whole screen: wherever the mouse is, it belongs to the editor.
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(io.DisplaySize);
		ImGui::Begin("##EscoLightsViewport", nullptr,
			ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
			ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollWithMouse);
		const bool overView = ImGui::IsWindowHovered();
		ImGui::End();

		const int si = lights::indexOf(*S, g_sel);
		if (g_picking && si < 0) g_picking = false;
		lights::g_wantScene = g_picking || g_showList;
		if (haveCam)
		{
			ImDrawList* dl = ImGui::GetBackgroundDrawList();
			const bool hover = overView && !g_drag;
			const uint32_t under = drawMarkers(dl, v, *S, c, mouse, hover && !g_picking);
			if (g_picking)
			{
				// a click picks what the selected light follows
				g_drag = 0;
				const lights::Scene sc = lights::scene();
				const int pick = lights::since(GetTickCount64(), sc.at) < 1000 ? drawScene(dl, v, sc, mouse, hover, -1) : -1;
				if (overView && pick >= 0 && ImGui::IsMouseClicked(0))
				{
					lights::Light& L = S->l[si];
					const lights::SceneItem& it = sc.it[pick];
					const lights::Frame camF = lights::frameOf(c.cam);
					if (lights::attachTo(L, lights::A_ENTITY, it.f, it.ent, it.kind, it.model, &camF))
					{
						bool am = false;
						if (g_jump) lights::jumpToParent(L, keyTime(c, &am));
						lights::markDirty();
						logf("lights: '%s' follows %s %08X now, %.1f m from the camera%s", L.name, lights::kindName(it.kind), it.model, it.dist,
							g_jump ? " - moved onto it" : "");
					}
					g_picking = false;
				}
			}
			else
			{
				// the row the mouse is on in the list, boxed where it is
				if (g_showList && g_listHover >= 0)
				{
					const lights::Scene sc = lights::scene();
					if (g_listHover < sc.count && lights::since(GetTickCount64(), sc.at) < 1000)
						drawScene(dl, v, sc, mouse, false, g_listHover);
				}
				int hot = H_NONE;
				if (si >= 0)
				{
					const lights::Light& L = S->l[si];
					lights::Params W;
					if (worldOf(L, shownParams(L, c), W)) hot = drawGizmo(dl, v, W, L.type == lights::T_SPOT, mouse, hover);
				}
				// Alt+click throws the light under the cursor (keeping its distance)
				// and carries on as a screen drag; the wheel pushes it away.
				if (si >= 0 && overView && !g_drag && io.KeyAlt && ImGui::IsMouseClicked(0))
				{
					lights::Light& L = S->l[si];
					if (placeOnRay(L, c, v, mouse, distToCam(L, c)))
					{
						lights::markDirty();
						beginDrag(H_PLANE, L, c, v, mouse);
					}
				}
				else if (si >= 0 && overView && !g_drag && io.MouseWheel != 0.0f)
				{
					if (pushPull(S->l[si], c, io.MouseWheel, io.KeyShift, io.KeyCtrl)) lights::markDirty();
				}
				if (overView && !g_drag && ImGui::IsMouseClicked(0))
				{
					if (hot != H_NONE && si >= 0) beginDrag(hot, S->l[si], c, v, mouse);
					else if (under) g_sel = under;
				}
				if (g_drag)
				{
					const int di = lights::indexOf(*S, g_dragId);
					if (di < 0 || !ImGui::IsMouseDown(0)) { lights::markDirty(); g_drag = 0; }
					else updateDrag(S->l[di], c, v, mouse, io.KeyCtrl, io.KeyShift ? 6.0f : io.KeyAlt ? 0.2f : 1.0f);
				}
			}
		}
		else g_drag = 0;

		panel(*S, c, haveCam);

		// The editor's keys, read from ImGui (ReShade keeps them from the game).
		const bool busy = io.WantTextInput || lights::g_capture != 0 || picked || lights::since(GetTickCount64(), lights::g_keyMsgAt) < 300;
		if (busy) g_closeArmed = false;
		else
		{
			if (sk != ImGuiKey_None)
			{
				if (!ImGui::IsKeyDown(sk)) g_closeArmed = true;
				else if (g_closeArmed && ImGui::IsKeyPressed(sk, false) && lights::toggleDue(GetTickCount64()))
				{
					g_open = false;
					lights::g_needRelease = true;
					lights::toggleDone(GetTickCount64());
				}
			}
			if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
			{
				if (g_picking) g_picking = false;
				else { g_open = false; lights::toggleDone(GetTickCount64()); }
			}
			if (ImGui::IsKeyPressed(ImGuiKey_W, false)) g_mode = 0;
			if (ImGui::IsKeyPressed(ImGuiKey_E, false)) g_mode = 1;
			if (ImGui::IsKeyPressed(ImGuiKey_R, false)) g_mode = 2;
			if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
			{
				const int di = lights::indexOf(*S, g_sel);
				if (di >= 0) { lights::removeLight(*S, di); g_sel = 0; g_picking = false; lights::markDirty(); }
			}
		}
		lights::g_uiOpen = g_open;
		if (!g_open) closeState();
	}

	// The "EscoEditor" tab in ReShade's own menu, drawn by ReShade while its menu is open.
	void onReshadeTab(reshade::api::effect_runtime*)
	{
		ImGui::Text("EscoEditor %s - scene lights for the Rockstar Editor", kVersion);
		ImGui::Spacing();
		if (!lights::g_ready)
		{
			ImGui::TextWrapped("Scene lights are not in: Lights=0 in EscoEditor.ini, a hook that has not gone in yet (the plugin keeps trying for two minutes after the game starts - give it a moment), or this game build is not mapped. EscoEditor.log says which.");
			return;
		}
		int count = -1, clip = -1;
		char project[128] = {};
		{
			lights::Lock lk;
			if (lights::LightSet* S = lights::cur()) { count = S->count; clip = lights::g_curClip; strcpy_s(project, lights::g_curProject); }
		}
		if (count < 0) ImGui::TextDisabled("Open a clip in the Rockstar Editor to place its lights.");
		else
		{
			ImGui::Text("Clip %d of %s: %d light(s)", clip + 1, project, count);
			if (ImGui::Button(g_open ? "Close the light editor" : "Open the light editor"))
			{
				if (g_open) g_open = false;
				else InterlockedExchange(&lights::g_openReq, 1);
			}
		}
		ImGui::Spacing();
		sceneUi(nullptr);
		ImGui::Spacing();
		shortcutUi(lights::CAP_RESHADE);
	}
}
