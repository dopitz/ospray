// ======================================================================== //
// Copyright 2009-2017 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#include <vector>

#include "mpiCommon/MPICommon.h"
#include "OSPWork.h"
#include "ospray/common/ObjectHandle.h"
#include "mpi/fb/DistributedFrameBuffer.h"
#include "mpi/render/MPILoadBalancer.h"

#include "common/Data.h"
#include "common/Library.h"
#include "common/Model.h"
#include "geometry/TriangleMesh.h"
#include "texture/Texture2D.h"

namespace ospray {
  namespace mpi {
    namespace work {

      void registerOSPWorkItems(WorkTypeRegistry &registry)
      {
        registerWorkUnit<NewRenderer>(registry);
        registerWorkUnit<NewModel>(registry);
        registerWorkUnit<NewGeometry>(registry);
        registerWorkUnit<NewCamera>(registry);
        registerWorkUnit<NewVolume>(registry);
        registerWorkUnit<NewTransferFunction>(registry);
        registerWorkUnit<NewPixelOp>(registry);

        registerWorkUnit<NewMaterial>(registry);
        registerWorkUnit<NewLight>(registry);

        registerWorkUnit<NewData>(registry);
        registerWorkUnit<NewTexture2d>(registry);

        registerWorkUnit<CommitObject>(registry);
        registerWorkUnit<CommandRelease>(registry);

        registerWorkUnit<LoadModule>(registry);

        registerWorkUnit<AddGeometry>(registry);
        registerWorkUnit<AddVolume>(registry);
        registerWorkUnit<RemoveGeometry>(registry);
        registerWorkUnit<RemoveVolume>(registry);

        registerWorkUnit<CreateFrameBuffer>(registry);
        registerWorkUnit<ClearFrameBuffer>(registry);
        registerWorkUnit<RenderFrame>(registry);

        registerWorkUnit<SetRegion>(registry);
        registerWorkUnit<SetPixelOp>(registry);

        registerWorkUnit<SetMaterial>(registry);
        registerWorkUnit<SetParam<OSPObject>>(registry);
        registerWorkUnit<SetParam<std::string>>(registry);
        registerWorkUnit<SetParam<int>>(registry);
        registerWorkUnit<SetParam<float>>(registry);
        registerWorkUnit<SetParam<vec2f>>(registry);
        registerWorkUnit<SetParam<vec2i>>(registry);
        registerWorkUnit<SetParam<vec3f>>(registry);
        registerWorkUnit<SetParam<vec3i>>(registry);
        registerWorkUnit<SetParam<vec4f>>(registry);

        registerWorkUnit<RemoveParam>(registry);

        registerWorkUnit<CommandFinalize>(registry);
      }

      // ospCommit ////////////////////////////////////////////////////////////
      
      CommitObject::CommitObject(ObjectHandle handle)
        : handle(handle)
      {}
      
      void CommitObject::run()
      {
        ManagedObject *obj = handle.lookup();
        if (obj) {
          obj->commit();

          // TODO: Do we need this hack anymore?
          // It looks like yes? or at least glutViewer segfaults if we don't do this
          // hack, to stay compatible with earlier version
          Model *model = dynamic_cast<Model*>(obj);
          if (model) model->finalize();
        } else {
          throw std::runtime_error("Error: rank "
                                   + std::to_string(mpi::world.rank)
                                   + " did not have object to commit!");
        }
        // TODO: Work units should not be directly making MPI calls.
        // What should be responsible for this barrier?
        // MPI_Barrier(MPI_COMM_WORLD);

        /// iw: nah, perfectly OK to do MPI calls, as long as they
        /// don't get into each other's ways, nad make sure there's no
        /// cyclical dependencies (ie, that the server unit actually
        /// does get flushed etcpp)
        
        mpi::app.barrier();
      }
      
      void CommitObject::runOnMaster()
      {
        if (handle.defined()) {
          ManagedObject *obj = handle.lookup();
          if (dynamic_cast<Renderer*>(obj)) {
            obj->commit();
          }
        }
        mpi::worker.barrier();
      }
      
      void CommitObject::serialize(WriteStream &b) const
      {
        b << (int64)handle;
      }
      
      void CommitObject::deserialize(ReadStream &b)
      {
        b >> handle.i64;
      }

      // ospNewFrameBuffer ////////////////////////////////////////////////////

      CreateFrameBuffer::CreateFrameBuffer(ObjectHandle handle,
                                           vec2i dimensions,
                                           OSPFrameBufferFormat format,
                                           uint32 channels)
        : handle(handle),
          dimensions(dimensions),
          format(format),
          channels(channels)
      {
      }
    
