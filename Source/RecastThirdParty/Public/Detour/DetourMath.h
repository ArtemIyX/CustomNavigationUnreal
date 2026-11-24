/**
@defgroup detour Detour

Members in this module are wrappers around the standard math library
*/

#ifndef DETOURMATH_H
#define DETOURMATH_H

#include <math.h>

inline RECASTTHIRDPARTY_API float dtMathFabsf(float x) { return fabsf(x); }
inline RECASTTHIRDPARTY_API float dtMathSqrtf(float x) { return sqrtf(x); }
inline RECASTTHIRDPARTY_API float dtMathFloorf(float x) { return floorf(x); }
inline RECASTTHIRDPARTY_API float dtMathCeilf(float x) { return ceilf(x); }
inline RECASTTHIRDPARTY_API float dtMathCosf(float x) { return cosf(x); }
inline RECASTTHIRDPARTY_API float dtMathSinf(float x) { return sinf(x); }
inline RECASTTHIRDPARTY_API float dtMathAtan2f(float y, float x) { return atan2f(y, x); }
inline RECASTTHIRDPARTY_API bool dtMathIsfinite(float x)
{
#ifndef RC_FAST_MATH
	return isfinite(x);
#else
	// Infinity and NaN are disabled when compiling with -ffast-math
	(void)x;
	return true;
#endif
}

#endif
