// Bubbles (multi-monitor) screensaver
// A faithful reimplementation of the Windows Bubbles screensaver renderer that
// respects the real monitor layout instead of treating the desktop as one big box.
//
// Visual model extracted from captures of the Microsoft saver (see NOTES.md):
//   * bubble = soft additive rim glow, peak just inside the silhouette, hard cut at R
//   * one white specular blob at ~10:30, centroid (-0.48R, -0.65R), ~0.28R x 0.22R
//   * per-bubble hue, saturation 0.09..0.63 (mean 0.42), peak value 0.77..1.0
//   * native bubble radius 92 px, drift speed median ~580 px/s, ~7 bubbles per 1080p
#define _WIN32_WINNT 0x0601
#define WINVER 0x0601
#include <windows.h>
#include <mmsystem.h>
#include <d3d9.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <vector>
#include <string>

// ---------------------------------------------------------------- sprite data
static const int SPR      = 200;          // sprite texture size
static const int SPRH     = SPR / 2;      // sprite centre
static const int FILM_N   = 4;            // soap-film marble variants
static const float SPR_R  = 92.0f;        // sprite's own outer radius in texels
static const float BASE_R = 92.0f;        // native bubble radius (px at 100%)
static const float SPEED_LO = 82.0f;      // measured drift speed range, px/s (median 190)
static const float SPEED_HI = 285.0f;
static const float DENSITY  = 1.1e-5f;    // ~23 bubbles per 1920x1080 screen
static const int   SPRITE_RES = 101;      // RCDATA id

static unsigned char *g_rIMask = nullptr; // 200*200 rim intensity
static unsigned char *g_wMask  = nullptr; // 200*200 specular whiteness

// ---------------------------------------------------------------- monitor set
static std::vector<RECT> g_mons;
static RECT g_virt = {0,0,0,0};

// user options (settings page)
static int g_optSize = 1;
static int g_optFilm = 1;
static int g_optMerge = 1;
static int g_optDepth  = 1;
static int g_optWobble = 1;
static int g_optPop    = 1;
static int g_optIri    = 1;   // iridescent rims
static int g_optBloom  = 1;   // soft halo
static int g_optMood   = 1;   // shared palette that drifts
static int g_optBack   = 1;   // faint gradient backdrop
static int g_optDrops  = 1;   // condensation that clings and slides
static int g_optHero   = 1;   // occasional giant bubble
static float g_scale = 1.0f;              // radius scale (preview mode shrinks)

// ---- windowed play mode ------------------------------------------------------
static bool   g_appMode  = false;
static int    g_dragId   = 0;             // bubble being dragged, by id
static float  g_dragOffX = 0.0f, g_dragOffY = 0.0f;
static double g_downT    = 0.0;
static float  g_downX    = 0.0f, g_downY = 0.0f, g_moved = 0.0f;
static float  g_flingVX  = 0.0f, g_flingVY = 0.0f;
static float  g_lastMX   = 0.0f, g_lastMY = 0.0f;
static double g_lastMT   = 0.0;
static const int APP_SETTINGS_ID = 2001, APP_RESET_ID = 2002, APP_EXIT_ID = 2003;

static inline bool inMon(int x, int y)
{
    for (size_t i = 0; i < g_mons.size(); ++i) {
        const RECT &m = g_mons[i];
        if (x >= m.left && x < m.right && y >= m.top && y < m.bottom) return true;
    }
    return false;
}

static long long monArea()
{
    long long a = 0;
    for (size_t i = 0; i < g_mons.size(); ++i)
        a += (long long)(g_mons[i].right - g_mons[i].left) * (g_mons[i].bottom - g_mons[i].top);
    return a;
}

static BOOL CALLBACK monProc(HMONITOR, HDC, LPRECT r, LPARAM lp)
{
    ((std::vector<RECT>*)lp)->push_back(*r);
    return TRUE;
}

static float g_testScale = 1.0f;

static void buildDeadRects();

static void refreshMonitors()
{
    g_mons.clear();
    EnumDisplayMonitors(nullptr, nullptr, monProc, (LPARAM)&g_mons);
    if (g_mons.empty()) {
        RECT r = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        g_mons.push_back(r);
    }
    g_virt = g_mons[0];
    for (size_t i = 1; i < g_mons.size(); ++i) {
        if (g_mons[i].left   < g_virt.left)   g_virt.left   = g_mons[i].left;
        if (g_mons[i].top    < g_virt.top)    g_virt.top    = g_mons[i].top;
        if (g_mons[i].right  > g_virt.right)  g_virt.right  = g_mons[i].right;
        if (g_mons[i].bottom > g_virt.bottom) g_virt.bottom = g_mons[i].bottom;
    }
    buildDeadRects();
}

// ---------------------------------------------------------------- rng
static unsigned int g_seed = 12345u;
static inline float rnd()          // 0..1
{
    g_seed ^= g_seed << 13; g_seed ^= g_seed >> 17; g_seed ^= g_seed << 5;
    return (float)(g_seed & 0x00FFFFFF) / 16777215.0f;
}
static inline float rrange(float a, float b) { return a + (b - a) * rnd(); }

// ---------------------------------------------------------------- bubble
struct Bubble {
    float x, y;
    float vx, vy;
    float r;            // current radius
    float hue;          // 0..1
    float hueRate;      // hue units per second
    float sat, val;
    float age, life;    // life = total lifetime
    float growT;        // grow-in time constant
    float p1, p2, p3;   // noise phases for the drift wander
    float speed;        // base drift speed px/s
    float sp;           // noise phase for speed modulation
    float dx, dy;       // personal drift direction (unit)
    float drift;        // personal drift speed px/s
    float grace;        // seconds left of collision immunity after being emitted
    float depth;        // 0 = far, 1 = near
    float sizeK;        // radius multiplier from depth
    float par;          // how much it answers to flow/puffs: distant things barely move
    float iriSpan;      // how far round the wheel this one's rim runs: 0 = flat, 0.6 = rainbow
    int   filmIdx;      // which soap-film marble pattern
    float filmOffA, filmOffB, filmRate;  // the two film hues, and how they drift
    float mergeT;       // a merged bubble is unstable: it pops on this countdown
    bool  merged;
    bool  merging;      // mid pull-together, on its way to the shared centre
    float mergeU;       // how far through that animation
    float mtx, mty;     // the point it is being drawn towards
    int   mteam;        // which merge event; a pair shares one
    float bloop;        // >0 while the new bubble is still swelling back down
    bool  held;         // being dragged with the mouse
    bool  mabsorb;      // unused now that the pair is shaded as one shape
    float mhue;         // the colour the pair is settling on
    float mr0, mrt;     // radius at merge start, and the fused radius
    int   mlead;        // id of the bubble that owns the shading for the pair
    float wobP, wobR, wobA;  // wobble phase / rate / amplitude
    float sq;           // squash amount 0..1, decays fast
    int   sqAxis;       // 0 = squeezed horizontally (hit a vertical wall)
    bool  hero;         // the occasional giant
    float off;          // palette offset from the shared mood hue
    float die;          // >0 while popping
    int   id;           // identity, survives the swap-remove when pops are replaced
};

// expanding ripple left behind by a pop
struct Ripple { float x, y, r0, t, dur; bool on; };
static Ripple g_rip[6] = {{0,0,0,0,1,false},{0,0,0,0,1,false},{0,0,0,0,1,false},
                          {0,0,0,0,1,false},{0,0,0,0,1,false},{0,0,0,0,1,false}};
static float g_popTimer = 8.0f;

// condensation on the glass: cling, then run down leaving a wet streak
struct Drop { float x, y, y0, r, speed, cling, fade, floorY; int state; bool on; };
static Drop  g_drop[3];
static float g_dropTimer = 5.0f;
static float g_heroTimer = 45.0f;
// one shared palette that drifts between Aero-ish families
// families all the way round the wheel: 0.00 red, 0.08 orange, 0.15 yellow,
// 0.30 green, 0.50 cyan, 0.61 blue, 0.78 violet, 0.90 pink
// the deck is weighted warm: reds, oranges and yellows are half of it, because that
// is what was missing and what she reaches for
static const float MOODS[10] = {0.02f, 0.05f, 0.09f, 0.13f, 0.19f,
                                0.33f, 0.50f, 0.62f, 0.78f, 0.90f};
static const int   MOOD_N = 10;
static float g_moodHue = 0.52f, g_moodTarget = 0.52f, g_moodTimer = 20.0f;
static int   g_moodIdx = 0;
static const int MOOD_ORDER[10] = {0, 5, 1, 6, 2, 7, 3, 8, 4, 9};   // warm, cool, warm, cool
static int   g_moodPos = 0;
static int g_popCount = 0, g_ripCount = 0;
static long long g_layerSkips = 0;
static float g_mergeCool = 7.0f;      // seconds before another merge is allowed
static int   g_merges = 0;
static int   g_mergeTeam = 0;
static double g_elapsed = 0.0;      // seconds since the animation started
static double g_mergeAt[64];        // when each merge animation began
static int    g_mergeAtN = 0;
static int   g_shotBurst = 0;      // /t only: dump the merge animation frame by frame
static int   g_curShot = -1;       // /t only: the last frame dumped
static int   g_mergeShots[64];
static int   g_mergeShotsN = 0;
static float g_mergePX[64], g_mergePY[64];
static int g_nextId = 1;
static const float POP_T = 0.22f;
static const float MERGE_ANIM = 0.38f;   // the pair pulling together
static const float BLOOP_T    = 0.34f;   // the swell when they become one

static void addRipple(float x, float y, float r0)
{
    for (int i = 0; i < 6; ++i) {
        if (!g_rip[i].on) {
            g_rip[i].on = true; g_rip[i].x = x; g_rip[i].y = y; ++g_ripCount;
            g_rip[i].r0 = r0; g_rip[i].t = 0.0f; g_rip[i].dur = 0.55f;
            return;
        }
    }
}

static std::vector<Bubble> g_bub;
static float g_spawnAcc = 0.0f, g_spawnGap = 0.9f;

// ---- slosh model -----------------------------------------------------------
// The real saver does not drive every bubble at once. Individually, or in small
// groups, bubbles get shoved; the rest sit or drift. So: sparse local "puffs"
// with a radial falloff, on top of each bubble's own lazy drift.
struct Puff { float x, y, r, ang, amp, t, dur; bool on; };
static Puff  g_puff[4] = {{0,0,300,0,600,0,1,false},{0,0,300,0,600,0,1,false},
                          {0,0,300,0,600,0,1,false},{0,0,300,0,600,0,1,false}};
static float g_puffTimer = 1.4f;

static void firePuff()
{
    int slot = 0, oldest = 0;
    float bestT = -1.0f;
    for (int i = 0; i < 4; ++i) {
        if (!g_puff[i].on) { slot = i; break; }
        if (g_puff[i].t > bestT) { bestT = g_puff[i].t; oldest = i; }
        slot = i;
    }
    for (int i = 0; i < 4; ++i) if (!g_puff[i].on) { slot = i; break; }
    (void)oldest;

    Puff &p = g_puff[slot];
    p.on = true;
    p.t = 0.0f;
    p.dur = rrange(0.45f, 1.10f);
    p.amp = rrange(520.0f, 1300.0f);
    p.ang = rrange(0.0f, 6.2831853f);
    p.r   = rrange(230.0f, 760.0f);

    if (!g_bub.empty()) {
        // centre it on a random bubble so a real group gets caught
        const Bubble &b = g_bub[(size_t)(rnd() * (float)g_bub.size()) % g_bub.size()];
        p.x = b.x + rrange(-90.0f, 90.0f);
        p.y = b.y + rrange(-90.0f, 90.0f);
    } else {
        const RECT &m = g_mons[(int)(rnd() * g_mons.size()) % g_mons.size()];
        p.x = rrange((float)m.left, (float)m.right);
        p.y = rrange((float)m.top, (float)m.bottom);
    }
}

static void spawnBubble(Bubble &b, bool anywhere)
{
    float R = BASE_R * g_scale;

    // The real saver emits from one corner of the desktop: they shoot out of the
    // bottom-left and are flung up and to the right. On this layout the desktop's
    // bottom-left corner is the foot of the vertical panel.
    float ox = (float)g_virt.left + R + rrange(0.0f, 60.0f);
    float oy = (float)g_virt.bottom - R - rrange(0.0f, 60.0f);

    int mi = 0;
    for (size_t i = 0; i < g_mons.size(); ++i) {
        const RECT &m = g_mons[i];
        if (ox >= m.left && ox < m.right && oy >= m.top && oy < m.bottom) { mi = (int)i; break; }
    }
    const RECT &m = g_mons[mi];
    // up and to the right, fanned out a little
    float aim = atan2f((m.top + m.bottom) * 0.5f - oy, (m.left + m.right) * 0.5f - ox);
    float ang = aim + rrange(-0.62f, 0.62f);
    if (cosf(ang) < 0.05f) ang = aim;                 // never launch backwards

    float along = rrange(0.0f, 150.0f);          // stagger so they never stack
    b.x = ox + cosf(ang) * along;
    b.y = oy + sinf(ang) * along;

    float eject = rrange(430.0f, 1150.0f) * g_scale;  // spat out hard, wide spread
    b.vx = cosf(ang) * eject;
    b.vy = sinf(ang) * eject;

    // personal lazy drift that the slosh decays back into
    float da = rrange(0.0f, 6.2831853f);
    b.dx = cosf(da); b.dy = sinf(da);
    b.drift = rrange(110.0f, 215.0f) * g_scale;
    b.speed = b.drift;

    b.p1 = rrange(0.0f, 6.2831853f);
    b.p2 = rrange(0.0f, 6.2831853f);
    b.p3 = rrange(0.0f, 6.2831853f);
    b.sp = rrange(0.0f, 6.2831853f);

    b.hue     = rnd();
    b.hueRate = (rnd() < 0.5f ? -1.0f : 1.0f) * rrange(0.18f, 0.40f);   // full wheel in ~2.5-5.5 s
    b.sat     = rrange(0.55f, 0.97f);   // mostly chroma, not white: punchy, not pastel
    b.val     = rrange(0.86f, 1.00f);
    b.life    = 1e9f;                                  // bubbles are permanent
    b.growT   = 0.001f;                                // and appear at full size
    b.age     = anywhere ? rrange(1.0f, 8.0f) : 0.0f;
    b.grace   = anywhere ? 0.0f : 0.9f;

    // depth: far ones are smaller, dimmer and slightly slower
    b.depth = g_optDepth ? rnd() : 1.0f;
    if (g_optDepth) b.depth = 0.15f + 0.85f * rnd();
    // size variation is its own thing; depth only trims it so far ones read smaller
    {
        float sz = g_optSize ? rrange(0.62f, 1.38f) : 1.0f;
        if (g_optDepth) sz *= (0.74f + 0.26f * b.depth);
        b.sizeK = sz;
    }
    // and motion parallax: a distant bubble must drift and slosh far less than a near
    // one, which is the cue that actually reads while everything is moving
    b.par = g_optDepth ? (0.26f + 0.74f * b.depth) : 1.0f;
    b.drift *= b.par;
    if (b.depth < 1.0f) b.drift *= (0.80f + 0.30f * b.depth);

    b.wobP = rrange(0.0f, 6.2831853f);
    b.wobR = rrange(0.45f, 1.30f);
    b.wobA = g_optWobble ? rrange(0.015f, 0.035f) : 0.0f;
    b.sq = 0.0f;
    b.sqAxis = 0;
    b.die = 0.0f;
    b.mergeT = 0.0f;
    b.merged = false;
    b.merging = false;
    b.mergeU = 0.0f;
    b.mtx = b.mty = 0.0f;
    b.mteam = 0;
    b.bloop = 0.0f;
    b.held = false;
    b.mabsorb = false;
    b.mhue = 0.0f;
    b.mr0 = b.mrt = 0.0f;
    b.mlead = 0;
    b.off = rrange(-0.10f, 0.10f);
    // not every bubble is equally prismatic: some are nearly one colour, some are
    // showing the whole spectrum round the rim
    b.iriSpan = (rnd() < 0.18f) ? rrange(0.02f, 0.07f)     // some are almost one colour
                               : rrange(0.16f, 0.80f);    // others run most of the wheel
    // soap film: a marble pattern, two neighbouring hues, drifting apart slowly
    b.filmIdx  = (int)(rnd() * (float)FILM_N) % FILM_N;
    b.filmOffA = rrange(-0.20f, -0.06f);
    b.filmOffB = rrange(0.06f, 0.22f);
    b.filmRate = (rnd() < 0.5f ? -1.0f : 1.0f) * rrange(0.010f, 0.030f);
    b.hero = false;
    b.id = g_nextId++;
    b.r = R * b.sizeK;
}

// Dead space = the virtual desktop rectangle minus the real monitors. On this
// layout that is one block above the primary's top-left and one below its
// bottom-left; described as rectangles so the disc can be pushed out of them
// continuously instead of being snapped monitor-by-monitor.
static std::vector<RECT> g_dead;

static void buildDeadRects()
{
    g_dead.clear();
    std::vector<int> ys;
    for (size_t i = 0; i < g_mons.size(); ++i) {
        ys.push_back(g_mons[i].top);
        ys.push_back(g_mons[i].bottom);
    }
    std::sort(ys.begin(), ys.end());
    ys.erase(std::unique(ys.begin(), ys.end()), ys.end());

    std::vector<RECT> bands;
    for (size_t k = 0; k + 1 < ys.size(); ++k) {
        int y0 = ys[k], y1 = ys[k + 1];
        if (y1 <= y0) continue;
        std::vector<std::pair<int, int> > cov;
        for (size_t i = 0; i < g_mons.size(); ++i) {
            const RECT &m = g_mons[i];
            if (m.top <= y0 && m.bottom >= y1) cov.push_back(std::make_pair((int)m.left, (int)m.right));
        }
        std::sort(cov.begin(), cov.end());
        int cur = g_virt.left;
        for (size_t i = 0; i < cov.size(); ++i) {
            if (cov[i].first > cur) { RECT r = {cur, y0, cov[i].first, y1}; bands.push_back(r); }
            if (cov[i].second > cur) cur = cov[i].second;
        }
        if (cur < g_virt.right) { RECT r = {cur, y0, g_virt.right, y1}; bands.push_back(r); }
    }
    for (size_t i = 0; i < bands.size(); ++i) {          // merge vertically adjacent
        bool merged = false;
        for (size_t j = 0; j < g_dead.size(); ++j) {
            if (g_dead[j].left == bands[i].left && g_dead[j].right == bands[i].right &&
                g_dead[j].bottom == bands[i].top) {
                g_dead[j].bottom = bands[i].bottom;
                merged = true;
                break;
            }
        }
        if (!merged) g_dead.push_back(bands[i]);
    }
}

static inline void hsv2rgb(float h, float &r, float &g, float &bl);

// the rim colour for a given hue, with depth dimming and the pop fade applied
static inline void bubbleColour(const Bubble &b, float hue, float &cr, float &cg, float &cb)
{
    float hr, hg, hb;
    hsv2rgb(hue, hr, hg, hb);
    float kA = b.val * (1.0f - b.sat);
    float kB = b.val * b.sat;
    cr = kA + kB * hr; cg = kA + kB * hg; cb = kA + kB * hb;
    if (g_optDepth) {
        // aerial perspective: distance loses contrast and saturation and takes on the
        // colour of the air, which in here is the backdrop. Going pale is wrong.
        float dim = 0.16f + 0.84f * b.depth;
        float hz  = powf(1.0f - b.depth, 1.10f) * 0.90f;
        cr = cr * dim * (1.0f - hz) + 0.050f * hz;
        cg = cg * dim * (1.0f - hz) + 0.120f * hz;
        cb = cb * dim * (1.0f - hz) + 0.160f * hz;
    }
    if (b.die > 0.0f) {
        float u = b.die / POP_T; if (u > 1.0f) u = 1.0f;
        float k = 1.0f - 0.85f * u;
        cr *= k; cg *= k; cb *= k;
    }
}

