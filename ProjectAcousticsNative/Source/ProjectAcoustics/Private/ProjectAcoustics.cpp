// Copyright (c) 2022 Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// The Project Acoustics module

#include "ProjectAcoustics.h"
#include "IAcoustics.h"
#include "AcousticsDebugRender.h"
#include "AcousticsParameterBlend.h"

using namespace TritonRuntime;

#define LOCTEXT_NAMESPACE "FProjectAcousticsModule"
DEFINE_LOG_CATEGORY(LogAcousticsRuntime)

DEFINE_STAT(STAT_Acoustics_UpdateObjectParams);
DEFINE_STAT(STAT_Acoustics_Query);
DEFINE_STAT(STAT_Acoustics_QueryOutdoorness);
DEFINE_STAT(STAT_Acoustics_LoadRegion);
DEFINE_STAT(STAT_Acoustics_LoadAce);
DEFINE_STAT(STAT_Acoustics_ClearAce);

// Safety margin for ACE streaming loads.
// When player gets to within this fraction of the loaded region's border,
// a new region is loaded, centered at the player.
// 0 is extremely safe but lots of I/O, 1 is no safety.
float c_AceTileLoadMargin = 0.8f;
// Console control of changing the load margin
static FAutoConsoleVariableRef CVarAcousticsAceTileLoadMargin(
    TEXT("PA.AceTileLoadMargin"), c_AceTileLoadMargin,
    TEXT("Safety margin for ACE streaming loads.\n")
        TEXT("When player gets to within this fraction of the loaded region's border,\n")
            TEXT("a new region is loaded, centered at the player.\n")
                TEXT("0 is extremely safe but lots of I/O, 1 is no safety.\n"),
    ECVF_Default);

// Computed outdoorness is 0 only if player is completely enclosed
// and 1 only when player is standing on a flat plane with no other geometry.
// These constants bring the range closer to practically observed values.
// Tune as necessary.
constexpr float c_OutdoornessIndoors = 0.02f;
constexpr float c_OutdoornessOutdoors = 1.0f;

// Triton's debug interface lets you query things like the voxel display and probe stats
// This is very helpful during development, but shouldn't be used when the game is shipped
#if !UE_BUILD_SHIPPING
constexpr bool c_UseTritonDebugInterface = true;
#else
constexpr bool c_UseTritonDebugInterface = false;
#endif

FProjectAcousticsModule::FProjectAcousticsModule()
    : m_Triton(nullptr)
    , m_AceFileLoaded(false)
    , m_LastLoadCenterPosition(0, 0, 0)
    , m_LastLoadTileSize(0, 0, 0)
    , m_PreviousTriton(nullptr)
    , m_PreviousAceFileLoaded(false)
    , m_IsAceCrossfadeActive(false)
    , m_AceCrossfadeDurationSeconds(0.0f)
    , m_AceCrossfadeElapsedSeconds(0.0f)
    , m_IsOutdoornessStale(true)
    , m_CachedOutdoorness(0)
    , m_GlobalDesign(FAcousticsDesignParams::Default())
    , m_NumRunningTasks(0)
{
#if !UE_BUILD_SHIPPING
    m_IsEnabled = true;
#endif
    // Create a threadpool of 1, so that we know that all queries will happen one at a time, from a single thread
    m_ThreadPool = FQueuedThreadPool::Allocate();
    m_ThreadPool->Create(1);
}

void FProjectAcousticsModule::StartupModule()
{
    m_TritonMemHook = TUniquePtr<FTritonMemHook>(new FTritonMemHook());
    m_TritonLogHook = TUniquePtr<FTritonLogHook>(new FTritonLogHook());
    auto initSuccess = TritonAcoustics::Init(m_TritonMemHook.Get(), m_TritonLogHook.Get());
    if (!initSuccess)
    {
        UE_LOG(LogAcousticsRuntime, Error, TEXT("Project Acoustics failed to initialize!"));
        return;
    }

    m_Triton = c_UseTritonDebugInterface ? TritonAcousticsDebug::CreateInstance() : TritonAcoustics::CreateInstance();

    if (!m_Triton)
    {
        UE_LOG(LogAcousticsRuntime, Error, TEXT("Project Acoustics failed to create instance!"));
        return;
    }
    m_SpaceTransform = FTransform::Identity;
    m_InverseSpaceTransform = m_SpaceTransform.Inverse();

#if !UE_BUILD_SHIPPING
    // setup debug rendering for ourself
    m_DebugRenderer.Reset(new FProjectAcousticsDebugRender(this));
#endif
}

void FProjectAcousticsModule::ShutdownModule()
{
    // This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
    // we call this function before unloading the module.
    if (m_Triton)
    {
        // Make sure there are no lingering background queries still running
        WaitForRunningTasks();

        ClearPreviousRuntime();

        TritonAcoustics::DestroyInstance(m_Triton);
        TritonAcoustics::TearDown();
        m_Triton = nullptr;

#if !UE_BUILD_SHIPPING
        m_DebugRenderer.Reset();
#endif
    }
}

