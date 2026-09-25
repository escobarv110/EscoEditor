// EscoFlare.fx 1.1 - lens flares and a light leak for EscoEditor's lights
// =============================================================================
//  Part of EscoEditor.asi (Rockstar Editor plugin). EscoEditor copies this file
//  into ReShade's shader folder, registers itself with ReShade as an add-on and
//  fills in every "EE_" value below each frame: where each of the clip's lights
//  is on screen, how far away it is, its colour, and the flare numbers set for
//  it in the light editor.
//
//  It is hidden in ReShade's menu on purpose - the light editor is the only
//  place it is set - and EscoEditor draws it itself, right before the game
//  draws its menus, so the editor's menus and timeline stay sharp.
//
//  Depth is used to hide a flare behind whatever is in front of the light, and
//  is turned into metres with the camera's near / far clip, the same numbers
//  the editor shows.
//
//  Everything is drawn analytically (no textures, one pass): a glow at the
//  light - round or squeezed oval - an anamorphic streak at any angle with a
//  second streak across it, a star of 2 to 8 spikes, up to six ghosts along the
//  line through the middle of the frame, a halo opposite it, a flicker, and a
//  soft leak from the edge the light sits towards.
//
//  MIT licence.
// =============================================================================
#include "ReShade.fxh"

#define EE_MAX_FLARES 8

// ---- set by EscoEditor every frame ------------------------------------------
uniform int EE_FlareCount < hidden = true;
	ui_label = "Flares this frame"; ui_category = "Set by EscoEditor";
> = 0;

// x, y  where the light is on screen (0..1; outside means off screen)
// z     how far it is from the camera, in metres
// w     brightness: the light editor's Brightness times the light's intensity
uniform float4 EE_FlareA[EE_MAX_FLARES] < hidden = true; >;

// rgb   the light's colour        w  size
uniform float4 EE_FlareB[EE_MAX_FLARES] < hidden = true; >;

// x streak    y star    z ghosts    w halo
uniform float4 EE_FlareC[EE_MAX_FLARES] < hidden = true; >;

// x colour fringe    y light leak    z angle of the streak and star, radians
// w cross streak: a second streak square across the first
uniform float4 EE_FlareD[EE_MAX_FLARES] < hidden = true; >;

// x star spikes (2..8)    y ghosts (1..6)    z how far apart the ghosts sit
// w the halo ring's radius
uniform float4 EE_FlareE[EE_MAX_FLARES] < hidden = true; >;

// x squeeze: an oval core, 1 round    y flicker    z flicker in Hz
// w how far the flare's own colour replaces the light's
uniform float4 EE_FlareF[EE_MAX_FLARES] < hidden = true; >;

// rgb the flare's own colour    w its flicker phase, so two lights differ
uniform float4 EE_FlareG[EE_MAX_FLARES] < hidden = true; >;

uniform float EE_NearClip < hidden = true;
	ui_label = "Camera near clip"; ui_category = "Set by EscoEditor";
> = 0.1;

uniform float EE_FarClip < hidden = true;
	ui_label = "Camera far clip"; ui_category = "Set by EscoEditor";
> = 1000.0;

// The clip's own time in seconds, not the wall clock: a flicker then falls on
// the same frame every time the clip is played or exported.
uniform float EE_Time < hidden = true;
	ui_label = "Clip time"; ui_category = "Set by EscoEditor";
> = 0.0;

static const float2 EE_Taps[5] = { float2(0, 0), float2(5, 0), float2(-5, 0), float2(0, 5), float2(0, -5) };

// Distance from the camera in metres. The game stores reversed depth (1 at the
// near plane, 0 at the far plane) from a normal perspective projection; the
// plugin passes the clips it read from the camera.
float EE_Depth(float2 uv)
{
	float raw = tex2Dlod(ReShade::DepthBuffer, float4(uv, 0.0, 0.0)).x;
#if RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN
	raw = tex2Dlod(ReShade::DepthBuffer, float4(uv.x, 1.0 - uv.y, 0.0, 0.0)).x;
#endif
#if RESHADE_DEPTH_INPUT_IS_REVERSED
	float d = raw;
#else
	float d = 1.0 - raw;
#endif
	float n = EE_NearClip, f = EE_FarClip;
	return n * f / max(d * (f - n) + n, 1e-6);
}