// visual half-extents: wobble, impact squash, and the pop swell
static inline void bubbleExtents(const Bubble &b, float &ax, float &ay)
{
    float R = b.r;
    float w = b.wobA * sinf(b.wobP);
    ax = R * (1.0f + w);
    ay = R * (1.0f - w);
    if (b.sq > 0.0f) {
        float q = b.sq;
        if (b.sqAxis == 0) { ax *= (1.0f - 0.14f * q); ay *= (1.0f + 0.10f * q); }
        else               { ay *= (1.0f - 0.14f * q); ax *= (1.0f + 0.10f * q); }
    }
    if (b.die > 0.0f) {
        float u = b.die / POP_T; if (u > 1.0f) u = 1.0f;
        float s = 1.0f + 0.32f * u;
        ax *= s; ay *= s;
    }
}

static inline void squashFrom(Bubble &b, int axis, float speed)
{
    if (!g_optWobble) return;
    float m = fabsf(speed) / 620.0f;
    if (m > 0.9f) m = 0.9f;
    if (m > b.sq) { b.sq = m; b.sqAxis = axis; }
}

static void constrain(Bubble &b)
{
    const float r = b.r;

    // centre has to be inside the desktop at all
    if (b.x < g_virt.left + r) { b.x = g_virt.left + r; squashFrom(b, 0, b.vx); b.vx = fabsf(b.vx) * 0.92f; }
    if (b.x > g_virt.right - r) { b.x = g_virt.right - r; squashFrom(b, 0, b.vx); b.vx = -fabsf(b.vx) * 0.92f; }
    if (b.y < g_virt.top + r) { b.y = g_virt.top + r; squashFrom(b, 1, b.vy); b.vy = fabsf(b.vy) * 0.92f; }
    if (b.y > g_virt.bottom - r) { b.y = g_virt.bottom - r; squashFrom(b, 1, b.vy); b.vy = -fabsf(b.vy) * 0.92f; }

    // push the disc out of every dead rectangle by the minimum amount. The push
    // shrinks to zero as the disc just touches, so there is nothing to snap: the
    // bubble slides around a corner instead of being teleported past it.
    for (int pass = 0; pass < 3; ++pass) {
        bool moved = false;
        for (size_t i = 0; i < g_dead.size(); ++i) {
            const RECT &d = g_dead[i];
            float cx = b.x, cy = b.y;
            bool insideDead = (cx > (float)d.left && cx < (float)d.right &&
                               cy > (float)d.top  && cy < (float)d.bottom);
            if (insideDead) {                            // should not happen; recover
                float best = 1e30f, tx = cx, ty = cy;
                for (size_t k = 0; k < g_mons.size(); ++k) {
                    const RECT &m = g_mons[k];
                    float qx = cx < (float)m.left + r ? (float)m.left + r
                             : (cx > (float)m.right - r ? (float)m.right - r : cx);
                    float qy = cy < (float)m.top + r ? (float)m.top + r
                             : (cy > (float)m.bottom - r ? (float)m.bottom - r : cy);
                    float dd = (cx - qx) * (cx - qx) + (cy - qy) * (cy - qy);
                    if (dd < best) { best = dd; tx = qx; ty = qy; }
                }
                b.x = tx; b.y = ty; moved = true;
                continue;
            }
            float px = cx < (float)d.left ? (float)d.left : (cx > (float)d.right ? (float)d.right : cx);
            float py = cy < (float)d.top  ? (float)d.top  : (cy > (float)d.bottom ? (float)d.bottom : cy);
            float dx = cx - px, dy = cy - py;
            float dist = sqrtf(dx * dx + dy * dy);
            if (dist >= r) continue;
            float nx, ny;
            if (dist > 1e-4f) { nx = dx / dist; ny = dy / dist; }
            else { nx = -1.0f; ny = 0.0f; }
            float push = r - dist + 0.02f;
            b.x += nx * push;
            b.y += ny * push;
            float vn = b.vx * nx + b.vy * ny;
            if (vn < 0.0f) {                             // kill the inward component
                const float e = 0.92f;
                b.vx -= (1.0f + e) * vn * nx;
                b.vy -= (1.0f + e) * vn * ny;
                squashFrom(b, (fabsf(nx) >= fabsf(ny)) ? 0 : 1, vn);
            }
            moved = true;
        }
        if (!moved) break;
    }
}



static void update(float dt)
{
    if (dt > 0.1f) dt = 0.1f;
    float R = BASE_R * g_scale;

    // ---- the shared palette drifts to a new family every 18-40 s
    if (g_optMood) {
        g_moodTimer -= dt;
        if (g_moodTimer <= 0.0f) {
            // fixed alternating order, warm then cool then warm: a shuffled deck can
            // deal three cool families in a row, which is exactly what she notices
            g_moodPos = (g_moodPos + 1) % MOOD_N;
            g_moodIdx = MOOD_ORDER[g_moodPos];
            g_moodTarget = MOODS[g_moodIdx];
            g_moodTimer = rrange(11.0f, 24.0f);
        }
        float d = g_moodTarget - g_moodHue;
        while (d >  0.5f) d -= 1.0f;
        while (d < -0.5f) d += 1.0f;
        g_moodHue += d * (1.0f - expf(-dt / 5.0f));
        if (g_moodHue < 0.0f) g_moodHue += 1.0f;
        if (g_moodHue >= 1.0f) g_moodHue -= 1.0f;
    }

    // ---- schedule and advance the local puffs
    g_puffTimer -= dt;
    if (g_puffTimer <= 0.0f) {
        firePuff();
        g_puffTimer = rrange(0.22f, 0.62f);
    }
    for (int i = 0; i < 4; ++i) {
        if (g_puff[i].on) {
            g_puff[i].t += dt;
            if (g_puff[i].t >= g_puff[i].dur) g_puff[i].on = false;
        }
    }

    g_elapsed += dt;
    if (g_mergeCool > 0.0f) g_mergeCool -= dt;

    // how many *merged* bubbles are alive. Counting every bubble over 1.15 would be
    // counting ordinary random sizes, of which there are a dozen, and the gate would
    // never open
    int bigCount = 0;
    for (size_t k = 0; k < g_bub.size(); ++k)
        if (g_bub[k].merged) ++bigCount;

    for (size_t i = 0; i < g_bub.size(); ++i) {
        Bubble &b = g_bub[i];
        if (b.held) { b.vx = 0.0f; b.vy = 0.0f; b.age += dt; continue; }
        b.age += dt;
        // freshly emitted bubbles keep their momentum, then settle into the drift
        const float drag = (b.age < 1.3f) ? 0.30f : 0.70f;

        // local puffs: only bubbles inside a puff's radius feel it, strongest at
        // its centre, plus a little swirl so it does not just push straight
        for (int pi = 0; pi < 4; ++pi) {
            const Puff &p = g_puff[pi];
            if (!p.on) continue;
            float dxp = b.x - p.x, dyp = b.y - p.y;
            float dist = sqrtf(dxp * dxp + dyp * dyp);
            if (dist >= p.r) continue;
            float fall = 1.0f - dist / p.r;          // linear falloff: a group, not one bubble
            float env = sinf(3.14159265f * (p.t / p.dur));
            float ca = cosf(p.ang), sa = sinf(p.ang);
            float push = p.amp * env * fall * dt * b.par;   // distant ones barely respond
            b.vx += ca * push + (-sa) * push * 0.35f;
            b.vy += sa * push + ( ca) * push * 0.35f;
        }

        // the flow: rotate the velocity itself (direction wanders ~30 deg/s,
        // which is what the real one measures) ...
        float wa = sinf(b.age * 0.09f + b.p1) * 0.42f * dt
                 + sinf(b.age * 0.031f + b.p2) * 0.28f * dt;
        float cc = cosf(wa), ss = sinf(wa);
        float nvx = b.vx * cc - b.vy * ss;
        float nvy = b.vx * ss + b.vy * cc;
        b.vx = nvx; b.vy = nvy;
        // ... while its speed relaxes back to this bubble's personal drift
        float sp = sqrtf(b.vx * b.vx + b.vy * b.vy);
        if (sp > 1e-3f) {
            float kk = 1.0f - expf(-drag * dt);
            float scale = 1.0f + (b.drift / sp - 1.0f) * kk;
            b.vx *= scale;
            b.vy *= scale;
        }

        b.x += b.vx * dt;
        b.y += b.vy * dt;

        b.r = R * b.sizeK;
        if (b.hero && b.age > 65.0f && b.die <= 0.0f) {
            b.die = 0.0001f;
            addRipple(b.x, b.y, b.r * 1.15f);
        }
        if (b.grace > 0.0f) b.grace -= dt;
        b.wobP += b.wobR * dt;
        if (b.sq > 0.0f) { b.sq -= b.sq * dt * 9.0f; if (b.sq < 0.002f) b.sq = 0.0f; }
        if (b.merged && b.die <= 0.0f) {
            b.mergeT -= dt;
            if (b.mergeT <= 0.0f) b.die = 0.0001f;      // shed itself, back to normal
        }
        if (b.bloop > 0.0f) b.bloop -= dt;
        if (b.die > 0.0f) b.die += dt;

        if (g_optMood) {
            // a bubble being pulled into a merge has its hue driven by the animation,
            // so the mood must not overwrite it every frame
            if (!b.merging) {
                b.off += b.hueRate * 0.16f * dt;      // wander inside the family
                if (b.off >  0.28f) b.off =  0.28f;
                if (b.off < -0.28f) b.off = -0.28f;
                b.hue = g_moodHue + b.off;
            }
            b.filmOffA += b.filmRate * dt;
            b.filmOffB -= b.filmRate * dt;
            if (b.filmOffA < -0.32f || b.filmOffA > -0.04f) b.filmRate = -b.filmRate;
            if (b.filmOffB >  0.34f || b.filmOffB <  0.04f) b.filmRate = -b.filmRate;
        } else {
            b.hue += b.hueRate * dt;
        }
        if (b.hue < 0.0f) b.hue += 1.0f;
        if (b.hue >= 1.0f) b.hue -= 1.0f;
    }

    // ---- bubble/bubble collisions (equal mass, mildly elastic) -----------------
    for (size_t i = 0; i < g_bub.size(); ++i) {
        Bubble &a = g_bub[i];
        int mergeA = -1, mergeB = -1;
        for (size_t j = i + 1; j < g_bub.size() && mergeA < 0; ++j) {
            Bubble &c = g_bub[j];
            if (a.merging || c.merging) continue;   // already on its way to a weld
            // different depth bands pass through each other: a bubble behind
            // another is behind it, not shoving it around
            if (g_optDepth) {
                float dd = a.depth - c.depth;
                if (dd > 0.34f || dd < -0.34f) { ++g_layerSkips; continue; }
            }
            float dx = c.x - a.x, dy = c.y - a.y;
            float rs = a.r + c.r;
            float d2 = dx * dx + dy * dy;
            if (d2 >= rs * rs || d2 < 1e-4f) continue;
            float d = sqrtf(d2);
            float nx = dx / d, ny = dy / d;
            float overlap = rs - d;
            // collisions ease in over the first moments of a bubble's life so it
            // glides out of the emitter instead of being snapped into place
            float g1 = 1.0f - a.grace / 0.9f; if (g1 < 0) g1 = 0; if (g1 > 1) g1 = 1;
            float g2 = 1.0f - c.grace / 0.9f; if (g2 < 0) g2 = 0; if (g2 > 1) g2 = 1;
            float ge = g1 < g2 ? g1 : g2;
            // a bubble held by the mouse does not move, so all of the separation goes
            // into the other one: dragging shoves the field around
            if (a.held || c.held) {
                Bubble &mv = a.held ? c : a;
                float dbx = a.held ? 1.0f : -1.0f;      // away from the held one
                mv.x += nx * overlap * dbx; mv.y += ny * overlap * dbx;
                float vn0 = (c.vx - a.vx) * nx + (c.vy - a.vy) * ny;
                if (vn0 * dbx < 0.0f) {
                    float imp0 = -(1.0f + 0.35f) * vn0 * dbx;
                    mv.vx += nx * imp0 * dbx; mv.vy += ny * imp0 * dbx;
                }
                continue;
            }
            a.x -= nx * overlap * 0.30f * ge; a.y -= ny * overlap * 0.30f * ge;
            c.x += nx * overlap * 0.30f * ge; c.y += ny * overlap * 0.30f * ge;
            // a low chance to merge, but only on a cooldown, only between settled
            // normal bubbles, and only while the result stays reasonably sized
            if (g_optMerge && g_mergeCool <= 0.0f && mergeA < 0
                && !a.hero && !c.hero && !a.merging && !c.merging
                && a.grace <= 0.0f && c.grace <= 0.0f) {
                float tot = a.r * a.r + c.r * c.r;
                float nr = sqrtf(tot);
                if (nr <= BASE_R * g_scale * 1.42f && bigCount < 3 && rnd() < 0.05f) {
                    mergeA = (int)i; mergeB = (int)j;
                }
            }
            float vn = (c.vx - a.vx) * nx + (c.vy - a.vy) * ny;
            // gentle pressure: bubbles ease away from each other instead of
            // sitting in touching chains (they still collide when they meet)
            float soft = rs * 1.58f;
            if (d < soft) {
                float w = 1.0f - d / soft;
                float push = 150.0f * w * dt * ge;
                a.vx -= nx * push; a.vy -= ny * push;
                c.vx += nx * push; c.vy += ny * push;
            }
            if (vn < 0.0f) {
                const float e = 0.45f;          // soft, they should not ping
                float imp = -(1.0f + e) * vn * 0.5f * ge;
                a.vx -= nx * imp; a.vy -= ny * imp;
                c.vx += nx * imp; c.vy += ny * imp;
            }
        }

        if (mergeA >= 0 && (size_t)mergeB < g_bub.size()) {
            Bubble &A = g_bub[(size_t)mergeA];
            Bubble &B = g_bub[(size_t)mergeB];
            // they do not fuse on the spot: both are told to converge on the same
            // point and are welded together when they get there
            float aA = A.r * A.r, aB = B.r * B.r, tot = aA + aB;
            int team = ++g_mergeTeam;
            float dh = B.hue - A.hue;
            while (dh >  0.5f) dh -= 1.0f;
            while (dh < -0.5f) dh += 1.0f;
            float nh = A.hue + dh * 0.5f;      // the fused colour is the midpoint,
            nh -= floorf(nh);                  // which is what the seam blends to
            A.merging = B.merging = true;
            A.mergeU  = B.mergeU  = 0.0f;
            A.mteam   = B.mteam   = team;
            A.mhue    = B.mhue    = nh;
            A.mr0 = A.r; B.mr0 = B.r;
            A.mrt = B.mrt = sqrtf(tot);        // both grow to the fused radius
            A.mlead = B.mlead = A.id;
            A.mtx = B.mtx = (A.x * aA + B.x * aB) / tot;
            A.mty = B.mty = (A.y * aA + B.y * aB) / tot;
            g_mergeCool = rrange(9.0f, 20.0f);
            g_shotBurst = 48;            // capture the whole animation
            if (g_mergeAtN < 64) {
                g_mergePX[g_mergeAtN] = A.mtx;
                g_mergePY[g_mergeAtN] = A.mty;
                g_mergeAt[g_mergeAtN++] = g_elapsed;
            }
            if (g_mergeShotsN < 64) {
                g_mergePX[g_mergeShotsN] = A.mtx;
                g_mergePY[g_mergeShotsN] = A.mty;
                g_mergeShots[g_mergeShotsN++] = g_curShot;
            }
        }
    }

    // ---- pull the pair together, then weld
    for (size_t i = 0; i < g_bub.size(); ++i) {
        Bubble &b = g_bub[i];
        if (!b.merging) continue;
        if (b.die > 0.0f) { b.merging = false; continue; }   // popped mid-pull
        b.mergeU += dt;
        float k = dt * 8.0f; if (k > 1.0f) k = 1.0f;
        b.x += (b.mtx - b.x) * k;
        b.y += (b.mty - b.y) * k;
        b.vx *= 0.80f;                       // the sloshing stops while they converge
        b.vy *= 0.80f;
        // radius grows late and fast: the join necks, then the whole thing rounds out
        float u = b.mergeU / MERGE_ANIM; if (u > 1.0f) u = 1.0f;
        b.r = b.mr0 + (b.mrt - b.mr0) * (u * u);
        // movement is wobble: the fused shape quivers while it is still moving
        b.r *= 1.0f + 0.025f * sinf(b.mergeU * 22.0f + (float)(b.id & 7) * 0.9f) * (1.0f - u);
        b.sizeK = b.r / (BASE_R * g_scale);
    }

    for (size_t i = 0; i < g_bub.size(); ++i) {
        Bubble &A = g_bub[i];
        if (!A.merging || A.mergeU < MERGE_ANIM) continue;
        int j = -1;
        for (size_t k = 0; k < g_bub.size(); ++k)
            if (k != i && g_bub[k].merging && g_bub[k].mteam == A.mteam) { j = (int)k; break; }
        if (j < 0) { A.merging = false; continue; }          // partner already gone

        Bubble &B = g_bub[(size_t)j];
        float aA = A.r * A.r, aB = B.r * B.r, tot = aA + aB;
        float nr  = sqrtf(tot);                              // area is conserved
        float cx  = (A.x * aA + B.x * aB) / tot;
        float cy  = (A.y * aA + B.y * aB) / tot;
        float dh  = B.hue - A.hue;
        while (dh >  0.5f) dh -= 1.0f;
        while (dh < -0.5f) dh += 1.0f;

        A.r     = nr;
        A.sizeK = nr / (BASE_R * g_scale);
        A.x = cx; A.y = cy;
        A.vx = (A.vx * aA + B.vx * aB) / tot;
        A.vy = (A.vy * aA + B.vy * aB) / tot;
        A.hue = A.mhue;
        if (g_optMood) {                    // keep this colour as the mood drifts on
            float want = A.mhue - g_moodHue;
            while (want >  0.5f) want -= 1.0f;
            while (want < -0.5f) want += 1.0f;
            if (want >  0.28f) want =  0.28f;
            if (want < -0.28f) want = -0.28f;
            A.off = want;
        }
        A.depth = (A.depth + B.depth) * 0.5f;
        A.merging = false;
        A.bloop   = BLOOP_T;                 // the swell, then it settles
        A.sq      = 1.0f;                    // and a squash on top of it
        A.grace   = 0.20f;
        A.merged  = true;
        A.mergeT  = rrange(30.0f, 65.0f);    // unstable: it pops in its own time
        addRipple(cx, cy, nr * 0.60f);

        g_bub[(size_t)j] = g_bub.back();
        g_bub.pop_back();
        ++g_merges;
        break;
    }

    for (size_t i = 0; i < g_bub.size(); ++i) constrain(g_bub[i]);

    // the saver keeps itself at the measured density; the play app does not spawn
    // anything on its own, or a pop would silently be undone a second later
    int target = (int)(DENSITY * (float)monArea() + 0.5f);
    if (target < 1) target = 1;
    if (g_appMode) target = 0;
    g_spawnAcc += dt;
    if ((int)g_bub.size() < target && g_spawnAcc >= g_spawnGap) {
        g_spawnAcc = 0.0f;
        float base = 18.0f / (float)target;
        g_spawnGap = base * rrange(0.7f, 1.35f);
        Bubble b; spawnBubble(b, false);
        g_bub.push_back(b);
    }

    // ---- condensation
    if (g_optDrops) {
        g_dropTimer -= dt;
        if (g_dropTimer <= 0.0f) {
            for (int i = 0; i < 3; ++i) {
                if (g_drop[i].on) continue;
                Drop &d = g_drop[i];
                const RECT &m = g_mons[(size_t)(rnd() * (float)g_mons.size()) % g_mons.size()];
                d.r = rrange(12.0f, 26.0f) * g_scale;
                d.x = rrange((float)m.left + 40.0f, (float)m.right - 40.0f);
                d.y = rrange((float)m.top + (float)(m.bottom - m.top) * 0.12f,
                             (float)m.top + (float)(m.bottom - m.top) * 0.55f);
                d.y0 = d.y;
                d.floorY = (float)m.bottom - 6.0f;
                d.speed = 0.0f;
                d.cling = rrange(2.5f, 8.0f);
                d.fade = 0.0f;
                d.state = 0;
                d.on = true;
                break;
            }
            g_dropTimer = rrange(4.0f, 12.0f);
        }
        for (int i = 0; i < 3; ++i) {
            Drop &d = g_drop[i];
            if (!d.on) continue;
            if (d.state == 0) {
                d.cling -= dt;
                d.r += 1.1f * g_scale * dt;                 // gathering on the glass
                if (d.cling <= 0.0f) { d.state = 1; d.y0 = d.y; }
            } else if (d.state == 1) {
                d.speed += 200.0f * g_scale * dt;
                if (d.speed > 150.0f * g_scale) d.speed = 150.0f * g_scale;
                d.y += d.speed * dt;
                if (d.y + d.r > d.floorY) { d.y = d.floorY - d.r; d.state = 2; d.fade = 2.5f; }
            } else {
                d.fade -= dt;
                if (d.fade <= 0.0f) d.on = false;
            }
        }
    }

    // ---- the occasional giant
    if (g_optHero) {
        g_heroTimer -= dt;
        bool have = false;
        for (size_t i = 0; i < g_bub.size(); ++i) if (g_bub[i].hero) have = true;
        if (!have && g_heroTimer <= 0.0f) {
            Bubble hb;
            spawnBubble(hb, false);
            hb.sizeK = rrange(1.9f, 2.4f);
            hb.r = BASE_R * g_scale * hb.sizeK;
            hb.depth = 1.0f;
            hb.drift = rrange(45.0f, 75.0f) * g_scale;
            hb.par = 1.0f;
            hb.hero = true;
            hb.age = 0.0f;
            hb.grace = 1.8f;
            const RECT &m = g_mons[(size_t)(rnd() * (float)g_mons.size()) % g_mons.size()];
            hb.x = rrange((float)m.left + hb.r, (float)m.right - hb.r);
            hb.y = rrange((float)m.top + hb.r, (float)m.bottom - hb.r);
            g_bub.push_back(hb);
            g_heroTimer = rrange(70.0f, 150.0f);
        }
    }

    // ---- ripples
    for (int i = 0; i < 6; ++i) {
        if (g_rip[i].on) {
            g_rip[i].t += dt;
            if (g_rip[i].t >= g_rip[i].dur) g_rip[i].on = false;
        }
    }

    // ---- the occasional pop: one vanishes with a ripple, a new one flies in
    if (g_optPop) {
        g_popTimer -= dt;
        // in the app an ambient pop must never eat into the handful you started
        // with, or the window quietly empties while you are not looking
        if (g_popTimer <= 0.0f && g_bub.size() > (g_appMode ? 12u : 4u)) {
            int cand = -1;
            for (int tries = 0; tries < 24; ++tries) {
                size_t i = (size_t)(rnd() * (float)g_bub.size()) % g_bub.size();
                if (g_bub[i].die <= 0.0f && g_bub[i].age > 6.0f) { cand = (int)i; break; }
            }
            if (cand >= 0) {
                g_bub[cand].die = 0.0001f;
                ++g_popCount;
                addRipple(g_bub[cand].x, g_bub[cand].y, g_bub[cand].r);
            }
            g_popTimer = rrange(6.0f, 16.0f);
        }
    }

    // ---- finished pops: drop them and send a replacement out of the corner
    for (size_t i = 0; i < g_bub.size();) {
        if (g_bub[i].die >= POP_T) {
            bool wasHero = g_bub[i].hero;
            g_bub[i] = g_bub.back();
            g_bub.pop_back();
            // heroes are not replaced, and in the play app nothing is: what you pop
            // stays popped until you spawn another yourself
            if (!wasHero && !g_appMode) {
                Bubble nb;
                spawnBubble(nb, false);
                g_bub.push_back(nb);
            }
        } else ++i;
    }
}