bool FProjectAcousticsModule::LoadAceFileIntoCurrentRuntime(const FString& filePath, const float cacheScale)
{
    if (!m_Triton)
    {
        return false;
    }

    auto fullFilePath = FPaths::ProjectDir() + filePath;
    {
        SCOPE_CYCLE_COUNTER(STAT_Acoustics_LoadAce);
        m_TritonIOHook = TUniquePtr<FTritonUnrealIOHook>(new FTritonUnrealIOHook());
        if (!m_TritonIOHook->OpenForRead(TCHAR_TO_ANSI(*fullFilePath)))
        {
            m_TritonIOHook.Reset();
            UE_LOG(LogAcousticsRuntime, Error, TEXT("Failed to open ACE file for reading: [%s]"), *fullFilePath);
            return false;
        }

        m_TritonTaskHook = TUniquePtr<FTritonAsyncTaskHook>(new FTritonAsyncTaskHook());
        if (!m_Triton->InitLoad(m_TritonIOHook.Get(), m_TritonTaskHook.Get(), cacheScale))
        {
            UE_LOG(LogAcousticsRuntime, Error, TEXT("Failed to load ACE file: [%s]"), *fullFilePath);
            m_TritonTaskHook.Reset();
            m_TritonIOHook.Reset();
            return false;
        }
    }

    m_AceFileLoaded = true;

#if !UE_BUILD_SHIPPING
    m_DebugRenderer->SetLoadedFilename(filePath);
#endif

    return true;
}

bool FProjectAcousticsModule::LoadAceFile(const FString& filePath, const float cacheScale)
{
    if (!m_Triton)
    {
        return false;
    }

    UnloadAceFile(false);
    const bool loadSuccess = LoadAceFileIntoCurrentRuntime(filePath, cacheScale);
    if (loadSuccess)
    {
        ReplayDynamicOpeningsToCurrentRuntime();
    }
    return loadSuccess;
}

void FProjectAcousticsModule::ClearPreviousRuntime()
{
    if (m_PreviousTriton)
    {
        if (m_PreviousAceFileLoaded)
        {
            m_PreviousTriton->Clear();
            m_PreviousAceFileLoaded = false;
        }
        TritonAcoustics::DestroyInstance(m_PreviousTriton);
        m_PreviousTriton = nullptr;
    }

    m_PreviousTritonTaskHook.Reset();
    m_PreviousTritonIOHook.Reset();
    m_IsAceCrossfadeActive = false;
    m_AceCrossfadeDurationSeconds = 0.0f;
    m_AceCrossfadeElapsedSeconds = 0.0f;
}

bool FProjectAcousticsModule::LoadAceFileForCrossfade(
    const FString& filePath, const float cacheScale, const float durationSeconds)
{
    if (!m_Triton)
    {
        return false;
    }

    if (!m_AceFileLoaded || durationSeconds <= 0.0f)
    {
        return LoadAceFile(filePath, cacheScale);
    }

    WaitForRunningTasks();
    ClearPreviousRuntime();

    m_PreviousTriton = m_Triton;
    m_PreviousAceFileLoaded = m_AceFileLoaded;
    m_PreviousTritonIOHook = MoveTemp(m_TritonIOHook);
    m_PreviousTritonTaskHook = MoveTemp(m_TritonTaskHook);

    m_Triton = c_UseTritonDebugInterface ? TritonAcousticsDebug::CreateInstance() : TritonAcoustics::CreateInstance();
    m_AceFileLoaded = false;

    if (!m_Triton || !LoadAceFileIntoCurrentRuntime(filePath, cacheScale))
    {
        if (m_Triton)
        {
            m_Triton->Clear();
            TritonAcoustics::DestroyInstance(m_Triton);
        }
        m_Triton = m_PreviousTriton;
        m_AceFileLoaded = m_PreviousAceFileLoaded;
        m_TritonIOHook = MoveTemp(m_PreviousTritonIOHook);
        m_TritonTaskHook = MoveTemp(m_PreviousTritonTaskHook);
        m_PreviousTriton = nullptr;
        m_PreviousAceFileLoaded = false;
        return false;
    }

    ReplayDynamicOpeningsToCurrentRuntime();

    m_IsAceCrossfadeActive = true;
    m_AceCrossfadeDurationSeconds = durationSeconds;
    m_AceCrossfadeElapsedSeconds = 0.0f;
    return true;
}

void FProjectAcousticsModule::TickAceCrossfade(float deltaSeconds)
{
    if (!m_IsAceCrossfadeActive)
    {
        return;
    }

    m_AceCrossfadeElapsedSeconds += FMath::Max(0.0f, deltaSeconds);
    if (m_AceCrossfadeElapsedSeconds >= m_AceCrossfadeDurationSeconds)
    {
        WaitForRunningTasks();
        ClearPreviousRuntime();
    }
}

bool FProjectAcousticsModule::IsAceCrossfadeActive() const
{
    return m_IsAceCrossfadeActive;
}

