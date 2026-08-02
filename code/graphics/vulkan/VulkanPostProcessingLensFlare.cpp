#include "VulkanPostProcessing.h"

#include <array>

#include "gr_vulkan.h"
#include "VulkanRenderer.h"
#include "VulkanPipeline.h"
#include "VulkanDescriptorManager.h"
#include "VulkanTexture.h"
#include "VulkanDeletionQueue.h"
#include "VulkanBarrier.h"
#include "VulkanHotspotFlareTypes.h"
#include "graphics/2d.h"
#include "graphics/grinternal.h"
#include "graphics/lens_flare.h"
#include "graphics/util/uniform_structs.h"

namespace graphics::vulkan {

// ===== Physically-based lens flare pass =====

namespace {
// One UBO slot per flare source per scene render. Sun counts are single-digit,
// but every lit nozzle is also a source, capped at MAX_THRUSTER_SOURCES (32) in
// lens_flare.cpp -- so this has to hold that plus the suns, several times over
// for a frame that renders the scene more than once. At ~5 KB a slot that is
// still under a megabyte per frame in flight. The draw loop bails out rather
// than overflowing the ring if a frame ever exceeds it anyway.
constexpr uint32_t LENS_FLARE_UBO_SLOTS = 128;
} // namespace

bool VulkanLensFlare::init(PostProcessContext& ctx, const RenderTarget& sceneColor)
{
	m_ctx = &ctx;
	m_sceneColor = &sceneColor;

	// Additive render pass on the scene color: identical shape to the bloom
	// composite pass (loadOp=eLoad, ends in eShaderReadOnlyOptimal so the
	// following bloom bright pass can sample the scene as usual)
	{
		vk::AttachmentDescription att;
		att.format = HDR_COLOR_FORMAT;
		att.samples = vk::SampleCountFlagBits::e1;
		att.loadOp = vk::AttachmentLoadOp::eLoad;
		att.storeOp = vk::AttachmentStoreOp::eStore;
		att.stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
		att.stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
		att.initialLayout = vk::ImageLayout::eColorAttachmentOptimal;
		att.finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

		vk::AttachmentReference colorRef;
		colorRef.attachment = 0;
		colorRef.layout = vk::ImageLayout::eColorAttachmentOptimal;

		vk::SubpassDescription subpass;
		subpass.pipelineBindPoint = vk::PipelineBindPoint::eGraphics;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &colorRef;

		vk::SubpassDependency dep;
		dep.srcSubpass = VK_SUBPASS_EXTERNAL;
		dep.dstSubpass = 0;
		dep.srcStageMask = vk::PipelineStageFlagBits::eFragmentShader
		                  | vk::PipelineStageFlagBits::eColorAttachmentOutput;
		dep.dstStageMask = vk::PipelineStageFlagBits::eFragmentShader
		                  | vk::PipelineStageFlagBits::eColorAttachmentOutput;
		dep.srcAccessMask = vk::AccessFlagBits::eShaderRead
		                  | vk::AccessFlagBits::eColorAttachmentWrite;
		dep.dstAccessMask = vk::AccessFlagBits::eColorAttachmentRead
		                  | vk::AccessFlagBits::eColorAttachmentWrite;

		vk::RenderPassCreateInfo rpInfo;
		rpInfo.attachmentCount = 1;
		rpInfo.pAttachments = &att;
		rpInfo.subpassCount = 1;
		rpInfo.pSubpasses = &subpass;
		rpInfo.dependencyCount = 1;
		rpInfo.pDependencies = &dep;

		try {
			m_renderPass = m_ctx->device.createRenderPass(rpInfo);
		} catch (const vk::SystemError& e) {
			nprintf(("vulkan", "VulkanLensFlare: Failed to create render pass: %s\n", e.what()));
			return false;
		}
	}

	if (!createFramebuffer()) {
		return false;
	}

	// Dedicated per-frame UBO ring: lens_flare_data exceeds the shared scratch
	// ring's slot size. One slot per visible sun, with room for the scene being
	// rendered more than once per frame.
	vk::DeviceSize slotSize = (sizeof(generic_data::lens_flare_data) + 255) & ~static_cast<vk::DeviceSize>(255);
	if (!m_ubo.init(m_ctx->device, m_ctx->memoryManager, LENS_FLARE_UBO_SLOTS, slotSize)) {
		nprintf(("vulkan", "VulkanLensFlare: Failed to create UBO ring!\n"));
		shutdown();
		return false;
	}

	m_initialized = true;
	nprintf(("vulkan", "VulkanLensFlare: Initialized\n"));
	return true;
}

bool VulkanLensFlare::createFramebuffer()
{
	vk::FramebufferCreateInfo fbInfo;
	fbInfo.renderPass = m_renderPass;
	fbInfo.attachmentCount = 1;
	fbInfo.pAttachments = &m_sceneColor->view;
	fbInfo.width = m_ctx->sceneExtent.width;
	fbInfo.height = m_ctx->sceneExtent.height;
	fbInfo.layers = 1;

	try {
		m_sceneColorFB = m_ctx->device.createFramebuffer(fbInfo);
	} catch (const vk::SystemError& e) {
		nprintf(("vulkan", "VulkanLensFlare: Failed to create framebuffer: %s\n", e.what()));
		return false;
	}
	return true;
}

bool VulkanLensFlare::resize()
{
	if (!m_initialized) {
		return true;
	}
	if (m_sceneColorFB) {
		m_ctx->device.destroyFramebuffer(m_sceneColorFB);
		m_sceneColorFB = nullptr;
	}
	return createFramebuffer();
}

void VulkanLensFlare::releaseTextures(bool deferred)
{
	auto* deletionQueue = deferred ? getDeletionQueue() : nullptr;

	auto release = [&](vk::Image& image, vk::ImageView& view, VulkanAllocation& alloc) {
		if (view) {
			if (deletionQueue) {
				deletionQueue->queueImageView(view);
			} else {
				m_ctx->device.destroyImageView(view);
			}
			view = nullptr;
		}
		if (image) {
			if (deletionQueue) {
				deletionQueue->queueImage(image, alloc);
			} else {
				m_ctx->device.destroyImage(image);
				m_ctx->memoryManager->freeAllocation(alloc);
			}
			image = nullptr;
			alloc = {};
		}
	};

	release(m_apertureImage, m_apertureView, m_apertureAlloc);
	release(m_starburstImage, m_starburstView, m_starburstAlloc);
}

// Drop them and forget what was uploaded, so the next frame uploads afresh.
// Distinct from releaseTextures(), which the re-upload path uses to retire the
// outgoing pair *after* the cache keys have been set to the incoming one.
void VulkanLensFlare::forgetTextures()
{
	releaseTextures(true);
	m_texLensIdx = -1;
	m_texGeneration = 0;
}

bool VulkanLensFlare::ensureTextures(int lensIdx)
{
	// Whether the pair we already hold is still current is a rule about the lens
	// module, so it answers it -- rather than each backend re-deriving the same
	// (lens, generation) comparison. A null return means nothing changed.
	const auto* tex = graphics::lens_flare_textures_if_changed(lensIdx, m_texLensIdx, m_texGeneration);
	if (tex == nullptr) {
		return m_apertureView.operator bool();
	}

	auto* texMgr = getTextureManager();
	if (texMgr == nullptr) {
		forgetTextures();
		return false;
	}

	// The outgoing textures may still be referenced by in-flight frames
	releaseTextures(true);

	if (!texMgr->createStaticTexture2D(tex->aperture_size, tex->aperture_size, vk::Format::eR8Unorm,
			tex->aperture.data(), tex->aperture.size(), "Lens flare aperture",
			m_apertureImage, m_apertureView, m_apertureAlloc)) {
		forgetTextures();
		return false;
	}

	if (!texMgr->createStaticTexture2D(tex->starburst_size, tex->starburst_size, vk::Format::eR32G32B32A32Sfloat,
			tex->starburst.data(), tex->starburst.size() * sizeof(float), "Lens flare starburst",
			m_starburstImage, m_starburstView, m_starburstAlloc)) {
		forgetTextures();
		return false;
	}

	return true;
}

void VulkanLensFlare::execute(vk::CommandBuffer cmd)
{
	if (!m_initialized) {
		return;
	}

	// No mounted lens: no flares at all, full stop -- neither the tracked
	// sources below nor a hotspot has anywhere to draw a starburst.
	const int lensIdx = graphics::lens_flare_active_lens();
	if (lensIdx < 0) {
		return;
	}

	// Whether there is anything tracked to draw was decided by
	// lens_flare_frame_update() during the scene render; this pass only draws
	// what it published, and must not second-guess the decision -- the
	// sprite suns have already stepped aside for whatever is in here, so a
	// backend that skipped a published draw would just delete the sun.
	//
	// An empty tracked-draw list is no longer "nothing to do": the hotspot
	// pipeline may still have detected something this frame with no tracked
	// source behind it at all -- in fact the common case once hotspots are
	// active, since thrusters/beams stop publishing tracked draws entirely
	// (see lens_flare_frame_update()). So the gate widens to "either kind has
	// something to draw".
	const auto& flareDraws = graphics::lens_flare_get_frame_draws();
	const bool hotspotsActive = m_hotspot != nullptr && m_hotspot->isInitialized()
		&& graphics::lens_flare_frame_hotspot_active();
	if (flareDraws.empty() && !hotspotsActive) {
		return;
	}

	auto* pipelineMgr = getPipelineManager();
	auto* descriptorMgr = getDescriptorManager();
	if (pipelineMgr == nullptr || descriptorMgr == nullptr || !m_ubo.isValid()) {
		return;
	}

	// Uploaded before the render pass starts, since that path submits its own
	// command buffer and waits. Needed even with zero tracked draws: a
	// hotspot-only frame still draws a starburst sampling this same texture.
	if (!ensureTextures(lensIdx)) {
		return;
	}

	GR_DEBUG_SCOPE("Lens flare");

	// Instanced ghost-quad pipeline (corners from gl_VertexIndex, no vertex input)
	PipelineConfig config;
	config.shaderType = SDR_TYPE_LENS_FLARE;
	config.shaderFlags = 0;
	config.vertexLayoutHash = 0;
	config.primitiveType = PRIM_TYPE_TRISTRIP;
	config.depthMode = ZBUFFER_TYPE_NONE;
	config.blendMode = ALPHA_BLEND_ADDITIVE;
	config.cullEnabled = false;
	config.depthWriteEnabled = false;
	config.renderPass = m_renderPass;

	vertex_layout emptyLayout;
	vk::Pipeline pipeline = pipelineMgr->getPipeline(config, emptyLayout);
	if (!pipeline) {
		return;
	}

	// Scene color: eShaderReadOnlyOptimal (after the scene pass, and after
	// hotspot detection's compute read of it -- see
	// VulkanPostProcessor::executeHotspotDetect(), called immediately before
	// this) -> eColorAttachmentOptimal. Migrated to sync2 (this was the one
	// remaining legacy vkCmdPipelineBarrier call in the Vulkan backend; see
	// VulkanBarrier.h) so srcStage could widen to include the compute read
	// without mixing barrier styles.
	{
		ImageBarrier2 barrier;
		barrier.image = m_sceneColor->image;
		barrier.oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
		barrier.newLayout = vk::ImageLayout::eColorAttachmentOptimal;
		barrier.srcStage = vk::PipelineStageFlagBits2::eFragmentShader
		                  | vk::PipelineStageFlagBits2::eComputeShader;
		barrier.srcAccess = vk::AccessFlagBits2::eShaderRead;
		barrier.dstStage = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
		barrier.dstAccess = vk::AccessFlagBits2::eColorAttachmentRead
		                   | vk::AccessFlagBits2::eColorAttachmentWrite;
		cmdImageBarrier(cmd, barrier);
	}

	vk::PipelineLayout pipelineLayout = pipelineMgr->getPipelineLayout();

	vk::RenderPassBeginInfo rpBegin;
	rpBegin.renderPass = m_renderPass;
	rpBegin.framebuffer = m_sceneColorFB;
	rpBegin.renderArea.offset = vk::Offset2D(0, 0);
	rpBegin.renderArea.extent = m_ctx->sceneExtent;

	cmd.beginRenderPass(rpBegin, vk::SubpassContents::eInline);
	cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

	// Negative viewport height (VK_KHR_maintenance1) for OpenGL-compatible
	// Y-up NDC: the shader emits GL-convention positions, and the scene color
	// image stores the screen top at row 0 (it was rendered with the same flip)
	vk::Viewport viewport;
	viewport.x = 0.0f;
	viewport.y = static_cast<float>(m_ctx->sceneExtent.height);
	viewport.width = static_cast<float>(m_ctx->sceneExtent.width);
	viewport.height = -static_cast<float>(m_ctx->sceneExtent.height);
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	cmd.setViewport(0, viewport);

	vk::Rect2D scissor;
	scissor.offset = vk::Offset2D(0, 0);
	scissor.extent = m_ctx->sceneExtent;
	cmd.setScissor(0, scissor);

	// Set 1: Material -- the mounted lens's iris + starburst, shared by every
	// sun and by the hotspot draw alike, so this is written and bound once
	// for the whole pass. When hotspots are active this frame, its
	// TransformSSBO binding (otherwise unused by SDR_TYPE_LENS_FLARE) doubles
	// as this frame's hotspot results buffer -- conflict-free, since this
	// Material set is freshly allocated every pass.
	DescriptorWriter writer;
	writer.reset(m_ctx->device, descriptorMgr->getFallbacks());

	const uint32_t frameIndex = descriptorMgr->getCurrentFrame();

	vk::DescriptorSet materialSet = descriptorMgr->allocateFrameSet(DescriptorSetIndex::Material);
	Verify(materialSet);
	writer.writeSet(materialSet, VulkanDescriptorManager::getSetTemplate(DescriptorSetIndex::Material));
	{
		std::array<vk::DescriptorImageInfo, VulkanDescriptorManager::MAX_TEXTURE_BINDINGS> texArrayInfos;
		texArrayInfos.fill(descriptorMgr->getFallbacks().texture2D);
		texArrayInfos[0] = {m_ctx->linearSampler, m_apertureView, vk::ImageLayout::eShaderReadOnlyOptimal};
		texArrayInfos[1] = {m_ctx->linearSampler, m_starburstView, vk::ImageLayout::eShaderReadOnlyOptimal};
		writer.setImageArray(MaterialBinding::TextureArray, texArrayInfos);

		if (hotspotsActive) {
			writer.setBuffer(MaterialBinding::TransformSSBO,
				{m_hotspot->getResultsBuffer(frameIndex), 0, VulkanHotspotFlare::getResultsBufferSize()});
		}
	}
	// Flushed here rather than left to the per-source loop below: that loop
	// may not run at all this frame (zero tracked draws, hotspots only), and
	// this write must reach the device either way.
	writer.flush();

	// One draw per visible sun: they share the lens, but each has its own flare
	// axis and tint, hence its own uniform block
	for (size_t i = 0; i < flareDraws.size(); i++) {
		if (m_ubo.cursor(frameIndex) >= m_ubo.slotsPerFrame()) {
			// More flaring suns than the ring can hold this frame; drop the rest
			// rather than trip the ring's overflow assertion
			nprintf(("vulkan", "VulkanLensFlare: out of UBO slots, skipping %d flare draw(s)\n",
				static_cast<int>(flareDraws.size() - i)));
			break;
		}

		// Set 2: PerDraw -- this sun's flare data from the dedicated UBO ring
		vk::DescriptorSet perDrawSet = descriptorMgr->allocateFrameSet(DescriptorSetIndex::PerDraw);
		Verify(perDrawSet);
		writer.writeSet(perDrawSet, VulkanDescriptorManager::getSetTemplate(DescriptorSetIndex::PerDraw));
		{
			vk::DeviceSize slotOffset = m_ubo.alloc(frameIndex, flareDraws[i].data,
				sizeof(generic_data::lens_flare_data));
			writer.setBuffer(PerDrawBinding::GenericData, {m_ubo.buffer(), slotOffset, m_ubo.slotSize()});
		}
		writer.flush();

		cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipelineLayout,
			static_cast<uint32_t>(DescriptorSetIndex::Material),
			{materialSet, perDrawSet}, {});

		cmd.draw(4, flareDraws[i].instances, 0, 0);
	}

