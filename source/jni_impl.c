#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <psp2/kernel/processmgr.h>

#include <falso_jni/FalsoJNI.h>
#include <falso_jni/FalsoJNI_Impl.h>

#include "java.h"
#include "audio.h"
#include "gfx.h"
#include "loader.h"
#include "movie.h"
#include "ime.h"
#include "util.h"
#include "bionic.h"

static const char *jstr(jstring s) {
    return s ? jni->GetStringUTFChars(&jni, s, NULL) : "";
}

static void jstr_free(jstring s, const char *c) {
    if (s)
        jni->ReleaseStringUTFChars(&jni, s, (char *)c);
}

#define MAX_VALUES 32

static struct { char *key, *value; } app_values[MAX_VALUES] = {
    { "STORAGE_ROOT", "/sdcard/" },
    { "BASE_DIR", "UnrealEngine3" },
    { "EXTERNAL_ROOT", "/sdcard/" },
    { "LOCAL_IP", "" },
};
static pthread_mutex_t values_lock = PTHREAD_MUTEX_INITIALIZER;

static int find_value(const char *key) {
    for (int i = 0; i < MAX_VALUES; i++)
        if (app_values[i].key && strcmp(app_values[i].key, key) == 0)
            return i;
    return -1;
}

static jboolean HasAppLocalValue(jmethodID id, va_list args) {
    jstring k = va_arg(args, jstring);
    const char *key = jstr(k);
    pthread_mutex_lock(&values_lock);
    int found = find_value(key) >= 0;
    pthread_mutex_unlock(&values_lock);
    VLOG("HasAppLocalValue %s -> %d", key, found);
    jstr_free(k, key);
    return found;
}

static jobject GetAppLocalValue(jmethodID id, va_list args) {
    jstring k = va_arg(args, jstring);
    const char *key = jstr(k);
    pthread_mutex_lock(&values_lock);
    int i = find_value(key);
    jstring ret = i >= 0 ? jni->NewStringUTF(&jni, app_values[i].value) : NULL;
    pthread_mutex_unlock(&values_lock);
    VLOG("GetAppLocalValue %s -> %s", key, i >= 0 ? app_values[i].value : "(null)");
    jstr_free(k, key);
    return ret;
}

static void SetAppLocalValue(jmethodID id, va_list args) {
    jstring k = va_arg(args, jstring);
    jstring v = va_arg(args, jstring);
    const char *key = jstr(k), *value = jstr(v);
    pthread_mutex_lock(&values_lock);
    int i = find_value(key);
    if (i < 0) {
        for (i = 0; i < MAX_VALUES && app_values[i].key; i++);
        if (i < MAX_VALUES)
            app_values[i].key = strdup(key);
    }
    if (i < MAX_VALUES)
        app_values[i].value = strdup(value);
    pthread_mutex_unlock(&values_lock);
    jstr_free(k, key);
    jstr_free(v, value);
}

enum {
    F_RED = 1, F_GREEN, F_BLUE, F_ALPHA, F_DEPTH, F_STENCIL, F_SAMPLE_BUFFERS, F_SAMPLE_SAMPLES,
    F_ACTION_DOWN, F_ACTION_UP, F_ACTION_POINTER_DOWN, F_ACTION_POINTER_UP, F_DESCRIPTOR,
    F_IS_HIDDEN, F_IS_UNLOCKED, F_KEYCODES,
};

FieldsInt fieldsInt[] = {
    { F_RED, 8 },
    { F_GREEN, 8 },
    { F_BLUE, 8 },
    { F_ALPHA, 8 },
    { F_DEPTH, 24 },
    { F_STENCIL, 8 },
    { F_SAMPLE_BUFFERS, 0 },
    { F_SAMPLE_SAMPLES, 0 },
    // android.view.MotionEvent / KeyEvent, falso hands back 1 for unknown ints which turns every touch into garbage
    { F_ACTION_DOWN, 0 },
    { F_ACTION_UP, 1 },
    { F_ACTION_POINTER_DOWN, 5 },
    { F_ACTION_POINTER_UP, 6 },
    { F_DESCRIPTOR, -1 },
    // android.view.KeyEvent / MotionEvent constants for the engine's keycode map (initMap)
    { F_KEYCODES + 0, 61 },
    { F_KEYCODES + 1, 66 },
    { F_KEYCODES + 2, 62 },
    { F_KEYCODES + 3, 4 },
    { F_KEYCODES + 4, 82 },
    { F_KEYCODES + 5, 3 },
    { F_KEYCODES + 6, 67 },
    { F_KEYCODES + 7, 111 },
    { F_KEYCODES + 8, 7 },
    { F_KEYCODES + 9, 8 },
    { F_KEYCODES + 10, 9 },
    { F_KEYCODES + 11, 10 },
    { F_KEYCODES + 12, 11 },
    { F_KEYCODES + 13, 12 },
    { F_KEYCODES + 14, 13 },
    { F_KEYCODES + 15, 14 },
    { F_KEYCODES + 16, 15 },
    { F_KEYCODES + 17, 16 },
    { F_KEYCODES + 18, 29 },
    { F_KEYCODES + 19, 30 },
    { F_KEYCODES + 20, 31 },
    { F_KEYCODES + 21, 32 },
    { F_KEYCODES + 22, 33 },
    { F_KEYCODES + 23, 34 },
    { F_KEYCODES + 24, 35 },
    { F_KEYCODES + 25, 36 },
    { F_KEYCODES + 26, 37 },
    { F_KEYCODES + 27, 38 },
    { F_KEYCODES + 28, 39 },
    { F_KEYCODES + 29, 40 },
    { F_KEYCODES + 30, 41 },
    { F_KEYCODES + 31, 42 },
    { F_KEYCODES + 32, 43 },
    { F_KEYCODES + 33, 44 },
    { F_KEYCODES + 34, 45 },
    { F_KEYCODES + 35, 46 },
    { F_KEYCODES + 36, 47 },
    { F_KEYCODES + 37, 48 },
    { F_KEYCODES + 38, 49 },
    { F_KEYCODES + 39, 50 },
    { F_KEYCODES + 40, 51 },
    { F_KEYCODES + 41, 52 },
    { F_KEYCODES + 42, 53 },
    { F_KEYCODES + 43, 54 },
    { F_KEYCODES + 44, 17 },
    { F_KEYCODES + 45, 81 },
    { F_KEYCODES + 46, 69 },
    { F_KEYCODES + 47, 78 },
    { F_KEYCODES + 48, 57 },
    { F_KEYCODES + 49, 58 },
    { F_KEYCODES + 50, 59 },
    { F_KEYCODES + 51, 60 },
    { F_KEYCODES + 52, 75 },
    { F_KEYCODES + 53, 74 },
    { F_KEYCODES + 54, 70 },
    { F_KEYCODES + 55, 55 },
    { F_KEYCODES + 56, 56 },
    { F_KEYCODES + 57, 76 },
    { F_KEYCODES + 58, 68 },
    { F_KEYCODES + 59, 71 },
    { F_KEYCODES + 60, 73 },
    { F_KEYCODES + 61, 72 },
    { F_KEYCODES + 62, 188 },
    { F_KEYCODES + 63, 189 },
    { F_KEYCODES + 64, 190 },
    { F_KEYCODES + 65, 191 },
    { F_KEYCODES + 66, 192 },
    { F_KEYCODES + 67, 193 },
    { F_KEYCODES + 68, 194 },
    { F_KEYCODES + 69, 195 },
    { F_KEYCODES + 70, 196 },
    { F_KEYCODES + 71, 197 },
    { F_KEYCODES + 72, 198 },
    { F_KEYCODES + 73, 199 },
    { F_KEYCODES + 74, 20 },
    { F_KEYCODES + 75, 19 },
    { F_KEYCODES + 76, 22 },
    { F_KEYCODES + 77, 21 },
    { F_KEYCODES + 78, 100 },
    { F_KEYCODES + 79, 97 },
    { F_KEYCODES + 80, 96 },
    { F_KEYCODES + 81, 99 },
    { F_KEYCODES + 82, 104 },
    { F_KEYCODES + 83, 105 },
    { F_KEYCODES + 84, 102 },
    { F_KEYCODES + 85, 103 },
    { F_KEYCODES + 86, 108 },
    { F_KEYCODES + 87, 109 },
    { F_KEYCODES + 88, 106 },
    { F_KEYCODES + 89, 107 },
    { F_KEYCODES + 90, 0 },
    { F_KEYCODES + 91, 1 },
    { F_KEYCODES + 92, 11 },
    { F_KEYCODES + 93, 14 },
    { F_KEYCODES + 94, 17 },
    { F_KEYCODES + 95, 18 },
};

