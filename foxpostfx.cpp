// foxpostfx.cpp - Vulkan loader shim that runs foxfx post-process passes right before present.
//
// How it plugs in (no inline hooks, no Dobby):
//   DXVK -> dlopen(GTAV_VULKAN_LIBRARY) -> vkGetInstanceProcAddr   (this file exports it)
//   this file -> dlopen(next lib: libgtavvk.so / libgtav_vulkan.so) -> real vkGetInstanceProcAddr
// We only intercept: vkCreateDevice, vkGetDeviceProcAddr, vkGetDeviceQueue, vkCreateSwapchainKHR,
// vkDestroySwapchainKHR, vkQueuePresentKHR, vkDestroyDevice.
//
// Per present: copy swapchain image -> A, run net/detail/final fullscreen passes (ping-pong A/B),
// copy result -> swapchain image, then present. Only 8-bit RGBA/BGRA swapchains are touched.
// UNTESTED on device.

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "spv_shaders.h"

#define TAG "foxpostfx"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define EXPORT extern "C" __attribute__((visibility("default")))

// ------------------------------------------------------------------ config
struct Cfg {
    int enable = 1, net = 1, detail = 1, final_ = 1;
    float amount = 1.0f, coring = 0.004f, clip = 0.07f, foliage = 0.9f;
    float horizon = 0.43f, strength = 1.2f, satmax = 0.29f, bloom = 0.07f;
    float glossrefl = 1.5f, bloomthr = 0.65f, bloomedge = 0.9f;
    float hud[4] = {0, 0, 0, 0};
};

static std::mutex g_cfgMu;
static Cfg g_cfg;
static std::string g_cfgPath;
static time_t g_cfgMtime = 0;
static uint64_t g_cfgFrame = 0;

static std::string findCfgPath() {
    if (const char* e = getenv("FOXPOSTFX_CFG")) return e;
    Dl_info info;
    if (dladdr((void*)&findCfgPath, &info) && info.dli_fname) {
        std::string p = info.dli_fname;
        size_t s = p.find_last_of('/');
        if (s != std::string::npos) {
            std::string c = p.substr(0, s + 1) + "foxpostfx.cfg";
            struct stat st;
            if (stat(c.c_str(), &st) == 0) return c;
        }
    }
    const char* fixed[] = {
        "/sdcard/Android/data/COM.FOXENT.GTAV/files/foxpostfx.cfg",
        "/storage/emulated/0/Android/data/COM.FOXENT.GTAV/files/foxpostfx.cfg",
        "/storage/emulated/0/Download/foxpostfx.cfg",
    };
    for (const char* f : fixed) {
        struct stat st;
        if (stat(f, &st) == 0) return f;
    }
    return "";
}

static void loadCfg(bool force) {
    std::lock_guard<std::mutex> lk(g_cfgMu);
    if (g_cfgPath.empty()) g_cfgPath = findCfgPath();
    if (g_cfgPath.empty()) return;
    struct stat st;
    if (stat(g_cfgPath.c_str(), &st) != 0) return;
    if (!force && st.st_mtime == g_cfgMtime) return;
    g_cfgMtime = st.st_mtime;
    FILE* f = fopen(g_cfgPath.c_str(), "r");
    if (!f) return;
    Cfg c;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == '\n' || *p == 0) continue;
        char* eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        std::string k = p, v = eq + 1;
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        float x = (float)atof(v.c_str());
        int i = atoi(v.c_str());
        if (k == "enable") c.enable = i;
        else if (k == "net") c.net = i;
        else if (k == "detail") c.detail = i;
        else if (k == "final") c.final_ = i;
        else if (k == "amount") c.amount = x;
        else if (k == "coring") c.coring = x;
        else if (k == "clip") c.clip = x;
        else if (k == "foliage") c.foliage = x;
        else if (k == "horizon") c.horizon = x;
        else if (k == "strength") c.strength = x;
        else if (k == "satmax") c.satmax = x;
        else if (k == "bloom") c.bloom = x;
        else if (k == "glossrefl") c.glossrefl = x;
        else if (k == "bloomthr") c.bloomthr = x;
        else if (k == "bloomedge") c.bloomedge = x;
        else if (k == "hud") sscanf(v.c_str(), "%f %f %f %f", &c.hud[0], &c.hud[1], &c.hud[2], &c.hud[3]);
    }
    fclose(f);
    g_cfg = c;
    LOGI("cfg loaded: %s (enable=%d net=%d detail=%d final=%d)", g_cfgPath.c_str(), c.enable, c.net, c.detail, c.final_);
}

