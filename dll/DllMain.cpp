// MinHook (to hook glOrtho on the render thread)
#include "jdk/minhook/include/MinHook.h"
#pragma comment(lib, "jdk/minhook/lib/libMinHook.x64.lib")

#include "Bytes.h"
#include "jdk/jni.h"
#include "jdk/jni_md.h"
#include "jdk/jvmti.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <mutex>

// OpenGL (for the glOrtho symbol)
#include <gl/GL.h>
#pragma comment(lib, "OpenGL32.lib")

// =========================================================
// Console + Logging
// =========================================================

static bool          g_consoleInitialized = false;
static std::mutex    g_logMutex;

static void InitConsole()
{
    if (g_consoleInitialized)
        return;

    g_consoleInitialized = true;

    AllocConsole();
    FILE* fDummy;
    freopen_s(&fDummy, "CONOUT$", "w", stdout);
    freopen_s(&fDummy, "CONOUT$", "w", stderr);
    SetConsoleTitleA("Main Debug Console");

    std::printf("[Main] Console initialized\n");
}

static void LOGF(const char* fmt, ...)
{
    if (!g_consoleInitialized)
        InitConsole();

    std::lock_guard<std::mutex> lock(g_logMutex);

    char buf[1024];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
    va_end(args);

    std::printf("%s\n", buf);
}

// =========================================================
// Globals JNI / JVM
// =========================================================

static JavaVM* g_vm = nullptr;

// =========================================================
// XRay state (toggle via key 'X')
// =========================================================

static bool          g_xrayEnabled = false;
static volatile bool g_needReload = false;

// =========================================================
// JNIBridge natives
// =========================================================
// public class JNIBridge {
//     public static native boolean allowBlock(String blockName);
//     public static native boolean xrayOn();
// }

static jboolean JNICALL Native_allowBlock(JNIEnv* env, jclass, jstring jName)
{
    if (!jName)
        return JNI_FALSE;

    const char* utf = env->GetStringUTFChars(jName, nullptr);
    bool allowed = false;

    if (utf)
    {
        const char* suffix = "_ore}";
        const size_t len = std::strlen(utf);
        const size_t suf_len = std::strlen(suffix);

        if (len >= suf_len && std::strcmp(utf + (len - suf_len), suffix) == 0)
        {
            allowed = true;
        }

        env->ReleaseStringUTFChars(jName, utf);
    }


    return allowed ? JNI_TRUE : JNI_FALSE;
}

static jboolean JNICALL Native_xrayOn(JNIEnv*, jclass)
{
    return g_xrayEnabled ? JNI_TRUE : JNI_FALSE;
}

// =========================================================
// JNI cache for Minecraft (used only for ReloadChunks)
// =========================================================

struct MinecraftJniCache
{
    bool      initialized;
    jclass    mcClass;
    jmethodID midGetMinecraft;
    jfieldID  fidRenderGlobal;
    jmethodID midLoadRenderers;
};

static MinecraftJniCache g_mcCache = { false, nullptr };

static bool InitMinecraftJniCache(JNIEnv*)
{
    if (!g_mcCache.initialized)
    {
        LOGF("[MinecraftCache] Not initialized from MainThread");
        return false;
    }
    return true;
}

// =========================================================
// Setup Minecraft cache based on Block's classloader
// =========================================================

