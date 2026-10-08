/*
 * dod-glfix: opengl32.dll proxy for Darkest of Days on modern AMD drivers.
 *
 * Problem: AMD's current OpenGL driver does not restore GL_DEPTH_BOUNDS_TEST_EXT
 * (nor the bounds) on glPopAttrib, although EXT_depth_bounds_test puts the
 * enable in GL_ENABLE_BIT/GL_DEPTH_BUFFER_BIT and the bounds in
 * GL_DEPTH_BUFFER_BIT. The game enables the test inside a push/pop pair and
 * relies on the pop to turn it off again. On AMD it stays on with a tiny
 * range, every fragment is rejected and the level renders black.
 *
 * Fix: shadow glPushAttrib/glPopAttrib and restore the depth bounds state
 * ourselves after the real glPopAttrib. Every other call is forwarded
 * unchanged to the system opengl32.dll (see gen_exports.py / stubs.S).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "exports.h"

typedef unsigned int GLenum;
typedef unsigned int GLbitfield;
typedef unsigned int GLuint;
typedef unsigned char GLboolean;
typedef int GLint;
typedef double GLclampd;
typedef double GLdouble;

#define GL_EXTENSIONS 0x1F03
#define GL_COMPILE 0x1300
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_ENABLE_BIT 0x00002000
#define GL_MAX_ATTRIB_STACK_DEPTH 0x0D35
#define GL_DEPTH_BOUNDS_TEST_EXT 0x8890
#define GL_DEPTH_BOUNDS_EXT 0x8891

/* Filled lazily by glfix_resolve; read by the asm stubs. */
void *real_table[GLFIX_EXPORT_COUNT];

static HMODULE real_dll;
static FILE *log_file;

static void log_open(void)
{
    char path[MAX_PATH];
    char *slash;
    if (log_file || !GetModuleFileNameA(NULL, path, MAX_PATH))
        return;
    slash = strrchr(path, '\\');
    if (!slash)
        return;
    strcpy(slash + 1, "dod_glfix.log");
    log_file = fopen(path, "w");
}

static void log_msg(const char *fmt, ...)
{
    va_list ap;
    if (!log_file)
        return;
    va_start(ap, fmt);
    vfprintf(log_file, fmt, ap);
    va_end(ap);
    fflush(log_file);
}

static void load_real_dll(void)
{
    WCHAR path[MAX_PATH];
    DWORD n;

    if (real_dll)
        return;
    /* Lets the fix be chained in front of another wrapper (e.g. apitrace). */
    n = GetEnvironmentVariableW(L"DOD_GLFIX_REAL_OPENGL32", path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        n = GetSystemDirectoryW(path, MAX_PATH);
        if (n == 0 || n + 14 >= MAX_PATH)
            goto fail;
        wcscat(path, L"\\opengl32.dll");
    }
    real_dll = LoadLibraryW(path);
    if (real_dll) {
        log_msg("real opengl32: %ls\n", path);
        return;
    }
fail:
    MessageBoxA(NULL, "dod-glfix: could not load the system opengl32.dll", "dod-glfix", MB_ICONERROR);
    ExitProcess(1);
}

/* Called from the asm stubs (cdecl) the first time an export is used. */
void *glfix_resolve(int index)
{
    void *p;

    load_real_dll();
    p = (void *)GetProcAddress(real_dll, export_names[index]);
    if (!p) {
        char msg[128];
        snprintf(msg, sizeof msg, "dod-glfix: system opengl32.dll lacks %s", export_names[index]);
        MessageBoxA(NULL, msg, "dod-glfix", MB_ICONERROR);
        ExitProcess(1);
    }
    real_table[index] = p;
    return p;
}

static void *real(int index)
{
    void *p = real_table[index];
    return p ? p : glfix_resolve(index);
}

#define REAL(name, type) ((type)real(IDX_##name))

typedef void(WINAPI *PFN_PushAttrib)(GLbitfield);
typedef void(WINAPI *PFN_PopAttrib)(void);
typedef void(WINAPI *PFN_NewList)(GLuint, GLenum);
typedef void(WINAPI *PFN_EndList)(void);
typedef void(WINAPI *PFN_Enable)(GLenum);
typedef GLboolean(WINAPI *PFN_IsEnabled)(GLenum);
typedef void(WINAPI *PFN_GetDoublev)(GLenum, GLdouble *);
typedef void(WINAPI *PFN_GetIntegerv)(GLenum, GLint *);
typedef const unsigned char *(WINAPI *PFN_GetString)(GLenum);
typedef void *(WINAPI *PFN_GetProcAddress)(const char *);
typedef void(APIENTRY *PFN_DepthBoundsEXT)(GLclampd, GLclampd);

