#include "AtlasMapRendererMapLayersStage_OpenGL.h"

#include <cassert>
#include <algorithm>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/transform.hpp>

#include "ignore_warnings_on_external_includes.h"
#include <SkColor.h>
#include "restore_internal_warnings.h"

#include "AtlasMapRenderer_OpenGL.h"
#include "AtlasMapRenderer_Metrics.h"
#include "IMapTiledDataProvider.h"
#include "IRasterMapLayerProvider.h"
#include "IMapElevationDataProvider.h"
#include "QKeyValueIterator.h"
#include "Utilities.h"

OsmAnd::AtlasMapRendererMapLayersStage_OpenGL::AtlasMapRendererMapLayersStage_OpenGL(AtlasMapRenderer_OpenGL* const renderer_)
    : AtlasMapRendererMapLayersStage(renderer_)
    , AtlasMapRendererStageHelper_OpenGL(this)
    , _maxNumberOfRasterMapLayersInBatch(0)
    , _rasterTileIndicesCount(-1)
{
}

OsmAnd::AtlasMapRendererMapLayersStage_OpenGL::~AtlasMapRendererMapLayersStage_OpenGL()
{
}

bool OsmAnd::AtlasMapRendererMapLayersStage_OpenGL::initialize()
{
    bool ok = true;
    ok = ok && initializeRasterLayers();
    return ok;
}

bool OsmAnd::AtlasMapRendererMapLayersStage_OpenGL::render(IMapRenderer_Metrics::Metric_renderFrame* const metric_)
{
    const auto metric = dynamic_cast<AtlasMapRenderer_Metrics::Metric_renderFrame*>(metric_);
    bool ok = true;

    const auto& internalState = getInternalState();
    const auto gpuAPI = getGPUAPI();

    if (currentState.mapLayersProviders.isEmpty())
        return ok;

    GL_PUSH_GROUP_MARKER(QLatin1String("mapLayers"));

    // First vector layer or first raster layers batch should be rendered without blending,
    // since blending is performed inside shader itself.
    bool blendingEnabled = false;
    glDisable(GL_BLEND);
    GL_CHECK_RESULT;

    // Initially, configure for premultiplied alpha channel type
    auto currentAlphaChannelType = AlphaChannelType::Premultiplied;
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    GL_CHECK_RESULT;

    int lastUsedProgram = -1;
    GLlocation activeElevationVertexAttribArray;
    const auto& batchedLayersByTiles = batchLayersByTiles(internalState);
    for (const auto& batchedLayersByTile : constOf(batchedLayersByTiles))
    {
        // Any layer or layers batch after first one has to be rendered using blending,
        // since output color of new batch needs to be blended with destination color.
        if (!batchedLayersByTile->containsOriginLayer != blendingEnabled)
        {
            if (batchedLayersByTile->containsOriginLayer)
            {
                glDisable(GL_BLEND);
                GL_CHECK_RESULT;
            }
            else
            {
                glEnable(GL_BLEND);
                GL_CHECK_RESULT;
            }

            blendingEnabled = !batchedLayersByTile->containsOriginLayer;
        }

        // Depending on type of first provider (and all others), batch is rendered differently
        const auto& firstProviderInBatch = currentState.mapLayersProviders[batchedLayersByTile->layers.first()->layerIndex];
        if (const auto rasterMapLayerProvider = std::dynamic_pointer_cast<IRasterMapLayerProvider>(firstProviderInBatch))
        {
            renderRasterLayersBatch(
                batchedLayersByTile,
                currentAlphaChannelType,
                activeElevationVertexAttribArray,
                lastUsedProgram);
        }
    }

    // Disable elevation vertex attrib array (if enabled)
    if (activeElevationVertexAttribArray.isValid())
    {
        glDisableVertexAttribArray(*activeElevationVertexAttribArray);
        GL_CHECK_RESULT;
    }

    // Deactivate program
    glUseProgram(0);
    GL_CHECK_RESULT;

    gpuAPI->unuseVAO();

    GL_POP_GROUP_MARKER;

    return ok;
}

bool OsmAnd::AtlasMapRendererMapLayersStage_OpenGL::release()
{
    bool ok = true;
    ok = ok && releaseRasterLayers();
    return ok;
}

bool OsmAnd::AtlasMapRendererMapLayersStage_OpenGL::initializeRasterLayers()
{
    const auto gpuAPI = getGPUAPI();

    // Determine maximum number of raster layers in one batch. It's determined by minimal of following numbers:
    //  - (maxVertexUniformVectors - alreadyOccupiedUniforms) / (vsUniformsPerLayer + fsUniformsPerLayer)
    //  - maxTextureUnitsInFragmentShader
    //  - setupOptions.maxNumberOfRasterMapLayersInBatch
    const auto vsUniformsPerLayer =
        1 /*nOffsetInTile*/ +
        1 /*nSizeInTile*/;
    const auto fsUniformsPerLayer =
        1 /*opacity*/ +
        1 /*isPremultipliedAlpha*/ +
        1 /*sampler*/;
    const auto alreadyOccupiedUniforms =
        4 /*param_vs_mProjectionView*/ +
        1 /*param_vs_targetInTilePosN*/ +
        (!gpuAPI->isSupported_textureLod
            ? 0
            : 1 /*param_vs_distanceFromCameraToTarget*/ +
              1 /*param_vs_cameraElevationAngleN*/ +
              1 /*param_vs_groundCameraPosition*/ +
              1 /*param_vs_scaleToRetainProjectedSize*/) +
        1 /*param_vs_tileCoordsOffset*/ +
        1 /*param_vs_elevationData_scaleFactor*/ +
        1 /*param_vs_elevationData_upperMetersPerUnit*/ +
        1 /*param_vs_elevationData_lowerMetersPerUnit*/ +
        (gpuAPI->isSupported_vertexShaderTextureLookup ? vsUniformsPerLayer : 0) /*param_vs_elevationDataLayer*/;
    _maxNumberOfRasterMapLayersInBatch =
        (gpuAPI->maxVertexUniformVectors - alreadyOccupiedUniforms) / (vsUniformsPerLayer + fsUniformsPerLayer);
    if (_maxNumberOfRasterMapLayersInBatch > gpuAPI->maxTextureUnitsInFragmentShader)
        _maxNumberOfRasterMapLayersInBatch = gpuAPI->maxTextureUnitsInFragmentShader;
    if (setupOptions.maxNumberOfRasterMapLayersInBatch != 0 &&
        _maxNumberOfRasterMapLayersInBatch > setupOptions.maxNumberOfRasterMapLayersInBatch)
    {
        _maxNumberOfRasterMapLayersInBatch = setupOptions.maxNumberOfRasterMapLayersInBatch;
    }

    // Initialize programs that support [1 ... _maxNumberOfRasterMapLayersInBatch] as number of layers
    auto supportedMaxNumberOfRasterMapLayersInBatch = _maxNumberOfRasterMapLayersInBatch;
    for (auto numberOfLayersInBatch = _maxNumberOfRasterMapLayersInBatch; numberOfLayersInBatch >= 1; numberOfLayersInBatch--)
    {
        RasterLayerTileProgram rasterLayerTileProgram;
        const auto success = initializeRasterLayersProgram(numberOfLayersInBatch, rasterLayerTileProgram);
        if (!success)
        {
            supportedMaxNumberOfRasterMapLayersInBatch -= 1;
            continue;
        }

        _rasterLayerTilePrograms.insert(numberOfLayersInBatch, rasterLayerTileProgram);
    }
    if (supportedMaxNumberOfRasterMapLayersInBatch != _maxNumberOfRasterMapLayersInBatch)
    {
        LogPrintf(LogSeverityLevel::Warning,
            "Seems like buggy driver. "
            "This device should be capable of rendering %d raster map layers in batch, but only %d variant compiles",
            _maxNumberOfRasterMapLayersInBatch,
            supportedMaxNumberOfRasterMapLayersInBatch);
        _maxNumberOfRasterMapLayersInBatch = supportedMaxNumberOfRasterMapLayersInBatch;
    }
    if (_maxNumberOfRasterMapLayersInBatch < 1)
        return false;

    initializeRasterTile();

    return true;
}