void FProjectAcousticsModule::UnloadAceFile(bool clearOldQueries)
{
    if (!m_Triton)
    {
        return;
    }

    if (m_AceFileLoaded)
    {
        // Make sure there are no lingering background queries still running
        WaitForRunningTasks();
        if (clearOldQueries)
        {
            m_AcousticQueryResultMap.Reset();
            m_NumRunningTasks = 0;
        }

        SCOPE_CYCLE_COUNTER(STAT_Acoustics_ClearAce);
        m_Triton->Clear();
        m_AceFileLoaded = false;
    }

    m_TritonIOHook.Reset();
    m_TritonTaskHook.Reset();
    ClearPreviousRuntime();
}

#if !UE_BUILD_SHIPPING

static TritonAcousticParameters MakeFreefieldParameters(const FVector& sourceLocation, const FVector& listenerLocation)
{

    auto arrivalDir = AcousticsUtils::UnrealDirectionToTriton(sourceLocation - listenerLocation);
    auto LOSDist = static_cast<float>(arrivalDir.Size());
    arrivalDir.Normalize();
    const float silenceDb = -100.0f;
    const float zeroDecayTime = 0;
    DryParams dp = {
        0,
        AcousticsUtils::UnrealValToTriton(LOSDist), // PathLength
        0,                          // Loudness
        // ArrivalDir
        {static_cast<float>(arrivalDir.X), static_cast<float>(arrivalDir.Y), static_cast<float>(arrivalDir.Z)}};
    WetParams wp = {
        silenceDb, // Loudness
        // ArrivalDir
        {
            0,
            0,
            0,
        },
        360,          // spread
        zeroDecayTime // T60
    };
    return TritonAcousticParameters{dp, wp};
}
#endif

bool FProjectAcousticsModule::AddDynamicOpeningToRuntime(
    TritonAcoustics* triton, class UAcousticsDynamicOpening* opening, const FVector& center, const FVector& normal,
    const TArray<FVector>& verticesIn)
{
    if (!triton || verticesIn.Num() == 0)
    {
        return false;
    }

    TArray<Triton::Vec3f> vertices;
    for (auto& v : verticesIn)
    {
        vertices.Add(AcousticsUtils::ToTritonVector(v));
    }

    return triton->AddDynamicOpening(
        reinterpret_cast<uint64_t>(opening),
        AcousticsUtils::ToTritonVectorDouble(center),
        AcousticsUtils::ToTritonVector(normal),
        vertices.Num(),
        &vertices[0]);
}

void FProjectAcousticsModule::ReplayDynamicOpeningsToCurrentRuntime()
{
    for (const auto& registrationPair : m_DynamicOpeningRegistrations)
    {
        UAcousticsDynamicOpening* opening = registrationPair.Key;
        const FDynamicOpeningRegistration& registration = registrationPair.Value;
        if (AddDynamicOpeningToRuntime(
                m_Triton, opening, registration.Center, registration.Normal, registration.Vertices))
        {
            m_Triton->UpdateDynamicOpening(
                reinterpret_cast<uint64_t>(opening), registration.DryAttenuationDb, registration.WetAttenuationDb);
        }
    }
}

bool FProjectAcousticsModule::AddDynamicOpening(
    class UAcousticsDynamicOpening* opening, const FVector& center, const FVector& normal,
    const TArray<FVector>& verticesIn)
{
    if (!opening || verticesIn.Num() == 0)
    {
        return false;
    }

    FDynamicOpeningRegistration& registration = m_DynamicOpeningRegistrations.FindOrAdd(opening);
    registration.Center = center;
    registration.Normal = normal;
    registration.Vertices = verticesIn;

    bool currentSuccess = AddDynamicOpeningToRuntime(m_Triton, opening, center, normal, verticesIn);
    bool previousSuccess = true;
    if (m_IsAceCrossfadeActive && m_PreviousTriton && m_PreviousAceFileLoaded)
    {
        previousSuccess = AddDynamicOpeningToRuntime(m_PreviousTriton, opening, center, normal, verticesIn);
    }

    return currentSuccess && previousSuccess;
}

bool FProjectAcousticsModule::RemoveDynamicOpening(class UAcousticsDynamicOpening* opening)
{
    if (!opening)
    {
        return false;
    }

    m_DynamicOpeningRegistrations.Remove(opening);

    bool currentSuccess = m_Triton ? m_Triton->RemoveDynamicOpening(reinterpret_cast<uint64_t>(opening)) : false;
    bool previousSuccess = true;
    if (m_IsAceCrossfadeActive && m_PreviousTriton && m_PreviousAceFileLoaded)
    {
        previousSuccess = m_PreviousTriton->RemoveDynamicOpening(reinterpret_cast<uint64_t>(opening));
    }

    return currentSuccess && previousSuccess;
}

bool FProjectAcousticsModule::UpdateDynamicOpening(
    class UAcousticsDynamicOpening* opening, float dryAttenuationDb, float wetAttenuationDb)
{
    if (!opening)
    {
        return false;
    }

    if (FDynamicOpeningRegistration* registration = m_DynamicOpeningRegistrations.Find(opening))
    {
        registration->DryAttenuationDb = dryAttenuationDb;
        registration->WetAttenuationDb = wetAttenuationDb;
    }

