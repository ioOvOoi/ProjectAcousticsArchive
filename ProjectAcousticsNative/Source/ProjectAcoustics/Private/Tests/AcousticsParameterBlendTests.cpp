#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "AcousticsParameterBlend.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAcousticsParameterBlendTest,
    "ProjectAcoustics.Crossfade.BlendsAcousticParameters",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAcousticsParameterBlendTest::RunTest(const FString& Parameters)
{
    TritonAcousticParameters From = {};
    From.Dry.PathLengthMeters = 2.0f;
    From.Dry.LoudnessDb = -20.0f;
    From.Dry.ArrivalDirection = ATKVectorF(1.0f, 0.0f, 0.0f);
    From.Wet.LoudnessDb = -30.0f;
    From.Wet.ArrivalDirection = ATKVectorF(0.0f, 1.0f, 0.0f);
    From.Wet.AngularSpreadDegrees = 60.0f;
    From.Wet.DecayTimeSeconds = 1.0f;

    TritonAcousticParameters To = {};
    To.Dry.PathLengthMeters = 10.0f;
    To.Dry.LoudnessDb = -10.0f;
    To.Dry.ArrivalDirection = ATKVectorF(0.0f, 1.0f, 0.0f);
    To.Wet.LoudnessDb = -10.0f;
    To.Wet.ArrivalDirection = ATKVectorF(1.0f, 0.0f, 0.0f);
    To.Wet.AngularSpreadDegrees = 180.0f;
    To.Wet.DecayTimeSeconds = 3.0f;

    const TritonAcousticParameters Mixed = AcousticsParameterBlend::Blend(From, To, 0.5f);

    TestEqual(TEXT("Path length blends linearly"), Mixed.Dry.PathLengthMeters, 6.0f);
    TestTrue(TEXT("Dry loudness blends in energy space"), Mixed.Dry.LoudnessDb > -13.5f && Mixed.Dry.LoudnessDb < -12.0f);
    TestTrue(TEXT("Wet loudness blends in energy space"), Mixed.Wet.LoudnessDb > -14.0f && Mixed.Wet.LoudnessDb < -12.0f);
    TestEqual(TEXT("Wet spread blends linearly"), Mixed.Wet.AngularSpreadDegrees, 120.0f);
    TestEqual(TEXT("Decay blends linearly"), Mixed.Wet.DecayTimeSeconds, 2.0f);

    const float DryDirectionLength = FMath::Sqrt(
        Mixed.Dry.ArrivalDirection.x * Mixed.Dry.ArrivalDirection.x +
        Mixed.Dry.ArrivalDirection.y * Mixed.Dry.ArrivalDirection.y +
        Mixed.Dry.ArrivalDirection.z * Mixed.Dry.ArrivalDirection.z);
    TestTrue(TEXT("Dry direction remains normalized"), FMath::Abs(DryDirectionLength - 1.0f) < 0.001f);

    return true;
}

#endif
