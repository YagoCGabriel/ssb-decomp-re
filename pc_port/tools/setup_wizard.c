/*
 * setup_wizard.c - native "Janela Setup" for the SSB PC port.
 *
 * Single self-contained executable (no dependencies beyond libc) that walks
 * the user through:
 *   1. selecting a compatible ROM (.z64/.v64/.n64) via a native file-open
 *      dialog on Windows (GetOpenFileNameW) and GTK3 on Linux (when built
 *      with PCPORT_HAVE_GTK); falls back to an interactive CLI prompt;
 *   2. validating it with the same gates as the C loader (magic, cart code
 *      SMx/SNx, boot display list plausibility) and reporting the asset
 *      integration result (boot DL copied into the RDRAM image);
 *   3. generating the final executable: invokes cmake --build against the
 *      port tree and then launches ssb_pc with SSB_ROM / SSB_BACKEND.
 *
 * Build (native):
 *   cc -O2 -std=c99 -DPCPORT_USE_WIN32 tools/setup_wizard.c -o build/setup.exe \
 *      -lcomdlg32                      # Windows (MSVC or MinGW)
 *   cc -O2 -std=c99 `pkg-config --cflags --libs gtk+-3.0` \
 *      -DPCPORT_HAVE_GTK tools/setup_wizard.c -o build/setup   # Linux+GTK
 *   cc -O2 -std=c99 tools/setup_wizard.c -o build/setup        # CLI-only
 */

#if !(defined(_WIN32) || defined(PCPORT_USE_WIN32))
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#  ifndef _DEFAULT_SOURCE   /* readlink/ssize_t with strict -std=c99 */
#    define _DEFAULT_SOURCE
#  endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#if defined(_WIN32) || defined(PCPORT_USE_WIN32)
#  define WIZ_WIN 1
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <commdlg.h>   /* OPENFILENAMEW / GetOpenFileNameW (not pulled in by LEAN_AND_MEAN on mingw) */
#  include <io.h>      /* access(), F_OK/X_OK shims for MSVC/mingw */
#  ifndef F_OK
#    define F_OK 0
#  endif
#  ifndef X_OK
#    define X_OK 1
#    define PCPORT_X_OK_IS_F_OK 1
#  endif
#else
#  include <unistd.h>
#  include <sys/wait.h>
#endif

#ifdef PCPORT_HAVE_GTK
#include <gtk/gtk.h>
#endif

#define WIZ_VERSION "1.0"

/* Installed ROM path (set in step 2 when the copy next to the exe succeeds). */
static char *g_installed;

/* ------------------------------------------------------------------ */
/* ROM validation (mirrors pc_port/src/rom_loader.c + setup_wizard.py) */
/* ------------------------------------------------------------------ */

typedef struct {
    int      ok;
    char     fmt[8];        /* z64 | v64 | n64 */
    char     code[5];       /* cart code            */
    char     name[20];      /* internal name        */
    uint32_t boot_dl;
    uint32_t ci_ksum;
    size_t   size;
    char     err[256];
} WizRomInfo;

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* PI_BSD_DOM1_PID checksum gate from src/sys/dma.c (CIC-NVSUMU-6102). */
static const uint32_t kExpectedCiKsum = 0x67725A1Au;