static void initBubbles(int count, float warm)
{
    for (int i = 0; i < count; ++i) {
        Bubble b; spawnBubble(b, true);
        b.age   = rrange(0.5f, 6.0f);
        b.grace = 2.0f;
        g_bub.push_back(b);
    }
    int steps = (int)(warm * 30.0f);
    for (int i = 0; i < steps; ++i) update(1.0f / 30.0f);
}

// ---------------------------------------------------------------- drawing
struct DIB {
    HDC     dc    = nullptr;
    HBITMAP bmp   = nullptr;
    HGDIOBJ old   = nullptr;
    unsigned int *px = nullptr;   // XRGB 0x00RRGGBB
    int w = 0, h = 0;

    void destroy()
    {
        if (dc) {
            if (old) SelectObject(dc, old);
            if (bmp) DeleteObject(bmp);
            DeleteDC(dc);
        }
        dc = nullptr; bmp = nullptr; old = nullptr; px = nullptr; w = h = 0;
    }

    void create(int w_, int h_)
    {
        destroy();
        w = w_; h = h_;
        BITMAPINFO bi;
        memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = w;
        bi.bmiHeader.biHeight      = -h;          // top-down
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void *bits = nullptr;
        dc  = CreateCompatibleDC(nullptr);
        bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        old = SelectObject(dc, bmp);
        px  = (unsigned int*)bits;
    }
};
static DIB g_dib;

static inline void hsv2rgb(float h, float &r, float &g, float &bl)
{
    float i = floorf(h * 6.0f);
    float f = h * 6.0f - i;
    int ii = (int)i % 6;
    float q = 1.0f - f, t = f;
    switch (ii) {
        case 0: r = 1; g = t; bl = 0; break;
        case 1: r = q; g = 1; bl = 0; break;
        case 2: r = 0; g = 1; bl = t; break;
        case 3: r = 0; g = q; bl = 1; break;
        case 4: r = t; g = 0; bl = 1; break;
        default: r = 1; g = 0; bl = q; break;
    }
}

// ---- sub-pixel-phase sprite atlas: 4x4 quarter-pixel positions at native radius ----
static const int   PHASES = 4;
static const int   AT_R   = (int)BASE_R + 1;      // 93
static const int   AT_SZ  = AT_R * 2;             // 186
static unsigned char *g_atlas = nullptr;          // [PHASES*PHASES][2][AT_SZ*AT_SZ]
static std::vector<RECT> g_dirty;

static inline unsigned char bsample(const unsigned char *m, float u, float v)
{
    int iu = (int)floorf(u), iv = (int)floorf(v);
    float fu = u - iu, fv = v - iv;
    if (iu < 0 || iv < 0 || iu + 1 >= SPR || iv + 1 >= SPR) return 0;
    const unsigned char *q = m + (size_t)iv * SPR + iu;
    float val = (q[0] * (1 - fu) + q[1] * fu) * (1 - fv)
              + (q[SPR] * (1 - fu) + q[SPR + 1] * fu) * fv;
    return (unsigned char)(val + 0.5f);
}

// ---------------------------------------------------------------- sprite extras
// Iridescence: the rim mask split into angular sectors (triangular windows that
// sum back to 1), so each sector can be drawn in its own hue and the rainbow
// runs around the bubble. Bloom: a blurred copy of the rim mask for the halo.
static const int IRI = 8;
static std::vector<unsigned char> g_sec;    // IRI * SPR * SPR
static std::vector<unsigned char> g_blur;   // SPR * SPR

static void buildSectors()
{
    g_sec.assign((size_t)IRI * SPR * SPR, 0);
    const float TAU = 6.2831853f;
    for (int y = 0; y < SPR; ++y) {
        for (int x = 0; x < SPR; ++x) {
            unsigned char r = g_rIMask[(size_t)y * SPR + x];
            if (!r) continue;
            float dx = (float)x + 0.5f - SPRH, dy = (float)y + 0.5f - SPRH;
            float t = (atan2f(dy, dx) + 3.14159265f) / TAU;   // 0..1 around the rim
            float pos = t * IRI;
            for (int k = 0; k < IRI; ++k) {
                float dd = pos - (float)k;
                while (dd >  IRI * 0.5f) dd -= IRI;
                while (dd < -IRI * 0.5f) dd += IRI;
                float w = 1.0f - fabsf(dd);
                if (w <= 0.0f) continue;
                g_sec[(size_t)k * SPR * SPR + (size_t)y * SPR + x] = (unsigned char)(r * w + 0.5f);
            }
        }
    }
}

static std::vector<unsigned char> g_cutRim, g_cutBlur;   // alpha-boosted for occlusion
static std::vector<unsigned char> g_soft;               // heavy blur, for far bubbles

// ---- soap film: smooth value noise, warped, masked to the body of the bubble
static std::vector<unsigned char> g_filmA[FILM_N], g_filmB[FILM_N];

static float vnoise(float x, float y, int seed)
{
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float xf = x - xi, yf = y - yi;
    #define NH(a, b) ({ unsigned n = (unsigned)((a) * 374761393 + (b) * 668265263 + seed * 1442695041u); \
                        n = (n ^ (n >> 13)) * 1274126177u; \
                        (float)((n ^ (n >> 16)) & 0xFFFFFFu) / 16777215.0f; })
    float h00 = NH(xi, yi), h10 = NH(xi + 1, yi), h01 = NH(xi, yi + 1), h11 = NH(xi + 1, yi + 1);
    #undef NH
    float u = xf * xf * xf * (xf * (xf * 6 - 15) + 10);
    float v = yf * yf * yf * (yf * (yf * 6 - 15) + 10);
    return (h00 * (1 - u) + h10 * u) * (1 - v) + (h01 * (1 - u) + h11 * u) * v;
}

static void buildFilms()
{
    for (int f = 0; f < FILM_N; ++f) {
        g_filmA[f].assign((size_t)SPR * SPR, 0);
        g_filmB[f].assign((size_t)SPR * SPR, 0);
        for (int y = 0; y < SPR; ++y) {
            for (int x = 0; x < SPR; ++x) {
                float u = ((float)x + 0.5f - SPRH) / SPRH;
                float v = ((float)y + 0.5f - SPRH) / SPRH;
                float rr = sqrtf(u * u + v * v);
                // full across the body, gone before the rim so it never fights it
                float body = 1.0f - (rr - 0.52f) / 0.44f;
                if (body < 0.0f) body = 0.0f;
                if (body > 1.0f) body = 1.0f;
                body = body * body * (3.0f - 2.0f * body);
                // two warped noise fields: one is the other's negative, so the
                // colours marble into each other instead of stacking up
                // both layers carry a floor so the body always glows faintly and the
                // swirls only vary how much: bright-here/dark-there reads as dirt
                float a = vnoise(u * 3.4f + v * 1.2f + f * 3.0f, v * 3.4f - u * 1.2f, f * 7 + 1);
                float b = vnoise(u * 4.3f - v * 1.4f, v * 4.3f + u * 1.4f + f * 5.0f, f * 13 + 7);
                a = 0.30f + 0.70f * a;
                b = 0.30f + 0.70f * b;
                g_filmA[f][(size_t)y * SPR + x] = (unsigned char)(a * body * 255.0f);
                g_filmB[f][(size_t)y * SPR + x] = (unsigned char)(b * body * 255.0f);
            }
        }
    }
}

static void buildCutMasks()
{
    const float K = 1.55f;
    g_cutRim.assign(g_rIMask, g_rIMask + (size_t)SPR * SPR);
    for (size_t i = 0; i < g_cutRim.size(); ++i) {
        float v = g_cutRim[i] * K;
        g_cutRim[i] = (unsigned char)(v > 255.0f ? 255.0f : v);
    }
    g_cutBlur.assign(g_blur.begin(), g_blur.end());
    for (size_t i = 0; i < g_cutBlur.size(); ++i) {
        float v = g_cutBlur[i] * K;
        g_cutBlur[i] = (unsigned char)(v > 255.0f ? 255.0f : v);
    }
}

static std::vector<unsigned char> g_bead;    // soft centred blob

static void buildBead()
{
    g_bead.assign((size_t)SPR * SPR, 0);
    for (int y = 0; y < SPR; ++y) {
        for (int x = 0; x < SPR; ++x) {
            float dx = ((float)x + 0.5f - SPRH) / SPRH;
            float dy = ((float)y + 0.5f - SPRH) / SPRH;
            float v = expf(-(dx * dx + dy * dy) * 3.4f) - expf(-3.4f);
            if (v < 0.0f) v = 0.0f;
            v *= 290.0f;
            g_bead[(size_t)y * SPR + x] = (unsigned char)(v > 255.0f ? 255.0f : v);
        }
    }
}

static void makeBlur(const unsigned char *src, int R, float stretch,
                     std::vector<unsigned char> &out)
{
    const int N = SPR * SPR;
    const float SCALE = 0.80f;          // halo must die before the texture edge
    std::vector<float> a(N, 0.0f), b(N, 0.0f);
    for (int y = 0; y < SPR; ++y) {
        for (int x = 0; x < SPR; ++x) {
            float sx = ((float)x - SPRH) / SCALE + SPRH;
            float sy = ((float)y - SPRH) / SCALE + SPRH;
            int x0 = (int)floorf(sx), y0 = (int)floorf(sy);
            float fx = sx - x0, fy = sy - y0;
            if (x0 < 0 || y0 < 0 || x0 + 1 >= SPR || y0 + 1 >= SPR) continue;
            const unsigned char *p0 = src + (size_t)y0 * SPR + x0;
            float v = (p0[0] * (1 - fx) + p0[1] * fx) * (1 - fy)
                    + (p0[SPR] * (1 - fx) + p0[SPR + 1] * fx) * fy;
            a[(size_t)y * SPR + x] = v;
        }
    }
    for (int pass = 0; pass < 2; ++pass) {
        for (int y = 0; y < SPR; ++y) {                 // horizontal
            float sum = 0.0f;
            for (int x = -R; x <= R; ++x) sum += a[(size_t)y * SPR + (x < 0 ? 0 : (x > SPR - 1 ? SPR - 1 : x))];
            for (int x = 0; x < SPR; ++x) {
                b[(size_t)y * SPR + x] = sum / (float)(2 * R + 1);
                int xo = x - R, xi = x + R + 1;
                sum -= a[(size_t)y * SPR + (xo < 0 ? 0 : xo)];
                sum += a[(size_t)y * SPR + (xi > SPR - 1 ? SPR - 1 : xi)];
            }
        }
        a.swap(b);
        for (int x = 0; x < SPR; ++x) {                 // vertical
            float sum = 0.0f;
            for (int y = -R; y <= R; ++y) sum += a[(size_t)(y < 0 ? 0 : (y > SPR - 1 ? SPR - 1 : y)) * SPR + x];
            for (int y = 0; y < SPR; ++y) {
                b[(size_t)y * SPR + x] = sum / (float)(2 * R + 1);
                int yo = y - R, yi = y + R + 1;
                sum -= a[(size_t)(yo < 0 ? 0 : yo) * SPR + x];
                sum += a[(size_t)(yi > SPR - 1 ? SPR - 1 : yi) * SPR + x];
            }
        }
        a.swap(b);
    }
    // a box blur leaves it dim; stretch it back to a usable glow
    out.assign(N, 0);
    for (int i = 0; i < N; ++i) {
        float v = a[i] * stretch;
        out[i] = (unsigned char)(v > 255.0f ? 255.0f : v);
    }
}

static void buildBloomMask() { makeBlur(g_rIMask, 9, 3.4f, g_blur); }
static void buildSoftMask()  { makeBlur(g_rIMask, 12, 4.2f, g_soft); }

static void buildAtlas()
{
    if (!g_atlas) g_atlas = (unsigned char*)malloc((size_t)PHASES * PHASES * 2 * AT_SZ * AT_SZ);
    for (int py = 0; py < PHASES; ++py) {
        for (int px = 0; px < PHASES; ++px) {
            unsigned char *base = g_atlas + ((size_t)(py * PHASES + px) * 2 * AT_SZ * AT_SZ);
            unsigned char *dI = base, *dW = base + AT_SZ * AT_SZ;
            float ox = 0.5f - px * 0.25f;
            float oy = 0.5f - py * 0.25f;
            for (int v = 0; v < AT_SZ; ++v) {
                for (int u = 0; u < AT_SZ; ++u) {
                    float su = (float)(u - AT_R) + ox + SPRH;
                    float sv = (float)(v - AT_R) + oy + SPRH;
                    dI[v * AT_SZ + u] = bsample(g_rIMask, su, sv);
                    dW[v * AT_SZ + u] = bsample(g_wMask,  su, sv);
                }
            }
        }
    }
}

static std::vector<unsigned int> g_bg;      // gradient the surface returns to

static void buildBackdrop(int w, int h)
{
    g_bg.assign((size_t)w * h, 0xFF000000u);
    for (int y = 0; y < h; ++y) {
        float t = (h > 1) ? (float)y / (float)(h - 1) : 0.0f;
        int r = (int)(1.0f + 3.0f * t);
        int g = (int)(6.0f + 22.0f * t);
        int bb = (int)(10.0f + 28.0f * t);
        unsigned int c = 0xFF000000u | ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)bb;
        for (int x = 0; x < w; ++x) g_bg[(size_t)y * w + x] = c;
    }
}

static void fillSurface()
{
    size_t n = (size_t)g_dib.w * g_dib.h;
    if (g_optBack && g_bg.size() == n) memcpy(g_dib.px, g_bg.data(), n * sizeof(unsigned int));
    else memset(g_dib.px, 0, n * sizeof(unsigned int));
}

static void clearRect(const RECT &r)
{
    RECT c = r;
    if (c.left   < g_virt.left)   c.left   = g_virt.left;
    if (c.top    < g_virt.top)    c.top    = g_virt.top;
    if (c.right  > g_virt.right)  c.right  = g_virt.right;
    if (c.bottom > g_virt.bottom) c.bottom = g_virt.bottom;
    if (c.right <= c.left || c.bottom <= c.top) return;
    size_t bytes = (size_t)(c.right - c.left) * 4;
    if (g_optBack && g_bg.size() == (size_t)g_dib.w * g_dib.h) {
        for (int y = c.top; y < c.bottom; ++y) {
            size_t off = (size_t)(y - g_virt.top) * g_dib.w + (c.left - g_virt.left);
            memcpy(g_dib.px + off, &g_bg[off], bytes);
        }
    } else {
        for (int y = c.top; y < c.bottom; ++y)
            memset(g_dib.px + (size_t)(y - g_virt.top) * g_dib.w + (c.left - g_virt.left), 0, bytes);
    }
}

