/*
 * GR544P v41 — Clean build
 *
 * Resolution: 960×544 (native Vita display, up from 720×408)
 * GPU perf: shadow map 16×16
 * Text: textbox expansion via hookTransform, scroll/cull fixes,
 *       credits canvas fix via TextViewer hook
 * Shaders: nuclear GXP reciprocal sweep (78 patches across 619 shaders)
 */

#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/clib.h>
#include <taihen.h>

/* ============================================================
 * TUNING
 * ============================================================ */
#define EXPAND_RIGHT   25.0f
#define EXPAND_DOWN    0.7f
#define EXPAND_UP      0.0f
#define OFFSET_X       0.0f
#define OFFSET_Y       -0.7f

#define SHADOW_DIM     5       /* shadow map: 0=1024, 5=16×16      */
#define TV_SCALE       3.0f    /* TextViewer canvas scale           */

/* ============================================================
 * GXP SHADER PATCHES — per-group toggles
 *
 * A:  #608+#609  comic panel + caption       (1/720, 1/408)
 * B:  #239       motion blur                 (1/360, 1/204)
 * C:  #604+#605  near comic area             (1/720, 1/408)
 * D1: #613       near comic area             (1/720, 1/408)
 * D2: #614       near comic area             (1/720, 1/408)
 * E1: #616       alt context                 (1/720, 1/408)
 * E2: #617       alt context                 (1/720, 1/408)
 * F:  #24        far shader                  (1/720, 1/408)
 * G:  #238       near motion blur            (1/720, 1/408)
 * H:  #545+#581  2-slot shaders              (1/720, 1/408)
 *
 * NUCLEAR: ignore all above, scan entire data segment instead
 * ============================================================ */
#define GXP_A       1
#define GXP_B       1
#define GXP_C       0
#define GXP_D1      1
#define GXP_D2      0
#define GXP_E1      1
#define GXP_E2      0
#define GXP_F       0
#define GXP_G       0
#define GXP_H       0
#define GXP_NUCLEAR 0   /* 1 = ignore above, patch all 78 reciprocals */

/* ============================================================ */
#define MAX_INJECT 24
static SceUID g_inject[MAX_INJECT];
static int g_ninject = 0;

static SceUID         g_hook_id = -1;
static tai_hook_ref_t g_hook_ref;
static SceUID         g_tv_hook_id = -1;
static tai_hook_ref_t g_tv_hook_ref;

static inline void inject(SceUID modid, uint32_t off,
                           const void *data, size_t len)
{
    if (g_ninject >= MAX_INJECT) return;
    SceUID id = taiInjectData(modid, 0, off, data, len);
    if (id >= 0) g_inject[g_ninject++] = id;
}

/* ================================================================
 * TEXTBOX EXPANSION — hook 0x11469C
 * ================================================================ */
static int hookTransform(uint32_t r0) {
    uint32_t lr;
    __asm__ volatile("mov %0, lr" : "=r"(lr));

    if (!r0)
        return TAI_CONTINUE(int, g_hook_ref, r0);

    switch (lr) {
    case 0x813B3F61:
    case 0x813B5181: {
        volatile float *f = (volatile float *)r0;
        float s0 = f[0], s1 = f[1];
        f[0] = s0 + OFFSET_X;
        f[1] = s1 + EXPAND_UP + OFFSET_Y;
        int ret = TAI_CONTINUE(int, g_hook_ref, r0);
        f[0] = s0; f[1] = s1;
        return ret;
    }
    case 0x813B3F85:
    case 0x813B51AD: {
        volatile float *f = (volatile float *)r0;
        float s0 = f[0], s1 = f[1];
        f[0] = s0 + EXPAND_RIGHT + OFFSET_X;
        f[1] = s1 - EXPAND_DOWN + OFFSET_Y;
        int ret = TAI_CONTINUE(int, g_hook_ref, r0);
        f[0] = s0; f[1] = s1;
        return ret;
    }
    default:
        return TAI_CONTINUE(int, g_hook_ref, r0);
    }
}

