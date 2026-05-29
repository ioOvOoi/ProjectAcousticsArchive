#pragma once

#include "CoreMinimal.h"
#include "TritonApiTypes.h"

namespace AcousticsParameterBlend
{
    inline float ClampAlpha(float Alpha)
    {
        return FMath::Clamp(Alpha, 0.0f, 1.0f);
    }

    inline float DbToPower(float Db)
    {
        return FMath::Pow(10.0f, Db / 10.0f);
    }

    inline float PowerToDb(float Power)
    {
        constexpr float MinPower = 1.0e-10f;
        return 10.0f * FMath::LogX(10.0f, FMath::Max(Power, MinPower));
    }

    inline float BlendDb(float FromDb, float ToDb, float Alpha)
    {
        Alpha = ClampAlpha(Alpha);
        return PowerToDb(FMath::Lerp(DbToPower(FromDb), DbToPower(ToDb), Alpha));
    }

    inline ATKVectorF BlendDirection(const ATKVectorF& From, const ATKVectorF& To, float Alpha)
    {
        Alpha = ClampAlpha(Alpha);
        FVector Mixed(
            FMath::Lerp(From.x, To.x, Alpha),
            FMath::Lerp(From.y, To.y, Alpha),
            FMath::Lerp(From.z, To.z, Alpha));

        if (!Mixed.Normalize())
        {
            Mixed = FVector(To.x, To.y, To.z);
            if (!Mixed.Normalize())
            {
                Mixed = FVector(1.0f, 0.0f, 0.0f);
            }
        }

        return ATKVectorF(static_cast<float>(Mixed.X), static_cast<float>(Mixed.Y), static_cast<float>(Mixed.Z));
    }

    inline TritonAcousticParameters Blend(
        const TritonAcousticParameters& From,
        const TritonAcousticParameters& To,
        float Alpha)
    {
        Alpha = ClampAlpha(Alpha);

        TritonAcousticParameters Result = To;
        Result.Dry.GeomDist = FMath::Lerp(From.Dry.GeomDist, To.Dry.GeomDist, Alpha);
        Result.Dry.PathLengthMeters = FMath::Lerp(From.Dry.PathLengthMeters, To.Dry.PathLengthMeters, Alpha);
        Result.Dry.LoudnessDb = BlendDb(From.Dry.LoudnessDb, To.Dry.LoudnessDb, Alpha);
        Result.Dry.ArrivalDirection = BlendDirection(From.Dry.ArrivalDirection, To.Dry.ArrivalDirection, Alpha);

        Result.Wet.LoudnessDb = BlendDb(From.Wet.LoudnessDb, To.Wet.LoudnessDb, Alpha);
        Result.Wet.ArrivalDirection = BlendDirection(From.Wet.ArrivalDirection, To.Wet.ArrivalDirection, Alpha);
        Result.Wet.AngularSpreadDegrees = FMath::Lerp(From.Wet.AngularSpreadDegrees, To.Wet.AngularSpreadDegrees, Alpha);
        Result.Wet.DecayTimeSeconds = FMath::Lerp(From.Wet.DecayTimeSeconds, To.Wet.DecayTimeSeconds, Alpha);

        return Result;
    }
}