/*
 * Shadow of the attribute stack. The game renders from one thread with one
 * context, so plain statics are enough.
 */
#define SHADOW_MAX 64

struct saved {
    GLbitfield mask;
    GLboolean enabled;
    GLdouble bounds[2];
};

static struct saved shadow[SHADOW_MAX];
static int shadow_depth;
static int driver_max_depth;
static int compiling_list;   /* inside glNewList(..., GL_COMPILE) */
static int dbt_support;      /* 0 = not checked yet, 1 = yes, -1 = no */
static PFN_DepthBoundsEXT real_DepthBoundsEXT;
static unsigned long fixes;  /* pops where the driver left the state wrong */

static int has_extension(const char *ext)
{
    const char *list = (const char *)REAL(glGetString, PFN_GetString)(GL_EXTENSIONS);
    size_t len = strlen(ext);

    while (list && (list = strstr(list, ext)) != NULL) {
        if (list[len] == ' ' || list[len] == '\0')
            return 1;
        list += len;
    }
    return 0;
}

static int depth_bounds_supported(void)
{
    if (dbt_support == 0) {
        GLint max = 0;
        if (has_extension("GL_EXT_depth_bounds_test"))
            real_DepthBoundsEXT = (PFN_DepthBoundsEXT)REAL(wglGetProcAddress, PFN_GetProcAddress)("glDepthBoundsEXT");
        dbt_support = real_DepthBoundsEXT ? 1 : -1;
        REAL(glGetIntegerv, PFN_GetIntegerv)(GL_MAX_ATTRIB_STACK_DEPTH, &max);
        driver_max_depth = (max > 0 && max < SHADOW_MAX) ? max : SHADOW_MAX;
        log_msg("GL_EXT_depth_bounds_test: %s, attrib stack depth %d\n",
                dbt_support > 0 ? "present, fix active" : "absent, fix inactive", max);
    }
    return dbt_support > 0;
}

void WINAPI glPushAttrib_hook(GLbitfield mask)
{
    struct saved s = { 0 };

    if (!compiling_list && depth_bounds_supported()) {
        s.mask = mask & (GL_ENABLE_BIT | GL_DEPTH_BUFFER_BIT);
        if (s.mask)
            s.enabled = REAL(glIsEnabled, PFN_IsEnabled)(GL_DEPTH_BOUNDS_TEST_EXT);
        if (s.mask & GL_DEPTH_BUFFER_BIT)
            REAL(glGetDoublev, PFN_GetDoublev)(GL_DEPTH_BOUNDS_EXT, s.bounds);
    }

    REAL(glPushAttrib, PFN_PushAttrib)(mask);

    /* Mirror the driver: a push past the limit is a GL_STACK_OVERFLOW no-op. */
    if (!compiling_list && dbt_support > 0 && shadow_depth < driver_max_depth)
        shadow[shadow_depth++] = s;
}

void WINAPI glPopAttrib_hook(void)
{
    struct saved *s;

    REAL(glPopAttrib, PFN_PopAttrib)();

    if (compiling_list || dbt_support <= 0 || shadow_depth == 0)
        return;
    s = &shadow[--shadow_depth];
    if (!s->mask)
        return;

    if (REAL(glIsEnabled, PFN_IsEnabled)(GL_DEPTH_BOUNDS_TEST_EXT) != s->enabled) {
        if (fixes++ == 0)
            log_msg("driver did not restore GL_DEPTH_BOUNDS_TEST_EXT on glPopAttrib; restoring it\n");
    }
    if (s->mask & GL_DEPTH_BUFFER_BIT)
        real_DepthBoundsEXT(s->bounds[0], s->bounds[1]);
    if (s->enabled)
        REAL(glEnable, PFN_Enable)(GL_DEPTH_BOUNDS_TEST_EXT);
    else
        REAL(glDisable, PFN_Enable)(GL_DEPTH_BOUNDS_TEST_EXT);
}

/*
 * Push/pop compiled into a display list run later via glCallList, where we
 * cannot see them, so leave the shadow stack alone while compiling.
 */
void WINAPI glNewList_hook(GLuint list, GLenum mode)
{
    compiling_list = (mode == GL_COMPILE);
    REAL(glNewList, PFN_NewList)(list, mode);
}

void WINAPI glEndList_hook(void)
{
    compiling_list = 0;
    REAL(glEndList, PFN_EndList)();
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        log_open();
        log_msg("dod-glfix loaded\n");
    } else if (reason == DLL_PROCESS_DETACH && log_file) {
        log_msg("depth bounds state restored after %lu glPopAttrib calls\n", fixes);
        fclose(log_file);
        log_file = NULL;
    }
    return TRUE;
}
