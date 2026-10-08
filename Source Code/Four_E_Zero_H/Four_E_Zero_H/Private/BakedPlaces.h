#pragma once

namespace BakedPlaces
{
    struct FPoint
    {
        float X;
        float Y;
        float Z;
    };

    struct FArea
    {
        const char* Name;
        const FPoint* Points;
        int Count;
    };

    
    
    static const FPoint None[] = { { 0.f, 0.f, 0.f } };

    static const FArea Areas[] = { { "", None, 0 } };
    static const int AreaCount = 0;

    static const FArea Routes[] = { { "", None, 0 } };
    static const int RouteCount = 0;
}
