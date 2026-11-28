// postfx_hook.c - plugs the post-processing chain (postfx_core.c) into Condor 3.
//
// How a Condor frame is organised (found with REA/Ghidra, addresses are IDA/Ghidra addresses, base 0x400000):
//
//   0x6D6BB4  frame function           for every window: bind its buffers (0x77DB00), scene (0x73B59C),
//                                      HUD (0x6E7404), Present (0x77DA20)
//   0x73B59C  scene                    land, sky, airports, shadows, trees, clouds, glider, sun, flares
//   0x6E7404  HUD                      <- we run just before this: the scene is complete, the HUD is not
//                                         drawn yet, so text and instruments are never post-processed
//
//   D3D11 objects are plain globals:   0x7DA0EC ID3D11Device*   0x7DA0F4 ID3D11DeviceContext*
//   swap chain manager:                *(*(0x793998)) -> +8 current index, +0xC array of items
//   item (one per swap chain):         +0x14 IDXGISwapChain  +0x18 RTV  +0x1C depth texture (R24G8_TYPELESS,
//                                      DSV + SRV bind)  +0x20 backbuffer texture (multisampled when MSAA is
//                                      on)  +0x24 DSV  +0x28 depth SRV (multisampled)
//   camera manager:                    *(0x7D96F8); D3DX projection matrix at +0x188 (view at +0x108)
//
// Hotkeys (while the game window has focus):  F8 reload postfx.hlsl   F9 post-processing on/off
//                                              F10 write postfx\capture.bin (frame for offline tuning)
#define COBJMACROS
#include "install_patch.h"
#include "dobby.h"
#include "debug_log.h"
#include "postfx_core.h"

#include <d3d11.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define ADDR_HUD_DRAW     0x006E7404
#define ADDR_DEVICE_VAR   0x007DA0EC
#define ADDR_CONTEXT_VAR  0x007DA0F4
#define ADDR_SWAPMGR_PTR  0x00793998
#define ADDR_CAMMGR_VAR   0x007D96F8

#define MGR_OFF_INDEX     0x08
#define MGR_OFF_ITEMS     0x0C
#define ITEM_OFF_COLOR    0x20
#define ITEM_OFF_DEPTH    0x1C
#define ITEM_OFF_RTV      0x18
#define CAM_OFF_PROJ      0x188

static PfxCore* g_core = NULL;
static char     g_shot[MAX_PATH];     // pending 'shot' request (capture path), consumed by the frame hook
static int      g_enabled = 1;
static int      g_dead = 0;           // gave up (fatal problem)
static int      g_fails = 0;
static char     g_dir[MAX_PATH];      // <exedir>\postfx
static unsigned g_frames = 0;
static FILE*    g_logf = NULL;

// ---- world depth capture -------------------------------------------------------------------------------
// The engine clears the depth buffer between the world and the cockpit, so at the HUD hook only the cockpit is
// left. We patch ID3D11DeviceContext::ClearDepthStencilView (vtable slot 53) and copy the depth buffer just before
// every clear of the main depth texture; the last copy of a frame is the world (see PfxCore_Run: max(depth, world)).
#define VT_CLEAR_DSV 53
typedef void (STDMETHODCALLTYPE *ClearDsvFn)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
static ClearDsvFn g_origClear = NULL;
static ID3D11Texture2D* g_mainDepth = NULL;   // watched (set by the HUD hook, not owned)
static ID3D11Texture2D* g_worldCopy = NULL;   // owned
static unsigned g_clearsThisFrame = 0, g_copiesThisFrame = 0, g_worldValid = 0;
static int g_clearLogN = 0;
static unsigned g_clearCalls = 0;          // every call that reaches ClearDsvHook (diagnostics)
static void* g_clearTarget = NULL;         // function address that was in vtable slot 53 when we hooked

static int DeviceCheck(ID3D11Device* dev);
static void STDMETHODCALLTYPE ClearDsvHook(ID3D11DeviceContext* self, ID3D11DepthStencilView* dsv, UINT flags, FLOAT d, UINT8 st) {
    g_clearCalls++;
    if (dsv && g_mainDepth && g_enabled) {
        ID3D11Resource* res = NULL;
        ID3D11DepthStencilView_GetResource(dsv, &res);
        if (res) {
            int isMain = ((void*)res == (void*)g_mainDepth);
            if (g_logf && g_clearLogN < 60) {
                g_clearLogN++;
                fprintf(g_logf, "postfx: frame %u clear dsv res=%p main=%d flags=%u depth=%g\n", g_frames, (void*)res, isMain, flags, d);
                fflush(g_logf);
            }
            if (isMain && (flags & D3D11_CLEAR_DEPTH)) {
                D3D11_TEXTURE2D_DESC md, wd;
                ID3D11Device* dev = NULL;
                int changed = 0;
                ID3D11DeviceContext_GetDevice(self, &dev);               // a new flight's first clears come before the HUD hook
                if (dev) { changed = DeviceCheck(dev); ID3D11Device_Release(dev); dev = NULL; }
                if (changed) { ID3D11Resource_Release(res); g_origClear(self, dsv, flags, d, st); return; }   // g_mainDepth was the old flight's
                g_clearsThisFrame++;
                ID3D11Texture2D_GetDesc(g_mainDepth, &md);
                if (g_worldCopy) { ID3D11Texture2D_GetDesc(g_worldCopy, &wd); if (wd.Width != md.Width || wd.Height != md.Height || wd.SampleDesc.Count != md.SampleDesc.Count || wd.Format != md.Format) { ID3D11Texture2D_Release(g_worldCopy); g_worldCopy = NULL; } }
                if (!g_worldCopy) {
                    ID3D11DeviceContext_GetDevice(self, &dev);
                    if (dev) {
                        md.BindFlags = D3D11_BIND_SHADER_RESOURCE; md.Usage = D3D11_USAGE_DEFAULT; md.CPUAccessFlags = 0; md.MiscFlags = 0;
                        if (FAILED(ID3D11Device_CreateTexture2D(dev, &md, NULL, &g_worldCopy)) && g_logf) { fprintf(g_logf, "postfx: world depth copy creation failed\n"); fflush(g_logf); }
                        ID3D11Device_Release(dev);
                    }
                }
                if (g_worldCopy) { ID3D11DeviceContext_CopyResource(self, (ID3D11Resource*)g_worldCopy, res); g_copiesThisFrame++; g_worldValid = 1; }
            }
            ID3D11Resource_Release(res);
        }
    }
    g_origClear(self, dsv, flags, d, st);
}


// ---- engine atmosphere constants -----------------------------------------------------------------------------
// The engine uploads the terrain effect's per-frame constant buffer (416 bytes) with UpdateSubresource every frame.
// Layout (cbChangesEveryFrame of EffectLand): View 0, Projection 64, 3x ViewProjLight 128..320, CameraPos 320,
// FogColor 336, SunPos 352, SunColor 368, AmbientColor 384, Time 400. We copy the interesting parts.
typedef struct { float view[16]; float camPos[3]; float fog[4]; float sun[3]; float sunColor[4]; float amb[4]; unsigned frame; int valid; } AtmoData;
static AtmoData g_atmo;
// ---- terrain effect access through Effects11 ---------------------------------------------------------------------
// The engine builds every effect with D3DX11CreateEffectFromMemory from d3dx11Effects_JSB.dll (plain Effects11 layout,
// verified against the engine's own call sites). We hook that export, recognise EffectLand by its variable ModP0 and then
//   (a) set the live parameters ModP0..4 (shadow strength, hemisphere ambient, sun gain, ambient gain, cloud shadow) and
//   (b) read View / CameraPos / FogColor / SunPos / SunColor / AmbientColor back from the effect's CPU-side copy.
// No D3D11 context hooks are involved: on Windows the runtime rewrites the context's method table while the game runs,
// so vtable hooks on UpdateSubresource / Map / ClearDepthStencilView stop working after the first frames.
#define FX_VT(obj, idx) (((void**)(*(void***)(obj)))[idx])
typedef unsigned char (__stdcall *FxIsValidFn)(void*);
typedef void*         (__stdcall *FxGetByNameFn)(void*, const char*);
typedef void*         (__stdcall *FxAsFn)(void*);
typedef long          (__stdcall *FxSetFloatFn)(void*, float);
typedef long          (__stdcall *FxGetFloatFn)(void*, float*);
typedef long          (__stdcall *FxSetPtrFn)(void*, const float*);
typedef long          (__stdcall *FxGetPtrFn)(void*, float*);
typedef unsigned long (__stdcall *FxRefFn)(void*);
enum {
    FXE_GETVARIABLEBYNAME = 9,                                    // ID3DX11Effect
    FXV_ISVALID = 0, FXV_ASSCALAR = 10, FXV_ASVECTOR = 11, FXV_ASMATRIX = 12,   // ID3DX11EffectVariable
    FXS_SETFLOAT = 28, FXS_GETFLOAT = 29,                         // ID3DX11EffectScalarVariable
    FXVEC_SETFLOATVECTOR = 30, FXVEC_GETFLOATVECTOR = 33,         // ID3DX11EffectVectorVariable
    FXM_SETMATRIX = 28, FXM_GETMATRIX = 29                        // ID3DX11EffectMatrixVariable
};
typedef struct { void* effect; void* modp[5]; void* view; void* camPos; void* fog; void* sunPos; void* sunColor; void* amb; int selfTest; } FxLand;
static FxLand g_fx;
static int    g_fxHookState = 0;     // 0 = not installed yet, 1 = installed, <0 = failed
typedef HRESULT (__stdcall *CreateFxFn)(const void*, SIZE_T, UINT, void*, void**);
static CreateFxFn g_origCreateFx = NULL;

static void* FxVar(void* effect, const char* name, int asIdx) {
    void* v = ((FxGetByNameFn)FX_VT(effect, FXE_GETVARIABLEBYNAME))(effect, name);
    void* r;
    if (!v || !((FxIsValidFn)FX_VT(v, FXV_ISVALID))(v)) return NULL;
    if (!asIdx) return v;
    r = ((FxAsFn)FX_VT(v, asIdx))(v);
    if (!r || !((FxIsValidFn)FX_VT(r, FXV_ISVALID))(r)) return NULL;
    return r;
}

// Round trip through the Set/Get slots we rely on: proves the vtable offsets before anything else uses them.
static int FxSelfTest(const FxLand* n) {
    float a = 0.4321f, b = -1.0f, vin[4] = { 0.25f, 0.5f, 0.75f, 1.0f }, vout[4] = { 0, 0, 0, 0 }, min[16], mout[16];
    int i, ok = 1;
    ((FxSetFloatFn)FX_VT(n->modp[0], FXS_SETFLOAT))(n->modp[0], a);
    ((FxGetFloatFn)FX_VT(n->modp[0], FXS_GETFLOAT))(n->modp[0], &b);
    if (fabsf(a - b) > 1e-6f) ok = 0;
    if (n->fog) {
        ((FxSetPtrFn)FX_VT(n->fog, FXVEC_SETFLOATVECTOR))(n->fog, vin);
        ((FxGetPtrFn)FX_VT(n->fog, FXVEC_GETFLOATVECTOR))(n->fog, vout);
        for (i = 0; i < 4; i++) if (fabsf(vin[i] - vout[i]) > 1e-6f) ok = 0;
    }
    if (n->view) {
        for (i = 0; i < 16; i++) { min[i] = (float)(i + 1); mout[i] = 0; }
        ((FxSetPtrFn)FX_VT(n->view, FXM_SETMATRIX))(n->view, min);
        ((FxGetPtrFn)FX_VT(n->view, FXM_GETMATRIX))(n->view, mout);
        for (i = 0; i < 16; i++) if (fabsf(min[i] - mout[i]) > 1e-6f) ok = 0;
    }
    return ok;
}

