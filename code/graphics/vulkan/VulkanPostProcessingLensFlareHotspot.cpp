#include "VulkanPostProcessing.h"

#include <array>
#include <cstddef>
#include <cstring>

#include "gr_vulkan.h"
#include "VulkanRenderer.h"
#include "VulkanShader.h"
#include "VulkanShaderCompiler.h"
#include "VulkanDescriptorManager.h"
#include "VulkanBarrier.h"
#include "VulkanHotspotFlareTypes.h"
#include "graphics/2d.h"
#include "graphics/lens_flare.h"

// ===== GPU-driven "hotspot" lens flare: detection (compute) + draw (indirect) =====
//
// See the class comment on VulkanHotspotFlare in VulkanPostProcessing.h for the
// overall design. This file owns everything Vulkan-specific to that design:
// the bespoke compute pipeline/descriptor-set layout (nothing in the shared
// 3-tier graphics layout exposes a writable SSBO/indirect buffer from a
// compute stage), the per-frame-in-flight results/indirect-args/exclusion
// buffers, and the compute dispatch + indirect draw themselves.

namespace graphics::vulkan {

namespace {
// Must match local_size_x/local_size_y in lensflare-hotspot-detect.sdr.
constexpr uint32_t COMPUTE_TILE_SIZE = 16;
} // namespace

bool VulkanHotspotFlare::init(PostProcessContext& ctx, const RenderTarget& sceneColor)
{
	m_ctx = &ctx;
	m_sceneColor = &sceneColor;

	if (!createComputeDescriptorSetLayout()) {
		shutdown();
		return false;
	}
	if (!createComputePipeline()) {
		shutdown();
		return false;
	}
	if (!createPerFrameData()) {
		shutdown();
		return false;
	}

	m_initialized = true;
	nprintf(("vulkan", "VulkanHotspotFlare: Initialized\n"));
	return true;
}

bool VulkanHotspotFlare::createComputeDescriptorSetLayout()
{
	// Set 0, matching lensflare-hotspot-detect.sdr's bindings exactly:
	//   0: scene color (combined image sampler, point-fetched via texelFetch)
	//   1: HotspotResults (read-write SSBO)
	//   2: HotspotIndirectArgs (read-write SSBO, also the indirect-draw source)
	//   3: HotspotExclusionData (read-only UBO, CPU-rewritten every frame)
	std::array<vk::DescriptorSetLayoutBinding, 4> bindings;
	bindings[0].binding = 0;
	bindings[0].descriptorType = vk::DescriptorType::eCombinedImageSampler;
	bindings[0].descriptorCount = 1;
	bindings[0].stageFlags = vk::ShaderStageFlagBits::eCompute;
	bindings[1].binding = 1;
	bindings[1].descriptorType = vk::DescriptorType::eStorageBuffer;
	bindings[1].descriptorCount = 1;
	bindings[1].stageFlags = vk::ShaderStageFlagBits::eCompute;
	bindings[2].binding = 2;
	bindings[2].descriptorType = vk::DescriptorType::eStorageBuffer;
	bindings[2].descriptorCount = 1;
	bindings[2].stageFlags = vk::ShaderStageFlagBits::eCompute;
	bindings[3].binding = 3;
	bindings[3].descriptorType = vk::DescriptorType::eUniformBuffer;
	bindings[3].descriptorCount = 1;
	bindings[3].stageFlags = vk::ShaderStageFlagBits::eCompute;

	vk::DescriptorSetLayoutCreateInfo layoutInfo;
	layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
	layoutInfo.pBindings = bindings.data();

	try {
		m_computeSetLayout = m_ctx->device.createDescriptorSetLayoutUnique(layoutInfo);
	} catch (const vk::SystemError& e) {
		nprintf(("vulkan", "VulkanHotspotFlare: Failed to create descriptor set layout: %s\n", e.what()));
		return false;
	}

	// Just enough for one set per frame-in-flight -- unlike the shared
	// per-frame descriptor pools (VulkanDescriptorManager), this pool never
	// needs to grow: the compute set count is fixed at MAX_FRAMES_IN_FLIGHT
	// for the life of this subsystem.
	std::array<vk::DescriptorPoolSize, 3> poolSizes = {
		vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler, MAX_FRAMES_IN_FLIGHT),
		vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer, MAX_FRAMES_IN_FLIGHT * 2),
		vk::DescriptorPoolSize(vk::DescriptorType::eUniformBuffer, MAX_FRAMES_IN_FLIGHT),
	};
	vk::DescriptorPoolCreateInfo poolInfo;
	poolInfo.maxSets = MAX_FRAMES_IN_FLIGHT;
	poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
	poolInfo.pPoolSizes = poolSizes.data();

	try {
		m_computeDescriptorPool = m_ctx->device.createDescriptorPoolUnique(poolInfo);
	} catch (const vk::SystemError& e) {
		nprintf(("vulkan", "VulkanHotspotFlare: Failed to create descriptor pool: %s\n", e.what()));
		return false;
	}

	std::array<vk::DescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts;
	layouts.fill(m_computeSetLayout.get());

	vk::DescriptorSetAllocateInfo allocInfo;
	allocInfo.descriptorPool = m_computeDescriptorPool.get();
	allocInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
	allocInfo.pSetLayouts = layouts.data();

	try {
		auto sets = m_ctx->device.allocateDescriptorSets(allocInfo);
		for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
			m_perFrame[i].computeSet = sets[i];
		}
	} catch (const vk::SystemError& e) {
		nprintf(("vulkan", "VulkanHotspotFlare: Failed to allocate descriptor sets: %s\n", e.what()));
		return false;
	}

	return true;
}

