#ifndef JAVA_H
#define JAVA_H

#include <stdint.h>
#include <falso_jni/FalsoJNI.h>

extern jobject java_thiz;
extern volatile int splash_hidden;

// natives the main thread calls after startup, looked up in main.c
extern jboolean (*NativeCallback_InputEvent)(JNIEnv *, jobject, int, int, int, int, int64_t);
extern jboolean (*NativeCallback_JoystickButtonEvent)(JNIEnv *, jobject, int, int, int, int64_t);
extern jboolean (*NativeCallback_JoystickAxisEvent)(JNIEnv *, jobject, int, int, int, float, int64_t);
extern void (*NativeCallback_MovieFinished)(JNIEnv *, jobject);
extern void (*NativeCallback_KeyboardFinished)(JNIEnv *, jobject, jstring);
extern void (*NativeCallback_HandleBackButtonPressed)(void);

#endif