      void CreateFrameBuffer::run()
      {
        const bool hasDepthBuffer    = channels & OSP_FB_DEPTH;
        const bool hasAccumBuffer    = channels & OSP_FB_ACCUM;
        const bool hasVarianceBuffer = channels & OSP_FB_VARIANCE;

        assert(dimensions.x > 0);
        assert(dimensions.y > 0);

        FrameBuffer *fb
          = new DistributedFrameBuffer(dimensions, handle,
                                       format, hasDepthBuffer,
                                       hasAccumBuffer, hasVarianceBuffer);
        fb->refInc();
        handle.assign(fb);
      }
      
      void CreateFrameBuffer::runOnMaster()
      {
        run();
      }
      
      void CreateFrameBuffer::serialize(WriteStream &b) const
      {
        b << (int64)handle << dimensions << (int32)format << channels;
      }
      
      void CreateFrameBuffer::deserialize(ReadStream &b)
      {
        int32 fmt;
        b >> handle.i64 >> dimensions >> fmt >> channels;
        format = (OSPFrameBufferFormat)fmt;
      }

      // ospLoadModule ////////////////////////////////////////////////////////
      
      LoadModule::LoadModule(const std::string &name)
        : name(name)
      {}
      
      void LoadModule::run()
      {
        const std::string libName = "ospray_module_" + name;
        loadLibrary(libName);

        const std::string initSymName = "ospray_init_module_" + name;
        void *initSym = getSymbol(initSymName);
        if (!initSym) {
          throw std::runtime_error("could not find module initializer "
                                   + initSymName);
        }
        void (*initMethod)() = (void(*)())initSym;
        initMethod();
      }
      void LoadModule::runOnMaster()
      {
        run();
      }

      void LoadModule::serialize(WriteStream &b) const
      {
        b << name;
      }
      
      void LoadModule::deserialize(ReadStream &b)
      {
        b >> name;
      }

      // ospSetParam //////////////////////////////////////////////////////////

      template<>
      void SetParam<std::string>::run()
      {
        ManagedObject *obj = handle.lookup();
        Assert(obj);
        obj->findParam(name.c_str(), true)->set(val.c_str());
      }
    
      template<>
      void SetParam<std::string>::runOnMaster()
      {
        if (!handle.defined())
          return;
        
        ManagedObject *obj = handle.lookup();
        if (dynamic_cast<Renderer*>(obj) || dynamic_cast<Volume*>(obj)) {
          obj->findParam(name.c_str(), true)->set(val.c_str());
        }
      }

      // ospSetMaterial ///////////////////////////////////////////////////////

      void SetMaterial::run() 
      {
        Geometry *geom = (Geometry*)handle.lookup();
        Material *mat = (Material*)material.lookup();
        Assert(geom);
        Assert(mat);
        /* might we worthwhile doing a dyncast here to check if that
           is actually a proper geometry .. */
        geom->setMaterial(mat);
      }

      // ospNewRenderer ///////////////////////////////////////////////////////

      template<>
      void NewRenderer::runOnMaster()
      {
        run();
      }

      // ospNewVolume /////////////////////////////////////////////////////////

      template<>
      void NewVolume::runOnMaster()
      {
        run();
      }

      // ospNewModel //////////////////////////////////////////////////////////

      template<>
      void NewModel::run()
      {
        auto *model = new Model;
        handle.assign(model);
      }

      // ospNewMaterial ///////////////////////////////////////////////////////

      void NewMaterial::run()
      {
        Renderer *renderer = (Renderer*)rendererHandle.lookup();
        Material *material = nullptr;
        if (renderer) {
          material = renderer->createMaterial(type.c_str());
          if (material) {
            material->refInc();
          }
        }
        // No renderer present or the renderer doesn't intercept this
        // material type.
        if (!material) material = Material::createMaterial(type.c_str());
        handle.assign(material);
      }

      // ospNewLight //////////////////////////////////////////////////////////

      void NewLight::run()
      {
        Renderer *renderer = (Renderer*)rendererHandle.lookup();
        Light *light = nullptr;
        if (renderer) {
          light = renderer->createLight(type.c_str());
          if (light) {
            light->refInc();
          }
        }
        // No renderer present or the renderer doesn't intercept this
        // material type.
        if (!light) light = Light::createLight(type.c_str());
        handle.assign(light);
      }
      
      // ospNewData ///////////////////////////////////////////////////////////

