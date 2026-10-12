/*
 * ssb-decomp PC Port - Direct3D 10 backend.
 *
 * Same state model as the D3D11 backend, on the older ID3D10* interfaces
 * (no immediate-context split; device == context). Only compiled when
 * PCPORT_HAVE_D3D is set (Windows + Platform SDK).
 */
#ifdef PCPORT_HAVE_D3D

#include "pc_port_common.h"
#include "pc_sw_raster.h"
#include <windows.h>
#include <d3d10.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "d3d10.lib")

typedef struct D3D10Backend {
    PcBackend base;
    ID3D10Device *dev;
    IDXGISwapChain *swap;
    ID3D10RenderTargetView *rtv;
    ID3D10DepthStencilView *dsav;
    ID3D10Texture2D *depthTex;
    ID3D10Buffer *vb;
    ID3D10VertexShader *vs;
    ID3D10PixelShader *ps;
    ID3D10InputLayout *layout;
    ID3D10RasterizerState *rsNoCull, *rsFront, *rsBack;
    ID3D10DepthStencilState *dsOff, *dsOn;
    HWND hwnd;
    int width, height;
    float viewport[4];
    const uint8_t *rdram;
    size_t rdram_size;
    unsigned draws;
} D3D10Backend;

static D3D10Backend g_d3d10;

typedef struct { float x, y, z; float r, g, b, a; } GpuVert10;

static const char *VS_SRC =
    "struct VS_IN { float3 pos : POSITION; float4 col : COLOR; };"
    "struct VS_OUT { float4 pos : SV_POSITION; float4 col : COLOR; };"
    "VS_OUT main(VS_IN i) { VS_OUT o; o.pos = float4(i.pos.x, -i.pos.y, i.pos.z, 1.0);"
    " o.col = i.col; return o; }";
static const char *PS_SRC =
    "float4 main(float4 pos : SV_POSITION, float4 col : COLOR) : SV_TARGET { return col; }";

static int compileShaders10(D3D10Backend *b)
{
    ID3D10Blob *vsb = NULL, *psb = NULL, *err = NULL;
    HMODULE dcc = LoadLibraryA("d3dcompiler.dll");
    typedef HRESULT (WINAPI *pfn)(LPCVOID, SIZE_T, LPCVOID, const void *, const void *,
                                  LPCVOID, LPCVOID, UINT, UINT, ID3D10Blob **, ID3D10Blob **);
    pfn compile = dcc ? (pfn)GetProcAddress(dcc, "D3DCompile") : NULL;
    if (!compile) return 0;
    if (FAILED(compile(VS_SRC, strlen(VS_SRC), NULL, NULL, NULL, "main", "vs_4_0", 0, 0, &vsb, &err)))
        return 0;
    if (FAILED(compile(PS_SRC, strlen(PS_SRC), NULL, NULL, NULL, "main", "ps_4_0", 0, 0, &psb, &err))) {
        vsb->Release(vsb); return 0;
    }
    if (FAILED(b->dev->CreateVertexShader(b->dev, vsb->GetBufferPointer(vsb),
            vsb->GetBufferSize(vsb), &b->vs))) return 0;
    if (FAILED(b->dev->CreatePixelShader(b->dev, psb->GetBufferPointer(psb),
            psb->GetBufferSize(psb), &b->ps))) return 0;
    {
        D3D10_INPUT_ELEMENT_DESC il[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D10_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D10_INPUT_PER_VERTEX_DATA, 0 },
        };
        if (FAILED(b->dev->CreateInputLayout(b->dev, il, 2,
                vsb->GetBufferPointer(vsb), vsb->GetBufferSize(vsb), &b->layout))) return 0;
    }
    vsb->Release(vsb); psb->Release(psb);
    return 1;
}

