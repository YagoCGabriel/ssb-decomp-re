#ifndef PC_WIN32_SHIM_H
#define PC_WIN32_SHIM_H

/*
 * ssb-decomp PC Port - Win32 API shim.
 *
 * Lets the Direct3D 9-ex-based D3D10/D3D11 backend sources compile and be
 * syntax-checked on non-Windows hosts (CI), while compiling natively against
 * the real Windows SDK headers when built on MSVC/MinGW-w64.
 * On Windows this header simply includes <windows.h>/<d3d11.h>/<d3d10.h>.
 */

#if defined(_WIN32)

#include <windows.h>
#include <d3d11.h>
#include <d3d10.h>
#include <dxgi.h>

#else /* portable stubs ---------------------------------------------------*/

#include <stdint.h>

typedef void *HWND, *HINSTANCE, *HDC;
typedef unsigned long DWORD;
typedef int BOOL;
typedef uint64_t UINT64_;
typedef union LARGE_INTEGER { uint64_t QuadPart; struct { uint32_t LowPart; int32_t HighPart; } u; } LARGE_INTEGER;

typedef struct RECT_ { long left, top, right, bottom; } RECT_;
typedef struct POINT_ { long x, y; } POINT_;

#define TRUE_ 1
#define FALSE_ 0

static inline void QueryPerformanceFrequency_(LARGE_INTEGER *f) { f->QuadPart = 10000000ull; }
static inline void QueryPerformanceCounter_(LARGE_INTEGER *c) { c->QuadPart = 0; }

#endif /* _WIN32 */
#endif /* PC_WIN32_SHIM_H */