      NewData::NewData(ObjectHandle handle,
                       size_t nItems,
                       OSPDataType format,
                       void *init,
                       int flags)
        : handle(handle),
          nItems(nItems),
          format(format),
          localData(nullptr),
          flags(flags)
      {
        // TODO: Is this check ok for ParaView e.g. what Carson is changing in 2e81c005 ?
        if (init && nItems) {
          if (flags & OSP_DATA_SHARED_BUFFER) {
            localData = init;
          } else {
            static WarnOnce warning("#osp.mpi: warning - newdata currently "
                                    "creates a std::vector copy of input data");
            data.resize(ospray::sizeOf(format) * nItems);
            std::memcpy(data.data(), init, data.size());
          }
        }
      }
      
      void NewData::run()
      {
        Data *ospdata = nullptr;
        if (!data.empty()) {
          // iw - shouldn't we _always_ set the shared_data flag here?
          // after deserialization all the data is in a std::vector
          // (data) that we own, and never free, anywya -- shouldn't
          // we just share this?
          ospdata = new Data(nItems, format, data.data(), flags);
        } else if (localData) {
          // iw - how can that ever trigger? localdata should get set
          // only on the master, but 'run' happens only on the
          // workers.... right?
          ospdata = new Data(nItems, format, localData, flags);
        } else {
          // iw - can this ever happen? (empty data?) if so, shouldn't
          // we make sure that flags get the shared flag removed (in
          // case it was set)
          ospdata = new Data(nItems, format, nullptr, flags);
        }
        Assert(ospdata);
        // iw - not sure if string would be handled correctly (I doubt
        // it), so let's assert that nobody accidentally uses it.
        assert(format != OSP_STRING);
        handle.assign(ospdata);
        if (format == OSP_OBJECT ||
            format == OSP_CAMERA  ||
            format == OSP_DATA ||
            format == OSP_FRAMEBUFFER ||
            format == OSP_GEOMETRY ||
            format == OSP_LIGHT ||
            format == OSP_MATERIAL ||
            format == OSP_MODEL ||
            format == OSP_RENDERER ||
            format == OSP_TEXTURE ||
            format == OSP_TRANSFER_FUNCTION ||
            format == OSP_VOLUME ||
            format == OSP_PIXEL_OP
            ) {
          /* translating handles to managedobject pointers: if a
             data array has 'object' or 'data' entry types, then
             what the host sends are _handles_, not pointers, but
             what the core expects are pointers; to make the core
             happy we translate all data items back to pointers at
             this stage */
          ObjectHandle   *asHandle = (ObjectHandle*)ospdata->data;
          ManagedObject **asObjPtr = (ManagedObject**)ospdata->data;
          for (size_t i = 0; i < nItems; ++i) {
            if (asHandle[i] != NULL_HANDLE) {
              asObjPtr[i] = asHandle[i].lookup();
              asObjPtr[i]->refInc();
            }
          }
        }
      }
      
      void NewData::serialize(WriteStream &b) const
      {
        static WarnOnce warning("#osp.mpi: Warning - newdata serialize "
                                "currently uses a std::vector... ");
        /* note there are two issues with this: first is that when
           sharing data buffer we'd have only localdata set (not the
           this->data vector; second is that even _if_ we use the data
           vector we're (temporarily) doubling memory consumption
           because we copy all data into the std::vector first, just
           so we can send it.... */
        b << (int64)handle << nItems << (int32)format << flags << data;
      }
      
      void NewData::deserialize(ReadStream &b)
      {
        int32 fmt;
        b >> handle.i64 >> nItems >> fmt >> flags >> data;
        format = (OSPDataType)fmt;
      }

      // ospNewTexture2d //////////////////////////////////////////////////////

      NewTexture2d::NewTexture2d(ObjectHandle handle,
                                 vec2i dimensions,
                                 OSPTextureFormat format,
                                 void *texture,
                                 uint32 flags)
        : handle(handle),
          dimensions(dimensions),
          format(format),
          flags(flags)
      {
        size_t sz = ospray::sizeOf(format) * dimensions.x * dimensions.y;
        data.resize(sz);
        std::memcpy(data.data(), texture, sz);
      }
      
      void NewTexture2d::run()
      {
        Texture2D *texture =
            Texture2D::createTexture(dimensions, format, data.data(),
                                     flags & ~OSP_TEXTURE_SHARED_BUFFER);
        Assert(texture);
        handle.assign(texture);
      }
      
      void NewTexture2d::serialize(WriteStream &b) const
      {
        b << (int64)handle << dimensions << (int32)format << flags << data;
      }
      