bool OsmAnd::AtlasMapRendererMapLayersStage_OpenGL::initializeRasterLayersProgram(
    const unsigned int numberOfLayersInBatch,
    RasterLayerTileProgram& outRasterLayerTileProgram)
{
    const auto gpuAPI = getGPUAPI();

    GL_CHECK_PRESENT(glDeleteShader);
    GL_CHECK_PRESENT(glDeleteProgram);

    const auto& vertexShader = QString::fromLatin1(
        // Input data
        "INPUT vec2 in_vs_vertexPosition;                                                                                   ""\n"
        "INPUT vec2 in_vs_vertexTexCoords;                                                                                  ""\n"
        "#if !VERTEX_TEXTURE_FETCH_SUPPORTED                                                                                ""\n"
        "    INPUT float in_vs_vertexElevation;                                                                             ""\n"
        "#endif // !VERTEX_TEXTURE_FETCH_SUPPORTED                                                                          ""\n"
        "                                                                                                                   ""\n"
        // Output data to next shader stages
        "%UnrolledPerRasterLayerTexCoordsDeclarationCode%                                                                   ""\n"
        "#if TEXTURE_LOD_SUPPORTED                                                                                          ""\n"
        "    PARAM_OUTPUT float v2f_mipmapLOD;                                                                              ""\n"
        "#endif // TEXTURE_LOD_SUPPORTED                                                                                    ""\n"
        "                                                                                                                   ""\n"
        // Parameters: common data
        "uniform mat4 param_vs_mProjectionView;                                                                             ""\n"
        "uniform vec2 param_vs_targetInTilePosN;                                                                            ""\n"
        "#if TEXTURE_LOD_SUPPORTED                                                                                          ""\n"
        "    uniform float param_vs_distanceFromCameraToTarget;                                                             ""\n"
        "    uniform float param_vs_cameraElevationAngleN;                                                                  ""\n"
        "    uniform vec2 param_vs_groundCameraPosition;                                                                    ""\n"
        "    uniform float param_vs_scaleToRetainProjectedSize;                                                             ""\n"
        "#endif // TEXTURE_LOD_SUPPORTED                                                                                    ""\n"
        "                                                                                                                   ""\n"
        // Parameters: per-tile data
        "uniform ivec2 param_vs_tileCoordsOffset;                                                                           ""\n"
        "uniform float param_vs_elevationData_scaleFactor;                                                                  ""\n"
        "uniform float param_vs_elevationData_upperMetersPerUnit;                                                           ""\n"
        "uniform float param_vs_elevationData_lowerMetersPerUnit;                                                           ""\n"
        "#if VERTEX_TEXTURE_FETCH_SUPPORTED                                                                                 ""\n"
        "    uniform highp sampler2D param_vs_elevationData_sampler;                                                        ""\n"
        "#endif // VERTEX_TEXTURE_FETCH_SUPPORTED                                                                           ""\n"
        "                                                                                                                   ""\n"
        // Parameters: per-layer-in-tile data
        "struct RasterLayerTile                                                                                             ""\n"
        "{                                                                                                                  ""\n"
        "    vec2 nOffsetInTile;                                                                                            ""\n"
        "    vec2 nSizeInTile;                                                                                              ""\n"
        "};                                                                                                                 ""\n"
        "%UnrolledPerRasterLayerParamsDeclarationCode%                                                                      ""\n"
        "#if VERTEX_TEXTURE_FETCH_SUPPORTED                                                                                 ""\n"
        "    uniform RasterLayerTile param_vs_elevationDataLayer;                                                           ""\n"
        "#endif // !VERTEX_TEXTURE_FETCH_SUPPORTED                                                                          ""\n"
        "                                                                                                                   ""\n"
        "void calculateTextureCoordinates(in RasterLayerTile tileLayer, out vec2 outTexCoords)                              ""\n"
        "{                                                                                                                  ""\n"
        "    outTexCoords = in_vs_vertexTexCoords * tileLayer.nSizeInTile + tileLayer.nOffsetInTile;                        ""\n"
        "}                                                                                                                  ""\n"
        "                                                                                                                   ""\n"
        "void main()                                                                                                        ""\n"
        "{                                                                                                                  ""\n"
        "    vec4 v = vec4(in_vs_vertexPosition.x, 0.0, in_vs_vertexPosition.y, 1.0);                                       ""\n"
        "                                                                                                                   ""\n"
        //   Shift vertex to it's proper position
        "    float xOffset = float(param_vs_tileCoordsOffset.x) - param_vs_targetInTilePosN.x;                              ""\n"
        "    v.x += xOffset * %TileSize3D%.0;                                                                               ""\n"
        "    float yOffset = float(param_vs_tileCoordsOffset.y) - param_vs_targetInTilePosN.y;                              ""\n"
        "    v.z += yOffset * %TileSize3D%.0;                                                                               ""\n"
        "                                                                                                                   ""\n"
        //   Process each tile layer texture coordinates (except elevation)
        "%UnrolledPerRasterLayerTexCoordsProcessingCode%                                                                    ""\n"
        "                                                                                                                   ""\n"
        //   If elevation data is active, use it
        "    if (abs(param_vs_elevationData_scaleFactor) > 0.0)                                                             ""\n"
        "    {                                                                                                              ""\n"
        "        float metersToUnits = mix(param_vs_elevationData_upperMetersPerUnit,                                       ""\n"
        "            param_vs_elevationData_lowerMetersPerUnit, in_vs_vertexTexCoords.t);                                   ""\n"
        "                                                                                                                   ""\n"
        //       Calculate texcoords for elevation data (pixel-is-area)
        "        float heightInMeters;                                                                                      ""\n"
        "#if VERTEX_TEXTURE_FETCH_SUPPORTED                                                                                 ""\n"
        "        vec2 elevationDataTexCoords;                                                                               ""\n"
        "        calculateTextureCoordinates(                                                                               ""\n"
        "            param_vs_elevationDataLayer,                                                                           ""\n"
        "            elevationDataTexCoords);                                                                               ""\n"
        "        heightInMeters = SAMPLE_TEXTURE_2D(param_vs_elevationData_sampler, elevationDataTexCoords).r;              ""\n"
        "#else // !VERTEX_TEXTURE_FETCH_SUPPORTED                                                                           ""\n"
        "        heightInMeters = in_vs_vertexElevation;                                                                    ""\n"
        "#endif // VERTEX_TEXTURE_FETCH_SUPPORTED                                                                           ""\n"
        "                                                                                                                   ""\n"
        "        v.y = heightInMeters / metersToUnits;                                                                      ""\n"
        "        v.y *= param_vs_elevationData_scaleFactor;                                                                 ""\n"
        "    }                                                                                                              ""\n"
        "                                                                                                                   ""\n"
        "#if TEXTURE_LOD_SUPPORTED                                                                                          ""\n"
        //   Calculate mipmap LOD
        "    vec2 groundVertex = v.xz;                                                                                      ""\n"
        "    vec2 groundCameraToVertex = groundVertex - param_vs_groundCameraPosition;                                      ""\n"
        "    float mipmapK = log(1.0 + 10.0 * log2(1.0 + param_vs_cameraElevationAngleN));                                  ""\n"
        "    float mipmapBaseLevelEndDistance = mipmapK * param_vs_distanceFromCameraToTarget;                              ""\n"
        "    v2f_mipmapLOD = 1.0 + (length(groundCameraToVertex) - mipmapBaseLevelEndDistance)                              ""\n"
        "        / (param_vs_scaleToRetainProjectedSize * %TileSize3D%.0);                                                  ""\n"
        "#endif // TEXTURE_LOD_SUPPORTED                                                                                    ""\n"
        "                                                                                                                   ""\n"
        //   Finally output processed modified vertex
        "    gl_Position = param_vs_mProjectionView * v;                                                                    ""\n"
        "}                                                                                                                  ""\n");
    const auto& vertexShader_perRasterLayerTexCoordsDeclaration = QString::fromLatin1(
        "PARAM_OUTPUT vec2 v2f_texCoordsPerLayer_%rasterLayerIndex%;                                                        ""\n");
    const auto& vertexShader_perRasterLayerParamsDeclaration = QString::fromLatin1(
        "uniform RasterLayerTile param_vs_rasterTileLayer_%rasterLayerIndex%;                                               ""\n");
    const auto& vertexShader_perRasterLayerTexCoordsProcessing = QString::fromLatin1(
        "    calculateTextureCoordinates(                                                                                   ""\n"
        "        param_vs_rasterTileLayer_%rasterLayerIndex%,                                                               ""\n"
        "        v2f_texCoordsPerLayer_%rasterLayerIndex%);                                                                 ""\n"
        "                                                                                                                   ""\n");

    const auto& fragmentShader = QString::fromLatin1(
        // Input data
        "%UnrolledPerRasterLayerTexCoordsDeclarationCode%                                                                   ""\n"
        "#if TEXTURE_LOD_SUPPORTED                                                                                          ""\n"
        "    PARAM_INPUT float v2f_mipmapLOD;                                                                               ""\n"
        "#endif // TEXTURE_LOD_SUPPORTED                                                                                    ""\n"
        "                                                                                                                   ""\n"
        // Parameters: per-layer data
        "struct RasterLayerTile                                                                                             ""\n"
        "{                                                                                                                  ""\n"
        "    lowp float opacity;                                                                                            ""\n"
        "    lowp float isPremultipliedAlpha;                                                                               ""\n"
        "    lowp sampler2D sampler;                                                                                        ""\n"
        "};                                                                                                                 ""\n"
        "%UnrolledPerRasterLayerParamsDeclarationCode%                                                                      ""\n"
        "                                                                                                                   ""\n"
        "void addExtraAlpha(inout vec4 color, in float alpha, in float isPremultipliedAlpha)                                ""\n"
        "{                                                                                                                  ""\n"
        "    lowp float colorAlpha = 1.0 - isPremultipliedAlpha + isPremultipliedAlpha * alpha;                             ""\n"
        "    color *= vec4(colorAlpha, colorAlpha, colorAlpha, alpha);                                                      ""\n"
        "}                                                                                                                  ""\n"
        "                                                                                                                   ""\n"
        "void mixColors(inout vec4 destColor, in vec4 srcColor, in float isPremultipliedAlpha)                              ""\n"
        "{                                                                                                                  ""\n"
        "    lowp float srcColorMultiplier =                                                                                ""\n"
        "        isPremultipliedAlpha + (1.0 - isPremultipliedAlpha) * srcColor.a;                                          ""\n"
        "    destColor = destColor * (1.0 - srcColor.a) + srcColor * srcColorMultiplier;                                    ""\n"
        "}                                                                                                                  ""\n"
        "                                                                                                                   ""\n"
        "void main()                                                                                                        ""\n"
        "{                                                                                                                  ""\n"
        "    lowp vec4 finalColor;                                                                                          ""\n"
        "                                                                                                                   ""\n"
        //   Mix colors of all layers.
        //   First layer is processed unconditionally, as well as its color is converted to premultiplied alpha.
        "#if TEXTURE_LOD_SUPPORTED                                                                                          ""\n"
        "    finalColor = SAMPLE_TEXTURE_2D_LOD(                                                                            ""\n"
        "        param_fs_rasterTileLayer_0.sampler,                                                                        ""\n"
        "        v2f_texCoordsPerLayer_0, v2f_mipmapLOD);                                                                   ""\n"
        "#else // !TEXTURE_LOD_SUPPORTED                                                                                    ""\n"
        "    finalColor = SAMPLE_TEXTURE_2D(                                                                                ""\n"
        "        param_fs_rasterTileLayer_0.sampler,                                                                        ""\n"
        "        v2f_texCoordsPerLayer_0);                                                                                  ""\n"
        "#endif // TEXTURE_LOD_SUPPORTED                                                                                    ""\n"
        "    addExtraAlpha(finalColor, param_fs_rasterTileLayer_0.opacity, param_fs_rasterTileLayer_0.isPremultipliedAlpha);""\n"
        "    lowp float firstLayerColorDivisor = param_fs_rasterTileLayer_0.isPremultipliedAlpha +                          ""\n"
        "        (1.0 - param_fs_rasterTileLayer_0.isPremultipliedAlpha) * finalColor.a;                                    ""\n"
        "    finalColor /= vec4(firstLayerColorDivisor, firstLayerColorDivisor, firstLayerColorDivisor, 1.0);               ""\n"
        "                                                                                                                   ""\n"
        "%UnrolledPerRasterLayerProcessingCode%                                                                             ""\n"
        "                                                                                                                   ""\n"
#if 0
        //   NOTE: Useful for debugging mipmap levels
        "    {                                                                                                              ""\n"
        "        vec4 mipmapDebugColor;                                                                                     ""\n"
        "        mipmapDebugColor.a = 1.0;                                                                                  ""\n"
        "        float value = v2f_mipmapLOD;                                                                               ""\n"
        //"        float value = textureQueryLod(param_vs_rasterTileLayer[0].sampler, v2f_texCoordsPerLayer[0]).x;          ""\n"
        "        mipmapDebugColor.r = clamp(value, 0.0, 1.0);                                                               ""\n"
        "        value -= 1.0;                                                                                              ""\n"
        "        mipmapDebugColor.g = clamp(value, 0.0, 1.0);                                                               ""\n"
        "        value -= 1.0;                                                                                              ""\n"
        "        mipmapDebugColor.b = clamp(value, 0.0, 1.0);                                                               ""\n"
        "        finalColor = mix(finalColor, mipmapDebugColor, 0.5);                                                       ""\n"
        "    }                                                                                                              ""\n"
#endif
        "    FRAGMENT_COLOR_OUTPUT = finalColor;                                                                            ""\n"
        "}                                                                                                                  ""\n");
    const auto& fragmentShader_perRasterLayer = QString::fromLatin1(
        "    {                                                                                                              ""\n"
        "#if TEXTURE_LOD_SUPPORTED                                                                                          ""\n"
        "        lowp vec4 layerColor = SAMPLE_TEXTURE_2D_LOD(                                                              ""\n"
        "            param_fs_rasterTileLayer_%rasterLayerIndex%.sampler,                                                   ""\n"
        "            v2f_texCoordsPerLayer_%rasterLayerIndex%, v2f_mipmapLOD);                                              ""\n"
        "#else // !TEXTURE_LOD_SUPPORTED                                                                                    ""\n"
        "        lowp vec4 layerColor = SAMPLE_TEXTURE_2D(                                                                  ""\n"
        "            param_fs_rasterTileLayer_%rasterLayerIndex%.sampler,                                                   ""\n"
        "            v2f_texCoordsPerLayer_%rasterLayerIndex%);                                                             ""\n"
        "#endif // TEXTURE_LOD_SUPPORTED                                                                                    ""\n"
        "                                                                                                                   ""\n"
        "        addExtraAlpha(layerColor, param_fs_rasterTileLayer_%rasterLayerIndex%.opacity,                             ""\n"
        "            param_fs_rasterTileLayer_%rasterLayerIndex%.isPremultipliedAlpha);                                     ""\n"
        "        mixColors(finalColor, layerColor, param_fs_rasterTileLayer_%rasterLayerIndex%.isPremultipliedAlpha);       ""\n"
        "    }                                                                                                              ""\n");
    const auto& fragmentShader_perRasterLayerTexCoordsDeclaration = QString::fromLatin1(
        "PARAM_INPUT vec2 v2f_texCoordsPerLayer_%rasterLayerIndex%;                                                         ""\n");
    const auto& fragmentShader_perRasterLayerParamsDeclaration = QString::fromLatin1(
        "uniform RasterLayerTile param_fs_rasterTileLayer_%rasterLayerIndex%;                                               ""\n");

    // Compile vertex shader
    auto preprocessedVertexShader = vertexShader;
    QString preprocessedVertexShader_UnrolledPerRasterLayerTexCoordsProcessingCode;
    QString preprocessedVertexShader_UnrolledPerRasterLayerParamsDeclarationCode;
    QString preprocessedVertexShader_UnrolledPerRasterLayerTexCoordsDeclarationCode;
    for (auto layerIndex = 0u; layerIndex < numberOfLayersInBatch; layerIndex++)
    {
        preprocessedVertexShader_UnrolledPerRasterLayerTexCoordsProcessingCode +=
            detachedOf(vertexShader_perRasterLayerTexCoordsProcessing).replace("%rasterLayerIndex%", QString::number(layerIndex));

        preprocessedVertexShader_UnrolledPerRasterLayerParamsDeclarationCode +=
            detachedOf(vertexShader_perRasterLayerParamsDeclaration).replace("%rasterLayerIndex%", QString::number(layerIndex));

        preprocessedVertexShader_UnrolledPerRasterLayerTexCoordsDeclarationCode +=
            detachedOf(vertexShader_perRasterLayerTexCoordsDeclaration).replace("%rasterLayerIndex%", QString::number(layerIndex));
    }
    preprocessedVertexShader.replace("%UnrolledPerRasterLayerTexCoordsProcessingCode%",
        preprocessedVertexShader_UnrolledPerRasterLayerTexCoordsProcessingCode);
    preprocessedVertexShader.replace("%UnrolledPerRasterLayerParamsDeclarationCode%",
        preprocessedVertexShader_UnrolledPerRasterLayerParamsDeclarationCode);
    preprocessedVertexShader.replace("%UnrolledPerRasterLayerTexCoordsDeclarationCode%",
        preprocessedVertexShader_UnrolledPerRasterLayerTexCoordsDeclarationCode);
    preprocessedVertexShader.replace("%TileSize3D%", QString::number(AtlasMapRenderer::TileSize3D));
    gpuAPI->preprocessVertexShader(preprocessedVertexShader);
    gpuAPI->optimizeVertexShader(preprocessedVertexShader);
    const auto vsId = gpuAPI->compileShader(GL_VERTEX_SHADER, qPrintable(preprocessedVertexShader));
    if (vsId == 0)
    {
        LogPrintf(LogSeverityLevel::Error,
            "Failed to compile AtlasMapRendererMapLayersStage_OpenGL vertex shader for %d raster map layers", numberOfLayersInBatch);
        return false;
    }

    // Compile fragment shader
    auto preprocessedFragmentShader = fragmentShader;
    QString preprocessedFragmentShader_UnrolledPerRasterLayerTexCoordsDeclarationCode;
    QString preprocessedFragmentShader_UnrolledPerRasterLayerParamsDeclarationCode;
    QString preprocessedFragmentShader_UnrolledPerRasterLayerProcessingCode;
    for (auto layerIndex = 0u; layerIndex < numberOfLayersInBatch; layerIndex++)
    {
        preprocessedFragmentShader_UnrolledPerRasterLayerTexCoordsDeclarationCode +=
            detachedOf(fragmentShader_perRasterLayerTexCoordsDeclaration).replace("%rasterLayerIndex%", QString::number(layerIndex));

        preprocessedFragmentShader_UnrolledPerRasterLayerParamsDeclarationCode +=
            detachedOf(fragmentShader_perRasterLayerParamsDeclaration).replace("%rasterLayerIndex%", QString::number(layerIndex));

        if (layerIndex > 0)
        {
            preprocessedFragmentShader_UnrolledPerRasterLayerProcessingCode +=
                detachedOf(fragmentShader_perRasterLayer).replace("%rasterLayerIndex%", QString::number(layerIndex));;
        }
    }
    preprocessedFragmentShader.replace("%UnrolledPerRasterLayerTexCoordsDeclarationCode%", preprocessedFragmentShader_UnrolledPerRasterLayerTexCoordsDeclarationCode);
    preprocessedFragmentShader.replace("%UnrolledPerRasterLayerParamsDeclarationCode%", preprocessedFragmentShader_UnrolledPerRasterLayerParamsDeclarationCode);
    preprocessedFragmentShader.replace("%UnrolledPerRasterLayerProcessingCode%", preprocessedFragmentShader_UnrolledPerRasterLayerProcessingCode);
    gpuAPI->preprocessFragmentShader(preprocessedFragmentShader);
    gpuAPI->optimizeFragmentShader(preprocessedFragmentShader);
    const auto fsId = gpuAPI->compileShader(GL_FRAGMENT_SHADER, qPrintable(preprocessedFragmentShader));
    if (fsId == 0)
    {
        glDeleteShader(vsId);
        GL_CHECK_RESULT;

        LogPrintf(LogSeverityLevel::Error,
            "Failed to compile AtlasMapRendererMapLayersStage_OpenGL fragment shader for %d raster map layers", numberOfLayersInBatch);
        return false;
    }

    // Link everything into program object
    GLuint shaders[] = { vsId, fsId };
    QHash< QString, GPUAPI_OpenGL::GlslProgramVariable > variablesMap;
    outRasterLayerTileProgram.id = getGPUAPI()->linkProgram(2, shaders, true, &variablesMap);
    if (!outRasterLayerTileProgram.id.isValid())
    {
        LogPrintf(LogSeverityLevel::Error,
            "Failed to link AtlasMapRendererMapLayersStage_OpenGL program for %d raster map layers", numberOfLayersInBatch);
        return false;
    }

    bool ok = true;
    const auto& lookup = gpuAPI->obtainVariablesLookupContext(outRasterLayerTileProgram.id, variablesMap);
    ok = ok && lookup->lookupLocation(outRasterLayerTileProgram.vs.in.vertexPosition, "in_vs_vertexPosition", GlslVariableType::In);
    ok = ok && lookup->lookupLocation(outRasterLayerTileProgram.vs.in.vertexTexCoords, "in_vs_vertexTexCoords", GlslVariableType::In);
    if (!gpuAPI->isSupported_vertexShaderTextureLookup)
    {
        ok = ok && lookup->lookupLocation(outRasterLayerTileProgram.vs.in.vertexElevation, "in_vs_vertexElevation", GlslVariableType::In);
    }
    ok = ok && lookup->lookupLocation(outRasterLayerTileProgram.vs.param.mProjectionView, "param_vs_mProjectionView", GlslVariableType::Uniform);
    ok = ok && lookup->lookupLocation(outRasterLayerTileProgram.vs.param.targetInTilePosN, "param_vs_targetInTilePosN", GlslVariableType::Uniform);
    if (gpuAPI->isSupported_textureLod)
    {
        ok = ok && lookup->lookupLocation(outRasterLayerTileProgram.vs.param.distanceFromCameraToTarget, "param_vs_distanceFromCameraToTarget", GlslVariableType::Uniform);
        ok = ok && lookup->lookupLocation(outRasterLayerTileProgram.vs.param.cameraElevationAngleN, "param_vs_cameraElevationAngleN", GlslVariableType::Uniform);
        ok = ok && lookup->lookupLocation(outRasterLayerTileProgram.vs.param.groundCameraPosition, "param_vs_groundCameraPosition", GlslVariableType::Uniform);
        ok = ok && lookup->lookupLocation(outRasterLayerTileProgram.vs.param.scaleToRetainProjectedSize, "param_vs_scaleToRetainProjectedSize", GlslVariableType::Uniform);
    }
    ok = ok && lookup->lookupLocation(outRasterLayerTileProgram.vs.param.tileCoordsOffset, "param_vs_tileCoordsOffset", GlslVariableType::Uniform);
    ok = ok && lookup->lookupLocation(outRasterLayerTileProgram.vs.param.elevationData_scaleFactor, "param_vs_elevationData_scaleFactor", GlslVariableType::Uniform);
    ok = ok && lookup->lookupLocation(outRasterLayerTileProgram.vs.param.elevationData_upperMetersPerUnit, "param_vs_elevationData_upperMetersPerUnit", GlslVariableType::Uniform);
    ok = ok && lookup->lookupLocation(outRasterLayerTileProgram.vs.param.elevationData_lowerMetersPerUnit, "param_vs_elevationData_lowerMetersPerUnit", GlslVariableType::Uniform);
    if (gpuAPI->isSupported_vertexShaderTextureLookup)
    {
        ok = ok && lookup->lookupLocation(outRasterLayerTileProgram.vs.param.elevationData_sampler, "param_vs_elevationData_sampler", GlslVariableType::Uniform);
        ok = ok && lookup->lookupLocation(outRasterLayerTileProgram.vs.param.elevationDataLayer.nOffsetInTile, "param_vs_elevationDataLayer.nOffsetInTile", GlslVariableType::Uniform);
        ok = ok && lookup->lookupLocation(outRasterLayerTileProgram.vs.param.elevationDataLayer.nSizeInTile, "param_vs_elevationDataLayer.nSizeInTile", GlslVariableType::Uniform);
    }
    outRasterLayerTileProgram.vs.param.rasterTileLayers.resize(numberOfLayersInBatch);
    outRasterLayerTileProgram.fs.param.rasterTileLayers.resize(numberOfLayersInBatch);
    for (auto layerIndex = 0u; layerIndex < numberOfLayersInBatch; layerIndex++)
    {
        // Vertex shader
        {
            auto layerStructName =
                QString::fromLatin1("param_vs_rasterTileLayer_%layerIndex%")
                .replace(QLatin1String("%layerIndex%"), QString::number(layerIndex));
            auto& layerStruct = outRasterLayerTileProgram.vs.param.rasterTileLayers[layerIndex];

            ok = ok && lookup->lookupLocation(layerStruct.nOffsetInTile, layerStructName + ".nOffsetInTile", GlslVariableType::Uniform);
            ok = ok && lookup->lookupLocation(layerStruct.nSizeInTile, layerStructName + ".nSizeInTile", GlslVariableType::Uniform);
        }

        // Fragment shader
        {
            auto layerStructName =
                QString::fromLatin1("param_fs_rasterTileLayer_%layerIndex%")
                .replace(QLatin1String("%layerIndex%"), QString::number(layerIndex));
            auto& layerStruct = outRasterLayerTileProgram.fs.param.rasterTileLayers[layerIndex];

            ok = ok && lookup->lookupLocation(layerStruct.opacity, layerStructName + ".opacity", GlslVariableType::Uniform);
            ok = ok && lookup->lookupLocation(layerStruct.isPremultipliedAlpha, layerStructName + ".isPremultipliedAlpha", GlslVariableType::Uniform);
            ok = ok && lookup->lookupLocation(layerStruct.sampler, layerStructName + ".sampler", GlslVariableType::Uniform);
        }
    }

    return ok;
}

