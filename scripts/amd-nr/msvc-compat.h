// lmxxf 4764e4f1a24f5234b8e29952dc41f493b2d77c90: the only GNU attribute
// used by this runtime is __attribute__((noinline)); map it for MSVC.
#ifdef _MSC_VER
#define __attribute__(x) __declspec(noinline)
#endif
