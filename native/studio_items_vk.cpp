#include "studio_items_vk.hpp"
#include "studio_items.hpp"
#include "studio_controls.hpp"
#include "studio_items_shaders.hpp"
#include <map>
#include <array>
#include <chrono>
#include <stdexcept>
#include <cstring>
namespace studio { namespace items { namespace {
VkPhysicalDevice gpu{};VkDevice dev{};VkQueue queue{};VkCommandPool pool{};VkCommandBuffer cmd{};VkDescriptorSetLayout setlayout{};VkDescriptorPool descriptors{};VkPipelineLayout layout{};VkSampler sampler{};VkPipeline front{},back{};VkFormat current{};bool disabled=false;
using Clock=std::chrono::steady_clock;
unsigned qfamily=0;
struct Texture {Pixels pixels;VkImage image{};VkImageView view{};VkDeviceMemory mem{};VkBuffer upload{};VkDeviceMemory host{};VkDescriptorSet set{};void* mapped=nullptr;size_t frame=size_t(-1);Clock::time_point start=Clock::now();};
std::map<std::string,Texture> cache;std::map<std::string,std::string> failures;
using CubismModel=Live2D::Cubism::Framework::CubismModel;
struct MeshFrame {
    const CubismModel* model=nullptr;
    const CubismModel* catalog_model=nullptr;
    std::vector<std::string> ids;
    std::map<std::string,int> indices;
    std::array<float,16> projection{};
    float aspect=1;
} mesh_frame;

const std::string& mesh_model_path(){
    const auto& original=active_original_model();
    return original.empty()?active_runtime_model():original;
}
struct Geometry {
    const float* vertices;
    const unsigned short* indices;
    int vertex_count,triangle_count;

    explicit Geometry(int index) :
        vertices(mesh_frame.model->GetDrawableVertices(index)),
        indices(mesh_frame.model->GetDrawableVertexIndices(index)),
        vertex_count(mesh_frame.model->GetDrawableVertexCount(index)),
        triangle_count(mesh_frame.model->GetDrawableVertexIndexCount(index)/3) {}

