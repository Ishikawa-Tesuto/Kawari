#pragma once
#include <vulkan/vulkan.h>
#include <string>
#include <vector>
namespace Live2D { namespace Cubism { namespace Framework { class CubismModel; } } }
namespace studio { namespace items {
struct Item;
void begin_mesh_frame();
void capture_mesh_frame(const Live2D::Cubism::Framework::CubismModel& model,const float* projection,unsigned width,unsigned height);
void forget_mesh_model(const Live2D::Cubism::Framework::CubismModel* model);
const std::vector<std::string>& artmesh_ids();
bool mesh_available(const Item& item);
bool attach_to_mesh(Item& item,const std::string& id);
bool pick_artmesh(Item& item,float canvas_x,float canvas_y);
bool attach_to_root(Item& item,bool root);
bool move_item(Item& item,float dx,float dy);
void init(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,unsigned family);
void shutdown();
void render(VkImage target,VkFormat format,unsigned w,unsigned h);
}}