static void FxCapture(void* effect) {
    FxLand n; int i; char nm[8]; void* old;
    if (!effect) return;
    memset(&n, 0, sizeof n);
    n.modp[0] = FxVar(effect, "ModP0", FXV_ASSCALAR);
    if (!n.modp[0]) return;                                       // not the terrain effect
    for (i = 1; i < 5; i++) { _snprintf(nm, sizeof nm, "ModP%d", i); n.modp[i] = FxVar(effect, nm, FXV_ASSCALAR); }
    n.view = FxVar(effect, "View", FXV_ASMATRIX);
    n.camPos = FxVar(effect, "CameraPos", FXV_ASVECTOR);
    n.fog = FxVar(effect, "FogColor", FXV_ASVECTOR);
    n.sunPos = FxVar(effect, "SunPos", FXV_ASVECTOR);
    n.sunColor = FxVar(effect, "SunColor", FXV_ASVECTOR);
    n.amb = FxVar(effect, "AmbientColor", FXV_ASVECTOR);
    n.selfTest = (FxVar(effect, "ZzNoSuchVariable", 0) == NULL) && FxSelfTest(&n);
    if (g_logf) {
        fprintf(g_logf, "postfx: terrain effect %p: ModP %p %p %p %p %p view %p camPos %p fog %p sunPos %p sunColor %p amb %p selfTest %d\n",
                effect, n.modp[0], n.modp[1], n.modp[2], n.modp[3], n.modp[4], n.view, n.camPos, n.fog, n.sunPos, n.sunColor, n.amb, n.selfTest);
        fflush(g_logf);
    }
    if (!n.selfTest) return;                                      // unexpected vtable layout: leave the effect alone
    ((FxRefFn)FX_VT(effect, 1))(effect);                          // AddRef: keep the effect (and our variable pointers) alive
    n.effect = effect;
    old = g_fx.effect;
    g_fx = n;
    if (old) ((FxRefFn)FX_VT(old, 2))(old);                       // Release the previous one
}

static HRESULT __stdcall CreateFxHook(const void* data, SIZE_T len, UINT flags, void* dev, void** ppEffect) {
    HRESULT hr = g_origCreateFx(data, len, flags, dev, ppEffect);
    if (SUCCEEDED(hr) && ppEffect && *ppEffect) FxCapture(*ppEffect);
    return hr;
}

static int InstallFxHook(void) {
    HMODULE h = GetModuleHandleA("d3dx11Effects_JSB.dll");
    void* p;
    if (!h) return 0;
    p = (void*)GetProcAddress(h, "D3DX11CreateEffectFromMemory");
    if (!p) return -1;
    return DobbyHook(p, (dobby_dummy_func_t)CreateFxHook, (dobby_dummy_func_t*)&g_origCreateFx) == 0 ? 1 : -2;
}

// once per frame (HudPre): push the live parameters into the terrain effect and read the atmosphere back
static void FxFrame(void) {
    static const int   slot[5] = { 40, 41, 42, 43, 44 };
    static const float def[5]  = { 0.75f, 0.80f, 1.20f, 0.60f, 0.75f };
    void* e = g_fx.effect;
    float m[16], c[4], sp[4], sc[4], am[4], fg[4], l;
    int i, j;
    if (!e) return;
    for (i = 0; i < 5; i++) if (g_fx.modp[i]) ((FxSetFloatFn)FX_VT(g_fx.modp[i], FXS_SETFLOAT))(g_fx.modp[i], PfxCore_GetTune(g_core, slot[i], def[i]));
    if (!g_fx.view || !g_fx.camPos || !g_fx.fog || !g_fx.sunPos || !g_fx.sunColor || !g_fx.amb) return;
    memset(m, 0, sizeof m); memset(c, 0, sizeof c); memset(sp, 0, sizeof sp); memset(sc, 0, sizeof sc); memset(am, 0, sizeof am); memset(fg, 0, sizeof fg);
    ((FxGetPtrFn)FX_VT(g_fx.view, FXM_GETMATRIX))(g_fx.view, m);
    ((FxGetPtrFn)FX_VT(g_fx.camPos, FXVEC_GETFLOATVECTOR))(g_fx.camPos, c);
    ((FxGetPtrFn)FX_VT(g_fx.fog, FXVEC_GETFLOATVECTOR))(g_fx.fog, fg);
    ((FxGetPtrFn)FX_VT(g_fx.sunPos, FXVEC_GETFLOATVECTOR))(g_fx.sunPos, sp);
    ((FxGetPtrFn)FX_VT(g_fx.sunColor, FXVEC_GETFLOATVECTOR))(g_fx.sunColor, sc);
    ((FxGetPtrFn)FX_VT(g_fx.amb, FXVEC_GETFLOATVECTOR))(g_fx.amb, am);
    l = sqrtf(sp[0] * sp[0] + sp[1] * sp[1] + sp[2] * sp[2]);
    if (fabsf(l - 1.0f) >= 0.02f) return;                         // the engine has not filled the per-frame values yet
    // GetMatrix returns the application (row-major) matrix; the shader constant buffer holds its transpose
    for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) g_atmo.view[i * 4 + j] = m[j * 4 + i];
    memcpy(g_atmo.camPos, c, 12); memcpy(g_atmo.fog, fg, 16); memcpy(g_atmo.sun, sp, 12);
    memcpy(g_atmo.sunColor, sc, 16); memcpy(g_atmo.amb, am, 16);
    g_atmo.frame = g_frames; g_atmo.valid = 1;
    {
        static int once = 0;
        if (!once && g_logf) {
            once = 1;
            fprintf(g_logf, "postfx: atmosphere from effect: view[12..15] %.3f %.3f %.3f %.3f (expect 0 0 0 1) sunDir %.3f %.3f %.3f camPos %.1f %.1f %.1f\n",
                    g_atmo.view[12], g_atmo.view[13], g_atmo.view[14], g_atmo.view[15], sp[0], sp[1], sp[2], c[0], c[1], c[2]);
            fflush(g_logf);
        }
    }
}

// On Windows on ARM (x86 emulation, e.g. Windows 11 ARM in Parallels) the inline detour on ClearDepthStencilView crashed with
// C000001D in the first frame. There the vtable patch (the path used under Wine) is used instead.
// POSTFX_FORCE_VTABLE: 0 = auto (vtable only on an ARM64 host), 1 = always vtable, -1 = never (always inline detour on native Windows).
#ifndef POSTFX_FORCE_VTABLE
#define POSTFX_FORCE_VTABLE 0
#endif
static int IsWine(void);
static int IsArm64Host(void) {
    typedef BOOL (WINAPI *IsWow64Process2_t)(HANDLE, USHORT*, USHORT*);
    HMODULE k = GetModuleHandleA("kernel32.dll");
    IsWow64Process2_t f = k ? (IsWow64Process2_t)GetProcAddress(k, "IsWow64Process2") : NULL;
    USHORT proc = 0, native = 0;
    if (!f || !f(GetCurrentProcess(), &proc, &native)) return 0;   // older Windows: not ARM
    return native == 0xAA64;                                          // IMAGE_FILE_MACHINE_ARM64
}
static int UseVtablePatch(void) {
    if (POSTFX_FORCE_VTABLE > 0) return 1;
    if (POSTFX_FORCE_VTABLE < 0) return IsWine();
    return IsWine() || IsArm64Host();
}
static int g_clearHooked = 0;
static int IsWine(void) {
    HMODULE n = GetModuleHandleA("ntdll.dll");
    return n && GetProcAddress(n, "wine_get_version") != NULL;
}
static void HookClear(ID3D11DeviceContext* ctx) {
    void** vt; DWORD old; int inl = 0;
    if (g_clearHooked || !ctx) return;
    g_clearHooked = 1;
    vt = *(void***)ctx;
    g_clearTarget = vt[VT_CLEAR_DSV];
    if (!UseVtablePatch()) {
        // native Windows: the D3D11 runtime rewrites the context's method table while the game runs, so a vtable patch
        // only survives the first frames. Detour the function itself instead.
        int r;
        g_origClear = NULL;
        r = DobbyHook(g_clearTarget, (dobby_dummy_func_t)ClearDsvHook, (dobby_dummy_func_t*)&g_origClear);
        inl = (r == 0 && g_origClear != NULL);
        if (g_logf) { fprintf(g_logf, "postfx: ClearDepthStencilView inline hook %s (target %p, trampoline %p, vtable %p, r=%d)\n", inl ? "OK" : "FAILED", g_clearTarget, (void*)g_origClear, (void*)vt, r); fflush(g_logf); }
    }
    if (!inl) {
        if (!VirtualProtect(&vt[VT_CLEAR_DSV], sizeof(void*), PAGE_READWRITE, &old)) { if (g_logf) { fprintf(g_logf, "postfx: VirtualProtect failed\n"); fflush(g_logf); } return; }
        g_origClear = (ClearDsvFn)vt[VT_CLEAR_DSV];
        vt[VT_CLEAR_DSV] = (void*)ClearDsvHook;
        VirtualProtect(&vt[VT_CLEAR_DSV], sizeof(void*), old, &old);
        if (g_logf) { fprintf(g_logf, "postfx: ClearDepthStencilView hooked via vtable (vtable %p, orig %p)\n", (void*)vt, (void*)g_origClear); fflush(g_logf); }
    }
}
// current content of vtable slot 53 (diagnostics: does the runtime put the same function back?)
static void* CurClearEntry(uint32_t ctx) {
    void** vt;
    if (!ctx || IsBadReadPtr((void*)(uintptr_t)ctx, 4)) return NULL;
    vt = *(void***)(uintptr_t)ctx;
    if (!vt || IsBadReadPtr(&vt[VT_CLEAR_DSV], 4)) return NULL;
    return vt[VT_CLEAR_DSV];
}


static void Diag(const char* fmt, ...) {
    static int n = 0;
    va_list ap;
    if (!g_logf || n >= 40 || (g_frames % 120) != 1) return;
    n++;
    va_start(ap, fmt); vfprintf(g_logf, fmt, ap); va_end(ap);
    fputc('\n', g_logf); fflush(g_logf);
}

static BOOL Rd32(uintptr_t addr, uint32_t* out) {
    if (!addr || IsBadReadPtr((const void*)addr, 4)) return FALSE;
    *out = *(volatile const uint32_t*)addr;
    return TRUE;
}

static void Key(int vk, int* prev, int* edge) {
    int down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    *edge = down && !*prev;
    *prev = down;
}

static int ReadProjection(PfxCamera* cam) {
    uint32_t camMgr = 0;
    const float* m;
    if (!Rd32((uintptr_t)IdaAddrToRuntime(ADDR_CAMMGR_VAR), &camMgr) || !camMgr) { Diag("postfx: camera manager unreadable"); return 0; }
    m = (const float*)(uintptr_t)(camMgr + CAM_OFF_PROJ);
    if (IsBadReadPtr(m, 64)) { Diag("postfx: projection unreadable (cam=%08x)", camMgr); return 0; }
    cam->m00 = m[0]; cam->m11 = m[5]; cam->m22 = m[10]; cam->m32 = m[14];
    Diag("postfx: cam=%08x proj m00=%g m11=%g m22=%g m32=%g", camMgr, m[0], m[5], m[10], m[14]);
    if (!(cam->m00 > 0.05f && cam->m00 < 50.f && cam->m11 > 0.05f && cam->m11 < 50.f)) return 0;
    if (!(fabsf(cam->m32) > 1e-6f) || !(fabsf(cam->m22) < 2.0f)) return 0;   // forward (m32<0) or reversed (m32>0) Z
    return 1;
}