FieldsBoolean fieldsBoolean[] = {
    { F_IS_HIDDEN, 0 },
    { F_IS_UNLOCKED, 0 },
};

NameToFieldID nameToFieldId[] = {
    { F_RED, "redSize", FIELD_TYPE_INT },
    { F_GREEN, "greenSize", FIELD_TYPE_INT },
    { F_BLUE, "blueSize", FIELD_TYPE_INT },
    { F_ALPHA, "alphaSize", FIELD_TYPE_INT },
    { F_DEPTH, "depthSize", FIELD_TYPE_INT },
    { F_STENCIL, "stencilSize", FIELD_TYPE_INT },
    { F_SAMPLE_BUFFERS, "sampleBuffers", FIELD_TYPE_INT },
    { F_SAMPLE_SAMPLES, "sampleSamples", FIELD_TYPE_INT },
    { F_ACTION_DOWN, "ACTION_DOWN", FIELD_TYPE_INT },
    { F_ACTION_UP, "ACTION_UP", FIELD_TYPE_INT },
    { F_ACTION_POINTER_DOWN, "ACTION_POINTER_DOWN", FIELD_TYPE_INT },
    { F_ACTION_POINTER_UP, "ACTION_POINTER_UP", FIELD_TYPE_INT },
    { F_DESCRIPTOR, "descriptor", FIELD_TYPE_INT },
    { F_IS_HIDDEN, "bIsHidden", FIELD_TYPE_BOOLEAN },
    { F_IS_UNLOCKED, "bIsUnlocked", FIELD_TYPE_BOOLEAN },
    { F_KEYCODES + 0, "KEYCODE_TAB", FIELD_TYPE_INT },
    { F_KEYCODES + 1, "KEYCODE_ENTER", FIELD_TYPE_INT },
    { F_KEYCODES + 2, "KEYCODE_SPACE", FIELD_TYPE_INT },
    { F_KEYCODES + 3, "KEYCODE_BACK", FIELD_TYPE_INT },
    { F_KEYCODES + 4, "KEYCODE_MENU", FIELD_TYPE_INT },
    { F_KEYCODES + 5, "KEYCODE_HOME", FIELD_TYPE_INT },
    { F_KEYCODES + 6, "KEYCODE_DEL", FIELD_TYPE_INT },
    { F_KEYCODES + 7, "KEYCODE_ESCAPE", FIELD_TYPE_INT },
    { F_KEYCODES + 8, "KEYCODE_0", FIELD_TYPE_INT },
    { F_KEYCODES + 9, "KEYCODE_1", FIELD_TYPE_INT },
    { F_KEYCODES + 10, "KEYCODE_2", FIELD_TYPE_INT },
    { F_KEYCODES + 11, "KEYCODE_3", FIELD_TYPE_INT },
    { F_KEYCODES + 12, "KEYCODE_4", FIELD_TYPE_INT },
    { F_KEYCODES + 13, "KEYCODE_5", FIELD_TYPE_INT },
    { F_KEYCODES + 14, "KEYCODE_6", FIELD_TYPE_INT },
    { F_KEYCODES + 15, "KEYCODE_7", FIELD_TYPE_INT },
    { F_KEYCODES + 16, "KEYCODE_8", FIELD_TYPE_INT },
    { F_KEYCODES + 17, "KEYCODE_9", FIELD_TYPE_INT },
    { F_KEYCODES + 18, "KEYCODE_A", FIELD_TYPE_INT },
    { F_KEYCODES + 19, "KEYCODE_B", FIELD_TYPE_INT },
    { F_KEYCODES + 20, "KEYCODE_C", FIELD_TYPE_INT },
    { F_KEYCODES + 21, "KEYCODE_D", FIELD_TYPE_INT },
    { F_KEYCODES + 22, "KEYCODE_E", FIELD_TYPE_INT },
    { F_KEYCODES + 23, "KEYCODE_F", FIELD_TYPE_INT },
    { F_KEYCODES + 24, "KEYCODE_G", FIELD_TYPE_INT },
    { F_KEYCODES + 25, "KEYCODE_H", FIELD_TYPE_INT },
    { F_KEYCODES + 26, "KEYCODE_I", FIELD_TYPE_INT },
    { F_KEYCODES + 27, "KEYCODE_J", FIELD_TYPE_INT },
    { F_KEYCODES + 28, "KEYCODE_K", FIELD_TYPE_INT },
    { F_KEYCODES + 29, "KEYCODE_L", FIELD_TYPE_INT },
    { F_KEYCODES + 30, "KEYCODE_M", FIELD_TYPE_INT },
    { F_KEYCODES + 31, "KEYCODE_N", FIELD_TYPE_INT },
    { F_KEYCODES + 32, "KEYCODE_O", FIELD_TYPE_INT },
    { F_KEYCODES + 33, "KEYCODE_P", FIELD_TYPE_INT },
    { F_KEYCODES + 34, "KEYCODE_Q", FIELD_TYPE_INT },
    { F_KEYCODES + 35, "KEYCODE_R", FIELD_TYPE_INT },
    { F_KEYCODES + 36, "KEYCODE_S", FIELD_TYPE_INT },
    { F_KEYCODES + 37, "KEYCODE_T", FIELD_TYPE_INT },
    { F_KEYCODES + 38, "KEYCODE_U", FIELD_TYPE_INT },
    { F_KEYCODES + 39, "KEYCODE_V", FIELD_TYPE_INT },
    { F_KEYCODES + 40, "KEYCODE_W", FIELD_TYPE_INT },
    { F_KEYCODES + 41, "KEYCODE_X", FIELD_TYPE_INT },
    { F_KEYCODES + 42, "KEYCODE_Y", FIELD_TYPE_INT },
    { F_KEYCODES + 43, "KEYCODE_Z", FIELD_TYPE_INT },
    { F_KEYCODES + 44, "KEYCODE_STAR", FIELD_TYPE_INT },
    { F_KEYCODES + 45, "KEYCODE_PLUS", FIELD_TYPE_INT },
    { F_KEYCODES + 46, "KEYCODE_MINUS", FIELD_TYPE_INT },
    { F_KEYCODES + 47, "KEYCODE_NUM", FIELD_TYPE_INT },
    { F_KEYCODES + 48, "KEYCODE_ALT_LEFT", FIELD_TYPE_INT },
    { F_KEYCODES + 49, "KEYCODE_ALT_RIGHT", FIELD_TYPE_INT },
    { F_KEYCODES + 50, "KEYCODE_SHIFT_LEFT", FIELD_TYPE_INT },
    { F_KEYCODES + 51, "KEYCODE_SHIFT_RIGHT", FIELD_TYPE_INT },
    { F_KEYCODES + 52, "KEYCODE_APOSTROPHE", FIELD_TYPE_INT },
    { F_KEYCODES + 53, "KEYCODE_SEMICOLON", FIELD_TYPE_INT },
    { F_KEYCODES + 54, "KEYCODE_EQUALS", FIELD_TYPE_INT },
    { F_KEYCODES + 55, "KEYCODE_COMMA", FIELD_TYPE_INT },
    { F_KEYCODES + 56, "KEYCODE_PERIOD", FIELD_TYPE_INT },
    { F_KEYCODES + 57, "KEYCODE_SLASH", FIELD_TYPE_INT },
    { F_KEYCODES + 58, "KEYCODE_GRAVE", FIELD_TYPE_INT },
    { F_KEYCODES + 59, "KEYCODE_LEFT_BRACKET", FIELD_TYPE_INT },
    { F_KEYCODES + 60, "KEYCODE_BACKSLASH", FIELD_TYPE_INT },
    { F_KEYCODES + 61, "KEYCODE_RIGHT_BRACKET", FIELD_TYPE_INT },
    { F_KEYCODES + 62, "KEYCODE_BUTTON_1", FIELD_TYPE_INT },
    { F_KEYCODES + 63, "KEYCODE_BUTTON_2", FIELD_TYPE_INT },
    { F_KEYCODES + 64, "KEYCODE_BUTTON_3", FIELD_TYPE_INT },
    { F_KEYCODES + 65, "KEYCODE_BUTTON_4", FIELD_TYPE_INT },
    { F_KEYCODES + 66, "KEYCODE_BUTTON_5", FIELD_TYPE_INT },
    { F_KEYCODES + 67, "KEYCODE_BUTTON_6", FIELD_TYPE_INT },
    { F_KEYCODES + 68, "KEYCODE_BUTTON_7", FIELD_TYPE_INT },
    { F_KEYCODES + 69, "KEYCODE_BUTTON_8", FIELD_TYPE_INT },
    { F_KEYCODES + 70, "KEYCODE_BUTTON_9", FIELD_TYPE_INT },
    { F_KEYCODES + 71, "KEYCODE_BUTTON_10", FIELD_TYPE_INT },
    { F_KEYCODES + 72, "KEYCODE_BUTTON_11", FIELD_TYPE_INT },
    { F_KEYCODES + 73, "KEYCODE_BUTTON_12", FIELD_TYPE_INT },
    { F_KEYCODES + 74, "KEYCODE_DPAD_DOWN", FIELD_TYPE_INT },
    { F_KEYCODES + 75, "KEYCODE_DPAD_UP", FIELD_TYPE_INT },
    { F_KEYCODES + 76, "KEYCODE_DPAD_RIGHT", FIELD_TYPE_INT },
    { F_KEYCODES + 77, "KEYCODE_DPAD_LEFT", FIELD_TYPE_INT },
    { F_KEYCODES + 78, "KEYCODE_BUTTON_Y", FIELD_TYPE_INT },
    { F_KEYCODES + 79, "KEYCODE_BUTTON_B", FIELD_TYPE_INT },
    { F_KEYCODES + 80, "KEYCODE_BUTTON_A", FIELD_TYPE_INT },
    { F_KEYCODES + 81, "KEYCODE_BUTTON_X", FIELD_TYPE_INT },
    { F_KEYCODES + 82, "KEYCODE_BUTTON_L2", FIELD_TYPE_INT },
    { F_KEYCODES + 83, "KEYCODE_BUTTON_R2", FIELD_TYPE_INT },
    { F_KEYCODES + 84, "KEYCODE_BUTTON_L1", FIELD_TYPE_INT },
    { F_KEYCODES + 85, "KEYCODE_BUTTON_R1", FIELD_TYPE_INT },
    { F_KEYCODES + 86, "KEYCODE_BUTTON_START", FIELD_TYPE_INT },
    { F_KEYCODES + 87, "KEYCODE_BUTTON_SELECT", FIELD_TYPE_INT },
    { F_KEYCODES + 88, "KEYCODE_BUTTON_THUMBL", FIELD_TYPE_INT },
    { F_KEYCODES + 89, "KEYCODE_BUTTON_THUMBR", FIELD_TYPE_INT },
    { F_KEYCODES + 90, "AXIS_X", FIELD_TYPE_INT },
    { F_KEYCODES + 91, "AXIS_Y", FIELD_TYPE_INT },
    { F_KEYCODES + 92, "AXIS_Z", FIELD_TYPE_INT },
    { F_KEYCODES + 93, "AXIS_RZ", FIELD_TYPE_INT },
    { F_KEYCODES + 94, "AXIS_LTRIGGER", FIELD_TYPE_INT },
    { F_KEYCODES + 95, "AXIS_RTRIGGER", FIELD_TYPE_INT },
};