// How much of the light the lens can see: the depth buffer right at it, over
// five taps, so a thin pole in front does not switch the whole flare off.
// 0 for a light that is off screen - only its leak is left.
float EE_Visible(float2 uv, float dist)
{
	if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 0.0;
	float vis = 0.0;
	[unroll] for (int i = 0; i < 5; ++i)
	{
		float2 p = saturate(uv + EE_Taps[i] * BUFFER_PIXEL_SIZE);
		vis += (EE_Depth(p) > dist * 0.97) ? 0.2 : 0.0;
	}
	// fade out over the last few per cent of the frame, so a light leaving the
	// picture does not take its flare with it in one frame
	float2 e = min(uv, 1.0 - uv);
	return vis * saturate(min(e.x, e.y) / 0.02);
}

// One light's flare. `me` and `s` are in screen units: x scaled by the aspect
// ratio, so a circle is a circle and 1.0 is the height of the frame.
float3 EE_OneFlare(int i, float2 me, out float3 leakAdd)
{
	leakAdd = 0.0;

	float2 suv   = EE_FlareA[i].xy;
	float  dist  = EE_FlareA[i].z;
	float  amp   = EE_FlareA[i].w;
	float  size  = max(EE_FlareB[i].w, 0.02);
	float  streak = EE_FlareC[i].x, star = EE_FlareC[i].y, ghosts = EE_FlareC[i].z, halo = EE_FlareC[i].w;
	float  chroma = EE_FlareD[i].x, leak = EE_FlareD[i].y, ang = EE_FlareD[i].z, crossAmt = EE_FlareD[i].w;
	float  spokes = clamp(EE_FlareE[i].x, 2.0, 8.0), gcount = clamp(EE_FlareE[i].y, 1.0, 6.0);
	float  gspread = clamp(EE_FlareE[i].z, 0.02, 1.5), halosize = clamp(EE_FlareE[i].w, 0.02, 0.8);
	float  squeeze = clamp(EE_FlareF[i].x, 0.05, 4.0), flick = saturate(EE_FlareF[i].y);
	if (amp <= 0.001) return 0.0;

	// its own colour, as far as the light editor's Tint says
	float3 col = lerp(EE_FlareB[i].rgb, EE_FlareG[i].rgb, saturate(EE_FlareF[i].w));

	// the flicker: two waves at once, so it breathes instead of humming
	if (flick > 0.001)
	{
		float t = EE_Time * max(EE_FlareF[i].z, 0.05) * 6.2831853 + EE_FlareG[i].w;
		float w = 0.5 + 0.5 * (sin(t) * 0.65 + sin(t * 2.37 + 1.7) * 0.35);
		amp *= lerp(1.0, w, flick);
		if (amp <= 0.001) return 0.0;
	}

	float2 s = (suv - 0.5) * float2(BUFFER_ASPECT_RATIO, 1.0);
	float2 d = me - s;
	float  vis = EE_Visible(suv, dist);
	float  ca = cos(ang), sa = sin(ang);
	float2 rot = float2(d.x * ca + d.y * sa, -d.x * sa + d.y * ca);   // d, turned by the angle

	float3 f = 0.0;

	// the glow at the light itself, squeezed along the streak's own axis
	float2 dq = float2(rot.x / squeeze, rot.y);
	f += col * exp(-length(dq) / (0.035 * size)) * 1.3;

	// the anamorphic streak through it, and the one across it
	if (streak > 0.001)
	{
		f += col * exp(-abs(rot.x) / (0.22 * size * streak)) * exp(-abs(rot.y) / (0.006 * size)) * streak * 2.0;
		if (crossAmt > 0.001)
			f += col * exp(-abs(rot.y) / (0.22 * size * streak * crossAmt)) * exp(-abs(rot.x) / (0.006 * size)) * streak * crossAmt * 2.0;
	}

	// a star: thin streaks crossed, as many spikes as asked for
	if (star > 0.001)
	{
		int n = (int)(spokes + 0.5);
		[loop] for (int k = 0; k < n; ++k)
		{
			float a = 3.14159265 * (float(k) / float(n) + 0.1667) + ang;
			float sca = cos(a), ssa = sin(a);
			float2 rd = float2(d.x * sca + d.y * ssa, -d.x * ssa + d.y * sca);
			f += col * exp(-abs(rd.x) / (0.10 * size * star)) * exp(-abs(rd.y) / (0.004 * size)) * star;
		}
	}

	// ghosts, stepping along the line from the light through the middle
	if (ghosts > 0.001)
	{
		int gn = (int)(gcount + 0.5);
		[loop] for (int g = 1; g <= gn; ++g)
		{
			float2 gp = s * (-gspread * float(g));
			float  gr = length(me - gp);
			float  gs = (0.05 + 0.02 * float(g)) * size;
			float3 tint = lerp(col, float3(0.55 + 0.45 * frac(float(g) * 0.37), 0.75, 1.0 - 0.35 * frac(float(g) * 0.61)), chroma);
			f += tint * exp(-(gr / gs) * (gr / gs)) * ghosts * 0.6 / float(g);
		}
	}

	// the halo ring opposite the light
	if (halo > 0.001)
	{
		float hr = length(me + s);
		float ring = halosize * size, w = 0.05 * size;
		float3 tint = lerp(col, float3(1.0, 0.75, 0.55), chroma);
		f += tint * exp(-((hr - ring) / w) * ((hr - ring) / w)) * halo * 0.8;
	}

	// the leak: a soft wash from the side the light sits towards, strongest
	// when it is near or already past the edge of the frame. It does not need
	// the light itself to be visible, which is what makes it look like light
	// creeping in past the barrel.
	if (leak > 0.001)
	{
		float2 dir = normalize(s + float2(1e-5, 1e-5));
		float along = dot(me, dir);
		float across = length(me - dir * along);
		float edge = saturate((length(s) - 0.30) / 0.45);
		float wash = saturate(along * 1.4 + 0.15) * exp(-across * 1.6) * edge;
		leakAdd = lerp(col, float3(1.0, 0.85, 0.7), 0.5) * wash * leak * amp * 0.8;
	}

	return f * amp * vis;
}