    bool currentSuccess = m_Triton
        ? m_Triton->UpdateDynamicOpening(reinterpret_cast<uint64_t>(opening), dryAttenuationDb, wetAttenuationDb)
        : false;
    bool previousSuccess = true;
    if (m_IsAceCrossfadeActive && m_PreviousTriton && m_PreviousAceFileLoaded)
    {
        previousSuccess = m_PreviousTriton->UpdateDynamicOpening(
            reinterpret_cast<uint64_t>(opening), dryAttenuationDb, wetAttenuationDb);
    }

    return currentSuccess && previousSuccess;
}

bool FProjectAcousticsModule::SetGlobalDesign(const FAcousticsDesignParams& params)
{
    m_GlobalDesign = params;
    return true;
}

void FProjectAcousticsModule::SetSpaceTransform(const FTransform& newTransform)
{
    m_SpaceTransform = newTransform;
    m_InverseSpaceTransform = m_SpaceTransform.Inverse();
}

AcousticQueryResults FProjectAcousticsModule::GetAcousticQueryResults(
    const uint64_t sourceObjectId, const FVector& sourceLocation, const FVector& listenerLocation, 
    AcousticsObjectParams objectParams)
{
    UpdateOutdoorness(listenerLocation);

    TritonAcousticParameters acousticParams = {};
    // Need to pass over the state of ApplyDynamicOpenings
    TritonDynamicOpeningInfo openingInfo = objectParams.DynamicOpeningInfo;
    InterpolationConfig interpConfig = objectParams.InterpolationConfig;
    AcousticQueryResults returnStruct = {};
    bool querySuccess = false;

#if !UE_BUILD_SHIPPING
    TritonRuntime::QueryDebugInfo queryDebugInfo;
    TritonRuntime::QueryDebugInfo* queryDebugInfoPtr = &queryDebugInfo;
#else
    TritonRuntime::QueryDebugInfo* queryDebugInfoPtr = nullptr;
#endif // !UE_BUILD_SHIPPING

    if (m_IsAceCrossfadeActive && m_PreviousTriton && m_PreviousAceFileLoaded)
    {
        TritonAcousticParameters previousParams = {};
        TritonDynamicOpeningInfo previousOpeningInfo = objectParams.DynamicOpeningInfo;
        bool previousSuccess = GetAcousticParametersFromRuntime(
            m_PreviousTriton, sourceLocation, listenerLocation, previousParams, previousOpeningInfo, interpConfig);

        TritonAcousticParameters currentParams = {};
        TritonDynamicOpeningInfo currentOpeningInfo = objectParams.DynamicOpeningInfo;
        bool currentSuccess = GetAcousticParametersFromRuntime(
            m_Triton, sourceLocation, listenerLocation, currentParams, currentOpeningInfo, interpConfig, queryDebugInfoPtr);

        if (previousSuccess && currentSuccess)
        {
            const float alpha = m_AceCrossfadeDurationSeconds > 0.0f
                ? FMath::Clamp(m_AceCrossfadeElapsedSeconds / m_AceCrossfadeDurationSeconds, 0.0f, 1.0f)
                : 1.0f;
            acousticParams = AcousticsParameterBlend::Blend(previousParams, currentParams, alpha);
            openingInfo = alpha < 0.5f ? previousOpeningInfo : currentOpeningInfo;
            querySuccess = true;
        }
        else if (currentSuccess)
        {
            acousticParams = currentParams;
            openingInfo = currentOpeningInfo;
            querySuccess = true;
        }
        else if (previousSuccess)
        {
            acousticParams = previousParams;
            openingInfo = previousOpeningInfo;
            querySuccess = true;
        }
    }
    else
    {
        querySuccess = GetAcousticParameters(
            sourceLocation, listenerLocation, acousticParams, openingInfo, interpConfig, queryDebugInfoPtr);
    }

#if !UE_BUILD_SHIPPING
    returnStruct.QueryDebugInfo = queryDebugInfo;
#endif // !UE_BUILD_SHIPPING

    returnStruct.AcousticParams = acousticParams;
    returnStruct.OpeningInfo = openingInfo;
    returnStruct.QueryResult = querySuccess;

    return returnStruct;
}

void FProjectAcousticsModule::RegisterSourceObject(const uint64_t sourceObjectId)
{
    FScopeLock lock(&m_AcousticQueryResultMapLock);

    // Re-use the old result if it exists. Just don't reset the QueuedWork, because it could still be running
    AsyncAcousticQueryResults& result = m_AcousticQueryResultMap.FindOrAdd(sourceObjectId);

    if (result.QueuedWork.IsValid())
    {
        // There could be an old query running that hasn't finished. Attempt to retract it
        auto retracted = m_ThreadPool->RetractQueuedWork(result.QueuedWork.Get());

        // If retraction fails, it could be because the task is running. Setting RetractionRequested to true 
        // to indicate to the running task not to store its irrelevant results.
        result.RetractionRequested = true;

        if (retracted)
        {
            // Retracted tasks don't get abandoned. We need to do it.
            result.QueuedWork->Abandon();
            result.QueuedWork.Reset();
        }
    }

    result.QueryResults = TFuture<AcousticQueryResults>();
    result.HasProcessed = false;
}