static void FreezeFrame(void);
static void DoDump(const char* name);
// ---- yaw string: translucent overlay layer ----------------------------------------------------------------
// Avion.Panel.DrawYawstring (FUN_006face8) draws the yaw string after the cockpit depth clear with the engine's global
// "depth disabled" state (*[0x793008]: DepthEnable = FALSE, so no depth test and no depth write), alpha blended over
// whatever is behind it. It leaves no depth, and it is translucent: its soft edges are mostly the world behind it.
// Whatever depth rule the postfx uses, a pixel that is part string, part world gets one treatment for both, e.g. the
// world part of the edges loses its haze below the horizon and the string looks thicker there.
// So the string is handled as a layer: both calls of FUN_00717304 (0x6fb543 and 0x6fb673, `CALL rel32`) go through
// YawDraw, which runs the original draw unchanged and then draws the string again into two private targets cleared to
// black and to white. The final pass gets C = ovB + (1 - a) * W, a = 1 - (ovW - ovB), and grades the string and the
// world behind it separately.
#define FN_YAW_DRAW 0x717304
#define YAW_CALL1 0x6fb543
#define YAW_CALL2 0x6fb673
#define REL(p) do { if (p) { (p)->lpVtbl->Release(p); (p) = NULL; } } while (0)
// With MSAA on (Windows setups usually; MSAACount in Setup.ini) the game's colour target and depth buffer are multisampled. The
// string is then drawn into multisampled targets of the same sample count and quality (they are bound together with the game's
// depth view, which must match) and resolved into the single-sampled textures the final pass reads, like the scene colour itself.
// The layer formula is linear, so resolving (averaging the samples) keeps it exact at the string's antialiased edges.
typedef struct { ID3D11Texture2D* tex; ID3D11RenderTargetView* rtv; ID3D11ShaderResourceView* srv; ID3D11Texture2D* msTex; ID3D11RenderTargetView* msRtv; } OvTarget;
static OvTarget g_ov[2];               // 0 = black, 1 = white
static UINT g_ovW = 0, g_ovH = 0, g_ovSamples = 0, g_ovQuality = 0;
static int g_ovDrawn = 0;              // set by YawDraw, consumed by HudPre
static int g_yawState = 0;             // 1 = call sites patched, <0 = unexpected code
static volatile int g_yawLayer = 1;    // debug: cmd yawlayer on|off

static void OvRelease(void) { int i; for (i = 0; i < 2; i++) { REL(g_ov[i].msRtv); REL(g_ov[i].msTex); REL(g_ov[i].srv); REL(g_ov[i].rtv); REL(g_ov[i].tex); } g_ovW = g_ovH = g_ovSamples = g_ovQuality = 0; }
static int OvEnsure(ID3D11Device* dev, UINT w, UINT h, DXGI_SAMPLE_DESC sd) {
    D3D11_TEXTURE2D_DESC d;
    int i;
    static UINT bad[4];                                          // shape that could not be created: not again every frame
    if (g_ov[0].tex && g_ovW == w && g_ovH == h && g_ovSamples == sd.Count && g_ovQuality == sd.Quality) return 1;
    if (bad[0] == w && bad[1] == h && bad[2] == sd.Count && bad[3] == sd.Quality) return 0;
    bad[0] = w; bad[1] = h; bad[2] = sd.Count; bad[3] = sd.Quality;            // cleared below on success
    OvRelease();
    memset(&d, 0, sizeof d);
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (i = 0; i < 2; i++) {
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &g_ov[i].tex)) ||
            FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource*)g_ov[i].tex, NULL, &g_ov[i].rtv)) ||
            FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource*)g_ov[i].tex, NULL, &g_ov[i].srv))) { OvRelease(); return 0; }
    }
    if (sd.Count > 1) {
        d.SampleDesc = sd; d.BindFlags = D3D11_BIND_RENDER_TARGET;
        for (i = 0; i < 2; i++) {
            if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &g_ov[i].msTex)) ||
                FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource*)g_ov[i].msTex, NULL, &g_ov[i].msRtv))) {
                if (g_logf) { fprintf(g_logf, "postfx: yaw string layer: no %ux%u target with %u samples (quality %u)\n", w, h, sd.Count, sd.Quality); fflush(g_logf); }
                OvRelease(); return 0;
            }
        }
    }
    g_ovW = w; g_ovH = h; g_ovSamples = sd.Count; g_ovQuality = sd.Quality; memset(bad, 0, sizeof bad);
    if (g_logf) { fprintf(g_logf, "postfx: yaw string layer targets %ux%u, %u samples\n", w, h, sd.Count); fflush(g_logf); }
    return 1;
}

// ---- one D3D11 device per flight ------------------------------------------------------------------------------
// The game destroys its device at the end of a flight and creates a new one for the next ("Destroying DirectX 11 framework" /
// "Created D3D11 Device" in Logs\Logfile.txt). Everything we create (effect core, yaw string layer, world depth copy) belongs to
// one device; used with the next flight's context, native D3D11 crashed in the next draw ("Draw thermals: Access violation in
// d3d11.dll", the game shut down; DXVK tolerated it). So: when the game's device is not the one our objects were made on, they are
// all released and made again on demand. Our objects hold references to the old device, so the new one never has its address.
static ID3D11Device* g_objDev = NULL;      // device of our objects (not referenced; only compared)
static void DssCloneRelease(void);
static int DeviceCheck(ID3D11Device* dev) {   // render thread; 1 = changed (everything released)
    if (!dev || dev == g_objDev) return 0;
    if (!g_objDev) { g_objDev = dev; return 0; }
    if (g_logf) { fprintf(g_logf, "postfx: new D3D11 device %p (was %p): our objects released, made again\n", (void*)dev, (void*)g_objDev); fflush(g_logf); }
    if (g_core) { PfxCore_Destroy(g_core); g_core = NULL; }
    OvRelease(); g_ovDrawn = 0; DssCloneRelease();
    if (g_worldCopy) { ID3D11Texture2D_Release(g_worldCopy); g_worldCopy = NULL; }
    g_mainDepth = NULL; g_worldValid = 0; g_clearsThisFrame = 0; g_copiesThisFrame = 0;
    g_objDev = dev;
    return 1;
}