bool VulkanHotspotFlare::createComputePipeline()
{
	auto* shaderManager = getShaderManager();
	auto* compiler = shaderManager ? shaderManager->getCompiler() : nullptr;
	if (compiler == nullptr) {
		nprintf(("vulkan", "VulkanHotspotFlare: No shader compiler available\n"));
		return false;
	}

	// Off the shared SHADER_TYPES table (SDR_TYPE_NONE): this is the only
	// compute shader in the engine, and unlike the graphics shaders, nothing
	// about variant flags or OpenGL applies to it.
	auto spirv = compiler->compile("lensflare-hotspot-detect.sdr", vk::ShaderStageFlagBits::eCompute,
		SDR_TYPE_NONE, 0, false);
	if (spirv.empty()) {
		nprintf(("vulkan", "VulkanHotspotFlare: Failed to compile detection compute shader\n"));
		return false;
	}

	vk::ShaderModuleCreateInfo moduleInfo;
	moduleInfo.codeSize = spirv.size() * sizeof(uint32_t);
	moduleInfo.pCode = spirv.data();
	try {
		m_computeShaderModule = m_ctx->device.createShaderModuleUnique(moduleInfo);
	} catch (const vk::SystemError& e) {
		nprintf(("vulkan", "VulkanHotspotFlare: Failed to create shader module: %s\n", e.what()));
		return false;
	}

	// Bespoke pipeline layout (a single set + a small push-constant range) --
	// unlike the graphics draw, nothing here rides the shared 3-tier layout.
	vk::PushConstantRange pcRange;
	pcRange.stageFlags = vk::ShaderStageFlagBits::eCompute;
	pcRange.offset = 0;
	pcRange.size = sizeof(HotspotDetectPushConstants);

	vk::DescriptorSetLayout setLayout = m_computeSetLayout.get();
	vk::PipelineLayoutCreateInfo layoutInfo;
	layoutInfo.setLayoutCount = 1;
	layoutInfo.pSetLayouts = &setLayout;
	layoutInfo.pushConstantRangeCount = 1;
	layoutInfo.pPushConstantRanges = &pcRange;

	try {
		m_computePipelineLayout = m_ctx->device.createPipelineLayoutUnique(layoutInfo);
	} catch (const vk::SystemError& e) {
		nprintf(("vulkan", "VulkanHotspotFlare: Failed to create pipeline layout: %s\n", e.what()));
		return false;
	}

	vk::PipelineShaderStageCreateInfo stageInfo;
	stageInfo.stage = vk::ShaderStageFlagBits::eCompute;
	stageInfo.module = m_computeShaderModule.get();
	stageInfo.pName = "main";

	vk::ComputePipelineCreateInfo pipelineInfo;
	pipelineInfo.stage = stageInfo;
	pipelineInfo.layout = m_computePipelineLayout.get();

	try {
		auto result = m_ctx->device.createComputePipelineUnique(nullptr, pipelineInfo);
		m_computePipeline = std::move(result.value);
	} catch (const vk::SystemError& e) {
		nprintf(("vulkan", "VulkanHotspotFlare: Failed to create compute pipeline: %s\n", e.what()));
		return false;
	}

	return true;
}