    bool project(unsigned triangle,Triangle& out)const{
        if(!vertices||!indices||triangle_count<=0||triangle>=unsigned(triangle_count))return false;
        for(unsigned k=0;k<3;++k){
            const int index=indices[triangle*3+k];
            if(index>=vertex_count||!project_vertex(mesh_frame.projection.data(),mesh_frame.aspect,
                    {vertices[index*2],vertices[index*2+1]},out.vertices[k]))return false;
        }
        return true;
    }
};
bool find_anchor(int mesh,Point point,bool inside_only,Anchor& result){
    const Geometry geometry(mesh);
    float best=std::numeric_limits<float>::infinity();
    for(int index=0;index<geometry.triangle_count;++index){
        Triangle triangle;Anchor anchor;float distance;
        if(!geometry.project(unsigned(index),triangle)||!bind_triangle(triangle,point,inside_only,anchor,distance)||distance>=best)continue;
        best=distance;anchor.triangle=unsigned(index);result=anchor;
        if(best==0.f)break;
    }
    return std::isfinite(best);
}
bool resolve_frame(const MeshBinding& binding,AttachmentFrame& frame){
    if(!mesh_frame.model||binding.model!=mesh_model_path())return false;
    const auto mesh=mesh_frame.indices.find(binding.id);
    if(mesh==mesh_frame.indices.end())return false;
    Triangle triangle;
    return Geometry(mesh->second).project(binding.anchor.triangle,triangle)&&attachment_frame(triangle,binding.anchor,frame);
}
bool item_pose(const Item& item,Pose& out){
    if(item.mesh){
        AttachmentFrame frame;
        if(!resolve_frame(item.mesh,frame))return false;
        out=mesh_pose(item,frame,mesh_frame.aspect);
    }else{
        float x,y;camera_offset(x,y);out=pose(item,camera_scale(),x,y);
    }
    return std::isfinite(out.x)&&std::isfinite(out.y)&&std::isfinite(out.size)&&out.size>0.f;
}
bool bind_mesh(Item& item,const std::string& id,const Anchor& anchor,const Pose& visible){
    Item next=item;
    next.attached=false;next.mesh={mesh_model_path(),id,anchor};
    AttachmentFrame frame;
    if(!resolve_frame(next.mesh,frame))return false;
    fit_mesh(next,visible,frame,mesh_frame.aspect);
    if(!valid(next))return false;
    item=std::move(next);
    return true;
}

void ck(VkResult r){if(r!=VK_SUCCESS)throw std::runtime_error("Vulkan item resource error "+std::to_string(int(r)));}
uint32_t memory(uint32_t bits,VkMemoryPropertyFlags flags){VkPhysicalDeviceMemoryProperties p;vkGetPhysicalDeviceMemoryProperties(gpu,&p);for(uint32_t i=0;i<p.memoryTypeCount;++i)if((bits&(1u<<i))&&(p.memoryTypes[i].propertyFlags&flags)==flags)return i;throw std::runtime_error("No item memory type");}
VkDeviceMemory alloc(VkMemoryRequirements r,VkMemoryPropertyFlags flags){VkMemoryAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;a.allocationSize=r.size;a.memoryTypeIndex=memory(r.memoryTypeBits,flags);VkDeviceMemory m{};ck(vkAllocateMemory(dev,&a,nullptr,&m));return m;}
VkImageView view(VkImage image,VkFormat format){VkImageViewCreateInfo a{};a.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;a.image=image;a.viewType=VK_IMAGE_VIEW_TYPE_2D;a.format=format;a.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};VkImageView v{};ck(vkCreateImageView(dev,&a,nullptr,&v));return v;}
void release(Texture& t){if(t.set)vkFreeDescriptorSets(dev,descriptors,1,&t.set);if(t.mapped)vkUnmapMemory(dev,t.host);if(t.upload)vkDestroyBuffer(dev,t.upload,nullptr);if(t.host)vkFreeMemory(dev,t.host,nullptr);if(t.view)vkDestroyImageView(dev,t.view,nullptr);if(t.image)vkDestroyImage(dev,t.image,nullptr);if(t.mem)vkFreeMemory(dev,t.mem,nullptr);}
VkShaderModule shader(const uint32_t* code,size_t size){VkShaderModuleCreateInfo s{};s.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;s.codeSize=size;s.pCode=code;VkShaderModule m{};ck(vkCreateShaderModule(dev,&s,nullptr,&m));return m;}
VkPipeline pipeline(VkFormat fmt,bool behind){VkShaderModule v{},f{};VkPipeline p{};try{v=shader(items_vert,sizeof(items_vert));f=shader(items_frag,sizeof(items_frag));VkPipelineShaderStageCreateInfo stages[2]{};for(auto& s:stages){s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;s.pName="main";}stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=v;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=f;
VkPipelineVertexInputStateCreateInfo vi{};vi.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;VkPipelineInputAssemblyStateCreateInfo ia{};ia.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;VkPipelineViewportStateCreateInfo vp{};vp.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;vp.viewportCount=vp.scissorCount=1;VkPipelineRasterizationStateCreateInfo rs{};rs.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;rs.polygonMode=VK_POLYGON_MODE_FILL;rs.cullMode=VK_CULL_MODE_NONE;rs.lineWidth=1;VkPipelineMultisampleStateCreateInfo ms{};ms.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;VkPipelineColorBlendAttachmentState a{};a.blendEnable=VK_TRUE;a.srcColorBlendFactor=a.srcAlphaBlendFactor=behind?VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA:VK_BLEND_FACTOR_ONE;a.dstColorBlendFactor=a.dstAlphaBlendFactor=behind?VK_BLEND_FACTOR_ONE:VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;a.colorBlendOp=a.alphaBlendOp=VK_BLEND_OP_ADD;a.colorWriteMask=15;VkPipelineColorBlendStateCreateInfo cb{};cb.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;cb.attachmentCount=1;cb.pAttachments=&a;VkDynamicState states[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};VkPipelineDynamicStateCreateInfo ds{};ds.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;ds.dynamicStateCount=2;ds.pDynamicStates=states;VkPipelineRenderingCreateInfoKHR ri{};ri.sType=VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;ri.colorAttachmentCount=1;ri.pColorAttachmentFormats=&fmt;VkGraphicsPipelineCreateInfo pi{};pi.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;pi.pNext=&ri;pi.stageCount=2;pi.pStages=stages;pi.pVertexInputState=&vi;pi.pInputAssemblyState=&ia;pi.pViewportState=&vp;pi.pRasterizationState=&rs;pi.pMultisampleState=&ms;pi.pColorBlendState=&cb;pi.pDynamicState=&ds;pi.layout=layout;ck(vkCreateGraphicsPipelines(dev,VK_NULL_HANDLE,1,&pi,nullptr,&p));}catch(...){if(v)vkDestroyShaderModule(dev,v,nullptr);if(f)vkDestroyShaderModule(dev,f,nullptr);throw;}vkDestroyShaderModule(dev,v,nullptr);vkDestroyShaderModule(dev,f,nullptr);return p;}
void prepare(VkFormat fmt){if(!pool){VkCommandPoolCreateInfo c{};c.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;c.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;c.queueFamilyIndex=qfamily;ck(vkCreateCommandPool(dev,&c,nullptr,&pool));VkCommandBufferAllocateInfo ca{};ca.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;ca.commandPool=pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;ck(vkAllocateCommandBuffers(dev,&ca,&cmd));VkDescriptorSetLayoutBinding b{};b.binding=0;b.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;b.descriptorCount=1;b.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;VkDescriptorSetLayoutCreateInfo dl{};dl.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;dl.bindingCount=1;dl.pBindings=&b;ck(vkCreateDescriptorSetLayout(dev,&dl,nullptr,&setlayout));VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,32};VkDescriptorPoolCreateInfo dp{};dp.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;dp.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;dp.maxSets=32;dp.poolSizeCount=1;dp.pPoolSizes=&ps;ck(vkCreateDescriptorPool(dev,&dp,nullptr,&descriptors));VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,32};VkPipelineLayoutCreateInfo pl{};pl.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;pl.setLayoutCount=1;pl.pSetLayouts=&setlayout;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&pr;ck(vkCreatePipelineLayout(dev,&pl,nullptr,&layout));VkSamplerCreateInfo sc{};sc.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;sc.magFilter=sc.minFilter=VK_FILTER_LINEAR;sc.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;sc.addressModeU=sc.addressModeV=sc.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;ck(vkCreateSampler(dev,&sc,nullptr,&sampler));}if(current!=fmt){if(front)vkDestroyPipeline(dev,front,nullptr);if(back)vkDestroyPipeline(dev,back,nullptr);front=back=VK_NULL_HANDLE;front=pipeline(fmt,false);back=pipeline(fmt,true);current=fmt;}}
void make_texture(Texture& t){auto w=uint32_t(t.pixels.w),h=uint32_t(t.pixels.h);VkImageCreateInfo i{};i.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;i.imageType=VK_IMAGE_TYPE_2D;i.format=VK_FORMAT_R8G8B8A8_UNORM;i.extent={w,h,1};i.mipLevels=i.arrayLayers=1;i.samples=VK_SAMPLE_COUNT_1_BIT;i.tiling=VK_IMAGE_TILING_OPTIMAL;i.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;ck(vkCreateImage(dev,&i,nullptr,&t.image));VkMemoryRequirements r;vkGetImageMemoryRequirements(dev,t.image,&r);t.mem=alloc(r,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);ck(vkBindImageMemory(dev,t.image,t.mem,0));t.view=view(t.image,i.format);VkBufferCreateInfo b{};b.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;b.size=VkDeviceSize(w)*h*4;b.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT;ck(vkCreateBuffer(dev,&b,nullptr,&t.upload));vkGetBufferMemoryRequirements(dev,t.upload,&r);t.host=alloc(r,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);ck(vkBindBufferMemory(dev,t.upload,t.host,0));ck(vkMapMemory(dev,t.host,0,VK_WHOLE_SIZE,0,&t.mapped));VkDescriptorSetAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;a.descriptorPool=descriptors;a.descriptorSetCount=1;a.pSetLayouts=&setlayout;ck(vkAllocateDescriptorSets(dev,&a,&t.set));VkDescriptorImageInfo di{sampler,t.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};VkWriteDescriptorSet wr{};wr.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;wr.dstSet=t.set;wr.descriptorCount=1;wr.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;wr.pImageInfo=&di;vkUpdateDescriptorSets(dev,1,&wr,0,nullptr);}
void barrier(VkImage img,VkImageLayout old,VkImageLayout next,VkAccessFlags from,VkAccessFlags to){VkImageMemoryBarrier b{};b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;b.oldLayout=old;b.newLayout=next;b.srcAccessMask=from;b.dstAccessMask=to;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=img;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);}
void update(Texture& t){size_t f=frame_at(t.pixels,std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-t.start).count());if(t.frame==f)return;auto& data=t.pixels.frames[f];std::memcpy(t.mapped,data.data(),data.size());barrier(t.image,t.frame==size_t(-1)?VK_IMAGE_LAYOUT_UNDEFINED:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,t.frame==size_t(-1)?0:VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={uint32_t(t.pixels.w),uint32_t(t.pixels.h),1};vkCmdCopyBufferToImage(cmd,t.upload,t.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);barrier(t.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);t.frame=f;}
void draw(const Item& i,unsigned w,unsigned h){auto it=cache.find(i.path);if(!i.visible||i.opacity<=0||it==cache.end())return;auto& t=it->second;Pose p;if(!item_pose(i,p))return;float aspect=float(w)/h;float pc[]={p.x,-p.y,p.size*float(t.pixels.w)/t.pixels.h/aspect,p.size,p.angle*.01745329252f,i.opacity,aspect,0};vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&t.set,0,nullptr);vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(pc),pc);vkCmdDraw(cmd,6,1,0,0);}
}
void begin_mesh_frame(){mesh_frame.model=nullptr;}

