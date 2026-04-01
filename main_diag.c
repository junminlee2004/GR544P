/*
 * GR544P v39l — Expand frame + offset + scroll + text cull + GPU perf
 *
 * EXPAND_RIGHT/DOWN grow BR corner outward.
 * EXPAND_UP grows TL corner upward (negative = shrink from top).
 * SCROLL: 16-byte patch at 0x3B52EC scales visible height × 0.75
 * and removes the scroll gate.
 *
 * TEXT CULL FIX:
 *   Static path (0x3B393C–0x3B40D8): 2 NOPs at 0x3B3CDC/3CF0
 *     Disables element-level height/region visibility cull.
 *   Animated path (0x3B40DC–0x3B5742): 3 NOPs at 0x3B549C/54E8/5524
 *     Disables per-line scroll cull checks that reject ALL lines when
 *     visible window bounds (derived from old 720×408 screen) go stale.
 *   The scroll patch (0x3B52EC) is in the animated path, confirming
 *   the Aldnouir challenge window uses THIS path — the static NOPs
 *   alone had no effect.
 *
 * v39b BUG: VFP imm8=0x68 expands to 1.25, not 0.75!
 *   ARM VFPExpandImm uses E-2=6 bit replications for single-precision,
 *   not 5.  Correct imm8 for 0.75 is 0x50.
 */

#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/clib.h>
#include <taihen.h>

/* ============================================================
 * GXP PATCH GROUPS — set to 0 to disable, 1 to enable.
 *
 * Start with A+B only (known-needed). Enable others one at a
 * time and check each comic scene for clipping.
 *
 * A:  #608+#609  type 09/22  comic panel + caption  (confirmed)
 * B:  #2         motion blur shader                  (confirmed)
 * C:  #604+#605  type 0d/20  (near comic area)
 * D1: #613       type 0f/20  (near comic area)
 * D2: #614       type 0f/20  (near comic area)
 * E1: #616       type 09/22  alt context (0.4545)
 * E2: #617       type 09/22  alt context (0.4545)
 * F:  #24        type 41/22  (far from comic area, data+0x5838)
 * G:  #238       type c1/12  (near motion blur, data+0x3B998)
 * H:  #545+#581  2-slot shaders (different types)
 * ============================================================ */
#define GXP_A   1   /* #608 + #609 — comic panel + caption       */
#define GXP_B   1   /* #2   — motion blur                        */
#define GXP_C   0   /* #604 + #605                                */
#define GXP_D1  1   /* #613 only                                  */
#define GXP_D2  0   /* #614 only                                  */
#define GXP_E1  1   /* #616 only                                  */
#define GXP_E2  0   /* #617 only                                  */
#define GXP_F   0   /* #24                                        */
#define GXP_G   0   /* #238                                       */
#define GXP_H   0   /* #545 + #581                                */

/* ============================================================
 * RESOLUTION INJECT PATCHES
 *   0 = disabled   (original 720×408 / 1024 shadow)
 *   1 = high res   (960×544)
 *   2 = low  res   (480×272)
 *   3 = ultra low  (240×136)
 *   4 = potato     (120×68)
 *
 * SM (shadow map) uses different scale:
 *   0 = original   (1024×1024)
 *   1 = reduced    (512×512)
 *   2 = minimum    (256×256)
 *   3 = enhanced   (2048×2048)  — uses ~16MB CDRAM, may OOM
 *   4 = mid-high   (768×768)   — ~56% of original area
 *
 * FB = framebuffer / display surface (what gets scanned out)
 * IB = internal rendering buffer (what the GPU draws into)
 * SM = shadow map (depth-only square buffer)
 * ============================================================ */
#define FB_1   1   /* 0x00284: r0  — display FB width               */
#define FB_2   1   /* 0x0028A: r0  — display FB height              */
#define FB_3   0   /* 0x45E9C: r0  — display mode reg height        */
#define FB_4   0   /* 0x45EA6: r0  — display mode reg width         */
#define FB_5   0   /* 0x45EC4: r3  — display mode reg stride        */
#define FB_6   1   /* 0x06974: r1  — color surface default W        */
#define FB_7   1   /* 0x0697A: r1  — color surface default H        */
#define IB_1   1   /* 0x0C46C: r9  — 3D render target height        */
#define IB_2   1   /* 0x0C474: r10 — 3D render target width         */
#define IB_3   1   /* 0x0C48A: r3  — 3D render target stride        */
#define IB_4   1   /* 0x0C4F6: r3  — HDR RT stride (must match IB_2)*/
#define SM_1   1   /* 0x0C62C: lr  — shadow map dimension           */
#define SM_2   1   /* 0x0C644: r3  — shadow map stride              */

/* ============================================================
 * GPU PERF OPTIMIZATIONS — set to 0 to disable, 1 to enable.
 * ============================================================ */
#define MSAA_OFF  1   /* 0x0C47C: disable 4X MSAA on ALL render targets */
                      /*   main RT, HDR RT, half-res post buffer         */
                      /*   IMPACT: MAJOR ~30-40% GPU fill rate savings   */
                      /*   VISUAL: slight edge aliasing at 544p          */

#define POST_HALF 4   /* Shrink the post-process buffer (bloom/DOF source)  */
                      /*   0 = original  360×204                             */
                      /*   1 = half      180×102   (¼ area)                  */
                      /*   2 = quarter    90×51    (1/16 area)               */
                      /*   3 = eighth     45×25    (1/64 area)               */
                      /*   4 = 1/32       30×17    (510 pixels)              */
                      /*   3 patches at 0xC6C4, 0xC6DC, 0xC6E8              */
                      /*   IMPACT: ~5-10% GPU per level                     */
                      /*   VISUAL: softer glow, blurrier DOF                */

