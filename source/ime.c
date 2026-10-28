#include "ime.h"
#include "java.h"
#include "util.h"

static volatile int pending;

void ime_open(const char *text, int password) {
    LOG("ShowKeyBoard '%s'", text);
    pending = 1;
}

void ime_tick(void) {
    if (pending) {
        pending = 0;
        NativeCallback_KeyboardFinished(&jni, java_thiz, jni->NewStringUTF(&jni, ""));
    }
}