bool OsmAnd::AtlasMapRendererMapLayersStage_OpenGL::renderRasterLayersBatch(
    const Ref<PerTileBatchedLayers>& batch,
    AlphaChannelType& currentAlphaChannelType,
    GLlocation& activeElevationVertexAttribArray,
    int& lastUsedProgram)
{
    const auto gpuAPI = getGPUAPI();

    GL_CHECK_PRESENT(glUniformMatrix4fv);
    GL_CHECK_PRESENT(glUniform1f);
    GL_CHECK_PRESENT(glUniform2f);
    GL_CHECK_PRESENT(glUniform1i);
    GL_CHECK_PRESENT(glUniform2i);
    GL_CHECK_PRESENT(glUniform2fv);
    GL_CHECK_PRESENT(glActiveTexture);
    GL_CHECK_PRESENT(glEnableVertexAttribArray);
    GL_CHECK_PRESENT(glVertexAttribPointer);
    GL_CHECK_PRESENT(glDisableVertexAttribArray);

    const auto& currentConfiguration = getCurrentConfiguration();
    const auto& internalState = getInternalState();

    const auto batchedLayersCount = batch->layers.size();
    const auto elevationDataSamplerIndex = gpuAPI->isSupported_vertexShaderTextureLookup ? batchedLayersCount : -1;

    GL_PUSH_GROUP_MARKER(QString("%1x%2@%3").arg(batch->tileId.x).arg(batch->tileId.y).arg(currentState.zoomBase));

    // Activate proper program depending on number of captured layers
    const auto wasActivated = activateRasterLayersProgram(
        batchedLayersCount,
        elevationDataSamplerIndex,
        activeElevationVertexAttribArray,
        lastUsedProgram);
    const auto& program = _rasterLayerTilePrograms[batchedLayersCount];
    const auto& vao = _rasterTileVAOs[batchedLayersCount];

    // Set tile coordinates offset
    glUniform2i(program.vs.param.tileCoordsOffset,
        batch->tileId.x - internalState.targetTileId.x,
        batch->tileId.y - internalState.targetTileId.y);
    GL_CHECK_RESULT;

    // Configure elevation data
    configureElevationData(
        program,
        elevationDataSamplerIndex,
        batch->tileId,
        activeElevationVertexAttribArray);

    // Shader expects blending to be premultiplied
    if (currentAlphaChannelType != AlphaChannelType::Premultiplied)
    {
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        GL_CHECK_RESULT;

        currentAlphaChannelType = AlphaChannelType::Premultiplied;
    }

    // Single pass tile rendering is possible for exact-scale and overscale cases
    if (batch->layers.first()->resourcesInGPU.size() == 1)
    {
        // Set uniform variables for each raster layer
        for (int layerIndexInBatch = 0; layerIndexInBatch < batchedLayersCount; layerIndexInBatch++)
        {
            const auto& layer = batch->layers[layerIndexInBatch];

            const auto layerConfiguration = currentState.mapLayersConfigurations[layer->layerIndex];
            const auto& perTile_vs = program.vs.param.rasterTileLayers[layerIndexInBatch];
            const auto& perTile_fs = program.fs.param.rasterTileLayers[layerIndexInBatch];

            glUniform1f(perTile_fs.opacity, layerConfiguration.opacity);
            GL_CHECK_RESULT;

            // Since it's single-pass tile rendering, there's only one resource per layer
            const auto& batchedResourceInGPU = layer->resourcesInGPU.first();

            switch (gpuAPI->getGpuResourceAlphaChannelType(batchedResourceInGPU->resourceInGPU))
            {
                case AlphaChannelType::Premultiplied:
                    glUniform1f(perTile_fs.isPremultipliedAlpha, 1.0f);
                    GL_CHECK_RESULT;
                    break;
                case AlphaChannelType::Straight:
                    glUniform1f(perTile_fs.isPremultipliedAlpha, 0.0f);
                    GL_CHECK_RESULT;
                    break;
                default:
                    break;
            }

            glActiveTexture(GL_TEXTURE0 + layerIndexInBatch);
            GL_CHECK_RESULT;

            glBindTexture(GL_TEXTURE_2D,
                static_cast<GLuint>(reinterpret_cast<intptr_t>(batchedResourceInGPU->resourceInGPU->refInGPU)));
            GL_CHECK_RESULT;

            gpuAPI->applyTextureBlockToTexture(GL_TEXTURE_2D, GL_TEXTURE0 + layerIndexInBatch);

            if (batchedResourceInGPU->resourceInGPU->type == GPUAPI::ResourceInGPU::Type::SlotOnAtlasTexture)
            {
                const auto tileOnAtlasTexture =
                    std::static_pointer_cast<const GPUAPI::SlotOnAtlasTextureInGPU>(batchedResourceInGPU->resourceInGPU);
                const auto rowIndex = tileOnAtlasTexture->slotIndex / tileOnAtlasTexture->atlasTexture->slotsPerSide;
                const auto colIndex = tileOnAtlasTexture->slotIndex - rowIndex * tileOnAtlasTexture->atlasTexture->slotsPerSide;
                const auto tileSizeN = tileOnAtlasTexture->atlasTexture->tileSizeN;
                const auto tilePaddingN = tileOnAtlasTexture->atlasTexture->tilePaddingN;
                const auto nSizeInAtlas = tileSizeN - 2.0f * tilePaddingN;
                PointF nOffsetInTile(colIndex * tileSizeN + tilePaddingN, rowIndex * tileSizeN + tilePaddingN);

                nOffsetInTile += batchedResourceInGPU->nOffsetInTile * nSizeInAtlas;
                const auto nSizeInTile = batchedResourceInGPU->nSizeInTile * nSizeInAtlas;

                glUniform2f(perTile_vs.nOffsetInTile, nOffsetInTile.x,nOffsetInTile.y);
                GL_CHECK_RESULT;
                glUniform2f(perTile_vs.nSizeInTile, nSizeInTile.x, nSizeInTile.y);
                GL_CHECK_RESULT;
            }
            else // if (resourceInGPU->type == GPUAPI::ResourceInGPU::Type::Texture)
            {
                glUniform2f(perTile_vs.nOffsetInTile,
                    batchedResourceInGPU->nOffsetInTile.x,
                    batchedResourceInGPU->nOffsetInTile.y);
                GL_CHECK_RESULT;
                glUniform2f(perTile_vs.nSizeInTile,
                    batchedResourceInGPU->nSizeInTile.x,
                    batchedResourceInGPU->nSizeInTile.y);
                GL_CHECK_RESULT;
            }
        }

        // Single-pass tile rendering always processes full tile
        glDrawElements(GL_TRIANGLES, _rasterTileIndicesCount, GL_UNSIGNED_SHORT, nullptr);
        GL_CHECK_RESULT;
    }
    else
    {
        //TODO: underscale cases are not supported so far, since they require multipass rendering
        assert(false);
    }

    // Disable textures
    const auto usedSamplersCount = batchedLayersCount + (gpuAPI->isSupported_vertexShaderTextureLookup ? 1 : 0);
    for (int samplerIndex = 0; samplerIndex < usedSamplersCount; samplerIndex++)
    {
        glActiveTexture(GL_TEXTURE0 + samplerIndex);
        GL_CHECK_RESULT;

        glBindTexture(GL_TEXTURE_2D, 0);
        GL_CHECK_RESULT;
    }

    // Unbind any binded buffer
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    GL_CHECK_RESULT;

    GL_POP_GROUP_MARKER;

    return true;
}