// one sprite at an arbitrary elliptical size, bilinear from a mask.
// maskW may be null when the white specular should not be added again.
static void blitMask(const unsigned char *maskI, const unsigned char *maskW,
                     float cx, float cy, float ax, float ay, float cr, float cg, float cb,
                     float wGain = 255.0f, bool gdiCut = false)
{
    if (ax < 0.7f) ax = 0.7f;
    if (ay < 0.7f) ay = 0.7f;
    if (!maskI && !maskW) return;
    int x0 = (int)floorf(cx - ax) - 1, x1 = (int)ceilf(cx + ax) + 1;
    int y0 = (int)floorf(cy - ay) - 1, y1 = (int)ceilf(cy + ay) + 1;
    RECT dr = {x0, y0, x1, y1};
    g_dirty.push_back(dr);
    if (x0 < g_virt.left)   x0 = g_virt.left;
    if (y0 < g_virt.top)    y0 = g_virt.top;
    if (x1 > g_virt.right)  x1 = g_virt.right;
    if (y1 > g_virt.bottom) y1 = g_virt.bottom;
    if (x1 <= x0 || y1 <= y0) return;

    const float invX = SPR_R / ax, invY = SPR_R / ay;
    const float inv255 = 1.0f / 255.0f;

    for (int y = y0; y < y1; ++y) {
        unsigned int *dst = g_dib.px + (size_t)(y - g_virt.top) * g_dib.w + (x0 - g_virt.left);
        float v = ((float)y + 0.5f - cy) * invY + SPRH;
        int   iv = (int)floorf(v); float fv = v - iv;
        for (int x = x0; x < x1; ++x, ++dst) {
            float u = ((float)x + 0.5f - cx) * invX + SPRH;
            int   iu = (int)floorf(u); float fu = u - iu;
            float i00, i10, i01, i11, w00 = 0, w10 = 0, w01 = 0, w11 = 0;
            if (iu >= 0 && iv >= 0 && iu + 1 < SPR && iv + 1 < SPR) {
                if (maskI) {
                    const unsigned char *pI = maskI + (size_t)iv * SPR + iu;
                    i00 = pI[0];   i10 = pI[1];
                    i01 = pI[SPR]; i11 = pI[SPR + 1];
                } else { i00 = i10 = i01 = i11 = 0; }
                if (maskW) {
                    const unsigned char *pW = maskW + (size_t)iv * SPR + iu;
                    w00 = pW[0];   w10 = pW[1];
                    w01 = pW[SPR]; w11 = pW[SPR + 1];
                }
            } else {
                i00 = i10 = i01 = i11 = 0;
            }
            float ii = (i00 * (1 - fu) + i10 * fu) * (1 - fv)
                     + (i01 * (1 - fu) + i11 * fu) * fv;
            float ww = (w00 * (1 - fu) + w10 * fu) * (1 - fv)
                     + (w01 * (1 - fu) + w11 * fu) * fv;
            if (ii == 0.0f && ww == 0.0f) continue;
            ii *= inv255; ww *= inv255;
            unsigned int d = *dst;
            int dr2, dg2, db2;
            if (gdiCut) {                          // occlusion: scale the destination down
                float cov = ii * 1.55f;
                if (cov > 1.0f) cov = 1.0f;
                float k = 1.0f - cov;
                dr2 = (int)(((d >> 16) & 0xFF) * k);
                dg2 = (int)(((d >> 8)  & 0xFF) * k);
                db2 = (int)(( d        & 0xFF) * k);
            } else {                               // additive, matching the D3D path
                dr2 = (int)((d >> 16) & 0xFF) + (int)(ii * cr + ww * wGain);
                dg2 = (int)((d >> 8)  & 0xFF) + (int)(ii * cg + ww * wGain);
                db2 = (int)( d        & 0xFF) + (int)(ii * cb + ww * wGain);
            }
            if (dr2 > 255) dr2 = 255;
            if (dg2 > 255) dg2 = 255;
            if (db2 > 255) db2 = 255;
            *dst = 0xFF000000u | ((unsigned)dr2 << 16) | ((unsigned)dg2 << 8) | (unsigned)db2;
        }
    }
}

// radial brightness profile of the rim mask, sampled once. The fused shape is shaded
// with this so a merged pair looks like the same glass as every other bubble.
static float g_prof[SPRH + 2];
static void buildProfile()
{
    for (int r = 0; r <= SPRH + 1; ++r) g_prof[r] = 0.0f;
    const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
    for (int r = 0; r <= SPRH; ++r) {
        float acc = 0.0f; int n = 0;
        for (int k = 0; k < 4; ++k) {
            int x = SPRH + dx[k] * r, y = SPRH + dy[k] * r;
            if (x >= 0 && x < SPR && y >= 0 && y < SPR) {
                acc += g_rIMask[(size_t)y * SPR + x]; ++n;
            }
        }
        g_prof[r] = n ? acc / (float)n / 255.0f : 0.0f;
    }
}

// shade two circles as one fused body: smooth-union of their signed distances, then
// the same rim profile, with the colour blending across the join
static void shadeMergePair(const Bubble &A, const Bubble &B,
                           std::vector<unsigned int> &out, int &ox, int &oy, int &ow, int &oh)
{
    ox = oy = 0; ow = oh = 0; out.clear();

    int x0 = (int)floorf(fminf(A.x - A.r, B.x - B.r));
    int x1 = (int)ceilf (fmaxf(A.x + A.r, B.x + B.r));
    int y0 = (int)floorf(fminf(A.y - A.r, B.y - B.r));
    int y1 = (int)ceilf (fmaxf(A.y + A.r, B.y + B.r));
    if (x0 < g_virt.left) x0 = g_virt.left;
    if (y0 < g_virt.top)  y0 = g_virt.top;
    if (x1 > g_virt.right)  x1 = g_virt.right;
    if (y1 > g_virt.bottom) y1 = g_virt.bottom;
    int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0) return;

    // colours either side of the join, and a small LUT so the seam blends smoothly
    const int LUT = 32;
    unsigned int lut[LUT + 1];
    {
        float d = B.hue - A.hue;
        while (d >  0.5f) d -= 1.0f;
        while (d < -0.5f) d += 1.0f;
        for (int i = 0; i <= LUT; ++i) {
            float t = (float)i / (float)LUT;
            float hh = A.hue + d * t; hh -= floorf(hh);
            float ss = A.sat + (B.sat - A.sat) * t;
            float vv = A.val + (B.val - A.val) * t;
            float hr, hg, hb; hsv2rgb(hh, hr, hg, hb);
            float kA = vv * (1.0f - ss), kB = vv * ss;      // the same maths as bubbleColour
            float cr = kA + kB * hr, cg = kA + kB * hg, cb = kA + kB * hb;
            if (cr > 1.0f) cr = 1.0f; if (cg > 1.0f) cg = 1.0f; if (cb > 1.0f) cb = 1.0f;
            lut[i] = ((unsigned)(cr * 255.0f) << 16) | ((unsigned)(cg * 255.0f) << 8) | (unsigned)(cb * 255.0f);
        }
    }

    const float rAvg = (A.r + B.r) * 0.5f;
    const float scale = SPR_R / (rAvg > 1.0f ? rAvg : 1.0f);   // screen px -> mask index
    const float k = 0.16f * rAvg;                              // size of the fillet at the join
    const float k2 = fmaxf(k, 4.0f);

    out.assign((size_t)w * h, 0u);
    for (int y = 0; y < h; ++y) {
        float py = (float)(y0 + y) + 0.5f;
        unsigned int *row = &out[(size_t)y * w];
        for (int x = 0; x < w; ++x) {
            float px = (float)(x0 + x) + 0.5f;
            float ax = px - A.x, ay = py - A.y;
            float bx = px - B.x, by = py - B.y;
            float da2 = ax * ax + ay * ay;
            float db2 = bx * bx + by * by;
            // outside both circles the profile is zero, so this is the cheap reject
            if (da2 > A.r * A.r && db2 > B.r * B.r) continue;
            float da = sqrtf(da2), db = sqrtf(db2);
            float sda = da - A.r, sdb = db - B.r;
            // polynomial smooth min: the join gets a fillet instead of a cusp
            float hh = 0.5f + 0.5f * (sdb - sda) / k;
            if (hh < 0.0f) hh = 0.0f;
            if (hh > 1.0f) hh = 1.0f;
            float g = sdb + (sda - sdb) * hh - k * hh * (1.0f - hh);
            float delta = -g;                      // how far inside the fused boundary
            if (delta <= 0.0f) continue;
            float rLocal = (fabsf(sda) < fabsf(sdb)) ? A.r : B.r;
            float idx = SPR_R - delta * (SPR_R / (rLocal > 1.0f ? rLocal : 1.0f));
            if (idx < 0.0f || idx > (float)SPRH) continue;
            float prof = g_prof[(int)(idx + 0.5f)];
            if (prof <= 0.0f) continue;
            float wB = 0.5f + 0.5f * (sda - sdb) / k2;
            if (wB < 0.0f) wB = 0.0f;
            if (wB > 1.0f) wB = 1.0f;
            unsigned int c = lut[(int)(wB * LUT + 0.5f)];
            int cr = (int)((float)((c >> 16) & 0xFF) * prof);
            int cg = (int)((float)((c >>  8) & 0xFF) * prof);
            int cb = (int)((float)( c        & 0xFF) * prof);
            row[x] = ((unsigned)cr << 16) | ((unsigned)cg << 8) | (unsigned)cb;
        }
    }
    ox = x0; oy = y0; ow = w; oh = h;
}

static void drawBubbles()
{
    // erase only what was drawn last frame, then redraw: no full-screen clear
    for (size_t i = 0; i < g_dirty.size(); ++i) clearRect(g_dirty[i]);
    g_dirty.clear();

    static const float WGAIN = 255.0f;
    unsigned char lutR[256], lutG[256], lutB[256];
    const float IRI_SPAN = 0.30f;

    std::vector<int> ord(g_bub.size());
    for (size_t i = 0; i < ord.size(); ++i) ord[i] = (int)i;
    if (g_optDepth && ord.size() > 1)
        std::sort(ord.begin(), ord.end(),
                  [](int a, int c) { return g_bub[(size_t)a].depth < g_bub[(size_t)c].depth; });

    for (size_t oi = 0; oi < ord.size(); ++oi) {
        const Bubble &b = g_bub[(size_t)ord[oi]];
        if (b.merging) {
            if (b.id != b.mlead) continue;            // the partner is drawn by the lead
            const Bubble *pr = nullptr;
            for (size_t q = 0; q < g_bub.size(); ++q)
                if (g_bub[q].merging && g_bub[q].mteam == b.mteam && g_bub[q].id != b.id) { pr = &g_bub[q]; break; }
            if (!pr) continue;      // the update pass clears a partnerless merge
            std::vector<unsigned int> mb; int mx, my, mw, mh;
            shadeMergePair(b, *pr, mb, mx, my, mw, mh);
            for (int yy = 0; yy < mh; ++yy) {
                unsigned int *dst = g_dib.px + (size_t)(my - g_virt.top + yy) * g_dib.w + (mx - g_virt.left);
                const unsigned int *src = &mb[(size_t)yy * mw];
                for (int xx = 0; xx < mw; ++xx) {
                    unsigned int s = src[xx];
                    if (!s) continue;
                    unsigned int d = dst[xx];
                    int rr = (int)((d >> 16) & 0xFF) + (int)((s >> 16) & 0xFF);
                    int gg = (int)((d >>  8) & 0xFF) + (int)((s >>  8) & 0xFF);
                    int bb2 = (int)(d & 0xFF) + (int)(s & 0xFF);
                    if (rr > 255) rr = 255; if (gg > 255) gg = 255; if (bb2 > 255) bb2 = 255;
                    dst[xx] = 0xFF000000u | ((unsigned)rr << 16) | ((unsigned)gg << 8) | (unsigned)bb2;
                }
            }
            continue;
        }

        float ax, ay;
        bubbleExtents(b, ax, ay);
        if (b.merging) {                     // one swells, the other is swallowed
            float u = b.mergeU / MERGE_ANIM; if (u > 1.0f) u = 1.0f;
            float sw = b.mabsorb ? (1.0f - 0.96f * u * u) : (1.0f + 0.16f * u * u);
            ax *= sw; ay *= sw;
        }
        if (b.bloop > 0.0f) {                // and the bloop when it becomes one
            float u = 1.0f - b.bloop / BLOOP_T;
            ax *= 1.0f + 0.20f * sinf(3.14159265f * u);
            ay *= 1.0f + 0.20f * sinf(3.14159265f * u);
        }
        if (ax < 0.7f) continue;

        // ---- background layer: faint, flat, no halo, no highlight, and nothing
        // behind it so it occludes nothing. Depth here comes from size, parallax and
        // haze; blur was never the cue that made it read.
        bool tiny = (ax < BASE_R * 0.55f);      // preview pane: needs the blurred mask
        bool bg   = (g_optDepth && b.depth < 0.45f);   // 'far' is a macro in windows.h
        if (bg) {
            float cr, cg, cb; bubbleColour(b, b.hue, cr, cg, cb);
            blitMask(g_rIMask, nullptr, b.x, b.y, ax, ay, cr * 255.0f, cg * 255.0f, cb * 255.0f);
            blitMask(nullptr, g_wMask, b.x, b.y, ax, ay, 0.0f, 0.0f, 0.0f, 68.0f);
            continue;
        }
        if (tiny) {
            float cr, cg, cb; bubbleColour(b, b.hue, cr, cg, cb);
            blitMask(g_soft.data(), nullptr, b.x, b.y, ax * 1.08f, ay * 1.08f,
                     cr * 255.0f, cg * 255.0f, cb * 255.0f);
            continue;
        }

        // ---- occlusion: a nearer bubble erases what is behind it
        blitMask(g_rIMask, nullptr, b.x, b.y, ax, ay, 0.0f, 0.0f, 0.0f, 0.0f, true);

        // ---- soap film: the marbled body of the bubble, behind its rim
        if (g_optFilm) {
            float fr, fg, fb;
            float ha = b.hue + b.filmOffA; ha -= floorf(ha);
            float hb = b.hue + b.filmOffB; hb -= floorf(hb);
            bubbleColour(b, ha, fr, fg, fb);
            blitMask(g_filmA[b.filmIdx].data(), nullptr, b.x, b.y, ax, ay,
                     fr * 40.0f, fg * 40.0f, fb * 40.0f);
            bubbleColour(b, hb, fr, fg, fb);
            blitMask(g_filmB[b.filmIdx].data(), nullptr, b.x, b.y, ax, ay,
                     fr * 34.0f, fg * 34.0f, fb * 34.0f);
        }

        // ---- bloom: the blurred mask, dim, just behind
        if (g_optBloom) {
            float cr, cg, cb; bubbleColour(b, b.hue, cr, cg, cb);
            // a small bubble must not squeeze the same glow into a tighter ring
            float gk = b.r / (BASE_R * g_scale);
            if (gk < 0.40f) gk = 0.40f;
            if (gk > 1.70f) gk = 1.70f;
            float k = 0.32f * gk * 255.0f;
            blitMask(g_blur.data(), nullptr, b.x, b.y, ax * 1.25f, ay * 1.25f, cr * k, cg * k, cb * k);
        }

        if (g_optIri) {
            // one hue band per sector, walking around the rim
            float phase = b.wobP * 0.15915494f;
            for (int k = 0; k < IRI; ++k) {
                float hh = b.hue + b.iriSpan * (((float)k + 0.5f) / (float)IRI - 0.5f) + phase;
                hh -= floorf(hh);
                float cr, cg, cb; bubbleColour(b, hh, cr, cg, cb);
                blitMask(g_sec.data() + (size_t)k * SPR * SPR, nullptr,
                         b.x, b.y, ax, ay, cr * 255.0f, cg * 255.0f, cb * 255.0f);
            }
            // the highlight is white and belongs on top, once
            {
                float dim = g_optDepth ? (0.12f + 0.88f * b.depth) : 1.0f;
                if (b.die > 0.0f) {
                    float u = b.die / POP_T; if (u > 1.0f) u = 1.0f;
                    dim *= 1.0f - 0.85f * u;
                }
                blitMask(nullptr, g_wMask, b.x, b.y, ax, ay, 0.0f, 0.0f, 0.0f);
                (void)dim;
            }
            continue;
        }

        float cr, cg, cb0;
        bubbleColour(b, b.hue, cr, cg, cb0);
        cr *= 255.0f; cg *= 255.0f; cb0 *= 255.0f;

        // ---- fast path: native radius, no bloom, pre-rendered atlas
        if (!g_optBloom && fabsf(ax - BASE_R) < 0.35f && fabsf(ay - BASE_R) < 0.35f) {
            for (int i = 0; i < 256; ++i) {
                lutR[i] = (unsigned char)(cr * i / 255.0f + 0.5f);
                lutG[i] = (unsigned char)(cg * i / 255.0f + 0.5f);
                lutB[i] = (unsigned char)(cb0 * i / 255.0f + 0.5f);
            }
            int ix = (int)lroundf(b.x * 4.0f);
            int iy = (int)lroundf(b.y * 4.0f);
            int px = ix & 3, py = iy & 3;
            int bx = (ix - px) / 4, by = (iy - py) / 4;
            int left = bx - AT_R, top = by - AT_R;

            RECT drc = {left, top, left + AT_SZ, top + AT_SZ};
            g_dirty.push_back(drc);

            int xs = left, xe = left + AT_SZ, ys = top, ye = top + AT_SZ;
            if (xs < g_virt.left)   xs = g_virt.left;
            if (ys < g_virt.top)    ys = g_virt.top;
            if (xe > g_virt.right)  xe = g_virt.right;
            if (ye > g_virt.bottom) ye = g_virt.bottom;

            const unsigned char *abase = g_atlas + ((size_t)(py * PHASES + px) * 2 * AT_SZ * AT_SZ);
            const unsigned char *sI = abase, *sW = abase + AT_SZ * AT_SZ;

            for (int y = ys; y < ye; ++y) {
                unsigned int *dst = g_dib.px + (size_t)(y - g_virt.top) * g_dib.w + (xs - g_virt.left);
                const unsigned char *ri = sI + (size_t)(y - top) * AT_SZ + (xs - left);
                const unsigned char *rw = sW + (size_t)(y - top) * AT_SZ + (xs - left);
                for (int x = xs; x < xe; ++x, ++dst, ++ri, ++rw) {
                    unsigned int a = *ri, w2 = *rw;
                    if (!a && !w2) continue;
                    int dr2 = lutR[a] + (int)w2;
                    int dg2 = lutG[a] + (int)w2;
                    int db2 = lutB[a] + (int)w2;
                    unsigned int d = *dst;
                    int r = (int)((d >> 16) & 0xFF) + dr2; if (r > 255) r = 255;
                    int g = (int)((d >> 8)  & 0xFF) + dg2; if (g > 255) g = 255;
                    int bl = (int)(d & 0xFF) + db2;        if (bl > 255) bl = 255;
                    *dst = 0xFF000000u | ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)bl;
                }
            }
            continue;
        }

        blitMask(g_rIMask, g_wMask, b.x, b.y, ax, ay, cr, cg, cb0);
    }

    // ---- condensation, on the glass so in front
    if (g_optDrops) {
        for (int i = 0; i < 3; ++i) {
            const Drop &d = g_drop[i];
            if (!d.on) continue;
            float a = (d.state == 2) ? (d.fade / 2.5f) : 1.0f;
            if (d.y - d.y0 > 3.0f) {                       // the wet streak behind it
                float hy = (d.y - d.y0) * 0.5f + d.r * 0.4f;
                float cy = (d.y + d.y0) * 0.5f;
                blitMask(g_blur.data(), nullptr, d.x, cy, d.r * 0.42f, hy,
                         54.0f * a, 64.0f * a, 78.0f * a);
            }
            blitMask(g_bead.data(), nullptr, d.x, d.y, d.r * 1.15f, d.r * 1.15f,
                     44.0f * a, 56.0f * a, 70.0f * a);
            blitMask(g_bead.data(), nullptr, d.x - d.r * 0.30f, d.y - d.r * 0.30f,
                     d.r * 0.42f, d.r * 0.42f, 150.0f * a, 165.0f * a, 180.0f * a);
        }
    }

    // ---- pop ripples on top
    for (int i = 0; i < 6; ++i) {
        const Ripple &r = g_rip[i];
        if (!r.on) continue;
        float u = r.t / r.dur; if (u > 1.0f) u = 1.0f;
        float k = (1.0f - u) * (1.0f - u) * 0.75f;
        float rr = r.r0 * (1.0f + 1.55f * u);
        blitMask(g_rIMask, nullptr, r.x, r.y, rr, rr, 118.0f * k, 132.0f * k, 155.0f * k);
    }
}




// ---------------------------------------------------------------- d3d9 presenter
// The GDI path below is kept for the headless /t mode and as a fallback, but
// presenting 23 MB through a window DC blocks on DWM for a fixed ~15 ms per
// BitBlt, which caps the frame rate at ~45. D3D9 presents asynchronously and
// does the additive blending on the GPU, like the original saver does.
static IDirect3D9         *g_d3d     = nullptr;
static IDirect3DDevice9   *g_dev     = nullptr;
static IDirect3DTexture9  *g_texRim  = nullptr;
static IDirect3DTexture9  *g_texSpec = nullptr;
static IDirect3DTexture9  *g_texBloom = nullptr;
static IDirect3DTexture9  *g_texWhite = nullptr;
static IDirect3DTexture9  *g_texBead = nullptr;
static IDirect3DTexture9  *g_texCutRim = nullptr, *g_texCutBlur = nullptr;
static IDirect3DTexture9  *g_texSoft = nullptr;
static IDirect3DTexture9  *g_texFilmA[FILM_N] = {nullptr}, *g_texFilmB[FILM_N] = {nullptr};
static IDirect3DTexture9  *g_texSec[IRI] = {nullptr};
static bool                g_useD3D  = false;

struct Vtx { float x, y, z, rhw; DWORD col; float u, v; };
#define BUB_VF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

