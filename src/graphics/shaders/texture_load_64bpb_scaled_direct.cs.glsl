#version 460
#extension GL_EXT_control_flow_attributes : require
#extension GL_EXT_samplerless_texture_functions : require
#extension GL_GOOGLE_include_directive : require

// vulkan_texture_load_direct, 64bpb: see texture_load_scaled_direct.glsli.
#define XE_TEXTURE_LOAD_DIRECT_64BPB
#include "texture_load_scaled_direct.glsli"