#define REFL_HALF 0   /* Halve the reflection/env map (384×256 → 192×128)  */
                      /*   3 patches at 0xB906, 0xB90C, 0xB916              */
                      /*   IMPACT: ~5% GPU                                  */
                      /*   VISUAL: blockier reflections/env lighting        */
                      /*   WARNING: doubles as occlusion buffer — may       */
                      /*   cause MORE objects to render (net negative)      */

/* --- Shadow map render pass optimization ---------------------- */
/*  The REAL shadow map is created at 0x5534 (DF32 depth format    */
/*  0xC000000, at renderer+0x8E94). The shadow render pass         */
/*  re-draws the ENTIRE SCENE from the light's viewpoint into      */
/*  this buffer every frame. Shrinking it drastically cuts the     */
/*  shadow pass fill rate while keeping shadows functional.         */
/*                                                                  */
/*  0 = original (1024×1024 — 1M pixels per shadow frame)          */
/*  1 = 512×512  (262K pixels — 4× cheaper)                        */
/*  2 = 256×256  (65K pixels — 16× cheaper)                        */
/*  3 = 128×128  (16K pixels — 64× cheaper, blocky shadows)        */
/*  4 = 64×64    (4K pixels — 256× cheaper, nearly free)            */
/*  5 = 16×16    (256 pixels — 4096× cheaper, vestigial shadows)    */
/*  6 = 1×1      (1 pixel — shadow pass is a single texel write)   */
/*  7 = 2×2      (4 pixels)                                        */
/*  8 = 4×4      (16 pixels)                                       */
/*  9 = 8×8      (64 pixels)                                       */
/* 10 = 768×768  (590K pixels — stride derived from dim)            */
/*                                                                  */
/*  NOTE: SM_1/SM_2 at 0xC62C/0xC644 patch a DIFFERENT buffer      */
/*  (format 0x44000, possibly depth/stencil for main RT).           */
/*  SHADOW_DIM patches the actual shadow map used for rendering.   */
/* -------------------------------------------------------------- */
#define SHADOW_DIM 6

/* --- Bloom disable --------------------------------------------- */
/*  NOPs the bloom render pass creation call at 0xC7D6 (4 bytes).  */
/*  The bloom render targets and shaders are never initialized,    */
/*  so the per-frame bloom pass is skipped entirely.                */
/*  IMPACT: ~5-15% GPU (eliminates bloom downsample + blur passes) */
/*  VISUAL: no glow/bloom on bright areas, flatter lighting        */
/*  RISK: may crash if bloom pass doesn't NULL-check its RT.       */
/*        Test this one first before combining with other tweaks.  */
/* -------------------------------------------------------------- */
#define NO_BLOOM  0

/* --- Skip secondary render passes ----------------------------- */
/*  Forces the render pass gate at 0xD000 to return 0, skipping    */
/*  passes 4+5+6 (secondary scene / transparency / resolve).       */
/*  These re-draw parts of the scene for alpha blending, deferred  */
/*  resolve, or cascaded shadows — 3 full scene traversals.        */
/*  IMPACT: ~15-25% GPU (3 fewer scene draws per frame)            */
/*  VISUAL: missing transparency, possible lighting artifacts      */
/*  RISK: test in open-world areas first — menus should be fine    */
/* -------------------------------------------------------------- */
#define SKIP_PASSES 0

/* --- Skip post-process chain between passes 2 & 3 ------------- */
/*  NOPs bl at 0xCE86 → 0x8115FFE8 (12 GXM calls, 4 fullscreen    */
/*  scene draws inside). This is the DOF / bloom-blur / resolve     */
/*  chain that runs between main scene passes 2 and 3.              */
/*  NOT covered by SKIP_PASSES (that only skips passes 4-6).       */
/*  IMPACT: ~5-15% GPU (4 fewer fullscreen post draws)              */
/*  VISUAL: no DOF, no bloom blur, flatter image                    */
/* -------------------------------------------------------------- */
#define SKIP_POSTFX 0

/* --- Skip pre-pass effect (env map / light probe?) ------------- */
/*  NOPs bl at 0xCC2A → 0x8115FACE (7 GXM calls).                  */
/*  Runs BEFORE scene pass 1. Likely environment map or light       */
/*  probe rendering.                                                */
/*  IMPACT: ~3-5% GPU                                               */
/*  VISUAL: possibly flat env lighting, missing reflections         */
/* -------------------------------------------------------------- */
#define SKIP_PREPASS 0

/* --- Skip mid-pipeline effect ---------------------------------- */
/*  NOPs bl at 0xD0F4 → 0x8115E81C (4 GXM calls).                  */
/*  Runs between passes 4 and 5 (inside SKIP_PASSES territory).    */
/*  Only matters if SKIP_PASSES is 0.                               */
/*  IMPACT: ~2-3% GPU                                               */
/* -------------------------------------------------------------- */
#define SKIP_MIDFX 1

