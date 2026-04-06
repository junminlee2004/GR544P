/*
 * GR544P v59 — Multi-type draw throttle
 *
 * v59: Extends particle throttle to distortion and effect shaders.
 *   Reads SceGxmFragmentProgram* from ctx+0x44 (confirmed v58d).
 *   Checks GXP type at SceGxmProgram+0x14, throttles by type:
 *
 *   PARTICLE_DIVISOR:    000030C1 / 000032C1  (29 shaders, fire/smoke)
 *   DISTORTION_DIVISOR:  00003041  (2 shaders, MBH/typhoon vortex warp)
 *   EFFECT_DIVISOR:      00001801  (12 shaders, attack debris passes)
 *
 *   MBH: 00003041 spikes from 2.5K baseline to 25K (10x).
 *   Typhoon: 00003041 spikes to 11.6K, 00001801 spikes 5.5x.
 *   Stasis: no draw-count spike (fullscreen quad, needs separate fix).
 *
 * Full LR→param map: see bottom of file.
 */

#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/display.h>
#include <psp2/gxm.h>
#include <taihen.h>

/* ================================================================
 *  LOD TUNING — all distances in game units (~meters)
 *
 *  Stock values ~50–1000. Lower = more aggressive LOD = better perf.
 *  Set to 0 to keep stock for that level.
 * ================================================================ */

/* ---- World geometry (buildings, trees, roads, terrain) ---------- */
#define ENABLE_WORLD_LOD    1
#define LOD_LEVEL1    25.0f
#define LOD_LEVEL2    65.0f
#define LOD_LEVEL3    85.0f    /* keep stock */
#define LOD_LEVEL4    200.0f
#define LOD_LEVEL5    325.0f
#define LOD_LEVEL6    350.0f
#define LOD_LEVEL7    400.0f

/* ---- Particles (effects, dust, sparks) -------------------------- */
#define ENABLE_PARTICLE_LOD 1
#define PLOD_LEVEL1        0.001f
#define PLOD_LEVEL2        0.001f
#define PLOD_LEVEL3        0.001f
#define PLOD_LEVEL4        0.001f
#define PLOD_LEVEL5        0.001f
#define PLOD_LEVEL6        0.001f
#define PLOD_LEVEL7        0.001f

/* ---- Characters (NPCs, enemies, Kat) ---------------------------- */
#define ENABLE_CHAR_LOD     1
#define CLOD_LEVEL1        7.0f
#define CLOD_LEVEL2        8.0f
#define CLOD_LEVEL3        9.0f
#define CLOD_LEVEL4        10.0f
#define CLOD_LEVEL5        10.0f
#define CLOD_LEVEL6        10.0f
#define CLOD_LEVEL7        10.0f

/* ================================================================
 *  SHADOW TUNING
 *
 *  Stock: FarMax1=100, FarMax2=60, NearMin=1, LightRange=300,
 *         PolyOffset=1, ZOffset=0.1, OutValue=0.5
 *  Set to 0 to keep stock for that param.
 * ================================================================ */
#define ENABLE_SHADOW       1
#define SHADOW_FARMAX1     100.0f
#define SHADOW_FARMAX2     50.0f
#define SHADOW_NEARMIN      0.0f    /* keep stock */
#define SHADOW_LIGHTRANGE  300.0f
#define SHADOW_POLYOFFSET   0.0f    /* keep stock */
#define SHADOW_ZOFFSET      0.0f    /* keep stock */
#define SHADOW_OUTVALUE     0.5f   /* keep stock */

/* ================================================================
 *  SKINNING — bone count per vertex
 *
 *  Stock: 4 bones per vertex (high quality blending).
 *  Set SKINNING_BONES to 2 for ~50% skinning cost reduction.
 *  Set to 1 for rigid (single-bone, cheapest, visible snapping).
 *  4 patch sites in the main skinning function at VA 0x8136AF78.
 * ================================================================ */
#define ENABLE_SKINNING_OPT 1
#define SKINNING_BONES      1       /* 1–4, stock=4 */

/* ================================================================
 *  GLOW — stasis sphere / effect glow reduction
 *
 *  GlowColor/Intensity: post-process glow brightness.
 *  Caller 0x81015E74 intercepted by hookParamLoader.
 *  Set GLOW_INTENSITY to 0 to keep stock.
 *
 *  NOTE: 0xB9C8/0xB9FA (180×102) is the OCCLUSION MAP, not glow RT.
 *  Do NOT downscale — breaks visibility culling and causes pop-in.
 * ================================================================ */
