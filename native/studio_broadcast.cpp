#include "studio_broadcast.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
namespace studio {
bool broadcast_requested() {
    const char* value = std::getenv("STUDIO_BROADCAST");
    return value && std::strcmp(value, "1") == 0;
}
#ifdef STUDIO_WITH_FUNNEL
}
#include <funnel-vk.h>
namespace studio {
namespace {
using Clock = std::chrono::steady_clock;
funnel_ctx* ctx = nullptr;
funnel_stream* stream = nullptr;
VkDevice dev = VK_NULL_HANDLE;
VkPhysicalDevice gpu = VK_NULL_HANDLE;
VkQueue queue = VK_NULL_HANDLE;
uint32_t family = 0;
VkCommandPool pool = VK_NULL_HANDLE;
VkCommandBuffer cmd = VK_NULL_HANDLE;
// A reusable device-local UNORM target: resizing here preserves encoded RGB
// values before copying the bytes to the SRGB-typed libfunnel image.
VkImage resize_image = VK_NULL_HANDLE;
VkDeviceMemory resize_memory = VK_NULL_HANDLE;
VkFormat resize_format = VK_FORMAT_UNDEFINED;
uint32_t resize_w = 0, resize_h = 0;
VkFilter resize_filter = VK_FILTER_LINEAR;
uint32_t last_w=0, last_h=0, last_dw=0, last_dh=0;
VkFormat last_format=VK_FORMAT_UNDEFINED;
bool started = false;
uint32_t out_w = 1920, out_h = 1080, out_fps = 60;
Clock::time_point next_frame{}, report{};
unsigned sent = 0, skipped = 0, dropped = 0;
const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
const VkImageSubresourceLayers layers = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
[[noreturn]] void fatal(const char* operation, int error) {
    std::fprintf(stderr,"[broadcast] %s failed (%d); stopping to avoid invalid GPU synchronization.\n",operation,error);
    std::fflush(stderr);
    std::_Exit(EXIT_FAILURE);
}
void vkcheck(VkResult r, const char* operation) { if (r != VK_SUCCESS) fatal(operation,r); }
void check(int r, const char* operation) { if (r < 0) fatal(operation,r); }
uint32_t setting(const char* key, uint32_t fallback, uint32_t lo, uint32_t hi) {
    const char* s = std::getenv(key); if (!s) return fallback;
    char* end = nullptr; const auto v = std::strtoul(s,&end,10);
    return end != s && !*end && v >= lo && v <= hi ? static_cast<uint32_t>(v) : fallback;
}
void barrier(VkImage image, VkImageLayout old_layout, VkImageLayout new_layout,
             VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags before,
             VkPipelineStageFlags after, uint32_t src_family=VK_QUEUE_FAMILY_IGNORED,
             uint32_t dst_family=VK_QUEUE_FAMILY_IGNORED) {
    VkImageMemoryBarrier b{};
    b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout=old_layout; b.newLayout=new_layout;
    b.srcAccessMask=src; b.dstAccessMask=dst;
    b.srcQueueFamilyIndex=src_family; b.dstQueueFamilyIndex=dst_family;
    b.image=image; b.subresourceRange=range;
    vkCmdPipelineBarrier(cmd,before,after,0,0,nullptr,0,nullptr,1,&b);
}
void destroy_resize_image() {
    if (resize_image) vkDestroyImage(dev,resize_image,nullptr);
    if (resize_memory) vkFreeMemory(dev,resize_memory,nullptr);
    resize_image=VK_NULL_HANDLE; resize_memory=VK_NULL_HANDLE;
    resize_w=resize_h=0; resize_format=VK_FORMAT_UNDEFINED;
}
VkFormat unorm_format(VkFormat format) {
    switch (format) {
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
        default: return VK_FORMAT_UNDEFINED;
    }
}
void ensure_resize_image(uint32_t w,uint32_t h,VkFormat format) {
    if (resize_image && resize_w==w && resize_h==h && resize_format==format) return;
    // Previous broadcast submission was completed by vkQueueWaitIdle.
    destroy_resize_image();
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(gpu,format,&props);
    const VkFormatFeatureFlags needed=VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((props.optimalTilingFeatures & needed)!=needed) fatal("UNORM resize image unsupported",format);
    VkImageCreateInfo info{}; info.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType=VK_IMAGE_TYPE_2D; info.format=format; info.extent={w,h,1};
    info.mipLevels=1; info.arrayLayers=1; info.samples=VK_SAMPLE_COUNT_1_BIT;
    info.tiling=VK_IMAGE_TILING_OPTIMAL;
    info.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode=VK_SHARING_MODE_EXCLUSIVE; info.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    vkcheck(vkCreateImage(dev,&info,nullptr,&resize_image),"create UNORM resize image");
    VkMemoryRequirements req{}; vkGetImageMemoryRequirements(dev,resize_image,&req);
    VkPhysicalDeviceMemoryProperties memory{}; vkGetPhysicalDeviceMemoryProperties(gpu,&memory);
    uint32_t type=memory.memoryTypeCount;
    for (uint32_t i=0;i<memory.memoryTypeCount;++i) {
        if ((req.memoryTypeBits & (1u<<i)) && (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {type=i; break;}
    }
    if (type==memory.memoryTypeCount) fatal("no device-local resize memory",-1);
    VkMemoryAllocateInfo alloc{}; alloc.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize=req.size; alloc.memoryTypeIndex=type;
    vkcheck(vkAllocateMemory(dev,&alloc,nullptr,&resize_memory),"allocate UNORM resize memory");
    vkcheck(vkBindImageMemory(dev,resize_image,resize_memory,0),"bind UNORM resize memory");
    resize_w=w; resize_h=h; resize_format=format;
}
struct Fit { uint32_t w,h,x,y; };
Fit aspect_fit(uint32_t w,uint32_t h,uint32_t dw,uint32_t dh) {
    uint32_t fit_w=dw, fit_h=dh;
    if (uint64_t(dw)*h <= uint64_t(dh)*w) fit_h=std::max(1u,static_cast<uint32_t>(uint64_t(h)*dw/w));
    else fit_w=std::max(1u,static_cast<uint32_t>(uint64_t(w)*dh/h));
    return {fit_w,fit_h,(dw-fit_w)/2,(dh-fit_h)/2};
}
void stats() {
    const auto now=Clock::now();
    if (now-report >= std::chrono::seconds(5)) {
        std::fprintf(stderr,"[broadcast] %u queued / %u no buffer / %u dropped (last %.1f s)\n",sent,skipped,dropped,std::chrono::duration<double>(now-report).count());
        sent=skipped=dropped=0; report=now;
    }
}
}
bool broadcast_extensions(VkPhysicalDevice physical) {
    uint32_t n=0;
    if (vkEnumerateDeviceExtensionProperties(physical,nullptr,&n,nullptr)!=VK_SUCCESS) return false;
    std::vector<VkExtensionProperties> ext(n);
    if (vkEnumerateDeviceExtensionProperties(physical,nullptr,&n,ext.data())!=VK_SUCCESS) return false;
    const char* required[]={VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME,VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME};
    for (auto name:required) {
        if (std::none_of(ext.begin(),ext.end(),[name](const VkExtensionProperties& e){return std::strcmp(e.extensionName,name)==0;})) {
            std::fprintf(stderr,"[broadcast] Missing extension: %s\n",name); return false;
        }
    }
    return true;
}
void broadcast_init(VkInstance instance, VkPhysicalDevice physical, VkDevice device, VkQueue q, uint32_t qfamily) {
    if (!broadcast_requested()) return;
    dev=device; gpu=physical; queue=q; family=qfamily;
    out_w=setting("STUDIO_BROADCAST_WIDTH",1920,320,7680);
    out_h=setting("STUDIO_BROADCAST_HEIGHT",1080,320,7680);
    out_fps=setting("STUDIO_BROADCAST_FPS",60,1,144);
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(gpu,VK_FORMAT_B8G8R8A8_UNORM,&props);
    if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) fatal("source BLIT_SRC",-1);
    if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
        resize_filter=VK_FILTER_NEAREST;
        std::fprintf(stderr,"[broadcast] Linear blit unavailable; falling back to nearest filtering.\n");
    }
    check(funnel_new(&ctx),"create context");
    check(funnel_set_app_name(ctx,"Kawari"),"app name");
    check(funnel_set_app_id(ctx,"ru.tesutoidclub.vtuber-studio"),"app id");
    check(funnel_set_app_version(ctx,"0.2b1"),"app version");
    check(funnel_connect(ctx),"connect PipeWire");
    check(funnel_stream_create(ctx,"Live2D",&stream),"create stream");
    check(funnel_stream_init_vulkan(stream,instance,gpu,dev),"init Vulkan");
    check(funnel_stream_set_size(stream,out_w,out_h),"set size");
    check(funnel_stream_set_mode(stream,FUNNEL_ASYNC),"set async mode");
    const auto rate=FUNNEL_FRACTION(out_fps,1);
    check(funnel_stream_set_rate(stream,rate,FUNNEL_RATE_VARIABLE,rate),"set rate");
    check(funnel_stream_vk_set_usage(stream,VK_IMAGE_USAGE_TRANSFER_DST_BIT),"set transfer usage");
    // Prefer alpha-bearing BGRA; allow RGBA if the consumer needs it.
    const int bgra=funnel_stream_vk_add_format(stream,VK_FORMAT_B8G8R8A8_SRGB,true,VK_FORMAT_FEATURE_BLIT_DST_BIT);
    const int rgba=funnel_stream_vk_add_format(stream,VK_FORMAT_R8G8B8A8_SRGB,true,VK_FORMAT_FEATURE_BLIT_DST_BIT);
    if (bgra<0 && rgba<0) fatal("no alpha-capable DMA-BUF format",bgra);
    VkCommandPoolCreateInfo pi{}; pi.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pi.queueFamilyIndex=family;
    vkcheck(vkCreateCommandPool(dev,&pi,nullptr,&pool),"create command pool");
    VkCommandBufferAllocateInfo ai{}; ai.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool=pool; ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount=1;
    vkcheck(vkAllocateCommandBuffers(dev,&ai,&cmd),"allocate command buffer");
    check(funnel_stream_configure(stream),"configure stream");
    check(funnel_stream_start(stream),"start stream");
    started=true; report=next_frame=Clock::now();
    std::fprintf(stderr,"[broadcast] Live2D %ux%u @ %u FPS (RGBA/BGRA alpha; color-preserving GPU transfer)\n",out_w,out_h,out_fps);
}
void broadcast_frame(VkImage source,VkFormat format,uint32_t w,uint32_t h) {
    if (!started || !source || !w || !h) return;
    if (format!=VK_FORMAT_B8G8R8A8_UNORM) fatal("unexpected source format",format);
    stats();
    const auto now=Clock::now();
    const auto period=std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0/out_fps));
    if (now+std::chrono::microseconds(250)<next_frame) return;
    next_frame=std::max(next_frame+period,now); // no catch-up bursts
    funnel_buffer* buf=nullptr;
    int ret=funnel_stream_dequeue(stream,&buf);
    if (ret==0) { ++skipped; return; }
    check(ret,"dequeue");
    if (!buf) fatal("empty dequeued buffer",-1);
    VkImage dest=VK_NULL_HANDLE; VkFormat dest_format{}; bool alpha=false;
    check(funnel_buffer_get_vk_image(buf,&dest),"get destination image");
    check(funnel_buffer_get_vk_format(buf,&dest_format,&alpha),"get destination format");
    uint32_t dw=0,dh=0; funnel_buffer_get_size(buf,&dw,&dh);
    if (!alpha || !dw || !dh || !dest) {check(funnel_stream_return(stream,buf),"return invalid buffer"); ++dropped; return;}
    // Integer aspect-fit avoids off-by-one scaling caused by floating point rounding.
    const Fit fit=aspect_fit(w,h,dw,dh);
    const uint32_t fit_w=fit.w, fit_h=fit.h;
    const int32_t bw=static_cast<int32_t>(fit.w), bh=static_cast<int32_t>(fit.h);
    const int32_t x=static_cast<int32_t>(fit.x), y=static_cast<int32_t>(fit.y);
    const VkFormat dst_unorm=unorm_format(dest_format);
    if (dst_unorm==VK_FORMAT_UNDEFINED) fatal("unexpected destination format",dest_format);
    const bool direct_copy=dst_unorm==format && fit_w==w && fit_h==h;
    if (!direct_copy) ensure_resize_image(dw,dh,dst_unorm); // fixed stream size; not reallocated on window resize
    if (w!=last_w || h!=last_h || dw!=last_dw || dh!=last_dh || dest_format!=last_format) {
        std::fprintf(stderr,"[broadcast] source %ux%u -> content %ux%u -> stream %ux%u | %s | %s%s\n",
            w,h,fit_w,fit_h,dw,dh,dst_unorm==VK_FORMAT_B8G8R8A8_UNORM?"BGRA":"RGBA",
            direct_copy?"exact byte copy":(resize_filter==VK_FILTER_LINEAR?"UNORM linear blit + byte copy":"UNORM nearest blit + byte copy"),
            fit_w>w || fit_h>h?" | UPSCALING: increase viewer framebuffer size for more detail":"");
        last_w=w; last_h=h; last_dw=dw; last_dh=dh; last_format=dest_format;
    }
    vkcheck(vkResetCommandBuffer(cmd,0),"reset commands");
    VkCommandBufferBeginInfo bi{}; bi.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkcheck(vkBeginCommandBuffer(cmd,&bi),"begin commands");
    barrier(source,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    // The consumer's old contents are discarded; acquire/release DMA-BUF ownership.
    barrier(dest,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_QUEUE_FAMILY_FOREIGN_EXT,family);
    const VkClearColorValue transparent={{0.f,0.f,0.f,0.f}};
    vkCmdClearColorImage(cmd,dest,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&transparent,1,&range);
    barrier(dest,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImage copy_source=source;
    if (!direct_copy) {
        // Never blit directly into an SRGB image: that would encode RGB again.
        // UNORM->UNORM also handles BGRA/RGBA channel conversion if required.
        barrier(resize_image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageBlit region{}; region.srcSubresource=layers; region.dstSubresource=layers;
        region.srcOffsets[1]={static_cast<int32_t>(w),static_cast<int32_t>(h),1};
        region.dstOffsets[1]={bw,bh,1};
        vkCmdBlitImage(cmd,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,resize_image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region,resize_filter);
        barrier(resize_image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        copy_source=resize_image;
    }
    // Copy-compatible 8-bit UNORM/SRGB images: vkCmdCopyImage does not apply
    // a transfer function; RGB and alpha bytes remain unchanged.
    VkImageCopy copy{}; copy.srcSubresource=layers; copy.dstSubresource=layers;
    copy.dstOffset={x,y,0}; copy.extent={fit_w,fit_h,1};
    vkCmdCopyImage(cmd,copy_source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,dest,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    barrier(source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    barrier(dest,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,0,
            VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,family,VK_QUEUE_FAMILY_FOREIGN_EXT);
    vkcheck(vkEndCommandBuffer(cmd),"end commands");
    VkSemaphore acquire=VK_NULL_HANDLE,release=VK_NULL_HANDLE; VkFence fence=VK_NULL_HANDLE;
    check(funnel_buffer_get_vk_semaphores(buf,&acquire,&release),"get sync semaphores");
    check(funnel_buffer_get_vk_fence(buf,&fence),"get completion fence");
    const VkPipelineStageFlags wait=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submit{}; submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount=1; submit.pWaitSemaphores=&acquire; submit.pWaitDstStageMask=&wait;
    submit.commandBufferCount=1; submit.pCommandBuffers=&cmd; submit.signalSemaphoreCount=1; submit.pSignalSemaphores=&release;
    vkcheck(vkQueueSubmit(queue,1,&submit,fence),"submit broadcast");
    // P0.2b deliberately serializes this extra GPU pass. No host pixel readback.
    vkcheck(vkQueueWaitIdle(queue),"wait broadcast commands");
    ret=funnel_stream_enqueue(stream,buf);
    check(ret,"enqueue"); if (ret==1) ++sent; else ++dropped;
}
void broadcast_shutdown() {
    if (!dev) return;
    vkDeviceWaitIdle(dev);
    if (stream) { if (started) funnel_stream_stop(stream); funnel_stream_destroy(stream); }
    if (ctx) funnel_shutdown(ctx);
    destroy_resize_image();
    if (pool) vkDestroyCommandPool(dev,pool,nullptr);
    started=false; stream=nullptr; ctx=nullptr; pool=VK_NULL_HANDLE; dev=VK_NULL_HANDLE;
}
#else
bool broadcast_extensions(VkPhysicalDevice) {return !broadcast_requested();}
void broadcast_init(VkInstance,VkPhysicalDevice,VkDevice,VkQueue,uint32_t) {
    if (broadcast_requested()) {std::fprintf(stderr,"[broadcast] Build with ./scripts/build.sh --with-broadcast\n"); std::exit(1);}
}
void broadcast_frame(VkImage,VkFormat,uint32_t,uint32_t) {}
void broadcast_shutdown() {}
#endif
}