/* --- Skip late-pipeline effects (post-SKIP_PASSES survivors) --- */
/*  These run EVEN WITH SKIP_PASSES enabled:                        */
/*    0xD3E8: bl 0x81161346 (10 GXM — tonemapping/color grading?)  */
/*    0xD3F4: bl 0x8115C2D4 (6 GXM — bloom/glow state)             */
/*  Both return values unused by next instruction.                  */
/*  IMPACT: ~5-10% GPU combined                                     */
/*  VISUAL: flatter colors, no glow                                 */
/*  RISK: 0xD3F4 (bloom) may crash if bloom RT is expected.         */
/*        Test 0xD3E8 alone first (safer).                          */
/* -------------------------------------------------------------- */
#define SKIP_LATEFX 1   /* 0=off, 1=skip 0xD3E8 only, 2=skip both */

/* ============================================================ */
#define EXPAND_RIGHT   9.80f
#define EXPAND_DOWN    0.7f
#define EXPAND_UP      0.0f     /* + = grow upward, - = shrink from top */
#define OFFSET_X       0.0f
#define OFFSET_Y       -0.7f
/* ============================================================ */

#define MAX_INJECT 36
static SceUID g_inject[MAX_INJECT];
static int g_ninject = 0;

static SceUID         g_hook_id = -1;
static tai_hook_ref_t g_hook_ref;

static void inject(SceUID modid, int seg, uint32_t off,
                   const void *data, size_t len)
{
    if (g_ninject >= MAX_INJECT) return;
    SceUID id = taiInjectData(modid, seg, off, data, len);
    if (id >= 0) g_inject[g_ninject++] = id;
}

static int hookTransform(uint32_t r0) {
    uint32_t lr;
    __asm__ volatile("mov %0, lr" : "=r"(lr));

    if ((lr == 0x813B3F61 || lr == 0x813B5181) && r0) {
        volatile float *f = (volatile float *)r0;
        float s0 = f[0], s1 = f[1];
        f[0] = s0 + OFFSET_X;
        f[1] = s1 + EXPAND_UP + OFFSET_Y;
        int ret = TAI_CONTINUE(int, g_hook_ref, r0);
        f[0] = s0; f[1] = s1;
        return ret;
    }

    if ((lr == 0x813B3F85 || lr == 0x813B51AD) && r0) {
        volatile float *f = (volatile float *)r0;
        float s0 = f[0], s1 = f[1];
        f[0] = s0 + EXPAND_RIGHT + OFFSET_X;
        f[1] = s1 - EXPAND_DOWN + OFFSET_Y;
        int ret = TAI_CONTINUE(int, g_hook_ref, r0);
        f[0] = s0; f[1] = s1;
        return ret;
    }

    return TAI_CONTINUE(int, g_hook_ref, r0);
}