static int get_field(int fid) {
    for (int i = 0; i < (int)(sizeof(fieldsInt) / sizeof(fieldsInt[0])); i++)
        if (fieldsInt[i].id == fid)
            return fieldsInt[i].value;
    return -1;
}

static void set_field(int fid, int value) {
    for (int i = 0; i < (int)(sizeof(fieldsInt) / sizeof(fieldsInt[0])); i++)
        if (fieldsInt[i].id == fid)
            fieldsInt[i].value = value;
}

// the engine fills an EGLConfigParms with what it wants, java writes back what it got
static jboolean InitEGL(jmethodID id, va_list args) {
    LOG("initEGL wants rgba %d%d%d%d depth %d stencil %d", get_field(F_RED), get_field(F_GREEN),
        get_field(F_BLUE), get_field(F_ALPHA), get_field(F_DEPTH), get_field(F_STENCIL));
    gfx_init();
    movie_gl_owner(1);
    set_field(F_RED, 8);
    set_field(F_GREEN, 8);
    set_field(F_BLUE, 8);
    set_field(F_ALPHA, 8);
    set_field(F_DEPTH, 24);
    set_field(F_STENCIL, 8);
    set_field(F_SAMPLE_BUFFERS, 0);
    set_field(F_SAMPLE_SAMPLES, 0);
    return JNI_TRUE;
}

// the object itself is never looked at, only its "descriptor" field which is global in falso
static jobject NewFileDescriptor(jmethodID id, va_list args) {
    return (jobject)0x46440000;
}

static jboolean ret_true(jmethodID id, va_list args) { return JNI_TRUE; }
static jboolean ret_false(jmethodID id, va_list args) { return JNI_FALSE; }
static jint ret_int0(jmethodID id, va_list args) { return 0; }
static jlong ret_long0(jmethodID id, va_list args) { return 0; }
static jfloat ret_float0(jmethodID id, va_list args) { return 0.0f; }
static jobject ret_null(jmethodID id, va_list args) { return NULL; }
static jobject ret_empty(jmethodID id, va_list args) { return jni->NewStringUTF(&jni, ""); }
static jobject ret_empty_array(jmethodID id, va_list args) { return jni->NewObjectArray(&jni, 0, NULL, NULL); }
static void nop(jmethodID id, va_list args) {}

static jboolean Swap(jmethodID id, va_list args) {
    gfx_swap();
    return JNI_TRUE;
}

// the engine moves its gl context between the game and rendering threads with these.
// vitagl has one global state, so the movie only touches gl on the thread holding it
static jboolean MakeCurrent(jmethodID id, va_list args) {
    movie_gl_owner(1);
    return JNI_TRUE;
}

static jboolean UnMakeCurrent(jmethodID id, va_list args) {
    movie_gl_owner(0);
    return JNI_TRUE;
}

static void ShutDownApp(jmethodID id, va_list args) {
    LOG("ShutDownApp");
    sceKernelExitProcess(0);
}

static jobject GetMainExpansion(jmethodID id, va_list args) {
    return jni->NewStringUTF(&jni, "/sdcard/" OBB_NAME);
}

// same text as assets/UE3CommandLine.txt in the apk
static jobject GetAppCommandLine(jmethodID id, va_list args) {
    return jni->NewStringUTF(&jni, "Frontend_Base.udk -novsync -installed");
}

static jobject GetDeviceModel(jmethodID id, va_list args) {
    return jni->NewStringUTF(&jni, "PS Vita");
}

static jint GetSDKVersion(jmethodID id, va_list args) { return 17; }
static jint GetDepthSize(jmethodID id, va_list args) { return 24; }
static jint GetPerformanceLevel(jmethodID id, va_list args) { return -1; }
// java caps it at 0.9 on every device
static jfloat GetResolutionScale(jmethodID id, va_list args) { return 0.9f; }

static void HideSplash(jmethodID id, va_list args) {
    LOG("HideSplash");
    splash_hidden = 1;
}

static void ShowWebPage(jmethodID id, va_list args) {
    jstring s = va_arg(args, jstring);
    const char *url = jstr(s);
    LOG("ShowWebPage %s", url);
    jstr_free(s, url);
}