      void NewTexture2d::deserialize(ReadStream &b)
      {
        int32 fmt;
        b >> handle.i64 >> dimensions >> fmt >> flags >> data;
        format = (OSPTextureFormat)fmt;
      }

      // ospSetRegion /////////////////////////////////////////////////////////

      SetRegion::SetRegion(OSPVolume volume, vec3i start, vec3i size,
                           const void *src, OSPDataType type)
        : handle((ObjectHandle&)volume), regionStart(start),
          regionSize(size), type(type)
      {
        size_t bytes = ospray::sizeOf(type) * size.x * size.y * size.z;
        // TODO: With the MPI batching this limitation should be lifted
        if (bytes > 2000000000LL) {
          throw std::runtime_error("MPI ospSetRegion does not support "
                                   "region sizes > 2GB");
        }
        data.resize(bytes);

        //TODO: should support sending data without copy
        std::memcpy(data.data(), src, bytes);
      }

      void SetRegion::run()
      {
        Volume *volume = (Volume*)handle.lookup();
        Assert(volume);
        // TODO: Does it make sense to do the allreduce & report back fails?
        // TODO: Should we be allocating the data with alignedMalloc instead?
        // We could use a std::vector with an aligned std::allocator
        if (!volume->setRegion(data.data(), regionStart, regionSize)) {
          throw std::runtime_error("Failed to set region for volume");
        }
      }

      void SetRegion::serialize(WriteStream &b) const
      {
        b << (int64)handle << regionStart << regionSize << (int32)type << data;
      }

      void SetRegion::deserialize(ReadStream &b)
      {
        int32 ty;
        b >> handle.i64 >> regionStart >> regionSize >> ty >> data;
        type = (OSPDataType)ty;
      }

      // ospFrameBufferClear //////////////////////////////////////////////////

      ClearFrameBuffer::ClearFrameBuffer(OSPFrameBuffer fb, uint32 channels)
        : handle((ObjectHandle&)fb), channels(channels)
      {}
      
      void ClearFrameBuffer::run()
      {
        FrameBuffer *fb = (FrameBuffer*)handle.lookup();
        Assert(fb);
        fb->clear(channels);
      }
      
      void ClearFrameBuffer::runOnMaster()
      {
        run();
      }

      void ClearFrameBuffer::serialize(WriteStream &b) const
      {
        b << (int64)handle << channels;
      }
      
      void ClearFrameBuffer::deserialize(ReadStream &b)
      {
        b >> handle.i64 >> channels;
      }

      // ospRenderFrame ///////////////////////////////////////////////////////
      
      RenderFrame::RenderFrame(OSPFrameBuffer fb,
                               OSPRenderer renderer,
                               uint32 channels)
        : fbHandle((ObjectHandle&)fb),
          rendererHandle((ObjectHandle&)renderer),
          channels(channels),
          varianceResult(0.f)
      {}
      
      void RenderFrame::run()
      {
        Renderer *renderer = (Renderer*)rendererHandle.lookup();
        FrameBuffer *fb    = (FrameBuffer*)fbHandle.lookup();
        Assert(renderer);
        Assert(fb);
        // TODO: This function execution must run differently
        // if we're the master vs. the worker in master/worker
        // mode. Actually, if the Master has the Master load balancer,
        // it should be fine??? Actually not if the renderer
        // takes over scheduling of tile work like the distributed volume renderer
        // We need some way to pick the right function to call, either to the
        // renderer or directly to the load balancer to render the frame
        varianceResult = renderer->renderFrame(fb, channels);
      }
      
      void RenderFrame::runOnMaster()
      {
        Renderer *renderer = (Renderer*)rendererHandle.lookup();
        FrameBuffer *fb    = (FrameBuffer*)fbHandle.lookup();
        Assert(renderer);
        Assert(fb);
        varianceResult =
            TiledLoadBalancer::instance->renderFrame(renderer, fb, channels);
      }
      
      void RenderFrame::serialize(WriteStream &b) const
      {
        b << (int64)fbHandle << (int64)rendererHandle << channels;
      }
      
      void RenderFrame::deserialize(ReadStream &b)
      {
        b >> fbHandle.i64 >> rendererHandle.i64 >> channels;
      }

      // ospAddGeometry ///////////////////////////////////////////////////////

      void AddGeometry::run()
      {
        Model *model = (Model*)modelHandle.lookup();
        Geometry *geometry = (Geometry*)objectHandle.lookup();
        Assert(model);
        Assert(geometry);
        model->geometry.push_back(geometry);
      }

      // ospAddVolume /////////////////////////////////////////////////////////