static uint32_t DelphiCall(uintptr_t fn, uint32_t a, uint32_t d, uint32_t c);
// Called instead of FUN_00717304 (Delphi register convention: object in EAX = regparm(1)).
static void __attribute__((regparm(1), used, noinline)) YawDraw(uint32_t obj) {
    uintptr_t fn = (uintptr_t)IdaAddrToRuntime(FN_YAW_DRAW);
    uint32_t ctxp = 0;
    ID3D11DeviceContext* x;
    ID3D11RenderTargetView* rtv[8] = { 0 };
    ID3D11DepthStencilView* dsv = NULL;
    ID3D11Device* dev = NULL;
    ID3D11Resource* res = NULL;
    D3D11_TEXTURE2D_DESC cd;
    int i;

    DelphiCall(fn, obj, 0, 0);                                   // the game's own draw, unchanged
    if (!g_yawLayer || !g_enabled || g_dead || !Rd32((uintptr_t)IdaAddrToRuntime(ADDR_CONTEXT_VAR), &ctxp) || !ctxp) return;
    x = (ID3D11DeviceContext*)(uintptr_t)ctxp;
    ID3D11DeviceContext_OMGetRenderTargets(x, 8, rtv, &dsv);
    if (rtv[0]) {
        ID3D11RenderTargetView_GetResource(rtv[0], &res);
        if (res) {
            ID3D11Texture2D_GetDesc((ID3D11Texture2D*)res, &cd);
            ID3D11DeviceContext_GetDevice(x, &dev);
            if (dev) DeviceCheck(dev);
            if (dev && OvEnsure(dev, cd.Width, cd.Height, cd.SampleDesc)) {
                static const FLOAT clr[2][4] = { { 0, 0, 0, 0 }, { 1, 1, 1, 1 } };
                for (i = 0; i < 2; i++) {
                    ID3D11RenderTargetView* t = g_ov[i].msRtv ? g_ov[i].msRtv : g_ov[i].rtv;
                    ID3D11DeviceContext_ClearRenderTargetView(x, t, clr[i]);
                    ID3D11DeviceContext_OMSetRenderTargets(x, 1, &t, dsv);
                    DelphiCall(fn, obj, 0, 0);
                    if (g_ov[i].msTex) ID3D11DeviceContext_ResolveSubresource(x, (ID3D11Resource*)g_ov[i].tex, 0, (ID3D11Resource*)g_ov[i].msTex, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
                }
                ID3D11DeviceContext_OMSetRenderTargets(x, 8, rtv, dsv);
                g_ovDrawn = 1;
            }
            REL(dev);
            ID3D11Resource_Release(res);
        }
    }
    for (i = 0; i < 8; i++) REL(rtv[i]);
    REL(dsv);
}

static int YawPatchCall(uintptr_t ida) {
    uint8_t* a = (uint8_t*)IdaAddrToRuntime(ida);
    uintptr_t orig = (uintptr_t)IdaAddrToRuntime(FN_YAW_DRAW);
    int32_t rel;
    DWORD old;
    if (IsBadReadPtr(a, 5) || a[0] != 0xE8 || (uintptr_t)(a + 5) + *(int32_t*)(a + 1) != orig) return 0;
    rel = (int32_t)((uintptr_t)YawDraw - (uintptr_t)(a + 5));
    if (!VirtualProtect(a, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    *(int32_t*)(a + 1) = rel;
    VirtualProtect(a, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), a, 5);
    return 1;
}
// ---- translucent cockpit parts: no depth ------------------------------------------------------------------------
// The model draw FUN_00772688 draws a model's opaque parts, then (local pass = 1, from 0x772929) its translucent parts (material
// alpha < 1, part+0x144) with the alpha blend state but the depth state unchanged: in the cockpit pass that is the default state
// with depth writes on. So glass like the clear-view window ("DV Panel", alpha 0.2) and the PDA glass write cockpit depth, the
// final pass takes the landscape seen through them for cockpit interior and grades it without haze, sky and AO. In the cockpit
// pass (after the cockpit depth clear) and while the effect is on, their loop runs with a copy of the bound depth state without
// depth writes (the depth test stays: glass behind cockpit parts stays hidden); the game's state is put back when the function
// returns (0x772e85). Not in the world pass: there a canopy without depth would let clouds drawn later appear in front of it.
// Only for the glider's own geometry (Discus2aIE.c3d etc.: canopy, clear-view window, PDA glass), drawn through the wrapper
// FUN_00762104 (return address 0x762126). The instruments are separate models (Gauges\*.c3d) whose "Glass" part (alpha 0.1)
// must keep writing depth: the panel drawn after them relies on it, without it the panel covered the dials (only the needles
// stayed in front).
#define ADDR_MODEL_TRANSLUCENT 0x772929
#define ADDR_MODEL_DRAW_EXIT   0x772e85
#define ADDR_GEOMETRY_DRAW_RET 0x762126   // FUN_00762104: geometry->model (+8) through FUN_00772688
typedef struct { ID3D11DepthStencilState* src; ID3D11DepthStencilState* noWrite; } DssClone;
static DssClone g_dssClone[8]; static int g_nDssClone;
static ID3D11DepthStencilState* g_dssSaved; static UINT g_dssSavedRef; static int g_dssSwapped;
static void DssCloneRelease(void) { int i; for (i = 0; i < g_nDssClone; i++) { REL(g_dssClone[i].noWrite); REL(g_dssClone[i].src); } g_nDssClone = 0; }
static void TranslucentPre(void* address, DobbyRegisterContext* rc) {
    uint32_t ctxp = 0; ID3D11DeviceContext* x; ID3D11DepthStencilState* cur = NULL; ID3D11DepthStencilState* nw = NULL; UINT ref = 0; int i;
    (void)address;
    if (g_dssSwapped || !g_enabled || g_dead || g_clearsThisFrame < 2) return;       // cockpit pass only (2nd clear of the main depth)
    {   uint32_t ret = 0;                                                           // FUN_00772688 has an EBP frame: [ebp+4] = its caller
        if (!Rd32(rc->general.regs.ebp + 4, &ret) || ret != (uint32_t)(uintptr_t)IdaAddrToRuntime(ADDR_GEOMETRY_DRAW_RET)) return; }
    if (!Rd32((uintptr_t)IdaAddrToRuntime(ADDR_CONTEXT_VAR), &ctxp) || !ctxp) return;
    x = (ID3D11DeviceContext*)(uintptr_t)ctxp;
    ID3D11DeviceContext_OMGetDepthStencilState(x, &cur, &ref);
    if (!cur) return;
    for (i = 0; i < g_nDssClone && !nw; i++) if (g_dssClone[i].src == cur) nw = g_dssClone[i].noWrite;
    if (!nw && i == g_nDssClone) {
        D3D11_DEPTH_STENCIL_DESC dd; ID3D11Device* dev = NULL;
        ID3D11DepthStencilState_GetDesc(cur, &dd);
        if (dd.DepthEnable && dd.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO && g_nDssClone < 8) {
            ID3D11DeviceContext_GetDevice(x, &dev);
            dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            if (dev && SUCCEEDED(ID3D11Device_CreateDepthStencilState(dev, &dd, &nw))) {
                ID3D11DepthStencilState_AddRef(cur);
                g_dssClone[g_nDssClone].src = cur; g_dssClone[g_nDssClone].noWrite = nw; g_nDssClone++;
                if (g_logf) { fprintf(g_logf, "postfx: translucent cockpit parts: depth state %p without depth writes\n", (void*)cur); fflush(g_logf); }
            }
            REL(dev);
        }
    }
    if (!nw) { ID3D11DepthStencilState_Release(cur); return; }                    // depth writes off already, or no copy
    g_dssSaved = cur; g_dssSavedRef = ref; g_dssSwapped = 1;                        // keeps the reference from OMGet
    ID3D11DeviceContext_OMSetDepthStencilState(x, nw, ref);
}
static void ModelDrawExitPre(void* address, DobbyRegisterContext* rc) {
    uint32_t ctxp = 0;
    (void)address; (void)rc;
    if (!g_dssSwapped) return;
    if (Rd32((uintptr_t)IdaAddrToRuntime(ADDR_CONTEXT_VAR), &ctxp) && ctxp)
        ID3D11DeviceContext_OMSetDepthStencilState((ID3D11DeviceContext*)(uintptr_t)ctxp, g_dssSaved, g_dssSavedRef);
    REL(g_dssSaved); g_dssSwapped = 0;
}
static int g_translucentHooks;
static void YawInstall(void) {
    if (g_yawState) return;
    if (!g_translucentHooks) {
        const uint8_t* a = (const uint8_t*)IdaAddrToRuntime(ADDR_MODEL_TRANSLUCENT); const uint8_t* e = (const uint8_t*)IdaAddrToRuntime(ADDR_MODEL_DRAW_EXIT);
        int r1 = -9, r2 = -9;
        // expected code: push -1; mov eax,[0x793580] ... / pop edi; pop esi; pop ebx; mov esp,ebp; pop ebp; ret 0xc
        if (!IsBadReadPtr(a, 7) && !IsBadReadPtr(e, 9) && a[0] == 0x6A && a[1] == 0xFF && a[2] == 0xA1 && !memcmp(e, "\x5F\x5E\x5B\x8B\xE5\x5D\xC2\x0C\x00", 9)) {
            r1 = DobbyInstrument((void*)a, TranslucentPre);
            r2 = DobbyInstrument((void*)e, ModelDrawExitPre);
        }
        g_translucentHooks = 1;
        if (g_logf) { fprintf(g_logf, "postfx: translucent cockpit parts without depth writes: hooks %d %d (0 = ok)\n", r1, r2); fflush(g_logf); }
    }
    g_yawState = (YawPatchCall(YAW_CALL1) & YawPatchCall(YAW_CALL2)) ? 1 : -1;
    if (g_logf) { fprintf(g_logf, "postfx: yaw string overlay layer %s\n", g_yawState == 1 ? "installed" : "NOT installed (unexpected code)"); fflush(g_logf); }
}

static void GotoApply(void); static volatile int g_gotoPending;
static void HudPre(void* address, DobbyRegisterContext* rc) {
    static int kF8, kF9, kF10;
    int e8, e9, e10;
    uint32_t dev = 0, ctx = 0, p1 = 0, mgr = 0, idx = 0, arr = 0, item = 0, color = 0, depth = 0, rtv = 0;
    PfxCamera cam;
    (void)address; (void)rc;

    int ovDrawn = g_ovDrawn;
    g_ovDrawn = 0;
    if (g_dead) return;
    g_frames++;
    FreezeFrame();                       // Ctrl+Shift+F freeze toggle, pending `att` (main thread)
#ifdef ENABLE_AGSTREAM
    { void AgStream_FrameTick(void); AgStream_FrameTick(); }
#endif

    Key(VK_F8, &kF8, &e8); Key(VK_F9, &kF9, &e9); Key(VK_F10, &kF10, &e10);
    if (e9) { g_enabled = !g_enabled; DBGLOG("postfx: %s", g_enabled ? "on" : "off"); if (g_logf) { fprintf(g_logf, "postfx: %s\n", g_enabled ? "on" : "off"); fflush(g_logf); } }
    // live parameters for the terrain shader + atmosphere read-back: independent of F9 and of the lookups below
    FxFrame();
    if (!g_enabled) return;

    if (!Rd32((uintptr_t)IdaAddrToRuntime(ADDR_DEVICE_VAR), &dev) || !dev) { Diag("postfx: no device"); return; }
    if (!Rd32((uintptr_t)IdaAddrToRuntime(ADDR_CONTEXT_VAR), &ctx) || !ctx) { Diag("postfx: no context"); return; }
    DeviceCheck((ID3D11Device*)(uintptr_t)dev);
    YawInstall();

    if (!Rd32((uintptr_t)IdaAddrToRuntime(ADDR_SWAPMGR_PTR), &p1) || !Rd32(p1, &mgr) || !mgr) { Diag("postfx: no swap manager (p1=%08x)", p1); return; }
    if (!Rd32(mgr + MGR_OFF_INDEX, &idx) || !Rd32(mgr + MGR_OFF_ITEMS, &arr) || !arr) { Diag("postfx: manager layout (mgr=%08x)", mgr); return; }
    if (!Rd32(arr + idx * 4, &item) || !item) { Diag("postfx: no item (arr=%08x idx=%u)", arr, idx); return; }
    if (!Rd32(item + ITEM_OFF_COLOR, &color) || !Rd32(item + ITEM_OFF_DEPTH, &depth) || !Rd32(item + ITEM_OFF_RTV, &rtv)) return;
    Diag("postfx: dev=%08x ctx=%08x item=%08x color=%08x depth=%08x rtv=%08x", dev, ctx, item, color, depth, rtv);
    if (!color || !depth || !rtv) return;
    g_mainDepth = (ID3D11Texture2D*)(uintptr_t)depth;
    HookClear((ID3D11DeviceContext*)(uintptr_t)ctx);
    if (!ReadProjection(&cam)) { Diag("postfx: projection rejected"); return; }
    cam.time = (float)(GetTickCount() % 3600000u) / 1000.0f;
    memset(cam.view, 0, sizeof cam.view);
    cam.camPos[0] = cam.camPos[1] = cam.camPos[2] = cam.camPos[3] = 0;
    if (g_atmo.valid && g_frames - g_atmo.frame < 10) {
        memcpy(cam.view, g_atmo.view, sizeof cam.view);
        memcpy(cam.camPos, g_atmo.camPos, 12); cam.camPos[3] = 1.0f;
        memcpy(cam.fog, g_atmo.fog, 16); memcpy(cam.sunDir, g_atmo.sun, 12); cam.sunDir[3] = 0;
        memcpy(cam.sunColor, g_atmo.sunColor, 16); memcpy(cam.amb, g_atmo.amb, 16);
    }

    if (!g_core) {
        char hlsl[MAX_PATH];
        _snprintf(hlsl, sizeof hlsl, "%s\\postfx.hlsl", g_dir);
        g_core = PfxCore_Create((ID3D11Device*)(uintptr_t)dev, hlsl);
        if (!g_core) { g_dead = 1; return; }
    }
    if (e8) PfxCore_RequestReload(g_core);
    if (g_shot[0]) { PfxCore_RequestCapture(g_core, g_shot); g_shot[0] = 0; }
    if (e10 || g_frames == 600) {      // F10, or automatically once ~10 s after start (diagnostics)
        char path[MAX_PATH];
        _snprintf(path, sizeof path, "%s\\capture.bin", g_dir);
        PfxCore_RequestCapture(g_core, path);
    }

    {   // diagnostics: log every change of the per-frame inputs (engine atmosphere valid? depth clears / world copies per frame)
        static int lastOk = -1, budget = 400; static unsigned lastCopies = 99, lastClears = 99;
        int ok = (g_atmo.valid && g_frames - g_atmo.frame < 10);
        if (g_logf && budget > 0 && (ok != lastOk || g_copiesThisFrame != lastCopies || g_clearsThisFrame != lastClears)) {
            budget--;
            fprintf(g_logf, "postfx: frame %u atmoOk=%d (age %u) clears=%u copies=%u worldValid=%u\n", g_frames, ok, g_frames - g_atmo.frame, g_clearsThisFrame, g_copiesThisFrame, g_worldValid);
            fflush(g_logf);
        }
        lastOk = ok; lastCopies = g_copiesThisFrame; lastClears = g_clearsThisFrame;
    }
    PfxCore_SetOverlay(g_core, ovDrawn ? g_ov[0].srv : NULL, ovDrawn ? g_ov[1].srv : NULL);
    if (PfxCore_Run(g_core, (ID3D11DeviceContext*)(uintptr_t)ctx, (ID3D11Texture2D*)(uintptr_t)color,
                    (ID3D11RenderTargetView*)(uintptr_t)rtv, (ID3D11Texture2D*)(uintptr_t)depth,
                    (g_worldCopy && g_worldValid && g_copiesThisFrame >= 2) ? g_worldCopy : NULL, &cam)) {   // 1 clear/frame (external view, no cockpit pass): the copy is last frame's depth, the live depth already is the world
        g_fails = 0;
        if (g_logf && (g_frames % 300) == 2) { fprintf(g_logf, "postfx: frame %u: %u clears, %u world copies, clearCalls %u, vt53 %p (hooked target %p)\n", g_frames, g_clearsThisFrame, g_copiesThisFrame, g_clearCalls, CurClearEntry(ctx), g_clearTarget); fflush(g_logf); }
    } else if (++g_fails > 600) {
        g_dead = 1;     // e.g. unsupported formats: stop trying, the game keeps running unmodified
        if (g_logf) { fprintf(g_logf, "postfx: giving up after repeated failures\n"); fflush(g_logf); }
    }
    g_worldValid = 0; g_clearsThisFrame = 0; g_copiesThisFrame = 0;
}


// ---- remote control (for automated testing) ---------------------------------------------------------------
// A thread inside the game polls <exedir>\postfx\cmd.txt once per 200 ms. Every line is a command, the file is
// deleted after it was read, results go to postfx.log. Clicks/keys are posted as window messages to the game
// window, so no macOS accessibility permission is needed.
//   windows                 list the game's windows (handle, title, client size, screen position)
//   click X Y               left click at client coordinates (of the main window)
//   key NAME|0xVK [n]       key press (NAME: enter esc space tab up down left right f1..f12 a..z 0..9), n times
//   keydown/keyup NAME      hold / release (e.g. for camera or controls)
//   post on|off             post-processing on/off
//   yawlayer on|off         debug: the yaw string overlay layer (extra draws for the final pass) on/off
//   freeze on|off|toggle    stop / restart the glider's physics without pausing (hotkey Ctrl+Shift+F); move/att still work
//   att [PITCH ROLL YAW]    print / set the glider's attitude in degrees (yaw = compass heading of the nose, pitch = nose up,
//                           roll = right wing down); the velocity turns with the glider
//   dump [NAME]             append "NAME x y z pitch roll yaw" to postfx\positions.txt (hotkey Ctrl+Shift+D)
//   goto NAME | goto X Y Z [PITCH ROLL YAW]   absolute position (+ attitude); NAME = last line of that name in positions.txt
//   move DX DY DZ           offset the glider position (metres, x west, y north, z up)
//   wait MS                 pause the script
#include <ctype.h>
static HWND g_main = NULL;
#define ADDR_STATUS_TEXT 0x007D96A4      // Delphi AnsiString: current loading/status message of the game
static volatile int g_wantOff = 0;
static void RLog(const char* fmt, ...) {
    va_list ap;
    if (!g_logf) return;
    va_start(ap, fmt); vfprintf(g_logf, fmt, ap); va_end(ap);
    fputc('\n', g_logf); fflush(g_logf);
}
static BOOL CALLBACK EnumW(HWND h, LPARAM lp) {
    DWORD pid; char t[128]; RECT r; POINT o = {0, 0};
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h)) return TRUE;
    GetWindowTextA(h, t, sizeof t);
    GetClientRect(h, &r); ClientToScreen(h, &o);
    if (lp) RLog("window %p '%s' client %ldx%ld at screen %ld,%ld", (void*)h, t, r.right, r.bottom, o.x, o.y);
    if (strncmp(t, "Condor", 6) == 0 && r.right > 200) g_main = h;   // the big game/menu window
    return TRUE;
}
static int KeyVk(const char* n) {
    static const struct { const char* n; int vk; } t[] = {
        {"enter",VK_RETURN},{"esc",VK_ESCAPE},{"space",VK_SPACE},{"tab",VK_TAB},{"up",VK_UP},{"down",VK_DOWN},
        {"left",VK_LEFT},{"right",VK_RIGHT},{"pgup",VK_PRIOR},{"pgdn",VK_NEXT},{"home",VK_HOME},{"end",VK_END},
        {"pause",VK_PAUSE},{"back",VK_BACK},{"plus",VK_ADD},{"minus",VK_SUBTRACT}};
    unsigned i;
    if (n[0] == '0' && (n[1] == 'x' || n[1] == 'X')) return (int)strtol(n, NULL, 16);
    for (i = 0; i < sizeof t / sizeof t[0]; i++) if (!_stricmp(n, t[i].n)) return t[i].vk;
    if ((n[0] == 'f' || n[0] == 'F') && isdigit((unsigned char)n[1])) return VK_F1 + atoi(n + 1) - 1;
    if (n[1] == 0) return toupper((unsigned char)n[0]);
    return 0;
}
static void KeyMsg(int vk, int down) {
    UINT sc = MapVirtualKeyA(vk, 0);
    LPARAM lp = 1 | (sc << 16) | (down ? 0 : (3u << 30));
    HWND h = GetForegroundWindow(); DWORD pid = 0;
    if (h) GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) h = g_main;
    PostMessageA(h, down ? WM_KEYDOWN : WM_KEYUP, (WPARAM)vk, lp);
}
static void DoClick(int x, int y) {
    HWND h = g_main, c;
    POINT p; LPARAM lp;
    if (!h) return;
    // deepest child window at that point
    p.x = x; p.y = y;
    for (;;) {
        c = ChildWindowFromPointEx(h, p, CWP_SKIPINVISIBLE | CWP_SKIPTRANSPARENT);
        if (!c || c == h) break;
        MapWindowPoints(h, c, &p, 1);
        h = c;
    }
    lp = MAKELPARAM(p.x, p.y);
    PostMessageA(h, WM_MOUSEMOVE, 0, lp);
    PostMessageA(h, WM_LBUTTONDOWN, MK_LBUTTON, lp);
    Sleep(60);
    PostMessageA(h, WM_LBUTTONUP, 0, lp);
    RLog("clicked %d,%d on window %p", x, y, (void*)h);
}


