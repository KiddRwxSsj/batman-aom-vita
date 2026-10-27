#include "input.h"
#include "java.h"
#include "gfx.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <psp2/touch.h>
#include <psp2/ctrl.h>

#define MAX_FINGERS 5
#define CONTROLS_PATH DATA_PATH "controls.txt"
#define CONTROLS_VERSION "# controls v3"

enum { ACTION_DOWN = 0, ACTION_UP = 1, ACTION_MOVE = 2 };

// vita touch ids keep counting up, android pointer ids are small and get reused
static struct {
    int active;
    int vita_id;
    int x, y;
} fingers[MAX_FINGERS];

// android hands out the lowest free pointer id, and the combat code reads id 0 as "no finger yet":
// a lone touch with any other id counts as the second finger of a two finger block.
// so buttons borrow ids from the same pool as real fingers
static int virtual_id = -1, hold_id = -1;

static int id_used(int id) {
    if (id == virtual_id || id == hold_id)
        return 1;
    for (int s = 0; s < MAX_FINGERS; s++)
        if (fingers[s].active && s == id)
            return 1;
    return 0;
}

static int free_id(void) {
    for (int id = 0; id < MAX_FINGERS; id++)
        if (!id_used(id))
            return id;
    return -1;
}

// the game only knows touch, so buttons get played back as taps and swipes
// on a pointer id the real fingers never use
enum { G_TAP, G_SWIPE, G_HOLD };

typedef struct {
    uint32_t button;
    int type;
    int x, y, dx, dy;
} mapping;

static mapping maps[32];
static int num_maps;

static struct {
    int busy, tick, steps, hold;
    int x0, y0, x1, y1;
} gesture;

// presses that land while a gesture plays wait here, so quick taps for combos don't get dropped
typedef struct { int x0, y0, x1, y1, steps; } pending;
static pending queue[4];
static int queue_len;

// held buttons (block) keep a second virtual finger down until released
static const mapping *held;

static int stick_armed = 1;

// "gamepad 1" in controls.txt sends the buttons as an android gamepad instead, the engine maps
// those to XboxTypeS_* keys (initMap). off by default, the game's own bindings for them are unknown
static int gamepad;

static const struct { uint32_t bit; int keycode; } pad_keys[] = {
    { SCE_CTRL_CROSS, 96 }, { SCE_CTRL_CIRCLE, 97 }, { SCE_CTRL_SQUARE, 99 }, { SCE_CTRL_TRIANGLE, 100 },
    { SCE_CTRL_LTRIGGER, 102 }, { SCE_CTRL_RTRIGGER, 103 }, { SCE_CTRL_START, 108 }, { SCE_CTRL_SELECT, 109 },
    { SCE_CTRL_UP, 19 }, { SCE_CTRL_DOWN, 20 }, { SCE_CTRL_LEFT, 21 }, { SCE_CTRL_RIGHT, 22 },
};

static const struct { const char *name; uint32_t bit; } button_names[] = {
    { "CROSS", SCE_CTRL_CROSS }, { "CIRCLE", SCE_CTRL_CIRCLE },
    { "SQUARE", SCE_CTRL_SQUARE }, { "TRIANGLE", SCE_CTRL_TRIANGLE },
    { "UP", SCE_CTRL_UP }, { "DOWN", SCE_CTRL_DOWN },
    { "LEFT", SCE_CTRL_LEFT }, { "RIGHT", SCE_CTRL_RIGHT },
    { "L", SCE_CTRL_LTRIGGER }, { "R", SCE_CTRL_RTRIGGER },
    { "START", SCE_CTRL_START }, { "SELECT", SCE_CTRL_SELECT },
};

// screen is 960x544, swipes are start point + offset. the right stick always
// swipes from the middle in whatever direction it gets flicked
static const char *default_controls =
    CONTROLS_VERSION "\n"
    "# <button> tap <x> <y>\n"
    "# <button> hold <x> <y>       finger stays down while the button is held\n"
    "# <button> swipe <x> <y> <dx> <dy>\n"
    "# screen is 960x544, the right stick swipes in the direction it's flicked\n"
    "# gamepad 1 sends the buttons as an android gamepad instead of taps and swipes\n"
    "gamepad 0\n"
    "# attack (tap 3 times for the combo)\n"
    "SQUARE tap 620 300\n"
    "CROSS tap 620 300\n"
    "# block (holds the shield icon)\n"
    "TRIANGLE hold 55 490\n"
    "# switch stance (bottom right icon)\n"
    "CIRCLE tap 905 490\n"
    "# special (bottom middle) and the small icon next to it\n"
    "R tap 480 490\n"
    "L tap 410 490\n"
    "# pause\n"
    "START tap 480 14\n"
    "UP swipe 480 380 0 -220\n"
    "DOWN swipe 480 160 0 220\n"
    "LEFT swipe 600 272 -240 0\n"
    "RIGHT swipe 360 272 240 0\n";