static bool SetupMinecraftCacheFromLoader(JNIEnv* env, jobject blockLoader)
{
    if (g_mcCache.initialized)
        return true;

    if (!blockLoader)
    {
        LOGF("[MinecraftCache] blockLoader is null");
        return false;
    }

    jclass loaderClass = env->GetObjectClass(blockLoader);
    if (!loaderClass || env->ExceptionCheck())
    {
        env->ExceptionClear();
        LOGF("[MinecraftCache] Failed to get classloader class");
        return false;
    }

    jmethodID midLoadClass = env->GetMethodID(
        loaderClass,
        "loadClass",
        "(Ljava/lang/String;)Ljava/lang/Class;"
    );
    env->DeleteLocalRef(loaderClass);

    if (!midLoadClass || env->ExceptionCheck())
    {
        env->ExceptionClear();
        LOGF("[MinecraftCache] Failed to get loadClass() method");
        return false;
    }

    // Load net.minecraft.client.Minecraft via blockLoader
    jstring jMcName = env->NewStringUTF("net.minecraft.client.Minecraft");
    jobject mcClassObj = env->CallObjectMethod(blockLoader, midLoadClass, jMcName);
    env->DeleteLocalRef(jMcName);

    if (!mcClassObj || env->ExceptionCheck())
    {
        env->ExceptionClear();
        if (mcClassObj) env->DeleteLocalRef(mcClassObj);
        LOGF("[MinecraftCache] Failed to load Minecraft class via loader");
        return false;
    }

    g_mcCache.mcClass = (jclass)env->NewGlobalRef(mcClassObj);
    env->DeleteLocalRef(mcClassObj);

    if (!g_mcCache.mcClass)
    {
        LOGF("[MinecraftCache] NewGlobalRef(Minecraft) failed");
        return false;
    }

    g_mcCache.midGetMinecraft = env->GetStaticMethodID(
        g_mcCache.mcClass,
        "getMinecraft",
        "()Lnet/minecraft/client/Minecraft;"
    );
    if (!g_mcCache.midGetMinecraft || env->ExceptionCheck())
    {
        env->ExceptionClear();
        LOGF("[MinecraftCache] Failed to get getMinecraft()");
        return false;
    }

    g_mcCache.fidRenderGlobal = env->GetFieldID(
        g_mcCache.mcClass,
        "renderGlobal",
        "Lnet/minecraft/client/renderer/RenderGlobal;"
    );
    if (!g_mcCache.fidRenderGlobal || env->ExceptionCheck())
    {
        env->ExceptionClear();
        LOGF("[MinecraftCache] Failed to get field renderGlobal");
        return false;
    }

    // Load net.minecraft.client.renderer.RenderGlobal
    jstring jRgName = env->NewStringUTF("net.minecraft.client.renderer.RenderGlobal");
    jobject rgClassObj = env->CallObjectMethod(blockLoader, midLoadClass, jRgName);
    env->DeleteLocalRef(jRgName);

    if (!rgClassObj || env->ExceptionCheck())
    {
        env->ExceptionClear();
        if (rgClassObj) env->DeleteLocalRef(rgClassObj);
        LOGF("[MinecraftCache] Failed to load RenderGlobal via loader");
        return false;
    }

    jclass localRGClass = (jclass)rgClassObj;

    g_mcCache.midLoadRenderers = env->GetMethodID(
        localRGClass,
        "loadRenderers",
        "()V"
    );
    env->DeleteLocalRef(localRGClass);

    if (!g_mcCache.midLoadRenderers || env->ExceptionCheck())
    {
        env->ExceptionClear();
        LOGF("[MinecraftCache] Failed to get loadRenderers()");
        return false;
    }

    g_mcCache.initialized = true;
    LOGF("[MinecraftCache] Initialized from blockLoader");
    return true;
}

// =========================================================
// Reload chunks via RenderGlobal.loadRenderers()
// =========================================================