static void patch_gxp(SceUID modid)
{
    SceKernelModuleInfo mi;
    sceClibMemset(&mi, 0, sizeof(mi));
    mi.size = sizeof(mi);
    if (sceKernelGetModuleInfo(modid, &mi) < 0) return;
    uint32_t *s1 = (uint32_t *)mi.segments[1].vaddr;

#if GXP_A /* #608 comic panel + #609 caption */
    s1[(0x72954+0x208)/4]=0x3A888889u;s1[(0x72954+0x21C)/4]=0x3A888889u;
    s1[(0x72954+0x224)/4]=0x3A888889u;s1[(0x72954+0x228)/4]=0x3A888889u;
    s1[(0x72954+0x210)/4]=0x3AF0F0F1u;s1[(0x72954+0x220)/4]=0x3AF0F0F1u;

    s1[(0x72C28+0x238)/4]=0x3A888889u;
    s1[(0x72C28+0x24C)/4]=0x3A888889u;
    s1[(0x72C28+0x254)/4]=0x3A888889u;
    s1[(0x72C28+0x258)/4]=0x3A888889u;
    s1[(0x72C28+0x240)/4]=0x3AF0F0F1u;
    s1[(0x72C28+0x250)/4]=0x3AF0F0F1u;
#endif

#if GXP_B /* #2 motion blur */
    s1[(0x3BBC0+0x158)/4]=0x3B088889u;s1[(0x3BBC0+0x19C)/4]=0x3B088889u;
    s1[(0x3BBC0+0x1A4)/4]=0x3B088889u;s1[(0x3BBC0+0x1A8)/4]=0x3B088889u;
    s1[(0x3BBC0+0x160)/4]=0x3B70F0F1u;s1[(0x3BBC0+0x1A0)/4]=0x3B70F0F1u;
#endif

#if GXP_C /* #604 + #605 */
    s1[(0x7211c+0x150)/4]=0x3A888889u;
    s1[(0x7211c+0x164)/4]=0x3A888889u;
    s1[(0x7211c+0x16c)/4]=0x3A888889u;
    s1[(0x7211c+0x170)/4]=0x3A888889u;
    s1[(0x7211c+0x158)/4]=0x3AF0F0F1u;
    s1[(0x7211c+0x168)/4]=0x3AF0F0F1u;

    s1[(0x722e0+0x190)/4]=0x3A888889u;
    s1[(0x722e0+0x1a4)/4]=0x3A888889u;
    s1[(0x722e0+0x1ac)/4]=0x3A888889u;
    s1[(0x722e0+0x1b0)/4]=0x3A888889u;
    s1[(0x722e0+0x198)/4]=0x3AF0F0F1u;
    s1[(0x722e0+0x1a8)/4]=0x3AF0F0F1u;
#endif

#if GXP_D1 /* #613 */
    s1[(0x737dc+0x2e8)/4]=0x3A888889u;
    s1[(0x737dc+0x314)/4]=0x3A888889u;
    s1[(0x737dc+0x31c)/4]=0x3A888889u;
    s1[(0x737dc+0x320)/4]=0x3A888889u;
    s1[(0x737dc+0x2f0)/4]=0x3AF0F0F1u;
    s1[(0x737dc+0x318)/4]=0x3AF0F0F1u;
#endif

#if GXP_D2 /* #614 */
    s1[(0x73c00+0x350)/4]=0x3A888889u;
    s1[(0x73c00+0x37c)/4]=0x3A888889u;
    s1[(0x73c00+0x384)/4]=0x3A888889u;
    s1[(0x73c00+0x388)/4]=0x3A888889u;
    s1[(0x73c00+0x358)/4]=0x3AF0F0F1u;
    s1[(0x73c00+0x380)/4]=0x3AF0F0F1u;
#endif

#if GXP_E1 /* #616 */
    s1[(0x7440C+0x2F0)/4]=0x3A888889u;
    s1[(0x7440C+0x30C)/4]=0x3A888889u;
    s1[(0x7440C+0x314)/4]=0x3A888889u;
    s1[(0x7440C+0x318)/4]=0x3A888889u;
    s1[(0x7440C+0x2F8)/4]=0x3AF0F0F1u;
    s1[(0x7440C+0x310)/4]=0x3AF0F0F1u;
#endif

#if GXP_E2 /* #617 */
    s1[(0x74800+0x330)/4]=0x3A888889u;
    s1[(0x74800+0x34C)/4]=0x3A888889u;
    s1[(0x74800+0x354)/4]=0x3A888889u;
    s1[(0x74800+0x358)/4]=0x3A888889u;
    s1[(0x74800+0x338)/4]=0x3AF0F0F1u;
    s1[(0x74800+0x350)/4]=0x3AF0F0F1u;
#endif

#if GXP_F /* #24 */
    s1[(0x05838+0x314)/4]=0x3A888889u;
    s1[(0x05838+0x368)/4]=0x3A888889u;
    s1[(0x05838+0x370)/4]=0x3A888889u;
    s1[(0x05838+0x374)/4]=0x3A888889u;
    s1[(0x05838+0x31c)/4]=0x3AF0F0F1u;
    s1[(0x05838+0x36c)/4]=0x3AF0F0F1u;
#endif

#if GXP_G /* #238 */
    s1[(0x3b998+0x144)/4]=0x3A888889u;
    s1[(0x3b998+0x188)/4]=0x3A888889u;
    s1[(0x3b998+0x190)/4]=0x3A888889u;
    s1[(0x3b998+0x194)/4]=0x3A888889u;
    s1[(0x3b998+0x14c)/4]=0x3AF0F0F1u;
    s1[(0x3b998+0x18c)/4]=0x3AF0F0F1u;
#endif

#if GXP_H /* #545 + #581 */
    s1[(0x699cc+0x124)/4]=0x3A888889u;
    s1[(0x699cc+0x130)/4]=0x3A888889u;
    s1[(0x699cc+0x12c)/4]=0x3AF0F0F1u;
    s1[(0x699cc+0x140)/4]=0x3AF0F0F1u;

    s1[(0x6fc54+0x164)/4]=0x3A888889u;
    s1[(0x6fc54+0x198)/4]=0x3A888889u;
    s1[(0x6fc54+0x15c)/4]=0x3AF0F0F1u;
    s1[(0x6fc54+0x1a8)/4]=0x3AF0F0F1u;
#endif
}

void _start() __attribute__((weak, alias("module_start")));