static void parse_controls(const char *text) {
    char line[128], name[16], kind[16];
    num_maps = 0;
    while (*text && num_maps < (int)(sizeof(maps) / sizeof(maps[0]))) {
        int len = strcspn(text, "\n");
        if (len >= (int)sizeof(line))
            len = sizeof(line) - 1;
        memcpy(line, text, len);
        line[len] = 0;
        text += strcspn(text, "\n");
        if (*text)
            text++;

        mapping m = {0};
        int on;
        if (sscanf(line, "gamepad %d", &on) == 1) {
            gamepad = on;
            continue;
        }
        int n = sscanf(line, "%15s %15s %d %d %d %d", name, kind, &m.x, &m.y, &m.dx, &m.dy);
        if (line[0] == '#' || n < 4)
            continue;
        m.type = !strcasecmp(kind, "swipe") ? G_SWIPE : !strcasecmp(kind, "hold") ? G_HOLD : G_TAP;
        for (int i = 0; i < (int)(sizeof(button_names) / sizeof(button_names[0])); i++)
            if (!strcasecmp(name, button_names[i].name))
                m.button = button_names[i].bit;
        if (!m.button)
            continue;
        maps[num_maps++] = m;
    }
}

static void load_controls(void) {
    void *buf;
    size_t size;
    if (!file_load(CONTROLS_PATH, &buf, &size)) {
        file_save(CONTROLS_PATH, default_controls, strlen(default_controls));
        parse_controls(default_controls);
        return;
    }
    char *text = realloc(buf, size + 1);
    text[size] = 0;
    if (strncmp(text, CONTROLS_VERSION, strlen(CONTROLS_VERSION))) {
        // older defaults, keep a copy and write the new ones
        file_save(CONTROLS_PATH ".old", text, size);
        file_save(CONTROLS_PATH, default_controls, strlen(default_controls));
        free(text);
        parse_controls(default_controls);
        LOG("controls: replaced old controls.txt (copy in controls.txt.old)");
        return;
    }
    parse_controls(text);
    free(text);
    LOG("controls: %d mappings, gamepad %d", num_maps, gamepad);
}

void input_init(void) {
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    load_controls();
}

static void send(int action, int x, int y, int pointer) {
    VLOG("touch %d %d,%d id %d", action, x, y, pointer);
    NativeCallback_InputEvent(&jni, java_thiz, action, x, y, pointer, (int64_t)uptime_ms());
}

// one gesture at a time. a tap is held for a few polls so the game sees it,
// a swipe is drawn over `steps` polls
static int start_gesture(int x0, int y0, int x1, int y1, int steps) {
    if (gesture.busy) {
        if (queue_len < (int)(sizeof(queue) / sizeof(queue[0])))
            queue[queue_len++] = (pending){ x0, y0, x1, y1, steps };
        return 0;
    }
    gesture.busy = 1;
    gesture.tick = 0;
    gesture.steps = steps;
    gesture.hold = steps ? 0 : 6;
    gesture.x0 = x0;
    gesture.y0 = y0;
    gesture.x1 = x1;
    gesture.y1 = y1;
    virtual_id = free_id();
    if (virtual_id < 0) {
        gesture.busy = 0;
        return 0;
    }
    send(ACTION_DOWN, x0, y0, virtual_id);
    return 1;
}

static void run_gesture(void) {
    if (!gesture.busy)
        return;
    gesture.tick++;
    if (gesture.tick <= gesture.steps) {
        int x = gesture.x0 + (gesture.x1 - gesture.x0) * gesture.tick / gesture.steps;
        int y = gesture.y0 + (gesture.y1 - gesture.y0) * gesture.tick / gesture.steps;
        send(ACTION_MOVE, x, y, virtual_id);
        return;
    }
    if (gesture.tick <= gesture.steps + gesture.hold)
        return;
    send(ACTION_UP, gesture.x1, gesture.y1, virtual_id);
    virtual_id = -1;
    gesture.busy = 0;
    if (queue_len) {
        pending p = queue[0];
        memmove(queue, queue + 1, --queue_len * sizeof(queue[0]));
        start_gesture(p.x0, p.y0, p.x1, p.y1, p.steps);
    }
}