static jint LoadSoundFile(jmethodID id, va_list args) {
    jstring s = va_arg(args, jstring);
    const char *name = jstr(s);
    int ret = audio_load_sound(name);
    jstr_free(s, name);
    return ret;
}

static void UnloadSoundID(jmethodID id, va_list args) {
    audio_unload_sound(va_arg(args, int));
}

static jint PlaySound(jmethodID id, va_list args) {
    int sound = va_arg(args, int);
    int loop = va_arg(args, int);
    return audio_play_sound(sound, loop);
}

static void StopSound(jmethodID id, va_list args) {
    audio_stop_sound(va_arg(args, int));
}

static void SetVolume(jmethodID id, va_list args) {
    int stream = va_arg(args, int);
    audio_set_volume(stream, (float)va_arg(args, double));
}

static void PlaySong(jmethodID id, va_list args) {
    va_arg(args, jobject);
    int fd = get_field(F_DESCRIPTOR);
    int64_t off = va_arg(args, int64_t);
    int64_t len = va_arg(args, int64_t);
    jstring s = va_arg(args, jstring);
    float fade_in = va_arg(args, double);
    float fade_out = va_arg(args, double);
    // the engine never closes these, java's FileDescriptor would have been collected
    if (fd >= 0)
        close_b(fd);
    const char *name = jstr(s);
    audio_play_song(off, len, name, fade_in, fade_out);
    jstr_free(s, name);
}

static void StopSong(jmethodID id, va_list args) {
    audio_stop_song();
}

static void SetSongVolume(jmethodID id, va_list args) {
    audio_song_volume((float)va_arg(args, double));
}

static void UpdateSongPlayer(jmethodID id, va_list args) {
    audio_update_song((float)va_arg(args, double));
}

static void StartVideo(jmethodID id, va_list args) {
    va_arg(args, jobject);
    int fd = get_field(F_DESCRIPTOR);
    int delay = va_arg(args, int);
    int64_t off = va_arg(args, int64_t);
    int64_t len = va_arg(args, int64_t);
    int loop = va_arg(args, int);
    if (fd >= 0)
        close_b(fd);
    // the engine passes a UBOOL bitfield, 0x80 means true
    movie_start(off, len, loop != 0, delay);
}

static void StopVideo(jmethodID id, va_list args) {
    movie_stop();
}

static jboolean IsVideoPlaying(jmethodID id, va_list args) {
    return movie_playing();
}

static void ShowKeyBoard(jmethodID id, va_list args) {
    jstring s = va_arg(args, jstring);
    const char *text = jstr(s);
    double x = va_arg(args, double), y = va_arg(args, double), w = va_arg(args, double), h = va_arg(args, double);
    int password = va_arg(args, int);
    ime_open(text, password);
    jstr_free(s, text);
}

enum {
    M_NONE,
    M_FD_INIT,
    M_SETWBREALM,
    M_SETWBTITLE,
    M_SETWBPLATFORMID,
    M_SETWBBLOWFISHKEY,
    M_PLAYSONG,
    M_STOPSONG,
    M_LOADSOUNDFILE,
    M_UNLOADSOUNDID,
    M_PLAYSOUND,
    M_STOPSOUND,
    M_SETVOLUME,
    M_SETSONGVOLUME,
    M_UPDATESONGPLAYER,
    M_RESTOREMEDIA,
    M_SWAPBUFFERS,
    M_MAKECURRENT,
    M_UNMAKECURRENT,
    M_INITEGL,
    M_SETFIXEDSIZESCALE,
    M_STARTVIDEO,
    M_STOPVIDEO,
    M_ISVIDEOPLAYING,
    M_VIDEOADDTEXTOVERLAY,
    M_SHOWKEYBOARD,
    M_HIDEKEYBOARD,
    M_SHOWWEBPAGE,
    M_SHOWEXITDIALOG,
    M_SHUTDOWNAPP,
    M_HASAPPLOCALVALUE,
    M_GETAPPLOCALVALUE,
    M_SETAPPLOCALVALUE,
    M_HIDESPLASH,
    M_HIDERELOADER,
    M_GETPERFORMANCELEVEL,
    M_GETRESOLUTIONSCALE,
    M_SETMAXPERFORMANCELEVEL,
    M_GETMAINAPKEXPANSIONNAME,
    M_GETPATCHAPKEXPANSIONNAME,
    M_OPENSETTINGSMENU,
    M_GETDEPTHSIZE,
    M_ISEXPANSIONINAPK,
    M_ISLARGESCREENDEVICE,
    M_GETAPKFILEPATH,
    M_GETAPPCOMMANDLINE,
    M_GETDEVICEMODEL,
    M_GETSDKVERSION,
    M_ANDROIDIAPISAMAZONBUILD,
    M_ANDROIDIAPISBILLINGSUPPORTED,
    M_ANDROIDIAPREQUESTAVAILABLEPRODUCTS,
    M_ANDROIDIAPREQUESTPURCHASE,
    M_ANDROIDIAPRESTOREPURCHASES,
    M_APSALARINIT,
    M_APSALARSTARTSESSION,
    M_APSALARENDSESSION,
    M_APSALAREVENT,
    M_APSALAREVENTPARAM,
    M_APSALAREVENTPARAMARRAY,
    M_APSALARLOGENGINEDATA,
    M_FLURRYSTARTSESSION,
    M_FLURRYENDSESSION,
    M_FLURRYSETCONTINUESESSIONMILLIS,
    M_FLURRYLOGEVENT,
    M_FLURRYLOGEVENTTIMED,
    M_FLURRYLOGSTRINGEVENTPARAM,
    M_FLURRYENDSTRINGEVENT,
    M_FLURRYONERROR,
    M_FLURRYONPAGEVIEW,
    M_FLURRYSETLOGENABLED,
    M_FLURRYSETUSEHTTPS,
    M_FLURRYSETUSERID,
    M_FLURRYSETAGE,
    M_FLURRYSETGENDER,
    M_SWRVEINIT,
    M_SWRVESTARTSESSION,
    M_SWRVEENDSESSION,
    M_SWRVEONEVENT,
    M_SWRVEONPURCHASE,
    M_SWRVEONIAP,
    M_SWRVEONCURRENCYGIVEN,
    M_SWRVEONUSERUPDATE,
    M_SWRVEONFLUSHEVENTS,
    M_ONSAVEEVENT,
    M_SWRVETALKEVENT,
    M_SWRVEGETUSERRESOURCES,
    M_SWRVEGETUSERRESOURCESDIFF,
    M_SWRVECLOSE,
    M_HASOFFERINIT,
    M_HASOFFERSTARTSESSION,
    M_HASOFFERENDSESSION,
    M_HASOFFERLOGSTRINGEVENT,
    M_HASOFFERPURCHASE,
    M_CHARTBOOSTINIT,
    M_GROWMOBILEPURCHASE,
    M_FIKSUPURCHASE,
    M_WBNETINIT,
    M_WBNETAUTOLOGIN,
    M_WBNETSPAWNLOGINDIALOG,
    M_WBNETSPAWNCREATEACCOUNTDIALOG,
    M_WBNETLOGGEDIN,
    M_WBNETLOGOUT,
    M_GETWBID,
    M_GETUSERID,
    M_GETPASSWORD,
    M_GETREFRESHTOKEN,
    M_GETSHAREDPROFILES,
    M_GETLOGINTIMELOCAL,
    M_GETMEMBERS,
    M_UPDATEMEMEBERHASHTABLE,
    M_WBNETAUTOLOGINWITHREFRESHTOKEN,
    M_SHOWMOREWBAPPS,
    M_FACEBOOKGETDEEPLINK,
    M_FACEBOOKOPENSESSION,
    M_FACEBOOKISAUTHORIZED,
    M_FACEBOOKCLOSESESSION,
    M_FACEBOOKGRAPHREQUEST,
    M_FACEBOOKSENDREQUEST,
    M_FACEBOOKSHOWFEEDDIALOG,
    M_FACEBOOKRETRIEVEFRIENDSLIST,
    M_CLOUDSTORAGEINIT,
    M_CLOUDSTORAGEPOSTSTARTUPINIT,
    M_CLOUDSTORAGEENABLE,
    M_CLOUDSTORAGEISENABLED,
    M_CLOUDSTORAGEQUERY,
    M_CLOUDSTORAGEREAD,
    M_CLOUDSTORAGELOAD,
    M_CLOUDSTORAGESAVE,
    M_CLOUDSTORAGEWRITE,
    M_GOOGLEPS_LOGIN,
    M_GOOGLEPS_LOGOUT,
    M_GOOGLEPS_USERAUTHENTICATED,
    M_GOOGLEPS_GETLOCALUSERNAME,
    M_GOOGLEPS_SUBMITSCORE,
    M_GOOGLEPS_LOADSCORES,
    M_GOOGLEPS_LOADSCORESNEXTPAGE,
    M_GOOGLEPS_HASNEXTPAGE,
    M_GOOGLEPS_HASPREVIOUSPAGE,
    M_GOOGLEPS_GETNUMLOADEDSCORES,
    M_GOOGLEPS_GETENTRYDISPLAYNAME,
    M_GOOGLEPS_GETENTRYPLAYERID,
    M_GOOGLEPS_GETENTRYRANK,
    M_GOOGLEPS_GETENTRYSCORE,
    M_GOOGLEPS_GETENTRYSCORETAG,
    M_GOOGLEPS_ISLOADERBOARDVALID,
    M_GOOGLEPS_ISLOADERBOARDLOADING,
    M_GOOGLEPS_GETLOCALPLAYERSCORE,
    M_GOOGLEPS_GETLOCALPLAYERTAG,
    M_GOOGLEPS_GETLOCALPLAYERRANK,
    M_LOADACHIEVEMENTS,
    M_UNLOCKACHIEVEMENT,
    M_GETACHIEVEMENTDETAILS,
    M_SHOWACHIEVEMENTSUI,
};