void capture_mesh_frame(const CubismModel& model,const float* projection,unsigned width,unsigned height){
    if(mesh_frame.model||!width||!height)return;
    if(mesh_frame.catalog_model!=&model){
        mesh_frame.catalog_model=&model;mesh_frame.ids.clear();mesh_frame.indices.clear();
        for(int i=0;i<model.GetDrawableCount();++i){
            const auto handle=model.GetDrawableId(i);
            if(!handle)continue;
            const std::string id=handle->GetString().GetRawString();
            if(clean_field(id,1024)&&mesh_frame.indices.emplace(id,i).second)mesh_frame.ids.push_back(id);
        }
        std::sort(mesh_frame.ids.begin(),mesh_frame.ids.end());
    }
    mesh_frame.model=&model;
    mesh_frame.aspect=float(width)/height;
    std::copy_n(projection,16,mesh_frame.projection.begin());
}
void forget_mesh_model(const CubismModel* model){
    if(mesh_frame.catalog_model==model)mesh_frame={};
}
const std::vector<std::string>& artmesh_ids(){
    static const std::vector<std::string> empty;
    return mesh_frame.model?mesh_frame.ids:empty;
}
bool mesh_available(const Item& item){
    AttachmentFrame frame;
    return item.mesh&&resolve_frame(item.mesh,frame);
}
bool attach_to_mesh(Item& item,const std::string& id){
    if(!mesh_frame.model||item.locked)return false;
    const auto mesh=mesh_frame.indices.find(id);
    if(mesh==mesh_frame.indices.end())return false;
    Pose visible{0,0,item.size,item.angle};
    const bool was_visible=item_pose(item,visible);
    Anchor anchor;
    if(!find_anchor(mesh->second,{visible.x*mesh_frame.aspect,visible.y},false,anchor))return false;
    if(!was_visible){
        Triangle triangle;AttachmentFrame frame;
        if(!Geometry(mesh->second).project(anchor.triangle,triangle)||!attachment_frame(triangle,anchor,frame))return false;
        visible.x=frame.origin.x/mesh_frame.aspect;visible.y=frame.origin.y;
    }
    return bind_mesh(item,id,anchor,visible);
}
bool pick_artmesh(Item& item,float canvas_x,float canvas_y){
    if(!mesh_frame.model||item.locked)return false;
    const auto& model=*mesh_frame.model;
    const int* orders=model.GetRenderOrders();
    if(!orders)return false;
    int best=-1;Anchor anchor;
    for(int index=0;index<model.GetDrawableCount();++index){
        if((best>=0&&orders[index]<=orders[best])||!model.GetDrawableDynamicFlagIsVisible(index)||
           model.GetDrawableOpacity(index)<=.001f)continue;
        Anchor candidate;
        if(find_anchor(index,{canvas_x*mesh_frame.aspect,canvas_y},true,candidate)){
            best=index;anchor=candidate;
        }
    }
    if(best<0)return false;
    const auto id=model.GetDrawableId(best);
    if(!id)return false;
    Pose visible{canvas_x,canvas_y,item.size,item.angle};
    item_pose(item,visible);
    return bind_mesh(item,id->GetString().GetRawString(),anchor,visible);
}
bool attach_to_root(Item& item,bool root){
    if(item.locked)return false;
    Pose visible{0,0,item.size,item.angle};
    item_pose(item,visible);
    Item next=item;
    next.mesh={};next.attached=false;
    next.x=visible.x;next.y=visible.y;next.size=visible.size;next.angle=std::remainder(visible.angle,360.f);
    float x,y;camera_offset(x,y);attach(next,root,camera_scale(),x,y);
    if(!valid(next))return false;
    item=std::move(next);
    return true;
}
bool move_item(Item& item,float dx,float dy){
    if(item.locked||(dx==0.f&&dy==0.f)||!finite({dx,dy}))return false;
    Point delta(dx,dy);
    if(item.mesh){
        AttachmentFrame frame;
        if(!resolve_frame(item.mesh,frame))return false;
        delta=frame.inverse({dx*mesh_frame.aspect,dy});
    }else if(item.attached)delta=delta*(1.f/camera_scale());
    if(!finite(delta))return false;
    item.x=std::max(-10000.f,std::min(10000.f,item.x+delta.x));
    item.y=std::max(-10000.f,std::min(10000.f,item.y+delta.y));
    return true;
}