static int d3d10Init(PcBackend *self, int width, int height, const char *title)
{
    D3D10Backend *b = (D3D10Backend *)self;
    DXGI_SWAP_CHAIN_DESC sc;
    RECT rc = { 0, 0, width, height };
    WNDCLASSA wc;
    static const char *cls = "ssb_pc_d3d10";
    D3D10_TEXTURE2D_DESC dd;
    D3D10_DEPTH_STENCIL_VIEW_DESC dsv;

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

    if (FAILED(D3D10CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
            D3D10_SDK_VERSION, &sc, &b->swap, &b->dev)))
        return 0;
    if (!compileShaders10(b)) return 0;

    {
        ID3D10Texture2D *bb = NULL;
        if (FAILED(b->swap->GetBuffer(b->swap, 0, &IID_ID3D10Texture2D, (void **)&bb))) return 0;
        if (FAILED(b->dev->CreateRenderTargetView(b->dev, bb, NULL, &b->rtv))) { bb->Release(bb); return 0; }
        bb->Release(bb);
    }
    memset(&dd, 0, sizeof(dd));
    dd.Width = width; dd.Height = height; dd.MipLevels = 1; dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; dd.SampleDesc.Count = 1;
    dd.Usage = D3D10_USAGE_DEFAULT; dd.BindFlags = D3D10_BIND_DEPTH_STENCIL;
    if (FAILED(b->dev->CreateTexture2D(b->dev, &dd, NULL, &b->depthTex))) return 0;
    memset(&dsv, 0, sizeof(dsv));
    dsv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; dsv.ViewDimension = D3D10_DSV_DIMENSION_TEXTURE2D;
    if (FAILED(b->dev->CreateDepthStencilView(b->dev, (ID3D10Resource *)b->depthTex, &dsv, &b->dsav))) return 0;

    {
        D3D10_BUFFER_DESC vd;
        memset(&vd, 0, sizeof(vd));
        vd.ByteWidth = sizeof(GpuVert10) * 3; vd.Usage = D3D10_USAGE_DEFAULT;
        vd.BindFlags = D3D10_BIND_VERTEX_BUFFER;
        if (FAILED(b->dev->CreateBuffer(b->dev, &vd, NULL, &b->vb))) return 0;
    }
    {
        D3D10_RASTERIZER_DESC rd;
        memset(&rd, 0, sizeof(rd));
        rd.FillMode = D3D10_FILL_SOLID; rd.CullMode = D3D10_CULL_NONE; rd.ScissorEnable = TRUE;
        rd.DepthClipEnable = TRUE;
        b->dev->CreateRasterizerState(b->dev, &rd, &b->rsNoCull);
        rd.CullMode = D3D10_CULL_FRONT;
        b->dev->CreateRasterizerState(b->dev, &rd, &b->rsFront);
        rd.CullMode = D3D10_CULL_BACK;
        b->dev->CreateRasterizerState(b->dev, &rd, &b->rsBack);
    }
    {
        D3D10_DEPTH_STENCIL_DESC dsd;
        memset(&dsd, 0, sizeof(dsd));
        dsd.DepthEnable = FALSE;
        b->dev->CreateDepthStencilState(b->dev, &dsd, &b->dsOff);
        dsd.DepthEnable = TRUE; dsd.DepthWriteMask = D3D10_DEPTH_WRITE_MASK_ALL;
        dsd.DepthFunc = D3D10_COMPARISON_LESS_EQUAL;
        b->dev->CreateDepthStencilState(b->dev, &dsd, &b->dsOn);
    }
    b->viewport[0] = 1.0f; b->viewport[2] = 1.0f;
    ShowWindow(b->hwnd, SW_SHOW);
    pclog("d3d10: initialized %dx%d\n", width, height);
    return 1;
}

static void d3d10Shutdown(PcBackend *self)
{
    D3D10Backend *b = (D3D10Backend *)self;
#define REL(x) do { if (b->x) { b->x->Release(b->x); b->x = NULL; } } while (0)
    REL(swap); REL(rtv); REL(dsav); REL(depthTex); REL(vb);
    REL(vs); REL(ps); REL(layout); REL(rsNoCull); REL(rsFront); REL(rsBack);
    REL(dsOff); REL(dsOn); REL(dev);
#undef REL
    if (b->hwnd) DestroyWindow(b->hwnd);
}

