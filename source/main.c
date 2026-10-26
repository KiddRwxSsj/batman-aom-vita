#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <psp2/apputil.h>
#include <psp2/system_param.h>
#include <psp2/power.h>
#include <psp2/kernel/threadmgr.h>

#include "audio.h"
#include "bionic.h"
#include "gfx.h"
#include "ime.h"
#include "input.h"
#include "java.h"
#include "loader.h"
#include "movie.h"
#include "opensl.h"
#include "util.h"

// vitaGL takes free memory minus its thresholds, gfx.c leaves 56MB of user ram for big blocks
int _newlib_heap_size_user = 200 * 1024 * 1024;

static int thiz_storage;
jobject java_thiz = (jobject)&thiz_storage;
volatile int splash_hidden;

jboolean (*NativeCallback_InputEvent)(JNIEnv *, jobject, int, int, int, int, int64_t);
jboolean (*NativeCallback_JoystickButtonEvent)(JNIEnv *, jobject, int, int, int, int64_t);
jboolean (*NativeCallback_JoystickAxisEvent)(JNIEnv *, jobject, int, int, int, float, int64_t);
void (*NativeCallback_MovieFinished)(JNIEnv *, jobject);
void (*NativeCallback_KeyboardFinished)(JNIEnv *, jobject, jstring);
void (*NativeCallback_HandleBackButtonPressed)(void);

static void system_language(const char **lang, const char **country) {
    static const struct { const char *lang, *country; } langs[] = {
        { "ja", "JP" }, { "en", "US" }, { "fr", "FR" }, { "es", "ES" }, { "de", "DE" }, { "it", "IT" },
        { "nl", "NL" }, { "pt", "PT" }, { "ru", "RU" }, { "ko", "KR" }, { "zh", "TW" }, { "zh", "CN" },
        { "fi", "FI" }, { "sv", "SE" }, { "da", "DK" }, { "no", "NO" }, { "pl", "PL" }, { "pt", "BR" },
        { "en", "GB" }, { "tr", "TR" },
    };
    int l = SCE_SYSTEM_PARAM_LANG_ENGLISH_US;
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &l);
    if (l < 0 || l >= (int)(sizeof(langs) / sizeof(langs[0])))
        l = SCE_SYSTEM_PARAM_LANG_ENGLISH_US;
    *lang = langs[l].lang;
    *country = langs[l].country;
}

static jstring jstring_new(const char *s) {
    return jni->NewStringUTF(&jni, s);
}

int main(void) {
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    SceAppUtilInitParam init = {0};
    SceAppUtilBootParam boot = {0};
    sceAppUtilInit(&init, &boot);

    bionic_init();
    mkpath(DATA_PATH);
    log_open();
    LOG("starting");

    if (!file_exists(OBB_PATH))
        fatal_error("Missing %s\nCopy it from Android/obb/com.wb.goog.batman.brawler2013 on the phone.", OBB_PATH);

    loader_init();
    jni_init();
    audio_init();
    opensl_init();
    input_init();

    int (*JNI_OnLoad)(JavaVM *, void *) = (void *)ue3_sym("JNI_OnLoad");
    jboolean (*SystemStats)(JNIEnv *, jobject, int64_t, float) =
        (void *)ue3_sym("_Z26NativeCallback_SystemStatsP7_JNIEnvP8_jobjectxf");
    void (*KeyPadChange)(JNIEnv *, jobject, jboolean) =
        (void *)ue3_sym("_Z27NativeCallback_KeyPadChangeP7_JNIEnvP8_jobjecth");
    jboolean (*InitEGLCallback)(JNIEnv *, jobject) =
        (void *)ue3_sym("_Z30NativeCallback_InitEGLCallbackP7_JNIEnvP8_jobject");
    void (*PostInitUpdate)(JNIEnv *, jobject, int, int) =
        (void *)ue3_sym("_Z29NativeCallback_PostInitUpdateP7_JNIEnvP8_jobjectii");
    void (*LanguageSet)(JNIEnv *, jobject, jstring, jstring) =
        (void *)ue3_sym("_Z26NativeCallback_LanguageSetP7_JNIEnvP8_jobjectP8_jstringS4_");
    jboolean (*Initialize)(JNIEnv *, jobject, int, int, float, jboolean, jobject, jboolean) =
        (void *)ue3_sym("_Z25NativeCallback_InitializeP7_JNIEnvP8_jobjectiifhS2_h");
    void (*NetworkUpdate)(JNIEnv *, jobject, jboolean, jboolean) =
        (void *)ue3_sym("_Z28NativeCallback_NetworkUpdateP7_JNIEnvP8_jobjecthh");
    NativeCallback_InputEvent = (void *)ue3_sym("_Z25NativeCallback_InputEventP7_JNIEnvP8_jobjectiiiix");
    NativeCallback_JoystickButtonEvent = (void *)ue3_sym("_Z34NativeCallback_JoystickButtonEventP7_JNIEnvP8_jobjectiiix");
    NativeCallback_JoystickAxisEvent = (void *)ue3_sym("_Z32NativeCallback_JoystickAxisEventP7_JNIEnvP8_jobjectiiifx");
    NativeCallback_MovieFinished = (void *)ue3_sym("_Z28NativeCallback_MovieFinishedP7_JNIEnvP8_jobject");
    NativeCallback_KeyboardFinished = (void *)ue3_sym("_Z31NativeCallback_KeyboardFinishedP7_JNIEnvP8_jobjectP8_jstring");
    NativeCallback_HandleBackButtonPressed = (void *)ue3_sym("_Z38NativeCallback_HandleBackButtonPressedv");

    LOG("JNI_OnLoad");
    JNI_OnLoad(&jvm, NULL);

    // same order the activity drives it in: systemStartupCheck, surfaceCreated, surfaceChanged, PostStartup
    SystemStats(&jni, java_thiz, 300LL * 1024 * 1024, 220.0f / 163.0f);
    KeyPadChange(&jni, java_thiz, JNI_FALSE);
    NetworkUpdate(&jni, java_thiz, JNI_FALSE, JNI_FALSE);

    LOG("InitEGLCallback");
    if (!InitEGLCallback(&jni, java_thiz))
        fatal_error("Engine failed to set up EGL.");
    PostInitUpdate(&jni, java_thiz, SCREEN_W, SCREEN_H);

    const char *lang, *country;
    system_language(&lang, &country);
    LanguageSet(&jni, java_thiz, jstring_new(lang), jstring_new(country));

    LOG("Initialize");
    // the vita's 5" screen
    if (!Initialize(&jni, java_thiz, SCREEN_W, SCREEN_H, 5.0f, JNI_FALSE, NULL, JNI_FALSE))
        fatal_error("Engine init failed.");

    LOG("engine thread running");
    while (1) {
        input_poll();
        movie_tick();
        ime_tick();
        sceKernelDelayThread(8000);
    }
    return 0;
}