NameToMethodID nameToMethodId[] = {
    { M_FD_INIT, "java/io/FileDescriptor/<init>", METHOD_TYPE_OBJECT },
    // misspelled in the engine, java never had them either
    { M_SETWBREALM, "JaveCallback_setWBRealm", METHOD_TYPE_VOID },
    { M_SETWBTITLE, "JaveCallback_setWBTitle", METHOD_TYPE_VOID },
    { M_SETWBPLATFORMID, "JaveCallback_setWBPlatformId", METHOD_TYPE_VOID },
    { M_SETWBBLOWFISHKEY, "JaveCallback_setWBBlowfishKey", METHOD_TYPE_VOID },
    { M_PLAYSONG, "JavaCallback_PlaySong", METHOD_TYPE_VOID },
    { M_STOPSONG, "JavaCallback_StopSong", METHOD_TYPE_VOID },
    { M_LOADSOUNDFILE, "JavaCallback_LoadSoundFile", METHOD_TYPE_INT },
    { M_UNLOADSOUNDID, "JavaCallback_UnloadSoundID", METHOD_TYPE_VOID },
    { M_PLAYSOUND, "JavaCallback_PlaySound", METHOD_TYPE_INT },
    { M_STOPSOUND, "JavaCallback_StopSound", METHOD_TYPE_VOID },
    { M_SETVOLUME, "JavaCallback_SetVolume", METHOD_TYPE_VOID },
    { M_SETSONGVOLUME, "JavaCallback_SetSongVolume", METHOD_TYPE_VOID },
    { M_UPDATESONGPLAYER, "JavaCallback_UpdateSongPlayer", METHOD_TYPE_VOID },
    { M_RESTOREMEDIA, "JavaCallback_RestoreMedia", METHOD_TYPE_VOID },
    { M_SWAPBUFFERS, "JavaCallback_swapBuffers", METHOD_TYPE_BOOLEAN },
    { M_MAKECURRENT, "JavaCallback_makeCurrent", METHOD_TYPE_BOOLEAN },
    { M_UNMAKECURRENT, "JavaCallback_unMakeCurrent", METHOD_TYPE_BOOLEAN },
    { M_INITEGL, "JavaCallback_initEGL", METHOD_TYPE_BOOLEAN },
    { M_SETFIXEDSIZESCALE, "JavaCallback_SetFixedSizeScale", METHOD_TYPE_VOID },
    { M_STARTVIDEO, "JavaCallback_StartVideo", METHOD_TYPE_VOID },
    { M_STOPVIDEO, "JavaCallback_StopVideo", METHOD_TYPE_VOID },
    { M_ISVIDEOPLAYING, "JavaCallback_IsVideoPlaying", METHOD_TYPE_BOOLEAN },
    { M_VIDEOADDTEXTOVERLAY, "JavaCallback_VideoAddTextOverlay", METHOD_TYPE_VOID },
    { M_SHOWKEYBOARD, "JavaCallback_ShowKeyBoard", METHOD_TYPE_VOID },
    { M_HIDEKEYBOARD, "JavaCallback_HideKeyBoard", METHOD_TYPE_VOID },
    { M_SHOWWEBPAGE, "JavaCallback_ShowWebPage", METHOD_TYPE_VOID },
    { M_SHOWEXITDIALOG, "JavaCallback_ShowExitDialog", METHOD_TYPE_VOID },
    { M_SHUTDOWNAPP, "JavaCallback_ShutDownApp", METHOD_TYPE_VOID },
    { M_HASAPPLOCALVALUE, "JavaCallback_hasAppLocalValue", METHOD_TYPE_BOOLEAN },
    { M_GETAPPLOCALVALUE, "JavaCallback_getAppLocalValue", METHOD_TYPE_OBJECT },
    { M_SETAPPLOCALVALUE, "JavaCallback_setAppLocalValue", METHOD_TYPE_VOID },
    { M_HIDESPLASH, "JavaCallback_HideSplash", METHOD_TYPE_VOID },
    { M_HIDERELOADER, "JavaCallback_HideReloader", METHOD_TYPE_VOID },
    { M_GETPERFORMANCELEVEL, "JavaCallback_GetPerformanceLevel", METHOD_TYPE_INT },
    { M_GETRESOLUTIONSCALE, "JavaCallback_GetResolutionScale", METHOD_TYPE_FLOAT },
    { M_SETMAXPERFORMANCELEVEL, "JavaCallback_SetMaxPerformanceLevel", METHOD_TYPE_VOID },
    { M_GETMAINAPKEXPANSIONNAME, "JavaCallback_GetMainAPKExpansionName", METHOD_TYPE_OBJECT },
    { M_GETPATCHAPKEXPANSIONNAME, "JavaCallback_GetPatchAPKExpansionName", METHOD_TYPE_OBJECT },
    { M_OPENSETTINGSMENU, "JavaCallback_OpenSettingsMenu", METHOD_TYPE_VOID },
    { M_GETDEPTHSIZE, "JavaCallback_GetDepthSize", METHOD_TYPE_INT },
    { M_ISEXPANSIONINAPK, "JavaCallback_IsExpansionInAPK", METHOD_TYPE_BOOLEAN },
    { M_ISLARGESCREENDEVICE, "JavaCallback_IsLargeScreenDevice", METHOD_TYPE_BOOLEAN },
    { M_GETAPKFILEPATH, "JavaCallback_GetAPKFilePath", METHOD_TYPE_OBJECT },
    { M_GETAPPCOMMANDLINE, "JavaCallback_GetAppCommandLine", METHOD_TYPE_OBJECT },
    { M_GETDEVICEMODEL, "JavaCallback_GetDeviceModel", METHOD_TYPE_OBJECT },
    { M_GETSDKVERSION, "JavaCallback_GetSDKVersion", METHOD_TYPE_INT },
    { M_ANDROIDIAPISAMAZONBUILD, "JavaCallback_AndroidIAPIsAmazonBuild", METHOD_TYPE_BOOLEAN },
    { M_ANDROIDIAPISBILLINGSUPPORTED, "JavaCallback_AndroidIAPIsBillingSupported", METHOD_TYPE_BOOLEAN },
    { M_ANDROIDIAPREQUESTAVAILABLEPRODUCTS, "JavaCallback_AndroidIAPRequestAvailableProducts", METHOD_TYPE_VOID },
    { M_ANDROIDIAPREQUESTPURCHASE, "JavaCallback_AndroidIAPRequestPurchase", METHOD_TYPE_VOID },
    { M_ANDROIDIAPRESTOREPURCHASES, "JavaCallback_AndroidIAPRestorePurchases", METHOD_TYPE_VOID },
    { M_APSALARINIT, "JavaCallback_ApsalarInit", METHOD_TYPE_VOID },
    { M_APSALARSTARTSESSION, "JavaCallback_ApsalarStartSession", METHOD_TYPE_VOID },
    { M_APSALARENDSESSION, "JavaCallback_ApsalarEndSession", METHOD_TYPE_VOID },
    { M_APSALAREVENT, "JavaCallback_ApsalarEvent", METHOD_TYPE_VOID },
    { M_APSALAREVENTPARAM, "JavaCallback_ApsalarEventParam", METHOD_TYPE_VOID },
    { M_APSALAREVENTPARAMARRAY, "JavaCallback_ApsalarEventParamArray", METHOD_TYPE_VOID },
    { M_APSALARLOGENGINEDATA, "JavaCallback_ApsalarLogEngineData", METHOD_TYPE_VOID },
    { M_FLURRYSTARTSESSION, "JavaCallback_FlurryStartSession", METHOD_TYPE_VOID },
    { M_FLURRYENDSESSION, "JavaCallback_FlurryEndSession", METHOD_TYPE_VOID },
    { M_FLURRYSETCONTINUESESSIONMILLIS, "JavaCallback_FlurrySetContinueSessionMillis", METHOD_TYPE_VOID },
    { M_FLURRYLOGEVENT, "JavaCallback_FlurryLogEvent", METHOD_TYPE_VOID },
    { M_FLURRYLOGEVENTTIMED, "JavaCallback_FlurryLogEventTimed", METHOD_TYPE_VOID },
    { M_FLURRYLOGSTRINGEVENTPARAM, "JavaCallback_FlurryLogStringEventParam", METHOD_TYPE_VOID },
    { M_FLURRYENDSTRINGEVENT, "JavaCallback_FlurryEndStringEvent", METHOD_TYPE_VOID },
    { M_FLURRYONERROR, "JavaCallback_FlurryOnError", METHOD_TYPE_VOID },
    { M_FLURRYONPAGEVIEW, "JavaCallback_FlurryOnPageView", METHOD_TYPE_VOID },
    { M_FLURRYSETLOGENABLED, "JavaCallback_FlurrySetLogEnabled", METHOD_TYPE_VOID },
    { M_FLURRYSETUSEHTTPS, "JavaCallback_FlurrySetUseHttps", METHOD_TYPE_VOID },
    { M_FLURRYSETUSERID, "JavaCallback_FlurrySetUserID", METHOD_TYPE_VOID },
    { M_FLURRYSETAGE, "JavaCallback_FlurrySetAge", METHOD_TYPE_VOID },
    { M_FLURRYSETGENDER, "JavaCallback_FlurrySetGender", METHOD_TYPE_VOID },
    { M_SWRVEINIT, "JavaCallback_SwrveInit", METHOD_TYPE_VOID },
    { M_SWRVESTARTSESSION, "JavaCallback_SwrveStartSession", METHOD_TYPE_VOID },
    { M_SWRVEENDSESSION, "JavaCallback_SwrveEndSession", METHOD_TYPE_VOID },
    { M_SWRVEONEVENT, "JavaCallback_SwrveOnEvent", METHOD_TYPE_VOID },
    { M_SWRVEONPURCHASE, "JavaCallback_SwrveOnPurchase", METHOD_TYPE_VOID },
    { M_SWRVEONIAP, "JavaCallback_SwrveOnIAP", METHOD_TYPE_VOID },
    { M_SWRVEONCURRENCYGIVEN, "JavaCallback_SwrveOnCurrencyGiven", METHOD_TYPE_VOID },
    { M_SWRVEONUSERUPDATE, "JavaCallback_SwrveOnUserUpdate", METHOD_TYPE_VOID },
    { M_SWRVEONFLUSHEVENTS, "JavaCallback_SwrveOnFlushEvents", METHOD_TYPE_VOID },
    { M_ONSAVEEVENT, "JavaCallback_OnSaveEvent", METHOD_TYPE_VOID },
    { M_SWRVETALKEVENT, "JavaCallback_SwrveTalkEvent", METHOD_TYPE_VOID },
    { M_SWRVEGETUSERRESOURCES, "JavaCallback_SwrveGetUserResources", METHOD_TYPE_VOID },
    { M_SWRVEGETUSERRESOURCESDIFF, "JavaCallback_SwrveGetUserResourcesDiff", METHOD_TYPE_VOID },
    { M_SWRVECLOSE, "JavaCallback_SwrveClose", METHOD_TYPE_VOID },
    { M_HASOFFERINIT, "JavaCallback_HasOfferInit", METHOD_TYPE_VOID },
    { M_HASOFFERSTARTSESSION, "JavaCallback_HasOfferStartSession", METHOD_TYPE_VOID },
    { M_HASOFFERENDSESSION, "JavaCallback_HasOfferEndSession", METHOD_TYPE_VOID },
    { M_HASOFFERLOGSTRINGEVENT, "JavaCallback_HasOfferLogStringEvent", METHOD_TYPE_VOID },
    { M_HASOFFERPURCHASE, "JavaCallback_HasOfferPurchase", METHOD_TYPE_VOID },
    { M_CHARTBOOSTINIT, "JavaCallback_ChartBoostInit", METHOD_TYPE_VOID },
    { M_GROWMOBILEPURCHASE, "JavaCallback_GrowMobilePurchase", METHOD_TYPE_VOID },
    { M_FIKSUPURCHASE, "JavaCallback_FiksuPurchase", METHOD_TYPE_VOID },
    { M_WBNETINIT, "JavaCallback_WBNetInit", METHOD_TYPE_VOID },
    { M_WBNETAUTOLOGIN, "JavaCallback_WBNetAutoLogin", METHOD_TYPE_BOOLEAN },
    { M_WBNETSPAWNLOGINDIALOG, "JavaCallback_WBNetSpawnLoginDialog", METHOD_TYPE_BOOLEAN },
    { M_WBNETSPAWNCREATEACCOUNTDIALOG, "JavaCallback_WBNetSpawnCreateAccountDialog", METHOD_TYPE_BOOLEAN },
    { M_WBNETLOGGEDIN, "JavaCallback_WBNetLoggedIn", METHOD_TYPE_BOOLEAN },
    { M_WBNETLOGOUT, "JavaCallback_WBNetLogOut", METHOD_TYPE_BOOLEAN },
    { M_GETWBID, "JavaCallback_GetWBID", METHOD_TYPE_OBJECT },
    { M_GETUSERID, "JavaCallback_GetUserID", METHOD_TYPE_OBJECT },
    { M_GETPASSWORD, "JavaCallback_GetPassword", METHOD_TYPE_OBJECT },
    { M_GETREFRESHTOKEN, "JavaCallback_GetRefreshToken", METHOD_TYPE_OBJECT },
    { M_GETSHAREDPROFILES, "JavaCallback_GetSharedProfiles", METHOD_TYPE_OBJECT },
    { M_GETLOGINTIMELOCAL, "JavaCallback_GetLoginTimeLocal", METHOD_TYPE_INT },
    { M_GETMEMBERS, "JavaCallback_GetMembers", METHOD_TYPE_BOOLEAN },
    { M_UPDATEMEMEBERHASHTABLE, "JavaCallback_UpdateMemeberHashTable", METHOD_TYPE_BOOLEAN },
    { M_WBNETAUTOLOGINWITHREFRESHTOKEN, "JavaCallback_WBNetAutoLoginWithRefreshToken", METHOD_TYPE_BOOLEAN },
    { M_SHOWMOREWBAPPS, "JavaCallback_ShowMoreWbApps", METHOD_TYPE_VOID },
    { M_FACEBOOKGETDEEPLINK, "JavaCallback_FacebookGetDeepLink", METHOD_TYPE_OBJECT },
    { M_FACEBOOKOPENSESSION, "JavaCallback_FacebookOpenSession", METHOD_TYPE_VOID },
    { M_FACEBOOKISAUTHORIZED, "JavaCallback_FacebookIsAuthorized", METHOD_TYPE_BOOLEAN },
    { M_FACEBOOKCLOSESESSION, "JavaCallback_FacebookCloseSession", METHOD_TYPE_VOID },
    { M_FACEBOOKGRAPHREQUEST, "JavaCallback_FacebookGraphRequest", METHOD_TYPE_VOID },
    { M_FACEBOOKSENDREQUEST, "JavaCallback_FacebookSendRequest", METHOD_TYPE_VOID },
    { M_FACEBOOKSHOWFEEDDIALOG, "JavaCallback_FacebookShowFeedDialog", METHOD_TYPE_VOID },
    { M_FACEBOOKRETRIEVEFRIENDSLIST, "JavaCallback_FacebookRetrieveFriendsList", METHOD_TYPE_VOID },
    { M_CLOUDSTORAGEINIT, "JavaCallback_CloudStorageInit", METHOD_TYPE_VOID },
    { M_CLOUDSTORAGEPOSTSTARTUPINIT, "JavaCallback_CloudStoragePostStartupInit", METHOD_TYPE_VOID },
    { M_CLOUDSTORAGEENABLE, "JavaCallback_CloudStorageEnable", METHOD_TYPE_VOID },
    { M_CLOUDSTORAGEISENABLED, "JavaCallback_CloudStorageIsEnabled", METHOD_TYPE_BOOLEAN },
    { M_CLOUDSTORAGEQUERY, "JavaCallback_CloudStorageQuery", METHOD_TYPE_BOOLEAN },
    { M_CLOUDSTORAGEREAD, "JavaCallback_CloudStorageRead", METHOD_TYPE_BOOLEAN },
    { M_CLOUDSTORAGELOAD, "JavaCallback_CloudStorageLoad", METHOD_TYPE_OBJECT },
    { M_CLOUDSTORAGESAVE, "JavaCallback_CloudStorageSave", METHOD_TYPE_BOOLEAN },
    { M_CLOUDSTORAGEWRITE, "JavaCallback_CloudStorageWrite", METHOD_TYPE_BOOLEAN },
    { M_GOOGLEPS_LOGIN, "JavaCallback_GooglePS_login", METHOD_TYPE_VOID },
    { M_GOOGLEPS_LOGOUT, "JavaCallback_GooglePS_logout", METHOD_TYPE_VOID },
    { M_GOOGLEPS_USERAUTHENTICATED, "JavaCallback_GooglePS_UserAuthenticated", METHOD_TYPE_BOOLEAN },
    { M_GOOGLEPS_GETLOCALUSERNAME, "JavaCallback_GooglePS_GetLocalUserName", METHOD_TYPE_OBJECT },
    { M_GOOGLEPS_SUBMITSCORE, "JavaCallback_GooglePS_submitScore", METHOD_TYPE_VOID },
    { M_GOOGLEPS_LOADSCORES, "JavaCallback_GooglePS_loadScores", METHOD_TYPE_VOID },
    { M_GOOGLEPS_LOADSCORESNEXTPAGE, "JavaCallback_GooglePS_loadScoresNextPage", METHOD_TYPE_VOID },
    { M_GOOGLEPS_HASNEXTPAGE, "JavaCallback_GooglePS_hasNextPage", METHOD_TYPE_BOOLEAN },
    { M_GOOGLEPS_HASPREVIOUSPAGE, "JavaCallback_GooglePS_hasPreviousPage", METHOD_TYPE_BOOLEAN },
    { M_GOOGLEPS_GETNUMLOADEDSCORES, "JavaCallback_GooglePS_GetNumLoadedScores", METHOD_TYPE_INT },
    { M_GOOGLEPS_GETENTRYDISPLAYNAME, "JavaCallback_GooglePS_GetEntryDisplayName", METHOD_TYPE_OBJECT },
    { M_GOOGLEPS_GETENTRYPLAYERID, "JavaCallback_GooglePS_GetEntryPlayerID", METHOD_TYPE_OBJECT },
    { M_GOOGLEPS_GETENTRYRANK, "JavaCallback_GooglePS_GetEntryRank", METHOD_TYPE_LONG },
    { M_GOOGLEPS_GETENTRYSCORE, "JavaCallback_GooglePS_GetEntryScore", METHOD_TYPE_LONG },
    { M_GOOGLEPS_GETENTRYSCORETAG, "JavaCallback_GooglePS_GetEntryScoreTag", METHOD_TYPE_OBJECT },
    { M_GOOGLEPS_ISLOADERBOARDVALID, "JavaCallback_GooglePS_IsLoaderboardValid", METHOD_TYPE_BOOLEAN },
    { M_GOOGLEPS_ISLOADERBOARDLOADING, "JavaCallback_GooglePS_IsLoaderboardLoading", METHOD_TYPE_BOOLEAN },
    { M_GOOGLEPS_GETLOCALPLAYERSCORE, "JavaCallback_GooglePS_GetLocalPlayerScore", METHOD_TYPE_LONG },
    { M_GOOGLEPS_GETLOCALPLAYERTAG, "JavaCallback_GooglePS_GetLocalPlayerTag", METHOD_TYPE_OBJECT },
    { M_GOOGLEPS_GETLOCALPLAYERRANK, "JavaCallback_GooglePS_GetLocalPlayerRank", METHOD_TYPE_LONG },
    { M_LOADACHIEVEMENTS, "JavaCallback_LoadAchievements", METHOD_TYPE_VOID },
    { M_UNLOCKACHIEVEMENT, "JavaCallback_UnlockAchievement", METHOD_TYPE_VOID },
    { M_GETACHIEVEMENTDETAILS, "JavaCallback_GetAchievementDetails", METHOD_TYPE_OBJECT },
    { M_SHOWACHIEVEMENTSUI, "JavaCallback_ShowAchievementsUI", METHOD_TYPE_VOID },
};