static void ReloadChunks()
{
    if (!g_vm)
        return;

    LOGF("[ReloadChunks] Starting");

    JNIEnv* env = nullptr;
    bool attached = false;

    jint res = g_vm->GetEnv((void**)&env, JNI_VERSION_1_8);
    if (res == JNI_EDETACHED)
    {
        if (g_vm->AttachCurrentThread((void**)&env, nullptr) != JNI_OK)
        {
            LOGF("[ReloadChunks] AttachCurrentThread failed");
            return;
        }
        attached = true;
    }
    else if (res != JNI_OK)
    {
        LOGF("[ReloadChunks] GetEnv failed (res=%d)", res);
        return;
    }

    if (!env)
    {
        LOGF("[ReloadChunks] env is null");
        if (attached)
            g_vm->DetachCurrentThread();
        return;
    }

    if (!InitMinecraftJniCache(env))
    {
        LOGF("[ReloadChunks] Minecraft cache init failed");
        if (attached)
            g_vm->DetachCurrentThread();
        return;
    }

    jobject mcObj = env->CallStaticObjectMethod(g_mcCache.mcClass, g_mcCache.midGetMinecraft);
    if (!mcObj || env->ExceptionCheck())
    {
        env->ExceptionClear();
        LOGF("[ReloadChunks] Failed to get Minecraft instance");
        if (mcObj) env->DeleteLocalRef(mcObj);
        if (attached)
            g_vm->DetachCurrentThread();
        return;
    }

    jobject rgObj = env->GetObjectField(mcObj, g_mcCache.fidRenderGlobal);
    if (!rgObj || env->ExceptionCheck())
    {
        env->ExceptionClear();
        LOGF("[ReloadChunks] Failed to get renderGlobal");
        if (rgObj) env->DeleteLocalRef(rgObj);
        env->DeleteLocalRef(mcObj);
        if (attached)
            g_vm->DetachCurrentThread();
        return;
    }

    env->CallVoidMethod(rgObj, g_mcCache.midLoadRenderers);
    if (env->ExceptionCheck())
    {
        env->ExceptionClear();
        LOGF("[ReloadChunks] loadRenderers() threw exception");
    }
    else
    {
        LOGF("[ReloadChunks] loadRenderers() OK");
    }

    env->DeleteLocalRef(rgObj);
    env->DeleteLocalRef(mcObj);

    if (attached)
        g_vm->DetachCurrentThread();

    LOGF("[ReloadChunks] Done");
}

// =========================================================
// Hook glOrtho : execute ReloadChunks on the render thread
// =========================================================

static decltype(&glOrtho) g_fn_glOrtho = nullptr;

void WINAPI hk_glOrtho(double left, double right, double bottom, double top, double zNear, double zFar)
{
    if (g_needReload)
    {
        g_needReload = false;
        LOGF("[glOrtho] g_needReload=true -> ReloadChunks()");
        ReloadChunks();
    }

    g_fn_glOrtho(left, right, bottom, top, zNear, zFar);
}

static bool InitHooks()
{
    LOGF("[Hooks] Initializing MinHook");

    if (MH_Initialize() != MH_OK)
    {
        LOGF("[Hooks] MH_Initialize failed");
        return false;
    }

    if (MH_CreateHook(
        &glOrtho,
        &hk_glOrtho,
        reinterpret_cast<void**>(&g_fn_glOrtho)
    ) != MH_OK)
    {
        LOGF("[Hooks] MH_CreateHook(glOrtho) failed");
        return false;
    }

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK)
    {
        LOGF("[Hooks] MH_EnableHook failed");
        return false;
    }

    LOGF("[Hooks] Hooks initialized");
    return true;
}

// =========================================================
// Keyboard thread : toggle XRay only
// =========================================================

static DWORD WINAPI KeyThread(LPVOID)
{
    LOGF("[KeyThread] Started");

    bool prevX = false;

    for (;;)
    {
        SHORT ksX = GetAsyncKeyState('X');
        bool curX = (ksX & 0x8000) != 0;

        if (curX && !prevX)
        {
            g_xrayEnabled = !g_xrayEnabled;
            LOGF("[KeyThread] XRay toggled: %s", g_xrayEnabled ? "ON" : "OFF");
            g_needReload = true;
        }

        prevX = curX;

        Sleep(80);
    }

    return 0;
}

// =========================================================
// Thread : attach JVM + JVMTI + Redefine Block + Define JNIBridge
// =========================================================

