/*
 * ssb_launcher — Janela de configuração avançada do port PC.
 *
 * GUI nativa no Windows (Win32/Common Controls, sem dependências extras) e
 * TUI interativa em terminal nos demais sistemas. O launcher:
 *   1. deixa escolher a ROM (.z64/.v64/.n64) com validação de compatibilidade;
 *   2. expõe as configurações avançadas (backend, resolução interna, escala,
 *      aspect ratio, vsync, FPS cap, cull, depth, wireframe, MSAA, anisotropy,
 *      log level, dump_dir, sim_buttons);
 *   3. salva ssb_config.ini (formato lido por pc_config.c);
 *   4. inicia ssb_pc.exe/ssb_pc passando --config (e variáveis SSB_* como
 *      redundância para builds antigos).
 */
#ifndef _WIN32
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#  ifndef _DEFAULT_SOURCE
#    define _DEFAULT_SOURCE
#  endif
#  include <unistd.h>   /* readlink for exe_dir */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#ifdef _MSC_VER
#pragma comment(lib, "comdlg32.lib")
#endif
/* mingw-w64 headers omit the Button_GetCheck/SetCheck inline wrappers */
#ifndef Button_GetCheck
#define Button_GetCheck(hwnd) (BOOL)SendMessageA((hwnd), BM_GETCHECK, 0, 0L)
#endif
#ifndef Button_SetCheck
#define Button_SetCheck(hwnd, check) SendMessageA((hwnd), BM_SETCHECK, (WPARAM)(check), 0L)
#endif
#endif

/* ---------------------------------------------------------------- helpers */
typedef struct {
    char rom[512];
    char backend[16];
    int  width, height, fullscreen, vsync, max_fps, frames, headless;
    float resolution_scale, aspect;
    int  integer_scale, internal_width, internal_height;
    int  cull_mode, depth_func, wireframe, anisotropy, msaa, log_level;
    unsigned clear_color;
    unsigned sim_buttons;
    char dump_dir[512];
} Lcfg;

static Lcfg g = {
    "", "auto",
    1280, 720, 0, 1, 0, 0, 0,
    1.0f, 4.0f / 3.0f,
    0, 0, 0,
    2, 2, 0, 1, 0, 2,
    0x101418FFu,
    0,
    ""
};

static void trim(char *s) {
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
    char *p = s; while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

static int load_ini(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#'); if (h) *h = '\0';
        char *eq = strchr(line, '='); if (!eq) continue;
        *eq = 0; char *k = line, *v = eq + 1;
        trim(k); trim(v);
        for (char *p = k; *p; p++) *p = (char)tolower((unsigned char)*p);
        if      (!strcmp(k, "rom")) snprintf(g.rom, sizeof g.rom, "%s", v);
        else if (!strcmp(k, "backend")) snprintf(g.backend, sizeof g.backend, "%s", v);
        else if (!strcmp(k, "width")) g.width = atoi(v);
        else if (!strcmp(k, "height")) g.height = atoi(v);
        else if (!strcmp(k, "fullscreen")) g.fullscreen = atoi(v);
        else if (!strcmp(k, "vsync")) g.vsync = atoi(v);
        else if (!strcmp(k, "max_fps")) g.max_fps = atoi(v);
        else if (!strcmp(k, "frames")) g.frames = atoi(v);
        else if (!strcmp(k, "headless")) g.headless = atoi(v);
        else if (!strcmp(k, "resolution_scale")) g.resolution_scale = (float)atof(v);
        else if (!strcmp(k, "aspect")) g.aspect = (float)atof(v);
        else if (!strcmp(k, "integer_scale")) g.integer_scale = atoi(v);
        else if (!strcmp(k, "internal_width")) g.internal_width = atoi(v);
        else if (!strcmp(k, "internal_height")) g.internal_height = atoi(v);
        else if (!strcmp(k, "cull_mode")) g.cull_mode = atoi(v);
        else if (!strcmp(k, "depth_func")) g.depth_func = atoi(v);
        else if (!strcmp(k, "wireframe")) g.wireframe = atoi(v);
        else if (!strcmp(k, "anisotropy")) g.anisotropy = atoi(v);
        else if (!strcmp(k, "msaa")) g.msaa = atoi(v);
        else if (!strcmp(k, "log_level")) g.log_level = atoi(v);
        else if (!strcmp(k, "clear_color")) g.clear_color = (unsigned)strtoul(v, NULL, 0);
        else if (!strcmp(k, "sim_buttons")) g.sim_buttons = (unsigned)strtoul(v, NULL, 0);
        else if (!strcmp(k, "dump_dir")) snprintf(g.dump_dir, sizeof g.dump_dir, "%s", v);
    }
    fclose(f);
    return 0;
}