#define ENABLE_GLOW_OPT     1
#define GLOW_INTENSITY      0.5f    /* 0.0–1.0, stock ~1.0 */

/* ================================================================
 *  DRAW THROTTLE — reduce overdraw by GXP shader type
 *
 *  Reads SceGxmFragmentProgram* from ctx+0x44 on every sceGxmDraw.
 *  Checks GXP type at SceGxmProgram+0x14. Matching draws get their
 *  indexCount divided, keeping whole quads (aligned to 6 indices).
 *
 *  Set any divisor to 1 to disable that category.
 * ================================================================ */
#define ENABLE_DRAW_THROTTLE  1

/* Particle/effect: fire, smoke, sparks, glow (29 shaders) */
#define PARTICLE_DIVISOR      256

/* Distortion: MBH vortex warp, typhoon swirl (2 shaders)
 * These read full framebuffer per draw — extremely expensive. */
#define DISTORTION_DIVISOR    256

/* Effect passes: attack debris/energy renders (12 shaders)
 * 2.6x spike during MBH, 5.5x during typhoon. */
#define EFFECT_DIVISOR        1

/* ================================================================ */

#define EXPAND_RIGHT 25.0f
#define EXPAND_DOWN  0.7f
#define OFFSET_Y     -0.7f
#define TV_SCALE     3.0f

#define MAX_INJECT 20
static SceUID g_inject[MAX_INJECT];
static int    g_ninject;
static SceUID g_hook_id=-1, g_tv_hook_id=-1, g_param_hook_id=-1;
static tai_hook_ref_t g_hook_ref, g_tv_hook_ref, g_param_hook_ref;

#if ENABLE_DRAW_THROTTLE
static SceUID g_draw_hook_id=-1;
static tai_hook_ref_t g_draw_hook_ref;
#define FP_CTX_OFF 0x44
#endif

static inline void inject(SceUID mod, uint32_t off, const void *d, size_t n) {
    if (g_ninject < MAX_INJECT) {
        SceUID id = taiInjectData(mod, 0, off, d, n);
        if (id >= 0) g_inject[g_ninject++] = id;
    }
}

/* ---- CORRECTED caller tables (v52, verified via MOVW/BL decode) ---- */

static const uint32_t lod_callers[7] = {
    0x81016156, 0x81016168, 0x8101617A, 0x8101618C,
    0x8101619E, 0x810161B0, 0x810161C2
};
static const float lod_values[7] = {
    LOD_LEVEL1, LOD_LEVEL2, LOD_LEVEL3, LOD_LEVEL4,
    LOD_LEVEL5, LOD_LEVEL6, LOD_LEVEL7
};

static const uint32_t plod_callers[7] = {
    0x810161D4, 0x810161E6, 0x810161F8, 0x8101620A,
    0x8101621C, 0x8101622E, 0x81016240
};
static const float plod_values[7] = {
    PLOD_LEVEL1, PLOD_LEVEL2, PLOD_LEVEL3, PLOD_LEVEL4,
    PLOD_LEVEL5, PLOD_LEVEL6, PLOD_LEVEL7
};

static const uint32_t clod_callers[7] = {
    0x81016252, 0x81016264, 0x81016276, 0x81016288,
    0x8101629A, 0x810162AC, 0x810162BE
};
static const float clod_values[7] = {
    CLOD_LEVEL1, CLOD_LEVEL2, CLOD_LEVEL3, CLOD_LEVEL4,
    CLOD_LEVEL5, CLOD_LEVEL6, CLOD_LEVEL7
};

static const uint32_t shd_callers[7] = {
    0x81015D9C,  /* Shadow/AutoAdjust   */
    0x81015DAE,  /* Shadow/FarMax1      */
    0x81015DC0,  /* Shadow/FarMax2      */
    0x81015DD2,  /* Shadow/NearMin      */
    0x81015DE4,  /* Shadow/LightRange   */
    0x81015DF6,  /* Shadow/PolygonOffset */
    0x81015E08   /* Shadow/ZOffset      */
};
static const float shd_values[7] = {
    0.0f, SHADOW_FARMAX1, SHADOW_FARMAX2, SHADOW_NEARMIN,
    SHADOW_LIGHTRANGE, SHADOW_POLYOFFSET, SHADOW_ZOFFSET
};