static void d3d10BeginFrame(PcBackend *self) { (void)self; }
static void d3d10EndFrame(PcBackend *self)
{
    D3D10Backend *b = (D3D10Backend *)self;
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    b->swap->Present(b->swap, 1, 0);
}

static void d3d10BindRdram(PcBackend *self, const uint8_t *r, size_t s)
{ D3D10Backend *b = (D3D10Backend *)self; b->rdram = r; b->rdram_size = s; }

static void d3d10SetViewport(PcBackend *self, float lx, float ly, float sx, float sy)
{
    D3D10Backend *b = (D3D10Backend *)self;
    D3D10_VIEWPORT v;
    b->viewport[0] = sx > 0 ? (float)b->width / sx : 1.0f;
    b->viewport[1] = lx * b->viewport[0];
    b->viewport[2] = sy > 0 ? (float)b->height / sy : 1.0f;
    b->viewport[3] = ly * b->viewport[2];
    memset(&v, 0, sizeof(v));
    v.TopLeftX = 0; v.TopLeftY = 0;
    v.Width = b->width; v.Height = b->height;
    v.MinDepth = 0; v.MaxDepth = 1.0f;
    b->dev->RSSetViewports(b->dev, 1, &v);
}
static void d3d10SetScissor(PcBackend *s, float l, float t, float r, float b) { (void)s;(void)l;(void)t;(void)r;(void)b; }
static void d3d10SetOthermode(PcBackend *s, uint64_t h, uint64_t l) { (void)s;(void)h;(void)l; }
static void d3d10SetGeomMode(PcBackend *s, uint32_t m) { (void)s;(void)m; }
static void d3d10SetCombine(PcBackend *s, const uint32_t a[4], const uint32_t b[4], const uint32_t c[4], const uint32_t d[4])
{ (void)s;(void)a;(void)b;(void)c;(void)d; }
static void d3d10SetTexture(PcBackend *s, const PcTexInfo *t) { (void)s;(void)t; }
static void d3d10SetPrimColor(PcBackend *s, uint32_t c) { (void)s;(void)c; }
static void d3d10SetEnvColor(PcBackend *s, uint32_t c) { (void)s;(void)c; }
static void d3d10SetMatrices(PcBackend *s, const float m[16], const float p[16]) { (void)s;(void)m;(void)p; }

static void mapTri10(const PcVertex *v, const float vp[4], int w, int h, GpuVert10 *out)
{
    float px = v->pos[0] * vp[0] + vp[1];
    float py = v->pos[1] * vp[2] + vp[3];
    out->x = (px / (float)w) * 2.0f - 1.0f;
    out->y = (py / (float)h) * 2.0f - 1.0f;
    out->z = v->pos[2] * 0.5f + 0.5f;
    if (out->z < 0) out->z = 0; if (out->z > 1) out->z = 1;
    out->r = v->color[0]; out->g = v->color[1]; out->b = v->color[2]; out->a = v->color[3];
}