bool OsmAnd::AtlasMapRendererMapLayersStage_OpenGL::activateRasterLayersProgram(
    const unsigned int numberOfLayersInBatch,
    const int elevationDataSamplerIndex,
    GLlocation& activeElevationVertexAttribArray,
    int& lastUsedProgram)
{
    const auto gpuAPI = getGPUAPI();

    GL_CHECK_PRESENT(glUseProgram);
    GL_CHECK_PRESENT(glUniformMatrix4fv);
    GL_CHECK_PRESENT(glUniform1f);
    GL_CHECK_PRESENT(glUniform1i);
    GL_CHECK_PRESENT(glUniform2f);
    GL_CHECK_PRESENT(glUniform2fv);

    const auto& currentConfiguration = getCurrentConfiguration();
    const auto& internalState = getInternalState();

    const auto& program = _rasterLayerTilePrograms[numberOfLayersInBatch];
    const auto& vao = _rasterTileVAOs[numberOfLayersInBatch];

    if (lastUsedProgram == *program.id)
        return false;

    GL_PUSH_GROUP_MARKER(QString("use '%1-batched-raster-map-layers' program").arg(numberOfLayersInBatch));

    // Disable elevation vertex attrib array (if enabled)
    if (activeElevationVertexAttribArray.isValid())
    {
        glDisableVertexAttribArray(*activeElevationVertexAttribArray);
        GL_CHECK_RESULT;

        activeElevationVertexAttribArray.reset();
    }

    // Set symbol VAO
    gpuAPI->useVAO(vao);

    // Activate program
    glUseProgram(program.id);
    GL_CHECK_RESULT;

    // Set matrices
    glUniformMatrix4fv(program.vs.param.mProjectionView, 1, GL_FALSE, glm::value_ptr(internalState.mPerspectiveProjectionView));
    GL_CHECK_RESULT;

    // Set center offset
    glUniform2f(program.vs.param.targetInTilePosN, internalState.targetInTileOffsetN.x, internalState.targetInTileOffsetN.y);
    GL_CHECK_RESULT;

    if (gpuAPI->isSupported_textureLod)
    {
        // Set distance from camera to target
        glUniform1f(program.vs.param.distanceFromCameraToTarget, internalState.distanceFromCameraToTarget);
        GL_CHECK_RESULT;

        // Set normalized [0.0 .. 1.0] angle of camera elevation
        glUniform1f(program.vs.param.cameraElevationAngleN, currentState.elevationAngle / 90.0f);
        GL_CHECK_RESULT;

        // Set position of camera in ground place
        glUniform2fv(program.vs.param.groundCameraPosition, 1, glm::value_ptr(internalState.groundCameraPosition));
        GL_CHECK_RESULT;

        // Set scale to retain projected size
        glUniform1f(program.vs.param.scaleToRetainProjectedSize, internalState.scaleToRetainProjectedSize);
        GL_CHECK_RESULT;
    }

    // Configure samplers
    auto bitmapTileSamplerType = GPUAPI_OpenGL::SamplerType::BitmapTile_Bilinear;
    if (gpuAPI->isSupported_textureLod)
    {
        switch (currentConfiguration.texturesFilteringQuality)
        {
            case TextureFilteringQuality::Good:
                bitmapTileSamplerType = GPUAPI_OpenGL::SamplerType::BitmapTile_BilinearMipmap;
                break;
            case TextureFilteringQuality::Best:
                bitmapTileSamplerType = GPUAPI_OpenGL::SamplerType::BitmapTile_TrilinearMipmap;
                break;
        }
    }
    for (auto layerLinearIdx = 0u; layerLinearIdx < numberOfLayersInBatch; layerLinearIdx++)
    {
        const auto samplerIndex = layerLinearIdx;

        glUniform1i(program.fs.param.rasterTileLayers[layerLinearIdx].sampler, samplerIndex);
        GL_CHECK_RESULT;

        gpuAPI->setTextureBlockSampler(GL_TEXTURE0 + samplerIndex, bitmapTileSamplerType);
    }
    if (gpuAPI->isSupported_vertexShaderTextureLookup)
    {
        glUniform1i(program.vs.param.elevationData_sampler, elevationDataSamplerIndex);
        GL_CHECK_RESULT;

        gpuAPI->setTextureBlockSampler(GL_TEXTURE0 + elevationDataSamplerIndex, GPUAPI_OpenGL::SamplerType::ElevationDataTile);
    }

    // Configure program for elevation data
    if (currentState.elevationDataProvider == nullptr)
    {
        glUniform1f(program.vs.param.elevationData_scaleFactor, 0.0f);
        GL_CHECK_RESULT;
    }
    if (!gpuAPI->isSupported_vertexShaderTextureLookup)
    {
        glDisableVertexAttribArray(*program.vs.in.vertexElevation);
        GL_CHECK_RESULT;
    }

    lastUsedProgram = program.id;

    GL_POP_GROUP_MARKER;

    return true;
}