void FProjectAcousticsModule::UnregisterSourceObject(const uint64_t sourceObjectId)
{
    FScopeLock lock(&m_AcousticQueryResultMapLock);

    if (m_AcousticQueryResultMap.Contains(sourceObjectId))
    {
        AsyncAcousticQueryResults& queryObject = m_AcousticQueryResultMap[sourceObjectId];
        if (queryObject.QueuedWork.IsValid())
        {
            auto queuedWorkPtr = queryObject.QueuedWork.Get();

            // A query for this source may still be queued. We want to retract it if we can so that it doesn't return
            // results later. If it was successfully retracted, we can safely remove it from the map. Or, if we know
            // it's not queued or running, we can remove it. Otherwise, it's possible it's in the running state and we
            // can't touch it yet. It will eventually be cleaned up during shutdown, where we do wait for tasks to
            // finish
            auto retracted = m_ThreadPool->RetractQueuedWork(queuedWorkPtr);
            auto isQueuedOrRunning = FPlatformAtomics::AtomicRead(&queuedWorkPtr->m_IsQueuedOrRunning);

            // If retraction fails, it could be because the task is running. Setting RetractionRequested to true 
            // to indicate to the running task not to store its irrelevant results.
            queryObject.RetractionRequested = true;
            
            if (retracted || (isQueuedOrRunning == 0))
            {
                if (retracted)
                {
                    // Retracted tasks don't get abandoned. We need to do it.
                    queuedWorkPtr->Abandon();
                    queryObject.QueuedWork.Reset();
                }
                m_AcousticQueryResultMap.Remove(sourceObjectId);
            }
        }
        else
        {
            m_AcousticQueryResultMap.Remove(sourceObjectId);
        }
    }
}