// ---- free camera ---------------------------------------------------------------------------------------------
// The camera manager (*(0x7D96F8)) holds the view matrix at +0x108 (D3DX, left handed) and its inverse at +0x148
// (rows: right, up, forward, position). World: z is up. We overwrite both right at the start of the scene pass.
#define ADDR_SCENE_DRAW 0x0073B59C
#define CAM_OFF_VIEW    0x108
#define CAM_OFF_WORLD   0x148
static volatile int g_camFree = 0;
static float g_camPos[3], g_camYaw = 0, g_camPitch = 0;      // yaw: degrees, 0 = world +y, clockwise seen from above; pitch: up positive
static volatile int g_camLatch = 0;                          // request: copy the engine camera into g_cam*

static void ScenePre(void* address, DobbyRegisterContext* rc) {
    uint32_t mgr = 0; float* view; float* world;
    (void)address; (void)rc;
    if (!g_camFree && !g_camLatch) return;
    if (!Rd32((uintptr_t)IdaAddrToRuntime(ADDR_CAMMGR_VAR), &mgr) || !mgr) return;
    view = (float*)(uintptr_t)(mgr + CAM_OFF_VIEW); world = (float*)(uintptr_t)(mgr + CAM_OFF_WORLD);
    if (IsBadReadPtr(view, 64) || IsBadReadPtr(world, 64) || IsBadWritePtr(view, 64) || IsBadWritePtr(world, 64)) return;
    if (g_camLatch) {       // read pos/orientation from the engine's own world matrix
        float fx = world[8], fy = world[9], fz = world[10];
        g_camPos[0] = world[12]; g_camPos[1] = world[13]; g_camPos[2] = world[14];
        g_camPitch = asinf(fz) * 57.29578f; g_camYaw = atan2f(fx, fy) * 57.29578f;
        g_camLatch = 0;
        RLog("cam latched: pos %.1f %.1f %.1f yaw %.1f pitch %.1f", g_camPos[0], g_camPos[1], g_camPos[2], g_camYaw, g_camPitch);
        if (!g_camFree) return;
    }
    {
        float y = g_camYaw * 0.0174533f, t = g_camPitch * 0.0174533f;
        float f[3] = { sinf(y) * cosf(t), cosf(y) * cosf(t), sinf(t) };
        float r[3] = { f[1], -f[0], 0 };                 // up0 x forward, up0 = (0,0,1)
        float rl = sqrtf(r[0] * r[0] + r[1] * r[1]), u[3]; int i;
        if (rl < 1e-6f) { r[0] = 1; r[1] = 0; rl = 1; }
        r[0] /= rl; r[1] /= rl;
        u[0] = f[1] * r[2] - f[2] * r[1]; u[1] = f[2] * r[0] - f[0] * r[2]; u[2] = f[0] * r[1] - f[1] * r[0];
        for (i = 0; i < 3; i++) {
            world[i] = r[i]; world[4 + i] = u[i]; world[8 + i] = f[i]; world[12 + i] = g_camPos[i];
            view[i * 4 + 0] = r[i]; view[i * 4 + 1] = u[i]; view[i * 4 + 2] = f[i]; view[i * 4 + 3] = 0;
        }
        view[12] = -(g_camPos[0] * r[0] + g_camPos[1] * r[1] + g_camPos[2] * r[2]);
        view[13] = -(g_camPos[0] * u[0] + g_camPos[1] * u[1] + g_camPos[2] * u[2]);
        view[14] = -(g_camPos[0] * f[0] + g_camPos[1] * f[1] + g_camPos[2] * f[2]);
        view[15] = 1; world[3] = world[7] = world[11] = 0; world[15] = 1;
    }
}

// peek/poke: EXPR = [*]ADDR[+OFF]; ADDR hex, "i:" prefix = Condor.exe (IDA/Ghidra) address; leading '*' = read the 32 bit
// value at ADDR first (follow a pointer), then add OFF.
static uintptr_t ResolveExpr(const char* e) {
    uintptr_t base; int deref = 0; unsigned long off = 0; const char* plus;
    if (*e == '*') { deref = 1; e++; }
    if (e[0] == 'i' && e[1] == ':') { e += 2; base = (uintptr_t)IdaAddrToRuntime((uintptr_t)strtoul(e, NULL, 16)); }
    else base = (uintptr_t)strtoul(e, NULL, 16);
    plus = strchr(e, '+'); if (plus) off = strtoul(plus + 1, NULL, 16);
    if (deref) { uint32_t v = 0; if (!Rd32(base, &v)) return 0; base = v; }
    return base + off;
}
static void DoPeek(const char* expr, int len) {
    uintptr_t a = ResolveExpr(expr); int i;
    if (!a || len < 4 || len > 4096 || IsBadReadPtr((void*)a, len)) { RLog("peek %s: unreadable (%p)", expr, (void*)a); return; }
    RLog("peek %s = %p", expr, (void*)a);
    for (i = 0; i < len; i += 16) {
        const float* f = (const float*)(a + i); const uint32_t* u = (const uint32_t*)(a + i);
        int n = (len - i >= 16) ? 4 : (len - i) / 4;
        char line[200]; int o = _snprintf(line, sizeof line, "  +%03x:", i), k;
        for (k = 0; k < n; k++) {
            float v = f[k];
            if (u[k] != 0 && fabsf(v) > 1e-6f && fabsf(v) < 1e7f) o += _snprintf(line + o, sizeof line - o, " %12.5g", v);
            else o += _snprintf(line + o, sizeof line - o, " %12s", u[k] ? "." : "0");
            o += _snprintf(line + o, sizeof line - o, "(%08x)", u[k]);
        }
        RLog("%s", line);
    }
}
static void DoPoke(const char* expr, float v) {
    uintptr_t a = ResolveExpr(expr);
    if (!a || IsBadWritePtr((void*)a, 4)) { RLog("poke %s: not writable", expr); return; }
    *(volatile float*)a = v; RLog("poke %s = %g", expr, v);
}

// move DX DY DZ: offset the glider's world position (doubles at obj+0x6d8/0x6e0/0x6e8, integrated by FUN_00706988).
static void DoMove(double dx, double dy, double dz) {
    uint32_t pp = 0, obj = 0; double* p; double d[3]; d[0] = dx; d[1] = dy; d[2] = dz;
    if (!Rd32((uintptr_t)IdaAddrToRuntime(0x792a64), &pp) || !pp || !Rd32(pp, &obj) || !obj) { RLog("move: no glider object"); return; }
    p = (double*)(obj + 0x6d8);
    if (IsBadWritePtr(p, 24)) { RLog("move: bad ptr"); return; }
    p[0] += d[0]; p[1] += d[1]; p[2] += d[2];
    RLog("move: obj %08x pos now %.2f %.2f %.2f", obj, p[0], p[1], p[2]);
}