bool VulkanHotspotFlare::createBuffer(vk::DeviceSize size, vk::BufferUsageFlags usage, MemoryUsage memUsage,
	BufferAlloc& out)
{
	vk::BufferCreateInfo bufferInfo;
	bufferInfo.size = size;
	bufferInfo.usage = usage;
	bufferInfo.sharingMode = vk::SharingMode::eExclusive;

	try {
		out.buffer = m_ctx->device.createBuffer(bufferInfo);
	} catch (const vk::SystemError& e) {
		nprintf(("vulkan", "VulkanHotspotFlare: Failed to create buffer: %s\n", e.what()));
		return false;
	}

	if (!m_ctx->memoryManager->allocateBufferMemory(out.buffer, memUsage, out.allocation)) {
		m_ctx->device.destroyBuffer(out.buffer);
		out.buffer = nullptr;
		nprintf(("vulkan", "VulkanHotspotFlare: Failed to allocate buffer memory\n"));
		return false;
	}

	return true;
}

bool VulkanHotspotFlare::createPerFrameData()
{
	// Host-visible (not device-local): the same choice VulkanDraw.cpp's
	// g_transformBuffers already makes for a compute/vertex-visible SSBO --
	// host-visible memory is still ordinary GPU-accessible memory for shader
	// atomics/reads/writes, and it is what lets the CPU rewrite the exclusion
	// list (and re-zero the counters) every frame via a plain mapMemory,
	// without a staging buffer or a transfer-stage barrier.
	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
		PerFrameData& pf = m_perFrame[i];

		if (!createBuffer(sizeof(HotspotResults), vk::BufferUsageFlagBits::eStorageBuffer,
				MemoryUsage::CpuToGpu, pf.results)) {
			return false;
		}
		if (!createBuffer(sizeof(HotspotIndirectArgs),
				vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndirectBuffer,
				MemoryUsage::CpuToGpu, pf.indirectArgs)) {
			return false;
		}
		if (!createBuffer(sizeof(HotspotExclusionData), vk::BufferUsageFlagBits::eUniformBuffer,
				MemoryUsage::CpuToGpu, pf.exclusion)) {
			return false;
		}

		// vertexCount/firstVertex/firstInstance never change again after this;
		// only instanceCount is touched again, every dispatch().
		{
			HotspotIndirectArgs initialArgs;
			void* mapped = m_ctx->memoryManager->mapMemory(pf.indirectArgs.allocation);
			if (mapped == nullptr) {
				nprintf(("vulkan", "VulkanHotspotFlare: Failed to map indirect-args buffer\n"));
				return false;
			}
			memcpy(mapped, &initialArgs, sizeof(initialArgs));
			m_ctx->memoryManager->flushMemory(pf.indirectArgs.allocation, 0, sizeof(initialArgs));
			m_ctx->memoryManager->unmapMemory(pf.indirectArgs.allocation);
		}

		// The other bindings (results/indirectArgs/exclusion buffers) never
		// change their underlying vk::Buffer after this point, so they are
		// written once here; only the scene-color image binding is rewritten
		// every dispatch() (its view changes on resize).
		vk::DescriptorBufferInfo resultsInfo(pf.results.buffer, 0, sizeof(HotspotResults));
		vk::DescriptorBufferInfo indirectInfo(pf.indirectArgs.buffer, 0, sizeof(HotspotIndirectArgs));
		vk::DescriptorBufferInfo exclusionInfo(pf.exclusion.buffer, 0, sizeof(HotspotExclusionData));

		std::array<vk::WriteDescriptorSet, 3> writes;
		writes[0].dstSet = pf.computeSet;
		writes[0].dstBinding = 1;
		writes[0].descriptorCount = 1;
		writes[0].descriptorType = vk::DescriptorType::eStorageBuffer;
		writes[0].pBufferInfo = &resultsInfo;
		writes[1].dstSet = pf.computeSet;
		writes[1].dstBinding = 2;
		writes[1].descriptorCount = 1;
		writes[1].descriptorType = vk::DescriptorType::eStorageBuffer;
		writes[1].pBufferInfo = &indirectInfo;
		writes[2].dstSet = pf.computeSet;
		writes[2].dstBinding = 3;
		writes[2].descriptorCount = 1;
		writes[2].descriptorType = vk::DescriptorType::eUniformBuffer;
		writes[2].pBufferInfo = &exclusionInfo;

		m_ctx->device.updateDescriptorSets(static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
	}

	return true;
}