      void AddVolume::run()
      {
        Model *model = (Model*)modelHandle.lookup();
        Volume *volume = (Volume*)objectHandle.lookup();
        Assert(model);
        Assert(volume);
        model->volume.push_back(volume);
      }

      // ospRemoveGeometry ////////////////////////////////////////////////////

      void RemoveGeometry::run()
      {
        Model *model = (Model*)modelHandle.lookup();
        Geometry *geometry = (Geometry*)objectHandle.lookup();
        Assert(model);
        Assert(geometry);
        auto it = std::find_if(model->geometry.begin(), model->geometry.end(),
                               [&](const Ref<Geometry> &g) {
                                 return geometry == &*g;
                               });
        if (it != model->geometry.end()) {
          model->geometry.erase(it);
        }
      }

      // ospRemoveVolume //////////////////////////////////////////////////////

      void RemoveVolume::run()
      {
        Model *model = (Model*)modelHandle.lookup();
        Volume *volume = (Volume*)objectHandle.lookup();
        Assert(model);
        Assert(volume);
        model->volume.push_back(volume);
        auto it = std::find_if(model->volume.begin(), model->volume.end(),
                               [&](const Ref<Volume> &v) {
                                 return volume == &*v;
                               });
        if (it != model->volume.end()) {
          model->volume.erase(it);
        }
      }

      // ospRemoveParam ///////////////////////////////////////////////////////

      RemoveParam::RemoveParam(ObjectHandle handle, const char *name)
        : handle(handle), name(name)
      {
        Assert(handle != nullHandle);
      }
      
      void RemoveParam::run()
      {
        ManagedObject *obj = handle.lookup();
        Assert(obj);
        obj->removeParam(name.c_str());
      }
      
      void RemoveParam::runOnMaster()
      {
        ManagedObject *obj = handle.lookup();
        if (dynamic_cast<Renderer*>(obj) || dynamic_cast<Volume*>(obj)) {
          obj->removeParam(name.c_str());
        }
      }

      void RemoveParam::serialize(WriteStream &b) const
      {
        b << (int64)handle << name;
      }

      void RemoveParam::deserialize(ReadStream &b)
      {
        b >> handle.i64 >> name;
      }

      // ospSetPixelOp ////////////////////////////////////////////////////////
      
      SetPixelOp::SetPixelOp(OSPFrameBuffer fb, OSPPixelOp op)
        : fbHandle((ObjectHandle&)fb),
          poHandle((ObjectHandle&)op)
      {}
      
      void SetPixelOp::run()
      {
        FrameBuffer *fb = (FrameBuffer*)fbHandle.lookup();
        PixelOp     *po = (PixelOp*)poHandle.lookup();
        Assert(fb);
        Assert(po);
        fb->pixelOp = po->createInstance(fb, fb->pixelOp.ptr);

        if (!fb->pixelOp) {
          postErrorMsg("#osp:mpi: WARNING: PixelOp did not create an instance!\n");
        }
      }

      void SetPixelOp::serialize(WriteStream &b) const
      {
        b << (int64)fbHandle << (int64)poHandle;
      }
      
      void SetPixelOp::deserialize(ReadStream &b)
      {
        b >> fbHandle.i64 >> poHandle.i64;
      }

      // ospRelease ///////////////////////////////////////////////////////////
      
      CommandRelease::CommandRelease(ObjectHandle handle)
        : handle(handle)
      {}
      
      void CommandRelease::run()
      {
        handle.freeObject();
      }

      void CommandRelease::serialize(WriteStream &b) const
      {
        b << (int64)handle;
      }
      
      void CommandRelease::deserialize(ReadStream &b)
      {
        b >> handle.i64;
      }

      // ospFinalize //////////////////////////////////////////////////////////
      
      void CommandFinalize::run()
      {
        runOnMaster();

        // TODO: Is it ok to call exit again here?
        // should we be calling exit? When the MPIDevice is
        // destroyed (at program exit) we'll send this command
        // to ourselves/other ranks. In master/worker mode
        // the workers should call std::exit to leave the worker loop
        // but the master or all ranks in collab mode would already
        // be exiting.
        std::exit(0);
      }
      
      void CommandFinalize::runOnMaster()
      {
        world.barrier();
        SERIALIZED_MPI_CALL(Finalize());
      }
      
      void CommandFinalize::serialize(WriteStream &b) const
      {}
      
      void CommandFinalize::deserialize(ReadStream &b)
      {}

    } // ::ospray::mpi::work
  } // ::ospray::mpi
} // ::ospray