static Cfg getCfg() {
    if ((g_cfgFrame++ % 120) == 0) loadCfg(false);
    std::lock_guard<std::mutex> lk(g_cfgMu);
    return g_cfg;
}

// ------------------------------------------------------------------ next loader in the chain
static PFN_vkGetInstanceProcAddr g_nextGipa = nullptr;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static VkInstance g_instance = VK_NULL_HANDLE;
static PFN_vkGetDeviceProcAddr g_realGdpa = nullptr;

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance, const char*);

static void initChain() {
    std::vector<std::string> names;
    if (const char* e = getenv("FOXPOSTFX_NEXT")) names.push_back(e);
#ifdef FOXPOSTFX_DROPIN
    // we ARE libgtavvk.so: go straight to what libgtavvk.so used to load
    names.push_back("libgtav_vulkan.so");
#else
    names.push_back("libgtavvk.so");
    names.push_back("libgtav_vulkan.so");
#endif
    names.push_back("libvulkan.so");
    for (auto& n : names) {
        void* h = dlopen(n.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) continue;
        auto f = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
        if (f && f != &vkGetInstanceProcAddr) {
            g_nextGipa = f;
            LOGI("next vkGetInstanceProcAddr from %s", n.c_str());
            return;
        }
    }
    LOGE("no next vkGetInstanceProcAddr found");
}

// ------------------------------------------------------------------ device / swapchain state
#define DEV_FUNCS(X)                                                                                     \
    X(DestroyDevice) X(GetDeviceQueue) X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements)      \
    X(AllocateMemory) X(FreeMemory) X(BindImageMemory) X(CreateImageView) X(DestroyImageView)            \
    X(CreateRenderPass) X(DestroyRenderPass) X(CreateFramebuffer) X(DestroyFramebuffer)                  \
    X(CreateShaderModule) X(DestroyShaderModule) X(CreateDescriptorSetLayout)                            \
    X(DestroyDescriptorSetLayout) X(CreatePipelineLayout) X(DestroyPipelineLayout)                       \
    X(CreateGraphicsPipelines) X(DestroyPipeline) X(CreateSampler) X(DestroySampler)                     \
    X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets)                           \
    X(UpdateDescriptorSets) X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers)         \
    X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdPipelineBarrier) X(CmdCopyImage)                      \
    X(CmdClearColorImage) X(CmdBeginRenderPass) X(CmdEndRenderPass) X(CmdBindPipeline)                   \
    X(CmdBindDescriptorSets) X(CmdPushConstants) X(CmdSetViewport) X(CmdSetScissor) X(CmdDraw)           \
    X(QueueSubmit) X(CreateFence) X(DestroyFence) X(WaitForFences) X(ResetFences)                        \
    X(CreateSemaphore) X(DestroySemaphore) X(GetSwapchainImagesKHR) X(CreateSwapchainKHR)                \
    X(DestroySwapchainKHR) X(QueuePresentKHR) X(DeviceWaitIdle)

struct Dev {
    VkDevice h = VK_NULL_HANDLE;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mp{};
#define X(n) PFN_vk##n n = nullptr;
    DEV_FUNCS(X)
#undef X
};

struct Swap {
    Dev* d = nullptr;
    VkSwapchainKHR h = VK_NULL_HANDLE;
    VkFormat fmt = VK_FORMAT_UNDEFINED, ifmt = VK_FORMAT_UNDEFINED;
    VkExtent2D ext{};
    bool supported = false, tried = false, ok = false, glInit = false;
    std::vector<VkImage> img;
    VkImage im[2] = {};
    VkDeviceMemory mem[2] = {};
    VkImageView view[2] = {};
    VkFramebuffer fb[2] = {};
    VkImage gl = VK_NULL_HANDLE;
    VkDeviceMemory glm = VK_NULL_HANDLE;
    VkImageView glv = VK_NULL_HANDLE;
    VkRenderPass rp = VK_NULL_HANDLE;
    VkSampler smp = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    VkPipeline pipe[3] = {};
    VkDescriptorPool dp = VK_NULL_HANDLE;
    VkDescriptorSet ds[2] = {};
    VkCommandPool pool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cb;
    std::vector<VkFence> fence;
    std::vector<VkSemaphore> sem;
};