bool FProjectAcousticsModule::UpdateObjectParameters(
    const uint64_t sourceObjectId, const FVector& sourceLocation, const FVector& listenerLocation,
    AcousticsObjectParams& objectParams)
{
    SCOPE_CYCLE_COUNTER(STAT_Acoustics_UpdateObjectParams);

    if (!m_Triton)
    {
        return false;
    }

    // Validate arguments
    if (!m_AceFileLoaded)
    {
        return false;
    }
    // Get acoustic parameters from Triton. Pass failure on to caller, caller should re-use previous acoustic parameters
    TritonAcousticParameters acousticParams = {};
    TritonDynamicOpeningInfo openingInfo = {};
    TritonRuntime::QueryDebugInfo queryDebugInfo = {};

    // We want to most acoustic queries on a background thread. So for each update call on a source, we will return any
    // past results and queue up a query to run in the background and be ready for the next call.
    bool alreadyStoredResult = false;
    bool querySuccess = false;

    // Need to have a lock on the result map because both background threads and the audio/game thread will be accessing
    // it
    m_AcousticQueryResultMapLock.Lock();

    // Check if we have past results for this source
    if (m_AcousticQueryResultMap.Contains(sourceObjectId))
    {
        // Have the results been saved?
        if (m_AcousticQueryResultMap[sourceObjectId].QueryResults.IsReady())
        {
            // Results are ready. Get them and use their values
            auto results = m_AcousticQueryResultMap[sourceObjectId].QueryResults.Get();
            acousticParams = results.AcousticParams;
            openingInfo = results.OpeningInfo;
            querySuccess = results.QueryResult;
#if !UE_BUILD_SHIPPING
            queryDebugInfo = results.QueryDebugInfo;
#endif
            m_AcousticQueryResultMap[sourceObjectId].QueryResults.Reset();
        }
        // This is the first time this source is being processed. Run the first acoustic query call directly on this
        // calling thread
        else if (!m_AcousticQueryResultMap[sourceObjectId].HasProcessed)
        {
            // We don't want to hold the map lock while we're calling Triton though
            m_AcousticQueryResultMapLock.Unlock();

            // Do the query now
            auto results = GetAcousticQueryResults(sourceObjectId, sourceLocation, listenerLocation, objectParams);
            // Save the results
            acousticParams = results.AcousticParams;
            openingInfo = results.OpeningInfo;
            querySuccess = results.QueryResult;
#if !UE_BUILD_SHIPPING
            queryDebugInfo = results.QueryDebugInfo;
#endif

            // Get the map lock back and then store these results in the map, so that the 2nd query will have something
            // ready. Normal background queries will resume the 2nd time around
            m_AcousticQueryResultMapLock.Lock();
            TPromise<AcousticQueryResults> newPromise;
            newPromise.SetValue(results);

            // Re-use the existing result. Don't reset the QueuedWork, which still could be running.
            AsyncAcousticQueryResults& result = m_AcousticQueryResultMap.FindOrAdd(sourceObjectId);
            result.QueryResults = newPromise.GetFuture();
            result.HasProcessed = true;

            alreadyStoredResult = true;
        }
        else
        {
            // No results were ready and this is not the first time this source has been processed. This probably means
            // a background query didn't complete in time.
            UE_LOG(
                LogAcousticsRuntime,
                Warning,
                TEXT("No acoustic query result found for source:%d. This most likely means a background query "
                     "did not complete in time."),
                sourceObjectId);
        }
    }
    else
    {
        UE_LOG(
            LogAcousticsRuntime,
            Error,
            TEXT("No key found in the acoustic query map for source:%d. This most likely means this source "
                 "did not register first (RegisterSourceObject) before updating."), sourceObjectId);
        m_AcousticQueryResultMapLock.Unlock();
        return false;
    }
    m_AcousticQueryResultMapLock.Unlock();

    // Queue up the query to run on the background thread.
    // Don't do this if we already stored the results

    if (!alreadyStoredResult)
    {
        // Function to perform an acoustic query on a separate thread and save the result to the local map
        TFunction<void()> RunBackgroundAcousticsQuery(
            [this, sourceObjectId, sourceLocation, listenerLocation, objectParams]()
            {
                // Run the acoustic query
                auto results = GetAcousticQueryResults(
                    sourceObjectId,
                    sourceLocation,
                    listenerLocation,
                    objectParams);

                FScopeLock lock(&m_AcousticQueryResultMapLock);
                if (m_AcousticQueryResultMap.Contains(sourceObjectId))
                {
                    AsyncAcousticQueryResults& result = m_AcousticQueryResultMap[sourceObjectId];
                    if (result.RetractionRequested)
                    {
                        // This task was attempted to be retracted but wasn't able to be. We don't want to store
                        // these results. Exit early.
                        result.RetractionRequested = false;
                        return;
                    }

                    // Store the promise/future in the map for retrieval on the next update pass
                    TPromise<AcousticQueryResults> newPromise;
                    newPromise.SetValue(results);

                    result.HasProcessed = true;
                    result.QueryResults = newPromise.GetFuture();
                }
            });

        m_AcousticQueryResultMapLock.Lock();
        AsyncAcousticQueryResults& result = m_AcousticQueryResultMap.FindOrAdd(sourceObjectId);

        // If the last query is still running, we don't want to schedule a new one and fall behind. Skip the 
        // scheduling, and try again next pass.
        auto queryStillRunning =
            result.QueuedWork.IsValid() ? FPlatformAtomics::AtomicRead(&result.QueuedWork->m_IsQueuedOrRunning) : 0;
        if (!queryStillRunning)
        {
            // Queue up the acoustic query
            result.RetractionRequested = false;

            // Save the QueuedWork item in case we want to retract it later
            result.QueuedWork = TUniquePtr<FAcousticsQueuedWork>(
                new FAcousticsQueuedWork(MoveTemp(RunBackgroundAcousticsQuery), &m_NumRunningTasks));

            // Signal that we've queued this item
            result.QueuedWork->SignalStart();

            // Add our query to the queue
            m_ThreadPool->AddQueuedWork(result.QueuedWork.Get());
        }
        m_AcousticQueryResultMapLock.Unlock();
    }

#if !UE_BUILD_SHIPPING
    if (!querySuccess)
    {
        // Even if query fails, we want to catch that debug information before exiting this function
        m_DebugRenderer->UpdateSourceAcoustics(
            sourceObjectId, sourceLocation, listenerLocation, querySuccess, objectParams, queryDebugInfo);
        int  NumMessages;
        const TritonRuntime::QueryDebugInfo::DebugMessage* Messages = queryDebugInfo.GetMessageList(NumMessages);
        UE_LOG(LogAcousticsRuntime, Verbose, TEXT("%s : Query for ObjID[%llu] at [%.2f, %.2f, %.2f] failed with %d messages:"), 
            ANSI_TO_TCHAR(__FUNCTION__),
            sourceObjectId,
            sourceLocation.X,
            sourceLocation.Y,
            sourceLocation.Z,
            NumMessages);
        for (int i=0; i<NumMessages; i++)
        {
            UE_LOG(LogAcousticsRuntime, Verbose, TEXT("  %s"), Messages[i].MessageString);
        }
        return false;
    }
#else
    if (!querySuccess)
    {
        return false;
    }
#endif // !UE_BUILD_SHIPPING

    // Caller passes in design adjustments for this emitter,
    // update them with global adjustments
    FAcousticsDesignParams::Combine(objectParams.Design, m_GlobalDesign);

    // Set the remaining fields apart from design
    objectParams.ObjectId = sourceObjectId;
    objectParams.TritonParams = acousticParams;
    objectParams.DynamicOpeningInfo = openingInfo;
    // Outdoorness value is shared across all emitters since it depends only on
    // listener location (for now), fill in that shared value.
    objectParams.Outdoorness = m_CachedOutdoorness;

#if !UE_BUILD_SHIPPING
    // If acoustics is disabled, intercept parameters headed to DSP
    // and substitute "no acoustics" in all parameters - i.e. how it
    // would sound if there were no geometry in the scene. Note that
    // the system's internal logic such as doing queries, updating streaming
    // etc. still remain active. This is intentional since the intended use
    // case is for someone to do a quick A/B by toggling switch to hear difference
    // such as for debugging.
    if (!m_IsEnabled)
    {
        objectParams.TritonParams = MakeFreefieldParameters(sourceLocation, listenerLocation);
        objectParams.Outdoorness = 1;
    }

    // Catch debug information for this source
    m_DebugRenderer->UpdateSourceAcoustics(
        sourceObjectId, sourceLocation, listenerLocation, querySuccess, objectParams, queryDebugInfo);
#endif

    return true;
}

