// Offline self-test: load the real cameras.ymt (a PSO file) into a private heap
// buffer and run the plugin's own scanner, fingerprint, write and restore paths
// on it, then the director hook's movement rescale on fake director/camera
// objects, the quaternion packing, the clip-speed index mapping, the weather /
// clock packet patching on a fake replay buffer, the keyframe clipboard's ini
// round trip and record writing, the Depth of Field copy, the scene lights and
// the light editor's key. PSO files are stored big-endian and byte-swapped by
// the game on load, so the buffer is swapped in 4-byte words first.
#include "../../src/main.cpp"
#include <tlhelp32.h>

static int g_fail = 0, g_ran = 0;
#define CHECK(cond, ...) do { ++g_ran; if (!(cond)) { ++g_fail; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static unsigned char g_seenWeather[scene::kWeatherSize];
static int g_extractCalls = 0;
static void __fastcall fakeExtract(const void* p) { memcpy(g_seenWeather, p, scene::kWeatherSize); ++g_extractCalls; }

// ---- MinHook itself: hook.c is patched to retry the thread snapshot, so a real
// hook is put in here to prove the enable path still works end to end.
__declspec(noinline) int mhTarget(int x)
{
    volatile int a = x + 1;
    volatile int b = a * 2;
    volatile int c = b - 1;
    return a + b + c;
}
static int (*mhOrig)(int) = nullptr;
__declspec(noinline) int mhTarget2(int x)
{
    volatile int a = x + 3;
    volatile int b = a * 2;
    volatile int c = b - 1;
    return a + b + c;
}
static int (*mhOrig2)(int) = nullptr;
__declspec(noinline) int mhDetour(int x) { return mhOrig(x) * 10; }
__declspec(noinline) int mhDetour2(int x) { return mhOrig2(x) * 7; }