static void d3d10DrawTris(PcBackend *self, const PcVertex *v0, const PcVertex *v1,
                          const PcVertex *v2, const PcDrawState *st)
{
    D3D10Backend *b = (D3D10Backend *)self;
    GpuVert10 tri[3];
    UINT stride = sizeof(GpuVert10), offset = 0;
    mapTri10(v0, b->viewport, b->width, b->height, &tri[0]);
    mapTri10(v1, b->viewport, b->width, b->height, &tri[1]);
    mapTri10(v2, b->viewport, b->width, b->height, &tri[2]);
    b->dev->RSSetState(b->dev, st->cull_face == 2 ? b->rsBack
                             : st->cull_face == 1 ? b->rsFront : b->rsNoCull);
    b->dev->OMSetDepthStencilState(b->dev, st->depth_test ? b->dsOn : b->dsOff, 0);
    b->dev->UpdateSubresource(b->dev, (ID3D10Resource *)b->vb, 0, NULL, tri, 0, 0);
    b->dev->IASetInputLayout(b->dev, b->layout);
    b->dev->IASetVertexBuffers(b->dev, 0, 1, &b->vb, &stride, &offset);
    b->dev->IASetPrimitiveTopology(b->dev, D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    b->dev->VSSetShader(b->dev, b->vs);
    b->dev->PSSetShader(b->dev, b->ps);
    b->dev->OMSetRenderTargets(b->dev, 1, &b->rtv, b->dsav);
    b->dev->Draw(b->dev, 3, 0);
    b->draws++;
}

static void d3d10FillRect(PcBackend *self, float x0, float y0, float x1, float y1,
                          uint32_t rgba, const PcDrawState *st)
{
    D3D10Backend *b = (D3D10Backend *)self;
    D3D10_RECT r; float fc[4];
    (void)st;
    r.left = (LONG)x0; r.top = (LONG)y0; r.right = (LONG)x1 + 1; r.bottom = (LONG)y1 + 1;
    fc[0] = ((rgba >> 24) & 0xFF) / 255.0f; fc[1] = ((rgba >> 16) & 0xFF) / 255.0f;
    fc[2] = ((rgba >> 8) & 0xFF) / 255.0f;  fc[3] = (rgba & 0xFF) / 255.0f;
    b->dev->ClearDepthStencilView(b->dev, b->dsav, D3D10_CLEAR_DEPTH, 1.0f, 0);
    b->dev->RSSetScissorRects(b->dev, 1, &r);
    b->dev->ClearRenderTargetView(b->dev, b->rtv, fc);
    b->dev->RSSetScissorRects(b->dev, 0, NULL);
}

static void d3d10Clear(PcBackend *self, uint32_t rgba, float depth)
{
    D3D10Backend *b = (D3D10Backend *)self;
    float fc[4] = { ((rgba >> 24) & 0xFF) / 255.0f, ((rgba >> 16) & 0xFF) / 255.0f,
                    ((rgba >> 8) & 0xFF) / 255.0f, (rgba & 0xFF) / 255.0f };
    b->dev->ClearRenderTargetView(b->dev, b->rtv, fc);
    b->dev->ClearDepthStencilView(b->dev, b->dsav, D3D10_CLEAR_DEPTH, depth, 0);
}

static void d3d10GetSize(PcBackend *self, int *w, int *h)
{ D3D10Backend *b = (D3D10Backend *)self; if (w) *w = b->width; if (h) *h = b->height; }

PcBackend *backendCreateD3D10(void)
{
    static PcBackend iface;
    memset(&g_d3d10, 0, sizeof(g_d3d10));
    iface.name = "Direct3D 10";
    iface.init = d3d10Init;
    iface.shutdown = d3d10Shutdown;
    iface.begin_frame = d3d10BeginFrame;
    iface.end_frame = d3d10EndFrame;
    iface.bind_rdram = d3d10BindRdram;
    iface.set_viewport = d3d10SetViewport;
    iface.set_scissor = d3d10SetScissor;
    iface.set_othermode = d3d10SetOthermode;
    iface.set_geom_mode = d3d10SetGeomMode;
    iface.set_combine = d3d10SetCombine;
    iface.set_texture = d3d10SetTexture;
    iface.set_prim_color = d3d10SetPrimColor;
    iface.set_env_color = d3d10SetEnvColor;
    iface.set_matrices = d3d10SetMatrices;
    iface.draw_triangles = d3d10DrawTris;
    iface.fill_rect = d3d10FillRect;
    iface.clear = d3d10Clear;
    iface.get_size = d3d10GetSize;
    return &iface;
}

#endif /* PCPORT_HAVE_D3D */