void VulkanHotspotFlare::destroyPerFrameData()
{
	if (m_ctx == nullptr || m_ctx->memoryManager == nullptr) {
		return;
	}

	for (auto& pf : m_perFrame) {
		auto release = [&](BufferAlloc& b) {
			if (b.buffer) {
				m_ctx->device.destroyBuffer(b.buffer);
				m_ctx->memoryManager->freeAllocation(b.allocation);
				b.buffer = nullptr;
				b.allocation = {};
			}
		};
		release(pf.results);
		release(pf.indirectArgs);
		release(pf.exclusion);
		pf.computeSet = nullptr; // freed with the pool in shutdown()
	}
}

void VulkanHotspotFlare::dispatch(vk::CommandBuffer cmd, uint32_t frameIndex)
{
	if (!m_initialized || !graphics::lens_flare_frame_hotspot_active()) {
		return;
	}

	PerFrameData& pf = m_perFrame[frameIndex];
	const graphics::lens_flare_tuning& tuning = graphics::lens_flare_get_tuning();
	auto* memManager = m_ctx->memoryManager;

	// Zero this frame's atomic counters. No GPU-side barrier needed: like
	// g_transformBuffers in VulkanDraw.cpp, this frame-in-flight slot's
	// previous use is already complete by the time its frame index comes
	// back around (gated by the frame-in-flight fence wait elsewhere in the
	// renderer), so a plain host write here happens-before anything the GPU
	// does with this buffer this frame -- Vulkan's host-write-visibility
	// rules only require the write to precede submission, not a device
	// barrier, and flushMemory() below covers non-coherent memory types.
	{
		uint32_t zero = 0;
		void* mapped = memManager->mapMemory(pf.results.allocation);
		if (mapped) {
			memcpy(mapped, &zero, sizeof(zero));
			memManager->flushMemory(pf.results.allocation, 0, sizeof(zero));
			memManager->unmapMemory(pf.results.allocation);
		}
	}
	{
		uint32_t zero = 0;
		void* mapped = memManager->mapMemory(pf.indirectArgs.allocation);
		if (mapped) {
			memcpy(static_cast<char*>(mapped) + offsetof(HotspotIndirectArgs, instanceCount), &zero, sizeof(zero));
			memManager->flushMemory(pf.indirectArgs.allocation, 0, sizeof(HotspotIndirectArgs));
			memManager->unmapMemory(pf.indirectArgs.allocation);
		}
	}

	// Rebuild this frame's exclusion list from the tracked sources
	// lens_flare_frame_update() already published -- a one-way CPU->GPU
	// write, not a readback. Once hotspots are active, thruster/beam
	// gathering is suppressed upstream (see lens_flare_frame_update()), so
	// this only ever contains suns.
	{
		HotspotExclusionData exclusionData;
		for (const auto& draw : graphics::lens_flare_get_frame_draws()) {
			if (exclusionData.count >= MAX_HOTSPOT_EXCLUSIONS) {
				break;
			}
			HotspotExclusion& ex = exclusionData.exclusions[exclusionData.count];
			ex.ndc_x = draw.source_ndc.x;
			ex.ndc_y = draw.source_ndc.y;
			ex.radius_ndc = tuning.hotspot_exclusion_radius_ndc;
			exclusionData.count++;
		}

		void* mapped = memManager->mapMemory(pf.exclusion.allocation);
		if (mapped) {
			memcpy(mapped, &exclusionData, sizeof(exclusionData));
			memManager->flushMemory(pf.exclusion.allocation, 0, sizeof(exclusionData));
			memManager->unmapMemory(pf.exclusion.allocation);
		}
	}

	// Scene color's view can change on resize; every other binding is fixed
	// for this frame-in-flight slot's lifetime (written once in
	// createPerFrameData()), so only this one needs rewriting here.
	{
		vk::DescriptorImageInfo imageInfo(m_ctx->linearSampler, m_sceneColor->view,
			vk::ImageLayout::eShaderReadOnlyOptimal);
		vk::WriteDescriptorSet write;
		write.dstSet = pf.computeSet;
		write.dstBinding = 0;
		write.descriptorCount = 1;
		write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
		write.pImageInfo = &imageInfo;
		m_ctx->device.updateDescriptorSets(1, &write, 0, nullptr);
	}

	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_computePipeline.get());
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_computePipelineLayout.get(), 0, {pf.computeSet}, {});

	HotspotDetectPushConstants pc;
	pc.sceneWidth = m_ctx->sceneExtent.width;
	pc.sceneHeight = m_ctx->sceneExtent.height;
	pc.threshold = tuning.hotspot_threshold;
	cmd.pushConstants(m_computePipelineLayout.get(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(pc), &pc);

	const uint32_t halfWidth = (m_ctx->sceneExtent.width + 1) / 2;
	const uint32_t halfHeight = (m_ctx->sceneExtent.height + 1) / 2;
	const uint32_t groupsX = (halfWidth + COMPUTE_TILE_SIZE - 1) / COMPUTE_TILE_SIZE;
	const uint32_t groupsY = (halfHeight + COMPUTE_TILE_SIZE - 1) / COMPUTE_TILE_SIZE;
	cmd.dispatch(groupsX, groupsY, 1);

	// Hand off compute writes to the vertex-shader read (results, via
	// recordDraw()'s Material-set binding) and the fixed-function indirect-
	// draw read (instanceCount), both inside VulkanLensFlare's render pass
	// that follows this dispatch.
	cmdMemoryBarrier(cmd,
		vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
		vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eDrawIndirect,
		vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eIndirectCommandRead);
}