std::shared_ptr<const OsmAnd::GPUAPI::ResourceInGPU>
OsmAnd::AtlasMapRendererMapLayersStage_OpenGL::captureElevationDataResource(
    const TileId normalizedTileId,
    const ZoomLevel zoomLevel)
{
    if (!currentState.elevationDataProvider)
        return nullptr;

    const auto& resourcesCollection_ = getResources().getCollectionSnapshot(
        MapRendererResourceType::ElevationData,
        currentState.elevationDataProvider);
    const auto& resourcesCollection =
        std::static_pointer_cast<const MapRendererTiledResourcesCollection::Snapshot>(resourcesCollection_);

    // Obtain tile entry by normalized tile coordinates, since tile may repeat several times
    std::shared_ptr<MapRendererBaseTiledResource> resource_;
    if (resourcesCollection->obtainResource(normalizedTileId, zoomLevel, resource_))
    {
        const auto resource = std::static_pointer_cast<MapRendererElevationDataResource>(resource_);

        // Check state and obtain GPU resource
        if (resource->setStateIf(MapRendererResourceState::Uploaded, MapRendererResourceState::IsBeingUsed))
        {
            // Capture GPU resource
            const auto gpuResource = resource->resourceInGPU;

            resource->setState(MapRendererResourceState::Uploaded);

            return gpuResource;
        }
    }

    return nullptr;
}

