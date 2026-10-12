#ifndef PC_WIN32_SHIM_H
#define PC_WIN32_SHIM_H
/*
 * ssb-decomp PC Port - minimal Win32/Direct3D type shim.
 *
 * Lets the D3D10/D3D11 backend translation units compile on non-Windows CI
 * hosts (type-check only). On Windows with the Platform SDK available, define
 * PCPORT_USE_REAL_WINDOWS before including to pull in the real headers.
 */
#if defined(PCPORT_USE_REAL_WINDOWS) && defined(_WIN32)
#include <windows.h>
#include <d3d11.h>
#include <d3d10.h>
#else
typedef void *HWND;
typedef void *HINSTANCE;
typedef unsigned int UINT;
typedef long LPARAM;
typedef long WPARAM;
typedef long LRESULT;
typedef int BOOL;
typedef unsigned char uint8_t_shim_guard;
#define TRUE 1
#define FALSE 0
#endif
#endif /* PC_WIN32_SHIM_H */
