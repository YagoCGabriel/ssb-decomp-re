/*
 * ssb-decomp PC Port - Direct3D 11 backend.
 *
 * Renders the translated F3DEX2 display lists on real hardware via D3D11
 * (Feature Level >= 10_0, so it also runs on DX10-class GPUs). The state
 * model mirrors the headless sw-raster reference exactly: viewport-mapped
 * triangles in N64 framebuffer space, depth test/write from G_ZBUFFER,
 * scissor, front/back cull and RGBA vertex colors.
 *
 * This TU is only compiled when PCPORT_HAVE_D3D is set (Windows + SDK).
 */
#ifdef PCPORT_HAVE_D3D

#include "pc_port_common.h"
#include "pc_sw_raster.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#endif


/* ID3DBlob accessors are inline-only in the mingw-w64 C headers; use vtbl directly. */
static void *blobPtr(ID3DBlob *b) { return b->lpVtbl->GetBufferPointer(b); }
static SIZE_T blobSize(ID3DBlob *b) { return b->lpVtbl->GetBufferSize(b); }

typedef struct D3D11Backend {
    PcBackend base;
    ID3D11Device *device;
    ID3D11DeviceContext *ctx;
    IDXGISwapChain *swap;
    ID3D11RenderTargetView *rtv;
    ID3D11DepthStencilView *dsav;
    ID3D11Texture2D *depthTex;
    ID3D11Buffer *vb;          /* triangle vertex buffer (3 verts/frame batch) */
    ID3D11Buffer *cbPerDraw;   /* matrices/viewport constant buffer */
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11InputLayout *layout;
    ID3D11RasterizerState *rsNoCull, *rsFront, *rsBack;
    ID3D11DepthStencilState *dsOff, *dsOn;
    HWND hwnd;
    int width, height;
    float viewport[4];
    const uint8_t *rdram;
    size_t rdram_size;
    unsigned draws;
} D3D11Backend;

static D3D11Backend g_d3d11;

typedef struct { float x, y, z; float r, g, b, a; } GpuVert;

static const char *VS_SRC =
    "struct VS_IN { float3 pos : POSITION; float4 col : COLOR; };"
    "struct VS_OUT { float4 pos : SV_POSITION; float4 col : COLOR; };"
    "VS_OUT main(VS_IN i) { VS_OUT o; o.pos = float4(i.pos.x, -i.pos.y, i.pos.z, 1.0);"
    " o.col = i.col; return o; }";
static const char *PS_SRC =
    "float4 main(float4 pos : SV_POSITION, float4 col : COLOR) : SV_TARGET { return col; }";