static int save_ini(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "# ssb_pc configuracao avancada (gerada pelo launcher)\n");
    fprintf(f, "rom = %s\n", g.rom);
    fprintf(f, "backend = %s\n", g.backend);
    fprintf(f, "width = %d\n", g.width);
    fprintf(f, "height = %d\n", g.height);
    fprintf(f, "fullscreen = %d\n", g.fullscreen);
    fprintf(f, "vsync = %d\n", g.vsync);
    fprintf(f, "max_fps = %d\n", g.max_fps);
    fprintf(f, "frames = %d\n", g.frames);
    fprintf(f, "headless = %d\n", g.headless);
    fprintf(f, "resolution_scale = %.3f\n", (double)g.resolution_scale);
    fprintf(f, "aspect = %.4f\n", (double)g.aspect);
    fprintf(f, "integer_scale = %d\n", g.integer_scale);
    fprintf(f, "internal_width = %d\n", g.internal_width);
    fprintf(f, "internal_height = %d\n", g.internal_height);
    fprintf(f, "cull_mode = %d\n", g.cull_mode);
    fprintf(f, "depth_func = %d\n", g.depth_func);
    fprintf(f, "wireframe = %d\n", g.wireframe);
    fprintf(f, "clear_color = 0x%08X\n", g.clear_color);
    fprintf(f, "anisotropy = %d\n", g.anisotropy);
    fprintf(f, "msaa = %d\n", g.msaa);
    fprintf(f, "log_level = %d\n", g.log_level);
    fprintf(f, "sim_buttons = 0x%X\n", g.sim_buttons);
    fprintf(f, "dump_dir = %s\n", g.dump_dir);
    fclose(f);
    return 0;
}

/* quick compatibility gate (mirrors rom_loader.c rules, header-only read) */
static int rom_ok(const char *path, char *msg, size_t msgcap) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(msg, msgcap, "arquivo nao encontrado"); return 0; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char hdr[0x40];
    if (sz < 0x100 || fread(hdr, 1, sizeof hdr, f) != sizeof hdr) {
        fclose(f); snprintf(msg, msgcap, "arquivo pequeno demais p/ ROM N64"); return 0;
    }
    fclose(f);
    int order = 0; /* 0 z64 BE, 1 v64 swapped, 2 n64 little */
    if      (hdr[0x00] == 0x80 && hdr[0x01] == 0x37) order = 0;
    else if (hdr[0x00] == 0x37 && hdr[0x01] == 0x80) order = 1;
    else if (hdr[0x00] == 0x40 && hdr[0x01] == 0x80) order = 2;
    else { snprintf(msg, msgcap, "magic invalido (nao e ROM N64)"); return 0; }
    /* cart code at 0x3B..0x3D in logical layout; remap per byte order */
    unsigned char cc[3];
    for (int i = 0; i < 3; i++) {
        size_t off = 0x3B + (size_t)i;
        switch (order) {
            case 0: cc[i] = hdr[off]; break;
            case 1: cc[i] = hdr[(off & ~(size_t)3u) | ((off ^ 2u) & 3u)]; break;
            default: cc[i] = hdr[(off & ~(size_t)3u) | ((off ^ 3u) & 3u)]; break;
        }
    }
    if (!((cc[0] == 'S' && cc[1] == 'M') || (cc[0] == 'S' && cc[1] == 'N'))) {
        snprintf(msg, msgcap,
                 "cart code '%c%c%c' nao e Super Smash Bros. (esperado SM*/SN*)",
                 isprint(cc[0]) ? cc[0] : '?', isprint(cc[1]) ? cc[1] : '?',
                 isprint(cc[2]) ? cc[2] : '?');
        return 0;
    }
    snprintf(msg, msgcap, "ROM valida (%s), tamanho %ld MB",
             order == 0 ? "z64" : order == 1 ? "v64" : "n64", sz / 1048576L);
    return 1;
}