float3 PS_EE_Flare(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	float3 base = tex2D(ReShade::BackBuffer, uv).rgb;
	if (EE_FlareCount <= 0) return base;

	float2 me = (uv - 0.5) * float2(BUFFER_ASPECT_RATIO, 1.0);
	float3 add = 0.0;
	[loop] for (int i = 0; i < EE_MAX_FLARES; ++i)
	{
		if (i >= EE_FlareCount) break;
		float3 leakAdd;
		add += EE_OneFlare(i, me, leakAdd) + leakAdd;
	}
	return base + add;
}

technique EscoFlare < hidden = true;
	ui_label = "EscoFlare";
	ui_tooltip = "Lens flares for EscoEditor's scene lights. EscoEditor draws this itself from the light editor - there is nothing to switch on here.";
>
{
	pass Flare { VertexShader = PostProcessVS; PixelShader = PS_EE_Flare; }
}

// ---------------------------------------------------------------------------
// EscoFocus: the raw depth at the middle of the picture, into one pixel that
// EscoEditor reads back - it is how the ENB option's Auto focus knows what is
// in front of the camera, and how Manual shows how far away it is. The raw
// value, not a distance: NVE's ENB depth of field works on the same raw depth.
// A small plus of samples, the centre counting most.
// ---------------------------------------------------------------------------
texture EE_FocusTex { Width = 1; Height = 1; Format = R32F; };

float PS_EE_Focus(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	const float2 c = float2(0.5, 0.5);
	const float2 o = float2(0.006 / BUFFER_ASPECT_RATIO, 0.006);
	float d = tex2Dlod(ReShade::DepthBuffer, float4(c, 0.0, 0.0)).x * 4.0;
	d += tex2Dlod(ReShade::DepthBuffer, float4(c + float2(o.x, 0.0), 0.0, 0.0)).x;
	d += tex2Dlod(ReShade::DepthBuffer, float4(c - float2(o.x, 0.0), 0.0, 0.0)).x;
	d += tex2Dlod(ReShade::DepthBuffer, float4(c + float2(0.0, o.y), 0.0, 0.0)).x;
	d += tex2Dlod(ReShade::DepthBuffer, float4(c - float2(0.0, o.y), 0.0, 0.0)).x;
	return d / 8.0;
}

technique EscoFocus < hidden = true; enabled = false;
	ui_label = "EscoFocus";
	ui_tooltip = "EscoEditor measures the depth in the middle of the picture with this, for its ENB focus. Nothing to switch on here.";
>
{
	pass Focus { VertexShader = PostProcessVS; PixelShader = PS_EE_Focus; RenderTarget = EE_FocusTex; }
}