static int hookParamLoader(uint32_t r0, uint32_t r1, uint32_t r2)
{
    uint32_t lr;
    __asm__ volatile("mov %0, lr" : "=r"(lr));

    int ret = TAI_CONTINUE(int, g_param_hook_ref, r0, r1, r2);

    uint32_t caller = lr & ~1u;

    #if ENABLE_WORLD_LOD
    for (int i = 0; i < 7; i++) {
        if (caller == lod_callers[i] && lod_values[i] > 0.0f && r2) {
            *(volatile float *)r2 = lod_values[i];
            return ret;
        }
    }
    #endif

    #if ENABLE_PARTICLE_LOD
    for (int i = 0; i < 7; i++) {
        if (caller == plod_callers[i] && plod_values[i] > 0.0f && r2) {
            *(volatile float *)r2 = plod_values[i];
            return ret;
        }
    }
    #endif

    #if ENABLE_CHAR_LOD
    for (int i = 0; i < 7; i++) {
        if (caller == clod_callers[i] && clod_values[i] > 0.0f && r2) {
            *(volatile float *)r2 = clod_values[i];
            return ret;
        }
    }
    #endif

    #if ENABLE_SHADOW
    for (int i = 0; i < 7; i++) {
        if (caller == shd_callers[i] && shd_values[i] > 0.0f && r2) {
            *(volatile float *)r2 = shd_values[i];
            return ret;
        }
    }
    #endif

    #if ENABLE_GLOW_OPT
    if (caller == 0x81015E74 && GLOW_INTENSITY > 0.0f && r2) {
        *(volatile float *)r2 = GLOW_INTENSITY;
        return ret;
    }
    #endif

    return ret;
}

static int hookTransform(uint32_t r0) {
    uint32_t lr; __asm__ volatile("mov %0, lr":"=r"(lr));
    if (!r0) return TAI_CONTINUE(int,g_hook_ref,r0);
    switch (lr) {
        case 0x813B3F61: case 0x813B5181: {
            volatile float *f=(volatile float*)r0;
            float s0=f[0],s1=f[1]; f[1]=s1+OFFSET_Y;
            int ret=TAI_CONTINUE(int,g_hook_ref,r0);
            f[0]=s0;f[1]=s1;return ret;}
        case 0x813B3F85: case 0x813B51AD: {
            volatile float *f=(volatile float*)r0;
            float s0=f[0],s1=f[1]; f[0]=s0+EXPAND_RIGHT;f[1]=s1-EXPAND_DOWN+OFFSET_Y;
            int ret=TAI_CONTINUE(int,g_hook_ref,r0);
            f[0]=s0;f[1]=s1;return ret;}
        default: return TAI_CONTINUE(int,g_hook_ref,r0);
    }
}
static int hookTextViewer(uint32_t r0) {
    if (!r0) return TAI_CONTINUE(int,g_tv_hook_ref,r0);
    volatile float *f=(volatile float*)r0;
    float sx=f[0x28/4],sy=f[0x2C/4],sw=f[0x30/4],sh=f[0x34/4];
    float dw=sw*(TV_SCALE-1.0f);
    f[0x30/4]=sw+dw;f[0x34/4]=sh*TV_SCALE;f[0x28/4]=sx-dw*0.5f;
    int ret=TAI_CONTINUE(int,g_tv_hook_ref,r0);
    f[0x28/4]=sx;f[0x2C/4]=sy;f[0x30/4]=sw;f[0x34/4]=sh;
    return ret;
}

