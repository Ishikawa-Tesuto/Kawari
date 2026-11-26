#pragma once
struct GLFWwindow {bool closed=false;};
enum {GLFW_KEY_UP, GLFW_KEY_DOWN, GLFW_KEY_LEFT, GLFW_KEY_RIGHT, GLFW_KEY_R, GLFW_KEY_P, GLFW_KEY_ESCAPE, GLFW_KEY_EQUAL, GLFW_KEY_MINUS, GLFW_KEY_HOME, GLFW_KEY_B, GLFW_KEY_F2, GLFW_KEY_F3, GLFW_KEY_W, GLFW_KEY_A, GLFW_KEY_S, GLFW_KEY_D};
#define GLFW_PRESS 1
#define GLFW_TRUE 1
extern int test_key;
inline int glfwGetKey(GLFWwindow*, int k) {return k==test_key?1:0;}
inline void glfwSetWindowTitle(GLFWwindow*,const char*) {}
inline void glfwSetWindowShouldClose(GLFWwindow* w,int x) {w->closed=x;}
