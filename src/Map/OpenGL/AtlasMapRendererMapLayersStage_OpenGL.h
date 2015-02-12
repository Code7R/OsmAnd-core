#ifndef _OSMAND_CORE_ATLAS_MAP_RENDERER_MAP_LAYERS_STAGE_OPENGL_H_
#define _OSMAND_CORE_ATLAS_MAP_RENDERER_MAP_LAYERS_STAGE_OPENGL_H_

#include "stdlib_common.h"
#include "ignore_warnings_on_external_includes.h"
#include <array>
#include "restore_internal_warnings.h"

#include "ignore_warnings_on_external_includes.h"
#include <glm/glm.hpp>
#include "restore_internal_warnings.h"

#include "QtExtensions.h"
#include "ignore_warnings_on_external_includes.h"
#include <QMap>
#include <QHash>
#include <QVector>
#include "restore_internal_warnings.h"

#include "OsmAndCore.h"
#include "CommonTypes.h"
#include "Ref.h"
#include "AtlasMapRendererMapLayersStage.h"
#include "AtlasMapRendererStageHelper_OpenGL.h"

namespace OsmAnd
{
    class AtlasMapRendererMapLayersStage_OpenGL
        : public AtlasMapRendererMapLayersStage
        , private AtlasMapRendererStageHelper_OpenGL
    {
    private:
    protected:
        struct BatchedLayerResource Q_DECL_FINAL
        {
            BatchedLayerResource(
                const std::shared_ptr<const GPUAPI::ResourceInGPU>& resourceInGPU,
                const int zoomShift = 0,
                const PointF nOffsetInTile = PointF(0.0f, 0.0f),
                const PointF nSizeInTile = PointF(1.0f, 1.0f));

            std::shared_ptr<const GPUAPI::ResourceInGPU> resourceInGPU;
            int zoomShift;
            PointF nOffsetInTile;
            PointF nSizeInTile;

            bool canBeBatchedWith(const BatchedLayerResource& that) const;

        private:
            Q_DISABLE_COPY(BatchedLayerResource);
        };
        struct BatchedLayer Q_DECL_FINAL
        {
            BatchedLayer(const int layerIndex);

            const int layerIndex;
            QList< Ref<BatchedLayerResource> > resourcesInGPU;
        
        private:
            Q_DISABLE_COPY(BatchedLayer);
        };
        struct PerTileBatchedLayers Q_DECL_FINAL
        {
            PerTileBatchedLayers(const TileId tileId, const bool containsOriginLayer);

            const TileId tileId;
            const bool containsOriginLayer;
            QList< Ref<BatchedLayer> > layers;

            bool operator<(const PerTileBatchedLayers& that) const;

        private:
            Q_DISABLE_COPY(PerTileBatchedLayers);
        };
        QList< Ref<PerTileBatchedLayers> > batchLayersByTiles(const AtlasMapRendererInternalState& internalState);

        // Raster layers support:
        unsigned int _maxNumberOfRasterMapLayersInBatch;
        GLsizei _rasterTileIndicesCount;
        GLname _rasterTileVBO;
        GLname _rasterTileIBO;
        QHash<unsigned int, GLname> _rasterTileVAOs;
        void initializeRasterTile();
        void releaseRasterTile();
        struct RasterLayerTileProgram
        {
            GLname id;

            struct {
                // Input data
                struct {
                    GLlocation vertexPosition;
                    GLlocation vertexTexCoords;
                    GLlocation vertexElevation;
                } in;

                // Parameters
                struct {
                    // Common data
                    GLlocation mProjectionView;
                    GLlocation mapScale;
                    GLlocation targetInTilePosN;
                    GLlocation distanceFromCameraToTarget;
                    GLlocation cameraElevationAngleN;
                    GLlocation groundCameraPosition;
                    GLlocation scaleToRetainProjectedSize;

                    // Per-tile data
                    GLlocation tileCoordsOffset;
                    GLlocation elevationData_scaleFactor;
                    GLlocation elevationData_sampler;
                    GLlocation elevationData_upperMetersPerUnit;
                    GLlocation elevationData_lowerMetersPerUnit;

                    // Per-tile-per-layer data
                    struct VsPerTilePerLayerParameters
                    {
                        GLlocation nOffsetInTile;
                        GLlocation nSizeInTile;
                    };
                    VsPerTilePerLayerParameters elevationDataLayer;
                    QVector<VsPerTilePerLayerParameters> rasterTileLayers;
                } param;
            } vs;

            struct {
                // Parameters
                struct {
                    // Per-tile-per-layer data
                    struct FsPerTilePerLayerParameters
                    {
                        GLlocation opacity;
                        GLlocation isPremultipliedAlpha;
                        GLlocation sampler;
                    };
                    QVector<FsPerTilePerLayerParameters> rasterTileLayers;
                } param;
            } fs;
        };
        QMap<unsigned int, RasterLayerTileProgram> _rasterLayerTilePrograms;
        bool initializeRasterLayers();
        bool initializeRasterLayersProgram(
            const unsigned int numberOfLayersInBatch,
            RasterLayerTileProgram& outRasterLayerTileProgram);
        bool renderRasterLayersBatch(
            const Ref<PerTileBatchedLayers>& batch,
            AlphaChannelType& currentAlphaChannelType,
            GLlocation& activeElevationVertexAttribArray,
            int& lastUsedProgram);
        void configureElevationData(
            const RasterLayerTileProgram& program,
            const int elevationDataSamplerIndex,
            const TileId tileId,
            GLlocation& activeElevationVertexAttribArray);
        bool activateRasterLayersProgram(
            const unsigned int numberOfLayersInBatch,
            const int elevationDataSamplerIndex,
            GLlocation& activeElevationVertexAttribArray,
            int& lastUsedProgram);
        std::shared_ptr<const GPUAPI::ResourceInGPU> captureElevationDataResource(
            const TileId normalizedTileId,
            const ZoomLevel zoomLevel);
        std::shared_ptr<const GPUAPI::ResourceInGPU> captureLayerResource(
            const std::shared_ptr<const IMapRendererResourcesCollection>& resourcesCollection,
            const TileId normalizedTileId,
            const ZoomLevel zoomLevel);
        bool releaseRasterLayers();
    public:
        AtlasMapRendererMapLayersStage_OpenGL(AtlasMapRenderer_OpenGL* const renderer);
        virtual ~AtlasMapRendererMapLayersStage_OpenGL();

        virtual bool initialize();
        virtual bool render(IMapRenderer_Metrics::Metric_renderFrame* const metric);

        virtual bool release();
    };
}

#endif // !defined(_OSMAND_CORE_ATLAS_MAP_RENDERER_MAP_LAYERS_STAGE_OPENGL_H_)