// ---- freeze + attitude ------------------------------------------------------------------------------------------
// Avion update (0x6F921C, "Avion.Physics") calls the physics step FUN_00706988 (no arguments, uses the glider [[0x792a64]]).
// It integrates position (doubles +0x6d8), velocity (floats +0x6f0) and the orientation matrix +0x5cc (4x4 floats, rows = nose,
// side, up in world axes x west / y north / z up; the world frame is mirrored, so a raised row-1 tip shows as a RIGHT bank [G]), then derives +0x60c (inverse, FUN_0047FA60) and +0x6c0 (position relative to
// the floating origin, FUN_0076107C). Frozen: we skip the step and only derive +0x60c / +0x6c0, like the game's own replay branch.
// So the glider stays where it is while the sim keeps running (no pause), and `move` / `att` place it freely.
#define FN_PHYSICS  0x00706988
#define FN_MAT_INV  0x0047FA60      // EAX = dst (+0x60c), EDX = src (+0x5cc)
#define FN_LS_POS   0x0076107C      // EAX = landscape, EDX = dst (+0x6c0), ECX = src (+0x6d8)
#define FN_MAT_QUAT 0x004B0018      // EAX = matrix (+0x5cc), EDX = quaternion (+0x6a4)
static volatile int g_freeze = 0;
static void (*g_origPhysics)(void);
static volatile int g_attPending = 0; static float g_attSet[3];      // pitch, roll, yaw (degrees), applied on the main thread
static uint32_t DelphiCall(uintptr_t fn, uint32_t a, uint32_t d, uint32_t c) {
    __asm__ volatile("call *%3" : "+a"(a), "+d"(d), "+c"(c) : "r"(fn) : "memory", "cc");
    return a;
}
static uint32_t GliderObj(void) {
    uint32_t pp = 0, obj = 0;
    if (!Rd32((uintptr_t)IdaAddrToRuntime(0x792a64), &pp) || !pp || !Rd32(pp, &obj) || !obj || IsBadWritePtr((void*)(uintptr_t)(obj + 0x5cc), 0x130)) return 0;
    return obj;
}
static uint32_t LandscapeObj(void) {
    uint32_t p = 0, w = 0, l = 0;
    if (!Rd32((uintptr_t)IdaAddrToRuntime(0x792fd0), &p) || !p || !Rd32(p, &w) || !w || !Rd32(w + 0x44, &l)) return 0;
    return l;
}
static void DeriveFromState(uint32_t o) {           // what the game computes from +0x5cc / +0x6d8 after a physics step
    uint32_t L = LandscapeObj();
    DelphiCall((uintptr_t)IdaAddrToRuntime(FN_MAT_INV), o + 0x60c, o + 0x5cc, 0);
    if (L) DelphiCall((uintptr_t)IdaAddrToRuntime(FN_LS_POS), L, o + 0x6c0, o + 0x6d8);
}
static void PhysicsHook(void) {
    uint32_t o;
    if (!g_freeze) { g_origPhysics(); return; }
    o = GliderObj();
    if (o) DeriveFromState(o);
}
// current attitude from the matrix rows (degrees): yaw = compass heading of the nose, pitch = nose up, roll = right wing down
static void AttGet(const float* m, float* pitch, float* roll, float* yaw) {
    const float *f = m, *r = m + 4; float th, ps, rl[3], ul[3];
    th = asinf(fmaxf(-1.0f, fminf(1.0f, f[2])));
    ps = atan2f(-f[0], f[1]);
    rl[0] = -cosf(ps); rl[1] = -sinf(ps); rl[2] = 0;
    ul[0] = sinf(th) * sinf(ps); ul[1] = -sinf(th) * cosf(ps); ul[2] = cosf(th);
    *pitch = th * 57.29578f; *yaw = ps * 57.29578f; if (*yaw < 0) *yaw += 360.0f;
    *roll = atan2f(r[0] * ul[0] + r[1] * ul[1] + r[2] * ul[2], r[0] * rl[0] + r[1] * rl[1] + r[2] * rl[2]) * 57.29578f;
}
static void AttApply(uint32_t o, float pitchD, float rollD, float yawD) {    // main thread
    float* m = (float*)(uintptr_t)(o + 0x5cc); float* v = (float*)(uintptr_t)(o + 0x6f0);
    float th = pitchD * 0.0174533f, ph = rollD * 0.0174533f, ps = yawD * 0.0174533f;
    float f[3], rl[3], ul[3], r[3], u[3], vb[3]; int i;
    for (i = 0; i < 3; i++) vb[i] = v[0] * m[i * 4] + v[1] * m[i * 4 + 1] + v[2] * m[i * 4 + 2];   // velocity in the old body frame
    f[0] = -sinf(ps) * cosf(th); f[1] = cosf(ps) * cosf(th); f[2] = sinf(th);
    rl[0] = -cosf(ps); rl[1] = -sinf(ps); rl[2] = 0;
    ul[0] = sinf(th) * sinf(ps); ul[1] = -sinf(th) * cosf(ps); ul[2] = cosf(th);
    for (i = 0; i < 3; i++) { r[i] = rl[i] * cosf(ph) + ul[i] * sinf(ph); u[i] = ul[i] * cosf(ph) - rl[i] * sinf(ph); }
    for (i = 0; i < 3; i++) { m[i] = f[i]; m[4 + i] = r[i]; m[8 + i] = u[i]; v[i] = vb[0] * f[i] + vb[1] * r[i] + vb[2] * u[i]; }
    m[3] = m[7] = m[11] = 0; m[12] = m[13] = m[14] = 0; m[15] = 1;
    DeriveFromState(o);
    DelphiCall((uintptr_t)IdaAddrToRuntime(FN_MAT_QUAT), o + 0x5cc, o + 0x6a4, 0);
}
static void FreezeSet(int on) {
    g_freeze = on;
    RLog("freeze: %s (physics %s)", on ? "on" : "off", on ? "stopped, glider holds its place" : "running");
}
// once per frame on the main thread (HUD hook): hotkey Ctrl+Shift+F, pending attitude
static void FreezeFrame(void) {
    static int prev;
    int down = (GetAsyncKeyState('F') & 0x8000) && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000);
    if (down && !prev) FreezeSet(!g_freeze);
    prev = down;
    {   static int prevD;
        int d = (GetAsyncKeyState('D') & 0x8000) && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000);
        if (d && !prevD) DoDump(NULL);
        prevD = d; }
    if (g_gotoPending) GotoApply();
    if (g_attPending) {
        uint32_t o = GliderObj(); float p, r, y;
        g_attPending = 0;
        if (!o) { RLog("att: no glider object"); return; }
        AttApply(o, g_attSet[0], g_attSet[1], g_attSet[2]);
        AttGet((float*)(uintptr_t)(o + 0x5cc), &p, &r, &y);
        RLog("att: set pitch %.2f roll %.2f yaw %.2f%s", p, r, y, g_freeze ? "" : " (not frozen: physics continues from here)");
    }
}
static void DoAtt(const char* line) {
    float a[3]; uint32_t o = GliderObj();
    if (sscanf(line, "%*s %f %f %f", &a[0], &a[1], &a[2]) == 3) { memcpy(g_attSet, a, sizeof a); g_attPending = 1; return; }
    if (!o) { RLog("att: no glider object"); return; }
    {   float p, r, y; double* pos = (double*)(uintptr_t)(o + 0x6d8); float* v = (float*)(uintptr_t)(o + 0x6f0);
        AttGet((float*)(uintptr_t)(o + 0x5cc), &p, &r, &y);
        RLog("att: pitch %.2f roll %.2f yaw %.2f pos %.2f %.2f %.2f speed %.1f m/s frozen %d", p, r, y, pos[0], pos[1], pos[2],
             sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), g_freeze); }
}

// dump [NAME]: append "NAME x y z pitch roll yaw" to <game>\postfx\positions.txt (same columns as a views file for viewshots.sh /
// flytest.sh, plus the attitude). Hotkey Ctrl+Shift+D. Default name: pos_HHMMSS.
static void DoDump(const char* name) {
    char path[MAX_PATH], nm[64]; uint32_t o = GliderObj(); FILE* f; int isNew; float p, r, y; double* pos;
    if (!o) { RLog("dump: no glider object"); return; }
    if (name && name[0]) { strncpy(nm, name, sizeof nm - 1); nm[sizeof nm - 1] = 0; }
    else { SYSTEMTIME t; GetLocalTime(&t); _snprintf(nm, sizeof nm, "pos_%02d%02d%02d", t.wHour, t.wMinute, t.wSecond); }
    pos = (double*)(uintptr_t)(o + 0x6d8);
    AttGet((float*)(uintptr_t)(o + 0x5cc), &p, &r, &y);
    _snprintf(path, sizeof path, "%s\\positions.txt", g_dir);
    isNew = GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES;
    f = fopen(path, "a");
    if (!f) { RLog("dump: cannot write %s", path); return; }
    if (isNew) fprintf(f, "# name  x(west) y(north) z(alt) pitch roll yaw  [camera: x y z pitch roll yaw]   (glider, then the rendered camera;"
                          " metres / degrees; usable as a views file for scripts/viewshots.sh)\n");
    {   // the camera the game renders with: camera manager *(0x7D96F8) +0x148 = rows right, up, forward, position (render frame =
        // world minus the floating origin at landscape +0x1c9 / +0x1cd, see FUN_00761014)
        uint32_t cm = 0, L = LandscapeObj(); const float* w; float cam[16], cp, cr, cy, ox = 0, oy = 0;
        if (Rd32((uintptr_t)IdaAddrToRuntime(0x7D96F8), &cm) && cm && !IsBadReadPtr((void*)(uintptr_t)(cm + 0x148), 64)) {
            w = (const float*)(uintptr_t)(cm + 0x148);
            if (L && !IsBadReadPtr((void*)(uintptr_t)(L + 0x1c9), 8)) { memcpy(&ox, (void*)(uintptr_t)(L + 0x1c9), 4); memcpy(&oy, (void*)(uintptr_t)(L + 0x1cd), 4); }
            memset(cam, 0, sizeof cam);
            memcpy(cam, w + 8, 12); memcpy(cam + 4, w, 12); memcpy(cam + 8, w + 4, 12);        // nose = forward, side = right, up
            AttGet(cam, &cp, &cr, &cy);
            fprintf(f, "%s %.2f %.2f %.2f %.2f %.2f %.2f  %.2f %.2f %.2f %.2f %.2f %.2f\n", nm, pos[0], pos[1], pos[2], p, r, y,
                    w[12] + ox, w[13] + oy, w[14], cp, cr, cy);
        } else fprintf(f, "%s %.2f %.2f %.2f %.2f %.2f %.2f\n", nm, pos[0], pos[1], pos[2], p, r, y);
    }
    fclose(f);
    RLog("dump: %s %.2f %.2f %.2f %.2f %.2f %.2f -> positions.txt", nm, pos[0], pos[1], pos[2], p, r, y);
}