#if ENABLE_DRAW_THROTTLE
static int hookDraw(SceGxmContext *ctx,
                    SceGxmPrimitiveType primType,
                    SceGxmIndexFormat   idxFmt,
                    const void         *idxData,
                    unsigned int        idxCount)
{
    if (idxCount > 6) {
        const SceGxmFragmentProgram *fp =
            *(const SceGxmFragmentProgram **)((uintptr_t)ctx + FP_CTX_OFF);
        if (fp) {
            const SceGxmProgram *prog = sceGxmFragmentProgramGetProgram(fp);
            if (prog) {
                uint32_t type = *(const uint32_t *)((uintptr_t)prog + 0x14);
                int div = 0;
                switch (type) {
                #if PARTICLE_DIVISOR > 1
                case 0x000030C1u:           /* particle/effect        */
                case 0x000032C1u:           /* particle+alpha         */
                    div = PARTICLE_DIVISOR;
                    break;
                #endif
                #if DISTORTION_DIVISOR > 1
                case 0x00003041u:           /* MBH/typhoon distortion */
                    div = DISTORTION_DIVISOR;
                    break;
                #endif
                #if EFFECT_DIVISOR > 1
                case 0x00001801u:           /* attack effect passes   */
                    div = EFFECT_DIVISOR;
                    break;
                #endif
                }
                if (div > 1) {
                    idxCount /= div;
                    idxCount -= idxCount % 6;
                    if (idxCount < 6) idxCount = 6;
                }
            }
        }
    }
    return TAI_CONTINUE(int, g_draw_hook_ref, ctx, primType, idxFmt,
                        idxData, idxCount);
}
#endif
static void patch_gxp(uint32_t *s1, int memsz) {
    (void)memsz;
    s1[(0x72954+0x208)/4]=0x3A888889u;s1[(0x72954+0x21C)/4]=0x3A888889u;
    s1[(0x72954+0x224)/4]=0x3A888889u;s1[(0x72954+0x228)/4]=0x3A888889u;
    s1[(0x72954+0x210)/4]=0x3AF0F0F1u;s1[(0x72954+0x220)/4]=0x3AF0F0F1u;
    s1[(0x72C28+0x238)/4]=0x3A888889u;s1[(0x72C28+0x24C)/4]=0x3A888889u;
    s1[(0x72C28+0x254)/4]=0x3A888889u;s1[(0x72C28+0x258)/4]=0x3A888889u;
    s1[(0x72C28+0x240)/4]=0x3AF0F0F1u;s1[(0x72C28+0x250)/4]=0x3AF0F0F1u;
    s1[(0x3BBC0+0x158)/4]=0x3B088889u;s1[(0x3BBC0+0x19C)/4]=0x3B088889u;
    s1[(0x3BBC0+0x1A4)/4]=0x3B088889u;s1[(0x3BBC0+0x1A8)/4]=0x3B088889u;
    s1[(0x3BBC0+0x160)/4]=0x3B70F0F1u;s1[(0x3BBC0+0x1A0)/4]=0x3B70F0F1u;
    s1[(0x737DC+0x2E8)/4]=0x3A888889u;s1[(0x737DC+0x314)/4]=0x3A888889u;
    s1[(0x737DC+0x31C)/4]=0x3A888889u;s1[(0x737DC+0x320)/4]=0x3A888889u;
    s1[(0x737DC+0x2F0)/4]=0x3AF0F0F1u;s1[(0x737DC+0x318)/4]=0x3AF0F0F1u;
    s1[(0x7440C+0x2F0)/4]=0x3A888889u;s1[(0x7440C+0x30C)/4]=0x3A888889u;
    s1[(0x7440C+0x314)/4]=0x3A888889u;s1[(0x7440C+0x318)/4]=0x3A888889u;
    s1[(0x7440C+0x2F8)/4]=0x3AF0F0F1u;s1[(0x7440C+0x310)/4]=0x3AF0F0F1u;
}