static IDirect3DTexture9 *makeTexture(int w, int h, const unsigned char *src, int stride)
{
    IDirect3DTexture9 *tex = nullptr;
    if (FAILED(g_dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8,
                                    D3DPOOL_MANAGED, &tex, nullptr))) return nullptr;
    D3DLOCKED_RECT lr;
    if (FAILED(tex->LockRect(0, &lr, nullptr, 0))) { tex->Release(); return nullptr; }
    for (int y = 0; y < h; ++y) {
        unsigned int *dst = (unsigned int*)((unsigned char*)lr.pBits + (size_t)y * lr.Pitch);
        const unsigned char *s = src + (size_t)y * stride;
        for (int x = 0; x < w; ++x) {
            // the mask has to live in RGB *and* alpha: RGB is what additive blending
            // shades with (white RGB here draws solid squares), alpha is what the
            // occlusion pass multiplies the framebuffer by
            unsigned int v = s[x];
            dst[x] = (v << 24) | (v << 16) | (v << 8) | v;
        }
    }
    tex->UnlockRect(0);
    return tex;
}

// the fused pair is shaded in software; it reaches the screen as one texture quad
static const int MERGE_TEX_W = 1024, MERGE_TEX_H = 1024;
static IDirect3DTexture9 *g_mergeTex = nullptr;

static bool blitMergeTex(const std::vector<unsigned int> &buf, int w, int h)
{
    if (!g_dev || w <= 0 || h <= 0 || w > MERGE_TEX_W || h > MERGE_TEX_H) return false;
    if (!g_mergeTex) {
        if (FAILED(g_dev->CreateTexture(MERGE_TEX_W, MERGE_TEX_H, 1, 0,
                                        D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_mergeTex, nullptr)))
            return false;
    }
    D3DLOCKED_RECT lr;
    if (FAILED(g_mergeTex->LockRect(0, &lr, nullptr, 0))) return false;
    for (int y = 0; y < h; ++y)
        memcpy((unsigned char*)lr.pBits + (size_t)y * lr.Pitch, &buf[(size_t)y * w], (size_t)w * 4);
    g_mergeTex->UnlockRect(0);
    return true;
}

static bool initD3D(HWND hwnd, int w, int h)
{
    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_d3d) return false;

    D3DPRESENT_PARAMETERS pp;
    memset(&pp, 0, sizeof(pp));
    pp.Windowed            = TRUE;
    pp.SwapEffect          = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow       = hwnd;
    pp.BackBufferWidth     = w;
    pp.BackBufferHeight    = h;
    pp.BackBufferFormat    = D3DFMT_X8R8G8B8;
    pp.BackBufferCount     = 1;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;  // we pace it ourselves
    pp.MultiSampleType     = D3DMULTISAMPLE_NONE;

    DWORD flags = D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE;
    if (FAILED(g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                   flags, &pp, &g_dev))) {
        if (FAILED(g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                       D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &g_dev))) {
            g_d3d->Release(); g_d3d = nullptr;
            return false;
        }
    }

    g_dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    g_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    // alpha must come from the mask, not the vertex: the occlusion pass composites
    // on it, and DISABLE would leave it at the vertex's 1.0 and cut the whole quad
    g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    g_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    g_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    g_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    g_dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    g_dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    g_dev->SetFVF(BUB_VF);

    g_texRim  = makeTexture(SPR, SPR, g_rIMask, SPR);
    g_texSpec = makeTexture(SPR, SPR, g_wMask,  SPR);
    if (!g_texRim || !g_texSpec) return false;
    { static const unsigned char one[1] = {255}; g_texWhite = makeTexture(1, 1, one, 1); }
    g_texBead = makeTexture(SPR, SPR, g_bead.data(), SPR);
    g_texSoft    = makeTexture(SPR, SPR, g_soft.data(),    SPR);
    g_texCutRim  = makeTexture(SPR, SPR, g_cutRim.data(),  SPR);
    g_texCutBlur = makeTexture(SPR, SPR, g_cutBlur.data(), SPR);
    g_texBloom = makeTexture(SPR, SPR, g_blur.data(), SPR);
    for (int k = 0; k < IRI; ++k)
        g_texSec[k] = makeTexture(SPR, SPR, g_sec.data() + (size_t)k * SPR * SPR, SPR);
    for (int f = 0; f < FILM_N; ++f) {
        g_texFilmA[f] = makeTexture(SPR, SPR, g_filmA[f].data(), SPR);
        g_texFilmB[f] = makeTexture(SPR, SPR, g_filmB[f].data(), SPR);
        if (!g_texFilmA[f] || !g_texFilmB[f]) return false;
    }
    if (!g_texBloom || !g_texWhite || !g_texBead || !g_texCutRim || !g_texCutBlur || !g_texSoft) return false;
    for (int k = 0; k < IRI; ++k) if (!g_texSec[k]) return false;
    g_useD3D = true;
    return true;
}

static inline DWORD packCol(float r, float g, float b)
{
    if (r < 0.0f) r = 0.0f; if (r > 1.0f) r = 1.0f;
    if (g < 0.0f) g = 0.0f; if (g > 1.0f) g = 1.0f;
    if (b < 0.0f) b = 0.0f; if (b > 1.0f) b = 1.0f;
    return 0xFF000000u | ((DWORD)(r * 255.0f + 0.5f) << 16)
                       | ((DWORD)(g * 255.0f + 0.5f) << 8)
                       |  (DWORD)(b * 255.0f + 0.5f);
}

static inline void pushQuad(Vtx *buf, int &n, int cap,
                            float x0, float y0, float x1, float y1, DWORD col)
{
    if (n + 6 > cap) return;
    Vtx v[6] = {
        {x0, y0, 0, 1, col, 0, 0}, {x1, y0, 0, 1, col, 1, 0}, {x0, y1, 0, 1, col, 0, 1},
        {x1, y0, 0, 1, col, 1, 0}, {x1, y1, 0, 1, col, 1, 1}, {x0, y1, 0, 1, col, 0, 1}
    };
    memcpy(&buf[n], v, sizeof(v)); n += 6;
}

static void d3dPresent()
{
    if (!g_useD3D) return;
    g_dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
    g_dev->BeginScene();

    const float IRI_SPAN = 0.30f;

    // ---- 0. backdrop
    if (g_optBack && g_texWhite) {
        float w = (float)(g_virt.right - g_virt.left), h = (float)(g_virt.bottom - g_virt.top);
        DWORD cTop = packCol(0.004f, 0.024f, 0.039f);
        DWORD cBot = packCol(0.014f, 0.086f, 0.110f);
        Vtx v[6] = {
            {0, 0, 0, 1, cTop, 0, 0}, {w, 0, 0, 1, cTop, 0, 0}, {0, h, 0, 1, cBot, 0, 0},
            {w, 0, 0, 1, cTop, 0, 0}, {w, h, 0, 1, cBot, 0, 0}, {0, h, 0, 1, cBot, 0, 0}
        };
        g_dev->SetTexture(0, g_texWhite);
        g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, v, sizeof(Vtx));
    }

    // ---- bubbles, far to near: with over blending this is what puts one in
    // front of another instead of adding the two glows together
    std::vector<int> ord(g_bub.size());
    for (size_t i = 0; i < ord.size(); ++i) ord[i] = (int)i;
    if (ord.size() > 1)
        std::sort(ord.begin(), ord.end(),
                  [](int a, int c) { return g_bub[(size_t)a].depth < g_bub[(size_t)c].depth; });

    Vtx q[6];
    #define DRAWQUAD(tex, cx, cy, ax, ay, s, col)                             \
        do {                                                                  \
            float _hx = SPRH * ((ax) * (s)) / SPR_R;                          \
            float _hy = SPRH * ((ay) * (s)) / SPR_R;                          \
            float _x0 = (cx) - g_virt.left - _hx, _x1 = (cx) - g_virt.left + _hx; \
            float _y0 = (cy) - g_virt.top  - _hy, _y1 = (cy) - g_virt.top  + _hy; \
            Vtx _v[6] = {                                                     \
                {_x0, _y0, 0, 1, col, 0, 0}, {_x1, _y0, 0, 1, col, 1, 0},     \
                {_x0, _y1, 0, 1, col, 0, 1}, {_x1, _y0, 0, 1, col, 1, 0},     \
                {_x1, _y1, 0, 1, col, 1, 1}, {_x0, _y1, 0, 1, col, 0, 1}      \
            };                                                                \
            memcpy(q, _v, sizeof(_v));                                        \
            g_dev->SetTexture(0, tex);                                        \
            g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, q, sizeof(Vtx));    \
        } while (0)

    for (size_t oi = 0; oi < ord.size(); ++oi) {
        const Bubble &bb = g_bub[(size_t)ord[oi]];
        if (bb.r < 0.6f) continue;
        if (bb.merging) {
            if (bb.id != bb.mlead) continue;
            const Bubble *pr = nullptr;
            for (size_t qq = 0; qq < g_bub.size(); ++qq)
                if (g_bub[qq].merging && g_bub[qq].mteam == bb.mteam && g_bub[qq].id != bb.id) { pr = &g_bub[qq]; break; }
            if (!pr) continue;
            std::vector<unsigned int> mb; int mx, my, mw, mh;
            shadeMergePair(bb, *pr, mb, mx, my, mw, mh);
            if (mw > 0 && mh > 0 && blitMergeTex(mb, mw, mh)) {
                float x0 = (float)(mx - g_virt.left), y0 = (float)(my - g_virt.top);
                float x1 = x0 + (float)mw,             y1 = y0 + (float)mh;
                float u1 = (float)mw / (float)MERGE_TEX_W, v1 = (float)mh / (float)MERGE_TEX_H;
                unsigned int col = packCol(1.0f, 1.0f, 1.0f);
                Vtx _v[6] = {
                    {x0, y0, 0, 1, col, 0,  0 }, {x1, y0, 0, 1, col, u1, 0 },
                    {x0, y1, 0, 1, col, 0,  v1}, {x1, y0, 0, 1, col, u1, 0 },
                    {x1, y1, 0, 1, col, u1, v1}, {x0, y1, 0, 1, col, 0,  v1}
                };
                memcpy(q, _v, sizeof(_v));
                g_dev->SetTexture(0, g_mergeTex);
                g_dev->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_ONE);
                g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
                g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, q, sizeof(Vtx));
            }
            continue;
        }

        float ax, ay;
        bubbleExtents(bb, ax, ay);
        if (bb.merging) {                     // swelling as it is pulled in
            float u = bb.mergeU / MERGE_ANIM; if (u > 1.0f) u = 1.0f;
            float sw = bb.mabsorb ? (1.0f - 0.96f * u * u) : (1.0f + 0.16f * u * u);
            ax *= sw; ay *= sw;
        }
        if (bb.bloop > 0.0f) {                // and the bloop when it becomes one
            float u = 1.0f - bb.bloop / BLOOP_T;
            ax *= 1.0f + 0.20f * sinf(3.14159265f * u);
            ay *= 1.0f + 0.20f * sinf(3.14159265f * u);
        }
        float gk = bb.r / (BASE_R * g_scale);
        if (gk < 0.40f) gk = 0.40f;
        if (gk > 1.70f) gk = 1.70f;
        bool soft = (g_optDepth && bb.depth < 0.45f);

        // ---- background layer: a faint flat ring, no halo, no highlight, no rainbow,
        // and it occludes nothing because nothing sits behind it
        if (soft) {
            float cr, cg, cb; bubbleColour(bb, bb.hue, cr, cg, cb);
            DRAWQUAD(g_texRim, bb.x, bb.y, ax, ay, 1.0f, packCol(cr, cg, cb));
            DRAWQUAD(g_texSpec, bb.x, bb.y, ax, ay, 1.0f, packCol(0.26f, 0.28f, 0.30f));
            continue;
        }

        // occlusion: erase what is behind this bubble wherever its own mask
        // covers, so a nearer bubble sits in front instead of both glows adding
        {
            float hx = SPRH * ax / SPR_R, hy = SPRH * ay / SPR_R;
            float x0 = bb.x - g_virt.left - hx, x1 = bb.x - g_virt.left + hx;
            float y0 = bb.y - g_virt.top  - hy, y1 = bb.y - g_virt.top  + hy;
            Vtx v[6] = {
                {x0, y0, 0, 1, 0xFFFFFFFFu, 0, 0}, {x1, y0, 0, 1, 0xFFFFFFFFu, 1, 0},
                {x0, y1, 0, 1, 0xFFFFFFFFu, 0, 1}, {x1, y0, 0, 1, 0xFFFFFFFFu, 1, 0},
                {x1, y1, 0, 1, 0xFFFFFFFFu, 1, 1}, {x0, y1, 0, 1, 0xFFFFFFFFu, 0, 1}
            };
            g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ZERO);
            g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            g_dev->SetTexture(0, g_texCutRim);
            g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, v, sizeof(Vtx));
            g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
            g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
        }

        // soap film: the marbled body, behind the rim
        if (g_optFilm) {
            float cr, cg, cb;
            float ha = bb.hue + bb.filmOffA; ha -= floorf(ha);
            float hb = bb.hue + bb.filmOffB; hb -= floorf(hb);
            bubbleColour(bb, ha, cr, cg, cb);
            DRAWQUAD(g_texFilmA[bb.filmIdx], bb.x, bb.y, ax, ay, 1.0f,
                     packCol(cr * 0.18f, cg * 0.18f, cb * 0.18f));
            bubbleColour(bb, hb, cr, cg, cb);
            DRAWQUAD(g_texFilmB[bb.filmIdx], bb.x, bb.y, ax, ay, 1.0f,
                     packCol(cr * 0.15f, cg * 0.15f, cb * 0.15f));
        }

        // bloom sits behind the bubble
        if (g_optBloom) {
            float cr, cg, cb; bubbleColour(bb, bb.hue, cr, cg, cb);
            float k = 0.32f * gk;
            DRAWQUAD(g_texBloom, bb.x, bb.y, ax, ay, 1.25f, packCol(cr * k, cg * k, cb * k));
        }

        if (g_optIri) {                              // rainbow, sector by sector
            float phase = bb.wobP * 0.15915494f;
            for (int k = 0; k < IRI; ++k) {
                float hh = bb.hue + bb.iriSpan * (((float)k + 0.5f) / (float)IRI - 0.5f) + phase;
                hh -= floorf(hh);
                float cr, cg, cb; bubbleColour(bb, hh, cr, cg, cb);
                DRAWQUAD(g_texSec[k], bb.x, bb.y, ax, ay, 1.0f, packCol(cr, cg, cb));
            }
        } else {
            float cr, cg, cb; bubbleColour(bb, bb.hue, cr, cg, cb);
            DRAWQUAD(g_texRim, bb.x, bb.y, ax, ay, 1.0f, packCol(cr, cg, cb));
        }

        // the highlight, once, on top of this bubble only
        {
            float dim = g_optDepth ? (0.34f + 0.66f * bb.depth) : 1.0f;
            if (bb.die > 0.0f) {
                float u = bb.die / POP_T; if (u > 1.0f) u = 1.0f;
                dim *= 1.0f - 0.85f * u;
            }
            DWORD wv = (DWORD)(255.0f * dim + 0.5f); if (wv > 255) wv = 255;
            DRAWQUAD(g_texSpec, bb.x, bb.y, ax, ay, 1.0f, 0xFF000000u | (wv << 16) | (wv << 8) | wv);
        }
    }

    // ---- condensation, on the glass so in front of everything
    if (g_optDrops) {
        for (int i = 0; i < 3; ++i) {
            const Drop &d = g_drop[i];
            if (!d.on) continue;
            float a = (d.state == 2) ? (d.fade / 2.5f) : 1.0f;
            if (d.y - d.y0 > 3.0f) {
                float hy = (d.y - d.y0) * 0.5f + d.r * 0.4f;
                float cy = (d.y + d.y0) * 0.5f;
                float hx = SPRH * (d.r * 0.42f) / SPR_R, hy2 = SPRH * hy / SPR_R;
                Vtx v[6] = {
                    {d.x - g_virt.left - hx, cy - g_virt.top - hy2, 0, 1, packCol(0.21f*a,0.25f*a,0.31f*a), 0, 0},
                    {d.x - g_virt.left + hx, cy - g_virt.top - hy2, 0, 1, packCol(0.21f*a,0.25f*a,0.31f*a), 1, 0},
                    {d.x - g_virt.left - hx, cy - g_virt.top + hy2, 0, 1, packCol(0.21f*a,0.25f*a,0.31f*a), 0, 1},
                    {d.x - g_virt.left + hx, cy - g_virt.top - hy2, 0, 1, packCol(0.21f*a,0.25f*a,0.31f*a), 1, 0},
                    {d.x - g_virt.left + hx, cy - g_virt.top + hy2, 0, 1, packCol(0.21f*a,0.25f*a,0.31f*a), 1, 1},
                    {d.x - g_virt.left - hx, cy - g_virt.top + hy2, 0, 1, packCol(0.21f*a,0.25f*a,0.31f*a), 0, 1}
                };
                g_dev->SetTexture(0, g_texBloom);
                g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, v, sizeof(Vtx));
            }
            DRAWQUAD(g_texBead, d.x, d.y, d.r * 1.15f, d.r * 1.15f, 1.0f, packCol(0.17f*a,0.22f*a,0.27f*a));
            DRAWQUAD(g_texBead, d.x - d.r*0.30f, d.y - d.r*0.30f, d.r*0.42f, d.r*0.42f, 1.0f,
                     packCol(0.59f*a, 0.65f*a, 0.71f*a));
        }
    }

    // ---- pop ripples
    for (int i = 0; i < 6; ++i) {
        const Ripple &r = g_rip[i];
        if (!r.on) continue;
        float u = r.t / r.dur; if (u > 1.0f) u = 1.0f;
        float k = (1.0f - u) * (1.0f - u) * 0.75f;
        float rr = r.r0 * (1.0f + 1.55f * u);
        DRAWQUAD(g_texRim, r.x, r.y, rr, rr, 1.0f, packCol(0.46f*k, 0.52f*k, 0.61f*k));
    }
    #undef DRAWQUAD

    g_dev->EndScene();
    g_dev->Present(nullptr, nullptr, nullptr, nullptr);
}

// ---------------------------------------------------------------- app state
static HWND  g_hwnd      = nullptr;
static HDC   g_wdc       = nullptr;
static bool  g_preview   = false;
static bool  g_running   = true;
static POINT g_mouse0    = {0, 0};
static bool  g_haveMouse = false;
static double g_qpcFreq  = 1.0;
static int    g_frames   = 0;
static double g_busy     = 0.0;
static double g_msSim    = 0.0;
static double g_msBlit   = 0.0;
static float  g_iv[1200] = {0};
static float  g_ivRamp[1200] = {0}, g_ivSteady[1200] = {0};
static int    g_nRamp = 0, g_nSteady = 0;
static double g_nextFrame = 0.0;
static double g_start    = 0.0;

static double nowSec()
{
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart / g_qpcFreq;
}

// ---------------------------------------------------------------- window proc
// play-mode helpers and the settings page are defined further down
static int  runSettings(HWND parent, HINSTANCE hInst);
static int  bubbleAt(float x, float y);
static void appFill();
static void appDown(HWND h, float x, float y);
static void appMove(float x, float y);
static void appUp(float x, float y);
static void appSpawnAt(float x, float y);