bool FProjectAcousticsModule::PostTick()
{
    if (!m_Triton)
    {
        return false;
    }

    m_IsOutdoornessStale = true;
    return true;
}

bool FProjectAcousticsModule::UpdateDistances(const FVector& listenerLocation)
{
    if (!m_Triton)
    {
        return false;
    }

    auto listener = AcousticsUtils::ToTritonVectorDouble(WorldPositionToTriton(listenerLocation));
    return m_Triton->UpdateDistancesForListener(listener);
}

bool FProjectAcousticsModule::QueryDistance(const FVector& lookDirection, float& outDistance)
{
    if (!m_Triton)
    {
        outDistance = 0;
        return false;
    }

    auto dir = AcousticsUtils::ToTritonVector(WorldDirectionToTriton(lookDirection));
    outDistance = m_Triton->QueryDistanceForListener(dir) * AcousticsUtils::c_TritonToUnrealScale;
    return true;
}

bool FProjectAcousticsModule::UpdateOutdoorness(const FVector& listenerLocation)
{
    if (!m_Triton)
    {
        return false;
    }

    // This function will be called by each sound source in a frame.
    // Since outdoorness depends only on player location, we do work
    // only once per frame, regardless of whether query succeeds or fails.
    // In case of failure, we leave the old cached outdoorness value unmodified.
    if (m_IsOutdoornessStale)
    {
        auto listener = AcousticsUtils::ToTritonVectorDouble(WorldPositionToTriton(listenerLocation));
        bool success = false;
        {
            SCOPE_CYCLE_COUNTER(STAT_Acoustics_QueryOutdoorness);
            auto outdoorness = 0.0f;
            success = m_Triton->GetOutdoornessAtListener(listener, outdoorness);
            if (success)
            {
                const float NormalizedVal =
                    (outdoorness - c_OutdoornessIndoors) / (c_OutdoornessOutdoors - c_OutdoornessIndoors);
                m_CachedOutdoorness = FMath::Clamp(NormalizedVal, 0.0f, 1.0f);
            }
        }

        m_IsOutdoornessStale = false;
        return success;
    }

    return true;
}

inline float FProjectAcousticsModule::GetOutdoorness() const
{
    return m_CachedOutdoorness;
}

bool FProjectAcousticsModule::CalculateReverbSendWeights(
    const float targetReverbTime, const uint32_t numReverbs, const float* reverbTimes, float* reverbSendWeights) const
{
    return TritonAcoustics::CalculateReverbSendWeights(targetReverbTime, numReverbs, reverbTimes, reverbSendWeights);
}


bool FProjectAcousticsModule::GetAcousticParameters(
    const FVector& sourceLocation, const FVector& listenerLocation, TritonAcousticParameters& params,
    TritonDynamicOpeningInfo& outOpeningInfo, const InterpolationConfig& interpConfig, TritonRuntime::QueryDebugInfo* outDebugInfo /* = nullptr */)
{
    return GetAcousticParametersFromRuntime(
        m_Triton, sourceLocation, listenerLocation, params, outOpeningInfo, interpConfig, outDebugInfo);
}

bool FProjectAcousticsModule::GetAcousticParametersFromRuntime(
    TritonAcoustics* triton, const FVector& sourceLocation, const FVector& listenerLocation,
    TritonAcousticParameters& params, TritonDynamicOpeningInfo& outOpeningInfo, const InterpolationConfig& interpConfig,
    TritonRuntime::QueryDebugInfo* outDebugInfo /* = nullptr */)
{
    if (!triton)
    {
        return false;
    }

    auto source = AcousticsUtils::ToTritonVectorDouble(WorldPositionToTriton(sourceLocation));
    auto listener = AcousticsUtils::ToTritonVectorDouble(WorldPositionToTriton(listenerLocation));

    bool acousticParamsValid = false;
    {
        SCOPE_CYCLE_COUNTER(STAT_Acoustics_Query);

#if !UE_BUILD_SHIPPING
        if (outDebugInfo)
        {
            acousticParamsValid = static_cast<TritonAcousticsDebug*>(triton)->QueryAcoustics(
                source, listener, params, outOpeningInfo, &interpConfig, outDebugInfo);
        }
        else
        {
            acousticParamsValid = triton->QueryAcoustics(source, listener, params, outOpeningInfo, &interpConfig);
        }
#else
        acousticParamsValid = triton->QueryAcoustics(source, listener, params, outOpeningInfo, &interpConfig);
#endif
    }


    return acousticParamsValid;
}

// Wait for any remaining background queries to finish
void FProjectAcousticsModule::WaitForRunningTasks()
{
    while (m_NumRunningTasks > 0)
    {
        FPlatformProcess::Sleep(0);
    }
}