// goto NAME | goto X Y Z [PITCH ROLL YAW]: absolute position (+ attitude). NAME = the last line with that name in positions.txt.
// Applied on the main thread with the next rendered frame. Best used frozen (a paused game may not redraw).
static double g_gotoPos[3]; static float g_gotoAtt[3]; static int g_gotoHasAtt; static char g_gotoName[64];
static void DoGoto(const char* line) {
    char nm[64] = "", path[MAX_PATH], buf[256]; double p[3]; float a[3] = {0, 0, 0}; int n;
    n = sscanf(line, "%*s %lf %lf %lf %f %f %f", &p[0], &p[1], &p[2], &a[0], &a[1], &a[2]);
    if (n >= 3) strcpy(nm, "(numbers)");
    else {
        FILE* f; int found = 0;
        sscanf(line, "%*s %63s", nm);
        if (!nm[0]) { RLog("goto: usage goto NAME | goto X Y Z [PITCH ROLL YAW]"); return; }
        _snprintf(path, sizeof path, "%s\\positions.txt", g_dir);
        f = fopen(path, "r");
        if (!f) { RLog("goto: no %s", path); return; }
        while (fgets(buf, sizeof buf, f)) {
            char k[64]; double q[3]; float b[3]; int m;
            if (buf[0] == '#') continue;
            m = sscanf(buf, "%63s %lf %lf %lf %f %f %f", k, &q[0], &q[1], &q[2], &b[0], &b[1], &b[2]);
            if (m >= 4 && !strcmp(k, nm)) { memcpy(p, q, sizeof p); n = m - 1; if (m >= 7) memcpy(a, b, sizeof a); found = 1; }
        }
        fclose(f);
        if (!found) { RLog("goto: '%s' not in positions.txt", nm); return; }
    }
    memcpy(g_gotoPos, p, sizeof p); memcpy(g_gotoAtt, a, sizeof a); g_gotoHasAtt = n >= 6; strcpy(g_gotoName, nm);
    g_gotoPending = 1;
}
static void GotoApply(void) {                                   // main thread
    uint32_t o = GliderObj(); double* pos;
    g_gotoPending = 0;
    if (!o) { RLog("goto: no glider object"); return; }
    pos = (double*)(uintptr_t)(o + 0x6d8);
    pos[0] = g_gotoPos[0]; pos[1] = g_gotoPos[1]; pos[2] = g_gotoPos[2];
    if (g_gotoHasAtt) AttApply(o, g_gotoAtt[0], g_gotoAtt[1], g_gotoAtt[2]); else DeriveFromState(o);
    RLog("goto %s: pos %.2f %.2f %.2f%s", g_gotoName, pos[0], pos[1], pos[2], g_gotoHasAtt ? " + attitude" : "");
}

// scan X Y Z [TOL]: find three consecutive floats near X,Y,Z in the process' writable memory (to find object positions)
static void DoScan(float x, float y, float z, float tol) {
    MEMORY_BASIC_INFORMATION mbi; uintptr_t a = 0x10000; int hits = 0;
    while (a < 0x7ff00000 && hits < 40) {
        if (!VirtualQuery((void*)a, &mbi, sizeof mbi)) break;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) && !(mbi.Protect & PAGE_GUARD)) {
            const float* f = (const float*)mbi.BaseAddress; size_t n = mbi.RegionSize / 4, i;
            for (i = 0; i + 2 < n && hits < 40; i++)
                if (fabsf(f[i] - x) < tol && fabsf(f[i + 1] - y) < tol && fabsf(f[i + 2] - z) < tol) {
                    RLog("scan hit %p: %.3f %.3f %.3f", (void*)&f[i], f[i], f[i + 1], f[i + 2]); hits++;
                }
        }
        a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    RLog("scan done, %d hits", hits);
}

// scand X Y Z [TOL]: same as scan, for three consecutive doubles
static void DoScanD(double x, double y, double z, double tol) {
    MEMORY_BASIC_INFORMATION mbi; uintptr_t a = 0x10000; int hits = 0;
    while (a < 0x7ff00000 && hits < 40) {
        if (!VirtualQuery((void*)a, &mbi, sizeof mbi)) break;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) && !(mbi.Protect & PAGE_GUARD)) {
            const double* f = (const double*)mbi.BaseAddress; size_t n = mbi.RegionSize / 8, i;
            for (i = 0; i + 2 < n && hits < 40; i++)
                if (fabs(f[i] - x) < tol && fabs(f[i + 1] - y) < tol && fabs(f[i + 2] - z) < tol) {
                    RLog("scand hit %p: %.4f %.4f %.4f", (void*)&f[i], f[i], f[i + 1], f[i + 2]); hits++;
                }
        }
        a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    RLog("scand done, %d hits", hits);
}

// real (injected) keyboard / mouse-look: keybd_event reaches the game like a physical key press
static void InjKey(int vk, int down) {
    keybd_event((BYTE)vk, (BYTE)MapVirtualKeyA(vk, 0), down ? 0 : KEYEVENTF_KEYUP, 0);
}
static void DoMouseDrag(int dx, int dy) {      // hold the left button in the middle of the game window and move by dx,dy
    POINT o = {0, 0}; RECT r; int i, cx, cy;
    if (!g_main) return;
    GetClientRect(g_main, &r); ClientToScreen(g_main, &o);
    cx = o.x + r.right / 2; cy = o.y + r.bottom / 2;
    SetCursorPos(cx, cy); Sleep(100);
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0); Sleep(100);
    for (i = 1; i <= 20; i++) { SetCursorPos(cx + dx * i / 20, cy + dy * i / 20); Sleep(25); }
    Sleep(100); mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    RLog("mouse drag %d,%d", dx, dy);
}