static void poll_gamepad(const SceCtrlData *pad, uint32_t changed) {
    int64_t now = uptime_ms();
    for (int i = 0; i < (int)(sizeof(pad_keys) / sizeof(pad_keys[0])); i++)
        if (changed & pad_keys[i].bit)
            NativeCallback_JoystickButtonEvent(&jni, java_thiz, 1, (pad->buttons & pad_keys[i].bit) ? ACTION_DOWN : ACTION_UP,
                                               pad_keys[i].keycode, now);
    // android AXIS_X, AXIS_Y, AXIS_Z, AXIS_RZ
    static const int axes[4] = { 0, 1, 11, 14 };
    static int last[4] = { 128, 128, 128, 128 };
    int cur[4] = { pad->lx, pad->ly, pad->rx, pad->ry };
    for (int i = 0; i < 4; i++) {
        if (cur[i] == last[i])
            continue;
        last[i] = cur[i];
        float v = (cur[i] - 128) / 128.0f;
        if (v > -0.15f && v < 0.15f)
            v = 0.0f;
        NativeCallback_JoystickAxisEvent(&jni, java_thiz, 1, ACTION_MOVE, axes[i], v, now);
    }
}

static void poll_buttons(void) {
    static uint32_t old;
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);
    uint32_t pressed = pad.buttons & ~old, changed = pad.buttons ^ old;
    old = pad.buttons;

    if (gamepad) {
        poll_gamepad(&pad, changed);
        return;
    }

#ifdef DEBUG_SHOTS
    if (pressed & SCE_CTRL_SELECT)
        shot_req = 1;
#endif
    if (held && !(pad.buttons & held->button)) {
        send(ACTION_UP, held->x, held->y, hold_id);
        held = NULL;
        hold_id = -1;
    }
    for (int i = 0; i < num_maps; i++) {
        mapping *m = &maps[i];
        if (!(pressed & m->button))
            continue;
        if (m->type == G_HOLD) {
            if (!held && (hold_id = free_id()) >= 0) {
                held = m;
                send(ACTION_DOWN, m->x, m->y, hold_id);
            }
        } else if (m->type == G_SWIPE)
            start_gesture(m->x, m->y, m->x + m->dx, m->y + m->dy, 10);
        else
            start_gesture(m->x, m->y, m->x, m->y, 0);
    }

    // fire once per flick, re-arm when the stick comes back to the middle
    float sx = (pad.rx - 128) / 128.0f, sy = (pad.ry - 128) / 128.0f;
    float len = sqrtf(sx * sx + sy * sy);
    if (len < 0.3f) {
        stick_armed = 1;
    } else if (stick_armed && len > 0.7f) {
        stick_armed = 0;
        // snap to the 4 sides, the game only knows up/down/left/right swipes
        int dx = 0, dy = 0;
        if (fabsf(sx) > fabsf(sy))
            dx = sx > 0 ? 240 : -240;
        else
            dy = sy > 0 ? 220 : -220;
        start_gesture(SCREEN_W / 2 - dx / 2, SCREEN_H / 2 - dy / 2, SCREEN_W / 2 + dx / 2, SCREEN_H / 2 + dy / 2, 10);
    }
}

static void poll_touch(void) {
    SceTouchData touch;
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) < 1)
        return;

    int seen[MAX_FINGERS] = {0};

    for (int i = 0; i < touch.reportNum && i < MAX_FINGERS; i++) {
        int id = touch.report[i].id;
        int x = touch.report[i].x * SCREEN_W / 1920;
        int y = touch.report[i].y * SCREEN_H / 1088;

        int slot = -1;
        for (int s = 0; s < MAX_FINGERS; s++) {
            if (fingers[s].active && fingers[s].vita_id == id) {
                slot = s;
                break;
            }
        }

        if (slot < 0) {
            slot = free_id();
            if (slot < 0)
                continue;
            fingers[slot].active = 1;
            fingers[slot].vita_id = id;
            fingers[slot].x = x;
            fingers[slot].y = y;
            send(ACTION_DOWN, x, y, slot);
        } else if (fingers[slot].x != x || fingers[slot].y != y) {
            fingers[slot].x = x;
            fingers[slot].y = y;
            send(ACTION_MOVE, x, y, slot);
        }
        seen[slot] = 1;
    }

    for (int s = 0; s < MAX_FINGERS; s++) {
        if (fingers[s].active && !seen[s]) {
            send(ACTION_UP, fingers[s].x, fingers[s].y, s);
            fingers[s].active = 0;
        }
    }
}

void input_poll(void) {
    // java drops touches until the engine hides its splash
    if (!splash_hidden)
        return;
    poll_touch();
    poll_buttons();
    run_gesture();
}