void FProjectAcousticsModule::UpdateLoadedRegion(
    const FVector& playerPosition, const FVector& tileSize, const bool forceUpdate, const bool unloadProbesOutsideTile,
    const bool blockOnCompletion)
{
    if (!m_Triton)
    {
        return;
    }

    const auto difference = (playerPosition - m_LastLoadCenterPosition).GetAbs();
    const auto loadThreshold = m_LastLoadTileSize * c_AceTileLoadMargin * 0.5f;
    bool shouldUpdate = forceUpdate || (difference.X > loadThreshold.X || difference.Y > loadThreshold.Y ||
                                        difference.Z > loadThreshold.Z);
    if (shouldUpdate)
    {
        int loadedProbes = 0;
        {
            SCOPE_CYCLE_COUNTER(STAT_Acoustics_LoadRegion);
            loadedProbes = m_Triton->LoadRegion(
                AcousticsUtils::ToTritonVectorDouble(WorldPositionToTriton(playerPosition)),
                AcousticsUtils::ToTritonVectorDouble(WorldScaleToTriton(tileSize).GetAbs()),
                unloadProbesOutsideTile,
                blockOnCompletion);
        }
        if (loadedProbes >= 0)
        {
            m_LastLoadCenterPosition = playerPosition;
            // Tile Size must be all positive values, otherwise triton fails to load probes
            m_LastLoadTileSize = tileSize.GetAbs();
        }
    }
}

FVector FProjectAcousticsModule::TritonPositionToWorld(const FVector& vec) const
{
    auto vectorInUnrealCoords = AcousticsUtils::TritonPositionToUnreal(vec);
    return m_SpaceTransform.TransformPosition(vectorInUnrealCoords);
}

FVector FProjectAcousticsModule::WorldPositionToTriton(const FVector& vec) const
{
    auto vectorWithTx = m_InverseSpaceTransform.TransformPosition(vec);
    return AcousticsUtils::UnrealPositionToTriton(vectorWithTx);
}

FVector FProjectAcousticsModule::TritonScaleToWorld(const FVector& vec) const
{
    auto vectorInUnrealCoords = AcousticsUtils::TritonPositionToUnreal(vec);
    return vectorInUnrealCoords * m_SpaceTransform.GetScale3D();
}

FVector FProjectAcousticsModule::WorldScaleToTriton(const FVector& vec) const
{
    auto vectorWithScale = vec * m_InverseSpaceTransform.GetScale3D();
    return AcousticsUtils::UnrealPositionToTriton(vectorWithScale);
}

FVector FProjectAcousticsModule::TritonDirectionToWorld(const FVector& vec) const
{
    auto directionInUnrealCoords = AcousticsUtils::TritonDirectionToUnreal(vec);
    return m_SpaceTransform.TransformVectorNoScale(directionInUnrealCoords);
}

FVector FProjectAcousticsModule::WorldDirectionToTriton(const FVector& vec) const
{
    auto directionWithTx = m_InverseSpaceTransform.TransformVectorNoScale(vec);
    return AcousticsUtils::UnrealDirectionToTriton(directionWithTx);
}

VectorF FProjectAcousticsModule::TritonDirectionToHrtfEngine(const VectorF& vec) const
{
    FVector vecD =
        AcousticsUtils::ToFVector(vec); // float to double because the inverse space transform requires double
    auto directionWithTx = m_InverseSpaceTransform.TransformVectorNoScale(vecD);
    auto hrtfDirectionWithTx = AcousticsUtils::TritonDirectionToHrtfEngine(directionWithTx);
    return VectorF(hrtfDirectionWithTx.X, hrtfDirectionWithTx.Y, hrtfDirectionWithTx.Z); // back to float
}

FQuat FProjectAcousticsModule::GetSpaceRotation() const
{
    return m_SpaceTransform.GetRotation();
}

#if !UE_BUILD_SHIPPING
TritonAcousticsDebug* FProjectAcousticsModule::GetTritonDebugInstance() const
{
    return static_cast<TritonAcousticsDebug*>(m_Triton);
}

void FProjectAcousticsModule::SetEnabled(bool isEnabled)
{
    m_IsEnabled = isEnabled;
}

// Sets flag for this source to render debug information (or not)
void FProjectAcousticsModule::UpdateSourceDebugInfo(
    uint64_t sourceID, bool shouldDraw, FName displayName, bool isBeingDestroyed)
{
    if (!m_Triton)
    {
        return;
    }

    m_DebugRenderer->UpdateSourceDebugInfo(sourceID, shouldDraw, displayName, isBeingDestroyed);
}

void FProjectAcousticsModule::SetVoxelVisibleDistance(const float InVisibleDistance)
{
    if (m_DebugRenderer)
    {
        m_DebugRenderer->SetVoxelVisibleDistance(InVisibleDistance);
    }
}

void FProjectAcousticsModule::DebugRender(
    UWorld* world, UCanvas* canvas, const FVector& cameraPos, const FVector& cameraLook, float cameraFOV,
    bool shouldDrawStats, bool shouldDrawVoxels, bool shouldDrawProbes, bool shouldDrawDistances,
    AcousticsDrawParameters shouldDrawSourceParameters)
{
    if (!m_Triton)
    {
        return;
    }

    m_DebugRenderer->Render(
        world,
        canvas,
        cameraPos,
        cameraLook,
        cameraFOV,
        shouldDrawStats,
        shouldDrawVoxels,
        shouldDrawProbes,
        shouldDrawDistances,
        shouldDrawSourceParameters);
}
#endif //! UE_BUILD_SHIPPING

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FProjectAcousticsModule, ProjectAcoustics)