int module_start(SceSize args, void *argp)
{
    (void)args; (void)argp;
    g_ninject = 0;

    tai_module_info_t info;
    info.size = sizeof(info);
    if (taiGetModuleInfo(TAI_MAIN_MODULE, &info) < 0)
        return SCE_KERNEL_START_FAILED;

    /* --- Resolution byte arrays -------------------------------------- */
    /* hi=960/544, lo=480/272, ul=240/136, pt=120/68                     */
    /* ----------------------------------------------------------------- */
    /* 960×544  (Thumb-2 rotated imm, hw1=0xF45F) */
    static const uint8_t whi_r0[]  = {0x5F,0xF4,0x70,0x70};  /* 960  */
    static const uint8_t hhi_r0[]  = {0x5F,0xF4,0x08,0x70};  /* 544  */
    static const uint8_t whi_r1[]  = {0x5F,0xF4,0x70,0x71};
    static const uint8_t hhi_r1[]  = {0x5F,0xF4,0x08,0x71};
    static const uint8_t whi_r3[]  = {0x5F,0xF4,0x70,0x73};
    static const uint8_t hhi_r9[]  = {0x5F,0xF4,0x08,0x79};
    static const uint8_t whi_r10[] = {0x5F,0xF4,0x70,0x7A};
    /* 480×272  (Thumb-2 rotated imm, hw1=0xF45F) */
    static const uint8_t wlo_r0[]  = {0x5F,0xF4,0xF0,0x70};  /* 480  */
    static const uint8_t hlo_r0[]  = {0x5F,0xF4,0x88,0x70};  /* 272  */
    static const uint8_t wlo_r1[]  = {0x5F,0xF4,0xF0,0x71};
    static const uint8_t hlo_r1[]  = {0x5F,0xF4,0x88,0x71};
    static const uint8_t wlo_r3[]  = {0x5F,0xF4,0xF0,0x73};
    static const uint8_t hlo_r9[]  = {0x5F,0xF4,0x88,0x79};
    static const uint8_t wlo_r10[] = {0x5F,0xF4,0xF0,0x7A};
    /* 240×136  (simple imm8, hw1=0xF05F) */
    static const uint8_t wul_r0[]  = {0x5F,0xF0,0xF0,0x00};  /* 240  */
    static const uint8_t hul_r0[]  = {0x5F,0xF0,0x88,0x00};  /* 136  */
    static const uint8_t wul_r1[]  = {0x5F,0xF0,0xF0,0x01};
    static const uint8_t hul_r1[]  = {0x5F,0xF0,0x88,0x01};
    static const uint8_t wul_r3[]  = {0x5F,0xF0,0xF0,0x03};
    static const uint8_t hul_r9[]  = {0x5F,0xF0,0x88,0x09};
    static const uint8_t wul_r10[] = {0x5F,0xF0,0xF0,0x0A};
    /* 120×68   (simple imm8, hw1=0xF05F) */
    static const uint8_t wpt_r0[]  = {0x5F,0xF0,0x78,0x00};  /* 120  */
    static const uint8_t hpt_r0[]  = {0x5F,0xF0,0x44,0x00};  /*  68  */
    static const uint8_t wpt_r1[]  = {0x5F,0xF0,0x78,0x01};
    static const uint8_t hpt_r1[]  = {0x5F,0xF0,0x44,0x01};
    static const uint8_t wpt_r3[]  = {0x5F,0xF0,0x78,0x03};
    static const uint8_t hpt_r9[]  = {0x5F,0xF0,0x44,0x09};
    static const uint8_t wpt_r10[] = {0x5F,0xF0,0x78,0x0A};
    /* Shadow map square sizes */
    static const uint8_t s512_lr[] = {0x5F,0xF4,0x00,0x7E};  /*  512 */
    static const uint8_t s512_r3[] = {0x5F,0xF4,0x00,0x73};
    static const uint8_t s256_lr[] = {0x5F,0xF4,0x80,0x7E};  /*  256 */
    static const uint8_t s256_r3[] = {0x5F,0xF4,0x80,0x73};
    static const uint8_t s2k_lr[]  = {0x5F,0xF4,0x00,0x6E};  /* 2048 */
    static const uint8_t s2k_r3[]  = {0x5F,0xF4,0x00,0x63};

    /* --- FB patches (0=720, 1=960, 2=480, 3=240, 4=120) ------------ */
#if   FB_1 == 1
    inject(info.modid, 0, 0x284, whi_r0, 4);
#elif FB_1 == 2
    inject(info.modid, 0, 0x284, wlo_r0, 4);
#elif FB_1 == 3
    inject(info.modid, 0, 0x284, wul_r0, 4);
#elif FB_1 == 4
    inject(info.modid, 0, 0x284, wpt_r0, 4);
#endif
#if   FB_2 == 1
    inject(info.modid, 0, 0x28A, hhi_r0, 4);
#elif FB_2 == 2
    inject(info.modid, 0, 0x28A, hlo_r0, 4);
#elif FB_2 == 3
    inject(info.modid, 0, 0x28A, hul_r0, 4);
#elif FB_2 == 4
    inject(info.modid, 0, 0x28A, hpt_r0, 4);
#endif
#if   FB_3 == 1
    inject(info.modid, 0, 0x45E9C, hhi_r0, 4);
#elif FB_3 == 2
    inject(info.modid, 0, 0x45E9C, hlo_r0, 4);
#elif FB_3 == 3
    inject(info.modid, 0, 0x45E9C, hul_r0, 4);
#elif FB_3 == 4
    inject(info.modid, 0, 0x45E9C, hpt_r0, 4);
#endif
#if   FB_4 == 1
    inject(info.modid, 0, 0x45EA6, whi_r0, 4);
#elif FB_4 == 2
    inject(info.modid, 0, 0x45EA6, wlo_r0, 4);
#elif FB_4 == 3
    inject(info.modid, 0, 0x45EA6, wul_r0, 4);
#elif FB_4 == 4
    inject(info.modid, 0, 0x45EA6, wpt_r0, 4);
#endif
#if   FB_5 == 1
    inject(info.modid, 0, 0x45EC4, whi_r3, 4);
#elif FB_5 == 2
    inject(info.modid, 0, 0x45EC4, wlo_r3, 4);
#elif FB_5 == 3
    inject(info.modid, 0, 0x45EC4, wul_r3, 4);
#elif FB_5 == 4
    inject(info.modid, 0, 0x45EC4, wpt_r3, 4);
#endif
#if   FB_6 == 1
    inject(info.modid, 0, 0x6974, whi_r1, 4);
#elif FB_6 == 2
    inject(info.modid, 0, 0x6974, wlo_r1, 4);
#elif FB_6 == 3
    inject(info.modid, 0, 0x6974, wul_r1, 4);
#elif FB_6 == 4
    inject(info.modid, 0, 0x6974, wpt_r1, 4);
#endif
#if   FB_7 == 1
    inject(info.modid, 0, 0x697A, hhi_r1, 4);
#elif FB_7 == 2
    inject(info.modid, 0, 0x697A, hlo_r1, 4);
#elif FB_7 == 3
    inject(info.modid, 0, 0x697A, hul_r1, 4);
#elif FB_7 == 4
    inject(info.modid, 0, 0x697A, hpt_r1, 4);
#endif

    /* --- IB patches (0=720, 1=960, 2=480, 3=240, 4=120) ------------ */
#if   IB_1 == 1
    inject(info.modid, 0, 0xC46C, hhi_r9, 4);
#elif IB_1 == 2
    inject(info.modid, 0, 0xC46C, hlo_r9, 4);
#elif IB_1 == 3
    inject(info.modid, 0, 0xC46C, hul_r9, 4);
#elif IB_1 == 4
    inject(info.modid, 0, 0xC46C, hpt_r9, 4);
#endif
#if   IB_2 == 1
    inject(info.modid, 0, 0xC474, whi_r10, 4);
#elif IB_2 == 2
    inject(info.modid, 0, 0xC474, wlo_r10, 4);
#elif IB_2 == 3
    inject(info.modid, 0, 0xC474, wul_r10, 4);
#elif IB_2 == 4
    inject(info.modid, 0, 0xC474, wpt_r10, 4);
#endif
#if   IB_3 == 1
    inject(info.modid, 0, 0xC48A, whi_r3, 4);
#elif IB_3 == 2
    inject(info.modid, 0, 0xC48A, wlo_r3, 4);
#elif IB_3 == 3
    inject(info.modid, 0, 0xC48A, wul_r3, 4);
#elif IB_3 == 4
    inject(info.modid, 0, 0xC48A, wpt_r3, 4);
#endif
#if   IB_4 == 1
    inject(info.modid, 0, 0xC4F6, whi_r3, 4);
#elif IB_4 == 2
    inject(info.modid, 0, 0xC4F6, wlo_r3, 4);
#elif IB_4 == 3
    inject(info.modid, 0, 0xC4F6, wul_r3, 4);
#elif IB_4 == 4
    inject(info.modid, 0, 0xC4F6, wpt_r3, 4);
#endif

    /* --- SM patches (0=1024, 1=512, 2=256, 3=2048) ------------------ */
#if   SM_1 == 1
    inject(info.modid, 0, 0xC62C, s512_lr, 4);
#elif SM_1 == 2
    inject(info.modid, 0, 0xC62C, s256_lr, 4);
#elif SM_1 == 3
    inject(info.modid, 0, 0xC62C, s2k_lr, 4);
#elif SM_1 == 4
    { static const uint8_t s768_lr[] = {0x40,0xF2,0x00,0x3E}; /* movw lr, #768 */
      inject(info.modid, 0, 0xC62C, s768_lr, 4); }
#endif
#if   SM_2 == 1
    inject(info.modid, 0, 0xC644, s512_r3, 4);
#elif SM_2 == 2
    inject(info.modid, 0, 0xC644, s256_r3, 4);
#elif SM_2 == 3
    inject(info.modid, 0, 0xC644, s2k_r3, 4);
#elif SM_2 == 4
    { static const uint8_t s768_r3[] = {0x40,0xF2,0x00,0x33}; /* movw r3, #768 */
      inject(info.modid, 0, 0xC644, s768_r3, 4); }
#endif

    /* --- GPU perf: MSAA disable -------------------------------------- */
    /* Original: movs.w fp, #2 (SCE_GXM_MULTISAMPLE_4X)                 */
    /* fp is reused for ALL 3 render targets:                            */
    /*   0xC480: main color RT (960×544)                                 */
    /*   0xC4F2: HDR half-float RT (960×544)                             */
    /*   0xC6C8: half-res post buffer (360×204)                          */
    /* Changing to #0 = MULTISAMPLE_NONE across all three.               */
    /* ----------------------------------------------------------------- */
#if MSAA_OFF
    static const uint8_t msaa_none[] = {0x5F,0xF0,0x00,0x0B}; /* movs.w fp, #0 */
    inject(info.modid, 0, 0xC47C, msaa_none, 4);
#endif

    /* --- GPU perf: shrink post-process buffer ----------------------- */
    /* Original: 360×204 (0x168 × 0xCC).                                 */
    /* This is the bloom/DOF source buffer — smaller = cheaper.          */
    /* ----------------------------------------------------------------- */
#if   POST_HALF == 1  /* 180×102 */
    { static const uint8_t ph[] = {0x5F,0xF0,0x66,0x0E}; /* lr=#102 */
      static const uint8_t pw[] = {0x5F,0xF0,0xB4,0x0E}; /* lr=#180 */
      static const uint8_t ps[] = {0x5F,0xF0,0xB4,0x03}; /* r3=#180 */
      inject(info.modid, 0, 0xC6C4, ph, 4);
      inject(info.modid, 0, 0xC6DC, pw, 4);
      inject(info.modid, 0, 0xC6E8, ps, 4); }
#elif POST_HALF == 2  /* 90×51 */
    { static const uint8_t ph[] = {0x5F,0xF0,0x33,0x0E}; /* lr=#51  */
      static const uint8_t pw[] = {0x5F,0xF0,0x5A,0x0E}; /* lr=#90  */
      static const uint8_t ps[] = {0x5F,0xF0,0x5A,0x03}; /* r3=#90  */
      inject(info.modid, 0, 0xC6C4, ph, 4);
      inject(info.modid, 0, 0xC6DC, pw, 4);
      inject(info.modid, 0, 0xC6E8, ps, 4); }
#elif POST_HALF == 3  /* 45×25 */
    { static const uint8_t ph[] = {0x5F,0xF0,0x19,0x0E}; /* lr=#25  */
      static const uint8_t pw[] = {0x5F,0xF0,0x2D,0x0E}; /* lr=#45  */
      static const uint8_t ps[] = {0x5F,0xF0,0x2D,0x03}; /* r3=#45  */
      inject(info.modid, 0, 0xC6C4, ph, 4);
      inject(info.modid, 0, 0xC6DC, pw, 4);
      inject(info.modid, 0, 0xC6E8, ps, 4); }
#elif POST_HALF == 4  /* 30×17 */
    { static const uint8_t ph[] = {0x5F,0xF0,0x11,0x0E}; /* lr=#17  */
      static const uint8_t pw[] = {0x5F,0xF0,0x1E,0x0E}; /* lr=#30  */
      static const uint8_t ps[] = {0x5F,0xF0,0x1E,0x03}; /* r3=#30  */
      inject(info.modid, 0, 0xC6C4, ph, 4);
      inject(info.modid, 0, 0xC6DC, pw, 4);
      inject(info.modid, 0, 0xC6E8, ps, 4); }
#endif

    /* --- GPU perf: half reflection/env map --------------------------- */
    /* Original: 384×256 (depth format 0x44000). Halved: 192×128.        */
    /* WARNING: doubles as occlusion buffer. Reducing resolution may     */
    /* make occlusion less accurate → MORE objects drawn → net negative. */
    /* ----------------------------------------------------------------- */
#if REFL_HALF
    static const uint8_t refl_h[]  = {0x5F,0xF0,0xC0,0x01}; /* movs.w r1, #192 */
    static const uint8_t refl_w[]  = {0x5F,0xF0,0x80,0x02}; /* movs.w r2, #128 */
    static const uint8_t refl_s[]  = {0x5F,0xF0,0x80,0x03}; /* movs.w r3, #128 */
    inject(info.modid, 0, 0xB906, refl_h, 4);  /* height 384→192 */
    inject(info.modid, 0, 0xB90C, refl_w, 4);  /* width  256→128 */
    inject(info.modid, 0, 0xB916, refl_s, 4);  /* stride 256→128 */
#endif

    /* --- GPU perf: shadow map dimension reduction -------------------- */
    /* Patches 0x5534: movs.w r0, #1024 → smaller value.                */
    /* This is the REAL shadow map (DF32, renderer+0x8E94).              */
    /* Shadow render pass redraws full scene — smaller = fewer pixels.   */
    /* ----------------------------------------------------------------- */
#if   SHADOW_DIM == 1
    { static const uint8_t sd[] = {0x5F,0xF4,0x00,0x70}; /* 512 */
      inject(info.modid, 0, 0x5534, sd, 4); }
#elif SHADOW_DIM == 2
    { static const uint8_t sd[] = {0x5F,0xF4,0x80,0x70}; /* 256 */
      inject(info.modid, 0, 0x5534, sd, 4); }
#elif SHADOW_DIM == 3
    { static const uint8_t sd[] = {0x5F,0xF0,0x80,0x00}; /* 128 */
      inject(info.modid, 0, 0x5534, sd, 4); }
#elif SHADOW_DIM == 4
    { static const uint8_t sd[] = {0x5F,0xF0,0x40,0x00}; /* 64  */
      inject(info.modid, 0, 0x5534, sd, 4); }
#elif SHADOW_DIM == 5
    { static const uint8_t sd[] = {0x5F,0xF0,0x10,0x00}; /* 16  */
      inject(info.modid, 0, 0x5534, sd, 4); }
#elif SHADOW_DIM == 6
    { static const uint8_t sd[] = {0x5F,0xF0,0x01,0x00}; /* 1   */
      inject(info.modid, 0, 0x5534, sd, 4); }
#elif SHADOW_DIM == 7
    { static const uint8_t sd[] = {0x5F,0xF0,0x02,0x00}; /* 2   */
      inject(info.modid, 0, 0x5534, sd, 4); }
#elif SHADOW_DIM == 8
    { static const uint8_t sd[] = {0x5F,0xF0,0x04,0x00}; /* 4   */
      inject(info.modid, 0, 0x5534, sd, 4); }
#elif SHADOW_DIM == 9
    { static const uint8_t sd[] = {0x5F,0xF0,0x08,0x00}; /* 8   */
      inject(info.modid, 0, 0x5534, sd, 4); }
#elif SHADOW_DIM == 10
    { static const uint8_t sd[] = {0x40,0xF2,0x00,0x30}; /* 768 (movw r0, #768) */
      inject(info.modid, 0, 0x5534, sd, 4); }
#endif

    /* --- GPU perf: disable bloom ------------------------------------- */
    /* NOPs the bl at 0xC7D6 that creates bloom render resources.        */
    /* Without bloom RTs, the per-frame bloom pass should be skipped.    */
    /* ----------------------------------------------------------------- */
#if NO_BLOOM
    static const uint8_t nop4[] = {0x00,0xBF,0x00,0xBF}; /* 2× Thumb NOP */
    inject(info.modid, 0, 0xC7D6, nop4, 4);
#endif

    /* --- GPU perf: skip secondary render passes ---------------------- */
    /* Original 0xD000: bl 0xC970 (gate check → returns 1 if passes      */
    /* needed). Replace with movs r0,#0; nop → r0=0 → existing           */
    /* beq.w at 0xD006 always fires → skip passes 4+5+6 (978 bytes).    */
    /* ----------------------------------------------------------------- */
#if SKIP_PASSES
    { static const uint8_t sp[] = {0x00,0x20,0x00,0xBF}; /* movs r0,#0; nop */
      inject(info.modid, 0, 0xD000, sp, 4); }
#endif

    /* --- GPU perf: skip post-process chain between passes 2 & 3 ------ */
    /* NOPs bl at 0xCE86 → 0x8115FFE8 (12 GXM, 4 fullscreen draws).     */
    /* Next insn is movw r0,#0x2560 — no return value dependency.        */
    /* ----------------------------------------------------------------- */
#if SKIP_POSTFX
    { static const uint8_t pf[] = {0x00,0xBF,0x00,0xBF}; /* 2× NOP */
      inject(info.modid, 0, 0xCE86, pf, 4); }
#endif

    /* --- GPU perf: skip pre-pass effect ------------------------------ */
#if SKIP_PREPASS
    { static const uint8_t pp[] = {0x00,0xBF,0x00,0xBF};
      inject(info.modid, 0, 0xCC2A, pp, 4); }
#endif

    /* --- GPU perf: skip mid-pipeline effect -------------------------- */
#if SKIP_MIDFX
    { static const uint8_t mf[] = {0x00,0xBF,0x00,0xBF};
      inject(info.modid, 0, 0xD0F4, mf, 4); }
#endif

    /* --- GPU perf: skip late-pipeline effects ------------------------ */
#if SKIP_LATEFX >= 1
    { static const uint8_t lf[] = {0x00,0xBF,0x00,0xBF};
      inject(info.modid, 0, 0xD3E8, lf, 4); }  /* 10 GXM tonemapping */
#endif
#if SKIP_LATEFX >= 2
    { static const uint8_t lb[] = {0x00,0xBF,0x00,0xBF};
      inject(info.modid, 0, 0xD3F4, lb, 4); }  /* 6 GXM bloom state  */
#endif

    /* --- Scroll room: visible height × 0.75, gate removed --------- */
    /*                                                                  */
    /* ORIGINAL 0x3B52EC:           PATCHED:                            */
    /*   B0 69     ldr r0,[r6,#18]  F5 EE 00 0A  vmov.f32 s1, #0.75   */
    /*   00 28     cmp r0, #0                                           */
    /*   38 EE 80 0A vadd s0,s17,s0 38 EE 80 0A  vadd (same)           */
    /*   86 ED 0D 0A vstr [r6,#34]  20 EE 20 0A  vmul.f32 s0,s0,s1    */
    /*   40 F3 4E 81 ble.w gate     86 ED 0D 0A  vstr s0,[r6,#0x34]   */
    /*                                                                  */
    /* v39b had imm8=0x68 → 1.25 (WRONG). Correct: imm8=0x50 → 0.75.  */
    /* VFPExpandImm uses E-2=6 replications for single, not 5.          */
    /* -------------------------------------------------------------- */
    static const uint8_t scroll_patch[16] = {
        0xF5, 0xEE, 0x00, 0x0A,  /* vmov.f32  s1, #0.75             */
        0x38, 0xEE, 0x80, 0x0A,  /* vadd.f32  s0, s17, s0           */
        0x20, 0xEE, 0x20, 0x0A,  /* vmul.f32  s0, s0, s1            */
        0x86, 0xED, 0x0D, 0x0A,  /* vstr      s0, [r6, #0x34]       */
    };
    inject(info.modid, 0, 0x3B52EC, scroll_patch, 16);

    /* --- Text visibility cull bypass --------------------------------- */
    static const uint8_t nop[] = {0x00, 0xBF};  /* Thumb NOP */

    /* STATIC path (0x3B393C–0x3B40D8): element-level cull.              */
    /* Skips ALL text if elem[0x3C] ≤ 0 or elem[0x20] ≤ elem[0x34].     */
    inject(info.modid, 0, 0x3B3CDC, nop, 2);  /* b 0x3B3F04 → NOP */
    inject(info.modid, 0, 0x3B3CF0, nop, 2);  /* b 0x3B3F04 → NOP */

    /* ANIMATED path (0x3B40DC–0x3B5742): per-line scroll cull.          */
    /* Three per-line checks skip individual glyph rows when they fall   */
    /* outside the computed visible window. The visible window derives   */
    /* from elem[0x20]/[0x34] which are based on old 720×408 screen      */
    /* bounds. When frame expansion pushes past those bounds, ALL lines  */
    /* fail and text vanishes entirely.                                   */
    /*                                                                    */
    /*   0x3B549C: b 0x3B558A  — s0 < s16  (line below window)          */
    /*   0x3B54E8: b 0x3B557E  — s18 ≥ s0  (line above window top)      */
    /*   0x3B5524: b 0x3B557E  — s18 ≤ s0  (line below window bottom)   */
    /* -------------------------------------------------------------- */
    inject(info.modid, 0, 0x3B549C, nop, 2);  /* per-line cull 1 */
    inject(info.modid, 0, 0x3B54E8, nop, 2);  /* per-line cull 2 */
    inject(info.modid, 0, 0x3B5524, nop, 2);  /* per-line cull 3 */

    patch_gxp(info.modid);

    g_hook_id = taiHookFunctionOffset(&g_hook_ref, info.modid, 0,
                                       0x11469C, 1, (void *)hookTransform);

    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, void *argp)
{
    (void)args; (void)argp;
    if (g_hook_id >= 0) taiHookRelease(g_hook_id, g_hook_ref);
    int i;
    for (i = 0; i < g_ninject; i++)
        if (g_inject[i] >= 0) taiInjectRelease(g_inject[i]);
    return SCE_KERNEL_STOP_SUCCESS;
}
