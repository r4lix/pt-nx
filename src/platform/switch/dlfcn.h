// libnx has no dynamic loader: the few desktop paths that look for a shared library (volk's own loader, optional runtimes) get
// "not found" and fall back, the driver itself comes from the linked nxvk archives.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define RTLD_LAZY 1
#define RTLD_NOW 2
#define RTLD_LOCAL 0
#define RTLD_GLOBAL 0x100

static inline void* dlopen(const char* file, int mode) { (void)file; (void)mode; return (void*)0; }
static inline void* dlsym(void* handle, const char* name) { (void)handle; (void)name; return (void*)0; }
static inline int dlclose(void* handle) { (void)handle; return 0; }
static inline char* dlerror(void) { return (char*)"dynamic loading is not available on this platform"; }

#ifdef __cplusplus
}
#endif
