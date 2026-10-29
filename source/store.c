// store tweaks, set in DATA_PATH "store.txt": cheaper batsuits, every suit buyable with regular
// currency (premium only and console unlock ones too), premium currency for each won mission and
// an unlock everything switch. the suit table and the save live in unrealscript objects, so this
// goes through the engine's own reflection (properties found by name) and runs on the game thread
// from wrapped natives
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "loader.h"
#include "util.h"

#define SUITS 16
// ESuitPurchaseMethod
#define ESUM_RegularOrPremium 0
#define ESUM_PremiumOnly 1
#define ESUM_ConsoleUnlock 2

static struct {
    int discount;
    int premium_per_win;
    int unlock_all;
} cfg = { 90, 5, 0 };

static const char cfg_default[] =
    "# store settings, read at every game start\n"
    "# suit_discount: percent taken off every batsuit price (0 = original prices)\n"
    "suit_discount 90\n"
    "# premium_per_win: premium currency given for each mission won (0 = off)\n"
    "premium_per_win 5\n"
    "# unlock_all_suits: 1 gives every batsuit for free\n"
    "unlock_all_suits 0\n";

static void load_config(void) {
    char *buf;
    size_t size;
    if (!file_load(DATA_PATH "store.txt", (void **)&buf, &size)) {
        file_save(DATA_PATH "store.txt", cfg_default, sizeof(cfg_default) - 1);
        return;
    }
    buf = realloc(buf, size + 1);
    buf[size] = 0;
    for (char *line = strtok(buf, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        char key[64];
        int val;
        if (line[0] == '#' || sscanf(line, "%63s %d", key, &val) != 2)
            continue;
        if (!strcmp(key, "suit_discount"))
            cfg.discount = val < 0 ? 0 : val > 100 ? 100 : val;
        else if (!strcmp(key, "premium_per_win"))
            cfg.premium_per_win = val < 0 ? 0 : val;
        else if (!strcmp(key, "unlock_all_suits"))
            cfg.unlock_all = val != 0;
    }
    free(buf);
    LOG("store: discount %d%%, %d premium per win, unlock all %d", cfg.discount, cfg.premium_per_win, cfg.unlock_all);
}

// engine side. TCHAR is a 4 byte wchar_t in the .so
typedef uint32_t tchar;
static void *(*StaticFindObject)(void *cls, void *outer, const tchar *name, unsigned exact);
static void *(*FindFieldProp)(void *ustruct, const tchar *name);
static void (*ProcessEvent)(void *obj, void *func, void *parms, void *result);

static const tchar *T(const char *s) {
    static tchar buf[4][128];
    static int n;
    tchar *o = buf[n++ & 3];
    int i = 0;
    for (; s[i] && i < 127; i++)
        o[i] = (uint8_t)s[i];
    o[i] = 0;
    return o;
}

#define OBJ_CLASS 0x34

// where ArrayDim/ElementSize/Offset sit inside a UProperty, found at runtime from PlayerSuitStats
// (a native member at 0x26c in UPersistentGameData, 16 entries of 40 bytes)
static int p_dim = -1, p_off = -1;

static int detect_layout(void *cls) {
    uint8_t *p = FindFieldProp(cls, T("PlayerSuitStats"));
    if (!p)
        return 0;
    for (int k = 0x3c; k < 0x90; k += 4) {
        uint32_t v = *(uint32_t *)(p + k);
        if (p_dim < 0 && v == SUITS && *(uint32_t *)(p + k + 4) == 40)
            p_dim = k;
        if (p_off < 0 && v == 0x26c)
            p_off = k;
    }
    LOG("store: property layout dim %x offset %x", p_dim, p_off);
    return p_dim >= 0 && p_off >= 0;
}

static int prop_offset(void *ustruct, const char *name, int *elem_size) {
    uint8_t *p = FindFieldProp(ustruct, T(name));
    if (!p) {
        if (strcmp(name, "ReturnValue"))
            LOG("store: no property %s", name);
        return -1;
    }
    if (elem_size)
        *elem_size = *(int *)(p + p_dim + 4);
    return *(int *)(p + p_off);
}

// calls an unrealscript function on obj, params by name (ints, bytes and bools). returns the
// ReturnValue if there is one
static int call_fn(void *obj, const char *path, int n, const char **names, const int *vals) {
    uint8_t *fn = StaticFindObject(NULL, NULL, T(path), 0);
    if (!fn) {
        LOG("store: no function %s", path);
        return 0;
    }
    uint8_t parms[256];
    memset(parms, 0, sizeof(parms));
    for (int i = 0; i < n; i++) {
        int size, off = prop_offset(fn, names[i], &size);
        if (off < 0 || off + 4 > (int)sizeof(parms))
            return 0;
        if (size == 1)
            parms[off] = (uint8_t)vals[i];
        else
            *(int *)(parms + off) = vals[i];
    }
    ProcessEvent(obj, fn, parms, NULL);
    int roff = prop_offset(fn, "ReturnValue", NULL);
    return roff >= 0 && roff + 4 <= (int)sizeof(parms) ? *(int *)(parms + roff) : 0;
}

// suit table: PersistentGameData.PlayerSuitMedia[16] of BatsuitMedia
static int media_off, media_size, f_reg, f_prem, f_method, f_tier;
static int table_ready = -1;
static int orig_reg[SUITS], orig_prem[SUITS], orig_method[SUITS], orig_tier[SUITS];
static int new_reg[SUITS], new_prem[SUITS];

static int discounted(int cost) {
    if (cost <= 0)
        return cost;
    int c = cost * (100 - cfg.discount) / 100;
    return c < 1 ? 1 : c;
}

static void plan_prices(void) {
    // most premium only / console suits still carry the designers' regular price. the ones that
    // don't get their premium price times the regular/premium ratio of the buyable suits of the
    // same tier (nearest tier if that one has none), which is how the game prices the others
    float ratio[8] = { 0 };
    int cnt[8] = { 0 };
    for (int i = 0; i < SUITS; i++)
        if (orig_method[i] == ESUM_RegularOrPremium && orig_reg[i] > 0 && orig_prem[i] > 0 && orig_tier[i] < 8) {
            ratio[orig_tier[i]] += (float)orig_reg[i] / orig_prem[i];
            cnt[orig_tier[i]]++;
        }
    for (int i = 0; i < SUITS; i++) {
        int reg = orig_reg[i];
        if (reg <= 0 && orig_prem[i] > 0) {
            int t = orig_tier[i] < 8 ? orig_tier[i] : 0;
            float r = 0;
            for (int d = 0; d < 8 && r == 0; d++) {
                if (t + d < 8 && cnt[t + d])
                    r = ratio[t + d] / cnt[t + d];
                else if (t - d >= 0 && cnt[t - d])
                    r = ratio[t - d] / cnt[t - d];
            }
            reg = (int)(orig_prem[i] * (r > 0 ? r : 1000));
            reg = (reg + 500) / 1000 * 1000;
        }
        if (reg <= 0)
            reg = 10000;
        // console suits have no premium price at all, a 0 there made them free
        int prem = orig_prem[i];
        if (prem <= 0) {
            int t = orig_tier[i] < 8 ? orig_tier[i] : 0;
            float r = 0;
            for (int d = 0; d < 8 && r == 0; d++) {
                if (t + d < 8 && cnt[t + d])
                    r = ratio[t + d] / cnt[t + d];
                else if (t - d >= 0 && cnt[t - d])
                    r = ratio[t - d] / cnt[t - d];
            }
            prem = (int)(reg / (r > 0 ? r : 1000) + 0.5f);
            if (prem < 1)
                prem = 1;
        }
        new_reg[i] = discounted(reg);
        new_prem[i] = discounted(prem);
    }
}

static void patch_table(uint8_t *gd) {
    if (table_ready < 0) {
        table_ready = 0;
        void *cls = *(void **)(gd + OBJ_CLASS);
        void *media = StaticFindObject(NULL, NULL, T("BatmanLockdown2Game.PersistentGameData.BatsuitMedia"), 0);
        if (!cls || !media || !detect_layout(cls))
            return;
        media_off = prop_offset(cls, "PlayerSuitMedia", &media_size);
        f_reg = prop_offset(media, "RegCost", NULL);
        f_prem = prop_offset(media, "PremCost", NULL);
        f_method = prop_offset(media, "SuitPurchaseMethod", NULL);
        f_tier = prop_offset(media, "SuitTier", NULL);
        if (media_off < 0 || media_size <= 0 || f_reg < 0 || f_prem < 0 || f_method < 0 || f_tier < 0)
            return;
        for (int i = 0; i < SUITS; i++) {
            uint8_t *e = gd + media_off + i * media_size;
            orig_reg[i] = *(int *)(e + f_reg);
            orig_prem[i] = *(int *)(e + f_prem);
            orig_method[i] = e[f_method];
            orig_tier[i] = e[f_tier];
        }
        plan_prices();
        for (int i = 0; i < SUITS; i++)
            LOG("store: suit %2d tier %d method %d reg %d prem %d -> reg %d prem %d", i, orig_tier[i], orig_method[i],
                orig_reg[i], orig_prem[i], new_reg[i], new_prem[i]);
        table_ready = 1;
    }
    if (table_ready != 1)
        return;
    // the object can be rebuilt (map changes), writing the same values again is cheap
    for (int i = 0; i < SUITS; i++) {
        uint8_t *e = gd + media_off + i * media_size;
        *(int *)(e + f_reg) = new_reg[i];
        *(int *)(e + f_prem) = new_prem[i];
        e[f_method] = ESUM_RegularOrPremium;
    }
}

// save side
static void suit_flags(void *save) {
    static int done;
    if (done || table_ready != 1)
        return;
    done = 1;
    const char *n2[] = { "SuitType", "val" };
    const char *n1[] = { "SuitType" };
    for (int i = 0; i < SUITS; i++) {
        int v1[] = { i };
        int unlocked = call_fn(save, "BatmanLockdown2Game.PlayerSaveSystem.GetSuitUnlockedBool", 1, n1, v1);
        int purchased = call_fn(save, "BatmanLockdown2Game.PlayerSaveSystem.GetSuitPurchasedBool", 1, n1, v1);
        LOG("store: suit %2d unlocked %d purchased %d", i, unlocked, purchased);
        // console unlock suits only show up in the store once flagged as unlocked
        if (!unlocked && (cfg.unlock_all || orig_method[i] == ESUM_ConsoleUnlock)) {
            int v[] = { i, 1 };
            call_fn(save, "BatmanLockdown2Game.PlayerSaveSystem.SetSuitUnlockedBool", 2, n2, v);
        }
        if (!purchased && cfg.unlock_all) {
            int v[] = { i, 1 };
            call_fn(save, "BatmanLockdown2Game.PlayerSaveSystem.SetSuitPurchasedBool", 2, n2, v);
        }
    }
}

// wrapped natives: void exec(UObject *this, FFrame &stack, void *result)
typedef void (*exec_fn)(void *self, void *stack, void *result);
static exec_fn orig_get_gamedata, orig_get_suit_level, orig_get_player_suit, orig_mission_completed;

static void get_gamedata(void *self, void *stack, void *result) {
    orig_get_gamedata(self, stack, result);
    void *gd = *(void **)result;
    if (gd)
        patch_table(gd);
}

static void get_suit_level(void *self, void *stack, void *result) {
    orig_get_suit_level(self, stack, result);
    suit_flags(self);
}

static void get_player_suit(void *self, void *stack, void *result) {
    orig_get_player_suit(self, stack, result);
    suit_flags(self);
}

static void mission_completed(void *self, void *stack, void *result) {
    orig_mission_completed(self, stack, result);
    if (cfg.premium_per_win > 0) {
        const char *n[] = { "AddAmount" };
        int v[] = { cfg.premium_per_win };
        call_fn(self, "BatmanLockdown2Game.PlayerSaveSystem.AddPremiumCurrency", 1, n, v);
        LOG("store: mission won, +%d premium", cfg.premium_per_win);
    }
}

// native tables are { const char *name, function, this adjust } and get bound when the script
// package loads, after this runs
static void wrap_native(const char *table, const char *name, exec_fn *orig, exec_fn fn) {
    uintptr_t *t = (uintptr_t *)ue3_sym(table);
    for (; t && t[0]; t += 3)
        if (!strcmp((const char *)t[0], name)) {
            *orig = (exec_fn)t[1];
            t[1] = (uintptr_t)fn;
            return;
        }
    LOG("store: native %s not found", name);
}

void store_init(void) {
    load_config();
    StaticFindObject = (void *)ue3_sym("_ZN7UObject16StaticFindObjectEP6UClassPS_PKwj");
    FindFieldProp = (void *)ue3_sym("_Z9FindFieldI9UPropertyEPT_P7UStructPKw");
    ProcessEvent = (void *)ue3_sym("_ZN7UObject12ProcessEventEP9UFunctionPvS2_");
    if (!StaticFindObject || !FindFieldProp || !ProcessEvent) {
        LOG("store: engine functions missing");
        return;
    }
    const char *gd = "GBatmanLockdown2GameUPersistentGameDataNatives", *ps = "GBatmanLockdown2GameUPlayerSaveSystemNatives";
    wrap_native(gd, "UPersistentGameDataexecGetPersistentGameData", &orig_get_gamedata, get_gamedata);
    wrap_native(ps, "UPlayerSaveSystemexecGetSuitLevel", &orig_get_suit_level, get_suit_level);
    wrap_native(ps, "UPlayerSaveSystemexecGetPlayerSuit", &orig_get_player_suit, get_player_suit);
    wrap_native(ps, "UPlayerSaveSystemexecMissionCompleted", &orig_mission_completed, mission_completed);
}