int main(int argc, char** argv)
{
    // With a real cameras.ymt: load it and swap it out of PSO's big-endian
    // words, which is how the block sits in the file. Without one: build the
    // same block in memory, so the suite runs anywhere - the scanner does not
    // care where the bytes came from.
    long n = 0;
    uint8_t* buf = nullptr;
    if (argc >= 2)
    {
        FILE* f = nullptr;
        if (fopen_s(&f, argv[1], "rb") != 0 || !f) { puts("cannot open"); return 2; }
        fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
        buf = (uint8_t*)VirtualAlloc(nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (fread(buf, 1, (size_t)n, f) != (size_t)n) { puts("short read"); return 2; }
        fclose(f);
        printf("loaded %ld bytes at %p\n", n, buf);
        for (long i = 0; i + 4 <= n; i += 4)
        {
            uint8_t t = buf[i]; buf[i] = buf[i + 3]; buf[i + 3] = t;
            t = buf[i + 1]; buf[i + 1] = buf[i + 2]; buf[i + 2] = t;
        }
    }
    else
    {
        n = 0x8000;
        buf = (uint8_t*)VirtualAlloc(nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        memset(buf, 0, (size_t)n);
        uint8_t* mb = buf + 0x10;                      // 16-byte aligned, as in the file
        auto wu = [&](uint32_t off, uint32_t v) { memcpy(mb + off, &v, 4); };
        auto wf = [&](uint32_t off, float v)    { memcpy(mb + off, &v, 4); };
        wu(OFF_Name, kNameHash);
        wu(OFF_CollRef, 0x8EC4EDE4u);
        wu(OFF_DofRef,  0x4959CC61u);
        wf(OFF_NearClip, 0.1f);
        for (uint32_t off = OFF_RespH; off <= 0xD8; off += 0x18)
        {
            wf(off + RF_Power, 4.0f);
            wf(off + RF_Accel, 30.0f);
            wf(off + RF_Decel, 200.0f);
            wf(off + RF_Speed, 1.0f);
        }
        wf(OFF_RespH + RF_Speed, 10.0f);               // the stock free camera
        wf(OFF_RespV + RF_Speed, 10.0f);
        wf(OFF_RespLook + RF_Speed, 4.0f);
        wf(OFF_MouseMul, 3.0f);
        wf(OFF_MaxPitch, 86.0f);
        wf(OFF_MinFov, 10.0f);
        wf(OFF_MaxFov, 100.0f);
        wf(OFF_DefFov, 45.0f);
        wf(OFF_Capsule, 0.31f);
        printf("no cameras.ymt given - built the stock block in memory at %p\n", buf);
    }

    g_logOn = false;
    const int found = findMetadata();
    printf("findMetadata -> %d block(s)\n", found);
    if (found != 1) { printf("FAIL: expected exactly one block\n"); return 1; }
    Meta& m0 = g_meta[0];
    printf("  block at file offset 0x%llX: move h %.2f v %.2f, look %.2f, mouse x%.2f\n",
        (unsigned long long)(m0.p - buf), m0.h.speed, m0.v.speed, m0.look.speed, m0.mouse);
    CHECK(m0.h.speed == 10.0f && m0.v.speed == 10.0f && m0.look.speed == 4.0f && m0.mouse == 3.0f, "stock values");

    char why[512] = {};
    uint8_t* p = g_meta[0].p;
    g_rescale = false;
    g_cfg.speed = 25; g_cfg.response = 2; g_cfg.look = 50;
    applyAll(1.0f, GetTickCount64());
    CHECK(rdf(p, OFF_RespH + RF_Speed) == 2.5f && rdf(p, OFF_RespLook + RF_Speed) == 2.0f && rdf(p, OFF_MouseMul) == 1.5f, "fallback apply");
    *(float*)(p + OFF_MinFov) = 0.5f;  *(float*)(p + OFF_MaxFov) = 179.0f;
    *(float*)(p + OFF_Capsule) = 0.0f; *(float*)(p + OFF_MaxPitch) = 89.99f;
    *(float*)(p + OFF_RespH + RF_Speed) = 40.0f; *(float*)(p + OFF_RespV + RF_Speed) = 40.0f;
    CHECK(metaStillValid(why, sizeof(why)), "still valid after a NoEditorRestrictions-style rewrite");

    g_rescale = true;
    g_cfg.speed = 50; g_cfg.vertical = 0; g_cfg.response = 1; g_lastWrittenH = -1.0f;
    applyAll(1.0f, GetTickCount64());
    {
        uint8_t* cam = (uint8_t*)calloc(1, 0x600);
        uint8_t* dir = (uint8_t*)calloc(1, 0x700);
        *(uint8_t**)(cam + 0x250) = p;
        *(uint8_t**)(dir + 0x2C0) = cam;
        int editIdx = 0; g_editIndex = &editIdx;
        int mode = 2;    g_replayMode = &mode;
        g_holdMul = 1.0f;
        auto setpos = [&](float x, float y, float z) {
            float v[3] = { x, y, z };
            memcpy(cam + 0x480, v, 12); memcpy(cam + 0x60, v, 12); memcpy(dir + 0x220, v, 12); memcpy(dir + 0x60, v, 12);
        };
        const float* cp = (const float*)(cam + 0x480);
        setpos(100, 200, 30); director::rescale(dir);
        setpos(104, 200, 32); director::rescale(dir);
        CHECK(fabsf(cp[0] - 100.5f) < 1e-4f && fabsf(cp[2] - 30.25f) < 1e-4f, "rescale k=0.125");
        editIdx = -1; setpos(504, 500, 30); director::rescale(dir); setpos(508, 500, 30); director::rescale(dir);
        CHECK(fabsf(cp[0] - 508.0f) < 1e-4f, "outside Edit Camera untouched");
        g_editIndex = nullptr; g_replayMode = nullptr;
        free(cam); free(dir);
    }
    g_rescale = false;
    restoreAll();
    CHECK(rdf(p, OFF_RespH + RF_Speed) == 10.0f && rdf(p, OFF_RespH + RF_Accel) == 30.0f && rdf(p, OFF_MouseMul) == 3.0f, "restore");

    // ---- Depth of Field copy: the marker fields, read and written ---------
    {
        CHECK(dof::MODE_DEFAULT == 0 && dof::MODE_CUSTOM == 1 && dof::MODE_NONE == 2, "DOF modes in the game's order: Default, Custom, None");
        uint8_t src[mk::SIZE] = {}, dst[mk::SIZE] = {};
        src[dof::OFF_Mode] = 2; src[dof::OFF_FocusMode] = 1; float fd = 12.5f, in = 7.0f; int32_t tid = 42;
        memcpy(src + dof::OFF_FocalDist, &fd, 4); memcpy(src + dof::OFF_Intensity, &in, 4); memcpy(src + dof::OFF_TargetId, &tid, 4);
        dof::Clip c; CHECK(dof::readFrom(src, c) && c.mode == 2 && c.focalDist == 12.5f && c.targetId == 42, "read marker DOF");
        dof::writeTo(dst, c, false);
        int32_t got; memcpy(&got, dst + dof::OFF_TargetId, 4);
        CHECK(dst[dof::OFF_Mode] == 2 && got == -1 && (mk::u64(dst, 0) & (1ull << 28)) && (mk::u64(dst, 0) & (1ull << 30)), "write marker DOF (no target across clips, edited bits set)");
    }

    // ---- scene lights: the engine record, keys, the gizmo maths, the file ---
    {
        namespace lt = lights;
        lt::init();
        lt::Params sp = lt::defaultParams(lt::T_SPOT);
        sp.pos[0] = 10; sp.pos[1] = 20; sp.pos[2] = 30; sp.range = 40.0f; sp.outer = 60.0f; sp.inner = 10.0f;
        sp.dir[0] = 0; sp.dir[1] = 0; sp.dir[2] = -1;
        alignas(16) uint8_t rec[lt::kRecordSize];
        lt::buildRecord(rec, 7, lt::T_SPOT, lt::LF_SHADOWS | lt::LF_VOLUME, sp);
        float f; uint32_t u; uint64_t tok;
        memcpy(&u, rec + 0x60, 4); CHECK(u == 2, "record: spot is type 2");
        memcpy(&u, rec + 0x64, 4);
        CHECK((u & 0x1C0u) == 0x1C0u && (u & (1u << 14)) && (u & (1u << 12)) && !(u & 3u) && (u & 0x80000000u), "record flags (0x%08X)", u);
        memcpy(&f, rec + 0x98, 4); CHECK(f == 40.0f, "record: range");
        memcpy(&f, rec + 0xB4, 4); CHECK(fabsf(f - 0.5f) < 1e-4f, "record: cos(outer 60) = 0.5");
        memcpy(&f, rec + 0xC0, 4); CHECK(fabsf(f - 40.0f * 0.8660254f) < 1e-3f, "record: cone radius at full range");
        memcpy(&tok, rec + 0x88, 8); CHECK(tok == 0x1007ull, "record: shadow owner token");
        memcpy(&u, rec + 0x6C, 4); CHECK(u == (0x00FFFFFFu | (1u << 29)) && rec[0xD0] == 0xFF && rec[0xD3] == 0xFF, "record: every hour, never faded");
        lt::buildRecord(rec, 8, lt::T_POINT, 0, lt::defaultParams(lt::T_POINT));
        memcpy(&u, rec + 0x64, 4); memcpy(&tok, rec + 0x88, 8);
        CHECK((u & 0x1C0u) == 0 && tok == 0 && (u & (1u << 14)), "record: a point without shadows has no owner token");
        lt::buildRecord(rec, 10, lt::T_POINT, lt::LF_SUN, lt::defaultParams(lt::T_POINT));
        memcpy(&u, rec + 0x64, 4);
        CHECK((u & (1u << 9)) != 0 && (u & (1u << 14)) != 0, "record: 'from the sun' sets CALC_FROM_SUNLIGHT (0x%08X)", u);
        sp.scale = 1.5f;
        lt::buildRecord(rec, 9, lt::T_SPOT, 0, sp);
        memcpy(&f, rec + 0x98, 4); CHECK(f == 60.0f, "record: scale 1.5 takes the 40 m range to 60 m");
        memcpy(&f, rec + 0xB4, 4); CHECK(fabsf(f - 0.5f) < 1e-4f, "record: a scaled spot keeps its cone angle");
        memcpy(&f, rec + 0xC0, 4); CHECK(fabsf(f - 60.0f * 0.8660254f) < 1e-3f, "record: the cone's end grows with the scale");
        sp.scale = 0.0f; lt::buildRecord(rec, 9, lt::T_SPOT, 0, sp);
        memcpy(&f, rec + 0x98, 4); CHECK(f == 40.0f, "record: a scale that is not a number counts as 1");

        static lt::Light Lk; memset(&Lk, 0, sizeof(Lk)); Lk.type = lt::T_SPOT; Lk.base = lt::defaultParams(lt::T_SPOT);
        lt::Params ka = Lk.base, kb = Lk.base;
        ka.pos[0] = 0; kb.pos[0] = 10; ka.dir[0] = 0; ka.dir[1] = 0; ka.dir[2] = -1; kb.dir[0] = 0; kb.dir[1] = 0; kb.dir[2] = 1;
        ka.scale = 1.0f; kb.scale = 3.0f;
        CHECK(lt::insertKey(Lk, 3000, kb) == 0 && lt::insertKey(Lk, 1000, ka) == 0 && Lk.nkeys == 2 && Lk.keys[1].t == 3000.0f, "keys stay in time order");
        const lt::Params mid = lt::evalLight(Lk, 2000);
        CHECK(fabsf(mid.pos[0] - 5.0f) < 1e-4f && fabsf(lt::evalLight(Lk, 0).pos[0]) < 1e-6f && fabsf(lt::evalLight(Lk, 9000).pos[0] - 10.0f) < 1e-6f,
              "halfway between keys, holding before the first and after the last");
        CHECK(fabsf(lt::dot3(mid.dir, mid.dir) - 1.0f) < 1e-4f && fabsf(mid.dir[2]) < 1e-3f, "a spot turning from down to up passes the horizon, not zero");
        CHECK(fabsf(mid.scale - 2.0f) < 1e-5f, "the scale blends between keys");
        CHECK(lt::keyAt(Lk, 1003) == 0 && lt::keyAt(Lk, 1010) == -1, "a key is found within 5 ms");

        lt::Cam cam = {}; cam.fwd[1] = 1; cam.right[0] = 1; cam.up[2] = 1; cam.fov = 60;
        const lt::View vw = lt::makeView(cam, 1920, 1080);
        const float ahead[3] = { 0, 10, 0 }, off[3] = { 2, 10, 1 };
        float px[2], ray[3];
        CHECK(lt::toScreen(vw, ahead, px) && fabsf(px[0] - 960) < 1e-3f && fabsf(px[1] - 540) < 1e-3f, "straight ahead is the middle of the screen");
        CHECK(lt::toScreen(vw, off, px), "a point up and to the right is on screen");
        lt::rayAt(vw, px[0], px[1], ray);
        CHECK(fabsf(ray[0] - 0.19518f) < 1e-3f && fabsf(ray[1] - 0.97590f) < 1e-3f && fabsf(ray[2] - 0.09759f) < 1e-3f, "the mouse ray goes back through that point");
        float sv; const float o3[3] = { 0, 0, 0 }, ax[3] = { 1, 0, 0 }, ro[3] = { 5, -10, 0 }, rdv[3] = { 0, 1, 0 };
        CHECK(lt::axisParam(o3, ax, ro, rdv, &sv) && fabsf(sv - 5.0f) < 1e-4f, "dragging an arrow follows the mouse along its axis");
        float hit3[3]; const float nz[3] = { 0, 0, 1 }, down[3] = { 0, 0.6f, -0.8f }, top[3] = { 0, 0, 8 };
        CHECK(lt::rayPlane(top, down, o3, nz, hit3) && fabsf(hit3[1] - 6.0f) < 1e-4f && fabsf(hit3[2]) < 1e-5f, "ray meets the ground plane");

        {
            lt::Lock lk;
            lt::clearStore();
            lt::LightSet* s1 = lt::findSet("Project 20", 1, true);
            lt::addLight(*s1, lt::T_SPOT, &cam);
            strcpy_s(s1->l[0].name, "Key light");
            s1->l[0].flags = lt::LF_SHADOWS;
            lt::insertKey(s1->l[0], 1500, s1->l[0].base);
            s1->l[0].keys[0].p.scale = 2.5f;
            lt::LightSet* s2 = lt::findSet("Project 20", 2, true);
            lt::addLight(*s2, lt::T_POINT, nullptr);
            const std::string text = lt::serialize();
            lt::clearStore();
            const int n = lt::parse(text);
            lt::LightSet* r1 = lt::findSet("Project 20", 1, false);
            lt::LightSet* r2 = lt::findSet("Project 20", 2, false);
            CHECK(n == 2 && r1 && r2 && r1->count == 1 && r2->count == 1 && strcmp(r1->l[0].name, "Key light") == 0 && r1->l[0].type == lt::T_SPOT &&
                  r1->l[0].flags == lt::LF_SHADOWS && r1->l[0].nkeys == 1 && r1->l[0].keys[0].t == 1500.0f && fabsf(r1->l[0].base.pos[1] - 1.5f) < 1e-5f,
                  "lights, names, switches and keys survive the file");
            CHECK(r1 && r1->l[0].nkeys == 1 && r1->l[0].keys[0].p.scale == 2.5f && r1->l[0].base.scale == 1.0f, "the scale survives the file");
            lt::clearStore();
            const std::string v1 = "EscoEditorLights 1\nscope 0 Project 21\nlight 4 1 1 0x1F Light 4\n"
                "b -572.5 4207 202.5 0 -0.75 -0.5 0.25 0.5 0.375 29.5 61 43.5 37.5 81.5 1 1\n"
                "k 5022.5 -572.5 4207 202.5 0 -0.75 -0.5 0.25 0.5 0.375 29.5 61 43.5 37.5 81.5 1 1\n";
            const int n1 = lt::parse(v1);
            lt::LightSet* o = lt::findSet("Project 21", 0, false);
            CHECK(n1 == 1 && o && o->count == 1 && o->l[0].type == lt::T_SPOT && o->l[0].base.range == 61.0f && o->l[0].base.scale == 1.0f &&
                  o->l[0].nkeys == 1 && o->l[0].keys[0].t == 5022.5f && o->l[0].keys[0].p.range == 61.0f && o->l[0].keys[0].p.scale == 1.0f,
                  "a 2.8 lights file (no scale) still reads, at scale 1");
            lt::clearStore();
        }
    }

    // ---- following: frames, re-framing, the pools, the file -----------------
    {
        namespace lt = lights;
        // a frame turned a quarter (its right = north, its front = west) at (10, 20, 30)
        lt::Frame F = { { 0, 1, 0 }, { -1, 0, 0 }, { 0, 0, 1 }, { 10, 20, 30 } };
        CHECK(lt::tidyFrame(F), "a turned frame is a rotation");
        lt::Frame bad = { { 1, 0, 0 }, { 1, 0, 0 }, { 0, 0, 1 }, { 0, 0, 0 } };
        CHECK(!lt::tidyFrame(bad), "two equal rows are not a rotation");
        lt::Params w = lt::defaultParams(lt::T_SPOT);
        w.pos[0] = 12; w.pos[1] = 23; w.pos[2] = 31; w.dir[0] = 0; w.dir[1] = 1; w.dir[2] = 0;
        const lt::Params l = lt::toLocal(F, w);
        CHECK(fabsf(l.pos[0] - 3) < 1e-5f && fabsf(l.pos[1] + 2) < 1e-5f && fabsf(l.pos[2] - 1) < 1e-5f && fabsf(l.dir[0] - 1) < 1e-5f && fabsf(l.dir[1]) < 1e-5f,
              "a world point in the frame: 3 m to its right, 2 m behind, 1 m up; north is its right");
        const lt::Params back = lt::toWorld(F, l);
        CHECK(fabsf(back.pos[0] - 12) < 1e-4f && fabsf(back.pos[1] - 23) < 1e-4f && fabsf(back.pos[2] - 31) < 1e-4f && fabsf(back.dir[1] - 1) < 1e-5f, "and back into the world");

        static lt::Light La; memset(&La, 0, sizeof(La)); La.type = lt::T_SPOT; La.base = w;
        lt::insertKey(La, 1000, w);
        CHECK(lt::attachTo(La, lt::A_ENTITY, F, nullptr, lt::E_VEHICLE, 0x1234, nullptr) && fabsf(La.base.pos[0] - 3) < 1e-5f && fabsf(La.keys[0].p.pos[1] + 2) < 1e-5f &&
              La.at.mode == lt::A_ENTITY && La.at.kind == lt::E_VEHICLE && La.at.model == 0x1234 && La.at.last[0] == 10.0f,
              "attaching keeps the light where it is: the whole clip and every key go into the vehicle's frame");
        lt::detach(La, nullptr);
        CHECK(La.at.mode == lt::A_NONE && !La.frameKnown && fabsf(La.base.pos[0] - 12) < 1e-4f && fabsf(La.keys[0].p.pos[2] - 31) < 1e-4f && fabsf(La.base.dir[1] - 1) < 1e-5f,
              "letting go puts it back in the world where it was");
        La.at.mode = lt::A_ENTITY; La.frameKnown = false;   // following something never seen: the fallback stands in
        lt::Frame camF = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 100, 0, 0 } };
        lt::detach(La, &camF);
        CHECK(fabsf(La.base.pos[0] - 112) < 1e-4f, "letting go of something never seen places it from the camera");

        {   // landing on what it follows keeps a keyed movement: everything shifts together
            static lt::Light J;
            memset(&J, 0, sizeof(J));
            J.id = 3;
            J.type = lt::T_POINT;
            J.base = lt::defaultParams(lt::T_POINT);
            J.at.mode = lt::A_ENTITY;
            lt::insertKey(J, 0.0f, J.base);
            J.keys[0].p.pos[0] = 2; J.keys[0].p.pos[1] = 0; J.keys[0].p.pos[2] = 0;
            lt::Params far2 = J.keys[0].p;
            far2.pos[0] = 6;                                  // 4 m further on in the parent's frame
            lt::insertKey(J, 1000.0f, far2);
            lt::jumpToParent(J, 0.0f);
            CHECK(J.nkeys == 2 && fabsf(J.keys[0].p.pos[0]) < 1e-5f && fabsf(J.keys[0].p.pos[2] - lt::kJumpUp) < 1e-5f &&
                  fabsf(J.keys[1].p.pos[0] - 4.0f) < 1e-5f,
                  "it lands on what it follows and keeps the keyed movement (%.2f then %.2f)", J.keys[0].p.pos[0], J.keys[1].p.pos[0]);
            const float was = J.keys[0].p.pos[2];
            J.at.mode = lt::A_NONE;
            lt::jumpToParent(J, 0.0f);
            CHECK(J.keys[0].p.pos[2] == was, "a light that follows nothing does not jump");
        }

        // The pool search runs only while a clip is open in the editor, so the
        // tests stand in for that: replay mode 2 is "the editor is open".
        static int fakeReplayMode = 2;
        g_replayMode = &fakeReplayMode;
        CHECK(editorOpen(), "the tests can say a clip is open");

        // a fake generic pool (peds): storage, flags (bit 7 = free), count, item size
        constexpr int kItem = 0x100;
        static uint8_t storage[3 * kItem];
        static uint8_t flags[3] = { 0x00, 0x80, 0x00 };
        static uint32_t archA[8], archB[8];
        archA[0x18 / 4] = 0xAAAA0001; archB[0x18 / 4] = 0xBBBB0002;
        auto putEnt = [kItem](int i, uint32_t* arch, float x) {
            uint8_t* e = storage + i * kItem;
            *(uint32_t**)(e + 0x20) = arch;
            const float m[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  x, 0, 0, 1 };
            memcpy(e + 0x60, m, sizeof(m));
        };
        putEnt(0, archA, 100); putEnt(1, archA, 5); putEnt(2, archA, 10);
        struct FakeGenericPool { uint8_t* storage; uint8_t* flags; int32_t size; int32_t item; };
        static FakeGenericPool gp = { storage, flags, 3, kItem };
        static FakeGenericPool* gpp = &gp;
        lt::a_Pool[lt::E_PED] = (uintptr_t)&gpp;
        void* e0 = storage; void* e1 = storage + kItem; void* e2 = storage + 2 * kItem;
        lt::Attach at = { lt::A_ENTITY, lt::E_PED, 0xAAAA0001, { 9, 0, 0 } };
        CHECK(lt::findEntity(at, e2) == e2, "the entity it was on stays while its slot holds the same model");

        {   // the vehicle pool: an array of pointers, not objects in a row
            static uint8_t v0[kItem], v1[kItem], v2[kItem];
            static uint32_t varch[8];
            varch[0x18 / 4] = 0xCCCC0003;
            auto putVeh = [](uint8_t* e, uint32_t* arch, float x) {
                *(uint32_t**)(e + 0x20) = arch;
                const float m[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  x, 0, 0, 1 };
                memcpy(e + 0x60, m, sizeof(m));
            };
            memset(v0, 0, sizeof(v0)); memset(v1, 0, sizeof(v1)); memset(v2, 0, sizeof(v2));
            putVeh(v0, varch, 1); putVeh(v1, varch, 2); putVeh(v2, varch, 3);
            static uint64_t slots[4] = { (uint64_t)v0, 0, (uint64_t)v1, (uint64_t)v2 };
            static uint32_t inuse[1] = { 0x0000000Du };          // slots 0, 2 and 3
            struct FakeVehPool { uint64_t* slots; int32_t size; uint8_t pad[0x30 - 0x0C]; uint32_t* bits; };
            static FakeVehPool vp = {};
            vp.slots = slots; vp.size = 4; vp.bits = inuse;
            static FakeVehPool* vpp = &vp;
            CHECK((uint8_t*)&vp.bits - (uint8_t*)&vp == 0x30, "the fake vehicle pool has its bits at +0x30 (%d)",
                  (int)((uint8_t*)&vp.bits - (uint8_t*)&vp));
            lt::a_Pool[lt::E_VEHICLE] = (uintptr_t)&vpp;
            lt::g_vehReady = 0;
            lt::g_vehTriedAt = 0;
            lt::g_vehTries = 0;
            lt::PoolView pv = {};
            CHECK(!lt::poolView(lt::E_VEHICLE, pv), "the game thread reads no vehicles until the worker has worked the pool out");
            lt::probeVehiclePool(GetTickCount64());      // the worker's job
            CHECK(lt::poolView(lt::E_VEHICLE, pv) && pv.slots == slots && pv.size == 4,
                  "the worker finds the layout and the game thread then reads it");
            CHECK(lt::g_vehReady && lt::g_vehLay.pointers && lt::g_vehLay.arrOff == 0 &&
                  lt::g_vehLay.sizeOff == 0x08 && lt::g_vehLay.bitsOff == 0x30,
                  "and the offsets it settled on are the right ones (arr +0x%X, count +0x%X, bits +0x%X)",
                  lt::g_vehLay.arrOff, lt::g_vehLay.sizeOff, lt::g_vehLay.bitsOff);
            CHECK(lt::slotAt(pv, 0) == v0 && lt::slotAt(pv, 2) == v1 && lt::slotAt(pv, 3) == v2,
                  "each in-use slot gives its own vehicle");
            CHECK(lt::slotAt(pv, 1) == nullptr, "a free slot gives nothing");
            CHECK(lt::entitiesIn(pv, 4) == 3, "three vehicles are found in it (%d)", lt::entitiesIn(pv, 4));
            lt::Attach vat = { lt::A_ENTITY, lt::E_VEHICLE, 0xCCCC0003, { 2.4f, 0, 0 } };
            int vslot = -1;
            void* vfound = lt::findEntity(vat, nullptr, &vslot);
            CHECK(vfound == v1 && vslot == 2, "a vehicle is found by model, nearest where it was, and its slot remembered (%d)", vslot);
            // 3.13 divided (cached - storage) by the item size here. The vehicle
            // pool has no item size, so that was a divide by zero and an
            // unhandled exception the frame after a light was put on a car.
            CHECK(lt::findEntity(vat, vfound, &vslot) == v1, "and the next frame it is confirmed from that slot, without dividing by anything");
            int badSlot = 99;
            CHECK(lt::findEntity(vat, vfound, &badSlot) == v1 && badSlot == 2, "a slot out of range just means looking again (%d)", badSlot);
            {   // the replay puts its cars where there is room: slots 300 and 301,
                // past anything a shallow sample would look at
                static uint64_t deep[320] = {};
                static uint32_t deepBits[10] = {};
                deep[300] = (uint64_t)v0;
                deep[301] = (uint64_t)v1;
                deepBits[300 >> 5] = (1u << (300 & 31)) | (1u << (301 & 31));
                struct FakeVehPool2 { uint64_t* slots; int32_t size; uint8_t pad[0x30 - 0x0C]; uint32_t* bits; };
                static FakeVehPool2 dp = {};
                dp.slots = deep; dp.size = 320; dp.bits = deepBits;
                static FakeVehPool2* dpp = &dp;
                lt::a_Pool[lt::E_VEHICLE] = (uintptr_t)&dpp;
                lt::g_vehReady = 0;
                lt::g_vehTriedAt = 0;
                lt::g_vehTries = 0;
                lt::probeVehiclePool(GetTickCount64());
                lt::PoolView dv = {};
                CHECK(lt::g_vehReady && lt::poolView(lt::E_VEHICLE, dv) && lt::slotAt(dv, 300) == v0 && lt::slotAt(dv, 301) == v1,
                      "cars sitting in slots 300 and 301 are still found (a shallow sample missed them in 3.14 - 3.17)");
            }
            lt::a_Pool[lt::E_VEHICLE] = 0;
            lt::g_vehReady = 0;
            lt::g_vehTriedAt = 0;
            lt::g_vehTries = 0;
        }
        CHECK(lt::findEntity(at, e1) == e2, "its slot freed: the same model nearest to where it was last seen");
        at.last[0] = 90;
        CHECK(lt::findEntity(at, nullptr) == e0, "nearest to the place it was last seen");
        at.model = 0xBBBB0002;
        CHECK(lt::findEntity(at, nullptr) == nullptr, "no entity of that model: nothing to follow");
        // a fake vehicle pool behind one more pointer: storage, count, item size +0x18, in-use bitset +0x30
        struct FakeVehiclePool { uint8_t* storage; int32_t size; int32_t pad0; uint64_t pad1; uint64_t item; uint64_t pad2[2]; uint32_t* bits; };
        static_assert(offsetof(FakeVehiclePool, item) == 0x18 && offsetof(FakeVehiclePool, bits) == 0x30, "the game's vehicle pool layout");
        static uint32_t vbits[1] = { 0x5 };
        static FakeVehiclePool vp = { storage, 3, 0, 0, kItem, { 0, 0 }, vbits };
        static FakeVehiclePool* vpp = &vp;
        static FakeVehiclePool** vppp = &vpp;
        lt::a_Pool[lt::E_VEHICLE] = (uintptr_t)&vppp;
        lt::g_vehReady = 0;
        lt::g_vehTriedAt = 0;
        lt::g_vehTries = 0;
        lt::probeVehiclePool(GetTickCount64());      // the worker works the layout out; the game thread only reads it
        lt::PoolView pv;
        CHECK(lt::g_vehReady && !lt::g_vehLay.pointers && lt::g_vehLay.hops == 1 && lt::g_vehLay.sizeOff == 0x08 && lt::g_vehLay.bitsOff == 0x30,
              "a pool of objects behind two pointers is recognised as that (hops %d, pointers %d)", lt::g_vehLay.hops, (int)lt::g_vehLay.pointers);
        CHECK(lt::poolView(lt::E_VEHICLE, pv) && pv.size == 3 && pv.item == kItem && lt::slotAt(pv, 1) == nullptr && lt::slotAt(pv, 2) == e2,
              "the vehicle pool: in-use bitset, storage + slot x item size");
        lt::Cam cam0 = {}; cam0.fwd[1] = 1; cam0.right[0] = 1; cam0.up[2] = 1; cam0.fov = 60;
        lt::gatherScene(cam0, 1);
        const lt::Scene sc = lt::scene();
        CHECK(sc.count == 4, "the scene near the camera: two peds and two vehicles (%d)", sc.count);
        static lt::Light Lp; memset(&Lp, 0, sizeof(Lp));
        Lp.at = { lt::A_ENTITY, lt::E_PED, 0xAAAA0001, { 9, 0, 0 } };
        lt::Ctx cx = {}; cx.valid = true; cx.cam = cam0; cx.cam.pos[2] = 7;
        lt::resolveParent(Lp, cx);
        CHECK(Lp.parentOk && Lp.ent == e2 && Lp.parent.d[0] == 10.0f && Lp.at.last[0] == 10.0f, "a light on a ped follows it and remembers where it was");
        flags[2] = 0x80;
        lt::resolveParent(Lp, cx);
        CHECK(Lp.parentOk && Lp.ent == e0, "the ped rebuilt in another slot: found again");
        flags[0] = 0x80;
        lt::resolveParent(Lp, cx);
        CHECK(!Lp.parentOk && Lp.frameKnown, "the ped gone: not drawn, its last frame kept");
        {   // and that kept frame is what the light is drawn at now, instead of
            // the light being dropped for that frame - the flicker on an
            // attached light when the camera is far from what it follows
            lt::Params held = lt::defaultParams(lt::T_POINT);
            held.pos[0] = 1; held.pos[1] = 0; held.pos[2] = 2;
            const lt::Params world = lt::toWorld(Lp.parent, held);
            CHECK(Lp.frameKnown && fabsf(world.pos[0] - (Lp.parent.d[0] + 1)) < 1e-4f,
                  "a light whose parent is out of the scene is still placed, at the last frame it saw (%.2f)", world.pos[0]);
        }
        Lp.at.mode = lt::A_CAMERA;
        lt::resolveParent(Lp, cx);
        CHECK(Lp.parentOk && Lp.parent.d[2] == 7.0f && Lp.parent.b[1] == 1.0f, "a light on the camera follows the camera's frame");
        lt::a_Pool[lt::E_PED] = 0; lt::a_Pool[lt::E_VEHICLE] = 0;

        {
            lt::Lock lk;
            lt::clearStore();
            lt::LightSet* s = lt::findSet("P", 0, true);
            lt::addLight(*s, lt::T_POINT, nullptr);
            s->l[0].at = { lt::A_ENTITY, lt::E_VEHICLE, 0xDEADBEEF, { 1.5f, -2.25f, 3.0f } };
            lt::addLight(*s, lt::T_SPOT, nullptr);
            s->l[1].at.mode = lt::A_CAMERA;
            const std::string text = lt::serialize();
            lt::clearStore();
            lt::parse(text);
            lt::LightSet* r = lt::findSet("P", 0, false);
            CHECK(r && r->count == 2 && r->l[0].at.mode == lt::A_ENTITY && r->l[0].at.kind == lt::E_VEHICLE && r->l[0].at.model == 0xDEADBEEF &&
                  r->l[0].at.last[1] == -2.25f && !r->l[0].frameKnown && r->l[1].at.mode == lt::A_CAMERA, "what a light follows survives the file");
            lt::clearStore();
        }
    }

    // ---- lens flares: the projection, the presets, the file, the pass -------
    {
        namespace lt = lights;
        float u = 0.0f, v = 0.0f;
        const float ahead[3] = { 0, 10, 0 };
        CHECK(lt::flareUv(ahead, 60.0f, 16.0f / 9.0f, &u, &v) && fabsf(u - 0.5f) < 1e-5f && fabsf(v - 0.5f) < 1e-5f,
              "a light straight ahead sits in the middle of the frame");
        const float upRight[3] = { 1, 10, 1 };
        CHECK(lt::flareUv(upRight, 60.0f, 16.0f / 9.0f, &u, &v) && u > 0.5f && v < 0.5f,
              "one up and to the right lands up and to the right (%.3f, %.3f)", u, v);
        const float behind[3] = { 0, -5, 0 };
        CHECK(!lt::flareUv(behind, 60.0f, 1.7778f, &u, &v), "one behind the lens has no place on screen");
        float uNarrow = 0.0f, uWide = 0.0f;
        lt::flareUv(upRight, 60.0f, 1.7778f, &uNarrow, &v);
        lt::flareUv(upRight, 100.0f, 1.7778f, &uWide, &v);
        CHECK(fabsf(uWide - 0.5f) < fabsf(uNarrow - 0.5f), "a wider lens brings it towards the middle");

        const lt::Flare cine = lt::flarePreset(lt::FP_CINEMATIC);
        CHECK(cine.on == 1.0f && cine.streak > 0.0f && cine.ghost > 0.0f && cine.halo > 0.0f &&
              lt::flarePreset(lt::FP_LEAK).leak == 1.0f && lt::flarePreset(lt::FP_OFF).on == 0.0f, "the flare presets");

        lt::Flare zero = {};
        lt::flareFill(zero);
        CHECK(zero.spokes == 3.0f && zero.ghostCount == 4.0f && fabsf(zero.ghostSpread - 0.35f) < 1e-6f &&
              fabsf(zero.haloSize - 0.18f) < 1e-6f && zero.squeeze == 1.0f && zero.flickerHz == 6.0f &&
              (zero.tint[0] + zero.tint[1] + zero.tint[2]) > 0.0f, "a zeroed flare is filled in with 3.3's look");
        CHECK(lt::flarePreset(lt::FP_BLUE).tintMix > 0.5f && lt::flarePreset(lt::FP_BLUE).cross > 0.0f &&
              lt::flarePreset(lt::FP_DIRTY).ghostCount >= 6.0f && lt::flarePreset(lt::FP_STAR).spokes >= 4.0f &&
              lt::flarePreset(lt::FP_CINEMATIC).flicker > 0.0f, "the new presets: Blue, Dirty, and the star's spikes");

        static lt::Light Lf;
        memset(&Lf, 0, sizeof(Lf));
        Lf.fl = cine;
        Lf.fl.bright = 1.0f;
        lt::Params pf = lt::defaultParams(lt::T_POINT);
        pf.pos[0] = 0; pf.pos[1] = 10; pf.pos[2] = 0; pf.intensity = 20.0f;
        lt::Cam camf = {}; camf.fwd[1] = 1; camf.right[0] = 1; camf.up[2] = 1; camf.fov = 60;
        lt::FlareSrc src = {};
        CHECK(lt::flareOf(Lf, pf, camf, src) && fabsf(src.cs[1] - 10.0f) < 1e-4f && fabsf(src.dist - 10.0f) < 1e-4f &&
              fabsf(src.bright - 2.0f) < 1e-4f, "a flare source: camera space, distance, brightness x the light's intensity");
        CHECK(src.spokes >= 2.0f && src.gcount >= 1.0f && src.squeeze >= 0.05f && src.flickerHz >= 0.05f &&
              src.tintMix >= 0.0f && src.tintMix <= 1.0f, "a flare source carries 3.5's numbers, clamped");
        {
            lt::FlareSrc s7 = {}, s8 = {};
            Lf.id = 7; lt::flareOf(Lf, pf, camf, s7);
            Lf.id = 8; lt::flareOf(Lf, pf, camf, s8);
            Lf.id = 0;
            CHECK(fabsf(s7.phase - s8.phase) > 1e-3f, "two lights flicker out of step (%.3f vs %.3f)", s7.phase, s8.phase);
        }
        Lf.fl.on = 0.0f;
        CHECK(!lt::flareOf(Lf, pf, camf, src), "no flare when it is switched off");
        Lf.fl.on = 1.0f;

        {   // a 3.3 file: its f line stops after nine numbers
            lt::Lock lk;
            lt::clearStore();
            lt::LightSet* s33 = lt::findSet("F33", 3, true);
            lt::addLight(*s33, lt::T_POINT, nullptr);
            s33->l[0].fl = cine;
            std::string text = lt::serialize();
            const size_t f0 = text.find("\nf ");
            CHECK(f0 != std::string::npos, "a flare is written to the file");
            if (f0 != std::string::npos)
            {
                const size_t eol = text.find('\n', f0 + 1);
                const std::string line = text.substr(f0 + 1, eol - f0 - 1);
                size_t cut = line.size();
                for (size_t i = 0, sp = 0; i < line.size(); ++i)
                    if (line[i] == ' ' && ++sp == 10) { cut = i; break; }
                text = text.substr(0, f0 + 1) + line.substr(0, cut) + text.substr(eol);
                lt::clearStore();
                lt::parse(text);
                lt::LightSet* r33 = lt::findSet("F33", 3, false);
                CHECK(r33 && r33->count == 1 && fabsf(r33->l[0].fl.size - cine.size) < 1e-5f &&
                      r33->l[0].fl.spokes == 3.0f && r33->l[0].fl.ghostCount == 4.0f && r33->l[0].fl.squeeze == 1.0f,
                      "a 3.3 flare line loads and gets 3.5's defaults");
            }
            lt::clearStore();
        }

        {
            lt::Lock lk;
            lt::clearStore();
            lt::LightSet* s = lt::findSet("F", 3, true);
            lt::addLight(*s, lt::T_SPOT, nullptr);
            s->l[0].fl = cine;
            s->l[0].fl.size = 1.75f;
            const std::string text = lt::serialize();
            lt::clearStore();
            lt::parse(text);
            lt::LightSet* r = lt::findSet("F", 3, false);
            CHECK(r && r->count == 1 && r->l[0].fl.on == 1.0f && r->l[0].fl.size == 1.75f && r->l[0].fl.halo == cine.halo,
                  "a light's flare survives the file");
            lt::clearStore();
        }

        char d[MAX_PATH];
        rs::resolveShaderDir(".\\reshade-shaders\\Shaders,.\\reshade-shaders\\Shaders\\Other", "C:\\base\\", d, sizeof(d));
        CHECK(strcmp(d, "C:\\base\\reshade-shaders\\Shaders\\") == 0, "first search path, relative (got %s)", d);
        rs::resolveShaderDir("D:\\sh\\**,.\\x", "C:\\base\\", d, sizeof(d));
        CHECK(strcmp(d, "D:\\sh\\") == 0, "absolute recursive search path (got %s)", d);
        rs::Frame fr = {};
        CHECK(!rs::frameDraw(fr, rs::K_DRAW, 3) && rs::frameDraw(fr, rs::K_INDEXED, 6) && !rs::frameDraw(fr, rs::K_INDEXED, 6) && fr.bbDraws == 3,
              "the picture comes in by a draw; the flare goes right before the first menu draw, once");
        fr = {}; rs::frameCopy(fr);
        CHECK(rs::frameDraw(fr, rs::K_INDEXED, 6) && fr.sceneKind == rs::K_COPY, "picture copied in: the flare goes before the first draw after it");
    }

    // ---- quaternions ------------------------------------------------------
    {
        const float a[3] = { 0.36f, 0.48f, 0.8f }, b[3] = { -0.8f, 0.6f, 0.0f }, c[3] = { -0.48f, -0.64f, 0.6f };
        CHECK(rq::validBasis(a, b, c), "test basis is orthonormal");
        const uint64_t packed = rq::encode(rq::fromRows(a, b, c));
        float a2[3], b2[3], c2[3]; rq::toRows(rq::decode(packed), a2, b2, c2);
        float err = 0.0f;
        for (int i = 0; i < 3; ++i) err = fmaxf(err, fmaxf(fabsf(a2[i] - a[i]), fmaxf(fabsf(b2[i] - b[i]), fabsf(c2[i] - c[i]))));
        CHECK(err < 1e-4f && rq::encode(rq::decode(packed)) == packed, "quaternion round trip");
    }

    // ---- the ENB option: the Rockstar Editor's DOF, drawn by ENB ----------------
    {
        namespace ed = enbdof;
        for (int i = 0; i < ed::N; ++i) ed::kinds[i] = (uint8_t)ed::kDefs[i].kind;
        CHECK(!strcmp(ed::kDefs[ed::I_DIST].ui, "--> Manual Focus : Distance") &&
              !strcmp(ed::kDefs[ed::I_APERTURE].ui, "--> Manual Focus : Aperture") &&
              !strcmp(ed::kDefs[ed::I_AUTO_APERTURE].ui, "--> Auto-focus : Aperture"),
              "the options the Rockstar Editor drives are ENB's exact UI names");

        float v[ed::N] = {};
        v[ed::I_DIST] = 7.0f; v[ed::I_APERTURE] = 0.5f; v[ed::I_AUTO_APERTURE] = 0.5f; v[ed::I_MOUSE] = 1.0f;
        ed::EdDof a = {}, b = {};
        a.have = true; a.mode = dof::MODE_CUSTOM; a.focus = dof::FOCUS_MANUAL; a.dist = 4.0f;  a.intensity = 50.0f;  a.t = 0.0f;
        b.have = true; b.mode = dof::MODE_CUSTOM; b.focus = dof::FOCUS_MANUAL; b.dist = 12.0f; b.intensity = 100.0f; b.t = 1000.0f;
        ed::applyEditor(a, b, 0.0f, 1.0f, v, nullptr, 0);
        CHECK(fabsf(v[ed::I_DIST] - 4.0f) < 1e-4f && fabsf(v[ed::I_APERTURE] - 0.125f) < 1e-5f && fabsf(v[ed::I_AUTO_APERTURE] - 1.0f) < 1e-5f,
              "following a Custom keyframe: focus at its 4 m, intensity 50%% = aperture 0.125 (Auto-focus 1.0) (%.2f m, %.4f)",
              v[ed::I_DIST], v[ed::I_APERTURE]);
        ed::applyEditor(a, b, 500.0f, 1.0f, v, nullptr, 0);
        CHECK(fabsf(v[ed::I_DIST] - 8.0f) < 1e-4f && fabsf(v[ed::I_APERTURE] - 0.28125f) < 1e-5f && fabsf(v[ed::I_AUTO_APERTURE] - 1.5f) < 1e-5f,
              "halfway to the next keyframe it blends: 8 m, 75%% = aperture 0.281 (%.2f m, %.4f)", v[ed::I_DIST], v[ed::I_APERTURE]);
        ed::applyEditor(b, b, 1000.0f, 1.0f, v, nullptr, 0);
        CHECK(fabsf(v[ed::I_APERTURE] - 0.5f) < 1e-5f, "100%% is NVE's own aperture, 0.5 (%.3f)", v[ed::I_APERTURE]);

        ed::EdDof none = a;
        none.mode = dof::MODE_NONE;
        float nv[ed::N] = {};
        nv[ed::I_DIST] = 7.0f; nv[ed::I_APERTURE] = 0.3f; nv[ed::I_AUTO_APERTURE] = 1.2f;
        ed::applyEditor(none, b, 0.0f, 1.0f, nv, nullptr, 0);
        CHECK(nv[ed::I_DIST] == 7.0f && nv[ed::I_APERTURE] == 0.3f && nv[ed::I_AUTO_APERTURE] == 1.2f,
              "a keyframe whose game Focus is None leaves ENB's DOF alone - None only keeps the game's own blur out");

        float keep[ed::N] = {};
        keep[ed::I_DIST] = 7.0f; keep[ed::I_APERTURE] = 0.5f;
        ed::EdDof def = a;
        def.mode = dof::MODE_DEFAULT;
        ed::applyEditor(def, b, 0.0f, 1.0f, keep, nullptr, 0);
        CHECK(keep[ed::I_DIST] == 7.0f && keep[ed::I_APERTURE] == 0.5f, "a Default keyframe leaves ENB's focus and aperture alone");

        ed::EdDof autoF = a;
        autoF.focus = dof::FOCUS_AUTO;
        float af[ed::N] = {};
        af[ed::I_DIST] = 7.0f;
        ed::applyEditor(autoF, autoF, 0.0f, 1.0f, af, nullptr, 0);
        CHECK(af[ed::I_DIST] == 4.0f && fabsf(af[ed::I_APERTURE] - 0.125f) < 1e-5f,
              "auto focus: NVE's Manual technique cannot measure one, so the keyframe's own focal distance is used");

        // NVE's Distance is not metres: the plane in focus is Distance^2 x near x (1 - near / z)
        {
            const float n = 0.1f;
            CHECK(fabsf(ed::metresOf(7.0f, n) - 4.798f) < 0.01f, "NVE's own 7.0 focuses at about 4.8 m (%.3f)", ed::metresOf(7.0f, n));
            bool round = true;
            const float ms[] = { 0.5f, 1.0f, 4.9f, 38.9f, 51.6f, 300.0f, 1500.0f };
            for (float m : ms)
            {
                const float d = ed::enbDistance(m, n);
                const float depth = n / m;                                       // GTA's reversed depth there
                const float focus = 1.0f - 1.0f / (d * d * depth);               // what NVE's manual pass computes
                if (fabsf(ed::metresOf(d, n) - m) > m * 1e-3f || fabsf(focus - depth) > 1e-4f) round = false;
            }
            CHECK(round, "metres -> NVE's Distance -> metres round trip, and NVE's own formula puts the focus exactly there");
            CHECK(ed::enbDistance(51.6f, n) < 23.0f && ed::metresOf(51.6f, n) > 250.0f,
                  "a keyframe's 51.6 m is NVE's %.1f - sent as it was until 4.9 it focused at %.0f m", ed::enbDistance(51.6f, n), ed::metresOf(51.6f, n));
        }

        // every ENB option keyed over the clip
        lights::EnbTrack tr = {};
        float k0[ed::N] = {}, k1[ed::N] = {};
        k0[ed::I_DIST] = 5.0f;  k1[ed::I_DIST] = 15.0f;     // a float
        k0[13] = 4.0f;          k1[13] = 9.0f;              // Blur : Quality Steps, a whole number
        k0[17] = 1.0f;          k1[17] = 0.0f;              // Anamorphic enable, a switch
        CHECK(lights::addEnbKey(tr, 0.0f, k0) == 0 && lights::addEnbKey(tr, 1000.0f, k1) == 1 && tr.n == 2, "ENB keys go in");
        float out[ed::N] = {};
        lights::evalEnb(tr, 500.0f, out, ed::kinds);
        CHECK(fabsf(out[ed::I_DIST] - 10.0f) < 1e-4f && out[13] == 7.0f && out[17] == 1.0f,
              "between keys: floats blend (%.2f), whole numbers round (%.0f), switches hold until the next key (%.0f)",
              out[ed::I_DIST], out[13], out[17]);
        lights::evalEnb(tr, 1000.0f, out, ed::kinds);
        CHECK(out[17] == 0.0f, "at the next key the switch changes");

        // NVE's six "-----" lines are labels the shader never reads; the menu's flat list
        {
            bool labels = true;
            for (int i = ed::I_SEC_NEAR; i <= ed::I_SEC_MISC; ++i) if (!ed::isLabel(i)) labels = false;
            for (int i = 0; i < ed::I_SEC_NEAR; ++i) if (ed::isLabel(i)) labels = false;
            CHECK(labels, "the six separator lines are labels, never sent to ENB; the 23 real options are not");
            // the menu list, as ENB's own window has it and no more
            for (int i = 0; i < ed::N; ++i) ed::s_known[i] = !ed::isLabel(i);
            ed::s_manual[ed::I_FOCUS] = 1.0f; ed::s_manual[ed::I_NEAR] = 1.0f; ed::s_manual[17] = 1.0f;
            ed::buildList();
            const int wantManual[] = { ed::I_FOCUS, ed::I_DIST, ed::I_APERTURE, ed::I_NEAR, ed::I_NEARPOWER, 12, 14, 11, 17, 18 };
            bool listOk = ed::listCount() == 10;
            for (int k = 0; listOk && k < 10; ++k) if (ed::listParam(k) != wantManual[k]) listOk = false;
            CHECK(listOk && ed::listParam(10) == -1, "Manual: Focus Mode, Focus Distance, Aperture, Near Field Blur / Power, Blur Size, Maximum Size, Chromatic Spread, Anamorphic / Stretch");
            ed::s_manual[ed::I_FOCUS] = 0.0f; ed::s_manual[ed::I_NEAR] = 0.0f; ed::s_manual[17] = 0.0f;
            ed::buildList();
            const int wantAuto[] = { ed::I_FOCUS, ed::I_AUTO_APERTURE, ed::I_NEAR, 12, 14, 11, 17 };
            bool autoOk = ed::listCount() == 7;
            for (int k = 0; autoOk && k < 7; ++k) if (ed::listParam(k) != wantAuto[k]) autoOk = false;
            CHECK(autoOk, "Auto: no distance, Auto-focus's own aperture, and a switch that is off hides what hangs on it");
            char fb[16];
            CHECK(!strcmp(ed::format(ed::I_FOCUS, 0.0f, fb, 16), "Auto") && !strcmp(ed::format(ed::I_FOCUS, 1.0f, fb, 16), "Manual") &&
                  ed::stepped(ed::I_FOCUS, 1.0f, 1) == 0.0f && ed::stepped(ed::I_FOCUS, 0.0f, -8) == 1.0f && ed::reshapes(ed::I_FOCUS),
                  "Focus Mode reads Auto / Manual, flips either way, and changing it redraws the rows");
            ed::kinds[ed::I_FOCUS] = (uint8_t)ed::kDefs[ed::I_FOCUS].kind;
            {
                lights::EnbTrack ft = {};
                float f0[ed::N] = {}, f1[ed::N] = {};
                f0[ed::I_FOCUS] = 1.0f; f1[ed::I_FOCUS] = 0.0f;
                lights::addEnbKey(ft, 0.0f, f0);
                lights::addEnbKey(ft, 1000.0f, f1);
                float fo[ed::N] = {};
                lights::evalEnb(ft, 900.0f, fo, ed::kinds);
                CHECK(fo[ed::I_FOCUS] == 1.0f, "Focus Mode holds until the next keyframe - no switch halfway");
            }
            for (int i = 0; i < ed::N; ++i) { ed::s_known[i] = false; ed::s_manual[i] = 0.0f; }
            CHECK(fabsf(ed::stepped(ed::I_NEARPOWER, 20.0f, 1) - 22.0f) < 1e-3f && ed::stepped(ed::I_NEARPOWER, 0.0f, 1) == 0.5f &&
                  ed::stepped(ed::I_NEARPOWER, 0.4f, -1) == 0.0f,
                  "near field power moves by 10%% of itself, starts from 0 and gets back to 0");
            CHECK(fabsf(ed::stepped(12, 1.0f, 4) - 1.2f) < 1e-4f && ed::stepped(12, 1.9f, 8) == 2.0f && ed::stepped(13, 6.0f, -8) == 1.0f,
                  "held, a press counts several times and stops at the shader's own limits");
            CHECK(fabsf(ed::stepped(ed::I_NEARPOWER, 20.0f, 4) - 20.0f * powf(1.1f, 4.0f)) < 0.01f, "held on a 10%% option, 4 presses in one");

            // a 4.9 key: 29 options, and a mask above 2^24 that a float could not hold
            namespace lt = lights;
            lt::Lock lk;
            lt::clearStore();
            lt::LightSet* ks = lt::findSet("Sec", 0, true);
            float vals[ed::N] = {};
            vals[ed::I_SEC_MISC] = 1.0f;
            lt::setEnbParam(ks->enb, 0.0f, ed::I_SEC_MISC, 1.0f, vals);
            ks->enb.k[0].set |= (1u << 28) | (1u << 1);
            const uint32_t want = ks->enb.k[0].set;
            const std::string txt = lt::serialize();
            lt::clearStore();
            lt::parse(txt);
            lt::LightSet* kb = lt::findSet("Sec", 0, false);
            CHECK(kb && kb->enb.n == 1 && kb->enb.k[0].set == want && kb->enb.k[0].v[ed::I_SEC_MISC] == 1.0f,
                  "a key setting option 28 reads back with its mask exact (0x%X)", kb ? kb->enb.k[0].set : 0u);
            lt::clearStore();
        }

        // finding ENB: the export reader against the real ENB file, and a module
        // walk that sees every module (4.7 stopped at 96 - FiveM loads more)
        {
            const wchar_t* enbPath = L"D:\\Grand Theft Auto V Legacy\\d3d11.dll";
            if (GetFileAttributesW(enbPath) != INVALID_FILE_ATTRIBUTES)
            {
                uint32_t img = 0;
                const uint32_t rs = ed::fileExportRva(enbPath, "ENBSetParameter", &img);
                const uint32_t rg = ed::fileExportRva(enbPath, "ENBGetParameter", &img);
                CHECK(rs && rg && rs < img && rg < img && rs != rg,
                      "ENBSetParameter / ENBGetParameter are read out of the real ENB d3d11.dll (rva 0x%X, 0x%X, image 0x%X)", rs, rg, img);
                CHECK(ed::fileExportRva(enbPath, "NoSuchExport", &img) == 0, "and a name it does not export gives 0");
            }
            ed::resolve();
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
            int real = 0;
            MODULEENTRY32W me = {};
            me.dwSize = sizeof(me);
            if (snap != INVALID_HANDLE_VALUE)
            {
                for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) ++real;
                CloseHandle(snap);
            }
            CHECK(!ed::s_found && ed::s_modsSeen == real && real > 0,
                  "the module walk sees every module in the process (%d of %d) and finds no ENB where there is none", ed::s_modsSeen, real);
        }

        // one option set on one keyframe: the others keep following the keyframes around it
        {
            lights::EnbTrack mt = {};
            float seed[ed::N] = {};
            seed[ed::I_DIST] = 7.0f; seed[12] = 1.0f;
            CHECK(lights::setEnbParam(mt, 1000.0f, 12, 3.0f, seed) && mt.n == 1 && mt.k[0].set == (1u << 12),
                  "a keyframe's own blur size is a key that sets only the blur size");
            float o[ed::N] = {};
            o[ed::I_DIST] = 99.0f;
            const uint32_t filled = lights::evalEnb(mt, 1000.0f, o, ed::kinds);
            CHECK(filled == (1u << 12) && o[12] == 3.0f && o[ed::I_DIST] == 99.0f,
                  "evaluating it decides the blur size and leaves the focus to the game's DOF (%.1f, %.1f)", o[12], o[ed::I_DIST]);
            float v2 = 0.0f;
            CHECK(lights::enbKeyValue(mt, 1000.0f, 12, &v2) && v2 == 3.0f && !lights::enbKeyValue(mt, 1000.0f, ed::I_DIST, &v2),
                  "the keyframe knows which options are its own");
            float ap[ed::N] = {};
            ed::EdDof ca = {};
            ca.have = true; ca.mode = dof::MODE_CUSTOM; ca.focus = dof::FOCUS_MANUAL; ca.dist = 4.0f; ca.intensity = 6.0f;
            ap[ed::I_APERTURE] = 2.2f;
            ca.intensity = 100.0f;
            ed::applyEditor(ca, ca, 0.0f, 1.0f, ap, nullptr, 0, 1u << ed::I_APERTURE);
            CHECK(ap[ed::I_APERTURE] == 2.2f && ap[ed::I_DIST] == 4.0f,
                  "the keyframe's own ENB Aperture row wins over the game's Intensity; the focus still follows (%.3f)", ap[ed::I_APERTURE]);
            ed::EdDof pct = ca;
            pct.intensity = 0.5f;                         // a fraction: 50%
            float pv[ed::N] = {};
            ed::applyEditor(pct, pct, 0.0f, 2.0f, pv, nullptr, 0);
            CHECK(fabsf(pv[ed::I_APERTURE] - 0.25f) < 1e-5f, "an intensity stored as 0..1 is a fraction, and the strength multiplies (%.3f)", pv[ed::I_APERTURE]);
            lights::clearEnbParam(mt, 1000.0f, 12);
            CHECK(mt.n == 0, "handing its only option back removes the keyframe's key");
        }

        // and they survive the lights file
        {
            namespace lt = lights;
            lt::Lock lk;
            lt::clearStore();
            lt::LightSet* es = lt::findSet("ENBProj", 0, true);
            es->enb = tr;
            const std::string text = lt::serialize();
            lt::clearStore();
            lt::parse(text);
            lt::LightSet* back = lt::findSet("ENBProj", 0, false);
            CHECK(back && back->enb.n == 2 && fabsf(back->enb.k[1].v[ed::I_DIST] - 15.0f) < 1e-4f && back->enb.k[0].v[17] == 1.0f,
                  "ENB keys are saved and read back with the clip");
            back->enb.k[1].set = (1u << 12) | (1u << ed::I_DIST);
            const std::string text2 = lt::serialize();
            lt::clearStore();
            lt::parse(text2);
            lt::LightSet* back2 = lt::findSet("ENBProj", 0, false);
            CHECK(back2 && back2->enb.n == 2 && back2->enb.k[1].set == ((1u << 12) | (1u << ed::I_DIST)) && back2->enb.k[0].set == lights::kEnbAll,
                  "which options a keyframe sets survives the file too");
            lt::clearStore();
            {
                std::string l410 = "EscoEditorLights 2\nscope 0 Old410\ne 250 4096";
                for (int q = 0; q < 29; ++q) l410 += " 3";
                l410 += "\n";
                lt::parse(l410);
                lt::LightSet* o410 = lt::findSet("Old410", 0, false);
                CHECK(o410 && o410->enb.n == 1 && o410->enb.k[0].set == 4096u && o410->enb.k[0].v[12] == 3.0f && o410->enb.k[0].v[ed::I_FOCUS] == 0.0f,
                      "a 4.9 / 4.10 key (29 options) still reads, and sets no Focus Mode");
            }
            std::string old46 = "EscoEditorLights 2\nscope 0 Old46\ne 500";
            for (int q = 0; q < 23; ++q) old46 += " 2";
            old46 += "\n";
            lt::parse(old46);
            lt::LightSet* o46 = lt::findSet("Old46", 0, false);
            CHECK(o46 && o46->enb.n == 1 && o46->enb.k[0].set == ((1u << 23) - 1u) && o46->enb.k[0].v[5] == 2.0f,
                  "a key written by 4.6 (every option, no mask) reads back as setting every option");
            lt::clearStore();
        }
    }

    // ---- a light that flashes ------------------------------------------------------
    {
        namespace lt = lights;
        lt::Flash f = {};
        f.mode = (float)lt::FX_BLINK; f.rate = 2.0f; f.depth = 1.0f; f.duty = 0.5f; f.phase = 0.0f;
        CHECK(lt::flashLevel(f, 0.10f, 1) == 1.0f && lt::flashLevel(f, 0.30f, 1) == 0.0f && lt::flashLevel(f, 0.60f, 1) == 1.0f,
              "Blink at 2 a second: on for the first quarter second, off for the next, on again");
        f.depth = 0.5f;
        CHECK(fabsf(lt::flashLevel(f, 0.30f, 1) - 0.5f) < 1e-5f, "depth 0.5: half as bright between blinks, not off");
        f.depth = 1.0f; f.phase = 0.5f;
        CHECK(lt::flashLevel(f, 0.10f, 1) == 0.0f, "an offset of half a beat takes turns with a light at 0");
        f.phase = 0.0f; f.mode = (float)lt::FX_PULSE;
        CHECK(lt::flashLevel(f, 0.0f, 1) < 1e-5f && fabsf(lt::flashLevel(f, 0.25f, 1) - 1.0f) < 1e-5f, "Pulse: dark at the start of a beat, full in its middle");
        f.mode = (float)lt::FX_DOUBLE;
        CHECK(lt::flashLevel(f, 0.02f, 1) == 1.0f && lt::flashLevel(f, 0.06f, 1) == 0.0f && lt::flashLevel(f, 0.10f, 1) == 1.0f && lt::flashLevel(f, 0.30f, 1) == 0.0f,
              "Double flash: two quick flashes, then a pause");
        f.mode = (float)lt::FX_STROBE; f.rate = 1.0f;
        CHECK(lt::flashLevel(f, 0.01f, 1) == 1.0f && lt::flashLevel(f, 0.10f, 1) == 0.0f, "Strobe: a short hard flash each beat");
        f.mode = (float)lt::FX_FLICKER; f.rate = 8.0f;
        bool inRange = true, varies = false, same = true;
        float prev = lt::flashLevel(f, 0.0f, 7);
        for (int i = 1; i < 400; ++i)
        {
            const float t = i * 0.0125f, v = lt::flashLevel(f, t, 7);
            if (!(v >= 0.0f && v <= 1.0f)) inRange = false;
            if (fabsf(v - prev) > 0.05f) varies = true;
            if (v != lt::flashLevel(f, t, 7)) same = false;
            prev = v;
        }
        CHECK(inRange && varies && same && lt::flashLevel(f, 1.3f, 7) != lt::flashLevel(f, 1.3f, 8),
              "Flicker wavers, stays in 0..1, is the same on every playback, and two lights flicker out of step");
        f.mode = 0.0f;
        CHECK(lt::flashLevel(f, 0.3f, 1) == 1.0f, "Off: the light as keyed");

        lt::Lock lk;
        lt::clearStore();
        lt::LightSet* fs = lt::findSet("FlashProj", 0, true);
        const int li = lt::addLight(*fs, lt::T_POINT, nullptr);
        fs->l[li].fx.mode = (float)lt::FX_DOUBLE; fs->l[li].fx.rate = 1.5f; fs->l[li].fx.depth = 0.8f; fs->l[li].fx.duty = 0.3f; fs->l[li].fx.phase = 0.25f;
        const std::string txt = lt::serialize();
        lt::clearStore();
        lt::parse(txt);
        lt::LightSet* fb = lt::findSet("FlashProj", 0, false);
        CHECK(fb && fb->count == 1 && fb->l[0].fx.mode == (float)lt::FX_DOUBLE && fb->l[0].fx.rate == 1.5f && fb->l[0].fx.depth == 0.8f &&
              fb->l[0].fx.duty == 0.3f && fb->l[0].fx.phase == 0.25f, "a light's flash is saved and read back");
        lt::clearStore();
    }

    // ---- time and weather keyed over a clip ---------------------------------
    {
        lights::SceneTrack sky = {};
        CHECK(lights::addSkyKey(sky, 0.0f, 18 * 60, 1) == 0 &&
              lights::addSkyKey(sky, 2000.0f, 20 * 60, 7) == 1 && sky.n == 2, "keys go in, sorted by time");
        CHECK(lights::addSkyKey(sky, 1000.0f, 19 * 60, 4) == 1 && sky.n == 3 && sky.k[1].minutes == 19 * 60,
              "a key in the middle is inserted in its place");
        CHECK(lights::addSkyKey(sky, 1002.0f, 19 * 60 + 30, 5) == 1 && sky.n == 3 && sky.k[1].minutes == 19 * 60 + 30,
              "a key at the same moment replaces it rather than doubling up");

        int mins = -1, from = -1, to = -1;
        float blend = -1.0f;
        CHECK(lights::evalSky(sky, 0.0f, &mins, &from, &to, &blend) && mins == 18 * 60 && from == 1 && blend == 0.0f,
              "at the first key: its own values (%02d:%02d)", mins / 60, mins % 60);
        lights::evalSky(sky, 500.0f, &mins, &from, &to, &blend);
        CHECK(mins == 18 * 60 + 45 && from == 1 && to == 5 && fabsf(blend - 0.5f) < 1e-4f,
              "halfway to the next key: the time is halfway and the weather blend is 0.5 (%02d:%02d, %d->%d, %.2f)",
              mins / 60, mins % 60, from, to, blend);
        lights::evalSky(sky, 99999.0f, &mins, &from, &to, &blend);
        CHECK(mins == 20 * 60 && from == 7 && to == 7 && blend == 0.0f, "past the last key it holds there");

        // the short way round midnight
        lights::SceneTrack night = {};
        lights::addSkyKey(night, 0.0f, 23 * 60, -1);
        lights::addSkyKey(night, 1000.0f, 1 * 60, -1);
        lights::evalSky(night, 500.0f, &mins, &from, &to, &blend);
        CHECK(mins == 0 && from < 0, "23:00 to 01:00 passes through midnight, not backwards through the day (%02d:%02d)",
              mins / 60, mins % 60);
        lights::evalSky(night, 250.0f, &mins, &from, &to, &blend);
        CHECK(mins == 23 * 60 + 30, "a quarter of the way is 23:30 (%02d:%02d)", mins / 60, mins % 60);

        lights::SceneTrack empty = {};
        CHECK(!lights::evalSky(empty, 100.0f, &mins, &from, &to, &blend) && mins == -1,
              "no keys means nothing keyed - the menu rows keep working as they did");

        lights::removeSkyKey(sky, 1);
        CHECK(sky.n == 2 && sky.k[1].minutes == 20 * 60, "a key can be taken out again");
    }

    // ---- the speed a marker keeps, as a percentage --------------------------
    {
        CHECK(mk::speedPercent(0) == 5 && mk::speedPercent(4) == 100 && mk::speedPercent(8) == 200,
              "the nine speeds the game stores by index");
        CHECK(mk::speedPercent(-1) == 100 && mk::speedPercent(9) == 100 && mk::speedPercent(1000) == 100,
              "anything the game does not know reads as 100 percent, the way its own switch defaults");
    }

    // ---- weather / clock packet patching ----------------------------------
    {
        alignas(16) unsigned char frame[scene::kClockSize + scene::kWeatherSize] = {};
        unsigned char* clk = frame; unsigned char* wea = frame + scene::kClockSize;
        auto hdr = [](unsigned char* pk, unsigned short id, unsigned size) {
            memcpy(pk, &id, 2); unsigned sw = size | (2u << 24); memcpy(pk + 4, &sw, 4); unsigned g = scene::kGuard; memcpy(pk + 8, &g, 4);
        };
        hdr(clk, scene::kIdClock, scene::kClockSize); clk[12] = 14; clk[13] = 30; clk[14] = 7;
        hdr(wea, scene::kIdWeather, scene::kWeatherSize);
        int rec = 6; memcpy(wea + 0x0C, &rec, 4); memcpy(wea + 0x10, &rec, 4);
        scene::origExtract = fakeExtract; scene::g_installed = true;
        int mode = 2; g_replayMode = &mode;
        scene::g_time = 20 * 60 + 15; scene::g_weather = 1;
        scene::hkExtract(wea);
        int seenType = -1; memcpy(&seenType, g_seenWeather + 0x0C, 4);
        int bufType = -1; memcpy(&bufType, wea + 0x0C, 4);
        CHECK(g_extractCalls == 1 && seenType == 1 && bufType == 6 && clk[12] == 20 && clk[13] == 15, "weather copy + clock patch");
        scene::g_time = -1; scene::g_weather = -1;
        scene::hkExtract(wea);
        CHECK(clk[12] == 14 && clk[13] == 30 && clk[14] == 7, "clock put back");
        g_replayMode = nullptr; scene::g_installed = false; scene::origExtract = nullptr;
    }

    // ---- keyframe clipboard: record writing and the ini round trip ---------
    {
        char tmp[MAX_PATH]; GetTempPathA(MAX_PATH, tmp); strcat_s(tmp, "EscoEditor_selftest.ini");
        DeleteFileA(tmp);
        strcpy_s(g_ini, tmp);
        clipboard::g_count = 2;
        for (int i = 0; i < 2; ++i)
        {
            clipboard::Item& it = clipboard::g_items[i];
            for (int b = 0; b < (int)mk::SIZE; ++b) it.rec[b] = (uint8_t)(b * 7 + i * 31);
            it.offsetMs = i * 1500.0f;
        }
        const int32_t attachId = 77; memcpy(clipboard::g_items[1].rec + 0x78, &attachId, 4);
        clipboard::g_items[1].rec[mk::OFF_Type] = 0;
        clipboard::save();
        clipboard::Item copy0 = clipboard::g_items[0], copy1 = clipboard::g_items[1];
        memset(clipboard::g_items, 0, sizeof(clipboard::g_items)); clipboard::g_count = 0;
        clipboard::load();
        CHECK(clipboard::g_count == 2 && memcmp(clipboard::g_items[0].rec, copy0.rec, mk::SIZE) == 0 &&
              memcmp(clipboard::g_items[1].rec, copy1.rec, mk::SIZE) == 0 && clipboard::g_items[1].offsetMs == 1500.0f, "keyframes survive the ini");
        uint8_t target[mk::SIZE] = {}; target[mk::OFF_Type] = 0;
        clipboard::writeRecord(target, clipboard::g_items[1], 4321.0f, true);
        float t; memcpy(&t, target + mk::OFF_TimeMs, 4); int32_t att; memcpy(&att, target + 0x78, 4);
        CHECK(t == 4321.0f && att == 77 && target[0x08] == copy1.rec[0x08], "same-clip paste keeps the targets, sets the time");
        clipboard::writeRecord(target, clipboard::g_items[1], 10.0f, false);
        memcpy(&att, target + 0x78, 4);
        CHECK(att == -1, "cross-clip paste resets the target ids");
        DeleteFileA(tmp);
    }

    // ---- the light editor's key: refusals, a pick, the walk ----------------
    {
        char tmp[MAX_PATH]; GetTempPathA(MAX_PATH, tmp); strcat_s(tmp, "EscoEditor_selftest_keys.ini");
        DeleteFileA(tmp);
        strcpy_s(g_ini, tmp);
        g_cfg.keySlower = 0x46; g_cfg.keyFaster = 0x47; g_cfg.keyReset = 0x30; g_cfg.keyBoost = 0xA0; g_cfg.keySlow = 0xA4;
        g_cfg.keyLights = 'L';
        lights::g_reshadeKey = VK_INSERT;
        CHECK(!lights::shortcutProblem('L') && !lights::shortcutProblem('K') && !lights::shortcutProblem(VK_F5) && !lights::shortcutProblem(VK_NUMPAD7),
              "L, K, F5 and keypad 7 can open the light editor");
        CHECK(lights::shortcutProblem('W') && lights::shortcutProblem('R') && lights::shortcutProblem('F') && lights::shortcutProblem('0') &&
              lights::shortcutProblem(VK_RETURN) && lights::shortcutProblem(VK_LSHIFT) && lights::shortcutProblem(VK_INSERT) &&
              lights::shortcutProblem('D') && lights::shortcutProblem(VK_NEXT) && lights::shortcutProblem(VK_LBUTTON),
              "keys already in use are refused");
        CHECK(lights::imguiKeyFor('L') == ImGuiKey_L && lights::imguiKeyFor('7') == ImGuiKey_7 && lights::imguiKeyFor(VK_F5) == ImGuiKey_F5 &&
              lights::imguiKeyFor(VK_NUMPAD3) == ImGuiKey_Keypad3 && lights::imguiKeyFor(VK_OEM_3) == ImGuiKey_GraveAccent &&
              lights::imguiKeyFor(VK_LBUTTON) == ImGuiKey_None, "keys map to ImGui's");
        {
            static bool seen[ImGuiKey_NamedKey_END];
            int mapped = 0; bool twice = false;
            for (int vk = 1; vk < 255; ++vk)
            {
                const ImGuiKey k = lights::imguiKeyFor(vk);
                if (k == ImGuiKey_None) continue;
                ++mapped;
                if (seen[k]) twice = true;
                seen[k] = true;
            }
            CHECK(mapped > 90 && !twice, "every watched key has its own ImGui key (%d)", mapped);
        }
        lights::beginCapture(lights::CAP_MENU);
        CHECK(!lights::offerKey('W') && lights::g_capture == 1 && lights::g_captureEnd == lights::END_REFUSED && strstr(lights::g_keyMsg, "can't be used") != nullptr,
              "a key in use is refused and the pick goes on (%s)", lights::g_keyMsg);
        CHECK(lights::offerKey('K') && lights::g_capture == 0 && g_cfg.keyLights == 'K' && iniInt("KeyLights", 0, 0, 255) == 'K' &&
              lights::g_captureEnd == lights::END_SAVED, "a free key is taken and saved (%s)", lights::g_keyMsg);
        lights::beginCapture(lights::CAP_WINDOW);
        CHECK(lights::offerKey(VK_ESCAPE) && lights::g_capture == 0 && g_cfg.keyLights == 'K' && lights::g_captureEnd == lights::END_CANCELLED,
              "Esc cancels and the key stays (%s)", lights::g_keyMsg);
        CHECK(lights::nextShortcut('L', +1) == 'K' && lights::nextShortcut('L', -1) == VK_END && lights::nextShortcut('M', +1) == 'B' &&
              lights::nextShortcut('Q', +1) == 'L', "left/right walk the list of keys");
        lights::g_reshadeKey = VK_HOME;
        CHECK(lights::nextShortcut(VK_INSERT, +1) == VK_END, "left/right skip a taken key");
        lights::stepShortcut(+1);
        CHECK(g_cfg.keyLights == 'J' && iniInt("KeyLights", 0, 0, 255) == 'J', "left/right save the key");
        CHECK(lights::since(100, 250) == 0 && lights::since(250, 100) == 150, "a clock read on another thread never runs backwards");
        lights::g_reshadeKey = 0;
        DeleteFileA(tmp);
    }

    CHECK(parsePattern(kSigLegacy.pat).n == 39 && parsePattern(director::kSig).n == 59 && parsePattern(game::kWeather).n == 48 &&
          parsePattern(game::kTcTable).n == 64 && parsePattern(game::kGnmi).n == 47 && parsePattern(menu::kPmm).n == 26, "pattern lengths");

    {   // same lights in every clip
        namespace lt = lights;
        lt::Lock lk;
        lt::clearStore();
        lt::LightSet* c0 = lt::findSet("P", 0, true);
        lt::LightSet* c1 = lt::findSet("P", 1, true);
        lt::LightSet* q0 = lt::findSet("Q", 0, true);
        lt::addLight(*c0, lt::T_SPOT, nullptr);
        lt::addLight(*q0, lt::T_POINT, nullptr);
        c0->l[0].ent = (void*)0x1234;                 // the game thread's view must not travel
        g_cfg.lightsAll = true;
        lt::g_cur = c0;
        lt::g_curClip = 0;
        strcpy_s(lt::g_curProject, "P");
        const int n = lt::syncAllClips();
        CHECK(n == 1 && c1->count == 1 && c1->l[0].type == lt::T_SPOT && c1->l[0].ent == nullptr,
              "every clip of the project gets the open clip's lights (%d clip(s))", n);
        CHECK(q0->count == 1 && q0->l[0].type == lt::T_POINT, "another project is left alone");

        lt::LightSet* c2 = lt::findSet("P", 2, true);
        CHECK(lt::seedFromSiblings(c2, "P") && c2->count == 1, "a clip opened for the first time starts with them");
        CHECK(!lt::seedFromSiblings(c2, "P"), "a clip that already has lights is not overwritten");

        g_cfg.lightsAll = false;
        c1->count = 0;
        CHECK(lt::syncAllClips() == 0 && c1->count == 0, "with the switch off each clip keeps its own");

        // bringing a rig over from another project, the way a lights file from
        // another machine has to be used
        {
            lt::LightSet* here = lt::findSet("R", 5, true);
            lt::g_cur = here;
            lt::g_curClip = 5;
            strcpy_s(lt::g_curProject, "R");
            lt::addLight(*here, lt::T_POINT, nullptr);        // one of its own already
            int qi = -1;
            for (int i = 0; i < lt::storeCount(); ++i)
            {
                const char* pn = nullptr; int pc = 0, pk = 0;
                if (lt::storeAt(i, &pn, &pc, &pk) && strcmp(pn, "Q") == 0) { qi = i; break; }
            }
            CHECK(qi >= 0, "the picker lists every project and clip in the file");
            const int moved = lt::copyFrom(qi);
            CHECK(moved == 1 && here->count == 2, "another project's rig is copied in, keeping what was here (%d, now %d)", moved, here->count);
            CHECK(here->l[0].id != here->l[1].id, "the copies get their own ids");
            CHECK(lt::copyFrom(-1) == 0 && lt::copyFrom(999) == 0, "a picker out of range copies nothing");
        }
        lt::clearStore();
        lt::g_cur = nullptr;
        lt::g_curClip = -1;
        lt::g_curProject[0] = 0;
    }

    {   // Free Look: the camera of a keyframe is saved and put back, and only it
        static uint8_t fm[mk::SIZE];
        memset(fm, 0, sizeof(fm));
        const float ft = 1234.0f;
        memcpy(fm + mk::OFF_TimeMs, &ft, 4);
        const float fpos[3] = { 10, 20, 30 };
        memcpy(fm + mk::OFF_Pos, fpos, 12);
        const uint64_t fquat = 0x1122334455667788ull;
        memcpy(fm + mk::OFF_Quat, &fquat, 8);
        fm[mk::OFF_Fov] = 45;
        fm[mk::OFF_CamValid] = 1;
        const int32_t fspeed = 7;
        memcpy(fm + mk::OFF_Speed, &fspeed, 4);

        camfree::Snap fs = {};
        CHECK(camfree::grab(fs, fm) && fs.have && fabsf(fs.t - ft) < 0.01f, "free look: a keyframe is snapshot with its time");

        const float flew[3] = { 99, 98, 97 };      // as if it had been flown somewhere else
        memcpy(fm + mk::OFF_Pos, flew, 12);
        fm[mk::OFF_Fov] = 70;
        const int32_t flewSpeed = 3;               // and something that is not the camera changed too
        memcpy(fm + mk::OFF_Speed, &flewSpeed, 4);

        CHECK(camfree::apply(fm, fs), "free look: the snapshot goes back onto the same keyframe");
        float fback[3];
        memcpy(fback, fm + mk::OFF_Pos, 12);
        uint64_t qback;
        memcpy(&qback, fm + mk::OFF_Quat, 8);
        int32_t sback;
        memcpy(&sback, fm + mk::OFF_Speed, 4);
        CHECK(fabsf(fback[0] - 10) < 1e-5f && fabsf(fback[2] - 30) < 1e-5f && fm[mk::OFF_Fov] == 45 && qback == fquat,
              "free look: position, orientation and fov are back (%.1f, %.1f, fov %d)", fback[0], fback[2], fm[mk::OFF_Fov]);
        CHECK(sback == flewSpeed, "free look: only the camera is put back, the rest of the keyframe is left as it is");

        const float otherT = 5000.0f;              // the editor rebuilt its list: another keyframe in that slot
        memcpy(fm + mk::OFF_TimeMs, &otherT, 4);
        memcpy(fm + mk::OFF_Pos, flew, 12);
        CHECK(!camfree::apply(fm, fs), "free look: a keyframe at another time is refused");
        memcpy(fback, fm + mk::OFF_Pos, 12);
        CHECK(fabsf(fback[0] - 99) < 1e-5f, "free look: ... and is left untouched");
    }

    {   // a hook goes in, and its trampoline still reaches the original
        int (* volatile call)(int) = &mhTarget;
        const int before = call(3);
        const MH_STATUS init = MH_Initialize();
        CHECK(init == MH_OK || init == MH_ERROR_ALREADY_INITIALIZED, "MinHook starts up (%s)", MH_StatusToString(init));
        const MH_STATUS cr = MH_CreateHook((void*)&mhTarget, (void*)&mhDetour, (void**)&mhOrig);
        CHECK(cr == MH_OK, "MinHook writes a trampoline for a small function (%s)", MH_StatusToString(cr));
        const MH_STATUS en = MH_EnableHook((void*)&mhTarget);
        CHECK(en == MH_OK, "MinHook enables the hook - the thread snapshot worked (%s)", MH_StatusToString(en));
        if (en == MH_OK)
        {
            CHECK(call(3) == before * 10, "the hook runs and its trampoline reaches the original (%d -> %d)", before, call(3));
            MH_DisableHook((void*)&mhTarget);
            CHECK(call(3) == before, "disabling it puts the original back");
        }
        MH_Uninitialize();
    }

    {   // Standing aside for Rockstar Editor Plus: it needs the game's own first
        // bytes to find its four required addresses, and installs nothing at all
        // without them. Our hooks have to come out and then go back ON TOP.
        int (* volatile call2)(int) = &mhTarget2;
        const int plain = call2(3);
        CHECK(hookFn((uintptr_t)&mhTarget2, (void*)&mhDetour2, (void**)&mhOrig2, "selftest"),
              "a hook goes in through the wrapper and is recorded");
        CHECK(call2(3) == plain * 7, "the detour runs");
        CHECK(unhookAll() >= 1, "unhookAll takes our hooks out");
        CHECK(call2(3) == plain, "the original bytes are back - what another plugin's signature scan needs to see");
        CHECK(rehookAll() >= 1, "rehookAll builds each one again");
        CHECK(call2(3) == plain * 7, "and the detour runs once more, chained onto whatever was there");
        unhookAll();
    }

    {   // Rockstar Editor Plus is recognised however its file has been renamed
        CHECK(!rePlusLoaded(), "no Rockstar Editor Plus in this process to start with");
        char sysdir[MAX_PATH] = {}, tmpdir[MAX_PATH] = {}, src[MAX_PATH] = {}, dst[MAX_PATH] = {};
        GetSystemDirectoryA(sysdir, sizeof(sysdir));
        GetTempPathA(sizeof(tmpdir), tmpdir);
        snprintf(src, sizeof(src), "%s\version.dll", sysdir);
        snprintf(dst, sizeof(dst), "%sRockstarEditorPlus-Custom-selftest.asi", tmpdir);
        if (CopyFileA(src, dst, FALSE))
        {
            HMODULE h = LoadLibraryA(dst);
            if (h)
            {
                CHECK(rePlusLoaded(), "a module whose name merely CONTAINS rockstareditorplus is found");
                FreeLibrary(h);
            }
            else CHECK(false, "could not load the stand-in module for the name test");
            DeleteFileA(dst);
        }
    }

    if (g_fail) printf("SELFTEST: %d FAILURE(S) of %d checks (built " __DATE__ " " __TIME__ ")\n", g_fail, g_ran);
    else        printf("SELFTEST: all %d checks passed (built " __DATE__ " " __TIME__ ")\n", g_ran);
    return g_fail ? 1 : 0;
}
