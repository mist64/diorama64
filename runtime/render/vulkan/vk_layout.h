#pragma once
#include <cstdint>
// The Vulkan pipeline layout shared by glsl_emitter.cpp (its layout(set=, binding=) qualifiers) and the device: descriptor
// sets, bindings, and the vertex layout (the same 32-byte vertex as the Metal vertex descriptor).
namespace ar {
enum : uint32_t { kSetUniforms = 0, kSetTextures = 1 };
// set 0 (kSetUniforms)
enum : uint32_t {
    kBindView = 0,      // ViewU, uniform buffer, dynamic offset (160 bytes)
    kBindDraw = 1,      // DrawU, uniform buffer, dynamic offset (80 bytes)
    kBindLight = 2,     // LightU, uniform buffer (704 bytes)
    kBindNormals = 3,   // storage buffer: one int per vertex, four signed bytes (x, y, z, pad) little-endian, by vertex index
    kBindPrevPos = 4,   // storage buffer: three floats per vertex (the previous tick's position), by vertex index
};
// set 1 (kSetTextures): combined image samplers
enum : uint32_t {
    kBindTex0 = 0,       // sampler2D
    kBindTex1 = 1,       // sampler2D
    kBindShadow = 2,     // sampler2DShadow: the key light's shadow map (compare sampler)
    kBindUnderside = 3,  // sampler2D: the underside depth map, read as raw depth (Metal: depth2d sample without compare)
    kBindAo = 4,         // sampler3D
    kBindEnv = 5,        // samplerCube
    kBindEnvOut = 6,     // samplerCube: the outgoing probe cube
    kBindWaterFoot = 7,  // sampler2D: the water footprint depth map, read by texel (Metal: depth2d read)
};
// Vertex layout: binding 0, stride 32. Formats are VkFormat values (no vulkan.h here; the device static_asserts them).
const uint32_t kVertexStride = 32;
const uint32_t kVertexAttribs = 4;
const uint32_t kVertexFormatR32G32B32Sfloat = 106;   // VK_FORMAT_R32G32B32_SFLOAT
const uint32_t kVertexFormatR32G32Sfloat = 103;      // VK_FORMAT_R32G32_SFLOAT
const uint32_t kVertexFormatR8G8B8A8Unorm = 37;      // VK_FORMAT_R8G8B8A8_UNORM
// location 0 pos, 1 uv0, 2 uv1, 3 shade
const uint32_t kVertexFormats[kVertexAttribs] = {kVertexFormatR32G32B32Sfloat, kVertexFormatR32G32Sfloat, kVertexFormatR32G32Sfloat,
                                                 kVertexFormatR8G8B8A8Unorm};
const uint32_t kVertexOffsets[kVertexAttribs] = {0, 12, 20, 28};
}