static void print_summary(void) {
    printf("---------------------------------------------------------------\n");
    printf(" ROM:              %s\n", g.rom[0] ? g.rom : "(nenhuma - cena demo)");
    printf(" Backend:          %s\n", g.backend);
    printf(" Janela:           %dx%d  fullscreen=%d  vsync=%d  fps_cap=%d\n",
           g.width, g.height, g.fullscreen, g.vsync, g.max_fps);
    printf(" Resolucao intern.: %.2fx (%dx%d)  aspect=%.3f  int_scale=%d\n",
           (double)g.resolution_scale, g.internal_width, g.internal_height,
           (double)g.aspect, g.integer_scale);
    printf(" Render:           cull=%d depth=%d wire=%d aniso=%d msaa=%d clear=0x%08X\n",
           g.cull_mode, g.depth_func, g.wireframe, g.anisotropy, g.msaa, g.clear_color);
    printf(" Debug:            log=%d frames=%d headless=%d dump='%s' sim_btn=0x%X\n",
           g.log_level, g.frames, g.headless, g.dump_dir, g.sim_buttons);
    printf("---------------------------------------------------------------\n");
}

/* ------------------------------------------------------- ROM install dir */
/* Directory that contains the running executable (best effort). */
static void exe_dir(char *out, size_t cap) {
#ifdef _WIN32
    char full[MAX_PATH * 2];
    DWORD n = GetModuleFileNameA(NULL, full, sizeof full);
    if (n == 0 || n >= sizeof full) { snprintf(out, cap, "."); return; }
    char *slash = strrchr(full, '\\');
    if (!slash) { snprintf(out, cap, "."); return; }
    *slash = '\0';
    snprintf(out, cap, "%s\\", full);   /* trailing separator */
#else
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0) { snprintf(out, cap, "."); return; }
    out[n] = '\0';
    char *slash = strrchr(out, '/');
    if (slash) slash[1] = '\0'; else snprintf(out, cap, "./");
#endif
}

/* Copy the selected ROM next to the executables as "smash.z64" — the name
 * ssb_pc/ssb_launcher auto-discover at startup. On success g.rom is updated
 * to the installed path. Returns 1 on success. */
static int install_rom_local(void) {
    char dir[1024], dst[1024 + 16];
    FILE *in, *out;
    static char buf[65536];
    size_t rd;

    if (!g.rom[0]) return 0;
    exe_dir(dir, sizeof dir);
#ifdef _WIN32
    snprintf(dst, sizeof dst, "%ssmash.z64", dir);
#else
    snprintf(dst, sizeof dst, "%ssmash.z64", dir);
#endif
    if (!strcmp(g.rom, dst)) return 1; /* already installed here */
    in = fopen(g.rom, "rb");
    if (!in) return 0;
    out = fopen(dst, "wb");
    if (!out) { fclose(in); return 0; }
    while ((rd = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, rd, out) != rd) { fclose(in); fclose(out); return 0; }
    }
    fclose(in); fclose(out);
    if (strlen(dst) < sizeof g.rom) snprintf(g.rom, sizeof g.rom, "%s", dst);
    printf("ROM instalada na pasta do programa: %s\n", dst);
    return 1;
}

/* ------------------------------------------------------------- launching */
static int find_game_exe(char *out, size_t cap) {
    /* Search order: beside the launcher (dist layout), then build/,
     * build/Release/ and pc_port/build/ under the current dir. */
#ifdef _WIN32
    const char *subdirs[] = { "", "build\\", "build\\Release\\",
                              "pc_port\\build\\", "pc_port\\build\\Release\\", NULL };
#else
    const char *subdirs[] = { "", "build/", "pc_port/build/", NULL };
#endif
    char dir[1024];
    exe_dir(dir, sizeof dir);
    const char *bases[] = { dir, "./", NULL };
    for (size_t b = 0; bases[b]; b++)
        for (size_t i = 0; subdirs[i]; i++) {
            snprintf(out, cap, "%s%s%s.exe", bases[b], subdirs[i], "ssb_pc");
#ifdef _WIN32
            FILE *t0 = fopen(out, "rb");
            if (t0) { fclose(t0); return 1; }
#endif
            snprintf(out, cap, "%s%s%s", bases[b], subdirs[i], "ssb_pc");
            FILE *t = fopen(out, "rb");
            if (t) { fclose(t); return 1; }
        }
    return 0;
}

