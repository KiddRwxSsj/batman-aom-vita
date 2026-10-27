#include <stdint.h>

#include "loader.h"
#include "util.h"

extern void kuser_cmpxchg(void);
extern void kuser_memory_barrier(void);

// libgcc's atomics in the .so jump straight into the linux kernel helper page, which the
// vita doesn't have. their literal pools all sit in one spot near the end of .text
static void patch_kuser_helpers(void) {
    int n = 0;
    for (uint32_t off = 0x1ec0000; off < 0x1ec2000; off += 4) {
        uint32_t *w = (uint32_t *)(ue3_mod.load_addr + off);
        if (*w == 0xffff0fc0) {
            *w = (uintptr_t)&kuser_cmpxchg;
            n++;
        } else if (*w == 0xffff0fa0) {
            *w = (uintptr_t)&kuser_memory_barrier;
            n++;
        }
    }
    LOG("kuser helpers: %d call sites", n);
}

// no stamina waits. spending has to stay real: the level only starts once the menu's bar animation
// sees the value drop (NavMenu.AS_OnStaminaConsumptionComplete). the regen check calls IncreaseStamina
// a few seconds after any spend, which now fills the bar to max
static void patch_stamina(void) {
    uint32_t *inc = (uint32_t *)ue3_sym("_ZN17UPlayerSaveSystem15IncreaseStaminaEf");
    if (!inc)
        return;
    inc[0] = 0xe5903040; // ldr r3, [r0, #0x40]
    inc[1] = 0xe59320d0; // ldr r2, [r3, #0xd0]
    inc[2] = 0xe58320d4; // str r2, [r3, #0xd4]
    inc[3] = 0xe12fff1e; // bx lr
    LOG("stamina patch applied");
}

// android's cpufeatures reads /proc/cpuinfo and /sys/devices/system/cpu, neither exists here, so it
// reported 1 core and no neon. games get 3 of the vita's cortex-a9 cores
#define CPU_FAMILY_ARM 1
#define CPU_FEATURES (1 | 2 | 4 | 8 | 16 | 32 | 64) // armv7, vfpv3, neon, ldrex/strex, vfpv2, d32, fp16

static int cpu_count(void) {
    static int logged;
    if (!logged++)
        LOG("android_getCpuCount from %x", (unsigned)((uintptr_t)__builtin_return_address(0) - ue3_mod.load_addr));
    return 3;
}

static int cpu_family(void) {
    return CPU_FAMILY_ARM;
}

static uint64_t cpu_features(void) {
    static int logged;
    if (!logged++)
        LOG("android_getCpuFeatures from %x", (unsigned)((uintptr_t)__builtin_return_address(0) - ue3_mod.load_addr));
    return CPU_FEATURES;
}

void patch_game(void) {
    extern void store_init(void);
    store_init();
    hook_arm(ue3_sym("android_getCpuCount"), (uintptr_t)cpu_count);
    hook_arm(ue3_sym("android_getCpuFamily"), (uintptr_t)cpu_family);
    hook_arm(ue3_sym("android_getCpuFeatures"), (uintptr_t)cpu_features);
    extern void app_sleep(float secs);
    hook_arm(ue3_sym("_Z8appSleepf"), (uintptr_t)app_sleep);
    patch_kuser_helpers();
    patch_stamina();
}
