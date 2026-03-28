/*
 * GR544P — Gravity Rush Native Resolution Patch
 * Supports: PCSA00011, PCSF00024, PCSD00035, PCSD00003
 *
 * Upgrades output from 720x408 to 960x544 (PS Vita native panel)
 * with surgical fixes for comic rendering and motion blur corruption.
 *
 * Patches applied at module_start (zero runtime overhead):
 *   1. FB (framebuffer output):  720x408 → 960x544       [7 injects]
 *   2. IB (3D scene rendering):  720x408 → 960x544       [4 injects]
 *   3. Shadow map:               1024x1024 → 512x512     [2 injects]
 *   4. Comic fix:  6 floats in GXP shader at seg1+0x72954
 *   5. Motion blur fix: 6 floats in GXP shader at seg1+0x3BBC0
 */

#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/clib.h>
#include <taihen.h>

/* ── Injection storage ────────────────────────────────────────────── */
#define MAX_INJECT 16
static SceUID g_inject[MAX_INJECT];
static int g_ninject = 0;

static void inject(SceUID modid, int seg, uint32_t off,
                   const void *data, size_t len)
{
    if (g_ninject >= MAX_INJECT) return;
    SceUID id = taiInjectData(modid, seg, off, data, len);
    if (id >= 0)
        g_inject[g_ninject++] = id;
}

/* ── GXP shader fixes ────────────────────────────────────────────── */
/*
 * Two GXP shaders contain hardcoded screen-dimension reciprocals as
 * default uniforms. After the resolution change these must be updated
 * to match 960x544, otherwise comic images shrink (shader 8) and
 * motion blur corrupts (shader 2).
 *
 * Shader 8 — comic image pixel→NDC (seg1+0x72954, 721 bytes):
 *   +0x208  1/720 → 1/960     +0x21C  1/720 → 1/960
 *   +0x210  1/408 → 1/544     +0x220  1/408 → 1/544
 *   +0x224  1/720 → 1/960     +0x228  1/720 → 1/960
 *
 * Shader 2 — motion blur (seg1+0x3BBC0, 571 bytes):
 *   +0x158  2/720 → 2/960     +0x19C  2/720 → 2/960
 *   +0x160  2/408 → 2/544     +0x1A0  2/408 → 2/544
 *   +0x1A4  2/720 → 2/960     +0x1A8  2/720 → 2/960
 */
static void patch_gxp_shaders(SceUID modid)
{
    SceKernelModuleInfo mi;
    sceClibMemset(&mi, 0, sizeof(mi));
    mi.size = sizeof(mi);
    if (sceKernelGetModuleInfo(modid, &mi) < 0)
        return;

    uint32_t *seg1 = (uint32_t *)mi.segments[1].vaddr;

    /* Shader 8: comic fix — 4x 1/720→1/960, 2x 1/408→1/544 */
    seg1[(0x72954 + 0x208) / 4] = 0x3A888889u;  /* 1/720 → 1/960 */
    seg1[(0x72954 + 0x21C) / 4] = 0x3A888889u;  /* 1/720 → 1/960 */
    seg1[(0x72954 + 0x224) / 4] = 0x3A888889u;  /* 1/720 → 1/960 */
    seg1[(0x72954 + 0x228) / 4] = 0x3A888889u;  /* 1/720 → 1/960 */
    seg1[(0x72954 + 0x210) / 4] = 0x3AF0F0F1u;  /* 1/408 → 1/544 */
    seg1[(0x72954 + 0x220) / 4] = 0x3AF0F0F1u;  /* 1/408 → 1/544 */

    /* Shader 2: motion blur fix — 4x 2/720→2/960, 2x 2/408→2/544 */
    seg1[(0x3BBC0 + 0x158) / 4] = 0x3B088889u;  /* 2/720 → 2/960 */
    seg1[(0x3BBC0 + 0x19C) / 4] = 0x3B088889u;  /* 2/720 → 2/960 */
    seg1[(0x3BBC0 + 0x1A4) / 4] = 0x3B088889u;  /* 2/720 → 2/960 */
    seg1[(0x3BBC0 + 0x1A8) / 4] = 0x3B088889u;  /* 2/720 → 2/960 */
    seg1[(0x3BBC0 + 0x160) / 4] = 0x3B70F0F1u;  /* 2/408 → 2/544 */
    seg1[(0x3BBC0 + 0x1A0) / 4] = 0x3B70F0F1u;  /* 2/408 → 2/544 */
}