void init(VkPhysicalDevice g,VkDevice d,VkQueue q,unsigned f){gpu=g;dev=d;queue=q;qfamily=f;disabled=false;}
void shutdown(){mesh_frame={};if(!dev)return;vkDeviceWaitIdle(dev);for(auto& x:cache)release(x.second);cache.clear();failures.clear();if(front)vkDestroyPipeline(dev,front,nullptr);if(back)vkDestroyPipeline(dev,back,nullptr);if(layout)vkDestroyPipelineLayout(dev,layout,nullptr);if(descriptors)vkDestroyDescriptorPool(dev,descriptors,nullptr);if(setlayout)vkDestroyDescriptorSetLayout(dev,setlayout,nullptr);if(sampler)vkDestroySampler(dev,sampler,nullptr);if(pool)vkDestroyCommandPool(dev,pool,nullptr);front=back=VK_NULL_HANDLE;layout=VK_NULL_HANDLE;descriptors=VK_NULL_HANDLE;setlayout=VK_NULL_HANDLE;sampler=VK_NULL_HANDLE;pool=VK_NULL_HANDLE;current=VK_FORMAT_UNDEFINED;dev=VK_NULL_HANDLE;}
void render(VkImage target,VkFormat format,unsigned w,unsigned h){if(!dev||disabled||!target||!w||!h)return;auto& s=scene();auto rows=s.rows;
const auto& model=mesh_model_path();
rows.erase(std::remove_if(rows.begin(),rows.end(),[&](const Item& item){return item.mesh&&item.mesh.model!=model;}),rows.end());
const auto now=parameters::Clock::now();
for(auto& i:rows)i=effective(i,editor_runtime(),now);
auto used=[&](const std::string& p){return std::any_of(rows.begin(),rows.end(),[&](const Item& i){return i.path==p;});};for(auto it=cache.begin();it!=cache.end();)if(!used(it->first)){release(it->second);it=cache.erase(it);}else ++it;for(auto it=failures.begin();it!=failures.end();)if(!used(it->first))it=failures.erase(it);else ++it;if(std::none_of(rows.begin(),rows.end(),[](const Item& i){return i.visible&&i.opacity>0;}))return;VkImageView targetview{};try{prepare(format);size_t bytes=0;for(auto& x:cache)bytes+=size_t(x.second.pixels.w)*x.second.pixels.h*4*x.second.pixels.frames.size();for(auto& i:rows){if(!i.visible||cache.count(i.path)||failures.count(i.path))continue;Texture t;std::string err;if(!load_pixels(i.path,t.pixels,err)){failures[i.path]=s.error=err;continue;}size_t n=size_t(t.pixels.w)*t.pixels.h*4*t.pixels.frames.size();if(bytes+n>256u*1024u*1024u){failures[i.path]=s.error="Scene image budget exceeded (256 MiB)";continue;}try{make_texture(t);}catch(...){release(t);throw;}cache.emplace(i.path,std::move(t));bytes+=n;}if(cache.empty())return;targetview=view(target,format);ck(vkResetCommandBuffer(cmd,0));VkCommandBufferBeginInfo bi{};bi.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;ck(vkBeginCommandBuffer(cmd,&bi));for(auto& x:cache)if(std::any_of(rows.begin(),rows.end(),[&](const Item& i){return i.path==x.first&&i.visible&&i.opacity>0;}))update(x.second);barrier(target,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);VkRenderingAttachmentInfoKHR a{};a.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;a.imageView=targetview;a.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;a.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;a.storeOp=VK_ATTACHMENT_STORE_OP_STORE;VkRenderingInfoKHR ri{};ri.sType=VK_STRUCTURE_TYPE_RENDERING_INFO_KHR;ri.renderArea.extent={w,h};ri.layerCount=1;ri.colorAttachmentCount=1;ri.pColorAttachments=&a;auto begin=reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(vkGetDeviceProcAddr(dev,"vkCmdBeginRenderingKHR"));auto end=reinterpret_cast<PFN_vkCmdEndRenderingKHR>(vkGetDeviceProcAddr(dev,"vkCmdEndRenderingKHR"));if(!begin||!end){begin=reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(vkGetDeviceProcAddr(dev,"vkCmdBeginRendering"));end=reinterpret_cast<PFN_vkCmdEndRenderingKHR>(vkGetDeviceProcAddr(dev,"vkCmdEndRendering"));}if(!begin||!end)throw std::runtime_error("Dynamic rendering unavailable");begin(cmd,&ri);VkViewport vp{0,0,float(w),float(h),0,1};VkRect2D sc{{0,0},{w,h}};vkCmdSetViewport(cmd,0,1,&vp);vkCmdSetScissor(cmd,0,1,&sc);vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,back);for(auto it=rows.rbegin();it!=rows.rend();++it)if(it->behind)draw(*it,w,h);vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,front);for(auto& i:rows)if(!i.behind)draw(i,w,h);end(cmd);barrier(target,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT);ck(vkEndCommandBuffer(cmd));VkSubmitInfo sub{};sub.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;sub.commandBufferCount=1;sub.pCommandBuffers=&cmd;ck(vkQueueSubmit(queue,1,&sub,VK_NULL_HANDLE));if(vkQueueWaitIdle(queue)!=VK_SUCCESS)std::abort();vkDestroyImageView(dev,targetview,nullptr);}catch(const std::exception& e){if(targetview)vkDestroyImageView(dev,targetview,nullptr);s.error=e.what();std::fprintf(stderr,"[items] %s\n",e.what());disabled=true;}}
}}
