#include "MapPrimitivesProvider_P.h"
#include "MapPrimitivesProvider.h"

//#define OSMAND_PERFORMANCE_METRICS 2
#if !defined(OSMAND_PERFORMANCE_METRICS)
#   define OSMAND_PERFORMANCE_METRICS 0
#endif // !defined(OSMAND_PERFORMANCE_METRICS)

#include "IMapObjectsProvider.h"
#include "Stopwatch.h"
#include "Utilities.h"
#include "Logging.h"

OsmAnd::MapPrimitivesProvider_P::MapPrimitivesProvider_P(MapPrimitivesProvider* owner_)
    : _primitiviserCache(new MapPrimitiviser::Cache())
    , owner(owner_)
{
}

OsmAnd::MapPrimitivesProvider_P::~MapPrimitivesProvider_P()
{
}

bool OsmAnd::MapPrimitivesProvider_P::obtainData(
    const TileId tileId,
    const ZoomLevel zoom,
    std::shared_ptr<MapPrimitivesProvider::Data>& outTiledData,
    MapPrimitivesProvider_Metrics::Metric_obtainData* const metric_,
    const IQueryController* const queryController)
{
#if OSMAND_PERFORMANCE_METRICS
    MapPrimitivesProvider_Metrics::Metric_obtainData localMetric;
    const auto metric = metric_ ? metric_ : &localMetric;
#else
    const auto metric = metric_;
#endif

    const Stopwatch totalStopwatch(metric != nullptr);

    std::shared_ptr<TileEntry> tileEntry;

    for (;;)
    {
        // Try to obtain previous instance of tile
        _tileReferences.obtainOrAllocateEntry(tileEntry, tileId, zoom,
            []
            (const TiledEntriesCollection<TileEntry>& collection, const TileId tileId, const ZoomLevel zoom) -> TileEntry*
            {
                return new TileEntry(collection, tileId, zoom);
            });

        // If state is "Undefined", change it to "Loading" and proceed with loading
        if (tileEntry->setStateIf(TileState::Undefined, TileState::Loading))
            break;

        // In case tile entry is being loaded, wait until it will finish loading
        if (tileEntry->getState() == TileState::Loading)
        {
            QReadLocker scopedLcoker(&tileEntry->loadedConditionLock);

            // If tile is in 'Loading' state, wait until it will become 'Loaded'
            while (tileEntry->getState() != TileState::Loaded)
                REPEAT_UNTIL(tileEntry->loadedCondition.wait(&tileEntry->loadedConditionLock));
        }

        if (!tileEntry->dataIsPresent)
        {
            // If there was no data, return same
            outTiledData.reset();
            return true;
        }
        else
        {
            // Otherwise, try to lock tile reference
            outTiledData = tileEntry->dataWeakRef.lock();

            // If successfully locked, just return it
            if (outTiledData)
                return true;

            // Otherwise consider this tile entry as expired, remove it from collection (it's safe to do that right now)
            // This will enable creation of new entry on next loop cycle
            _tileReferences.removeEntry(tileId, zoom);
            tileEntry.reset();
        }
    }

    const Stopwatch totalTimeStopwatch(
#if OSMAND_PERFORMANCE_METRICS
        true
#else
        metric != nullptr
#endif // OSMAND_PERFORMANCE_METRICS
        );

    // Obtain map objects data tile
    std::shared_ptr<IMapObjectsProvider::Data> dataTile;
    std::shared_ptr<Metric> submetric;
    owner->mapObjectsProvider->obtainData(
        tileId,
        zoom,
        dataTile,
        metric ? &submetric : nullptr,
        nullptr);
    if (metric && submetric)
        metric->addOrReplaceSubmetric(submetric);
    if (!dataTile)
    {
        // Store flag that there was no data and mark tile entry as 'Loaded'
        tileEntry->dataIsPresent = false;
        tileEntry->setState(TileState::Loaded);

        // Notify that tile has been loaded
        {
            QWriteLocker scopedLcoker(&tileEntry->loadedConditionLock);
            tileEntry->loadedCondition.wakeAll();
        }

        outTiledData.reset();
        return true;
    }

    // Get primitivised objects
    std::shared_ptr<MapPrimitiviser::PrimitivisedObjects> primitivisedObjects;
    if (owner->mode == MapPrimitivesProvider::Mode::AllObjectsWithoutPolygonFiltering)
    {
        primitivisedObjects = owner->primitiviser->primitiviseAllMapObjects(
            zoom,
            dataTile->mapObjects,
            //NOTE: So far it's safe to turn off this cache. But it has to be rewritten. Since lock/unlock occurs too often, this kills entire performance
            //NOTE: Maybe a QuadTree-based cache with leaf-only locking will save up much. Or use supernodes, like DataBlock
            nullptr, //_primitiviserCache,
            nullptr,
            metric ? metric->findOrAddSubmetricOfType<MapPrimitiviser_Metrics::Metric_primitiviseAllMapObjects>().get() : nullptr);
    }
    else if (owner->mode == MapPrimitivesProvider::Mode::AllObjectsWithPolygonFiltering)
    {
        primitivisedObjects = owner->primitiviser->primitiviseAllMapObjects(
            Utilities::getScaleDivisor31ToPixel(PointI(owner->tileSize, owner->tileSize), zoom),
            zoom,
            dataTile->mapObjects,
            //NOTE: So far it's safe to turn off this cache. But it has to be rewritten. Since lock/unlock occurs too often, this kills entire performance
            //NOTE: Maybe a QuadTree-based cache with leaf-only locking will save up much. Or use supernodes, like DataBlock
            nullptr, //_primitiviserCache,
            nullptr,
            metric ? metric->findOrAddSubmetricOfType<MapPrimitiviser_Metrics::Metric_primitiviseAllMapObjects>().get() : nullptr);
    }
    else if (owner->mode == MapPrimitivesProvider::Mode::WithoutSurface)
    {
        primitivisedObjects = owner->primitiviser->primitiviseWithoutSurface(
            Utilities::getScaleDivisor31ToPixel(PointI(owner->tileSize, owner->tileSize), zoom),
            zoom,
            dataTile->mapObjects,
            //NOTE: So far it's safe to turn off this cache. But it has to be rewritten. Since lock/unlock occurs too often, this kills entire performance
            //NOTE: Maybe a QuadTree-based cache with leaf-only locking will save up much. Or use supernodes, like DataBlock
            nullptr, //_primitiviserCache,
            nullptr,
            metric ? metric->findOrAddSubmetricOfType<MapPrimitiviser_Metrics::Metric_primitiviseWithoutSurface>().get() : nullptr);
    }
    else // if (owner->mode == MapPrimitivesProvider::Mode::WithSurface)
    {
        const auto tileBBox31 = Utilities::tileBoundingBox31(tileId, zoom);
        primitivisedObjects = owner->primitiviser->primitiviseWithSurface(
            tileBBox31,
            PointI(owner->tileSize, owner->tileSize),
            zoom,
            dataTile->tileSurfaceType,
            dataTile->mapObjects,
            //NOTE: So far it's safe to turn off this cache. But it has to be rewritten. Since lock/unlock occurs too often, this kills entire performance
            //NOTE: Maybe a QuadTree-based cache with leaf-only locking will save up much. Or use supernodes, like DataBlock
            nullptr, //_primitiviserCache,
            nullptr,
            metric ? metric->findOrAddSubmetricOfType<MapPrimitiviser_Metrics::Metric_primitiviseWithSurface>().get() : nullptr);
    }

    // Create tile
    const std::shared_ptr<MapPrimitivesProvider::Data> newTiledData(new MapPrimitivesProvider::Data(
        tileId,
        zoom,
        dataTile,
        primitivisedObjects,
        new RetainableCacheMetadata(tileEntry, dataTile->retainableCacheMetadata)));

    // Publish new tile
    outTiledData = newTiledData;

    // Store weak reference to new tile and mark it as 'Loaded'
    tileEntry->dataIsPresent = true;
    tileEntry->dataWeakRef = newTiledData;
    tileEntry->setState(TileState::Loaded);

    // Notify that tile has been loaded
    {
        QWriteLocker scopedLcoker(&tileEntry->loadedConditionLock);
        tileEntry->loadedCondition.wakeAll();
    }

    if (metric)
        metric->elapsedTime = totalStopwatch.elapsed();

#if OSMAND_PERFORMANCE_METRICS
#if OSMAND_PERFORMANCE_METRICS <= 1
    LogPrintf(LogSeverityLevel::Info,
        "%d polygons, %d polylines, %d points primitivised from %dx%d@%d in %fs",
        primitivisedObjects->polygons.size(),
        primitivisedObjects->polylines.size(),
        primitivisedObjects->polygons.size(),
        tileId.x,
        tileId.y,
        zoom,
        totalStopwatch.elapsed());
#else
    LogPrintf(LogSeverityLevel::Info,
        "%d polygons, %d polylines, %d points primitivised from %dx%d@%d in %fs:\n%s",
        primitivisedObjects->polygons.size(),
        primitivisedObjects->polylines.size(),
        primitivisedObjects->polygons.size(),
        tileId.x,
        tileId.y,
        zoom,
        totalStopwatch.elapsed(),
        qPrintable(metric ? metric->toString(QLatin1String("\t - ")) : QLatin1String("(null)")));
#endif // OSMAND_PERFORMANCE_METRICS <= 1
#endif // OSMAND_PERFORMANCE_METRICS
    
    return true;
}

OsmAnd::MapPrimitivesProvider_P::RetainableCacheMetadata::RetainableCacheMetadata(
    const std::shared_ptr<TileEntry>& tileEntry,
    const std::shared_ptr<const IMapDataProvider::RetainableCacheMetadata>& binaryMapRetainableCacheMetadata_)
    : tileEntryWeakRef(tileEntry)
    , binaryMapRetainableCacheMetadata(binaryMapRetainableCacheMetadata_)
{
}

OsmAnd::MapPrimitivesProvider_P::RetainableCacheMetadata::~RetainableCacheMetadata()
{
    // Remove tile reference from collection. All checks here does not matter,
    // since entry->tile reference is already expired (execution is already in destructor of OfflineMapDataTile!)
    if (const auto tileEntry = tileEntryWeakRef.lock())
    {
        if (const auto link = tileEntry->link.lock())
            link->collection.removeEntry(tileEntry->tileId, tileEntry->zoom);
    }
}