std::shared_ptr<const OsmAnd::GPUAPI::ResourceInGPU> OsmAnd::AtlasMapRendererMapLayersStage_OpenGL::captureLayerResource(
    const std::shared_ptr<const IMapRendererResourcesCollection>& resourcesCollection_,
    const TileId normalizedTileId,
    const ZoomLevel zoomLevel)
{
    const auto& resourcesCollection =
        std::static_pointer_cast<const MapRendererTiledResourcesCollection::Snapshot>(resourcesCollection_);

    // Obtain tile entry by normalized tile coordinates, since tile may repeat several times
    std::shared_ptr<MapRendererBaseTiledResource> resource_;
    if (resourcesCollection->obtainResource(normalizedTileId, zoomLevel, resource_))
    {
        const auto resource = std::static_pointer_cast<MapRendererElevationDataResource>(resource_);

        // Check state and obtain GPU resource
        if (resource->setStateIf(MapRendererResourceState::Uploaded, MapRendererResourceState::IsBeingUsed))
        {
            // Capture GPU resource
            const auto gpuResource = resource->resourceInGPU;

            resource->setState(MapRendererResourceState::Uploaded);

            return gpuResource;
        }
    }

    return nullptr;
}

bool OsmAnd::AtlasMapRendererMapLayersStage_OpenGL::releaseRasterLayers()
{
    GL_CHECK_PRESENT(glDeleteProgram);

    _maxNumberOfRasterMapLayersInBatch = 0;

    releaseRasterTile();

    for (auto& rasterLayerTileProgram : _rasterLayerTilePrograms)
    {
        if (rasterLayerTileProgram.id.isValid())
        {
            glDeleteProgram(rasterLayerTileProgram.id);
            GL_CHECK_RESULT;
            rasterLayerTileProgram = RasterLayerTileProgram();
        }
    }
    _rasterLayerTilePrograms.clear();

    return true;
}