void _start() __attribute__((weak, alias("module_start")));
int module_start(SceSize args, void *argp) {
    (void)args;(void)argp; g_ninject=0;
    tai_module_info_t info; info.size=sizeof(info);
    if (taiGetModuleInfo(TAI_MAIN_MODULE,&info)<0) return SCE_KERNEL_START_FAILED;
    SceUID mod=info.modid;
    SceKernelModuleInfo mi; sceClibMemset(&mi,0,sizeof(mi)); mi.size=sizeof(mi);
    if (sceKernelGetModuleInfo(mod,&mi)<0) return SCE_KERNEL_START_FAILED;

    /* ---- FB resolution: 720×408 → 960×544 ----------------------- */
    /* fb0 @ 0x00284: width=960, store, height=544  (10 bytes)       */
    static const uint8_t fb0[]={
        0x5F,0xF4,0x70,0x70,              /* movs.w r0, #960           */
        0x1A,0x90,                         /* str    r0, [sp, #0x68]    */
        0x5F,0xF4,0x08,0x70               /* movs.w r0, #544           */
    };
    inject(mod, 0x00284, fb0, sizeof(fb0));

    /* fb1+w_r3 @ 0x45E9C: merged late FB init (44 bytes, was 2 injects)
     * bytes 0–13:  height=544, store, stride=0x6994, width=960
     * bytes 14–39: stock code (untouched, bridges the gap)
     * bytes 40–43: width=960 in r3                                  */
    static const uint8_t fb1m[]={
        0x5F,0xF4,0x08,0x70,              /* movs.w r0, #544           */
        0x00,0x90,                         /* str    r0, [sp]           */
        0x46,0xF2,0x94,0x55,              /* movw   r5, #0x6994(stride)*/
        0x5F,0xF4,0x70,0x70,              /* movs.w r0, #960           */
        /* --- stock bytes 0x45EAA–0x45EC3 (26 bytes, unchanged) --- */
        0x01,0x90,0x43,0xF6,0xC4,0x36,
        0xC8,0xF2,0x52,0x15,0x28,0x68,
        0xC8,0xF2,0x68,0x16,0x03,0x94,
        0x03,0x22,0x31,0x1C,
        0xC0,0xF2,0x10,0x02,
        /* --- w_r3 @ 0x45EC4 ------------------------------------ */
        0x5F,0xF4,0x70,0x73               /* movs.w r3, #960           */
    };
    inject(mod, 0x45E9C, fb1m, sizeof(fb1m));

    /* fb2 @ 0x06974: IB init (10 bytes)                             */
    static const uint8_t fb2[]={
        0x5F,0xF4,0x70,0x71,              /* movs.w r1, #960           */
        0x01,0x60,                         /* str    r1, [r0]           */
        0x5F,0xF4,0x08,0x71               /* movs.w r1, #544           */
    };
    inject(mod, 0x06974, fb2, sizeof(fb2));

    /* ---- IRB: internal render buffer 960×544 -------------------- */
    static const uint8_t irb[]={
        0x5F,0xF4,0x08,0x79,              /* movs.w r9,  #544          */
        0xCD,0xF8,0x00,0x90,              /* str.w  r9,  [sp]          */
        0x5F,0xF4,0x70,0x7A,              /* movs.w r10, #960          */
        0xCD,0xF8,0x04,0xA0,              /* str.w  r10, [sp, #4]      */
        0x5F,0xF0,0x00,0x0B,              /* movs.w r11, #0   (depth)  */
        0xCD,0xF8,0x0C,0xB0,              /* str.w  r11, [sp, #0xC]    */
        0x21,0x1C,                         /* movs   r1,  r4            */
        0x5F,0xF0,0x80,0x72,              /* movs.w r2,  #0x1000000    */
        0x5F,0xF4,0x70,0x73               /* movs.w r3,  #960          */
    };
    inject(mod, 0x0C46C, irb, sizeof(irb));

    /* w_r3 @ 0x0C4F6 (4 bytes)                                     */
    static const uint8_t w_r3[]={0x5F,0xF4,0x70,0x73};
    inject(mod, 0x0C4F6, w_r3, 4);

    /* ---- MSAA: force disable ------------------------------------ */
    { static const uint8_t sd[]={0x5F,0xF0,0x10,0x00};
      inject(mod, 0x05534, sd, 4); }

    /* ---- Scroll: VFP scroll speed for 960-space ----------------- */
    static const uint8_t scroll[]={
        0xF5,0xEE,0x00,0x0A,              /* vsub.f32 s0, s30, s0      */
        0x38,0xEE,0x80,0x0A,              /* vsub.f32 s0, s16, s0      */
        0x20,0xEE,0x20,0x0A,              /* vmul.f32 s0, s0,  s1      */
        0x86,0xED,0x0D,0x0A               /* vstr     s0, [r6, #0x34]  */
    };
    inject(mod, 0x3B52EC, scroll, sizeof(scroll));

    /* ---- Text cull: disable 75% width clipping ------------------ */
    static const uint8_t cull01[]={
        0x00,0xBF,                         /* nop                       */
        0x98,0xED,0x08,0x0A,              /* vldr    s0, [r8, #0x20]   */
        0xD8,0xED,0x0D,0x0A,              /* vldr    s1, [r8, #0x34]   */
        0xB4,0xEE,0x60,0x0A,              /* vcmp.f32 s0, s1           */
        0xF1,0xEE,0x10,0xFA,              /* vmrs    APSR_nzcv, FPSCR  */
        0x00,0xDC,                         /* bgt     +0                */
        0x00,0xBF                          /* nop                       */
    };
    static const uint8_t nop2[]={0x00,0xBF};
    inject(mod, 0x3B3CDC, cull01, sizeof(cull01));
    inject(mod, 0x3B549C, nop2, 2);
    inject(mod, 0x3B54E8, nop2, 2);
    inject(mod, 0x3B5524, nop2, 2);

    /* ---- GXP: shader resolution constants ----------------------- */
    patch_gxp((uint32_t*)mi.segments[1].vaddr, mi.segments[1].memsz);

    /* ---- Skinning: reduce bone count per vertex ----------------- */
    #if ENABLE_SKINNING_OPT && SKINNING_BONES < 4
    {   static const uint8_t skin[] = {SKINNING_BONES-1, 0x22};
        inject(mod, 0x36B236, skin, 2);   /* path A position           */
        inject(mod, 0x36B274, skin, 2);   /* path A normal             */
        inject(mod, 0x36B5F0, skin, 2);   /* path B position           */
        inject(mod, 0x36B636, skin, 2);   /* path B normal             */
    }
    #endif

    /* ---- Hooks -------------------------------------------------- */
    g_hook_id=taiHookFunctionOffset(&g_hook_ref,mod,0,0x11469C,1,(void*)hookTransform);
    g_tv_hook_id=taiHookFunctionOffset(&g_tv_hook_ref,mod,0,0x382130,1,(void*)hookTextViewer);
    g_param_hook_id=taiHookFunctionOffset(&g_param_hook_ref,mod,0,0x19230,1,(void*)hookParamLoader);

    #if ENABLE_DRAW_THROTTLE
    g_draw_hook_id=taiHookFunctionImport(&g_draw_hook_ref,
        TAI_MAIN_MODULE, TAI_ANY_LIBRARY,
        0xBC059AFC, (const void *)hookDraw);
    #endif

    return SCE_KERNEL_START_SUCCESS;
}
int module_stop(SceSize args, void *argp) {
    (void)args;(void)argp;
    if (g_hook_id>=0) taiHookRelease(g_hook_id,g_hook_ref);
    if (g_tv_hook_id>=0) taiHookRelease(g_tv_hook_id,g_tv_hook_ref);
    if (g_param_hook_id>=0) taiHookRelease(g_param_hook_id,g_param_hook_ref);
    #if ENABLE_DRAW_THROTTLE
    if (g_draw_hook_id>=0) taiHookRelease(g_draw_hook_id,g_draw_hook_ref);
    #endif
    for (int i=0;i<g_ninject;i++) if (g_inject[i]>=0) taiInjectRelease(g_inject[i]);
    return SCE_KERNEL_STOP_SUCCESS;
}

