// Copyright 2009 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

// ospray
#include "Renderer.h"
#include "LoadBalancer.h"
#include "Material.h"
#include "common/Instance.h"
#include "geometry/GeometricModel.h"
#include "ospray/OSPEnums.h"
#include "pf/PixelFilter.h"
#ifdef OSPRAY_TARGET_DPCPP
#include "render/RendererRenderTaskFn.inl"
#include "render/RendererType.ih"
#include "render/util.ih"
#else
// ispc exports
#include "render/Renderer_ispc.h"
#include "render/util_ispc.h"
#endif

namespace ospray {

// Renderer definitions ///////////////////////////////////////////////////////

Renderer::Renderer(api::ISPCDevice &device)
    : AddStructShared(device.getIspcrtDevice(), device)
{
  managedObjectType = OSP_RENDERER;
  pixelFilter = nullptr;
  mathConstants = rkcommon::make_unique<MathConstants>(device);
  getSh()->mathConstants = mathConstants->getSh();
#ifndef OSPRAY_TARGET_DPCPP
  getSh()->renderSample = reinterpret_cast<ispc::Renderer_RenderSampleFct>(
      ispc::Renderer_default_renderSample_addr());
#endif
}

std::string Renderer::toString() const
{
  return "ospray::Renderer";
}

void Renderer::commit()
{
  spp = std::max(1, getParam<int>("pixelSamples", 1));
  const int32 maxDepth = std::max(0, getParam<int>("maxPathLength", 20));
  const float minContribution = getParam<float>("minContribution", 0.001f);
  errorThreshold = getParam<float>("varianceThreshold", 0.f);

  maxDepthTexture = (Texture2D *)getParamObject("map_maxDepth");
  backplate = (Texture2D *)getParamObject("map_backplate");

  if (maxDepthTexture) {
    if (maxDepthTexture->format != OSP_TEXTURE_R32F
        || maxDepthTexture->filter != OSP_TEXTURE_FILTER_NEAREST) {
      static WarnOnce warning(
          "maxDepthTexture provided to the renderer "
          "needs to be of type OSP_TEXTURE_R32F and have "
          "the OSP_TEXTURE_FILTER_NEAREST flag");
    }
  }

  vec3f bgColor3 = getParam<vec3f>(
      "backgroundColor", vec3f(getParam<float>("backgroundColor", 0.f)));
  bgColor = getParam<vec4f>("backgroundColor", vec4f(bgColor3, 0.f));

  // Handle materials assigned to renderer
  materialArray = nullptr;
  getSh()->material = nullptr;
  materialData = getParamDataT<Material *>("material");
  if (materialData) {
    materialArray = make_buffer_shared_unique<ispc::Material *>(
        getISPCDevice().getIspcrtDevice(),
        createArrayOfSh<ispc::Material>(*materialData));
    getSh()->numMaterials = materialArray->size();
    getSh()->material = materialArray->sharedPtr();
  }

  getSh()->spp = spp;
  getSh()->maxDepth = maxDepth;
  getSh()->minContribution = minContribution;
  getSh()->bgColor = bgColor;
  getSh()->backplate = backplate ? backplate->getSh() : nullptr;
  getSh()->maxDepthTexture =
      maxDepthTexture ? maxDepthTexture->getSh() : nullptr;

  setupPixelFilter();
  getSh()->pixelFilter = pixelFilter ? pixelFilter->getSh() : nullptr;

  ispc::precomputeZOrder();
}

#ifdef OSPRAY_TARGET_DPCPP
/*
void Renderer::setGPUFunctionPtrs(sycl::queue &syclQueue)
{
  auto *sSh = getSh();
  auto event = syclQueue.submit([&](sycl::handler &cgh) {
    cgh.parallel_for(1, [=](cl::sycl::id<1>) RTC_SYCL_KERNEL {
      sSh->renderSample = ispc::Renderer_default_renderSample;
      sSh->renderTask = ispc::Renderer_default_renderTask;
    });
  });
  event.wait();
}
*/
#endif

#ifndef OSPRAY_TARGET_DPCPP
void Renderer::renderTasks(FrameBuffer *fb,
    Camera *camera,
    World *world,
    void *perFrameData,
    const utility::ArrayView<uint32_t> &taskIDs) const
{
  ispc::Renderer_renderTasks(getSh(),
      fb->getSh(),
      camera->getSh(),
      world->getSh(),
      perFrameData,
      taskIDs.data(),
      taskIDs.size());
}
#else
void Renderer::renderTasks(FrameBuffer *fb,
    Camera *camera,
    World *world,
    void *perFrameData,
    const utility::ArrayView<uint32_t> &taskIDs,
    sycl::queue &syclQueue) const
{
  auto *rendererSh = getSh();
  auto *fbSh = fb->getSh();
  auto *cameraSh = camera->getSh();
  auto *worldSh = world->getSh();
  const uint32_t *taskIDsPtr = taskIDs.data();
  const size_t numTasks = taskIDs.size();

  auto event = syclQueue.submit([&](sycl::handler &cgh) {
    const cl::sycl::nd_range<1> dispatchRange =
        computeDispatchRange(numTasks, RTC_SYCL_SIMD_WIDTH);
    cgh.parallel_for(
        dispatchRange, [=](cl::sycl::nd_item<1> taskIndex) RTC_SYCL_KERNEL {
          if (taskIndex.get_global_id(0) < numTasks) {
            ispc::Renderer_default_renderTask(rendererSh,
                fbSh,
                cameraSh,
                worldSh,
                perFrameData,
                taskIDsPtr,
                taskIndex.get_global_id(0),
                ispc::Renderer_dispatch_renderSample);
          }
        });
  });
  event.wait_and_throw();
  // For prints we have to flush the entire queue, because other stuff is queued
  syclQueue.wait_and_throw();
}

cl::sycl::nd_range<1> Renderer::computeDispatchRange(
    const size_t globalSize, const size_t workgroupSize) const
{
  const size_t roundedRange =
      ((globalSize + workgroupSize - 1) / workgroupSize) * workgroupSize;
  return cl::sycl::nd_range<1>(roundedRange, workgroupSize);
}
#endif

OSPPickResult Renderer::pick(
    FrameBuffer *fb, Camera *camera, World *world, const vec2f &screenPos)
{
  OSPPickResult res;

  res.instance = nullptr;
  res.model = nullptr;
  res.primID = RTC_INVALID_GEOMETRY_ID;

  int instID = RTC_INVALID_GEOMETRY_ID;
  int geomID = RTC_INVALID_GEOMETRY_ID;
  int primID = RTC_INVALID_GEOMETRY_ID;

  ispc::Renderer_pick(getSh(),
      fb->getSh(),
      camera->getSh(),
      world->getSh(),
      (const ispc::vec2f &)screenPos,
      (ispc::vec3f &)res.worldPosition[0],
      instID,
      geomID,
      primID,
      res.hasHit);

  if (res.hasHit) {
    auto *instance = (*world->instances)[instID];
    auto *group = instance->group.ptr;
    if (!group->geometricModels) {
      res.hasHit = false;
      return res;
    }
    auto *model = (*group->geometricModels)[geomID];

    instance->refInc();
    model->refInc();

    res.instance = (OSPInstance)instance;
    res.model = (OSPGeometricModel)model;
    res.primID = static_cast<uint32_t>(primID);
  }

  return res;
}

void Renderer::setupPixelFilter()
{
  OSPPixelFilterTypes pixelFilterType =
      (OSPPixelFilterTypes)getParam<uint8_t>("pixelFilter",
          getParam<int32_t>(
              "pixelFilter", OSPPixelFilterTypes::OSP_PIXELFILTER_GAUSS));
  pixelFilter = nullptr;
  switch (pixelFilterType) {
  case OSPPixelFilterTypes::OSP_PIXELFILTER_BOX: {
    pixelFilter = new BoxPixelFilter(getISPCDevice());
    break;
  }
  case OSPPixelFilterTypes::OSP_PIXELFILTER_POINT: {
    pixelFilter = new PointPixelFilter(getISPCDevice());
    break;
  }
  case OSPPixelFilterTypes::OSP_PIXELFILTER_BLACKMAN_HARRIS: {
    pixelFilter = new BlackmanHarrisLUTPixelFilter(getISPCDevice());
    break;
  }
  case OSPPixelFilterTypes::OSP_PIXELFILTER_MITCHELL: {
    pixelFilter = new MitchellNetravaliLUTPixelFilter(getISPCDevice());
    break;
  }
  case OSPPixelFilterTypes::OSP_PIXELFILTER_GAUSS:
  default: {
    pixelFilter = new GaussianLUTPixelFilter(getISPCDevice());
    break;
  }
  }
  if (pixelFilter) {
    // Need to remove extra local ref
    pixelFilter->refDec();
  }
}

OSPTYPEFOR_DEFINITION(Renderer *);

} // namespace ospray
