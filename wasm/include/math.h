/* Freestanding math.h for the WebAssembly build.
 *
 * Nothing is implemented here. Four of these map straight onto WebAssembly
 * instructions and clang emits them inline; the rest are left undefined on
 * purpose, so the linker turns them into module imports and the host supplies
 * them. The host is a browser, and a browser already has a correctly rounded
 * transcendental library sitting in Math - so shipping a second one compiled
 * into the module would be adding a dependency to avoid using one that is
 * already there.
 *
 * The precision difference is real and irrelevant: JS computes in double and
 * the result is rounded to float on the way back, which is at worst a half-ulp
 * disagreement with a native libm's float entry points, on quantities that end
 * up as one of 256 shades of a colour channel.
 */
#ifndef CPORE_WASM_MATH_H
#define CPORE_WASM_MATH_H

/* these become f32.sqrt / f32.abs / f32.floor / f32.ceil, no import needed */
static inline float sqrtf(float x)  { return __builtin_sqrtf(x); }
static inline float fabsf(float x)  { return __builtin_fabsf(x); }
static inline float floorf(float x) { return __builtin_floorf(x); }
static inline float ceilf(float x)  { return __builtin_ceilf(x); }
static inline double sqrt(double x) { return __builtin_sqrt(x); }
static inline double fabs(double x) { return __builtin_fabs(x); }

/* imported from the host */
float sinf(float);
float cosf(float);
float tanf(float);
float asinf(float);
float acosf(float);
float atanf(float);
float atan2f(float, float);
float powf(float, float);
float expf(float);
float logf(float);
float fmodf(float, float);

#define M_PI 3.14159265358979323846

#endif