static int launch_game(int build_if_missing) {
    const char *ini = "ssb_config.ini";
    char exe[1600] = "";
    if (g.rom[0] && !install_rom_local())
        printf("AVISO: nao foi possivel instalar a ROM ao lado dos executaveis;"
               " usando o caminho original.\n");
    if (save_ini(ini) != 0) { fprintf(stderr, "falha ao salvar %s\n", ini); return 1; }
    printf("config gravada em %s\n", ini);

    if (!find_game_exe(exe, sizeof exe)) {
        if (!build_if_missing) {
            fprintf(stderr, "executavel do jogo nao encontrado;\n"
                    "extraia todo o conteudo do zip para UMA pasta e rode o launcher de la.\n");
            return 1;
        }
        printf("compilando o executavel...\n");
#ifdef _WIN32
        int rc = system("cmake -S . -B build && cmake --build build --config Release");
#else
        int rc = system("cmake -S . -B build >/dev/null && cmake --build build -j2");
#endif
        if (rc != 0) { fprintf(stderr, "build falhou\n"); return 1; }
        if (!find_game_exe(exe, sizeof exe)) {
            fprintf(stderr, "executavel ainda nao encontrado apos build\n");
            return 1;
        }
    }
    printf("executavel: %s\n", exe);

    char cmd[6400];
    snprintf(cmd, sizeof cmd,
#ifdef _WIN32
             "set \"SSB_BACKEND=%s\" && set \"SSB_ROM=%s\" && \"%s\" --config %s",
#else
             "SSB_BACKEND='%s' SSB_ROM='%s' '%s' --config %s",
#endif
             g.backend, g.rom, exe, ini);
    printf("iniciando: %s\n", cmd);
    return system(cmd);
}

/* ------------------------------------------------------------ Win32 GUI */
#ifdef _WIN32
static HWND hRom, hBackend, hW, hH, hScale, hAspect, hVsync, hFps, hFrames;
static HWND hCull, hDepth, hMsaa, hAniso, hLog, hDump, hFs, hWire, hIntScale;

static void get_text(HWND h, char *buf, size_t cap) { GetWindowTextA(h, buf, (int)cap); }
static int   get_int(HWND h, int dflt) { char b[64]; get_text(h, b, sizeof b); return b[0] ? atoi(b) : dflt; }
static float get_flt(HWND h, float dflt) { char b[64]; get_text(h, b, sizeof b); return b[0] ? (float)atof(b) : dflt; }

static void gui_collect(void) {
    get_text(hRom, g.rom, sizeof g.rom);
    get_text(hBackend, g.backend, sizeof g.backend);
    g.width  = get_int(hW, 1280);
    g.height = get_int(hH, 720);
    g.resolution_scale = get_flt(hScale, 1.0f);
    g.aspect = get_flt(hAspect, 1.3333f);
    g.vsync = get_int(hVsync, 1);
    g.max_fps = get_int(hFps, 0);
    g.frames = get_int(hFrames, 0);
    g.cull_mode = get_int(hCull, 2);
    g.depth_func = get_int(hDepth, 2);
    g.msaa = get_int(hMsaa, 0);
    g.anisotropy = get_int(hAniso, 1);
    g.log_level = get_int(hLog, 2);
    get_text(hDump, g.dump_dir, sizeof g.dump_dir);
    g.fullscreen = Button_GetCheck(hFs) ? 1 : 0;
    g.wireframe = Button_GetCheck(hWire) ? 1 : 0;
    g.integer_scale = Button_GetCheck(hIntScale) ? 1 : 0;
}