void OsmAnd::AtlasMapRendererMapLayersStage_OpenGL::initializeRasterTile()
{
    const auto gpuAPI = getGPUAPI();

    GL_CHECK_PRESENT(glGenBuffers);
    GL_CHECK_PRESENT(glBindBuffer);
    GL_CHECK_PRESENT(glBufferData);
    GL_CHECK_PRESENT(glEnableVertexAttribArray);
    GL_CHECK_PRESENT(glVertexAttribPointer);

#pragma pack(push, 1)
    struct Vertex
    {
        GLfloat positionXZ[2];
        GLfloat textureUV[2];
    };
#pragma pack(pop)

    Vertex* pVertices = nullptr;
    GLsizei verticesCount = 0;
    GLushort* pIndices = nullptr;
    GLsizei indicesCount = 0;

    const auto heixelsPerTileSide = 1u << MapRenderer::MaxMissingDataZoomShift;

    // Complex tile patch, that consists of (heightPrimitivesPerSide*heightPrimitivesPerSide) number of
    // height clusters. Height cluster itself consists of 4 vertices, 6 indices and 2 polygons
    const auto heightPrimitivesPerSide = heixelsPerTileSide - 1;
    const GLfloat clusterSize =
        static_cast<GLfloat>(AtlasMapRenderer::TileSize3D) / static_cast<float>(heightPrimitivesPerSide);
    verticesCount = heixelsPerTileSide * heixelsPerTileSide;
    pVertices = new Vertex[verticesCount];
    indicesCount = (heightPrimitivesPerSide * heightPrimitivesPerSide) * 6;
    pIndices = new GLushort[indicesCount];

    Vertex* pV = pVertices;

    // Form vertices
    assert(verticesCount <= std::numeric_limits<GLushort>::max());
    for (auto row = 0u, count = heixelsPerTileSide; row < count; row++)
    {
        for (auto col = 0u, count = heixelsPerTileSide; col < count; col++, pV++)
        {
            pV->positionXZ[0] = static_cast<float>(col)* clusterSize;
            pV->positionXZ[1] = static_cast<float>(row)* clusterSize;

            pV->textureUV[0] = static_cast<float>(col) / static_cast<float>(heightPrimitivesPerSide);
            pV->textureUV[1] = static_cast<float>(row) / static_cast<float>(heightPrimitivesPerSide);
        }
    }

    // Form indices
    GLushort* pI = pIndices;
    for (auto row = 0u; row < heightPrimitivesPerSide; row++)
    {
        for (auto col = 0u; col < heightPrimitivesPerSide; col++)
        {
            const auto p0 = (row + 1) * heixelsPerTileSide + col + 0;//BL
            const auto p1 = (row + 0) * heixelsPerTileSide + col + 0;//TL
            const auto p2 = (row + 0) * heixelsPerTileSide + col + 1;//TR
            const auto p3 = (row + 1) * heixelsPerTileSide + col + 1;//BR
            assert(p0 <= verticesCount);
            assert(p1 <= verticesCount);
            assert(p2 <= verticesCount);
            assert(p3 <= verticesCount);

            // Triangle 0
            pI[0] = p0;
            pI[1] = p1;
            pI[2] = p2;
            pI += 3;

            // Triangle 1
            pI[0] = p0;
            pI[1] = p2;
            pI[2] = p3;
            pI += 3;
        }
    }

    // Create VBO
    glGenBuffers(1, &_rasterTileVBO);
    GL_CHECK_RESULT;
    glBindBuffer(GL_ARRAY_BUFFER, _rasterTileVBO);
    GL_CHECK_RESULT;
    glBufferData(GL_ARRAY_BUFFER, verticesCount * sizeof(Vertex), pVertices, GL_STATIC_DRAW);
    GL_CHECK_RESULT;
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    GL_CHECK_RESULT;

    // Create IBO
    glGenBuffers(1, &_rasterTileIBO);
    GL_CHECK_RESULT;
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _rasterTileIBO);
    GL_CHECK_RESULT;
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indicesCount * sizeof(GLushort), pIndices, GL_STATIC_DRAW);
    GL_CHECK_RESULT;
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    GL_CHECK_RESULT;

    for (auto numberOfLayersInBatch = _maxNumberOfRasterMapLayersInBatch; numberOfLayersInBatch >= 1; numberOfLayersInBatch--)
    {
        auto& rasterTileVAO = _rasterTileVAOs[numberOfLayersInBatch];
        const auto& rasterLayerTileProgram = constOf(_rasterLayerTilePrograms)[numberOfLayersInBatch];

        rasterTileVAO = gpuAPI->allocateUninitializedVAO();

        // Bind IBO to VAO
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _rasterTileIBO);
        GL_CHECK_RESULT;

        // Bind VBO to VAO
        glBindBuffer(GL_ARRAY_BUFFER, _rasterTileVBO);
        GL_CHECK_RESULT;

        glEnableVertexAttribArray(*rasterLayerTileProgram.vs.in.vertexPosition);
        GL_CHECK_RESULT;
        glVertexAttribPointer(*rasterLayerTileProgram.vs.in.vertexPosition,
            2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<GLvoid*>(offsetof(Vertex, positionXZ)));
        GL_CHECK_RESULT;
        glEnableVertexAttribArray(*rasterLayerTileProgram.vs.in.vertexTexCoords);
        GL_CHECK_RESULT;
        glVertexAttribPointer(*rasterLayerTileProgram.vs.in.vertexTexCoords,
            2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<GLvoid*>(offsetof(Vertex, textureUV)));
        GL_CHECK_RESULT;

        gpuAPI->initializeVAO(rasterTileVAO);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        GL_CHECK_RESULT;
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        GL_CHECK_RESULT;
    }

    _rasterTileIndicesCount = indicesCount;

    delete[] pVertices;
    delete[] pIndices;
}

void OsmAnd::AtlasMapRendererMapLayersStage_OpenGL::releaseRasterTile()
{
    const auto gpuAPI = getGPUAPI();

    GL_CHECK_PRESENT(glDeleteBuffers);

    for (auto& rasterTileVAO : _rasterTileVAOs)
    {
        if (rasterTileVAO.isValid())
        {
            gpuAPI->releaseVAO(rasterTileVAO);
            rasterTileVAO.reset();
        }
    }

    if (_rasterTileIBO.isValid())
    {
        glDeleteBuffers(1, &_rasterTileIBO);
        GL_CHECK_RESULT;
        _rasterTileIBO.reset();
    }
    if (_rasterTileVBO.isValid())
    {
        glDeleteBuffers(1, &_rasterTileVBO);
        GL_CHECK_RESULT;
        _rasterTileVBO.reset();
    }
    _rasterTileIndicesCount = -1;
}

void OsmAnd::AtlasMapRendererMapLayersStage_OpenGL::configureElevationData(
    const RasterLayerTileProgram& program,
    const int elevationDataSamplerIndex,
    const TileId tileId,
    GLlocation& activeElevationVertexAttribArray)
{
    const auto gpuAPI = getGPUAPI();

    const auto tileIdN = Utilities::normalizeTileId(tileId, currentState.zoomBase);
    const auto elevationDataResource = captureElevationDataResource(tileIdN, currentState.zoomBase);

    auto configuredElevationVertexAttribArray = false;
    if (currentState.elevationDataProvider != nullptr)
    {
        if (!elevationDataResource)
        {
            // We have no elevation data, so we can not do anything
            glUniform1f(program.vs.param.elevationData_scaleFactor, 0.0f);
            GL_CHECK_RESULT;
        }
        else
        {
            glUniform1f(program.vs.param.elevationData_scaleFactor, currentState.elevationDataConfiguration.scaleFactor);
            GL_CHECK_RESULT;

            const auto upperMetersPerUnit = Utilities::getMetersPerTileUnit(
                currentState.zoomBase,
                tileIdN.y,
                AtlasMapRenderer::TileSize3D);
            glUniform1f(program.vs.param.elevationData_upperMetersPerUnit, upperMetersPerUnit);
            const auto lowerMetersPerUnit = Utilities::getMetersPerTileUnit(
                currentState.zoomBase,
                tileIdN.y + 1,
                AtlasMapRenderer::TileSize3D);
            glUniform1f(program.vs.param.elevationData_lowerMetersPerUnit, lowerMetersPerUnit);

            const auto& perTile_vs = program.vs.param.elevationDataLayer;

            if (gpuAPI->isSupported_vertexShaderTextureLookup)
            {
                glActiveTexture(GL_TEXTURE0 + elevationDataSamplerIndex);
                GL_CHECK_RESULT;

                glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(reinterpret_cast<intptr_t>(elevationDataResource->refInGPU)));
                GL_CHECK_RESULT;

                gpuAPI->applyTextureBlockToTexture(GL_TEXTURE_2D, GL_TEXTURE0);

                if (elevationDataResource->type == GPUAPI::ResourceInGPU::Type::SlotOnAtlasTexture)
                {
                    const auto tileOnAtlasTexture =
                        std::static_pointer_cast<const GPUAPI::SlotOnAtlasTextureInGPU>(elevationDataResource);

                    const auto rowIndex = tileOnAtlasTexture->slotIndex / tileOnAtlasTexture->atlasTexture->slotsPerSide;
                    const auto colIndex = tileOnAtlasTexture->slotIndex - rowIndex * tileOnAtlasTexture->atlasTexture->slotsPerSide;
                    const auto tileSizeN = tileOnAtlasTexture->atlasTexture->tileSizeN;
                    const auto tilePaddingN = tileOnAtlasTexture->atlasTexture->uHalfTexelSizeN;
                    const auto nSizeInTile = tileSizeN - 2.0f * tilePaddingN;
                    const PointF nOffsetInTile(colIndex * tileSizeN + tilePaddingN, rowIndex * tileSizeN + tilePaddingN);

                    glUniform2f(perTile_vs.nOffsetInTile, nOffsetInTile.x, nOffsetInTile.y);
                    GL_CHECK_RESULT;
                    glUniform2f(perTile_vs.nSizeInTile, nSizeInTile, nSizeInTile);
                    GL_CHECK_RESULT;
                }
                else // if (elevationDataResource->type == GPUAPI::ResourceInGPU::Type::Texture)
                {
                    const auto& texture = std::static_pointer_cast<const GPUAPI::TextureInGPU>(elevationDataResource);

                    const auto nSizeInTile = 1.0f - 2.0f * texture->uHalfTexelSizeN;
                    const PointF nOffsetInTile(texture->uHalfTexelSizeN, texture->uHalfTexelSizeN);

                    glUniform2f(perTile_vs.nOffsetInTile, nOffsetInTile.x, nOffsetInTile.y);
                    GL_CHECK_RESULT;
                    glUniform2f(perTile_vs.nSizeInTile, nSizeInTile, nSizeInTile);
                    GL_CHECK_RESULT;
                }
            }
            else
            {
                assert(elevationDataResource->type == GPUAPI::ResourceInGPU::Type::ArrayBuffer);

                const auto& arrayBuffer = std::static_pointer_cast<const GPUAPI::ArrayBufferInGPU>(elevationDataResource);
                assert(arrayBuffer->itemsCount ==
                    (1u << MapRenderer::MaxMissingDataZoomShift)*(1u << MapRenderer::MaxMissingDataZoomShift));

                if (!activeElevationVertexAttribArray.isValid())
                {
                    glEnableVertexAttribArray(*program.vs.in.vertexElevation);
                    GL_CHECK_RESULT;

                    activeElevationVertexAttribArray = program.vs.in.vertexElevation;
                }

                glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(reinterpret_cast<intptr_t>(elevationDataResource->refInGPU)));
                GL_CHECK_RESULT;

                glVertexAttribPointer(*program.vs.in.vertexElevation, 1, GL_FLOAT, GL_FALSE, sizeof(float), nullptr);
                GL_CHECK_RESULT;

                configuredElevationVertexAttribArray = true;
            }
        }
    }
    if (activeElevationVertexAttribArray.isValid() && !configuredElevationVertexAttribArray)
    {
        // In case for this tile there was no elevation data, but vertex attrib array is enabled, disable it
        glDisableVertexAttribArray(*activeElevationVertexAttribArray);
        GL_CHECK_RESULT;

        activeElevationVertexAttribArray.reset();
    }
}