static int compileShaders(D3D11Backend *b)
{
    ID3DBlob *vsb = NULL, *psb = NULL, *err = NULL;
    HRESULT hr;
    extern HRESULT WINAPI D3DCompile(LPCVOID, SIZE_T, LPCVOID, const void *, const void *,
                                     LPCVOID, LPCVOID, UINT, UINT, ID3DBlob **, ID3DBlob **);
    HMODULE dcc = LoadLibraryA("d3dcompiler.dll");
    typedef HRESULT(WINAPI *pfn)(LPCVOID, SIZE_T, LPCVOID, const void *, const void *,
                                 LPCVOID, LPCVOID, UINT, UINT, ID3DBlob **, ID3DBlob **);
    pfn compile = dcc ? (pfn)(void *)GetProcAddress(dcc, "D3DCompile") : NULL;
    if (!compile) return 0;
    hr = compile(VS_SRC, strlen(VS_SRC), NULL, NULL, NULL, "main", "vs_4_0_level_9_1", 0, 0, &vsb, &err);
    if (FAILED(hr)) return 0;
    hr = compile(PS_SRC, strlen(PS_SRC), NULL, NULL, NULL, "main", "ps_4_0_level_9_1", 0, 0, &psb, &err);
    if (FAILED(hr)) { vsb->lpVtbl->Release(vsb); return 0; }
    if (FAILED(b->device->lpVtbl->CreateVertexShader(b->device, blobPtr(vsb),
            blobSize(vsb), NULL, &b->vs))) { return 0; }
    if (FAILED(b->device->lpVtbl->CreatePixelShader(b->device, blobPtr(psb),
            blobSize(psb), NULL, &b->ps))) { return 0; }
    {
        D3D11_INPUT_ELEMENT_DESC il[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        if (FAILED(b->device->lpVtbl->CreateInputLayout(b->device, il, 2,
                blobPtr(vsb), blobSize(vsb), &b->layout))) return 0;
    }
    vsb->lpVtbl->Release(vsb); psb->lpVtbl->Release(psb);
    return 1;
}

static int d3d11Init(PcBackend *self, int width, int height, const char *title)
{
    D3D11Backend *b = (D3D11Backend *)self;
    DXGI_SWAP_CHAIN_DESC sc;
    RECT rc = { 0, 0, width, height };
    WNDCLASSA wc;
    static const char *cls = "ssb_pc_d3d11";
    D3D11_TEXTURE2D_DESC dd;
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv;
    MSG dummy; (void)dummy;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcA; wc.lpszClassName = cls; wc.hInstance = GetModuleHandleA(NULL);
    RegisterClassA(&wc);
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    b->hwnd = CreateWindowExA(0, cls, title ? title : "SSB PC Port", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                              NULL, NULL, wc.hInstance, NULL);
    b->width = width; b->height = height;

    memset(&sc, 0, sizeof(sc));
    sc.BufferDesc.Width = width; sc.BufferDesc.Height = height;
    sc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc.SampleDesc.Count = 1; sc.BufferCount = 1;
    sc.OutputWindow = b->hwnd; sc.Windowed = TRUE;
    sc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    {
        D3D_FEATURE_LEVEL flWant[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
        D3D_FEATURE_LEVEL flGot;
        /* Runtime-load d3d11.dll so the final .exe has no import-time
         * dependency: machines without D3D11 fall through to the next
         * backend instead of failing with "dll not found". */
        typedef HRESULT (WINAPI *PFN_D3D11CDS)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE,
            UINT, const D3D_FEATURE_LEVEL *, UINT, UINT, const DXGI_SWAP_CHAIN_DESC *,
            IDXGISwapChain **, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
        PFN_D3D11CDS createFn = NULL;
        HMODULE d3d11Dll = LoadLibraryA("d3d11.dll");
        if (d3d11Dll) createFn = (PFN_D3D11CDS)(void *)GetProcAddress(d3d11Dll, "D3D11CreateDeviceAndSwapChain");
        if (!createFn) return 0;
        if (FAILED(createFn(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                flWant, 3, D3D11_SDK_VERSION, &sc, &b->swap, &b->device, &flGot, &b->ctx)))
            return 0;
    }
    pclog("d3d11: device created\n");
    if (!compileShaders(b)) return 0;

    /* render target view from back buffer */
    {
        ID3D11Texture2D *bb = NULL;
        if (FAILED(b->swap->lpVtbl->GetBuffer(b->swap, 0, &IID_ID3D11Texture2D, (void **)&bb))) return 0;
        if (FAILED(b->device->lpVtbl->CreateRenderTargetView(b->device, (ID3D11Resource *)bb, NULL, &b->rtv))) { bb->lpVtbl->Release(bb); return 0; }
        bb->lpVtbl->Release(bb);
    }
    memset(&dd, 0, sizeof(dd));
    dd.Width = width; dd.Height = height; dd.MipLevels = 1; dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_DEFAULT; dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(b->device->lpVtbl->CreateTexture2D(b->device, &dd, NULL, &b->depthTex))) return 0;
    memset(&dsv, 0, sizeof(dsv));
    dsv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (FAILED(b->device->lpVtbl->CreateDepthStencilView(b->device, (ID3D11Resource *)b->depthTex, &dsv, &b->dsav))) return 0;

    {   /* dynamic vertex buffer for one triangle at a time */
        D3D11_BUFFER_DESC vd;
        memset(&vd, 0, sizeof(vd));
        vd.ByteWidth = sizeof(GpuVert) * 3; vd.Usage = D3D11_USAGE_DEFAULT;
        vd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        if (FAILED(b->device->lpVtbl->CreateBuffer(b->device, &vd, NULL, &b->vb))) return 0;
    }
    /* rasterizer + depth states */
    {
        D3D11_RASTERIZER_DESC rd;
        memset(&rd, 0, sizeof(rd));
        rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.ScissorEnable = TRUE;
        rd.DepthClipEnable = TRUE;
        b->device->lpVtbl->CreateRasterizerState(b->device, &rd, &b->rsNoCull);
        rd.CullMode = D3D11_CULL_FRONT;
        b->device->lpVtbl->CreateRasterizerState(b->device, &rd, &b->rsFront);
        rd.CullMode = D3D11_CULL_BACK;
        b->device->lpVtbl->CreateRasterizerState(b->device, &rd, &b->rsBack);
    }
    {
        D3D11_DEPTH_STENCIL_DESC dsd;
        memset(&dsd, 0, sizeof(dsd));
        dsd.DepthEnable = FALSE;
        b->device->lpVtbl->CreateDepthStencilState(b->device, &dsd, &b->dsOff);
        dsd.DepthEnable = TRUE; dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dsd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        b->device->lpVtbl->CreateDepthStencilState(b->device, &dsd, &b->dsOn);
    }
    b->viewport[0] = 1.0f; b->viewport[2] = 1.0f;
    ShowWindow(b->hwnd, SW_SHOW);
    pclog("d3d11: initialized %dx%d\n", width, height);
    return 1;
}

static void d3d11Shutdown(PcBackend *self)
{
    D3D11Backend *b = (D3D11Backend *)self;
    if (b->ctx) b->ctx->lpVtbl->ClearState(b->ctx);
#define REL(x) do { if (b->x) { b->x->lpVtbl->Release(b->x); b->x = NULL; } } while (0)
    REL(swap); REL(rtv); REL(dsav); REL(depthTex); REL(vb); REL(cbPerDraw);
    REL(vs); REL(ps); REL(layout); REL(rsNoCull); REL(rsFront); REL(rsBack);
    REL(dsOff); REL(dsOn); REL(ctx); REL(device);
#undef REL
    if (b->hwnd) DestroyWindow(b->hwnd);
}

static void d3d11BeginFrame(PcBackend *self) { (void)self; }

static void d3d11EndFrame(PcBackend *self)
{
    D3D11Backend *b = (D3D11Backend *)self;
    MSG msg;
    /* pump window messages so the OS can repaint/move/close the window */
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    b->swap->lpVtbl->Present(b->swap, 1, 0);
}

static void d3d11BindRdram(PcBackend *self, const uint8_t *r, size_t s)
{ D3D11Backend *b = (D3D11Backend *)self; b->rdram = r; b->rdram_size = s; }

static void d3d11SetViewport(PcBackend *self, float lx, float ly, float sx, float sy)
{
    D3D11Backend *b = (D3D11Backend *)self;
    D3D11_VIEWPORT v;
    b->viewport[0] = sx > 0 ? (float)b->width / sx : 1.0f;
    b->viewport[1] = lx * b->viewport[0];
    b->viewport[2] = sy > 0 ? (float)b->height / sy : 1.0f;
    b->viewport[3] = ly * b->viewport[2];
    /* also drive the real GPU viewport so sub-window viewports clip */
    memset(&v, 0, sizeof(v));
    v.TopLeftX = 0; v.TopLeftY = 0;
    v.Width = (FLOAT)b->width; v.Height = (FLOAT)b->height;
    v.MinDepth = 0.0f; v.MaxDepth = 1.0f;
    b->ctx->lpVtbl->RSSetViewports(b->ctx, 1, &v);
}
static void d3d11SetScissor(PcBackend *self, float l, float t, float r, float b)
{
    /* scissor is applied per-draw from PcDrawState::scissor (see DrawTris);
     * nothing global to cache here. */
    (void)self; (void)l; (void)t; (void)r; (void)b;
}
static void d3d11SetOthermode(PcBackend *s, uint64_t h, uint64_t l) { (void)s;(void)h;(void)l; }
static void d3d11SetGeomMode(PcBackend *s, uint32_t m) { (void)s;(void)m; }
static void d3d11SetCombine(PcBackend *s, const uint32_t a[4], const uint32_t b[4], const uint32_t c[4], const uint32_t d[4])
{ (void)s;(void)a;(void)b;(void)c;(void)d; }
static void d3d11SetTexture(PcBackend *s, const PcTexInfo *t) { (void)s;(void)t; }
static void d3d11SetPrimColor(PcBackend *s, uint32_t c) { (void)s;(void)c; }
static void d3d11SetEnvColor(PcBackend *s, uint32_t c) { (void)s;(void)c; }
static void d3d11SetMatrices(PcBackend *s, const float m[16], const float p[16]) { (void)s;(void)m;(void)p; }

static void mapTri(const PcVertex *v, const float vp[4], int w, int h, GpuVert *out)
{
    float px = v->pos[0] * vp[0] + vp[1];
    float py = v->pos[1] * vp[2] + vp[3];
    out->x = (px / (float)w) * 2.0f - 1.0f;
    out->y = (py / (float)h) * 2.0f - 1.0f;
    out->z = v->pos[2] * 0.5f + 0.5f;
    if (out->z < 0) out->z = 0;
    if (out->z > 1) out->z = 1;
    out->r = v->color[0]; out->g = v->color[1]; out->b = v->color[2]; out->a = v->color[3];
}

static void d3d11DrawTris(PcBackend *self, const PcVertex *v0, const PcVertex *v1,
                          const PcVertex *v2, const PcDrawState *st)
{
    D3D11Backend *b = (D3D11Backend *)self;
    GpuVert tri[3];
    FLOAT bcol[4] = { 0, 0, 0, 1 };
    UINT stride = sizeof(GpuVert), offset = 0;
    mapTri(v0, b->viewport, b->width, b->height, &tri[0]);
    mapTri(v1, b->viewport, b->width, b->height, &tri[1]);
    mapTri(v2, b->viewport, b->width, b->height, &tri[2]);
    b->ctx->lpVtbl->RSSetState(b->ctx, st->cull_face == 2 ? b->rsBack
                                : st->cull_face == 1 ? b->rsFront : b->rsNoCull);
    /* per-draw scissor (N64 fb space -> render-target pixels) */
    {
        D3D11_RECT sr;
        if (st->scissor[2] > st->scissor[0]) {
            sr.left   = (LONG)(st->scissor[0]);
            sr.top    = (LONG)(st->scissor[1]);
            sr.right  = (LONG)(st->scissor[2]);
            sr.bottom = (LONG)(st->scissor[3]);
        } else {
            sr.left = 0; sr.top = 0; sr.right = b->width; sr.bottom = b->height;
        }
        b->ctx->lpVtbl->RSSetScissorRects(b->ctx, 1, &sr);
    }
    b->ctx->lpVtbl->OMSetDepthStencilState(b->ctx, st->depth_test ? b->dsOn : b->dsOff, 0);
    b->ctx->lpVtbl->UpdateSubresource(b->ctx, (ID3D11Resource *)b->vb, 0, NULL, tri, 0, 0);
    b->ctx->lpVtbl->IASetInputLayout(b->ctx, b->layout);
    b->ctx->lpVtbl->IASetVertexBuffers(b->ctx, 0, 1, &b->vb, &stride, &offset);
    b->ctx->lpVtbl->IASetPrimitiveTopology(b->ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    b->ctx->lpVtbl->VSSetShader(b->ctx, b->vs, NULL, 0);
    b->ctx->lpVtbl->PSSetShader(b->ctx, b->ps, NULL, 0);
    b->ctx->lpVtbl->OMSetRenderTargets(b->ctx, 1, &b->rtv, b->dsav);
    b->ctx->lpVtbl->Draw(b->ctx, 3, 0);
    (void)bcol;
    b->draws++;
}

static void d3d11FillRect(PcBackend *self, float x0, float y0, float x1, float y1,
                          uint32_t rgba, const PcDrawState *st)
{
    D3D11Backend *b = (D3D11Backend *)self;
    D3D11_RECT r; float fc[4];
    (void)st;
    r.left = (LONG)x0; r.top = (LONG)y0; r.right = (LONG)x1 + 1; r.bottom = (LONG)y1 + 1;
    fc[0] = ((rgba >> 24) & 0xFF) / 255.0f; fc[1] = ((rgba >> 16) & 0xFF) / 255.0f;
    fc[2] = ((rgba >> 8) & 0xFF) / 255.0f;  fc[3] = (rgba & 0xFF) / 255.0f;
    b->ctx->lpVtbl->ClearDepthStencilView(b->ctx, b->dsav, D3D11_CLEAR_DEPTH, 1.0f, 0);
    b->ctx->lpVtbl->RSSetScissorRects(b->ctx, 1, &r);
    b->ctx->lpVtbl->ClearRenderTargetView(b->ctx, b->rtv, fc);
    b->ctx->lpVtbl->RSSetScissorRects(b->ctx, 0, NULL);
}

static void d3d11Clear(PcBackend *self, uint32_t rgba, float depth)
{
    D3D11Backend *b = (D3D11Backend *)self;
    float fc[4] = { ((rgba >> 24) & 0xFF) / 255.0f, ((rgba >> 16) & 0xFF) / 255.0f,
                    ((rgba >> 8) & 0xFF) / 255.0f, (rgba & 0xFF) / 255.0f };
    b->ctx->lpVtbl->ClearRenderTargetView(b->ctx, b->rtv, fc);
    b->ctx->lpVtbl->ClearDepthStencilView(b->ctx, b->dsav, D3D11_CLEAR_DEPTH, depth, 0);
}

static void d3d11GetSize(PcBackend *self, int *w, int *h)
{ D3D11Backend *b = (D3D11Backend *)self; if (w) *w = b->width; if (h) *h = b->height; }

PcBackend *backendCreateD3D11(void)
{
    static PcBackend iface;
    memset(&g_d3d11, 0, sizeof(g_d3d11));
    iface.name = "Direct3D 11";
    iface.init = d3d11Init;
    iface.shutdown = d3d11Shutdown;
    iface.begin_frame = d3d11BeginFrame;
    iface.end_frame = d3d11EndFrame;
    iface.bind_rdram = d3d11BindRdram;
    iface.set_viewport = d3d11SetViewport;
    iface.set_scissor = d3d11SetScissor;
    iface.set_othermode = d3d11SetOthermode;
    iface.set_geom_mode = d3d11SetGeomMode;
    iface.set_combine = d3d11SetCombine;
    iface.set_texture = d3d11SetTexture;
    iface.set_prim_color = d3d11SetPrimColor;
    iface.set_env_color = d3d11SetEnvColor;
    iface.set_matrices = d3d11SetMatrices;
    iface.draw_triangles = d3d11DrawTris;
    iface.fill_rect = d3d11FillRect;
    iface.clear = d3d11Clear;
    iface.get_size = d3d11GetSize;
    return &iface;
}

#endif /* PCPORT_HAVE_D3D */
