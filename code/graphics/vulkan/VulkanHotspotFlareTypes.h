#pragma once

#include "globalincs/pstypes.h"

#include <cstdint>

namespace graphics::vulkan {

// Vulkan-only GPU-driven "hotspot" lens flare types. No OpenGL mirror exists
// (the OpenGL backend has no compute-shader support to run this pipeline at
// all), so these live here rather than in graphics/util/uniform_structs.h,
// which is shared by both backends.

// Upper bound on hotspots detected in a single frame. Thruster/beam glints no
// longer draw as tracked sources once this pipeline is active (see
// lens_flare_frame_update()), so they -- along with any incidental specular
// highlight -- all compete for this same budget. May need to grow if a busy
// fleet-battle scene shows nozzles/beams getting dropped rather than flared;
// see the "Open judgement calls" note in the design doc this implements.
constexpr uint32_t MAX_HOTSPOTS = 24;

// Upper bound on tracked-source screen positions excluded from detection each
// frame (Phase D). Once thruster/beam gathering is suppressed while hotspots
// are active, only suns ever populate this list, so it is sized for a
// generous sun count rather than the old thruster/beam budgets.
constexpr uint32_t MAX_HOTSPOT_EXCLUSIONS = 16;

/**
 * @brief One hotspot candidate written by the detection compute shader
 *
 * 16 bytes so it matches std430's vec4-equivalent array stride/alignment,
 * keeping the layout simple to mirror in GLSL.
 */
struct HotspotCandidate {
	float ndc_x = 0.0f;
	float ndc_y = 0.0f;
	float luminance = 0.0f;
	float pad = 0.0f;
};

/**
 * @brief Compute-written results buffer: an atomic counter + fixed candidate array
 *
 * std430 layout: a bare `uint` counter has 4-byte alignment, but the
 * HotspotCandidate array that follows has 16-byte alignment (its base
 * alignment is that of its largest/only member's rounding, vec4-equivalent),
 * so the counter is padded out to 16 bytes here. That means `candidates[0]`
 * sits at byte offset 16 -- one HotspotCandidate-sized "slot" after the
 * struct's start. The hotspot vertex shader's gl_InstanceIndex indexes
 * `candidates[]` directly (index 0 is the first hotspot), NOT into the raw
 * buffer as if it started at the counter -- this header offset is already
 * accounted for by the GLSL struct declaration matching this one field for
 * field, not something the shader needs to add manually. Recorded here so a
 * future change to either side (this struct or the .sdr's mirror) is caught
 * by mismatched layouts rather than silently reading the wrong hotspot.
 */
struct HotspotResults {
	uint32_t count = 0;
	uint32_t pad[3] = {0, 0, 0};
	HotspotCandidate candidates[MAX_HOTSPOTS];
};

/**
 * @brief Field-for-field VkDrawIndirectCommand layout
 *
 * Written by the detection compute shader (instanceCount = clamp(count,
 * MAX_HOTSPOTS)) and consumed directly by cmd.drawIndirect() -- no CPU
 * readback in between.
 */
struct HotspotIndirectArgs {
	uint32_t vertexCount = 4;   // quad, drawn as a tristrip like the tracked-source pass
	uint32_t instanceCount = 0; // GPU-written: number of hotspots detected this frame
	uint32_t firstVertex = 0;
	uint32_t firstInstance = 0;
};

/**
 * @brief One tracked-source screen position the detector should ignore (Phase D)
 */
struct HotspotExclusion {
	float ndc_x = 0.0f;
	float ndc_y = 0.0f;
	float radius_ndc = 0.0f;
	float pad = 0.0f;
};

/**
 * @brief CPU->GPU one-way exclusion list, rebuilt every frame from this frame's
 * tracked (sun) draws
 *
 * Not a readback: the CPU already knows every tracked source's screen
 * position (lens_flare_draw::source_ndc), so this is a small upload, not a
 * round trip through GPU-written data.
 */
struct HotspotExclusionData {
	uint32_t count = 0;
	uint32_t pad[3] = {0, 0, 0};
	HotspotExclusion exclusions[MAX_HOTSPOT_EXCLUSIONS];
};

/**
 * @brief Per-draw constants for the hotspot starburst quad
 *
 * Written into VulkanLensFlare's existing PerDraw UBO ring slot (set 2
 * binding 0, the same binding the tracked-source pass uses for its much
 * larger lens_flare_data block) rather than a push constant -- a push
 * constant range would have to be added to the shared 3-tier pipeline
 * layout, which this pass otherwise rides unchanged. 16 bytes: four plain
 * floats in sequence need no std140 padding, since each is already aligned
 * to its own 4-byte size and the block as a whole is exactly one 16-byte
 * alignment unit.
 */
struct HotspotDrawParams {
	float quadRadiusNdc = 0.0f;    // fixed on-screen quad radius, "small glint, big flash"
	float threshold = 0.0f;        // must match this frame's detection threshold
	float scale = 0.0f;            // multiplies the log2-stops-over-threshold curve
	float maxApparentRatio = 0.0f; // shared ceiling, reused from lens_flare_internal.h
};

/**
 * @brief Push constants for the detection compute shader
 *
 * The compute pipeline layout is bespoke (see VulkanHotspotFlare), unlike the
 * graphics draw's shared 3-tier layout, so a push constant range costs
 * nothing extra to add here.
 */
struct HotspotDetectPushConstants {
	uint32_t sceneWidth = 0;
	uint32_t sceneHeight = 0;
	float threshold = 0.0f;
};

} // namespace graphics::vulkan