	if (hotspotsActive) {
		if (m_ubo.cursor(frameIndex) >= m_ubo.slotsPerFrame()) {
			nprintf(("vulkan", "VulkanLensFlare: out of UBO slots, skipping hotspot draw\n"));
		} else {
			// Small enough (16 bytes) to ride the same per-frame UBO ring as
			// the tracked sources above -- one more slot, not a new resource.
			const graphics::lens_flare_tuning& tuning = graphics::lens_flare_get_tuning();
			HotspotDrawParams params;
			params.quadRadiusNdc = tuning.hotspot_quad_radius_ndc;
			params.threshold = tuning.hotspot_threshold;
			params.scale = tuning.hotspot_scale;
			params.maxApparentRatio = graphics::lens_flare_max_apparent_ratio();

			vk::DescriptorSet hotspotPerDrawSet = descriptorMgr->allocateFrameSet(DescriptorSetIndex::PerDraw);
			Verify(hotspotPerDrawSet);
			writer.writeSet(hotspotPerDrawSet, VulkanDescriptorManager::getSetTemplate(DescriptorSetIndex::PerDraw));
			vk::DeviceSize slotOffset = m_ubo.alloc(frameIndex, &params, sizeof(params));
			writer.setBuffer(PerDrawBinding::GenericData, {m_ubo.buffer(), slotOffset, m_ubo.slotSize()});
			writer.flush();

			// Starburst-only quad pipeline, same render pass as the tracked-source
			// pipeline above (this pipeline is only ever compatible with m_renderPass,
			// which only VulkanLensFlare knows -- VulkanHotspotFlare just draws with it).
			PipelineConfig hotspotConfig;
			hotspotConfig.shaderType = SDR_TYPE_LENS_FLARE_HOTSPOT;
			hotspotConfig.shaderFlags = 0;
			hotspotConfig.vertexLayoutHash = 0;
			hotspotConfig.primitiveType = PRIM_TYPE_TRISTRIP;
			hotspotConfig.depthMode = ZBUFFER_TYPE_NONE;
			hotspotConfig.blendMode = ALPHA_BLEND_ADDITIVE;
			hotspotConfig.cullEnabled = false;
			hotspotConfig.depthWriteEnabled = false;
			hotspotConfig.renderPass = m_renderPass;

			vk::Pipeline hotspotPipeline = pipelineMgr->getPipeline(hotspotConfig, emptyLayout);
			if (hotspotPipeline) {
				m_hotspot->recordDraw(cmd, frameIndex, hotspotPipeline, pipelineLayout, materialSet, hotspotPerDrawSet);
			}
		}
	}

	cmd.endRenderPass();

	// Scene color is back in eShaderReadOnlyOptimal (render pass finalLayout),
	// exactly what the following bloom bright pass expects
}

void VulkanLensFlare::shutdown()
{
	if (m_ctx == nullptr) {
		return;
	}

	// Called with the device idle (VulkanPostProcessor::shutdown waits), so this
	// destroys immediately rather than queueing, and forgets what was uploaded so
	// a re-init starts from nothing.
	releaseTextures(false);
	m_texLensIdx = -1;
	m_texGeneration = 0;

	m_ubo.shutdown();

	if (m_sceneColorFB) {
		m_ctx->device.destroyFramebuffer(m_sceneColorFB);
		m_sceneColorFB = nullptr;
	}
	if (m_renderPass) {
		m_ctx->device.destroyRenderPass(m_renderPass);
		m_renderPass = nullptr;
	}

	m_initialized = false;
}

} // namespace graphics::vulkan