static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SETCURSOR:
        if (g_preview || g_appMode) break;       // a window keeps its normal pointer
        SetCursor(nullptr);
        return TRUE;
    case WM_ERASEBKGND:
        if (g_appMode) return 1;                 // we paint every frame
        break;
    case WM_COMMAND:
        if (!g_appMode) break;
        switch (LOWORD(wp)) {
        case APP_SETTINGS_ID:
            runSettings(h, GetModuleHandleW(nullptr));
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case APP_RESET_ID:
            appFill();
            return 0;
        case APP_EXIT_ID:
            g_running = false;
            PostQuitMessage(0);
            return 0;
        }
        return 0;
    case WM_DISPLAYCHANGE:
        // a preview child is sized by whoever hosts it: never re-read the real
        // monitors or resize here, or the pane renders a desktop-sized canvas
        if (g_preview) return 0;
        refreshMonitors();
        g_dib.create(g_virt.right - g_virt.left, g_virt.bottom - g_virt.top);
        buildBackdrop(g_dib.w, g_dib.h);
        fillSurface();
        g_dirty.clear();
        SetWindowPos(h, nullptr, g_virt.left, g_virt.top,
                     g_virt.right - g_virt.left, g_virt.bottom - g_virt.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (g_appMode) {
            if (wp == VK_F2) { runSettings(h, GetModuleHandleW(nullptr)); InvalidateRect(h, nullptr, FALSE); }
            return 0;                            // a keypress must not close the app
        }
        if (!g_preview) { g_running = false; PostQuitMessage(0); }
        return 0;
    case WM_LBUTTONDOWN:
        if (g_appMode) { appDown(h, (float)(short)LOWORD(lp), (float)(short)HIWORD(lp)); return 0; }
        if (!g_preview) { g_running = false; PostQuitMessage(0); }
        return 0;
    case WM_MOUSEMOVE:
        if (g_appMode) { appMove((float)(short)LOWORD(lp), (float)(short)HIWORD(lp)); return 0; }
        return 0;
    case WM_LBUTTONUP:
        if (g_appMode) { appUp((float)(short)LOWORD(lp), (float)(short)HIWORD(lp)); return 0; }
        return 0;
    case WM_RBUTTONDOWN:
        if (g_appMode) { appSpawnAt((float)(short)LOWORD(lp), (float)(short)HIWORD(lp)); return 0; }
        if (!g_preview) { g_running = false; PostQuitMessage(0); }
        return 0;
    case WM_MBUTTONDOWN:
        if (g_appMode) return 0;
        if (!g_preview) { g_running = false; PostQuitMessage(0); }
        return 0;
    case WM_MOUSEWHEEL:
        // scroll on a bubble to size it by hand. lParam is in screen coords here, not
        // client, so it has to be converted before the hit test
        if (g_appMode) {
            POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
            ScreenToClient(h, &pt);
            int wd = GET_WHEEL_DELTA_WPARAM(wp);
            if (wd != 0) {
                int i = bubbleAt((float)pt.x, (float)pt.y);
                if (i >= 0) {
                    Bubble &b = g_bub[(size_t)i];
                    float k = powf(1.12f, (float)wd / (float)WHEEL_DELTA);
                    b.sizeK *= k;
                    if (b.sizeK < 0.45f) b.sizeK = 0.45f;   // limits both ways
                    if (b.sizeK > 3.20f) b.sizeK = 3.20f;
                }
            }
            return 0;
        }
        break;
    case WM_CLOSE:
        g_running = false;
        PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

// ---------------------------------------------------------------- helpers
static void saveBMP(const char *path, const unsigned int *px, int w, int h)
{
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    memset(&fh, 0, sizeof(fh));
    memset(&ih, 0, sizeof(ih));
    int rowBytes = w * 3, pad = (4 - (rowBytes % 4)) % 4;
    int imgSize  = (rowBytes + pad) * h;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + imgSize;
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = h;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = imgSize;
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    std::vector<unsigned char> row(rowBytes + pad, 0);
    for (int y = h - 1; y >= 0; --y) {
        const unsigned int *src = px + (size_t)y * w;
        for (int x = 0; x < w; ++x) {
            unsigned int c = src[x];
            row[x * 3 + 0] = (unsigned char)(c & 0xFF);
            row[x * 3 + 1] = (unsigned char)((c >> 8) & 0xFF);
            row[x * 3 + 2] = (unsigned char)((c >> 16) & 0xFF);
        }
        fwrite(row.data(), row.size(), 1, f);
    }
    fclose(f);
}

// Violation metric: how far a bubble's disc pokes into *dead* desktop space
// (area that belongs to no monitor). 0 = perfect.
static long long deadOverlap(const Bubble &b, long long samples)
{
    long long bad = 0;
    for (long long i = 0; i < samples; ++i) {
        float th = (float)i / (float)samples * 6.2831853f;
        float rr = b.r * 0.95f;
        int px = (int)(b.x + cosf(th) * rr);
        int py = (int)(b.y + sinf(th) * rr);
        if (!inMon(px, py)) ++bad;
    }
    return bad;
}

// ---------------------------------------------------------------- test mode
// grid test: one row of bubbles at known sizes, so glow can be measured per size
static int runGridTest(const char *outDir)
{
    refreshMonitors();
    g_scale = 1.0f;
    g_dib.create(g_virt.right - g_virt.left, g_virt.bottom - g_virt.top);
    buildBackdrop(g_dib.w, g_dib.h);
    fillSurface();

    const int N = 5;
    const float sizes[N] = {0.80f, 0.90f, 1.00f, 1.10f, 1.20f};
    g_bub.clear();
    for (int i = 0; i < N; ++i) {
        Bubble b;
        spawnBubble(b, true);
        b.sizeK = sizes[i];
        b.depth = (sizes[i] - 0.80f) / 0.40f;      // depth follows size, as in play
        b.r = BASE_R * g_scale * b.sizeK;
        b.wobA = 0.0f; b.sq = 0.0f; b.die = 0.0f; b.grace = 0.0f;
        b.hue = 0.55f; b.sat = 0.45f; b.val = 0.80f;
        b.x = g_mons[0].left + 260.0f + i * 350.0f;
        b.y = g_mons[0].top  + 420.0f;
        b.vx = 0.0f; b.vy = 0.0f;
        g_bub.push_back(b);
    }
    fillSurface();
    g_dirty.clear();
    drawBubbles();

    const RECT &m = g_mons[0];
    int w = m.right - m.left, h = m.bottom - m.top;
    std::vector<unsigned int> buf((size_t)w * h);
    for (int y = 0; y < h; ++y)
        memcpy(&buf[(size_t)y * w],
               g_dib.px + (size_t)(m.top - g_virt.top + y) * g_dib.w + (m.left - g_virt.left),
               (size_t)w * 4);
    char path[MAX_PATH];
    sprintf(path, "%s/grid_%dx%d.bmp", outDir, w, h);
    saveBMP(path, buf.data(), w, h);

    FILE *f = fopen((std::string(outDir) + "/grid.txt").c_str(), "w");
    if (f) {
        for (int i = 0; i < N; ++i)
            fprintf(f, "size %.2f r=%.1f x=%.0f y=%.0f\n", sizes[i],
                    g_bub[i].r, g_bub[i].x - m.left, g_bub[i].y - m.top);
        fprintf(f, "options: bloom=%d iri=%d depth=%d back=%d\n",
                g_optBloom, g_optIri, g_optDepth, g_optBack);
        fclose(f);
    }
    g_dib.destroy();
    return 0;
}

static int runTest(const char *outDir, float seconds)
{
    initBubbles(0, 0.0f);
    int frames = (int)(seconds * 60.0f);
    char path[MAX_PATH];
    long long violations = 0, checks = 0;
    int maxAlive = 0;
    double ticksToMs = 0.0;
    double drawMs = 0.0; int drawN = 0;
    // motion character stats (steady state only)
    double statSpeed = 0, statStill = 0, statNN = 0, statOcc = 0;
    int    statN = 0;
    LARGE_INTEGER qf; QueryPerformanceFrequency(&qf);
    double t = 0;
    int shot = 0;
    LARGE_INTEGER q0, q1;
    QueryPerformanceCounter(&q0);
    static float hue0[4] = {-1, -1, -1, -1};
    // teleport detector: any frame where a bubble moves further than its own
    // velocity accounts for means the constraint snapped it
    std::vector<float> prevx, prevy;
    std::vector<int> previd;
    int snaps = 0; float worstExcess = 0.0f;
    for (int f = 0; f < frames; ++f) {
        update(1.0f / 60.0f);
        t += 1.0 / 60.0;
        if (hue0[0] < 0.0f)
            for (int i = 0; i < 4 && i < (int)g_bub.size(); ++i) hue0[i] = g_bub[i].hue;
        if ((int)g_bub.size() > maxAlive) maxAlive = (int)g_bub.size();

        { static int lastMerges = 0;
          if (g_merges != lastMerges) {           // a merge moves one and reindexes all
              lastMerges = g_merges;
              prevx.clear(); prevy.clear(); previd.clear();
          } }
        if (prevx.size() != g_bub.size()) {        // new bubbles: seed, do not score
            size_t oldn = prevx.size();
            prevx.resize(g_bub.size(), 0.0f);
            prevy.resize(g_bub.size(), 0.0f);
            previd.resize(g_bub.size(), 0);
            for (size_t i = oldn; i < g_bub.size(); ++i) {
                prevx[i] = g_bub[i].x; prevy[i] = g_bub[i].y; previd[i] = g_bub[i].id;
            }
        }
        for (size_t i = 0; i < g_bub.size(); ++i) {
            if (f > 2 && previd[i] == g_bub[i].id && !g_bub[i].merging) {
                float step = sqrtf((g_bub[i].x - prevx[i]) * (g_bub[i].x - prevx[i]) +
                                   (g_bub[i].y - prevy[i]) * (g_bub[i].y - prevy[i]));
                float expect = sqrtf(g_bub[i].vx * g_bub[i].vx + g_bub[i].vy * g_bub[i].vy) / 60.0f + 8.0f;
                if (step > expect) {
                    ++snaps;
                    if (step - expect > worstExcess) worstExcess = step - expect;
                }
            }
            prevx[i] = g_bub[i].x;
            prevy[i] = g_bub[i].y;
            previd[i] = g_bub[i].id;
        }

        if (t > seconds * 0.5 && !g_bub.empty()) {
            float spsum = 0; int still = 0;
            for (size_t i = 0; i < g_bub.size(); ++i) {
                float sp = sqrtf(g_bub[i].vx * g_bub[i].vx + g_bub[i].vy * g_bub[i].vy);
                spsum += sp;
                if (sp < 50.0f) ++still;
            }
            statSpeed += spsum / g_bub.size();
            statStill += (double)still / g_bub.size();
            if (f % 15 == 0) {
                // mean nearest-neighbour distance + share of a coarse grid occupied
                double nn = 0;
                for (size_t i = 0; i < g_bub.size(); ++i) {
                    float best = 1e9f;
                    for (size_t j = 0; j < g_bub.size(); ++j) {
                        if (i == j) continue;
                        float dx = g_bub[i].x - g_bub[j].x, dy = g_bub[i].y - g_bub[j].y;
                        float d = sqrtf(dx * dx + dy * dy);
                        if (d < best) best = d;
                    }
                    nn += (best < 1e8f ? best : 0);
                }
                statNN += nn / g_bub.size();
                const int GX = 40, GY = 26;
                int occ[GY][GX]; memset(occ, 0, sizeof(occ));
                float fw = (float)(g_virt.right - g_virt.left) / GX;
                float fh = (float)(g_virt.bottom - g_virt.top) / GY;
                int total = 0;
                for (size_t i = 0; i < g_bub.size(); ++i) {
                    int gx = (int)((g_bub[i].x - g_virt.left) / fw);
                    int gy = (int)((g_bub[i].y - g_virt.top) / fh);
                    if (gx >= 0 && gx < GX && gy >= 0 && gy < GY) occ[gy][gx] = 1;
                }
                for (int y = 0; y < GY; ++y) for (int x = 0; x < GX; ++x) total += occ[y][x];
                statOcc += (double)total / (GX * GY);
                ++statN;
            }
        }
        for (size_t i = 0; i < g_bub.size(); ++i) {
            long long bad = deadOverlap(g_bub[i], 24);
            violations += bad; checks += 24;
        }
        if (f % 120 == 0 || g_shotBurst > 0) {
            if (g_shotBurst > 0) --g_shotBurst;
            LARGE_INTEGER d0, d1; QueryPerformanceCounter(&d0);
            // the incremental renderer needs a clean surface when frames are skipped
            fillSurface();
            g_dirty.clear();
            drawBubbles();
            QueryPerformanceCounter(&d1);
            drawMs += 1000.0 * (double)(d1.QuadPart - d0.QuadPart) / (double)qf.QuadPart;
            ++drawN;
            g_curShot = shot;
            for (size_t i = 0; i < g_mons.size(); ++i) {
                const RECT &m = g_mons[i];
                int w = m.right - m.left, h = m.bottom - m.top;
                std::vector<unsigned int> buf((size_t)w * h);
                for (int y = 0; y < h; ++y)
                    memcpy(&buf[(size_t)y * w],
                           g_dib.px + (size_t)(m.top - g_virt.top + y) * g_dib.w + (m.left - g_virt.left),
                           (size_t)w * 4);
                sprintf(path, "%s/frame%03d_mon%u_%dx%d.bmp", outDir, shot, (unsigned)i, w, h);
                saveBMP(path, buf.data(), w, h);
            }
            ++shot;
        }
    }
    QueryPerformanceCounter(&q1);
    ticksToMs = 1000.0 * (double)(q1.QuadPart - q0.QuadPart) / (double)qf.QuadPart / (double)frames;
    sprintf(path, "%s/report.txt", outDir);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "monitors: %u\n", (unsigned)g_mons.size());
        for (size_t i = 0; i < g_mons.size(); ++i)
            fprintf(f, "  mon%u: %d,%d %dx%d\n", (unsigned)i, g_mons[i].left - g_virt.left,
                    g_mons[i].top - g_virt.top,
                    g_mons[i].right - g_mons[i].left, g_mons[i].bottom - g_mons[i].top);
        fprintf(f, "virtual: %dx%d\n", g_virt.right - g_virt.left, g_virt.bottom - g_virt.top);
        fprintf(f, "bubbles: %u\n", (unsigned)g_bub.size());
        fprintf(f, "frames: %d  seconds: %.1f\n", frames, (double)seconds);
        fprintf(f, "dead-space overlap samples: %lld of %lld (%.4f%%)\n",
                violations, checks, 100.0 * (double)violations / (double)(checks ? checks : 1));
        fprintf(f, "sim cost: %.2f ms/frame (update only, draw counted separately)\n", ticksToMs);
        fprintf(f, "draw cost: %.2f ms/frame\n", drawN ? drawMs / drawN : 0.0);
        fprintf(f, "max bubbles alive: %d\n", maxAlive);
        {
            float mn = 1e9f, mx = 0.0f; double av = 0;
            for (size_t i = 0; i < g_bub.size(); ++i) {
                if (g_bub[i].sizeK < mn) mn = g_bub[i].sizeK;
                if (g_bub[i].sizeK > mx) mx = g_bub[i].sizeK;
                av += g_bub[i].sizeK;
            }
            fprintf(f, "options: depth=%d wobble=%d pop=%d iri=%d bloom=%d mood=%d back=%d drops=%d hero=%d | mood_hue=%.3f\n",
                    g_optDepth, g_optWobble, g_optPop, g_optIri, g_optBloom, g_optMood,
                    g_optBack, g_optDrops, g_optHero, g_moodHue);
            { int hcount = 0, dcount = 0;
              for (size_t i = 0; i < g_bub.size(); ++i) if (g_bub[i].hero) ++hcount;
              for (int i = 0; i < 3; ++i) if (g_drop[i].on) ++dcount;
              { int big = 0; float mxr = 0.0f;
                for (size_t i = 0; i < g_bub.size(); ++i) {
                    if (g_bub[i].merged) ++big;
                    if (g_bub[i].sizeK > mxr) mxr = g_bub[i].sizeK;
                }
                fprintf(f, "extras: heroes=%d droplets=%d layer_skips=%lld merges=%d big=%d maxSizeK=%.2f\n",
                        hcount, dcount, g_layerSkips, g_merges, big, mxr); }
              for (int i = 0; i < 3; ++i)
                  if (g_drop[i].on)
                      fprintf(f, "  drop%d: x=%.0f y=%.0f y0=%.0f r=%.0f state=%d\n",
                              i, g_drop[i].x, g_drop[i].y, g_drop[i].y0, g_drop[i].r, g_drop[i].state); }
            float wob = 0.0f;
            for (size_t i = 0; i < g_bub.size(); ++i) {
                float ax, ay; bubbleExtents(g_bub[i], ax, ay);
                wob += fabsf(ax - ay) / (g_bub[i].r > 0.1f ? g_bub[i].r : 0.1f);
            }
            fprintf(f, "sizeK: min=%.2f max=%.2f mean=%.2f | ellipse delta=%.1f%% | pops=%d ripples=%d\n",
                    mn, mx, g_bub.empty() ? 0.0 : av / g_bub.size(),
                    g_bub.empty() ? 0.0 : 100.0 * wob / g_bub.size(), g_popCount, g_ripCount);
        }
        fprintf(f, "mergeShots:");
        for (int i = 0; i < g_mergeShotsN; ++i) fprintf(f, " %d@%.0f,%.0f", g_mergeShots[i], g_mergePX[i], g_mergePY[i]);
        fprintf(f, "\n");
        fprintf(f, "snaps (frames where a bubble moved further than its velocity allows): %d, worst excess %.1f px\n", snaps, worstExcess);
        fprintf(f, "hue travel over the run (same 4 bubbles):\n");
        for (int i = 0; i < 4 && i < (int)g_bub.size(); ++i) {
            float r0, g0, b0, r1, g1, b1;
            hsv2rgb(hue0[i], r0, g0, b0);
            hsv2rgb(g_bub[i].hue, r1, g1, b1);
            float kA = g_bub[i].val * (1.0f - g_bub[i].sat), kB = g_bub[i].val * g_bub[i].sat;
            fprintf(f, "  b%d h %.3f -> %.3f  rgb %d,%d,%d -> %d,%d,%d  rate %+.3f/s\n",
                    i, hue0[i], g_bub[i].hue,
                    (int)((kA + kB * r0) * 255.0f), (int)((kA + kB * g0) * 255.0f), (int)((kA + kB * b0) * 255.0f),
                    (int)((kA + kB * r1) * 255.0f), (int)((kA + kB * g1) * 255.0f), (int)((kA + kB * b1) * 255.0f),
                    g_bub[i].hueRate);
        }
        fprintf(f, "hue: ");
        for (int i = 0; i < 4 && i < (int)g_bub.size(); ++i) {
            float rr, gg, bb; hsv2rgb(g_bub[i].hue, rr, gg, bb);
            float kA = g_bub[i].val * (1.0f - g_bub[i].sat), kB = g_bub[i].val * g_bub[i].sat;
            fprintf(f, "[h=%.3f rgb=%d,%d,%d rate=%+.3f] ", g_bub[i].hue,
                    (int)((kA + kB * rr) * 255.0f), (int)((kA + kB * gg) * 255.0f), (int)((kA + kB * bb) * 255.0f),
                    g_bub[i].hueRate);
        }
        fprintf(f, "\n");
        fprintf(f, "debug: ");
        for (int i = 0; i < 8 && i < (int)g_bub.size(); ++i)
            fprintf(f, "(v=%.0f dr=%.0f) ", sqrtf(g_bub[i].vx * g_bub[i].vx + g_bub[i].vy * g_bub[i].vy), g_bub[i].drift);
        fprintf(f, "\n");
        {   // how many pairs are in contact right now?
            int contacts = 0;
            for (size_t i = 0; i < g_bub.size(); ++i)
                for (size_t j = i + 1; j < g_bub.size(); ++j) {
                    float dx = g_bub[i].x - g_bub[j].x, dy = g_bub[i].y - g_bub[j].y;
                    if (dx * dx + dy * dy < (g_bub[i].r + g_bub[j].r) * (g_bub[i].r + g_bub[j].r)) ++contacts;
                }
            fprintf(f, "contacts: %d pairs of %d bubbles\n", contacts, (int)(g_bub.size() * (g_bub.size() - 1) / 2));
        }
        if (statN > 0) {
            fprintf(f, "motion: mean_speed=%.0f px/s  still_share=%.2f (<50px/s)\n",
                    statSpeed / (double)(frames - (frames / 2)), statStill / (double)(frames - (frames / 2)));
            fprintf(f, "spread: mean_nearest_neighbour=%.0f px (bubble diameter 184)  grid_occupied=%.2f\n",
                    statNN / statN, statOcc / statN);
        }
        fclose(f);
    }
    return 0;
}

// ---------------------------------------------------------------- play mode
static int bubbleById(int id)
{
    for (size_t i = 0; i < g_bub.size(); ++i) if (g_bub[i].id == id) return (int)i;
    return -1;
}