static std::mutex g_mu;
static std::unordered_map<VkDevice, Dev*> g_devs;
static std::unordered_map<VkSwapchainKHR, Swap*> g_swaps;
static std::unordered_map<VkQueue, uint32_t> g_queueFamily;

static PFN_vkVoidFunction nextProc(VkInstance inst, const char* name) {
    pthread_once(&g_once, initChain);
    return g_nextGipa ? g_nextGipa(inst, name) : nullptr;
}

// ------------------------------------------------------------------ helpers
static uint32_t findMem(Dev* d, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < d->mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (d->mp.memoryTypes[i].propertyFlags & want) == want) return i;
    for (uint32_t i = 0; i < d->mp.memoryTypeCount; ++i)
        if (bits & (1u << i)) return i;
    return 0;
}

static bool makeImage(Dev* d, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage, VkImage* im,
                      VkDeviceMemory* mem, VkImageView* view) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (d->CreateImage(d->h, &ci, nullptr, im) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    d->GetImageMemoryRequirements(d->h, *im, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = findMem(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (d->AllocateMemory(d->h, &ai, nullptr, mem) != VK_SUCCESS) return false;
    if (d->BindImageMemory(d->h, *im, *mem, 0) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = *im;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return d->CreateImageView(d->h, &vi, nullptr, view) == VK_SUCCESS;
}

static void barrier(Dev* d, VkCommandBuffer cb, VkImage im, VkImageLayout o, VkImageLayout n) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = o;
    b.newLayout = n;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                          nullptr, 0, nullptr, 1, &b);
}

static VkFormat unormOf(VkFormat f) {
    switch (f) {
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
        default: return VK_FORMAT_UNDEFINED;
    }
}

static VkShaderModule makeModule(Dev* d, const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    d->CreateShaderModule(d->h, &ci, nullptr, &m);
    return m;
}

static void destroySwapRes(Swap* s) {
    Dev* d = s->d;
    if (!d) return;
    d->DeviceWaitIdle(d->h);
    for (auto f : s->fence) if (f) d->DestroyFence(d->h, f, nullptr);
    for (auto m : s->sem) if (m) d->DestroySemaphore(d->h, m, nullptr);
    if (s->pool) d->DestroyCommandPool(d->h, s->pool, nullptr);
    if (s->dp) d->DestroyDescriptorPool(d->h, s->dp, nullptr);
    for (auto p : s->pipe) if (p) d->DestroyPipeline(d->h, p, nullptr);
    if (s->pl) d->DestroyPipelineLayout(d->h, s->pl, nullptr);
    if (s->dsl) d->DestroyDescriptorSetLayout(d->h, s->dsl, nullptr);
    if (s->smp) d->DestroySampler(d->h, s->smp, nullptr);
    for (auto f : s->fb) if (f) d->DestroyFramebuffer(d->h, f, nullptr);
    if (s->rp) d->DestroyRenderPass(d->h, s->rp, nullptr);
    for (int i = 0; i < 2; ++i) {
        if (s->view[i]) d->DestroyImageView(d->h, s->view[i], nullptr);
        if (s->im[i]) d->DestroyImage(d->h, s->im[i], nullptr);
        if (s->mem[i]) d->FreeMemory(d->h, s->mem[i], nullptr);
    }
    if (s->glv) d->DestroyImageView(d->h, s->glv, nullptr);
    if (s->gl) d->DestroyImage(d->h, s->gl, nullptr);
    if (s->glm) d->FreeMemory(d->h, s->glm, nullptr);
    *s = Swap();
}

static bool initSwap(Swap* s, uint32_t family) {
    Dev* d = s->d;
    uint32_t w = s->ext.width, h = s->ext.height;
    uint32_t n = 0;
    d->GetSwapchainImagesKHR(d->h, s->h, &n, nullptr);
    s->img.resize(n);
    d->GetSwapchainImagesKHR(d->h, s->h, &n, s->img.data());

    const VkImageUsageFlags rt = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    for (int i = 0; i < 2; ++i)
        if (!makeImage(d, w, h, s->ifmt, rt, &s->im[i], &s->mem[i], &s->view[i])) return false;
    if (!makeImage(d, 1, 1, VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   &s->gl, &s->glm, &s->glv))
        return false;

    // render pass (layouts are handled by explicit barriers)
    VkAttachmentDescription ad{};
    ad.format = s->ifmt;
    ad.samples = VK_SAMPLE_COUNT_1_BIT;
    ad.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ad.stencilLoadOp = ad.stencilStoreOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ad.initialLayout = ad.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ar{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &ar;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 1;
    rpi.pAttachments = &ad;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sp;
    if (d->CreateRenderPass(d->h, &rpi, nullptr, &s->rp) != VK_SUCCESS) return false;
    for (int i = 0; i < 2; ++i) {
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = s->rp;
        fi.attachmentCount = 1;
        fi.pAttachments = &s->view[i];
        fi.width = w;
        fi.height = h;
        fi.layers = 1;
        if (d->CreateFramebuffer(d->h, &fi, nullptr, &s->fb[i]) != VK_SUCCESS) return false;
    }

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.25f;
    if (d->CreateSampler(d->h, &si, nullptr, &s->smp) != VK_SUCCESS) return false;

    VkDescriptorSetLayoutBinding lb[2]{};
    for (int i = 0; i < 2; ++i) {
        lb[i].binding = (uint32_t)i;
        lb[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        lb[i].descriptorCount = 1;
        lb[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 2;
    dli.pBindings = lb;
    if (d->CreateDescriptorSetLayout(d->h, &dli, nullptr, &s->dsl) != VK_SUCCESS) return false;
    VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &s->dsl;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (d->CreatePipelineLayout(d->h, &pli, nullptr, &s->pl) != VK_SUCCESS) return false;

    VkShaderModule vs = makeModule(d, fullscreen_vert_spv, sizeof fullscreen_vert_spv);
    VkShaderModule fs[3] = {makeModule(d, foxfx_net_frag_spv, sizeof foxfx_net_frag_spv),
                            makeModule(d, foxfx_detail_frag_spv, sizeof foxfx_detail_frag_spv),
                            makeModule(d, foxfx_final_frag_spv, sizeof foxfx_final_frag_spv)};
    bool ok = vs && fs[0] && fs[1] && fs[2];
    for (int i = 0; ok && i < 3; ++i) {
        VkPipelineShaderStageCreateInfo st[2]{};
        st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        st[0].module = vs;
        st[0].pName = "main";
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        st[1].module = fs[i];
        st[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &cba;
        VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = 2;
        dy.pDynamicStates = dyn;
        VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gi.stageCount = 2;
        gi.pStages = st;
        gi.pVertexInputState = &vin;
        gi.pInputAssemblyState = &ia;
        gi.pViewportState = &vp;
        gi.pRasterizationState = &rs;
        gi.pMultisampleState = &ms;
        gi.pColorBlendState = &cb;
        gi.pDynamicState = &dy;
        gi.layout = s->pl;
        gi.renderPass = s->rp;
        ok = d->CreateGraphicsPipelines(d->h, VK_NULL_HANDLE, 1, &gi, nullptr, &s->pipe[i]) == VK_SUCCESS;
    }
    if (vs) d->DestroyShaderModule(d->h, vs, nullptr);
    for (auto m : fs) if (m) d->DestroyShaderModule(d->h, m, nullptr);
    if (!ok) return false;

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 2;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    if (d->CreateDescriptorPool(d->h, &dpi, nullptr, &s->dp) != VK_SUCCESS) return false;
    VkDescriptorSetLayout layouts[2] = {s->dsl, s->dsl};
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = s->dp;
    dai.descriptorSetCount = 2;
    dai.pSetLayouts = layouts;
    if (d->AllocateDescriptorSets(d->h, &dai, s->ds) != VK_SUCCESS) return false;
    for (int i = 0; i < 2; ++i) {
        VkDescriptorImageInfo ii[2] = {
            {s->smp, s->view[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {s->smp, s->glv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        };
        VkWriteDescriptorSet w[2]{};
        for (int b = 0; b < 2; ++b) {
            w[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[b].dstSet = s->ds[i];
            w[b].dstBinding = (uint32_t)b;
            w[b].descriptorCount = 1;
            w[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[b].pImageInfo = &ii[b];
        }
        d->UpdateDescriptorSets(d->h, 2, w, 0, nullptr);
    }

    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = family;
    if (d->CreateCommandPool(d->h, &cpi, nullptr, &s->pool) != VK_SUCCESS) return false;
    s->cb.resize(n);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = s->pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = n;
    if (d->AllocateCommandBuffers(d->h, &cai, s->cb.data()) != VK_SUCCESS) return false;
    s->fence.assign(n, VK_NULL_HANDLE);
    s->sem.assign(n, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < n; ++i) {
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VkSemaphoreCreateInfo smi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (d->CreateFence(d->h, &fi, nullptr, &s->fence[i]) != VK_SUCCESS) return false;
        if (d->CreateSemaphore(d->h, &smi, nullptr, &s->sem[i]) != VK_SUCCESS) return false;
    }
    LOGI("swapchain %ux%u fmt=%d: postfx ready (%u images)", w, h, (int)s->fmt, n);
    return true;
}

struct PcDetail { float amount, coring, clip, foliage; };
struct PcFinal {
    float horizon, strength, satMax, bloom;
    float hud[4];
    float glossRefl, bloomThr, bloomEdge, pad;
};
static_assert(sizeof(PcFinal) == 48, "push constant layout");

// returns the VkSemaphore to wait on in present, or VK_NULL_HANDLE if postfx was skipped
static VkSemaphore runPostFx(Swap* s, VkQueue q, uint32_t idx, const VkPresentInfoKHR* pi, const Cfg& c) {
    Dev* d = s->d;
    int passes[3], np = 0;
    if (c.net) passes[np++] = 0;
    if (c.detail) passes[np++] = 1;
    if (c.final_) passes[np++] = 2;
    if (np == 0) return VK_NULL_HANDLE;

    if (d->WaitForFences(d->h, 1, &s->fence[idx], VK_TRUE, 1000000000ull) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBuffer cb = s->cb[idx];
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    // the pool has RESET_COMMAND_BUFFER, begin implicitly resets once the fence has signalled
    if (d->BeginCommandBuffer(cb, &bi) != VK_SUCCESS) return VK_NULL_HANDLE;

    if (!s->glInit) {
        barrier(d, cb, s->gl, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue z{};
        VkImageSubresourceRange rg{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        d->CmdClearColorImage(cb, s->gl, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &rg);
        barrier(d, cb, s->gl, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        s->glInit = true;
    }

    VkImage sc = s->img[idx];
    VkImageCopy cp{};
    cp.srcSubresource = cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.extent = {s->ext.width, s->ext.height, 1};

    VkImageLayout lay[2] = {VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_UNDEFINED};
    barrier(d, cb, sc, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(d, cb, s->im[0], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    d->CmdCopyImage(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->im[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    lay[0] = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    int cur = 0;
    VkViewport vp{0, 0, (float)s->ext.width, (float)s->ext.height, 0, 1};
    VkRect2D sci{{0, 0}, s->ext};
    for (int k = 0; k < np; ++k) {
        int src = cur, dst = 1 - cur;
        barrier(d, cb, s->im[src], lay[src], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        barrier(d, cb, s->im[dst], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        lay[src] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        lay[dst] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rb.renderPass = s->rp;
        rb.framebuffer = s->fb[dst];
        rb.renderArea = sci;
        d->CmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
        d->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe[passes[k]]);
        d->CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pl, 0, 1, &s->ds[src], 0, nullptr);
        d->CmdSetViewport(cb, 0, 1, &vp);
        d->CmdSetScissor(cb, 0, 1, &sci);
        if (passes[k] == 1) {
            PcDetail p{c.amount, c.coring, c.clip, c.foliage};
            d->CmdPushConstants(cb, s->pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof p, &p);
        } else if (passes[k] == 2) {
            PcFinal p{c.horizon, c.strength, c.satmax, c.bloom,
                      {c.hud[0], c.hud[1], c.hud[2], c.hud[3]},
                      c.glossrefl, c.bloomthr, c.bloomedge, 0.0f};
            d->CmdPushConstants(cb, s->pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof p, &p);
        }
        d->CmdDraw(cb, 3, 1, 0, 0);
        d->CmdEndRenderPass(cb);
        cur = dst;
    }

    barrier(d, cb, s->im[cur], lay[cur], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(d, cb, sc, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    d->CmdCopyImage(cb, s->im[cur], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    barrier(d, cb, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    if (d->EndCommandBuffer(cb) != VK_SUCCESS) return VK_NULL_HANDLE;

    std::vector<VkPipelineStageFlags> ws(pi->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = pi->waitSemaphoreCount;
    si.pWaitSemaphores = pi->pWaitSemaphores;
    si.pWaitDstStageMask = ws.data();
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &s->sem[idx];
    d->ResetFences(d->h, 1, &s->fence[idx]);
    if (d->QueueSubmit(q, 1, &si, s->fence[idx]) != VK_SUCCESS) {
        LOGE("QueueSubmit failed, disabling postfx for this swapchain");
        s->ok = false;
        return VK_NULL_HANDLE;
    }
    return s->sem[idx];
}

// ------------------------------------------------------------------ hooks
static void registerDevice(VkPhysicalDevice pd, VkDevice dev) {
    Dev* d = new Dev();
    d->h = dev;
    d->pd = pd;
    if (!g_realGdpa) g_realGdpa = (PFN_vkGetDeviceProcAddr)nextProc(g_instance, "vkGetDeviceProcAddr");
#define X(n) d->n = (PFN_vk##n)g_realGdpa(dev, "vk" #n);
    DEV_FUNCS(X)
#undef X
    auto gpm = (PFN_vkGetPhysicalDeviceMemoryProperties)nextProc(g_instance, "vkGetPhysicalDeviceMemoryProperties");
    if (gpm) gpm(pd, &d->mp);
    std::lock_guard<std::mutex> lk(g_mu);
    g_devs[dev] = d;
}

static VKAPI_ATTR VkResult VKAPI_CALL fx_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci,
                                                      const VkAllocationCallbacks* a, VkDevice* out) {
    auto real = (PFN_vkCreateDevice)nextProc(g_instance, "vkCreateDevice");
    VkResult r = real(pd, ci, a, out);
    if (r == VK_SUCCESS) registerDevice(pd, *out);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL fx_DestroyDevice(VkDevice dev, const VkAllocationCallbacks* a) {
    Dev* d = nullptr;
    std::vector<Swap*> ss;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_devs.find(dev);
        if (it != g_devs.end()) { d = it->second; g_devs.erase(it); }
        for (auto i = g_swaps.begin(); i != g_swaps.end();)
            if (i->second->d == d) { ss.push_back(i->second); i = g_swaps.erase(i); } else ++i;
    }
    for (auto s : ss) { destroySwapRes(s); delete s; }
    auto real = (PFN_vkDestroyDevice)g_realGdpa(dev, "vkDestroyDevice");
    real(dev, a);
    delete d;
}

static VKAPI_ATTR void VKAPI_CALL fx_GetDeviceQueue(VkDevice dev, uint32_t fam, uint32_t idx, VkQueue* q) {
    auto real = (PFN_vkGetDeviceQueue)g_realGdpa(dev, "vkGetDeviceQueue");
    real(dev, fam, idx, q);
    if (q && *q) { std::lock_guard<std::mutex> lk(g_mu); g_queueFamily[*q] = fam; }
}

static VKAPI_ATTR VkResult VKAPI_CALL fx_CreateSwapchainKHR(VkDevice dev, const VkSwapchainCreateInfoKHR* ci,
                                                            const VkAllocationCallbacks* a, VkSwapchainKHR* out) {
    Dev* d = nullptr;
    { std::lock_guard<std::mutex> lk(g_mu); auto it = g_devs.find(dev); if (it != g_devs.end()) d = it->second; }
    auto real = (PFN_vkCreateSwapchainKHR)g_realGdpa(dev, "vkCreateSwapchainKHR");
    VkSwapchainCreateInfoKHR mod = *ci;
    VkFormat u = unormOf(ci->imageFormat);
    bool supported = d && u != VK_FORMAT_UNDEFINED;
    if (supported) mod.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkResult r = real(dev, &mod, a, out);
    if (r != VK_SUCCESS && supported) {   // driver refused extra usage: retry untouched
        supported = false;
        r = real(dev, ci, a, out);
    }
    if (r == VK_SUCCESS) {
        Swap* s = new Swap();
        s->d = d;
        s->h = *out;
        s->fmt = ci->imageFormat;
        s->ifmt = u;
        s->ext = ci->imageExtent;
        s->supported = supported;
        std::lock_guard<std::mutex> lk(g_mu);
        g_swaps[*out] = s;   // an old swapchain handle can never collide: handles are unique while alive
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL fx_DestroySwapchainKHR(VkDevice dev, VkSwapchainKHR sc, const VkAllocationCallbacks* a) {
    Swap* s = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_swaps.find(sc);
        if (it != g_swaps.end()) { s = it->second; g_swaps.erase(it); }
    }
    if (s) { destroySwapRes(s); delete s; }
    auto real = (PFN_vkDestroySwapchainKHR)g_realGdpa(dev, "vkDestroySwapchainKHR");
    real(dev, sc, a);
}

static VKAPI_ATTR VkResult VKAPI_CALL fx_QueuePresentKHR(VkQueue q, const VkPresentInfoKHR* pi) {
    Swap* s = nullptr;
    uint32_t family = 0;
    PFN_vkQueuePresentKHR real = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (pi->swapchainCount == 1) {
            auto it = g_swaps.find(pi->pSwapchains[0]);
            if (it != g_swaps.end()) s = it->second;
        }
        auto qf = g_queueFamily.find(q);
        if (qf != g_queueFamily.end()) family = qf->second;
        if (s && s->d) real = s->d->QueuePresentKHR;
    }
    if (!real) {   // unknown swapchain / multi-swapchain present: find any device's entry point
        std::lock_guard<std::mutex> lk(g_mu);
        for (auto& kv : g_devs) { real = kv.second->QueuePresentKHR; break; }
    }
    if (!real) return VK_ERROR_DEVICE_LOST;

    Cfg c = getCfg();
    if (!s || !s->supported || !c.enable) return real(q, pi);
    if (!s->tried) {
        s->tried = true;
        s->ok = initSwap(s, family);
        if (!s->ok) LOGE("initSwap failed - postfx disabled for this swapchain");
    }
    if (!s->ok) return real(q, pi);

    uint32_t idx = pi->pImageIndices[0];
    VkSemaphore sem = runPostFx(s, q, idx, pi, c);
    if (sem == VK_NULL_HANDLE) return real(q, pi);

    VkPresentInfoKHR p2 = *pi;
    p2.waitSemaphoreCount = 1;
    p2.pWaitSemaphores = &sem;
    return real(q, &p2);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fx_GetDeviceProcAddr(VkDevice dev, const char* name) {
    if (!strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)fx_DestroyDevice;
    if (!strcmp(name, "vkGetDeviceQueue")) return (PFN_vkVoidFunction)fx_GetDeviceQueue;
    if (!strcmp(name, "vkCreateSwapchainKHR")) return (PFN_vkVoidFunction)fx_CreateSwapchainKHR;
    if (!strcmp(name, "vkDestroySwapchainKHR")) return (PFN_vkVoidFunction)fx_DestroySwapchainKHR;
    if (!strcmp(name, "vkQueuePresentKHR")) return (PFN_vkVoidFunction)fx_QueuePresentKHR;
    if (!g_realGdpa) g_realGdpa = (PFN_vkGetDeviceProcAddr)nextProc(g_instance, "vkGetDeviceProcAddr");
    return g_realGdpa ? g_realGdpa(dev, name) : nullptr;
}

// ------------------------------------------------------------------ exported entry points
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance inst, const char* name) {
    if (inst) g_instance = inst;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)fx_CreateDevice;
    if (!strcmp(name, "vkGetDeviceProcAddr")) {
        if (!g_realGdpa) g_realGdpa = (PFN_vkGetDeviceProcAddr)nextProc(inst, "vkGetDeviceProcAddr");
        return (PFN_vkVoidFunction)fx_GetDeviceProcAddr;
    }
    // device-level names can also be asked through the instance
    if (!strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)fx_DestroyDevice;
    if (!strcmp(name, "vkGetDeviceQueue")) return (PFN_vkVoidFunction)fx_GetDeviceQueue;
    if (!strcmp(name, "vkCreateSwapchainKHR")) return (PFN_vkVoidFunction)fx_CreateSwapchainKHR;
    if (!strcmp(name, "vkDestroySwapchainKHR")) return (PFN_vkVoidFunction)fx_DestroySwapchainKHR;
    if (!strcmp(name, "vkQueuePresentKHR")) return (PFN_vkVoidFunction)fx_QueuePresentKHR;
    return nextProc(inst, name);
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice dev, const char* name) {
    return fx_GetDeviceProcAddr(dev, name);
}