void VulkanHotspotFlare::recordDraw(vk::CommandBuffer cmd, uint32_t frameIndex, vk::Pipeline pipeline,
	vk::PipelineLayout pipelineLayout, vk::DescriptorSet materialSet, vk::DescriptorSet perDrawSet)
{
	if (!m_initialized || !pipeline || !graphics::lens_flare_frame_hotspot_active()) {
		return;
	}

	cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipelineLayout,
		static_cast<uint32_t>(DescriptorSetIndex::Material), {materialSet, perDrawSet}, {});

	const PerFrameData& pf = m_perFrame[frameIndex];
	cmd.drawIndirect(pf.indirectArgs.buffer, 0, 1, sizeof(HotspotIndirectArgs));
}

void VulkanHotspotFlare::shutdown()
{
	if (m_ctx == nullptr) {
		return;
	}

	// Called with the device idle (VulkanPostProcessor::shutdown waits, and
	// resize() callers do too), so buffers/pipeline objects are destroyed
	// immediately rather than queued.
	destroyPerFrameData();

	m_computePipeline.reset();
	m_computePipelineLayout.reset();
	m_computeShaderModule.reset();
	m_computeDescriptorPool.reset(); // frees every computeSet allocated from it
	m_computeSetLayout.reset();

	m_initialized = false;
}

} // namespace graphics::vulkan