static int bubbleAt(float x, float y)
{
    int best = -1; float bestD = 0.0f;
    for (size_t i = 0; i < g_bub.size(); ++i) {
        const Bubble &b = g_bub[i];
        if (b.die > 0.0f) continue;
        float dx = x - b.x, dy = y - b.y;
        float d2 = dx * dx + dy * dy;
        if (d2 > b.r * b.r) continue;
        if (best < 0 || d2 < bestD) { best = (int)i; bestD = d2; }
    }
    return best;
}

static void appFill()
{
    g_bub.clear();
    const int target = 10;            // a starting handful; after that it is yours
    for (int i = 0; i < target; ++i) {
        Bubble b; spawnBubble(b, true);
        float R = BASE_R * g_scale * b.sizeK;
        b.x = rrange((float)g_virt.left + R, (float)g_virt.right  - R);
        b.y = rrange((float)g_virt.top  + R, (float)g_virt.bottom - R);
        float a2 = rrange(0.0f, 6.2831853f);
        b.vx = cosf(a2) * b.drift; b.vy = sinf(a2) * b.drift;
        b.grace = 0.0f;
        b.held  = false;
        g_bub.push_back(b);
    }
}

static void appDown(HWND h, float x, float y)
{
    int i = bubbleAt(x, y);
    if (i < 0) return;
    Bubble &b = g_bub[(size_t)i];
    g_dragId  = b.id;
    b.held    = true;
    g_dragOffX = x - b.x; g_dragOffY = y - b.y;
    g_downT = nowSec(); g_downX = x; g_downY = y; g_moved = 0.0f;
    g_lastMX = x; g_lastMY = y; g_lastMT = g_downT;
    g_flingVX = g_flingVY = 0.0f;
    SetCapture(h);
}

static void appMove(float x, float y)
{
    if (!g_dragId) return;
    int i = bubbleById(g_dragId);
    if (i < 0) { g_dragId = 0; return; }
    Bubble &b = g_bub[(size_t)i];
    b.x = x - g_dragOffX;
    b.y = y - g_dragOffY;
    if (b.x < g_virt.left   + b.r) b.x = (float)g_virt.left   + b.r;
    if (b.x > g_virt.right  - b.r) b.x = (float)g_virt.right  - b.r;
    if (b.y < g_virt.top    + b.r) b.y = (float)g_virt.top    + b.r;
    if (b.y > g_virt.bottom - b.r) b.y = (float)g_virt.bottom - b.r;
    float dx = x - g_downX, dy = y - g_downY;
    float d = sqrtf(dx * dx + dy * dy);
    if (d > g_moved) g_moved = d;
    double t = nowSec();
    double dt = t - g_lastMT;
    if (dt > 0.002) {
        float vx = (float)((x - g_lastMX) / dt), vy = (float)((y - g_lastMY) / dt);
        g_flingVX = g_flingVX * 0.55f + vx * 0.45f;
        g_flingVY = g_flingVY * 0.55f + vy * 0.45f;
        g_lastMX = x; g_lastMY = y; g_lastMT = t;
    }
}

static void appUp(float x, float y)
{
    (void)x; (void)y;
    if (!g_dragId) return;
    int i = bubbleById(g_dragId);
    g_dragId = 0;
    ReleaseCapture();
    if (i < 0) return;
    Bubble &b = g_bub[(size_t)i];
    b.held = false;
    bool tap = (nowSec() - g_downT) < 0.30 && g_moved < 6.0f;
    if (tap) {                                  // a click, not a drag: pop
        b.die = 0.0001f;
        addRipple(b.x, b.y, b.r * 1.15f);
        return;
    }
    float lim = 3600.0f * (g_scale > 0.2f ? g_scale : 0.2f);
    float vx = g_flingVX, vy = g_flingVY;
    float sp = sqrtf(vx * vx + vy * vy);
    if (sp > lim) { vx *= lim / sp; vy *= lim / sp; }
    b.vx = vx; b.vy = vy;
    b.grace = 0.25f;
}

static void appSpawnAt(float x, float y)
{
    Bubble b; spawnBubble(b, true);
    float R = BASE_R * g_scale * b.sizeK;
    if (x < g_virt.left   + R) x = (float)g_virt.left   + R;
    if (x > g_virt.right  - R) x = (float)g_virt.right  - R;
    if (y < g_virt.top    + R) y = (float)g_virt.top    + R;
    if (y > g_virt.bottom - R) y = (float)g_virt.bottom - R;
    b.x = x; b.y = y;
    float a2 = rrange(0.0f, 6.2831853f);
    b.vx = cosf(a2) * b.drift; b.vy = sinf(a2) * b.drift;
    b.age = 3.0f; b.grace = 0.30f; b.held = false;
    g_bub.push_back(b);
}

// ---------------------------------------------------------------- window proc

// ---------------------------------------------------------------- settings
static const wchar_t *SET_KEY_REAL =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Screensavers\\BubblesMultiMon";
static const wchar_t *SET_KEY_TEST =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Screensavers\\BubblesMultiMonTest";
// the play app keeps its own toggles: fiddling in there must not restyle the saver
static const wchar_t *SET_KEY_PLAY =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Screensavers\\BubblesMultiMonPlay";
static const wchar_t *SET_KEY = SET_KEY_REAL;

static void loadSettings()
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SET_KEY, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        DWORD v, sz, ty;
        sz = sizeof(v); ty = 0;
        if (RegQueryValueExW(k, L"Size",   0, &ty, (LPBYTE)&v, &sz) == ERROR_SUCCESS && ty == REG_DWORD) g_optSize   = (int)(v != 0);
        sz = sizeof(v); ty = 0;
        if (RegQueryValueExW(k, L"Film",   0, &ty, (LPBYTE)&v, &sz) == ERROR_SUCCESS && ty == REG_DWORD) g_optFilm   = (int)(v != 0);
        sz = sizeof(v); ty = 0;
        if (RegQueryValueExW(k, L"Merge",  0, &ty, (LPBYTE)&v, &sz) == ERROR_SUCCESS && ty == REG_DWORD) g_optMerge  = (int)(v != 0);
        if (RegQueryValueExW(k, L"Depth",  0, &ty, (LPBYTE)&v, &sz) == ERROR_SUCCESS && ty == REG_DWORD) g_optDepth  = (int)(v != 0);
        sz = sizeof(v); ty = 0;
        if (RegQueryValueExW(k, L"Wobble", 0, &ty, (LPBYTE)&v, &sz) == ERROR_SUCCESS && ty == REG_DWORD) g_optWobble = (int)(v != 0);
        sz = sizeof(v); ty = 0;
        if (RegQueryValueExW(k, L"Pop",    0, &ty, (LPBYTE)&v, &sz) == ERROR_SUCCESS && ty == REG_DWORD) g_optPop    = (int)(v != 0);
        sz = sizeof(v); ty = 0;
        if (RegQueryValueExW(k, L"Iridescent", 0, &ty, (LPBYTE)&v, &sz) == ERROR_SUCCESS && ty == REG_DWORD) g_optIri   = (int)(v != 0);
        sz = sizeof(v); ty = 0;
        if (RegQueryValueExW(k, L"Bloom", 0, &ty, (LPBYTE)&v, &sz) == ERROR_SUCCESS && ty == REG_DWORD) g_optBloom = (int)(v != 0);
        sz = sizeof(v); ty = 0;
        if (RegQueryValueExW(k, L"Mood",  0, &ty, (LPBYTE)&v, &sz) == ERROR_SUCCESS && ty == REG_DWORD) g_optMood  = (int)(v != 0);
        sz = sizeof(v); ty = 0;
        if (RegQueryValueExW(k, L"Backdrop", 0, &ty, (LPBYTE)&v, &sz) == ERROR_SUCCESS && ty == REG_DWORD) g_optBack  = (int)(v != 0);
        sz = sizeof(v); ty = 0;
        if (RegQueryValueExW(k, L"Droplets", 0, &ty, (LPBYTE)&v, &sz) == ERROR_SUCCESS && ty == REG_DWORD) g_optDrops = (int)(v != 0);
        sz = sizeof(v); ty = 0;
        if (RegQueryValueExW(k, L"Hero",     0, &ty, (LPBYTE)&v, &sz) == ERROR_SUCCESS && ty == REG_DWORD) g_optHero  = (int)(v != 0);
        RegCloseKey(k);
    }
}

static void saveSettings()
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SET_KEY, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return;
    DWORD a = (DWORD)g_optDepth, b = (DWORD)g_optWobble, c = (DWORD)g_optPop;
    DWORD d = (DWORD)g_optIri, e = (DWORD)g_optBloom, f2 = (DWORD)g_optMood;
    DWORD g3 = (DWORD)g_optBack, h3 = (DWORD)g_optDrops, i3 = (DWORD)g_optHero;
    DWORD z9 = (DWORD)g_optSize, z8 = (DWORD)g_optFilm, z7 = (DWORD)g_optMerge;
    RegSetValueExW(k, L"Size",   0, REG_DWORD, (const BYTE*)&z9, sizeof(z9));
    RegSetValueExW(k, L"Film",   0, REG_DWORD, (const BYTE*)&z8, sizeof(z8));
    RegSetValueExW(k, L"Merge",  0, REG_DWORD, (const BYTE*)&z7, sizeof(z7));
    RegSetValueExW(k, L"Depth",  0, REG_DWORD, (const BYTE*)&a, sizeof(a));
    RegSetValueExW(k, L"Wobble", 0, REG_DWORD, (const BYTE*)&b, sizeof(b));
    RegSetValueExW(k, L"Pop",    0, REG_DWORD, (const BYTE*)&c, sizeof(c));
    RegSetValueExW(k, L"Iridescent", 0, REG_DWORD, (const BYTE*)&d, sizeof(d));
    RegSetValueExW(k, L"Bloom",  0, REG_DWORD, (const BYTE*)&e, sizeof(e));
    RegSetValueExW(k, L"Mood",   0, REG_DWORD, (const BYTE*)&f2, sizeof(f2));
    RegSetValueExW(k, L"Backdrop", 0, REG_DWORD, (const BYTE*)&g3, sizeof(g3));
    RegSetValueExW(k, L"Droplets", 0, REG_DWORD, (const BYTE*)&h3, sizeof(h3));
    RegSetValueExW(k, L"Hero",     0, REG_DWORD, (const BYTE*)&i3, sizeof(i3));
    RegCloseKey(k);
}

#define IDC_MERGE  1012
#define IDC_FILM   1011
#define IDC_SIZE   1010
#define IDC_DEPTH  1001
#define IDC_WOBBLE 1002
#define IDC_POP    1003
#define IDC_IRI    1004
#define IDC_BLOOM  1005
#define IDC_MOOD   1006
#define IDC_BACK   1007
#define IDC_DROPS  1008
#define IDC_HERO   1009

static HWND g_setHwnd = nullptr;

static HFONT settingsFont()
{
    static HFONT f = nullptr;
    if (!f) {
        NONCLIENTMETRICSW ncm;
        ncm.cbSize = sizeof(ncm);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
            f = CreateFontIndirectW(&ncm.lfMessageFont);
        if (!f) f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    }
    return f;
}

static HWND addCheck(HWND parent, const wchar_t *text, int id, int x, int y, int w, int h)
{
    HWND c = CreateWindowExW(0, L"BUTTON", text,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                             x, y, w, h, parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)settingsFont(), TRUE);
    return c;
}

static LRESULT CALLBACK settingsProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CREATE: {
        RECT rc; GetClientRect(h, &rc);
        int s = 1;
        typedef UINT (WINAPI *GDW)(HWND);
        HMODULE u = GetModuleHandleW(L"user32.dll");
        if (u) { GDW gdw = (GDW)GetProcAddress(u, "GetDpiForWindow"); if (gdw) s = (int)(gdw(h) / 96u); }
        if (s < 1) s = 1;
        int pad = 16 * s, bh = 22 * s, gap = 8 * s;
        int y = pad;
        HWND sz = addCheck(h, L"Size  (bubbles of random sizes)", IDC_SIZE, pad, y, rc.right - 2 * pad, bh);
        y += bh + gap;
        HWND fm = addCheck(h, L"Film  (marble swirl of soap film inside each bubble)", IDC_FILM, pad, y, rc.right - 2 * pad, bh);
        y += bh + gap;
        HWND mg = addCheck(h, L"Merge  (rarely, two bubbles join into a bigger one)", IDC_MERGE, pad, y, rc.right - 2 * pad, bh);
        y += bh + gap;
        HWND d = addCheck(h, L"Depth  (far layer: smaller, dimmer, drifts slower)", IDC_DEPTH,  pad, y, rc.right - 2 * pad, bh);
        y += bh + gap;
        HWND w = addCheck(h, L"Wobble  (soft breathing and a squash on impact)", IDC_WOBBLE, pad, y, rc.right - 2 * pad, bh);
        y += bh + gap;
        HWND p = addCheck(h, L"Pop  (occasionally one pops and a new one flies in)", IDC_POP, pad, y, rc.right - 2 * pad, bh);
        y += bh + gap;
        HWND ir = addCheck(h, L"Iridescent  (rainbow running around each rim)", IDC_IRI, pad, y, rc.right - 2 * pad, bh);
        y += bh + gap;
        HWND bl = addCheck(h, L"Bloom  (soft halo behind every bubble)", IDC_BLOOM, pad, y, rc.right - 2 * pad, bh);
        y += bh + gap;
        HWND md = addCheck(h, L"Mood  (one drifting palette instead of random hues)", IDC_MOOD, pad, y, rc.right - 2 * pad, bh);
        y += bh + gap;
        HWND bk = addCheck(h, L"Backdrop  (faint teal gradient instead of pure black)", IDC_BACK, pad, y, rc.right - 2 * pad, bh);
        y += bh + gap;
        HWND dp = addCheck(h, L"Droplets  (condensation that clings and slides down)", IDC_DROPS, pad, y, rc.right - 2 * pad, bh);
        y += bh + gap;
        HWND hr = addCheck(h, L"Hero  (a rare giant bubble drifts through)", IDC_HERO, pad, y, rc.right - 2 * pad, bh);
        if (g_optSize)   SendMessageW(sz, BM_SETCHECK, BST_CHECKED, 0);
        if (g_optFilm)   SendMessageW(fm, BM_SETCHECK, BST_CHECKED, 0);
        if (g_optMerge)  SendMessageW(mg, BM_SETCHECK, BST_CHECKED, 0);
        if (g_optDepth)  SendMessageW(d,  BM_SETCHECK, BST_CHECKED, 0);
        if (g_optWobble) SendMessageW(w,  BM_SETCHECK, BST_CHECKED, 0);
        if (g_optPop)    SendMessageW(p,  BM_SETCHECK, BST_CHECKED, 0);
        if (g_optIri)    SendMessageW(ir, BM_SETCHECK, BST_CHECKED, 0);
        if (g_optBloom)  SendMessageW(bl, BM_SETCHECK, BST_CHECKED, 0);
        if (g_optMood)   SendMessageW(md, BM_SETCHECK, BST_CHECKED, 0);
        if (g_optBack)   SendMessageW(bk, BM_SETCHECK, BST_CHECKED, 0);
        if (g_optDrops)  SendMessageW(dp, BM_SETCHECK, BST_CHECKED, 0);
        if (g_optHero)   SendMessageW(hr, BM_SETCHECK, BST_CHECKED, 0);

        int by = y + bh + gap * 2;          // flow, not pinned to the bottom
        int bw = 84 * s;
        HWND ok = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                  rc.right - pad - bw * 2 - gap, by, bw, bh, h, (HMENU)IDOK, nullptr, nullptr);
        HWND cn = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                  rc.right - pad - bw, by, bw, bh, h, (HMENU)IDCANCEL, nullptr, nullptr);
        SendMessageW(ok, WM_SETFONT, (WPARAM)settingsFont(), TRUE);
        SendMessageW(cn, WM_SETFONT, (WPARAM)settingsFont(), TRUE);

        // fit the window to what was actually laid out: every time a toggle was added
        // the list grew and the OK/Cancel row fell off the bottom of a fixed height
        {
            int needH = by + bh + pad;
            RECT ar = { 0, 0, rc.right, needH };
            AdjustWindowRectEx(&ar, (DWORD)GetWindowLongW(h, GWL_STYLE), FALSE,
                               (DWORD)GetWindowLongW(h, GWL_EXSTYLE));
            SetWindowPos(h, nullptr, 0, 0, ar.right - ar.left, ar.bottom - ar.top,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }

        (void)g_setHwnd;
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            g_optSize   = (IsDlgButtonChecked(h, IDC_SIZE)   == BST_CHECKED);
            g_optFilm   = (IsDlgButtonChecked(h, IDC_FILM)   == BST_CHECKED);
            g_optMerge  = (IsDlgButtonChecked(h, IDC_MERGE)  == BST_CHECKED);
            g_optDepth  = (IsDlgButtonChecked(h, IDC_DEPTH)  == BST_CHECKED);
            g_optWobble = (IsDlgButtonChecked(h, IDC_WOBBLE) == BST_CHECKED);
            g_optPop    = (IsDlgButtonChecked(h, IDC_POP)    == BST_CHECKED);
            g_optIri    = (IsDlgButtonChecked(h, IDC_IRI)    == BST_CHECKED);
            g_optBloom  = (IsDlgButtonChecked(h, IDC_BLOOM)  == BST_CHECKED);
            g_optMood   = (IsDlgButtonChecked(h, IDC_MOOD)   == BST_CHECKED);
            g_optBack   = (IsDlgButtonChecked(h, IDC_BACK)   == BST_CHECKED);
            g_optDrops  = (IsDlgButtonChecked(h, IDC_DROPS)  == BST_CHECKED);
            g_optHero   = (IsDlgButtonChecked(h, IDC_HERO)   == BST_CHECKED);
            saveSettings();
            DestroyWindow(h);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) { DestroyWindow(h); return 0; }
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