/* ── Plugin entry ─────────────────────────────────────────────────── */
void _start() __attribute__((weak, alias("module_start")));

int module_start(SceSize args, void *argp)
{
    (void)args; (void)argp;
    g_ninject = 0;

    tai_module_info_t info;
    info.size = sizeof(info);
    if (taiGetModuleInfo(TAI_MAIN_MODULE, &info) < 0)
        return SCE_KERNEL_START_FAILED;

    /* ─── 1. FB output resolution: 720x408 → 960x544 ─────────────── */
    static const uint8_t w960_r0[]  = { 0x5F, 0xF4, 0x70, 0x70 };
    static const uint8_t h544_r0[]  = { 0x5F, 0xF4, 0x08, 0x70 };
    static const uint8_t w960_r1[]  = { 0x5F, 0xF4, 0x70, 0x71 };
    static const uint8_t h544_r1[]  = { 0x5F, 0xF4, 0x08, 0x71 };
    static const uint8_t w960_r3[]  = { 0x5F, 0xF4, 0x70, 0x73 };

    inject(info.modid, 0, 0x284,   w960_r0, 4);  /* FB init width      */
    inject(info.modid, 0, 0x28A,   h544_r0, 4);  /* FB init height     */
    inject(info.modid, 0, 0x45E9C, h544_r0, 4);  /* display RT height  */
    inject(info.modid, 0, 0x45EA6, w960_r0, 4);  /* display RT width   */
    inject(info.modid, 0, 0x45EC4, w960_r3, 4);  /* display RT width   */
    inject(info.modid, 0, 0x6974,  w960_r1, 4);  /* FB width (r1)      */
    inject(info.modid, 0, 0x697A,  h544_r1, 4);  /* FB height (r1)     */

    /* ─── 2. IB (3D scene) resolution: 720x408 → 960x544 ─────────── */
    static const uint8_t h544_r9[]  = { 0x5F, 0xF4, 0x08, 0x79 };
    static const uint8_t w960_r10[] = { 0x5F, 0xF4, 0x70, 0x7A };

    inject(info.modid, 0, 0xC46C, h544_r9,  4);  /* IB height          */
    inject(info.modid, 0, 0xC474, w960_r10, 4);  /* IB width           */
    inject(info.modid, 0, 0xC48A, w960_r3,  4);  /* IB create width    */
    inject(info.modid, 0, 0xC4F6, w960_r3,  4);  /* IB create width 2  */

    /* ─── 3. Shadow map: 1024x1024 → 512x512 ─────────────────────── */
    static const uint8_t s512_lr[] = { 0x5F, 0xF4, 0x00, 0x7E }; /* movs.w lr, #512 */
    static const uint8_t s512_r3[] = { 0x5F, 0xF4, 0x00, 0x73 }; /* movs.w r3, #512 */

    inject(info.modid, 0, 0xC62C, s512_lr, 4);   /* shadow size        */
    inject(info.modid, 0, 0xC644, s512_r3, 4);   /* shadow create      */

    /* ─── 4. GXP shader fixes (comic + motion blur) ─────────────────── */
    patch_gxp_shaders(info.modid);

    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, void *argp)
{
    (void)args; (void)argp;
    int i;
    for (i = 0; i < g_ninject; i++) {
        if (g_inject[i] >= 0)
            taiInjectRelease(g_inject[i]);
    }
    return SCE_KERNEL_STOP_SUCCESS;
}