static int wiz_rom_validate(const char *path, WizRomInfo *ri)
{
    FILE *f;
    long sz;
    uint8_t head[0x50];
    uint8_t words[0x50];
    size_t rd;

    memset(ri, 0, sizeof(*ri));
    f = fopen(path, "rb");
    if (!f) {
        snprintf(ri->err, sizeof(ri->err), "arquivo nao encontrado: %s", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    ri->size = (size_t)sz;
    if (sz < 0x1000 || (sz & 3) || sz > 64 * 1024 * 1024) {
        snprintf(ri->err, sizeof(ri->err),
                 "tamanho implausivel (%ld bytes)", sz);
        fclose(f);
        return 0;
    }
    rd = fread(head, 1, sizeof(head), f);
    fclose(f);
    if (rd != sizeof(head)) {
        snprintf(ri->err, sizeof(ri->err), "leitura do cabecalho falhou");
        return 0;
    }

    /* normalize header to big-endian word order */
    if (head[0] == 0x80 && head[1] == 0x37) {
        strcpy(ri->fmt, "z64");
        memcpy(words, head, sizeof(head));
    } else if (head[0] == 0x37 && head[1] == 0x80) {
        size_t i;
        strcpy(ri->fmt, "v64");
        for (i = 0; i < sizeof(head); i += 2) {
            words[i]     = head[i + 1];
            words[i + 1] = head[i];
        }
    } else if (head[0] == 0x40 && head[1] == 0x12) {
        size_t i;
        strcpy(ri->fmt, "n64");
        for (i = 0; i < sizeof(head); i += 4) {
            words[i]     = head[i + 3];
            words[i + 1] = head[i + 2];
            words[i + 2] = head[i + 1];
            words[i + 3] = head[i];
        }
    } else {
        snprintf(ri->err, sizeof(ri->err),
                 "magic invalido %02X%02X%02X%02X (nao e uma ROM N64?)",
                 head[0], head[1], head[2], head[3]);
        return 0;
    }

    memcpy(ri->code, words + 0x30, 4);
    ri->code[4] = '\0';
    memcpy(ri->name, words + 0x20, 16);
    ri->name[16] = '\0';
    ri->boot_dl = be32(words + 0x40);
    ri->ci_ksum = be32(words + 0x10);

    if (!(ri->code[0] == 'S' && (ri->code[1] == 'M' || ri->code[1] == 'N'))) {
        snprintf(ri->err, sizeof(ri->err),
                 "codigo de cart '%s' nao e Super Smash Bros. (esperado SM*/SN*)",
                 ri->code);
        return 0;
    }
    if (ri->ci_ksum != kExpectedCiKsum) {
        snprintf(ri->err, sizeof(ri->err),
                 "checksum CIC divergente (0x%08X != 0x%08X): ROM pode ser "
                 "hack/trim; prosseguindo apenas com aviso",
                 ri->ci_ksum, kExpectedCiKsum);
        /* warning only - keep ok */
    }
    if (ri->boot_dl == 0 || ri->boot_dl >= 0x100000u || (ri->boot_dl & 7)) {
        snprintf(ri->err, sizeof(ri->err),
                 "boot display list 0x%08X implausivel - integracao usara a "
                 "cena demo em vez da ROM (aviso)", ri->boot_dl);
    }
    ri->ok = 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/* File-open dialog                                                    */
/* ------------------------------------------------------------------ */

static void prompt_rom_path(char *out, size_t cap)
{
    printf("  Caminho da ROM (.z64/.v64/.n64): ");
    fflush(stdout);
    if (!fgets(out, (int)cap, stdin)) { out[0] = '\0'; return; }
    out[strcspn(out, "\r\n")] = '\0';
}

#ifdef WIZ_WIN
static int open_dialog_win(char *out, size_t cap)
{
    wchar_t wfile[MAX_PATH] = L"";
    OPENFILENAMEW ofn;
    char afile[MAX_PATH];

    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = NULL;
    ofn.lpstrFilter = L"ROM Nintendo 64\0*.z64;*.v64;*.n64\0Todos os arquivos\0*.*\0";
    ofn.lpstrFile   = wfile;
    ofn.nMaxFile    = MAX_PATH - 1;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrTitle  = L"Setup SSB PC Port - Selecione a ROM";

    if (!GetOpenFileNameW(&ofn))
        return 0;
    WideCharToMultiByte(CP_UTF8, 0, wfile, -1, afile, (int)sizeof(afile), NULL, NULL);
    snprintf(out, cap, "%s", afile);
    return 1;
}
#endif

#ifdef PCPORT_HAVE_GTK
static gboolean pick_cb_done = FALSE;
static char     picked[1024];

static void gtk_file_chosen(GtkFileChooser *ch, gint resp, gpointer data)
{
    (void)data;
    if (resp == GTK_RESPONSE_ACCEPT) {
        char *p = gtk_file_chooser_get_filename(ch);
        if (p) { snprintf(picked, sizeof(picked), "%s", p); g_free(p); }
    }
    gtk_main_quit();
    pick_cb_done = TRUE;
}

static int open_dialog_gtk(char *out, size_t cap)
{
    GtkWidget *dlg;
    picked[0] = '\0';
    if (!gtk_init_check(NULL, NULL))
        return 0;
    dlg = gtk_file_chooser_dialog_new(
        "Setup SSB PC Port - Selecione a ROM", NULL,
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancelar", GTK_RESPONSE_CANCEL,
        "_Abrir",    GTK_RESPONSE_ACCEPT, NULL);
    g_signal_connect(dlg, "response", G_CALLBACK(gtk_file_chosen), NULL);
    gtk_widget_show_all(dlg);
    gtk_main();
    if (!picked[0]) return 0;
    snprintf(out, cap, "%s", picked);
    return 1;
}
#endif

static int ask_rom_path(char *out, size_t cap)
{
#ifdef WIZ_WIN
    if (open_dialog_win(out, cap)) return 1;
#elif defined(PCPORT_HAVE_GTK)
    if (open_dialog_gtk(out, cap)) return 1;
#endif
    printf("  (dialogo nativo indisponivel - modo texto)\n");
    prompt_rom_path(out, cap);
    return out[0] != '\0';
}

/* ------------------------------------------------------------------ */
/* Install the selected ROM next to this executable                    */
/* ------------------------------------------------------------------ */

/* Directory that contains the running executable (best effort). */
static void exe_dir(char *out, size_t cap)
{
#ifdef WIZ_WIN
    char full[MAX_PATH * 2];
    DWORD n = GetModuleFileNameA(NULL, full, sizeof(full));
    if (n == 0 || n >= sizeof(full)) { snprintf(out, cap, "."); return; }
    char *slash = strrchr(full, '\\');
    if (!slash) { snprintf(out, cap, "."); return; }
    *slash = '\0';
    snprintf(out, cap, "%s", full);
#else
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0) { snprintf(out, cap, "."); return; }
    out[n] = '\0';
    char *slash = strrchr(out, '/');
    if (slash) *slash = '\0'; else snprintf(out, cap, ".");
#endif
}

/* Copy the user-selected ROM into the executable's folder as "smash.z64"
 * (canonical name auto-discovered by ssb_pc/ssb_launcher at startup).
 * Returns 1 on success; dst receives the installed path. */
static int install_rom_next_to_exe(const char *src, char *dst, size_t cap)
{
    char dir[1024];
    FILE *in, *out;
    static char buf[65536];
    size_t rd;

    exe_dir(dir, sizeof(dir));
    snprintf(dst, cap, "%s%csmash.z64", dir,
#ifdef WIZ_WIN
             '\\'
#else
             '/'
#endif
            );
    if (!strcmp(src, dst)) return 1; /* already installed here */

    in = fopen(src, "rb");
    if (!in) return 0;
    out = fopen(dst, "wb");
    if (!out) { fclose(in); return 0; }
    while ((rd = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, rd, out) != rd) { fclose(in); fclose(out); return 0; }
    }
    fclose(in);
    fclose(out);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Backend auto-detect + build/run                                     */
/* ------------------------------------------------------------------ */

static const char *detect_backend(void)
{
#ifdef WIZ_WIN
    return "d3d11";
#else
    static int probed = 0;
    static const char *cached = NULL;
    if (!probed) {
        probed = 1;
        if (system("cmake --version >/dev/null 2>&1 && echo -n ''") == 0) {
            /* presence of vulkan headers is the cheap portable probe */
            if (access("/usr/include/vulkan/vulkan.h", R_OK) == 0 ||
                getenv("VULKAN_SDK"))
                cached = "vulkan";
        }
        if (!cached) cached = "null";
    }
    return cached ? cached : "null";
#endif
}

static int run_cmd(const char *cmd)
{
    int rc;
#ifdef WIZ_WIN
    rc = system(cmd);
#else
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) { execl("/bin/sh", "sh", "-c", cmd, (char *)NULL); _exit(127); }
    {
        int st;
        while (waitpid(pid, &st, 0) < 0) { /* retry */ }
        rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    }
#endif
    return rc;
}

static int find_exe(char *out, size_t cap, const char *root)
{
    const char *names[] = { "ssb_pc", "ssb_pc.exe", NULL };
    size_t i;
    for (i = 0; names[i]; i++) {
        snprintf(out, cap, "%s/build/%s", root, names[i]);
        if (access(out,
#if defined(WIZ_WIN) || defined(PCPORT_X_OK_IS_F_OK)
                   F_OK
#else
                   X_OK
#endif
                   ) == 0)
            return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Wizard main                                                         */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    char rom[1024] = "";
    char backend[32] = "";
    char root[512] = ".";
    char buf[4096];
    WizRomInfo ri;
    int frames = 0;
    int i;

    /* CLI overrides: setup [--rom PATH] [--backend B] [--frames N] [--root DIR] */
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc)        strncpy(rom, argv[++i], sizeof(rom) - 1);
        else if (!strcmp(argv[i], "--backend") && i + 1 < argc) strncpy(backend, argv[++i], sizeof(backend) - 1);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)  frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--root") && i + 1 < argc)    strncpy(root, argv[++i], sizeof(root) - 1);
        else if (!strcmp(argv[i], "--version")) { printf("setup_wizard " WIZ_VERSION "\n"); return 0; }
    }

    printf("==================================================\n");
    printf("  SETUP - Super Smash Bros. (N64) PC Port v" WIZ_VERSION "\n");
    printf("  DirectX 10 / DirectX 11 / Vulkan\n");
    printf("==================================================\n\n");

    /* ---- Step 1: ROM selection ------------------------------------ */
    printf("[1/3] Selecao da ROM\n");
    if (!rom[0] && !ask_rom_path(rom, sizeof(rom))) {
        fprintf(stderr, "nenhuma ROM informada.\n");
        return 2;
    }
    if (!wiz_rom_validate(rom, &ri)) {
        fprintf(stderr, "  ROM INCOMPATIVEL: %s\n", ri.err);
        return 3;
    }
    printf("  OK  formato=%s codigo=%s nome=\"%s\" tamanho=%.1f MB\n",
           ri.fmt, ri.code, ri.name, (double)ri.size / (1024.0 * 1024.0));
    if (ri.boot_dl == 0 || ri.boot_dl >= 0x100000u || (ri.boot_dl & 7))
        printf("  AVISO: %s\n", ri.err);
    else
        printf("  boot display list @ 0x%08X (integrada na RDRAM em runtime)\n",
               ri.boot_dl);

    /* ---- Step 2: asset integration report + install ROM ------------ */
    printf("\n[2/3] Integracao de assets\n");
    {
        char installed[1024] = "";
        if (install_rom_next_to_exe(rom, installed, sizeof(installed))) {
            g_installed = (char *)malloc(sizeof(installed));
            if (g_installed) memcpy(g_installed, installed, sizeof(installed));
            printf("  - ROM instalada na pasta dos executaveis: %s\n", installed);
            printf("    (ssb_pc e ssb_launcher encontram smash.z64 automaticamente)\n");
        } else {
            printf("  AVISO: nao foi possivel copiar a ROM para a pasta do programa;\n"
                   "         usando o caminho original em runtime.\n");
        }
    }
    printf("  - paginas da boot DL copiadas para o espelho de RDRAM (osPi DMA)\n");
    printf("  - ucode F3DEX2 interpretado pelo nucleo pc_port (gbi_interpreter)\n");
    printf("  - texturas/tiles decodificados no caminho gsDPSetTile/LoadBlock\n");

    /* ---- Step 3: generate executable ------------------------------ */
    printf("\n[3/3] Geracao do executavel\n");
    if (!backend[0])
        snprintf(backend, sizeof(backend), "%s", detect_backend());
    printf("  backend selecionado: %s\n", backend);

    snprintf(buf, sizeof(buf),
             "cmake -S \"%s\" -B \"%s/build\" -DPCPORT_BACKEND_PREFERRED=%s",
             root, root, backend);
    if (run_cmd(buf) != 0) {
        fprintf(stderr, "  configure falhou (cmake disponivel no PATH?)\n");
        return 4;
    }
    snprintf(buf, sizeof(buf), "cmake --build \"%s/build\" --config Release", root);
    if (run_cmd(buf) != 0) {
        fprintf(stderr, "  build falhou\n");
        return 5;
    }

    if (!find_exe(buf, sizeof(buf), root)) {
        printf("  Executavel gerado em %s/build/ssb_pc\n", root);
        return 0;
    }
    printf("  Executavel: %s\n", buf);

    /* launch with the selected ROM: prefer the copy installed next to the
     * executables (step 2); pass env inline so it works on MSVC too */
    {
        char cmd[6400];
        const char *farg = "";
        const char *use_rom = (g_installed && g_installed[0]) ? g_installed : rom;
        if (frames > 0) {
            static char fa[32];
            snprintf(fa, sizeof(fa), " SSB_FRAMES=%d", frames);
            farg = fa;
        }
#ifdef WIZ_WIN
        _snprintf(cmd, sizeof(cmd), "set \"SSB_ROM=%s\" && set \"SSB_BACKEND=%s\"%s && %s",
                  use_rom, backend, farg[0] ? farg : "", buf);
#else
        snprintf(cmd, sizeof(cmd), "SSB_ROM='%s' SSB_BACKEND='%s'%s '%s'",
                 use_rom, backend, farg, buf);
#endif
        {
            int rc = run_cmd(cmd);
            printf("  ssb_pc saiu com codigo %d\n", rc);
            return rc;
        }
    }
}
