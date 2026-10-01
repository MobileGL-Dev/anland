#define _GNU_SOURCE
/* The Anland-owned renderer ABI. libMobileGL is loaded only in :mobilegl;
 * Android's framework/UI process never installs MobileGL's GL entry points. */
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

static int (*display_install)(void (*)(void *, uint32_t, uint32_t), void *);
static int (*display_attach)(ANativeWindow *, uint32_t, uint32_t);
static int (*display_detach)(uint32_t);
static int (*display_uninstall)(uint32_t);
static int (*server_serve)(const char *);
static void (*server_stop)(void);
static void *runtime;
static JavaVM *geometry_vm;
static jobject geometry_worker;
static jmethodID geometry_method;
static pthread_mutex_t geometry_lock = PTHREAD_MUTEX_INITIALIZER;

static void fail(JNIEnv *env, const char *message)
{
    jclass error = (*env)->FindClass(env, "java/lang/IllegalStateException");
    if (error) (*env)->ThrowNew(env, error, message);
}

static void request_geometry(void *user, uint32_t width, uint32_t height)
{
    (void)user;
    pthread_mutex_lock(&geometry_lock);
    JavaVM *vm = geometry_vm;
    pthread_mutex_unlock(&geometry_lock);
    if (!vm) return;
    JNIEnv *env = NULL;
    int attached = 0;
    const jint current = (*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6);
    if (current == JNI_EDETACHED) {
        char name[16] = "mgl-srv-apply";
        pthread_getname_np(pthread_self(), name, sizeof(name));
        JavaVMAttachArgs args = {JNI_VERSION_1_6, name, NULL};
        if ((*vm)->AttachCurrentThread(vm, &env, &args) != JNI_OK) return;
        attached = 1;
    } else if (current != JNI_OK) {
        return;
    }
    pthread_mutex_lock(&geometry_lock);
    jobject worker = geometry_worker ? (*env)->NewLocalRef(env, geometry_worker) : NULL;
    jmethodID method = geometry_method;
    pthread_mutex_unlock(&geometry_lock);
    if (worker && method) {
        (*env)->CallVoidMethod(env, worker, method, (jint)width, (jint)height);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionDescribe(env);
            (*env)->ExceptionClear(env);
        }
        (*env)->DeleteLocalRef(env, worker);
    }
    if (attached) (*vm)->DetachCurrentThread(vm);
}

JNIEXPORT void JNICALL Java_com_anland_consumer_MobileGLWorker_nativeLoad(
    JNIEnv *env, jclass clazz, jstring path)
{
    (void)clazz;
    if (runtime) return;
    const char *name = (*env)->GetStringUTFChars(env, path, NULL);
    if (!name) return;
    runtime = dlopen(name, RTLD_NOW | RTLD_LOCAL);
    (*env)->ReleaseStringUTFChars(env, path, name);
    if (!runtime) {
        fail(env, dlerror());
        return;
    }
#define LOAD(variable, symbol) do { \
    *(void **)(&(variable)) = dlsym(runtime, symbol); \
    if (!(variable)) { fail(env, "Embedded MobileGL is missing " symbol); return; } \
} while (0)
    LOAD(display_install, "mobilegl_server_display_install_android");
    LOAD(display_attach, "mobilegl_server_display_attach_android");
    LOAD(display_detach, "mobilegl_server_display_detach");
    LOAD(display_uninstall, "mobilegl_server_display_uninstall");
    LOAD(server_serve, "mobilegl_server_serve_inprocess");
    LOAD(server_stop, "mobilegl_server_stop_inprocess");
#undef LOAD
}

JNIEXPORT void JNICALL Java_com_anland_consumer_MobileGLWorker_nativeInstallDisplay(
    JNIEnv *env, jobject worker)
{
    jclass clazz = (*env)->GetObjectClass(env, worker);
    jmethodID method = (*env)->GetMethodID(env, clazz, "requestGeometry", "(II)V");
    if (!method) return;
    JavaVM *vm = NULL;
    if ((*env)->GetJavaVM(env, &vm) != JNI_OK) {
        fail(env, "Cannot attach MobileGL geometry callback to Anland");
        return;
    }
    pthread_mutex_lock(&geometry_lock);
    geometry_vm = vm;
    if (geometry_worker) (*env)->DeleteGlobalRef(env, geometry_worker);
    geometry_worker = (*env)->NewGlobalRef(env, worker);
    geometry_method = method;
    pthread_mutex_unlock(&geometry_lock);
    if (!display_install || display_install(request_geometry, NULL) != 0)
        fail(env, "Could not install Anland's MobileGL display");
}

JNIEXPORT void JNICALL Java_com_anland_consumer_MobileGLWorker_nativeAttachWindow(
    JNIEnv *env, jclass clazz, jobject surface, jint width, jint height)
{
    (void)clazz;
    ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
    if (!window || width <= 0 || height <= 0) {
        if (window) ANativeWindow_release(window);
        fail(env, "Anland Surface has no valid buffer geometry");
        return;
    }
    const int rc = display_attach(window, (uint32_t)width, (uint32_t)height);
    ANativeWindow_release(window);
    if (rc != 0) fail(env, "Embedded MobileGL could not retain Anland Surface");
}

JNIEXPORT jint JNICALL Java_com_anland_consumer_MobileGLWorker_nativeDetachWindow(
    JNIEnv *env, jclass clazz)
{
    (void)env; (void)clazz;
    return display_detach ? display_detach(3000) : 0;
}

JNIEXPORT void JNICALL Java_com_anland_consumer_MobileGLWorker_nativeUninstallDisplay(
    JNIEnv *env, jclass clazz)
{
    (void)clazz;
    if (display_uninstall) display_uninstall(3000);
    pthread_mutex_lock(&geometry_lock);
    if (geometry_worker) (*env)->DeleteGlobalRef(env, geometry_worker);
    geometry_worker = NULL;
    geometry_method = NULL;
    pthread_mutex_unlock(&geometry_lock);
}

JNIEXPORT jint JNICALL Java_com_anland_consumer_MobileGLWorker_nativeServe(
    JNIEnv *env, jclass clazz, jstring endpoint)
{
    (void)clazz;
    const char *name = (*env)->GetStringUTFChars(env, endpoint, NULL);
    if (!name) return 71;
    const int result = server_serve(name);
    (*env)->ReleaseStringUTFChars(env, endpoint, name);
    return result;
}

JNIEXPORT void JNICALL Java_com_anland_consumer_MobileGLWorker_nativeStop(
    JNIEnv *env, jclass clazz)
{
    (void)env; (void)clazz;
    if (server_stop) server_stop();
}