MethodsVoid methodsVoid[] = {
    { M_SETWBREALM, nop },
    { M_SETWBTITLE, nop },
    { M_SETWBPLATFORMID, nop },
    { M_SETWBBLOWFISHKEY, nop },
    { M_PLAYSONG, PlaySong },
    { M_STOPSONG, StopSong },
    { M_UNLOADSOUNDID, UnloadSoundID },
    { M_STOPSOUND, StopSound },
    { M_SETVOLUME, SetVolume },
    { M_SETSONGVOLUME, SetSongVolume },
    { M_UPDATESONGPLAYER, UpdateSongPlayer },
    { M_RESTOREMEDIA, nop },
    { M_SETFIXEDSIZESCALE, nop },
    { M_STARTVIDEO, StartVideo },
    { M_STOPVIDEO, StopVideo },
    { M_VIDEOADDTEXTOVERLAY, nop },
    { M_SHOWKEYBOARD, ShowKeyBoard },
    { M_HIDEKEYBOARD, nop },
    { M_SHOWWEBPAGE, ShowWebPage },
    { M_SHOWEXITDIALOG, nop },
    { M_SHUTDOWNAPP, ShutDownApp },
    { M_SETAPPLOCALVALUE, SetAppLocalValue },
    { M_HIDESPLASH, HideSplash },
    { M_HIDERELOADER, nop },
    { M_SETMAXPERFORMANCELEVEL, nop },
    { M_OPENSETTINGSMENU, nop },
    { M_ANDROIDIAPREQUESTAVAILABLEPRODUCTS, nop },
    { M_ANDROIDIAPREQUESTPURCHASE, nop },
    { M_ANDROIDIAPRESTOREPURCHASES, nop },
    { M_APSALARINIT, nop },
    { M_APSALARSTARTSESSION, nop },
    { M_APSALARENDSESSION, nop },
    { M_APSALAREVENT, nop },
    { M_APSALAREVENTPARAM, nop },
    { M_APSALAREVENTPARAMARRAY, nop },
    { M_APSALARLOGENGINEDATA, nop },
    { M_FLURRYSTARTSESSION, nop },
    { M_FLURRYENDSESSION, nop },
    { M_FLURRYSETCONTINUESESSIONMILLIS, nop },
    { M_FLURRYLOGEVENT, nop },
    { M_FLURRYLOGEVENTTIMED, nop },
    { M_FLURRYLOGSTRINGEVENTPARAM, nop },
    { M_FLURRYENDSTRINGEVENT, nop },
    { M_FLURRYONERROR, nop },
    { M_FLURRYONPAGEVIEW, nop },
    { M_FLURRYSETLOGENABLED, nop },
    { M_FLURRYSETUSEHTTPS, nop },
    { M_FLURRYSETUSERID, nop },
    { M_FLURRYSETAGE, nop },
    { M_FLURRYSETGENDER, nop },
    { M_SWRVEINIT, nop },
    { M_SWRVESTARTSESSION, nop },
    { M_SWRVEENDSESSION, nop },
    { M_SWRVEONEVENT, nop },
    { M_SWRVEONPURCHASE, nop },
    { M_SWRVEONIAP, nop },
    { M_SWRVEONCURRENCYGIVEN, nop },
    { M_SWRVEONUSERUPDATE, nop },
    { M_SWRVEONFLUSHEVENTS, nop },
    { M_ONSAVEEVENT, nop },
    { M_SWRVETALKEVENT, nop },
    { M_SWRVEGETUSERRESOURCES, nop },
    { M_SWRVEGETUSERRESOURCESDIFF, nop },
    { M_SWRVECLOSE, nop },
    { M_HASOFFERINIT, nop },
    { M_HASOFFERSTARTSESSION, nop },
    { M_HASOFFERENDSESSION, nop },
    { M_HASOFFERLOGSTRINGEVENT, nop },
    { M_HASOFFERPURCHASE, nop },
    { M_CHARTBOOSTINIT, nop },
    { M_GROWMOBILEPURCHASE, nop },
    { M_FIKSUPURCHASE, nop },
    { M_WBNETINIT, nop },
    { M_SHOWMOREWBAPPS, nop },
    { M_FACEBOOKOPENSESSION, nop },
    { M_FACEBOOKCLOSESESSION, nop },
    { M_FACEBOOKGRAPHREQUEST, nop },
    { M_FACEBOOKSENDREQUEST, nop },
    { M_FACEBOOKSHOWFEEDDIALOG, nop },
    { M_FACEBOOKRETRIEVEFRIENDSLIST, nop },
    { M_CLOUDSTORAGEINIT, nop },
    { M_CLOUDSTORAGEPOSTSTARTUPINIT, nop },
    { M_CLOUDSTORAGEENABLE, nop },
    { M_GOOGLEPS_LOGIN, nop },
    { M_GOOGLEPS_LOGOUT, nop },
    { M_GOOGLEPS_SUBMITSCORE, nop },
    { M_GOOGLEPS_LOADSCORES, nop },
    { M_GOOGLEPS_LOADSCORESNEXTPAGE, nop },
    { M_LOADACHIEVEMENTS, nop },
    { M_UNLOCKACHIEVEMENT, nop },
    { M_SHOWACHIEVEMENTSUI, nop },
};