/* ================================================================
 * CREDITS TEXT FIX — hook TextViewer at 0x382130
 *
 * Context struct rect: +0x28=X, +0x2C=Y, +0x30=W, +0x34=H
 * Scale W/H by TV_SCALE, shift X left to anchor left edge.
 * Y unchanged (credits scroll bottom-to-top).
 * ================================================================ */
static int hookTextViewer(uint32_t r0) {
    if (!r0)
        return TAI_CONTINUE(int, g_tv_hook_ref, r0);

    volatile float *f = (volatile float *)r0;
    float sx = f[0x28/4], sy = f[0x2C/4];
    float sw = f[0x30/4], sh = f[0x34/4];
    float dw = sw * (TV_SCALE - 1.0f);
    f[0x30/4] = sw + dw;
    f[0x34/4] = sh * TV_SCALE;
    f[0x28/4] = sx - dw * 0.5f;

    int ret = TAI_CONTINUE(int, g_tv_hook_ref, r0);

    f[0x28/4] = sx; f[0x2C/4] = sy;
    f[0x30/4] = sw; f[0x34/4] = sh;
    return ret;
}

/* ================================================================
 * GXP SHADER PATCHES
 * ================================================================ */
static void patch_gxp(uint32_t *s1, int memsz)
{
    #if GXP_NUCLEAR
    /* Scan entire data segment — replaces all 78 reciprocals */
    int n = memsz / 4, i;
    for (i = 0; i < n; i++) {
        switch (s1[i]) {
            case 0x3AB60B61u: s1[i] = 0x3A888889u; break; /* 1/720 → 1/960 */
            case 0x3B20A0A1u: s1[i] = 0x3AF0F0F1u; break; /* 1/408 → 1/544 */
            case 0x3B360B61u: s1[i] = 0x3B088889u; break; /* 1/360 → 1/480 */
            case 0x3BA0A0A1u: s1[i] = 0x3B70F0F1u; break; /* 1/204 → 1/272 */
        }
    }
    #else
    (void)memsz;
    #if GXP_A /* #608 comic panel + #609 caption */
    s1[(0x72954+0x208)/4]=0x3A888889u;s1[(0x72954+0x21C)/4]=0x3A888889u;
    s1[(0x72954+0x224)/4]=0x3A888889u;s1[(0x72954+0x228)/4]=0x3A888889u;
    s1[(0x72954+0x210)/4]=0x3AF0F0F1u;s1[(0x72954+0x220)/4]=0x3AF0F0F1u;
    s1[(0x72C28+0x238)/4]=0x3A888889u;s1[(0x72C28+0x24C)/4]=0x3A888889u;
    s1[(0x72C28+0x254)/4]=0x3A888889u;s1[(0x72C28+0x258)/4]=0x3A888889u;
    s1[(0x72C28+0x240)/4]=0x3AF0F0F1u;s1[(0x72C28+0x250)/4]=0x3AF0F0F1u;
    #endif
    #if GXP_B /* #239 motion blur (1/360, 1/204) */
    s1[(0x3BBC0+0x158)/4]=0x3B088889u;s1[(0x3BBC0+0x19C)/4]=0x3B088889u;
    s1[(0x3BBC0+0x1A4)/4]=0x3B088889u;s1[(0x3BBC0+0x1A8)/4]=0x3B088889u;
    s1[(0x3BBC0+0x160)/4]=0x3B70F0F1u;s1[(0x3BBC0+0x1A0)/4]=0x3B70F0F1u;
    #endif
    #if GXP_C /* #604 + #605 */
    s1[(0x7211c+0x150)/4]=0x3A888889u;s1[(0x7211c+0x164)/4]=0x3A888889u;
    s1[(0x7211c+0x16c)/4]=0x3A888889u;s1[(0x7211c+0x170)/4]=0x3A888889u;
    s1[(0x7211c+0x158)/4]=0x3AF0F0F1u;s1[(0x7211c+0x168)/4]=0x3AF0F0F1u;
    s1[(0x722e0+0x190)/4]=0x3A888889u;s1[(0x722e0+0x1a4)/4]=0x3A888889u;
    s1[(0x722e0+0x1ac)/4]=0x3A888889u;s1[(0x722e0+0x1b0)/4]=0x3A888889u;
    s1[(0x722e0+0x198)/4]=0x3AF0F0F1u;s1[(0x722e0+0x1a8)/4]=0x3AF0F0F1u;
    #endif
    #if GXP_D1 /* #613 */
    s1[(0x737dc+0x2e8)/4]=0x3A888889u;s1[(0x737dc+0x314)/4]=0x3A888889u;
    s1[(0x737dc+0x31c)/4]=0x3A888889u;s1[(0x737dc+0x320)/4]=0x3A888889u;
    s1[(0x737dc+0x2f0)/4]=0x3AF0F0F1u;s1[(0x737dc+0x318)/4]=0x3AF0F0F1u;
    #endif
    #if GXP_D2 /* #614 */
    s1[(0x73c00+0x350)/4]=0x3A888889u;s1[(0x73c00+0x37c)/4]=0x3A888889u;
    s1[(0x73c00+0x384)/4]=0x3A888889u;s1[(0x73c00+0x388)/4]=0x3A888889u;
    s1[(0x73c00+0x358)/4]=0x3AF0F0F1u;s1[(0x73c00+0x380)/4]=0x3AF0F0F1u;
    #endif
    #if GXP_E1 /* #616 */
    s1[(0x7440C+0x2F0)/4]=0x3A888889u;s1[(0x7440C+0x30C)/4]=0x3A888889u;
    s1[(0x7440C+0x314)/4]=0x3A888889u;s1[(0x7440C+0x318)/4]=0x3A888889u;
    s1[(0x7440C+0x2F8)/4]=0x3AF0F0F1u;s1[(0x7440C+0x310)/4]=0x3AF0F0F1u;
    #endif
    #if GXP_E2 /* #617 */
    s1[(0x74800+0x330)/4]=0x3A888889u;s1[(0x74800+0x34C)/4]=0x3A888889u;
    s1[(0x74800+0x354)/4]=0x3A888889u;s1[(0x74800+0x358)/4]=0x3A888889u;
    s1[(0x74800+0x338)/4]=0x3AF0F0F1u;s1[(0x74800+0x350)/4]=0x3AF0F0F1u;
    #endif
    #if GXP_F /* #24 */
    s1[(0x05838+0x314)/4]=0x3A888889u;s1[(0x05838+0x368)/4]=0x3A888889u;
    s1[(0x05838+0x370)/4]=0x3A888889u;s1[(0x05838+0x374)/4]=0x3A888889u;
    s1[(0x05838+0x31c)/4]=0x3AF0F0F1u;s1[(0x05838+0x36c)/4]=0x3AF0F0F1u;
    #endif
    #if GXP_G /* #238 */
    s1[(0x3b998+0x144)/4]=0x3A888889u;s1[(0x3b998+0x188)/4]=0x3A888889u;
    s1[(0x3b998+0x190)/4]=0x3A888889u;s1[(0x3b998+0x194)/4]=0x3A888889u;
    s1[(0x3b998+0x14c)/4]=0x3AF0F0F1u;s1[(0x3b998+0x18c)/4]=0x3AF0F0F1u;
    #endif
    #if GXP_H /* #545 + #581 */
    s1[(0x699cc+0x124)/4]=0x3A888889u;s1[(0x699cc+0x130)/4]=0x3A888889u;
    s1[(0x699cc+0x12c)/4]=0x3AF0F0F1u;s1[(0x699cc+0x140)/4]=0x3AF0F0F1u;
    s1[(0x6fc54+0x164)/4]=0x3A888889u;s1[(0x6fc54+0x198)/4]=0x3A888889u;
    s1[(0x6fc54+0x15c)/4]=0x3AF0F0F1u;s1[(0x6fc54+0x1a8)/4]=0x3AF0F0F1u;
    #endif
    #endif /* !GXP_NUCLEAR */
}

