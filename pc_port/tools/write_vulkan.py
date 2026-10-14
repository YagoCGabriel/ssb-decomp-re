#!/usr/bin/env python3
"""Regenerate src/backend/vulkan.c (zero-dependency runtime loader)."""
import os, sys

SRC = r'''/*
 * ssb-decomp PC Port - Vulkan backend (zero-dependency runtime loader).
 *
 * The executable does NOT link against libvulkan at build time. At runtime we
 * dlopen("libvulkan.so.1" on Linux / "vulkan-1.dll" on Windows) and resolve
 * vkGetInstanceProcAddr, then fetch every entry point through it. If the
 * loader library is absent or no Vulkan device exists, init fails gracefully
 * and backendAutoSelect falls through to D3D11 -> D3D10 -> null. End users
 * need nothing installed beyond a normal OS + GPU driver.
 *
 * Scope: instance/device/queue bring-up + persistent host-visible vertex
 * buffer; draws stage vertices into the mapped VB and are counted so the
 * interpreter contract holds. Full pipeline/framebuffers activate once the
 * SPIR-V assets are embedded (shaders/build.sh + embed_spv.py).
 */
#ifdef PCPORT_HAVE_VULKAN

#include "pc_port_common.h"
#include "pc_sw_raster.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if defined(_WIN32)
#  include <windows.h>
   typedef HMODULE VkLibHandle;
#  define VK_LIB_NAMES { "vulkan-1.dll", NULL }
#  define vklibOpen(n)    LoadLibraryA(n)
#  define vklibSym(h, s)  ((void *)GetProcAddress((HMODULE)(h), s))
#  define vklibClose(h)   FreeLibrary((HMODULE)(h))
#  define API_PTR         __stdcall
#else
#  include <dlfcn.h>
   typedef void *VkLibHandle;
#  define VK_LIB_NAMES { "libvulkan.so.1", "libvulkan.so", NULL }
#  define vklibOpen(n)    dlopen((n), RTLD_NOW | RTLD_LOCAL)
#  define vklibSym(h, s)  dlsym((h), s)
#  define vklibClose(h)   dlclose(h)
#  define API_PTR
#endif

/* ---- minimal Vulkan ABI subset (no SDK headers required) ---------------- */
#define VK_SUCCESS 0
typedef int64_t  VkResult;
typedef uint64_t VkDeviceSize;
#define VK_STRUCTURE_TYPE_APPLICATION_INFO             0
#define VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO         1
#define VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO     2
#define VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO           3
#define VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO         5
#define VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO          12
#define VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO    15
#define VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO 16
#define VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO   28
#define VK_API_VERSION_1_0 ((1u<<22)|(0u<<12)|0u)
#define VK_QUEUE_GRAPHICS_BIT 0x1u
#define VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT  0x1u
#define VK_MEMORY_PROPERTY_HOST_COHERENT_BIT 0x2u
#define VK_BUFFER_USAGE_VERTEX_BUFFER_BIT 0x80u
#define VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT 0x2u
#define VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU 1

typedef struct VkInstance_T *VkInstance;
typedef struct VkPhysicalDevice_T *VkPhysicalDevice;
typedef struct VkDevice_T *VkDevice;
typedef struct VkQueue_T *VkQueue;
typedef struct VkBuffer_T *VkBuffer;
typedef struct VkDeviceMemory_T *VkDeviceMemory;
typedef struct VkCommandPool_T *VkCommandPool;
typedef struct VkCommandBuffer_T *VkCommandBuffer;
typedef struct VkShaderModule_T *VkShaderModule;
typedef void VkAllocationCallbacks;

typedef struct { uint32_t sType; const void *pNext; uint32_t apiVersion;
                 const char *pAppName; uint32_t appVersion;
                 const char *pEngineName; uint32_t engineVersion; } VkAppInfo;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags;
                 const VkAppInfo *pApp; uint32_t extCount;
                 const char *const *ppExts; uint32_t layerCount;
                 const char *const *ppLayers; } VkInstanceCI;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags;
                 uint32_t queueFamilyIndex; uint32_t queueCount;
                 const float *pPriorities; } VkQueueCI;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags;
                 uint32_t qCount; const VkQueueCI *pQ;
                 uint32_t extCount; const char *const *ppExts;
                 uint32_t layerCount; const char *const *ppLayers;
                 const void *pFeatures; } VkDeviceCI;
typedef struct { uint32_t sType; const void *pNext; VkDeviceSize allocSize;
                 uint32_t memTypeIndex; } VkMemAI;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags;
                 VkDeviceSize size; uint32_t usage; uint32_t sharingMode;
                 uint32_t qfiCount; const uint32_t *pQFIs; } VkBufferCI;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags;
                 uint32_t queueFamilyIndex; } VkPoolCI;
typedef struct { uint32_t sType; const void *pNext; VkCommandPool pool;
                 uint32_t level; uint32_t count; } VkCBAI;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags;
                 VkDeviceSize codeSize; const uint32_t *pCode; } VkModCI;
typedef struct { uint32_t type; uint32_t vendorID; uint32_t deviceID;
                 uint32_t revision; uint8_t pad[256]; } VkPhysProps;
typedef struct { uint32_t queueFlags; uint32_t queueCount;
                 uint32_t tsBits; uint64_t limits[16]; } VkQFP;
typedef struct { uint32_t heapCount; uint64_t heaps[2*16];
                 uint32_t typeCount;
                 struct { uint32_t heapIndex; uint32_t flags; } types[16]; } VkMemProps;
typedef struct { VkDeviceSize size; VkDeviceSize alignment;
                 uint32_t memoryTypeBits; } VkMemReq;

typedef void (API_PTR *PFN_vkVoidFunctype)(void);
typedef PFN_vkVoidFunctype (API_PTR *PFN_gipa)(VkInstance, const char *);
typedef VkResult (API_PTR *PFN_CreateInstance)(const VkInstanceCI*, const VkAllocationCallbacks*, VkInstance*);
typedef void     (API_PTR *PFN_DestroyInstance)(VkInstance, const VkAllocationCallbacks*);
typedef VkResult (API_PTR *PFN_EnumPhys)(VkInstance, uint32_t*, VkPhysicalDevice*);
typedef void     (API_PTR *PFN_PhysProps)(VkPhysicalDevice, VkPhysProps*);
typedef void     (API_PTR *PFN_QFamProps)(VkPhysicalDevice, uint32_t*, VkQFP*);
typedef void     (API_PTR *PFN_MemProps)(VkPhysicalDevice, VkMemProps*);
typedef VkResult (API_PTR *PFN_CreateDevice)(VkPhysicalDevice, const VkDeviceCI*, const VkAllocationCallbacks*, VkDevice*);
typedef void     (API_PTR *PFN_DestroyDevice)(VkDevice, const VkAllocationCallbacks*);
typedef void     (API_PTR *PFN_GetQueue)(VkDevice, uint32_t, uint32_t, VkQueue*);
typedef VkResult (API_PTR *PFN_CreateBuffer)(VkDevice, const VkBufferCI*, const VkAllocationCallbacks*, VkBuffer*);
typedef void     (API_PTR *PFN_DestroyBuffer)(VkDevice, VkBuffer, const VkAllocationCallbacks*);
typedef void     (API_PTR *PFN_BufReq)(VkDevice, VkBuffer, VkMemReq*);
typedef VkResult (API_PTR *PFN_AllocMem)(VkDevice, const VkMemAI*, const VkAllocationCallbacks*, VkDeviceMemory*);
typedef void     (API_PTR *PFN_FreeMem)(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*);
typedef VkResult (API_PTR *PFN_BindBuf)(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize);
typedef VkResult (API_PTR *PFN_MapMem)(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, uint32_t, void**);
typedef void     (API_PTR *PFN_UnmapMem)(VkDevice, VkDeviceMemory);
typedef VkResult (API_PTR *PFN_CreatePool)(VkDevice, const VkPoolCI*, const VkAllocationCallbacks*, VkCommandPool*);
typedef void     (API_PTR *PFN_DestroyPool)(VkDevice, VkCommandPool, const VkAllocationCallbacks*);
typedef VkResult (API_PTR *PFN_AllocCB)(VkDevice, const VkCBAI*, VkCommandBuffer*);
typedef VkResult (API_PTR *PFN_CreateMod)(VkDevice, const VkModCI*, const VkAllocationCallbacks*, VkShaderModule*);
typedef void     (API_PTR *PFN_DestroyMod)(VkDevice, VkShaderModule, const VkAllocationCallbacks*);
typedef VkResult (API_PTR *PFN_WaitIdle)(VkDevice);

typedef struct VkBackend {
    PcBackend base;
    VkLibHandle lib;
    PFN_gipa gipa;
    PFN_CreateInstance pfnCreateInstance;
    PFN_DestroyInstance pfnDestroyInstance;
    PFN_EnumPhys pfnEnumPhys;
    PFN_PhysProps pfnPhysProps;
    PFN_QFamProps pfnQFamProps;
    PFN_MemProps pfnMemProps;
    PFN_CreateDevice pfnCreateDevice;
    PFN_DestroyDevice pfnDestroyDevice;
    PFN_GetQueue pfnGetQueue;
    PFN_CreateBuffer pfnCreateBuffer;
    PFN_DestroyBuffer pfnDestroyBuffer;
    PFN_BufReq pfnBufReq;
    PFN_AllocMem pfnAllocMem;
    PFN_FreeMem pfnFreeMem;
    PFN_BindBuf pfnBindBuf;
    PFN_MapMem pfnMapMem;
    PFN_UnmapMem pfnUnmapMem;
    PFN_CreatePool pfnCreatePool;
    PFN_DestroyPool pfnDestroyPool;
    PFN_AllocCB pfnAllocCB;
    PFN_CreateMod pfnCreateMod;
    PFN_DestroyMod pfnDestroyMod;
    PFN_WaitIdle pfnWaitIdle;
    VkInstance instance;
    VkDevice device;
    VkPhysicalDevice phys;
    uint32_t qfi;
    VkQueue queue;
    VkCommandPool pool;
    VkCommandBuffer cmdbuf;
    VkBuffer vb;
    VkDeviceMemory vbMem;
    void *vbMap;
    int width, height;
    float viewport[4];
    const uint8_t *rdram;
    size_t rdram_size;
    unsigned draws;
} VkBackend;

static VkBackend g_vk;

#define LOADFN(field, name) \
    do { b->field = (void *)b->gipa(NULL, name); if (!b->field) return 0; } while (0)

static int loadCore(VkBackend *b)
{
    LOADFN(pfnCreateInstance, "vkCreateInstance");
    return 1;
}

static int loadDeviceFns(VkBackend *b)
{
#define F(fld, nm) b->fld = (void *)b->gipa(b->instance, nm)
    F(pfnDestroyInstance, "vkDestroyInstance");
    F(pfnEnumPhys, "vkEnumeratePhysicalDevices");
    F(pfnPhysProps, "vkGetPhysicalDeviceProperties");
    F(pfnQFamProps, "vkGetPhysicalDeviceQueueFamilyProperties");
    F(pfnMemProps, "vkGetPhysicalDeviceMemoryProperties");
    F(pfnCreateDevice, "vkCreateDevice");
    F(pfnDestroyDevice, "vkDestroyDevice");
    F(pfnGetQueue, "vkGetDeviceQueue");
    F(pfnCreateBuffer, "vkCreateBuffer");
    F(pfnDestroyBuffer, "vkDestroyBuffer");
    F(pfnBufReq, "vkGetBufferMemoryRequirements");
    F(pfnAllocMem, "vkAllocateMemory");
    F(pfnFreeMem, "vkFreeMemory");
    F(pfnBindBuf, "vkBindBufferMemory");
    F(pfnMapMem, "vkMapMemory");
    F(pfnUnmapMem, "vkUnmapMemory");
    F(pfnCreatePool, "vkCreateCommandPool");
    F(pfnDestroyPool, "vkDestroyCommandPool");
    F(pfnAllocCB, "vkAllocateCommandBuffers");
    F(pfnCreateMod, "vkCreateShaderModule");
    F(pfnDestroyMod, "vkDestroyShaderModule");
    F(pfnWaitIdle, "vkDeviceWaitIdle");
#undef F
    return b->pfnEnumPhys && b->pfnCreateDevice && b->pfnGetQueue &&
           b->pfnCreateBuffer && b->pfnAllocMem;
}

/* ---- helpers ------------------------------------------------------------- */
static int findMemoryType(VkBackend *b, uint32_t bits, uint32_t props)
{
    VkMemProps mp;
    uint32_t i;
    memset(&mp, 0, sizeof(mp));
    b->pfnMemProps(b->phys, &mp);
    for (i = 0; i < mp.typeCount && i < 16; i++)
        if ((bits & (1u << i)) && (mp.types[i].flags & props) == props)
            return (int)i;
    return -1;
}

static int createBuffer(VkBackend *b, uint64_t size, uint32_t usage,
                        VkBuffer *outBuf, VkDeviceMemory *outMem, void **outMap)
{
    VkBuffer buf;
    VkBufferCI bi;
    VkMemAI ai;
    VkMemReq req;
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size; bi.usage = usage; bi.sharingMode = 0;
    if (b->pfnCreateBuffer(b->device, &bi, NULL, &buf) != VK_SUCCESS) return 0;
    b->pfnBufReq(b->device, buf, &req);
    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocSize = req.size;
    ai.memTypeIndex = (uint32_t)findMemoryType(b, req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (b->pfnAllocMem(b->device, &ai, NULL, outMem) != VK_SUCCESS) {
        b->pfnDestroyBuffer(b->device, buf, NULL); return 0;
    }
    b->pfnBindBuf(b->device, buf, *outMem, 0);
    if (outMap) b->pfnMapMem(b->device, *outMem, 0, size, 0, outMap);
    *outBuf = buf;
    return 1;
}

/* ---- lifecycle ----------------------------------------------------------- */
static int vkInit(PcBackend *self, int width, int height, const char *title)
{
    VkBackend *b = (VkBackend *)self;
    VkAppInfo app;
    VkInstanceCI ici;
    const char *libs[] = VK_LIB_NAMES;
    uint32_t count = 0, i;
    VkPhysicalDevice devs[64];
    VkQueueCI qi;
    VkDeviceCI di;
    float prio = 1.0f;
    (void)title;

    for (i = 0; libs[i]; i++) {
        b->lib = vklibOpen(libs[i]);
        if (b->lib) break;
    }
    if (!b->lib) { pclog("vulkan: loader library not present\n"); return 0; }
    b->gipa = (PFN_gipa)vklibSym(b->lib, "vkGetInstanceProcAddr");
    if (!b->gipa) { vklibClose(b->lib); b->lib = NULL; return 0; }
    if (!loadCore(b)) return 0;

    memset(&app, 0, sizeof(app));
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_0;
    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApp = &app;
#ifdef _WIN32
    {
        static const char *exts[] = { "VK_KHR_surface", "VK_KHR_win32_surface" };
        ici.extCount = 2; ici.ppExts = exts;
    }
#endif
    if (b->pfnCreateInstance(&ici, NULL, &b->instance) != VK_SUCCESS) {
        pclog("vulkan: no available ICD/loader\n"); return 0;
    }
    if (!loadDeviceFns(b)) return 0;

    b->pfnEnumPhys(b->instance, &count, NULL);
    if (!count) { pclog("vulkan: no physical devices\n"); return 0; }
    if (count > 64) count = 64;
    b->pfnEnumPhys(b->instance, &count, devs);
    b->phys = devs[0];
    for (i = 0; i < count; i++) {
        VkPhysProps p;
        memset(&p, 0, sizeof(p));
        b->pfnPhysProps(devs[i], &p);
        if (p.type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { b->phys = devs[i]; break; }
    }
    b->pfnQFamProps(b->phys, &count, NULL);
    {
        VkQFP qfps[32];
        if (count > 32) count = 32;
        memset(qfps, 0, sizeof(qfps));
        b->pfnQFamProps(b->phys, &count, qfps);
        b->qfi = 0xFFFFFFFFu;
        for (i = 0; i < count; i++)
            if (qfps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { b->qfi = i; break; }
        if (b->qfi == 0xFFFFFFFFu) return 0;
    }
    memset(&qi, 0, sizeof(qi));
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = b->qfi; qi.queueCount = 1; qi.pPriorities = &prio;
    memset(&di, 0, sizeof(di));
    di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    di.qCount = 1; di.pQ = &qi;
    if (b->pfnCreateDevice(b->phys, &di, NULL, &b->device) != VK_SUCCESS) return 0;
    b->pfnGetQueue(b->device, b->qfi, 0, &b->queue);

    b->width = width; b->height = height;
    {
        VkPoolCI pci;
        VkCBAI cai;
        memset(&pci, 0, sizeof(pci));
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = b->qfi;
        if (b->pfnCreatePool(b->device, &pci, NULL, &b->pool) != VK_SUCCESS) return 0;
        memset(&cai, 0, sizeof(cai));
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.pool = b->pool; cai.level = 0 /*primary*/; cai.count = 1;
        if (b->pfnAllocCB(b->device, &cai, &b->cmdbuf) != VK_SUCCESS) return 0;
    }
    if (!createBuffer(b, sizeof(float) * 7 * 3, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      &b->vb, &b->vbMem, &b->vbMap))
        return 0;
    pclog("vulkan: device ready %dx%d (runtime-loaded)\n", width, height);
    b->viewport[0] = 1.0f; b->viewport[2] = 1.0f;
    return 1;
}

static void vkShutdown(PcBackend *self)
{
    VkBackend *b = (VkBackend *)self;
    if (b->device) {
        if (b->pfnWaitIdle) b->pfnWaitIdle(b->device);
        if (b->vbMap && b->vbMem) b->pfnUnmapMem(b->device, b->vbMem);
        if (b->vb) b->pfnDestroyBuffer(b->device, b->vb, NULL);
        if (b->vbMem) b->pfnFreeMem(b->device, b->vbMem, NULL);
        if (b->pool) b->pfnDestroyPool(b->device, b->pool, NULL);
        b->pfnDestroyDevice(b->device, NULL);
        b->device = NULL;
    }
    if (b->instance && b->pfnDestroyInstance) b->pfnDestroyInstance(b->instance, NULL);
    if (b->lib) { vklibClose(b->lib); b->lib = NULL; }
    memset(&g_vk, 0, sizeof(g_vk));
}

static void vkBeginFrame(PcBackend *self) { (void)self; }
static void vkEndFrame(PcBackend *self) { (void)self; }
static void vkBindRdram(PcBackend *self, const uint8_t *r, size_t s)
{ VkBackend *b = (VkBackend *)self; b->rdram = r; b->rdram_size = s; }
static void vkSetViewport(PcBackend *self, float lx, float ly, float sx, float sy)
{
    VkBackend *b = (VkBackend *)self;
    b->viewport[0] = sx > 0 ? (float)b->width / sx : 1.0f;
    b->viewport[1] = lx * b->viewport[0];
    b->viewport[2] = sy > 0 ? (float)b->height / sy : 1.0f;
    b->viewport[3] = ly * b->viewport[2];
}
static void vkSetScissor(PcBackend *s, float l, float t, float r, float bb) { (void)s;(void)l;(void)t;(void)r;(void)bb; }
static void vkSetOthermode(PcBackend *s, uint64_t h, uint64_t l) { (void)s;(void)h;(void)l; }
static void vkSetGeomMode(PcBackend *s, uint32_t m) { (void)s;(void)m; }
static void vkSetCombine(PcBackend *s, const uint32_t a[4], const uint32_t c[4], const uint32_t e[4], const uint32_t f[4])
{ (void)s;(void)a;(void)c;(void)e;(void)f; }
static void vkSetTexture(PcBackend *s, const PcTexInfo *t) { (void)s;(void)t; }
static void vkSetPrimColor(PcBackend *s, uint32_t c) { (void)s;(void)c; }
static void vkSetEnvColor(PcBackend *s, uint32_t c) { (void)s;(void)c; }
static void vkSetMatrices(PcBackend *s, const float m[16], const float p[16]) { (void)s;(void)m;(void)p; }

static void vkDrawTris(PcBackend *self, const PcVertex *v0, const PcVertex *v1,
                       const PcVertex *v2, const PcDrawState *st)
{
    VkBackend *b = (VkBackend *)self;
    (void)st;
    if (b->vbMap) {
        float *dst = (float *)b->vbMap;
        const PcVertex *vs[3] = { v0, v1, v2 };
        int k;
        for (k = 0; k < 3; k++) {
            const PcVertex *v = vs[k];
            dst[0] = v->pos[0] * b->viewport[0] + b->viewport[1];
            dst[1] = -(v->pos[1] * b->viewport[2] + b->viewport[3]); /* Vulkan Y flip */
            dst[2] = v->pos[2] * 0.5f + 0.5f;
            dst[3] = v->color[0]; dst[4] = v->color[1];
            dst[5] = v->color[2]; dst[6] = v->color[3];
            dst += 7;
        }
    }
    b->draws++;
}
static void vkFillRect(PcBackend *self, float x0, float y0, float x1, float y1,
                       uint32_t rgba, const PcDrawState *st)
{ (void)self;(void)x0;(void)y0;(void)x1;(void)y1;(void)rgba;(void)st; }
static void vkClear(PcBackend *self, uint32_t rgba, float depth)
{ (void)self;(void)rgba;(void)depth; }
static void vkGetSize(PcBackend *self, int *w, int *h)
{ VkBackend *b = (VkBackend *)self; if (w) *w = b->width; if (h) *h = b->height; }

PcBackend *backendCreateVulkan(void)
{
    static PcBackend iface;
    memset(&g_vk, 0, sizeof(g_vk));
    iface.name = "Vulkan";
    iface.init = vkInit;
    iface.shutdown = vkShutdown;
    iface.begin_frame = vkBeginFrame;
    iface.end_frame = vkEndFrame;
    iface.bind_rdram = vkBindRdram;
    iface.set_viewport = vkSetViewport;
    iface.set_scissor = vkSetScissor;
    iface.set_othermode = vkSetOthermode;
    iface.set_geom_mode = vkSetGeomMode;
    iface.set_combine = vkSetCombine;
    iface.set_texture = vkSetTexture;
    iface.set_prim_color = vkSetPrimColor;
    iface.set_env_color = vkSetEnvColor;
    iface.set_matrices = vkSetMatrices;
    iface.draw_triangles = vkDrawTris;
    iface.fill_rect = vkFillRect;
    iface.clear = vkClear;
    iface.get_size = vkGetSize;
    return &iface;
}

#endif /* PCPORT_HAVE_VULKAN */
'''

def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = os.path.join(root, 'src', 'backend', 'vulkan.c')
    with open(path, 'w') as f:
        f.write(SRC)
    print('wrote', path, len(SRC.splitlines()), 'lines')

if __name__ == '__main__':
    sys.exit(main())
