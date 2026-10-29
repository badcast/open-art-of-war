#pragma once

#include <cmath>

// Math of the simulation that gives the same bits on every platform. The C library's sin / cos / atan2 /
// hypot are only accurate to an ulp or so, and that last bit differs between glibc, MSVC and Apple's
// libm: enough for two peers of a network game to drift apart. These use nothing but + - * / and sqrt
// (exactly rounded by IEEE 754 everywhere); the build turns off FMA contraction (-ffp-contract=off),
// so the compiler cannot fuse them differently either.
namespace OpenArtOfWar::DetMath
{

    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kHalfPi = 1.57079632679489661923f;
    constexpr float kTwoPi = 6.28318530717958647692f;

    inline float hypot(float x, float y)
    {
        return std::sqrt(x * x + y * y);
    }

    // sin on [-pi/2, pi/2] (Taylor to x^13, error < 1e-7)
    inline float sinReduced(float r)
    {
        const float r2 = r * r;
        float p = -1.0f / 6227020800.0f;
        p = p * r2 + 1.0f / 39916800.0f;
        p = p * r2 - 1.0f / 362880.0f;
        p = p * r2 + 1.0f / 5040.0f;
        p = p * r2 - 1.0f / 120.0f;
        p = p * r2 + 1.0f / 6.0f;
        return r - r * r2 * p;
    }

    inline float sin(float x)
    {
        // To [-pi, pi]: x - k * 2pi with k = round(x / 2pi) (the 2pi split in two parts keeps it exact)
        const float k = std::floor(x * (1.0f / kTwoPi) + 0.5f);
        float r = (x - k * 6.28125f) - k * 1.93530717958647692e-3f;
        if(r > kHalfPi)
            r = kPi - r;
        else if(r < -kHalfPi)
            r = -kPi - r;
        return sinReduced(r);
    }

    inline float cos(float x)
    {
        return sin(x + kHalfPi);
    }

    // atan on [-1, 1] (minimax, error < 1e-5 rad)
    inline float atanUnit(float z)
    {
        const float z2 = z * z;
        float p = -0.0117212f;
        p = p * z2 + 0.05265332f;
        p = p * z2 - 0.11643287f;
        p = p * z2 + 0.19354346f;
        p = p * z2 - 0.33262347f;
        p = p * z2 + 0.99997726f;
        return z * p;
    }

    inline float atan2(float y, float x)
    {
        if(x == 0.0f && y == 0.0f)
            return 0.0f;
        const float ax = x < 0.0f ? -x : x;
        const float ay = y < 0.0f ? -y : y;
        // Angle of the first octant, then mirrored into place
        float a = ax >= ay ? atanUnit(ay / ax) : kHalfPi - atanUnit(ax / ay);
        if(x < 0.0f)
            a = kPi - a;
        return y < 0.0f ? -a : a;
    }

} // namespace OpenArtOfWar::DetMath
