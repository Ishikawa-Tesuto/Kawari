#pragma once
#include <Model/CubismModel.hpp>
#include <GLFW/glfw3.h>
#include "studio_parameters.hpp"

namespace studio {
// Runs on the renderer thread before the physics updater.
void update_controls(Live2D::Cubism::Framework::CubismModel* model, GLFWwindow* window);
void reset_controls();
void apply_manual_overrides(Live2D::Cubism::Framework::CubismModel* model);
void pace_frame();
void apply_live_broadcast_settings(int fps,bool show_stats);
void begin_frame();
}

namespace studio {
void update_view(GLFWwindow* window);
void apply_camera(float* projection);
bool camera_is_default();
float camera_scale();
float camera_base_scale();
void camera_offset(float& x,float& y);
void camera_zoom(float factor);
void camera_zoom_at(float factor,float x,float y);
void camera_pan(float x,float y);
void camera_scroll(float delta,float x,float y);
void camera_advance(float seconds);
void camera_reset();
void camera_save();
void cycle_background();
bool demo_visible();
bool offscreen();
float background();
int window_width(int fallback);
int window_height(int fallback);
}

namespace studio {
// Editor access is restricted to the same renderer/UI thread.
const parameters::Runtime& editor_runtime();
const std::vector<float>& editor_values();
bool editor_apply_mapping(const parameters::Mapping&,std::string& error);
bool editor_can_save();
bool editor_dirty();
bool editor_save(std::string& error);
void editor_preview(size_t target,float value);
void editor_release();
bool expression_preview(const std::string& packet);
void expression_preview_clear();
}

namespace studio {
// All model-switch state belongs to the renderer thread.
struct ModelSelection { std::string original,runtime,mapping,view; bool editable=false; };
bool hot_switch_enabled();
bool take_model_selection(ModelSelection& selection);
void commit_model_selection(const ModelSelection& selection);
const std::string& active_original_model();
std::string expression_ui_directory();
const std::string& active_runtime_model();
std::string physics_profile_path();
std::string animation_profile_path();
}