QList< OsmAnd::Ref<OsmAnd::AtlasMapRendererMapLayersStage_OpenGL::PerTileBatchedLayers> >
OsmAnd::AtlasMapRendererMapLayersStage_OpenGL::batchLayersByTiles(const AtlasMapRendererInternalState& internalState)
{
    const auto gpuAPI = getGPUAPI();

    QList< Ref<PerTileBatchedLayers> > perTileBatchedLayers;

    for (const auto& tileId : constOf(internalState.visibleTiles))
    {
        const auto tileIdN = Utilities::normalizeTileId(tileId, currentState.zoomBase);

        Ref<PerTileBatchedLayers> batch = new PerTileBatchedLayers(tileId, true);
        perTileBatchedLayers.push_back(batch);

        for (const auto& mapLayerEntry : rangeOf(constOf(currentState.mapLayersProviders)))
        {
            const auto layerIndex = mapLayerEntry.key();
            const auto& provider = mapLayerEntry.value();
            const auto resourcesCollection = getResources().getCollectionSnapshot(
                MapRendererResourceType::MapLayer,
                std::dynamic_pointer_cast<IMapDataProvider>(provider));

            // In case there's no resources collection for this provider, there's nothing to do here, move on
            if (!resourcesCollection)
                continue;

            Ref<BatchedLayer> batchedLayer = new BatchedLayer(layerIndex);
            if (const auto gpuResource = captureLayerResource(resourcesCollection, tileIdN, currentState.zoomBase))
            {
                // Exact match, no zoom shift or offset
                batchedLayer->resourcesInGPU.push_back(Ref<BatchedLayerResource>(
                    new BatchedLayerResource(gpuResource)));
            }
            else if (Q_LIKELY(!debugSettings->rasterLayersOverscaleForbidden || !debugSettings->rasterLayersUnderscaleForbidden))
            {
                // Exact match was not found, so now try to look for overscaled/underscaled resources, taking into account
                // MaxMissingDataZoomShift and current zoom. It's better to show Z-"nearest" resource available,
                // giving preference to underscaled resource
                for (int absZoomShift = 1; absZoomShift <= MapRenderer::MaxMissingDataZoomShift; absZoomShift++)
                {
                    //TODO: Try to find underscaled first (that is, currentState.zoomBase + 1). Only full match is accepted
                    
                    // If underscaled was not found, look for overscaled (surely, if such zoom level exists at all)
                    if (!debugSettings->rasterLayersOverscaleForbidden)
                    {
                        const auto overscaleZoom = static_cast<int>(currentState.zoomBase) - absZoomShift;
                        if (overscaleZoom >= static_cast<int>(MinZoomLevel))
                        {
                            PointF nOffsetInTile;
                            PointF nSizeInTile;
                            const auto overscaledTileIdN = Utilities::getTileIdOverscaledByZoomShift(
                                tileIdN,
                                -absZoomShift,
                                &nOffsetInTile,
                                &nSizeInTile);
                            if (const auto gpuResource = captureLayerResource(
                                resourcesCollection,
                                overscaledTileIdN,
                                static_cast<ZoomLevel>(overscaleZoom)))
                            {
                                batchedLayer->resourcesInGPU.push_back(Ref<BatchedLayerResource>(new BatchedLayerResource(
                                    gpuResource,
                                    -absZoomShift,
                                    nOffsetInTile,
                                    nSizeInTile)));
                                break;
                            }
                        }
                    }
                }
            }
            if (!batchedLayer || batchedLayer->resourcesInGPU.isEmpty())
                continue;

            // Only raster layers can be batched, while if there's no previous
            bool canBeBatched = true;
            if (!batch->layers.isEmpty())
            {
                const auto& lastBatchedLayer = batch->layers.last();
                const auto& previousProvider = currentState.mapLayersProviders[lastBatchedLayer->layerIndex];

                // Only raster layers can be batched
                const auto previousProviderIsRaster =
                    (std::dynamic_pointer_cast<IRasterMapLayerProvider>(previousProvider) != nullptr);
                const auto currentProviderIsRaster =
                    (std::dynamic_pointer_cast<IRasterMapLayerProvider>(provider) != nullptr);
                canBeBatched = (previousProviderIsRaster && currentProviderIsRaster);

                // Number of batched raster layers is limited
                canBeBatched = canBeBatched && (batch->layers.size() < _maxNumberOfRasterMapLayersInBatch);

                // Batching is possible only if all BatchedLayerResources are compatible
                if (canBeBatched)
                    canBeBatched = (batchedLayer->resourcesInGPU.size() == lastBatchedLayer->resourcesInGPU.size());
                if (canBeBatched)
                {
                    for (const auto& batchedLayerResource : constOf(batchedLayer->resourcesInGPU))
                    {
                        canBeBatched = std::any_of(lastBatchedLayer->resourcesInGPU.cbegin(), lastBatchedLayer->resourcesInGPU.cend(),
                            [batchedLayerResource]
                            (const Ref<BatchedLayerResource>& otherBatchedLayerResource)
                            {
                                return batchedLayerResource->canBeBatchedWith(*otherBatchedLayerResource);
                            });
                        if (!canBeBatched)
                            break;
                    }
                }
            }

            if (!canBeBatched)
            {
                batch = new PerTileBatchedLayers(tileId, true);
                perTileBatchedLayers.push_back(batch);
            }
            batch->layers.push_back(qMove(batchedLayer));
        }

        // If there are no resources inside batch (and that batch is the only one),
        // insert an "unavailable" stub for first provider
        if (batch->layers.isEmpty())
        {
            Ref<BatchedLayer> batchedLayer = new BatchedLayer(currentState.mapLayersProviders.firstKey());
            batchedLayer->resourcesInGPU.push_back(Ref<BatchedLayerResource>(
                new BatchedLayerResource(getResources().unavailableTileStub)));
            batch->layers.push_back(qMove(batchedLayer));
        }
    }

    // Finally sort per-tile batched layers, so that batches were rendered by layer indices order
    std::sort(perTileBatchedLayers.begin(), perTileBatchedLayers.end(),
        []
        (const Ref<PerTileBatchedLayers>& l, const Ref<PerTileBatchedLayers>& r) -> bool
        {
            return *l < *r;
        });

    return perTileBatchedLayers;
}

OsmAnd::AtlasMapRendererMapLayersStage_OpenGL::BatchedLayerResource::BatchedLayerResource(
    const std::shared_ptr<const GPUAPI::ResourceInGPU>& resourceInGPU_,
    const int zoomShift_ /*= 0*/,
    const PointF nOffsetInTile_ /*= PointF(0.0f, 0.0f)*/,
    const PointF nSizeInTile_ /*= PointF(1.0f, 1.0f)*/)
    : resourceInGPU(resourceInGPU_)
    , zoomShift(zoomShift_)
    , nOffsetInTile(nOffsetInTile_)
    , nSizeInTile(nSizeInTile_)
{
}

bool OsmAnd::AtlasMapRendererMapLayersStage_OpenGL::BatchedLayerResource::canBeBatchedWith(
    const BatchedLayerResource& that) const
{
    return
        zoomShift == that.zoomShift &&
        nOffsetInTile == that.nOffsetInTile &&
        nSizeInTile == that.nSizeInTile;
}

OsmAnd::AtlasMapRendererMapLayersStage_OpenGL::BatchedLayer::BatchedLayer(
    const int layerIndex_)
    : layerIndex(layerIndex_)
{
}

OsmAnd::AtlasMapRendererMapLayersStage_OpenGL::PerTileBatchedLayers::PerTileBatchedLayers(
    const TileId tileId_,
    const bool containsOriginLayer_)
    : tileId(tileId_)
    , containsOriginLayer(containsOriginLayer_)
{
}

bool OsmAnd::AtlasMapRendererMapLayersStage_OpenGL::PerTileBatchedLayers::operator<(const PerTileBatchedLayers& that) const
{
    if (this == &that)
        return false;

    return layers.first()->layerIndex < that.layers.first()->layerIndex;
}