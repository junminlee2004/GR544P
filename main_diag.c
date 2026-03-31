/*
 * GR544P v39c — Expand frame + offset position + scroll room
 *
 * SCROLL: 16-byte patch at 0x3B52EC scales visible height × 0.75
 * and removes the scroll gate.
 *
 * v39b BUG: VFP imm8=0x68 expands to 1.25, not 0.75!
 *   ARM VFPExpandImm uses E-2=6 bit replications for single-precision,
 *   not 5.  Correct imm8 for 0.75 is 0x50.
 */

#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/clib.h>
#include <taihen.h>

/* ============================================================ */
#define EXPAND_RIGHT   9.80f
#define EXPAND_DOWN    1.05f
#define OFFSET_X       0.0f
#define OFFSET_Y       -0.35f
/* ============================================================ */

#define MAX_INJECT 18
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
        f[1] = s1 + OFFSET_Y;
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

    s1[(0x72954+0x208)/4]=0x3A888889u;s1[(0x72954+0x21C)/4]=0x3A888889u;
    s1[(0x72954+0x224)/4]=0x3A888889u;s1[(0x72954+0x228)/4]=0x3A888889u;
    s1[(0x72954+0x210)/4]=0x3AF0F0F1u;s1[(0x72954+0x220)/4]=0x3AF0F0F1u;

    s1[(0x3BBC0+0x158)/4]=0x3B088889u;s1[(0x3BBC0+0x19C)/4]=0x3B088889u;
    s1[(0x3BBC0+0x1A4)/4]=0x3B088889u;s1[(0x3BBC0+0x1A8)/4]=0x3B088889u;
    s1[(0x3BBC0+0x160)/4]=0x3B70F0F1u;s1[(0x3BBC0+0x1A0)/4]=0x3B70F0F1u;

    s1[(0x72C28+0x238)/4]=0x3A888889u;
    s1[(0x72C28+0x24C)/4]=0x3A888889u;
    s1[(0x72C28+0x254)/4]=0x3A888889u;
    s1[(0x72C28+0x258)/4]=0x3A888889u;
    s1[(0x72C28+0x240)/4]=0x3AF0F0F1u;
    s1[(0x72C28+0x250)/4]=0x3AF0F0F1u;

    s1[(0x05838+0x314)/4]=0x3A888889u;
    s1[(0x05838+0x368)/4]=0x3A888889u;
    s1[(0x05838+0x370)/4]=0x3A888889u;
    s1[(0x05838+0x374)/4]=0x3A888889u;
    s1[(0x05838+0x31c)/4]=0x3AF0F0F1u;
    s1[(0x05838+0x36c)/4]=0x3AF0F0F1u;

    s1[(0x3b998+0x144)/4]=0x3A888889u;
    s1[(0x3b998+0x188)/4]=0x3A888889u;
    s1[(0x3b998+0x190)/4]=0x3A888889u;
    s1[(0x3b998+0x194)/4]=0x3A888889u;
    s1[(0x3b998+0x14c)/4]=0x3AF0F0F1u;
    s1[(0x3b998+0x18c)/4]=0x3AF0F0F1u;

    s1[(0x699cc+0x124)/4]=0x3A888889u;
    s1[(0x699cc+0x130)/4]=0x3A888889u;
    s1[(0x699cc+0x12c)/4]=0x3AF0F0F1u;
    s1[(0x699cc+0x140)/4]=0x3AF0F0F1u;

    s1[(0x6fc54+0x164)/4]=0x3A888889u;
    s1[(0x6fc54+0x198)/4]=0x3A888889u;
    s1[(0x6fc54+0x15c)/4]=0x3AF0F0F1u;
    s1[(0x6fc54+0x1a8)/4]=0x3AF0F0F1u;

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

    s1[(0x737dc+0x2e8)/4]=0x3A888889u;
    s1[(0x737dc+0x314)/4]=0x3A888889u;
    s1[(0x737dc+0x31c)/4]=0x3A888889u;
    s1[(0x737dc+0x320)/4]=0x3A888889u;
    s1[(0x737dc+0x2f0)/4]=0x3AF0F0F1u;
    s1[(0x737dc+0x318)/4]=0x3AF0F0F1u;

    s1[(0x73c00+0x350)/4]=0x3A888889u;
    s1[(0x73c00+0x37c)/4]=0x3A888889u;
    s1[(0x73c00+0x384)/4]=0x3A888889u;
    s1[(0x73c00+0x388)/4]=0x3A888889u;
    s1[(0x73c00+0x358)/4]=0x3AF0F0F1u;
    s1[(0x73c00+0x380)/4]=0x3AF0F0F1u;

    s1[(0x7440C+0x2F0)/4]=0x3A888889u;
    s1[(0x7440C+0x30C)/4]=0x3A888889u;
    s1[(0x7440C+0x314)/4]=0x3A888889u;
    s1[(0x7440C+0x318)/4]=0x3A888889u;
    s1[(0x7440C+0x2F8)/4]=0x3AF0F0F1u;
    s1[(0x7440C+0x310)/4]=0x3AF0F0F1u;

    s1[(0x74800+0x330)/4]=0x3A888889u;
    s1[(0x74800+0x34C)/4]=0x3A888889u;
    s1[(0x74800+0x354)/4]=0x3A888889u;
    s1[(0x74800+0x358)/4]=0x3A888889u;
    s1[(0x74800+0x338)/4]=0x3AF0F0F1u;
    s1[(0x74800+0x350)/4]=0x3AF0F0F1u;
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

    static const uint8_t w960_r0[]  = {0x5F,0xF4,0x70,0x70};
    static const uint8_t h544_r0[]  = {0x5F,0xF4,0x08,0x70};
    static const uint8_t w960_r1[]  = {0x5F,0xF4,0x70,0x71};
    static const uint8_t h544_r1[]  = {0x5F,0xF4,0x08,0x71};
    static const uint8_t w960_r3[]  = {0x5F,0xF4,0x70,0x73};
    static const uint8_t h544_r9[]  = {0x5F,0xF4,0x08,0x79};
    static const uint8_t w960_r10[] = {0x5F,0xF4,0x70,0x7A};
    static const uint8_t s512_lr[]  = {0x5F,0xF4,0x00,0x7E};
    static const uint8_t s512_r3[]  = {0x5F,0xF4,0x00,0x73};

    inject(info.modid, 0, 0x284,   w960_r0,  4);
    inject(info.modid, 0, 0x28A,   h544_r0,  4);
    inject(info.modid, 0, 0x45E9C, h544_r0,  4);
    inject(info.modid, 0, 0x45EA6, w960_r0,  4);
    inject(info.modid, 0, 0x45EC4, w960_r3,  4);
    inject(info.modid, 0, 0x6974,  w960_r1,  4);
    inject(info.modid, 0, 0x697A,  h544_r1,  4);
    inject(info.modid, 0, 0xC46C,  h544_r9,  4);
    inject(info.modid, 0, 0xC474,  w960_r10, 4);
    inject(info.modid, 0, 0xC48A,  w960_r3,  4);
    inject(info.modid, 0, 0xC4F6,  w960_r3,  4);
    inject(info.modid, 0, 0xC62C,  s512_lr,  4);
    inject(info.modid, 0, 0xC644,  s512_r3,  4);

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