// ---- software write watchpoint (PAGE_GUARD + vectored exception handler) --------------------------------------
// watch ADDR LEN: logs the code address (EIP) of every write into [ADDR, ADDR+LEN) until `unwatch`. Used to find the engine code
// that computes a value (e.g. the glider's world matrix).
static volatile uintptr_t g_wAddr = 0, g_wLen = 0, g_wPage = 0; static volatile int g_wRearm = 0;
static uintptr_t g_wSeen[64]; static int g_wSeenN = 0; static volatile long g_wHits = 0;
static LONG CALLBACK WatchVeh(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == STATUS_GUARD_PAGE_VIOLATION && g_wPage) {
        uintptr_t a = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1], eip = (uintptr_t)ep->ContextRecord->Eip;
        if ((a & ~0xFFFu) == g_wPage) {
            if (ep->ExceptionRecord->ExceptionInformation[0] == 1 && a >= g_wAddr && a < g_wAddr + g_wLen) {
                int i, seen = 0;
                InterlockedIncrement(&g_wHits);
                for (i = 0; i < g_wSeenN; i++) if (g_wSeen[i] == eip) seen = 1;
                if (!seen && g_wSeenN < 64) { g_wSeen[g_wSeenN++] = eip; RLog("watch: write to %p from code %p (ida %p) esp=%p", (void*)a, (void*)eip, (void*)(eip - (uintptr_t)IdaAddrToRuntime(0x400000) + 0x400000), (void*)ep->ContextRecord->Esp); }
            }
            g_wRearm = 1; ep->ContextRecord->EFlags |= 0x100;     // single step, then re-arm
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (code == EXCEPTION_SINGLE_STEP && g_wRearm) {
        DWORD old; g_wRearm = 0;
        if (g_wPage) VirtualProtect((void*)g_wPage, 0x1000, PAGE_READWRITE | PAGE_GUARD, &old);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static void DoWatch(uintptr_t addr, int len) {
    static int vehAdded = 0; DWORD old;
    if (!vehAdded) { AddVectoredExceptionHandler(1, WatchVeh); vehAdded = 1; }
    g_wSeenN = 0; g_wHits = 0; g_wAddr = addr; g_wLen = len; g_wPage = addr & ~0xFFFu;
    VirtualProtect((void*)g_wPage, 0x1000, PAGE_READWRITE | PAGE_GUARD, &old);
    RLog("watch armed on page %p for [%p, +%d)", (void*)g_wPage, (void*)addr, len);
}
static void DoUnwatch(void) {
    DWORD old; uintptr_t pg = g_wPage; g_wPage = 0;
    if (pg) VirtualProtect((void*)pg, 0x1000, PAGE_READWRITE, &old);
    RLog("watch off, %ld writes seen, %d distinct writers", g_wHits, g_wSeenN);
}
static void LogState(void) {
    uint32_t sp = 0, len = 0; char txt[128] = "";
    static unsigned lastFrames = 0; unsigned f = g_frames;
    if (Rd32((uintptr_t)IdaAddrToRuntime(ADDR_STATUS_TEXT), &sp) && sp && Rd32(sp - 4, &len) && len < 100 && !IsBadReadPtr((void*)(uintptr_t)sp, len + 1)) {
        memcpy(txt, (void*)(uintptr_t)sp, len); txt[len] = 0;
    }
    Sleep(500);   // is the frame hook still running?  (flight / rendering active)
    RLog("state: status='%s' flying=%d frames=%u postfx=%s core=%s", txt, g_frames != f, g_frames, g_enabled ? "on" : "off", g_core ? "ready" : "none");
    (void)lastFrames;
    g_main = NULL; EnumWindows(EnumW, 1);
}
// vm: summary of the 32-bit address space (committed by type, reserved, free, largest free block)
static void DoVmStat(const char* tag) {
    MEMORY_BASIC_INFORMATION mbi; uintptr_t a = 0x10000;
    unsigned long long img = 0, map = 0, priv = 0, res = 0, freeb = 0, big = 0;
    while (a < 0xFFFF0000u) {
        if (!VirtualQuery((void*)a, &mbi, sizeof mbi)) break;
        if (mbi.State == MEM_FREE) { freeb += mbi.RegionSize; if (mbi.RegionSize > big) big = mbi.RegionSize; }
        else if (mbi.State == MEM_RESERVE) res += mbi.RegionSize;
        else if (mbi.Type == MEM_IMAGE) img += mbi.RegionSize;
        else if (mbi.Type == MEM_MAPPED) map += mbi.RegionSize;
        else priv += mbi.RegionSize;
        if ((uintptr_t)mbi.BaseAddress + mbi.RegionSize <= a) break;
        a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    RLog("vm %s: frame %u commit private %llu MB, mapped %llu MB, image %llu MB, reserved %llu MB, free %llu MB (largest %llu MB)",
         tag, g_frames, priv >> 20, map >> 20, img >> 20, res >> 20, freeb >> 20, big >> 20);
    if (!strcmp(tag, "cmd")) {   // committed private memory by allocation (region group) size: big DXVK chunks vs heap blocks
        static const unsigned long long lim[6] = { 1ull << 20, 4ull << 20, 16ull << 20, 64ull << 20, 256ull << 20, ~0ull };
        unsigned long long sum[6] = {0}; unsigned cnt[6] = {0}; uintptr_t base = 0, cur = 0x10000; unsigned long long asz = 0;
        for (;;) {
            int k; int done = !VirtualQuery((void*)cur, &mbi, sizeof mbi) || (uintptr_t)mbi.BaseAddress + mbi.RegionSize <= cur || cur >= 0xFFFF0000u;
            if (done || (uintptr_t)mbi.AllocationBase != base) {          // close the previous allocation
                if (base && asz) { for (k = 0; asz > lim[k]; k++); sum[k] += asz; cnt[k]++; }
                if (done) break;
                base = (uintptr_t)mbi.AllocationBase; asz = 0;
            }
            if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE) asz += mbi.RegionSize;
            cur = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        }
        RLog("vm sizes (allocations: count/MB): <1M %u/%llu, <4M %u/%llu, <16M %u/%llu, <64M %u/%llu, <256M %u/%llu, more %u/%llu",
             cnt[0], sum[0] >> 20, cnt[1], sum[1] >> 20, cnt[2], sum[2] >> 20, cnt[3], sum[3] >> 20, cnt[4], sum[4] >> 20, cnt[5], sum[5] >> 20);
    }
}
static void DoMouseClick(int x, int y) {   // real (injected) input at client coordinates of the main window
    POINT o = {0, 0};
    if (!g_main) { RLog("mclick: no main window"); return; }
    ClientToScreen(g_main, &o);
    SetCursorPos(o.x + x, o.y + y);
    Sleep(80);
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    Sleep(80);
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    RLog("mouse click at screen %ld,%ld", o.x + x, o.y + y);
}
static volatile int g_vmAuto;
static DWORD WINAPI RemoteThread(LPVOID unused) {
    char path[MAX_PATH], line[256];
    (void)unused;
    _snprintf(path, sizeof path, "%s\\cmd.txt", g_dir);
    for (;;) {
        FILE* f; char* lines[64]; int n = 0, i;
        static unsigned tick = 0;
        Sleep(200);
        if (g_vmAuto && ++tick % 50 == 0) DoVmStat("auto");         // every ~10 s, only when switched on (`vm auto on`): walking the
                                                                    // address space takes Wine's memory lock

        if (g_fxHookState == 0) {                                   // Effects11 DLL was not loaded when we started: retry
            g_fxHookState = InstallFxHook();
            if (g_fxHookState != 0 && g_logf) { fprintf(g_logf, "postfx: Effects11 hook state %d (retry)\n", g_fxHookState); fflush(g_logf); }
        }
        f = fopen(path, "r");
        if (!f) continue;
        while (n < 64 && fgets(line, sizeof line, f)) { lines[n] = _strdup(line); n++; }
        fclose(f);
        DeleteFileA(path);
        g_main = NULL; EnumWindows(EnumW, 0);
        for (i = 0; i < n; i++) {
            char c[32] = {0}, a[64] = {0}; int x = 0, y = 0, cnt = 1, k;
            sscanf(lines[i], "%31s %63s %d", c, a, &cnt);
            RLog("cmd: %s", lines[i]);
            if (!_stricmp(c, "windows")) { g_main = NULL; EnumWindows(EnumW, 1); }
            else if (!_stricmp(c, "click")) { sscanf(lines[i], "%*s %d %d", &x, &y); DoClick(x, y); }
            else if (!_stricmp(c, "atmo")) {
                int q;
                RLog("atmo: valid=%d frame=%u (now %u)", g_atmo.valid, g_atmo.frame, g_frames);
                RLog("  effects hook %d, terrain effect %p, selfTest %d", g_fxHookState, g_fx.effect, g_fx.selfTest);
                RLog("  view row0 %.4f %.4f %.4f %.4f", g_atmo.view[0], g_atmo.view[1], g_atmo.view[2], g_atmo.view[3]);
                RLog("  camPos %.1f %.1f %.1f  fog %.3f %.3f %.3f %.3f", g_atmo.camPos[0], g_atmo.camPos[1], g_atmo.camPos[2], g_atmo.fog[0], g_atmo.fog[1], g_atmo.fog[2], g_atmo.fog[3]);
                RLog("  sunDir %.4f %.4f %.4f  sunCol %.3f %.3f %.3f %.3f  amb %.3f %.3f %.3f %.3f", g_atmo.sun[0], g_atmo.sun[1], g_atmo.sun[2], g_atmo.sunColor[0], g_atmo.sunColor[1], g_atmo.sunColor[2], g_atmo.sunColor[3], g_atmo.amb[0], g_atmo.amb[1], g_atmo.amb[2], g_atmo.amb[3]);
            }
            else if (!_stricmp(c, "state")) LogState();
            else if (!_stricmp(c, "vm")) { if (!_stricmp(a, "auto")) { g_vmAuto = cnt != 0; RLog("vm auto %s", g_vmAuto ? "on" : "off"); } else DoVmStat("cmd"); }
            else if (!_stricmp(c, "vmbig")) {   // every committed private allocation >= 4 MB: base, size, first bytes (who owns it?)
                MEMORY_BASIC_INFORMATION mb; uintptr_t cur = 0x10000, base = 0, first = 0; unsigned long long asz = 0; int nb = 0;
                for (;;) {
                    int done = !VirtualQuery((void*)cur, &mb, sizeof mb) || (uintptr_t)mb.BaseAddress + mb.RegionSize <= cur || cur >= 0xFFFF0000u;
                    if (done || (uintptr_t)mb.AllocationBase != base) {
                        if (base && asz >= (4u << 20) && first && nb < 80) {
                            const unsigned char* q = (const unsigned char*)first; char hx[100]; int o = 0, t;
                            for (t = 0; t < 24; t++) o += _snprintf(hx + o, sizeof hx - o, "%02x", q[t]);
                            RLog("big %p %llu KB: %s", (void*)base, asz >> 10, hx); nb++;
                        }
                        if (done) break;
                        base = (uintptr_t)mb.AllocationBase; asz = 0; first = 0;
                    }
                    if (mb.State == MEM_COMMIT && mb.Type == MEM_PRIVATE) { asz += mb.RegionSize; if (!first && !(mb.Protect & (PAGE_GUARD | PAGE_NOACCESS))) first = (uintptr_t)mb.BaseAddress; }
                    cur = (uintptr_t)mb.BaseAddress + mb.RegionSize;
                }
            }
#ifdef ENABLE_AGSTREAM
            else if (!_stricmp(c, "ag")) { void AgStream_Stats(void (*)(const char*, ...)); AgStream_Stats(RLog); }
#endif
            else if (!_stricmp(c, "cam")) {     // cam off | here | X Y Z YAW PITCH
                float v[5] = {0}; int nn;
                if (!_stricmp(a, "off")) { g_camFree = 0; RLog("cam: engine camera"); }
                else if (!_stricmp(a, "here")) { g_camLatch = 1; g_camFree = 1; Sleep(300); }
                else if ((nn = sscanf(lines[i], "%*s %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4])) >= 3) {
                    if (!g_camFree) { g_camLatch = 1; Sleep(300); g_camFree = 1; }
                    g_camPos[0] = v[0]; g_camPos[1] = v[1]; g_camPos[2] = v[2]; if (nn >= 4) g_camYaw = v[3]; if (nn >= 5) g_camPitch = v[4];
                    RLog("cam: pos %.1f %.1f %.1f yaw %.1f pitch %.1f", g_camPos[0], g_camPos[1], g_camPos[2], g_camYaw, g_camPitch);
                }
            }
            else if (!_stricmp(c, "camrel")) {  // camrel FWD RIGHT UP DYAW DPITCH (metres / degrees, relative to the current free camera)
                float v[5] = {0}, y; sscanf(lines[i], "%*s %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4]);
                if (!g_camFree) { g_camLatch = 1; Sleep(300); g_camFree = 1; }
                y = g_camYaw * 0.0174533f;
                g_camPos[0] += sinf(y) * v[0] + cosf(y) * v[1]; g_camPos[1] += cosf(y) * v[0] - sinf(y) * v[1]; g_camPos[2] += v[2];
                g_camYaw += v[3]; g_camPitch += v[4];
                RLog("cam: pos %.1f %.1f %.1f yaw %.1f pitch %.1f", g_camPos[0], g_camPos[1], g_camPos[2], g_camYaw, g_camPitch);
            }
            else if (!_stricmp(c, "scan")) { float v[4] = {0, 0, 0, 2.0f}; sscanf(lines[i], "%*s %f %f %f %f", &v[0], &v[1], &v[2], &v[3]); DoScan(v[0], v[1], v[2], v[3]); }
            else if (!_stricmp(c, "scand")) { float v[4] = {0, 0, 0, 2.0f}; sscanf(lines[i], "%*s %f %f %f %f", &v[0], &v[1], &v[2], &v[3]); DoScanD(v[0], v[1], v[2], v[3]); }
            else if (!_stricmp(c, "kkey")) { int vk = KeyVk(a); if (cnt < 1) cnt = 1; for (k = 0; k < cnt; k++) { InjKey(vk, 1); Sleep(80); InjKey(vk, 0); Sleep(150); } RLog("injected key %s", a); }
            else if (!_stricmp(c, "kdown")) InjKey(KeyVk(a), 1);
            else if (!_stricmp(c, "kup")) InjKey(KeyVk(a), 0);
            else if (!_stricmp(c, "mdrag")) { sscanf(lines[i], "%*s %d %d", &x, &y); DoMouseDrag(x, y); }
            else if (!_stricmp(c, "watch")) DoWatch(ResolveExpr(a), cnt > 1 ? cnt : 64);
            else if (!_stricmp(c, "unwatch")) DoUnwatch();
            else if (!_stricmp(c, "peek")) DoPeek(a, cnt > 0 && cnt != 1 ? cnt : 64);
            else if (!_stricmp(c, "freeze")) FreezeSet(!_stricmp(a, "on") ? 1 : !_stricmp(a, "off") ? 0 : !g_freeze);
            else if (!_stricmp(c, "att")) DoAtt(lines[i]);
            else if (!_stricmp(c, "dump")) DoDump(a);
            else if (!_stricmp(c, "goto")) DoGoto(lines[i]);
            else if (!_stricmp(c, "move")) { double m[3] = {0, 0, 0}; sscanf(lines[i], "%*s %lf %lf %lf", &m[0], &m[1], &m[2]); DoMove(m[0], m[1], m[2]); }
            else if (!_stricmp(c, "poke")) { float fv = 0; sscanf(lines[i], "%*s %*s %f", &fv); DoPoke(a, fv); }
            else if (!_stricmp(c, "shot")) { CreateDirectoryA(g_dir, NULL); { char d2[MAX_PATH]; _snprintf(d2, sizeof d2, "%s\\shots", g_dir); CreateDirectoryA(d2, NULL); _snprintf(g_shot, sizeof g_shot, "%s\\%s.bin", d2, a); } RLog("shot requested: %s", g_shot); }
            else if (!_stricmp(c, "mclick")) { sscanf(lines[i], "%*s %d %d", &x, &y); DoMouseClick(x, y); }
            else if (!_stricmp(c, "key")) { int vk = KeyVk(a); if (cnt < 1) cnt = 1; for (k = 0; k < cnt; k++) { KeyMsg(vk, 1); Sleep(60); KeyMsg(vk, 0); Sleep(120); } }
            else if (!_stricmp(c, "keydown")) KeyMsg(KeyVk(a), 1);
            else if (!_stricmp(c, "keyup")) KeyMsg(KeyVk(a), 0);
            else if (!_stricmp(c, "yawlayer")) { g_yawLayer = _stricmp(a, "off") != 0; RLog("yawlayer: %s", g_yawLayer ? "on" : "off"); }
            else if (!_stricmp(c, "post")) { g_enabled = _stricmp(a, "off") != 0; RLog("postfx: %s", g_enabled ? "on" : "off"); }
            else if (!_stricmp(c, "wait")) Sleep(atoi(a));
            free(lines[i]);
        }
    }
    return 0;
}

void InstallPostFX(void) {
    char exe[MAX_PATH], logp[MAX_PATH];
    char* slash;
    void* target;
    int r;

    GetModuleFileNameA(NULL, exe, MAX_PATH);
    slash = strrchr(exe, '\\');
    if (slash) *slash = 0;
    _snprintf(g_dir, sizeof g_dir, "%s\\postfx", exe);
    _snprintf(logp, sizeof logp, "%s\\postfx.log", g_dir);
    g_logf = fopen(logp, "w");
    if (g_logf) PfxSetLog(g_logf);

    g_fxHookState = InstallFxHook();
    if (g_logf) { fprintf(g_logf, "postfx: Effects11 hook state %d (1 = installed, 0 = DLL not loaded yet)\n", g_fxHookState); fflush(g_logf); }

    target = IdaAddrToRuntime(ADDR_HUD_DRAW);
    r = DobbyInstrument(target, HudPre);
    DBGLOG("postfx: hook on HUD draw (%p) -> %s (%d)", target, r == 0 ? "OK" : "FAILED", r);
    if (g_logf) { fprintf(g_logf, "postfx: hook on HUD draw %p -> %d\n", target, r); fflush(g_logf); }
    (void)r; (void)target;
    r = DobbyHook(IdaAddrToRuntime(FN_PHYSICS), (dobby_dummy_func_t)PhysicsHook, (dobby_dummy_func_t*)&g_origPhysics);
    if (g_logf) { fprintf(g_logf, "postfx: physics hook (freeze) -> %d\n", r); fflush(g_logf); }
    // ScenePre is not installed: instrumenting 0x73B59C crashes the game while loading a flight (see git log)
    CreateThread(NULL, 0, RemoteThread, NULL, 0, NULL);
}
