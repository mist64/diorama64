#include "qx_perm.h"
#include <android_native_app_glue.h>
#include <jni.h>
#include "vos_log.h"

namespace ar {

namespace {
// The JNI env of this thread for the scope; detaches on exit only when it attached.
struct Env {
    JavaVM *vm = nullptr;
    JNIEnv *env = nullptr;
    bool attached = false;
    explicit Env(android_app *app) : vm(app->activity->vm) {
        if (vm->GetEnv((void **)&env, JNI_VERSION_1_6) == JNI_OK) return;
        env = nullptr;
        if (vm->AttachCurrentThread(&env, nullptr) == JNI_OK) attached = true;
        else env = nullptr;
    }
    ~Env() {
        if (env && env->ExceptionCheck()) env->ExceptionClear();
        if (attached) vm->DetachCurrentThread();
    }
    bool failed() {   // a pending exception (cleared): the call failed
        if (!env->ExceptionCheck()) return false;
        env->ExceptionClear();
        return true;
    }
};
}  // namespace

bool qx_perm_granted(android_app *app, const char *perm) {
    Env e(app);
    if (!e.env) return false;
    JNIEnv *env = e.env;
    jobject act = app->activity->clazz;
    jclass cls = env->GetObjectClass(act);
    jmethodID check = env->GetMethodID(cls, "checkSelfPermission", "(Ljava/lang/String;)I");
    bool granted = false;
    if (!e.failed() && check) {
        jstring p = env->NewStringUTF(perm);
        const jint r = env->CallIntMethod(act, check, p);
        granted = !e.failed() && r == 0;   // PackageManager.PERMISSION_GRANTED
        env->DeleteLocalRef(p);
    }
    env->DeleteLocalRef(cls);
    return granted;
}

bool qx_perm_request(android_app *app, const char *perm) {
    Env e(app);
    if (!e.env) return false;
    JNIEnv *env = e.env;
    jobject act = app->activity->clazz;
    jclass cls = env->GetObjectClass(act);
    jmethodID req = env->GetMethodID(cls, "requestPermissions", "([Ljava/lang/String;I)V");
    bool made = false;
    if (!e.failed() && req) {
        jclass str_cls = env->FindClass("java/lang/String");
        jstring p = env->NewStringUTF(perm);
        jobjectArray arr = str_cls ? env->NewObjectArray(1, str_cls, p) : nullptr;
        if (!e.failed() && arr) {
            env->CallVoidMethod(act, req, arr, (jint)1);
            made = !e.failed();
        }
        if (arr) env->DeleteLocalRef(arr);
        env->DeleteLocalRef(p);
        if (str_cls) env->DeleteLocalRef(str_cls);
    }
    env->DeleteLocalRef(cls);
    vos_log("AR_QUEST_SCENE permission=requested ok=%d", made ? 1 : 0);
    return made;
}

}  // namespace ar