MethodsObject methodsObject[] = {
    { M_FD_INIT, NewFileDescriptor },
    { M_GETAPPLOCALVALUE, GetAppLocalValue },
    { M_GETMAINAPKEXPANSIONNAME, GetMainExpansion },
    { M_GETPATCHAPKEXPANSIONNAME, ret_empty },
    { M_GETAPKFILEPATH, ret_empty },
    { M_GETAPPCOMMANDLINE, GetAppCommandLine },
    { M_GETDEVICEMODEL, GetDeviceModel },
    { M_GETWBID, ret_empty },
    { M_GETUSERID, ret_empty },
    { M_GETPASSWORD, ret_empty },
    { M_GETREFRESHTOKEN, ret_empty },
    { M_GETSHAREDPROFILES, ret_empty_array },
    { M_FACEBOOKGETDEEPLINK, ret_empty },
    { M_CLOUDSTORAGELOAD, ret_null },
    { M_GOOGLEPS_GETLOCALUSERNAME, ret_empty },
    { M_GOOGLEPS_GETENTRYDISPLAYNAME, ret_empty },
    { M_GOOGLEPS_GETENTRYPLAYERID, ret_empty },
    { M_GOOGLEPS_GETENTRYSCORETAG, ret_empty },
    { M_GOOGLEPS_GETLOCALPLAYERTAG, ret_empty },
    { M_GETACHIEVEMENTDETAILS, ret_empty_array },
};