/* ================================================================
 * COMPLETE LR→PARAM MAP (verified via MOVW/BL decode)
 *
 * --- Shadow ---
 * 0x81015D9C → Shadow/AutoAdjust     Stock: ?
 * 0x81015DAE → Shadow/FarMax1        Stock: 100.0
 * 0x81015DC0 → Shadow/FarMax2        Stock: 60.0
 * 0x81015DD2 → Shadow/NearMin        Stock: 1.0
 * 0x81015DE4 → Shadow/LightRange     Stock: 300.0
 * 0x81015DF6 → Shadow/PolygonOffset  Stock: 1.0
 * 0x81015E08 → Shadow/ZOffset        Stock: 0.1
 * 0x81015E1A → Shadow/OutValue       Stock: 0.5
 *
 * --- GlowColor ---
 * 0x81015E74 → GlowColor/Intensity   Stock: scene-defined (~1.0)
 *
 * --- World LodDistance ---
 * 0x81016156–0x810161C2 → LodDistance/Level1–7
 *
 * --- ParticleLodDistance ---
 * 0x810161D4–0x81016240 → ParticleLodDistance/Level1–7
 *
 * --- CharacterLodDistance ---
 * 0x81016252–0x810162BE → CharacterLodDistance/Level1–7
 *
 * --- ClearColor ---
 * 0x810162D0–0x810162F4 → ClearColor/R,G,B
 * ================================================================ */