/* ================================================================ */
void _start() __attribute__((weak, alias("module_start")));

int module_start(SceSize args, void *argp)
{
    (void)args; (void)argp;
    g_ninject = 0;

    tai_module_info_t info;
    info.size = sizeof(info);
    if (taiGetModuleInfo(TAI_MAIN_MODULE, &info) < 0)
        return SCE_KERNEL_START_FAILED;

    SceUID mod = info.modid;

    /* Single module info lookup — shared with patch_gxp */
    SceKernelModuleInfo mi;
    sceClibMemset(&mi, 0, sizeof(mi));
    mi.size = sizeof(mi);
    if (sceKernelGetModuleInfo(mod, &mi) < 0)
        return SCE_KERNEL_START_FAILED;

    /* Thumb-2 movs.w encodings: width=960, height=544 per register */
    static const uint8_t w_r0[]  = {0x5F,0xF4,0x70,0x70};
    static const uint8_t h_r0[]  = {0x5F,0xF4,0x08,0x70};
    static const uint8_t w_r1[]  = {0x5F,0xF4,0x70,0x71};
    static const uint8_t h_r1[]  = {0x5F,0xF4,0x08,0x71};
    static const uint8_t w_r3[]  = {0x5F,0xF4,0x70,0x73};
    static const uint8_t h_r9[]  = {0x5F,0xF4,0x08,0x79};
    static const uint8_t w_r10[] = {0x5F,0xF4,0x70,0x7A};
    static const uint8_t nop2[]  = {0x00,0xBF};
    static const uint8_t sm_lr[] = {0x5F,0xF4,0x00,0x7E};
    static const uint8_t sm_r3[] = {0x5F,0xF4,0x00,0x73};
    #if SHADOW_DIM == 5
    static const uint8_t sd_r0[] = {0x5F,0xF0,0x10,0x00};
    #endif
    static const uint8_t scroll[16] = {
        0xF5,0xEE,0x00,0x0A, 0x38,0xEE,0x80,0x0A,
        0x20,0xEE,0x20,0x0A, 0x86,0xED,0x0D,0x0A };

    /* Framebuffer 720×408 → 960×544 */
    inject(mod, 0x284,   w_r0,  4);
    inject(mod, 0x28A,   h_r0,  4);
    inject(mod, 0x45E9C, h_r0,  4);
    inject(mod, 0x45EA6, w_r0,  4);
    inject(mod, 0x45EC4, w_r3,  4);
    inject(mod, 0x6974,  w_r1,  4);
    inject(mod, 0x697A,  h_r1,  4);

    /* Internal render buffer */
    inject(mod, 0xC46C, h_r9,  4);
    inject(mod, 0xC474, w_r10, 4);
    inject(mod, 0xC48A, w_r3,  4);
    inject(mod, 0xC4F6, w_r3,  4);

    /* Shadow map 1024 → 512 */
    inject(mod, 0xC62C, sm_lr, 4);
    inject(mod, 0xC644, sm_r3, 4);

    /* Shadow dim 1024 → 16 */
    #if SHADOW_DIM == 5
    inject(mod, 0x5534, sd_r0, 4);
    #endif

    /* Scroll patch + text cull bypass */
    inject(mod, 0x3B52EC, scroll, 16);
    inject(mod, 0x3B3CDC, nop2, 2);
    inject(mod, 0x3B3CF0, nop2, 2);
    inject(mod, 0x3B549C, nop2, 2);
    inject(mod, 0x3B54E8, nop2, 2);
    inject(mod, 0x3B5524, nop2, 2);

    /* GXP shader sweep — reuse module info from above */
    patch_gxp((uint32_t *)mi.segments[1].vaddr, mi.segments[1].memsz);

    /* Hooks */
    g_hook_id = taiHookFunctionOffset(&g_hook_ref, mod, 0,
                                      0x11469C, 1, (void *)hookTransform);
    g_tv_hook_id = taiHookFunctionOffset(&g_tv_hook_ref, mod, 0,
                                         0x382130, 1, (void *)hookTextViewer);

    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, void *argp)
{
    (void)args; (void)argp;
    if (g_hook_id >= 0)    taiHookRelease(g_hook_id, g_hook_ref);
    if (g_tv_hook_id >= 0) taiHookRelease(g_tv_hook_id, g_tv_hook_ref);
    int i;
    for (i = 0; i < g_ninject; i++)
        if (g_inject[i] >= 0) taiInjectRelease(g_inject[i]);
    return SCE_KERNEL_STOP_SUCCESS;
}