static void add_label(HWND p, const char *txt, int x, int y) {
    CreateWindowA("STATIC", txt, WS_CHILD | WS_VISIBLE | SS_LEFT,
                  x, y, 150, 18, p, NULL, NULL, NULL);
}
static HWND add_edit(HWND p, const char *txt, int x, int y, int w) {
    return CreateWindowA("EDIT", txt, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                         x, y, w, 22, p, NULL, NULL, NULL);
}
static HWND add_check(HWND p, const char *txt, int x, int y, int chk) {
    HWND h = CreateWindowA("BUTTON", txt, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                           x, y, 240, 20, p, NULL, NULL, NULL);
    Button_SetCheck(h, chk ? BST_CHECKED : BST_UNCHECKED);
    return h;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case 101: { /* Escolher ROM */
            char file[MAX_PATH] = "";
            OPENFILENAMEA of; memset(&of, 0, sizeof of);
            of.lStructSize = sizeof of; of.hwndOwner = hwnd;
            of.lpstrFilter = "ROMs N64 (*.z64;*.v64;*.n64)\0*.z64;*.v64;*.n64\0Todos\0*.*\0";
            of.lpstrFile = file; of.nMaxFile = MAX_PATH;
            of.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
            if (GetOpenFileNameA(&of)) {
                SetWindowTextA(hRom, file);
                char m[256];
                if (rom_ok(file, m, sizeof m)) {
                    snprintf(g.rom, sizeof g.rom, "%s", file);
                    install_rom_local();      /* copia p/ pasta do programa */
                    SetWindowTextA(hRom, g.rom);
                    MessageBoxA(hwnd, m, "ROM compativel - instalada na pasta do jogo",
                                MB_OK | MB_ICONINFORMATION);
                } else {
                    MessageBoxA(hwnd, m, "ROM incompativel", MB_OK | MB_ICONERROR);
                }
            }
            break; }
        case 102: /* Salvar */
            gui_collect(); print_summary();
            save_ini("ssb_config.ini");
            MessageBoxA(hwnd, "Configuracoes salvas em ssb_config.ini", "Launcher",
                        MB_OK | MB_ICONINFORMATION);
            break;
        case 103: /* Jogar */
            gui_collect(); print_summary();
            if (g.rom[0]) {
                char m[256];
                if (!rom_ok(g.rom, m, sizeof m)) {
                    if (MessageBoxA(hwnd, m, "ROM incompativel", MB_YESNO | MB_ICONWARNING) != IDYES)
                        break;
                }
            }
            EnableWindow(GetDlgItem(hwnd, 103), FALSE);
            SetWindowTextA(GetDlgItem(hwnd, 103), "Iniciando...");
            launch_game(1);
            DestroyWindow(hwnd);
            break;
        case IDCANCEL: DestroyWindow(hwnd); break;
        }
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static int gui_main(HINSTANCE hInst) {
    WNDCLASSA wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "SsbLauncherWnd";
    RegisterClassA(&wc);

    HWND p = CreateWindowExA(0, wc.lpszClassName,
        "SSB64 PC Port - Launcher (config avancada)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 520, 560, NULL, NULL, hInst, NULL);

    int y = 12;
    add_label(p, "ROM:", 14, y + 2);
    hRom = add_edit(p, g.rom, 90, y, 300);
    CreateWindowA("BUTTON", "Escolher...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                  398, y, 100, 24, p, (HMENU)101, hInst, NULL);
    y += 32;

    add_label(p, "Backend:", 14, y + 2);
    hBackend = add_edit(p, g.backend, 150, y, 120);
    add_label(p, "(auto/vulkan/d3d11/d3d10/null)", 278, y + 2);
    y += 28;

    add_label(p, "Largura:", 14, y + 2);   hW = add_edit(p, "1280", 90, y, 60);
    add_label(p, "Altura:", 170, y + 2);   hH = add_edit(p, "720", 220, y, 60);
    add_label(p, "Escala:", 300, y + 2);   hScale = add_edit(p, "1.0", 350, y, 50);
    y += 28;

    add_label(p, "Aspect:", 14, y + 2);    hAspect = add_edit(p, "1.3333", 90, y, 60);
    add_label(p, "VSync:", 170, y + 2);    hVsync = add_edit(p, "1", 220, y, 40);
    add_label(p, "FPS cap:", 280, y + 2);  hFps = add_edit(p, "0", 340, y, 50);
    y += 28;

    add_label(p, "Frames:", 14, y + 2);    hFrames = add_edit(p, "0", 90, y, 60);
    add_label(p, "Cull:", 170, y + 2);     hCull = add_edit(p, "2", 220, y, 40);
    add_label(p, "Depth:", 280, y + 2);    hDepth = add_edit(p, "2", 330, y, 40);
    y += 28;

    add_label(p, "MSAA:", 14, y + 2);      hMsaa = add_edit(p, "0", 90, y, 40);
    add_label(p, "Aniso:", 150, y + 2);    hAniso = add_edit(p, "1", 200, y, 40);
    add_label(p, "Log:", 260, y + 2);      hLog = add_edit(p, "2", 300, y, 40);
    y += 28;

    add_label(p, "Dump dir:", 14, y + 2);  hDump = add_edit(p, "", 90, y, 300);
    y += 28;

    hFs = add_check(p, "Tela cheia", 14, y, g.fullscreen);
    hWire = add_check(p, "Wireframe (debug)", 260, y, g.wireframe);
    y += 24;
    hIntScale = add_check(p, "Escala inteira (mult. de 320x240)", 14, y, g.integer_scale);
    y += 34;

    CreateWindowA("BUTTON", "Salvar config", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                  14, y, 140, 30, p, (HMENU)102, hInst, NULL);
    CreateWindowA("BUTTON", "JOGAR >>", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                  358, y, 140, 30, p, (HMENU)103, hInst, NULL);

    ShowWindow(p, SW_SHOW); UpdateWindow(p);
    MSG m;
    while (GetMessageA(&m, NULL, 0, 0)) { TranslateMessage(&m); DispatchMessageA(&m); }
    return 0;
}
#endif /* _WIN32 */

/* ------------------------------------------------------------------ TUI */
static int ask_int(const char *prompt, int cur) {
    char b[64];
    printf("%s [%d]: ", prompt, cur);
    if (!fgets(b, sizeof b, stdin)) return cur;
    trim(b);
    return b[0] ? atoi(b) : cur;
}
static float ask_flt(const char *prompt, float cur) {
    char b[64];
    printf("%s [%.3f]: ", prompt, (double)cur);
    if (!fgets(b, sizeof b, stdin)) return cur;
    trim(b);
    return b[0] ? (float)atof(b) : cur;
}
static void ask_str(const char *prompt, char *dst, size_t cap) {
    char b[512];
    printf("%s [%s]: ", prompt, dst);
    if (!fgets(b, sizeof b, stdin)) return;
    trim(b);
    if (b[0]) snprintf(dst, cap, "%s", b);
}

static int tui_main(void) {
    printf("===== SSB64 PC Port - Launcher (modo texto) =====\n");
    printf("(Enter mantem o valor atual)\n");
    for (;;) {
        print_summary();
        printf("a) escolher ROM   b) backend    w) largura   h) altura\n");
        printf("s) escala         p) aspect     v) vsync     f) fps cap\n");
        printf("n) frames         c) cull       d) depth     m) msaa\n");
        printf("r) anisotropia    l) log level  u) dump dir  x) tela cheia/wire/intesc\n");
        printf("G) salvar e jogar S) salvar     q) sair\n> ");
        char sel[16];
        if (!fgets(sel, sizeof sel, stdin)) return 0;
        trim(sel);
        if (!sel[0]) continue;
        switch (sel[0]) {
        case 'a': {
            ask_str("caminho da ROM", g.rom, sizeof g.rom);
            char m[256];
            if (g.rom[0] && !rom_ok(g.rom, m, sizeof m)) printf("AVISO: %s\n", m);
            else if (g.rom[0]) printf("%s\n", m);
            break; }
        case 'b': ask_str("backend", g.backend, sizeof g.backend); break;
        case 'w': g.width = ask_int("largura", g.width); break;
        case 'h': g.height = ask_int("altura", g.height); break;
        case 's': g.resolution_scale = ask_flt("escala interna", g.resolution_scale); break;
        case 'p': g.aspect = ask_flt("aspect", g.aspect); break;
        case 'v': g.vsync = ask_int("vsync 0/1/2", g.vsync); break;
        case 'f': g.max_fps = ask_int("fps cap (0=sem limite)", g.max_fps); break;
        case 'n': g.frames = ask_int("frames (0=infinito)", g.frames); break;
        case 'c': g.cull_mode = ask_int("cull 0/1/2", g.cull_mode); break;
        case 'd': g.depth_func = ask_int("depth 0..3", g.depth_func); break;
        case 'm': g.msaa = ask_int("msaa 0/2/4/8", g.msaa); break;
        case 'r': g.anisotropy = ask_int("aniso 1..16", g.anisotropy); break;
        case 'l': g.log_level = ask_int("log 0..3", g.log_level); break;
        case 'u': ask_str("diretorio de dumps", g.dump_dir, sizeof g.dump_dir); break;
        case 'x':
            g.fullscreen = ask_int("tela cheia 0/1", g.fullscreen);
            g.wireframe = ask_int("wireframe 0/1", g.wireframe);
            g.integer_scale = ask_int("escala inteira 0/1", g.integer_scale);
            break;
        case 'S': save_ini("ssb_config.ini"); printf("salvo.\n"); break;
        case 'i':
            if (install_rom_local()) { save_ini("ssb_config.ini"); printf("instalado e salvo.\n"); }
            else printf("nada a instalar (defina a ROM em 'a').\n");
            break;
        case 'G': return launch_game(1);
        case 'q': return 0;
        default: printf("opcao desconhecida\n");
        }
    }
}

int main(int argc, char **argv) {
    load_ini("ssb_config.ini"); /* optional pre-existing config */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--launch") || !strcmp(argv[i], "-L"))
            return launch_game(1);
        if (!strcmp(argv[i], "--text") || !strcmp(argv[i], "-t"))
            return tui_main();
    }
#ifdef _WIN32
    return gui_main(GetModuleHandleA(NULL));
#else
    return tui_main();
#endif
}