MethodsBoolean methodsBoolean[] = {
    { M_SWAPBUFFERS, Swap },
    { M_MAKECURRENT, MakeCurrent },
    { M_UNMAKECURRENT, UnMakeCurrent },
    { M_INITEGL, InitEGL },
    { M_ISVIDEOPLAYING, IsVideoPlaying },
    { M_HASAPPLOCALVALUE, HasAppLocalValue },
    { M_ISEXPANSIONINAPK, ret_false },
    { M_ISLARGESCREENDEVICE, ret_false },
    { M_ANDROIDIAPISAMAZONBUILD, ret_false },
    { M_ANDROIDIAPISBILLINGSUPPORTED, ret_false },
    { M_WBNETAUTOLOGIN, ret_false },
    { M_WBNETSPAWNLOGINDIALOG, ret_false },
    { M_WBNETSPAWNCREATEACCOUNTDIALOG, ret_false },
    { M_WBNETLOGGEDIN, ret_false },
    { M_WBNETLOGOUT, ret_false },
    { M_GETMEMBERS, ret_false },
    { M_UPDATEMEMEBERHASHTABLE, ret_false },
    { M_WBNETAUTOLOGINWITHREFRESHTOKEN, ret_false },
    { M_FACEBOOKISAUTHORIZED, ret_false },
    { M_CLOUDSTORAGEISENABLED, ret_false },
    { M_CLOUDSTORAGEQUERY, ret_false },
    { M_CLOUDSTORAGEREAD, ret_false },
    { M_CLOUDSTORAGESAVE, ret_false },
    { M_CLOUDSTORAGEWRITE, ret_false },
    { M_GOOGLEPS_USERAUTHENTICATED, ret_false },
    { M_GOOGLEPS_HASNEXTPAGE, ret_false },
    { M_GOOGLEPS_HASPREVIOUSPAGE, ret_false },
    { M_GOOGLEPS_ISLOADERBOARDVALID, ret_false },
    { M_GOOGLEPS_ISLOADERBOARDLOADING, ret_false },
};

MethodsInt methodsInt[] = {
    { M_LOADSOUNDFILE, LoadSoundFile },
    { M_PLAYSOUND, PlaySound },
    { M_GETPERFORMANCELEVEL, GetPerformanceLevel },
    { M_GETDEPTHSIZE, GetDepthSize },
    { M_GETSDKVERSION, GetSDKVersion },
    { M_GETLOGINTIMELOCAL, ret_int0 },
    { M_GOOGLEPS_GETNUMLOADEDSCORES, ret_int0 },
};

MethodsLong methodsLong[] = {
    { M_GOOGLEPS_GETENTRYRANK, ret_long0 },
    { M_GOOGLEPS_GETENTRYSCORE, ret_long0 },
    { M_GOOGLEPS_GETLOCALPLAYERSCORE, ret_long0 },
    { M_GOOGLEPS_GETLOCALPLAYERRANK, ret_long0 },
};

MethodsFloat methodsFloat[] = {
    { M_GETRESOLUTIONSCALE, GetResolutionScale },
};

MethodsByte methodsByte[] = {};
MethodsChar methodsChar[] = {};
MethodsDouble methodsDouble[] = {};
MethodsShort methodsShort[] = {};

FieldsByte fieldsByte[] = {};
FieldsChar fieldsChar[] = {};
FieldsDouble fieldsDouble[] = {};
FieldsFloat fieldsFloat[] = {};
FieldsObject fieldsObject[] = {};
FieldsLong fieldsLong[] = {};
FieldsShort fieldsShort[] = {};

__FALSOJNI_IMPL_CONTAINER_SIZES