static DWORD WINAPI MainThread(LPVOID)
{
    LOGF("[MainThread] Started");

    HMODULE hJvm = GetModuleHandleA("jvm.dll");
    if (!hJvm)
    {
        LOGF("[MainThread] jvm.dll not found");
        return 0;
    }

    using tJNI_GetCreatedJavaVMs = jint(JNICALL*)(JavaVM** vmBuf, jsize bufLen, jsize* numVMs);
    tJNI_GetCreatedJavaVMs GetCreatedJavaVMs =
        (tJNI_GetCreatedJavaVMs)GetProcAddress(hJvm, "JNI_GetCreatedJavaVMs");
    if (!GetCreatedJavaVMs)
    {
        LOGF("[MainThread] JNI_GetCreatedJavaVMs not found");
        return 0;
    }

    jsize vmCount = 0;
    if (GetCreatedJavaVMs(nullptr, 0, &vmCount) != JNI_OK || vmCount == 0)
    {
        LOGF("[MainThread] No Java VM found");
        return 0;
    }

    JavaVM* vm = nullptr;
    if (GetCreatedJavaVMs(&vm, 1, &vmCount) != JNI_OK || vmCount == 0 || !vm)
    {
        LOGF("[MainThread] Failed to get Java VM pointer");
        return 0;
    }

    g_vm = vm;
    LOGF("[MainThread] Got Java VM");

    JNIEnv* env = nullptr;
    bool attached = false;

    jint jniRes = vm->GetEnv((void**)&env, JNI_VERSION_1_8);
    if (jniRes == JNI_EDETACHED)
    {
        if (vm->AttachCurrentThread((void**)&env, nullptr) != JNI_OK)
        {
            LOGF("[MainThread] AttachCurrentThread failed");
            return 0;
        }
        attached = true;
        LOGF("[MainThread] Attached to JVM");
    }
    else if (jniRes != JNI_OK)
    {
        LOGF("[MainThread] GetEnv failed (res=%d)", jniRes);
        return 0;
    }

    if (!env)
    {
        LOGF("[MainThread] env is null");
        if (attached)
            vm->DetachCurrentThread();
        return 0;
    }

    jvmtiEnv* tiEnv = nullptr;
    if (vm->GetEnv((void**)&tiEnv, JVMTI_VERSION_1_0) != JNI_OK || !tiEnv)
    {
        LOGF("[MainThread] Failed to get JVMTI env");
        if (attached)
            vm->DetachCurrentThread();
        return 0;
    }

    LOGF("[MainThread] JVMTI env acquired");

    do
    {
        jvmtiCapabilities caps;
        std::memset(&caps, 0, sizeof(caps));
        caps.can_redefine_classes = JNI_TRUE;

        jvmtiError err = tiEnv->AddCapabilities(&caps);
        if (err != JVMTI_ERROR_NONE && err != JVMTI_ERROR_NOT_AVAILABLE)
        {
            LOGF("[MainThread] AddCapabilities failed: %d", err);
            break;
        }

        // Search for already loaded Block class
        jint    classCount = 0;
        jclass* classes = nullptr;

        err = tiEnv->GetLoadedClasses(&classCount, &classes);
        if (err != JVMTI_ERROR_NONE)
        {
            LOGF("[MainThread] GetLoadedClasses failed: %d", err);
            break;
        }

        LOGF("[MainThread] Loaded classes: %d", classCount);

        jclass blockClass = nullptr;

        for (int i = 0; i < classCount; ++i)
        {
            char* sig = nullptr;
            if (tiEnv->GetClassSignature(classes[i], &sig, nullptr) == JVMTI_ERROR_NONE && sig)
            {
                if (std::strcmp(sig, "Lnet/minecraft/block/Block;") == 0)
                {
                    blockClass = classes[i];
                    tiEnv->Deallocate((unsigned char*)sig);
                    LOGF("[MainThread] Found Block class");
                    break;
                }
                tiEnv->Deallocate((unsigned char*)sig);
            }
        }

        if (classes)
            tiEnv->Deallocate((unsigned char*)classes);

        if (!blockClass)
        {
            LOGF("[MainThread] Block class not found");
            break;
        }

        jvmtiClassDefinition def;
        std::memset(&def, 0, sizeof(def));
        def.klass = blockClass;
        def.class_byte_count = (jint)sizeof(block_class_bytes);
        def.class_bytes = block_class_bytes;

        err = tiEnv->RedefineClasses(1, &def);
        if (err != JVMTI_ERROR_NONE)
        {
            LOGF("[MainThread] RedefineClasses(Block) failed: %d", err);
            break;
        }
        LOGF("[MainThread] Block class redefined (len=%d)", (int)sizeof(block_class_bytes));

        // Retrieve Block's classloader to DefineClass(JNIBridge)
        jobject blockLoader = nullptr;
        err = tiEnv->GetClassLoader(blockClass, &blockLoader);
        if (err != JVMTI_ERROR_NONE || !blockLoader)
        {
            LOGF("[MainThread] GetClassLoader(Block) failed: %d", err);
            break;
        }

        jclass jniBridgeClass = env->DefineClass(
            "JNIBridge",
            blockLoader,
            reinterpret_cast<const jbyte*>(JNIBridge_class),
            (jsize)sizeof(JNIBridge_class)
        );

        if (!jniBridgeClass || env->ExceptionCheck())
        {
            env->ExceptionClear();
            LOGF("[MainThread] DefineClass(JNIBridge) failed");
            env->DeleteLocalRef(blockLoader);
            break;
        }

        LOGF("[MainThread] JNIBridge defined");

        JNINativeMethod methods[] = {
            {
                const_cast<char*>("allowBlock"),
                const_cast<char*>("(Ljava/lang/String;)Z"),
                (void*)&Native_allowBlock
            },
            {
                const_cast<char*>("xrayOn"),
                const_cast<char*>("()Z"),
                (void*)&Native_xrayOn
            }
        };

        if (env->RegisterNatives(jniBridgeClass, methods,
            (jint)(sizeof(methods) / sizeof(methods[0]))) != 0)
        {
            LOGF("[MainThread] RegisterNatives(JNIBridge) failed");
            env->DeleteLocalRef(jniBridgeClass);
            env->DeleteLocalRef(blockLoader);
            break;
        }

        LOGF("[MainThread] JNIBridge natives registered");

        // Initialize Minecraft cache for ReloadChunks()
        if (!SetupMinecraftCacheFromLoader(env, blockLoader))
        {
            LOGF("[MainThread] SetupMinecraftCacheFromLoader failed");
            env->DeleteLocalRef(jniBridgeClass);
            env->DeleteLocalRef(blockLoader);
            break;
        }

        env->DeleteLocalRef(jniBridgeClass);
        env->DeleteLocalRef(blockLoader);

        // Initialize hooks (glOrtho)
        if (!InitHooks())
        {
            LOGF("[MainThread] InitHooks failed");
            break;
        }

    } while (false);

    if (attached)
        vm->DetachCurrentThread();

    LOGF("[MainThread] Exiting");
    return 0;
}

// =========================================================
// DllMain
// =========================================================

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID)
{
    if (fdwReason == DLL_PROCESS_ATTACH)
    {
        InitConsole();
        LOGF("[DllMain] DLL_PROCESS_ATTACH");

        DisableThreadLibraryCalls(hinstDLL);
        CreateThread(nullptr, 0, &MainThread, nullptr, 0, nullptr);
        CreateThread(nullptr, 0, &KeyThread, nullptr, 0, nullptr);
    }
    else if (fdwReason == DLL_PROCESS_DETACH)
    {
        LOGF("[DllMain] DLL_PROCESS_DETACH");
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
    }
    return TRUE;
}