// headless check of the settings page: same window, same controls, never shown
static int runSettingsTest(const char *outPath)
{
    FILE *f = fopen(outPath, "w");
    if (!f) return 1;

    // the test flips every box and presses OK, which SAVES. Point the save at a
    // scratch key so a self-test can never touch the user's own settings
    SET_KEY = SET_KEY_TEST;
    loadSettings();
    const int keepSize = g_optSize, keepFilm = g_optFilm, keepMerge = g_optMerge;
    const int keepDepth = g_optDepth, keepWob = g_optWobble, keepPop = g_optPop;
    const int keepIri = g_optIri, keepBloom = g_optBloom, keepMood = g_optMood;
    const int keepBack = g_optBack, keepDrops = g_optDrops, keepHero = g_optHero;
    fprintf(f, "loaded: size=%d film=%d merge=%d depth=%d wobble=%d pop=%d iri=%d bloom=%d mood=%d back=%d drops=%d hero=%d\n",
            g_optSize, g_optFilm, g_optMerge, g_optDepth, g_optWobble, g_optPop,
            g_optIri, g_optBloom, g_optMood, g_optBack, g_optDrops, g_optHero);

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = settingsProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"BubblesMultiMonSettingsTest";
    BOOL reg = RegisterClassExW(&wc);
    fprintf(f, "registerclass=%d err=%lu\n", (int)reg, GetLastError());

    HWND hw = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, L"Bubbles (multi-monitor)",
                              WS_CAPTION | WS_SYSMENU,          // deliberately NOT visible
                              0, 0, 430, 220, nullptr, nullptr, wc.hInstance, nullptr);
    fprintf(f, "createwindow=%d err=%lu\n", (int)(hw != nullptr), GetLastError());
    if (!hw) { fclose(f); return 1; }

    struct { int id; const char *name; } items[] = {
        { IDC_DEPTH, "depth" }, { IDC_WOBBLE, "wobble" }, { IDC_POP, "pop" },
        { IDC_IRI, "iri" }, { IDC_BLOOM, "bloom" }, { IDC_MOOD, "mood" },
        { IDC_BACK, "back" }, { IDC_DROPS, "drops" }, { IDC_HERO, "hero" },
        { IDC_SIZE, "size" }, { IDC_FILM, "film" }, { IDC_MERGE, "merge" },
        { IDOK, "ok" }, { IDCANCEL, "cancel" }
    };
    for (int i = 0; i < 14; ++i) {
        HWND c = GetDlgItem(hw, items[i].id);
        fprintf(f, "control %-6s present=%d", items[i].name, (int)(c != nullptr));
        if (c) {
            wchar_t txt[128] = {0};
            GetWindowTextW(c, txt, 128);
            char narrow[256] = {0};
            WideCharToMultiByte(CP_ACP, 0, txt, -1, narrow, 255, nullptr, nullptr);
            fprintf(f, " checked=%d text=\"%s\"", (int)(SendMessageW(c, BM_GETCHECK, 0, 0) == BST_CHECKED), narrow);
        }
        fprintf(f, "\n");
    }

    // geometry: every control, and the buttons, must sit inside the client area
    {
        RECT cr; GetClientRect(hw, &cr);
        fprintf(f, "client=%dx%d\n", (int)(cr.right - cr.left), (int)(cr.bottom - cr.top));
        struct { int id; const char *name; } geo[] = {
            { IDC_SIZE, "size" }, { IDC_FILM, "film" }, { IDC_MERGE, "merge" },
            { IDC_DEPTH, "depth" }, { IDC_WOBBLE, "wobble" }, { IDC_POP, "pop" },
            { IDC_IRI, "iri" }, { IDC_BLOOM, "bloom" }, { IDC_MOOD, "mood" },
            { IDC_BACK, "back" }, { IDC_DROPS, "drops" }, { IDC_HERO, "hero" },
            { IDOK, "ok" }, { IDCANCEL, "cancel" }
        };
        int outside = 0;
        for (int i = 0; i < 14; ++i) {
            HWND c = GetDlgItem(hw, geo[i].id);
            if (!c) { fprintf(f, "geo %-6s MISSING\n", geo[i].name); ++outside; continue; }
            RECT r; GetWindowRect(c, &r);
            MapWindowPoints(HWND_DESKTOP, hw, (POINT*)&r, 2);
            bool in = (r.top >= 0 && r.left >= 0 && r.bottom <= cr.bottom && r.right <= cr.right);
            if (!in) ++outside;
            fprintf(f, "geo %-6s top=%d bottom=%d inside=%d\n",
                    geo[i].name, (int)r.top, (int)r.bottom, (int)in);
        }
        fprintf(f, "geo_outside=%d\n", outside);
    }

    // flip every box and press OK exactly as a user would
    int ids[12] = { IDC_SIZE, IDC_FILM, IDC_MERGE, IDC_DEPTH, IDC_WOBBLE, IDC_POP,
                    IDC_IRI, IDC_BLOOM, IDC_MOOD, IDC_BACK, IDC_DROPS, IDC_HERO };
    for (int i = 0; i < 12; ++i) {
        HWND c = GetDlgItem(hw, ids[i]);
        if (!c) continue;
        bool on = SendMessageW(c, BM_GETCHECK, 0, 0) == BST_CHECKED;
        SendMessageW(c, BM_SETCHECK, on ? BST_UNCHECKED : BST_CHECKED, 0);
    }
    SendMessageW(hw, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)GetDlgItem(hw, IDOK));
    Sleep(50);
    fprintf(f, "after OK: window_alive=%d\n", (int)IsWindow(hw));

    // back to the real key, and reload so this process matches what she actually has
    SET_KEY = SET_KEY_REAL;
    loadSettings();

    // read the registry back through a fresh load
    g_optDepth = g_optWobble = g_optPop = -7;
    loadSettings();
    fprintf(f, "reloaded: size=%d depth=%d wobble=%d pop=%d iri=%d bloom=%d mood=%d back=%d drops=%d hero=%d\n",
            g_optSize,
            g_optDepth, g_optWobble, g_optPop, g_optIri, g_optBloom, g_optMood,
            g_optBack, g_optDrops, g_optHero);
    fclose(f);
    return 0;
}

static int runSettings(HWND parent, HINSTANCE hInst)
{
    loadSettings();

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = settingsProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"BubblesMultiMonSettings";
    wc.hIcon = LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) return 0;

    int s = 1;
    {
        HDC dc = GetDC(nullptr);
        if (dc) { s = (int)(GetDeviceCaps(dc, LOGPIXELSX) / 96.0 + 0.5); ReleaseDC(nullptr, dc); }
    }
    if (s < 1) s = 1;
    int w = 450 * s, h = 358 * s;

    int x = 0, y = 0;
    if (parent) {
        RECT pr; GetWindowRect(parent, &pr);
        x = pr.left + ((pr.right - pr.left) - w) / 2;
        y = pr.top  + ((pr.bottom - pr.top) - h) / 2;
    } else {
        RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        x = wa.left + ((wa.right - wa.left) - w) / 2;
        y = wa.top  + ((wa.bottom - wa.top) - h) / 2;
    }

    HWND hw = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName,
                              L"Bubbles (multi-monitor)",
                              WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                              x, y, w, h, parent, nullptr, hInst, nullptr);
    if (!hw) return 0;
    g_setHwnd = hw;

    if (parent) EnableWindow(parent, FALSE);
    MSG msg;
    while (IsWindow(hw) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(hw, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    if (parent) EnableWindow(parent, TRUE);
    g_setHwnd = nullptr;
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR cmdLine, int)
{
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    g_qpcFreq = (double)fq.QuadPart;

    // DPI awareness: work in real pixels
    typedef BOOL (WINAPI *SetCtxFn)(HANDLE);
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        SetCtxFn setCtx = (SetCtxFn)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (setCtx) setCtx((HANDLE)-4);            // PER_MONITOR_AWARE_V2
        else SetProcessDPIAware();
    }

    loadSettings();

    // sprite resource
    HRSRC rs = FindResourceW(hInst, MAKEINTRESOURCEW(SPRITE_RES), RT_RCDATA);
    if (!rs) return 1;
    HGLOBAL rg = LoadResource(hInst, rs);
    unsigned char *data = (unsigned char*)LockResource(rg);
    DWORD sz = SizeofResource(hInst, rs);
    if (!data || sz < (DWORD)(SPR * SPR * 2)) return 1;
    g_rIMask = data;
    g_wMask  = data + SPR * SPR;
    buildAtlas();
    buildSectors();
    buildBloomMask();
    buildSoftMask();
    buildFilms();
    buildProfile();
    buildBead();
    buildCutMasks();

    // parse command line: /s /p <hwnd> /c /t <dir> <seconds>
    std::wstring cl = cmdLine ? cmdLine : L"";
    wchar_t op = 0;
    std::wstring arg;
    {
        size_t i = 0;
        while (i < cl.size() && cl[i] == L' ') ++i;
        if (i < cl.size() && (cl[i] == L'/' || cl[i] == L'-')) {
            op = cl[i + 1];
            size_t j = i + 2;
            while (j < cl.size() && (cl[j] == L':' || cl[j] == L' ')) ++j;
            arg = cl.substr(j);
        }
    }
    // A bare .scr means "show the settings page", which is the Windows convention. A
    // bare play build means "open the window", so the name decides.
    if (op == L'w' || op == L'W') g_appMode = true;
    if (op == 0) {
        wchar_t exe[MAX_PATH] = {0};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring e = exe;
        for (size_t k = 0; k < e.size(); ++k)
            e[k] = (wchar_t)towlower(e[k]);
        if (e.find(L"play") != std::wstring::npos) g_appMode = true;
    }
    HWND parent = nullptr;
    if (op == L'p' || op == L'P' || op == L'c' || op == L'C') {
        parent = (HWND)(LONG_PTR)_wtoi64(arg.c_str());
        g_preview = (op == L'p' || op == L'P');
    }

    if (op == L'g' || op == L'G') {                  // glow grid test
        std::string path(arg.begin(), arg.end());
        return runGridTest(path.c_str());
    }

    if (op == L'c' && (arg[0] == L't' || arg[0] == L'T') && arg[1] == L' ') {   // settings self-test
        std::string path(arg.begin() + 2, arg.end());
        return runSettingsTest(path.c_str());
    }

    if (op == L't' || op == L'T') {                 // self-test / capture mode
        // "arg" = "<dir> <seconds>"
        std::wstring dir = arg, secs;
        size_t sp = arg.find(L' ');
        if (sp != std::wstring::npos) { dir = arg.substr(0, sp); secs = arg.substr(sp + 1); }
        refreshMonitors();
        { char sb[32]; if (GetEnvironmentVariableA("BUBBLES_TEST_SCALE", sb, 32) > 0) g_testScale = (float)atof(sb); }
        g_scale = g_testScale;
        g_dib.create(g_virt.right - g_virt.left, g_virt.bottom - g_virt.top);
        buildBackdrop(g_dib.w, g_dib.h);
        fillSurface();
        return runTest(std::string(dir.begin(), dir.end()).c_str(),
                       secs.empty() ? 8.0f : (float)_wtof(secs.c_str()));
    }

    if (g_preview && !parent) return 0;
    if (g_appMode) {
        // first run: start from whatever the screensaver is set to, then diverge
        HKEY probe;
        bool have = (RegOpenKeyExW(HKEY_CURRENT_USER, SET_KEY_PLAY, 0, KEY_READ, &probe) == ERROR_SUCCESS);
        if (have) RegCloseKey(probe);
        if (!have) { loadSettings(); SET_KEY = SET_KEY_PLAY; saveSettings(); }
        SET_KEY = SET_KEY_PLAY;
        loadSettings();
    }

    if ((op == 0 || op == L'c' || op == L'C') && !g_appMode)
        return runSettings(parent, hInst);          // settings page

    refreshMonitors();

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hInst;
    wc.hCursor = nullptr;
    wc.lpszClassName = L"BubblesMultiMonWnd";
    wc.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassExW(&wc);

    HWND hwnd;
    if (g_preview) {
        RECT pr; GetClientRect(parent, &pr);
        int w = pr.right - pr.left, h = pr.bottom - pr.top;
        if (w < 4 || h < 4) return 0;
        g_virt.left = 0; g_virt.top = 0; g_virt.right = w; g_virt.bottom = h;
        g_mons.clear();
        RECT r = {0, 0, w, h};
        g_mons.push_back(r);
        g_scale = (h < 1080) ? (h / 1080.0f) : 1.0f;
        if (g_scale < 0.10f) g_scale = 0.10f;
        hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_CHILD | WS_VISIBLE,
                               0, 0, w, h, parent, nullptr, hInst, nullptr);
    } else if (g_appMode) {
        // a normal window. Fixed size for now: the renderer has no device-reset path,
        // so a resize would need the swapchain rebuilt as well.
        int cw = 1280, ch = 800;
        RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        int aw = wa.right - wa.left, ah = wa.bottom - wa.top;
        if (cw > aw - 60) cw = aw - 60;
        if (ch > ah - 60) ch = ah - 60;
        RECT wr = { 0, 0, cw, ch };
        DWORD style = WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
        AdjustWindowRectEx(&wr, style, TRUE, 0);
        int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
        int wx = wa.left + (aw - ww) / 2, wy = wa.top + (ah - wh) / 2;

        g_virt.left = 0; g_virt.top = 0; g_virt.right = cw; g_virt.bottom = ch;
        g_mons.clear();
        RECT mr = { 0, 0, cw, ch };
        g_mons.push_back(mr);
        g_dead.clear();
        g_scale = (ch < 1080) ? (ch / 1080.0f) : 1.0f;
        if (g_scale < 0.10f) g_scale = 0.10f;

        HMENU mb = CreateMenu();
        HMENU mm = CreatePopupMenu();
        AppendMenuW(mm, MF_STRING, APP_SETTINGS_ID, L"&Settings...\tF2");
        AppendMenuW(mm, MF_STRING, APP_RESET_ID,    L"&Reset field");
        AppendMenuW(mm, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(mm, MF_STRING, APP_EXIT_ID,     L"E&xit");
        AppendMenuW(mb, MF_POPUP, (UINT_PTR)mm, L"&Bubbles");

        hwnd = CreateWindowExW(0, wc.lpszClassName, L"Bubbles",
                               style | WS_VISIBLE, wx, wy, ww, wh,
                               nullptr, mb, hInst, nullptr);
    } else {
        g_scale = 1.0f;
        int w = g_virt.right - g_virt.left, h = g_virt.bottom - g_virt.top;
        hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"",
                               WS_POPUP | WS_VISIBLE,
                               g_virt.left, g_virt.top, w, h, nullptr, nullptr, hInst, nullptr);
    }
    if (!hwnd) return 1;
    g_hwnd = hwnd;
    g_dib.create(g_virt.right - g_virt.left, g_virt.bottom - g_virt.top);
    buildBackdrop(g_dib.w, g_dib.h);
    fillSurface();
    if (!g_preview) initD3D(hwnd, g_virt.right - g_virt.left, g_virt.bottom - g_virt.top);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    if (g_appMode) {
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
    } else if (!g_preview) {
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
        ShowCursor(FALSE);
        SetCursor(nullptr);
        GetCursorPos(&g_mouse0);
        g_haveMouse = true;
        SetCapture(hwnd);
    }

    if (g_appMode) appFill();                    // a full field to play with
    else initBubbles(6, 4.0f);                   // otherwise they stream in from the corner

    timeBeginPeriod(1);
    double last = nowSec();
    g_start = last;
    const double frameTime = 1.0 / 60.0;
    MSG msg;

    while (g_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_running) break;

        if (!g_preview && !g_appMode && g_haveMouse) {
            POINT p; GetCursorPos(&p);
            int dx = p.x - g_mouse0.x, dy = p.y - g_mouse0.y;
            if (dx * dx + dy * dy > 100) { g_running = false; break; }
        }

        double t = nowSec();
        float dt = (float)(t - last);
        last = t;
        if (g_frames > 0) {
            float ms = (float)(dt * 1000.0);
            if (g_frames < (int)(sizeof(g_iv) / sizeof(g_iv[0]))) g_iv[g_frames] = ms;
            double el = t - g_start;
            if (el < 20.0 && g_nRamp  < 1200) g_ivRamp[g_nRamp++]     = ms;   // still filling
            else if (el >= 20.0 && g_nSteady < 1200) g_ivSteady[g_nSteady++] = ms;
        }

        update(dt);
        if (g_useD3D) {
            d3dPresent();
        } else {
            drawBubbles();
            if (!g_wdc) g_wdc = GetDC(hwnd);
            if (!g_dirty.empty()) {
                RECT u = g_dirty[0];
                if (g_preview) { u = g_virt; }        // the pane has no owner to repaint it
                for (size_t i = 1; i < g_dirty.size(); ++i) {
                    if (g_dirty[i].left   < u.left)   u.left   = g_dirty[i].left;
                    if (g_dirty[i].top    < u.top)    u.top    = g_dirty[i].top;
                    if (g_dirty[i].right  > u.right)  u.right  = g_dirty[i].right;
                    if (g_dirty[i].bottom > u.bottom) u.bottom = g_dirty[i].bottom;
                }
                if (u.left   < g_virt.left)   u.left   = g_virt.left;
                if (u.top    < g_virt.top)    u.top    = g_virt.top;
                if (u.right  > g_virt.right)  u.right  = g_virt.right;
                if (u.bottom > g_virt.bottom) u.bottom = g_virt.bottom;
                if (u.right > u.left && u.bottom > u.top)
                    BitBlt(g_wdc, u.left - g_virt.left, u.top - g_virt.top,
                           u.right - u.left, u.bottom - u.top,
                           g_dib.dc, u.left - g_virt.left, u.top - g_virt.top, SRCCOPY);
            }
        }

        ++g_frames;
        g_busy += nowSec() - t;
        {   // optional dump of what the GDI path is compositing
            static bool dumped = false;
            static char dpath[MAX_PATH] = {0};
            if (!dumped && GetEnvironmentVariableA("BUBBLES_DUMP_DIB", dpath, MAX_PATH) > 0
                && t - g_start > 3.0) {
                dumped = true;
                saveBMP(dpath, g_dib.px, g_dib.w, g_dib.h);
                std::string tf = std::string(dpath) + ".txt";
                FILE *df = fopen(tf.c_str(), "w");
                if (df) {
                    fprintf(df, "preview=%d scale=%.3f virt=%ld,%ld,%ld,%ld dib=%dx%d bubbles=%d d3d=%d\n",
                            (int)g_preview, g_scale, (long)g_virt.left, (long)g_virt.top,
                            (long)g_virt.right, (long)g_virt.bottom, g_dib.w, g_dib.h,
                            (int)g_bub.size(), (int)g_useD3D);
                    fclose(df);
                }
            }
        }

        // pace to an absolute deadline: relative sleeps drift and judder
        double now = nowSec();
        if (g_nextFrame == 0.0 || g_nextFrame < now - 0.05) g_nextFrame = now;
        g_nextFrame += frameTime;
        double wait = g_nextFrame - nowSec();
        if (wait > 0.002) Sleep((DWORD)((wait - 0.0015) * 1000.0));
        while (nowSec() < g_nextFrame) { YieldProcessor(); }
    }

    if (!g_preview) {
        ReleaseCapture();
        ShowCursor(TRUE);
    }
    if (g_wdc) { ReleaseDC(hwnd, g_wdc); g_wdc = nullptr; }
    if (g_frames > 10) {
        double total = nowSec() - g_start;
        int n = g_frames < (int)(sizeof(g_iv) / sizeof(g_iv[0])) ? g_frames : (int)(sizeof(g_iv) / sizeof(g_iv[0]));
        std::vector<float> iv;
        for (int i = 2; i < n; ++i) iv.push_back(g_iv[i]);
        float p50 = 0, p95 = 0, mx = 0, sd = 0;
        if (!iv.empty()) {
            std::sort(iv.begin(), iv.end());
            p50 = iv[iv.size() / 2];
            p95 = iv[(size_t)(iv.size() * 0.95)];
            mx  = iv.back();
            float mean = 0; for (size_t i = 0; i < iv.size(); ++i) mean += iv[i];
            mean /= iv.size();
            for (size_t i = 0; i < iv.size(); ++i) sd += (iv[i] - mean) * (iv[i] - mean);
            sd = sqrtf(sd / iv.size());
        }
        char buf[MAX_PATH];
        if (GetEnvironmentVariableA("BUBBLES_FPS_LOG", buf, MAX_PATH) > 0) {
            FILE *fl = fopen(buf, "w");
            if (fl) {
                fprintf(fl, "renderer=%s frames=%d seconds=%.2f fps=%.1f\n",
                        g_useD3D ? "d3d9" : "gdi", g_frames, total, g_frames / total);
                fprintf(fl, "frame_interval_ms p50=%.2f p95=%.2f max=%.2f sd=%.2f\n", p50, p95, mx, sd);
                for (int pass = 0; pass < 2; ++pass) {
                    float *arr = pass ? g_ivSteady : g_ivRamp;
                    int n2 = pass ? g_nSteady : g_nRamp;
                    if (n2 < 10) continue;
                    std::vector<float> v2(arr, arr + n2);
                    std::sort(v2.begin(), v2.end());
                    float mn2 = 0; for (int i = 0; i < n2; ++i) mn2 += v2[i]; mn2 /= n2;
                    float sd2 = 0; for (int i = 0; i < n2; ++i) sd2 += (v2[i] - mn2) * (v2[i] - mn2);
                    sd2 = sqrtf(sd2 / n2);
                    fprintf(fl, "%s: n=%d p50=%.2f p95=%.2f max=%.2f sd=%.2f\n",
                            pass ? "settled" : "spawn_ramp", n2, v2[n2 / 2], v2[(int)(n2 * 0.95)], v2[n2 - 1], sd2);
                }
                fprintf(fl, "merge_times:");
                for (int i = 0; i < g_mergeAtN; ++i) fprintf(fl, " %.2f@%.0f,%.0f", g_mergeAt[i], g_mergePX[i], g_mergePY[i]);
                fprintf(fl, "\n");
                fprintf(fl, "cpu_ms_per_frame=%.2f bubbles=%d\n",
                        1000.0 * g_busy / g_frames, (int)g_bub.size());
                fclose(fl);
            }
        }
    }
    timeEndPeriod(1);
    g_dib.destroy();
    return 0;
}
