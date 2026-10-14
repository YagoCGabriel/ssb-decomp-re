/* pc_config.c — see include/pc_config.h for the precedence rules. */
#include "pc_config.h"
#include "pc_port_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static PcConfig g_cfg;
static int      g_loaded;

static void trim(char *s) {
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

static void strip_quotes(char *s) {
    size_t n = strlen(s);
    if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') || (s[0] == '\'' && s[n - 1] == '\''))) {
        s[n - 1] = '\0';
        memmove(s, s + 1, n - 1);
    }
}

static void set_str(char *dst, size_t cap, const char *v) {
    snprintf(dst, cap, "%s", v);
}

/* Apply one key=value pair from any source (ini/env/argv). */
static void apply(PcConfig *c, const char *key, const char *val) {
    if (!key || !val) return;
    if      (!strcmp(key, "rom"))              set_str(c->rom, sizeof c->rom, val);
    else if (!strcmp(key, "backend"))          set_str(c->backend, sizeof c->backend, val);
    else if (!strcmp(key, "frames"))           c->frames = atoi(val);
    else if (!strcmp(key, "headless"))         c->headless = atoi(val);
    else if (!strcmp(key, "width"))            c->width = atoi(val);
    else if (!strcmp(key, "height"))           c->height = atoi(val);
    else if (!strcmp(key, "fullscreen"))       c->fullscreen = atoi(val);
    else if (!strcmp(key, "vsync"))            c->vsync = atoi(val);
    else if (!strcmp(key, "max_fps"))          c->max_fps = atoi(val);
    else if (!strcmp(key, "resolution_scale")) c->resolution_scale = (float)atof(val);
    else if (!strcmp(key, "internal_width"))   c->internal_width = atoi(val);
    else if (!strcmp(key, "internal_height"))  c->internal_height = atoi(val);
    else if (!strcmp(key, "aspect"))           c->aspect = (float)atof(val);
    else if (!strcmp(key, "integer_scale"))    c->integer_scale = atoi(val);
    else if (!strcmp(key, "cull_mode"))        c->cull_mode = atoi(val);
    else if (!strcmp(key, "depth_func"))       c->depth_func = atoi(val);
    else if (!strcmp(key, "wireframe"))        c->wireframe = atoi(val);
    else if (!strcmp(key, "clear_color"))      c->clear_color = (uint32_t)strtoul(val, NULL, 0);
    else if (!strcmp(key, "anisotropy"))       c->anisotropy = atoi(val);
    else if (!strcmp(key, "msaa"))             c->msaa = atoi(val);
    else if (!strcmp(key, "log_level"))        c->log_level = atoi(val);
    else if (!strcmp(key, "sim_buttons"))      c->sim_buttons = (uint32_t)strtoul(val, NULL, 0);
    else if (!strcmp(key, "dump_dir"))         set_str(c->dump_dir, sizeof c->dump_dir, val);
    /* unknown keys are silently ignored so newer launchers stay compatible */
}

void pcConfigDefaults(PcConfig *c) {
    memset(c, 0, sizeof *c);
    set_str(c->backend, sizeof c->backend, "auto");
    c->frames = 0;
    c->headless = 0;
    c->width = 1280;
    c->height = 720;
    c->fullscreen = 0;
    c->vsync = 1;
    c->max_fps = 0;
    c->resolution_scale = 1.0f;
    c->internal_width = 0;
    c->internal_height = 0;
    c->aspect = 4.0f / 3.0f;
    c->integer_scale = 0;
    c->cull_mode = 2;
    c->depth_func = 2;
    c->wireframe = 0;
    c->clear_color = 0x101418FFu;
    c->anisotropy = 1;
    c->msaa = 0;
    c->log_level = 2;
    c->sim_buttons = 0;
    c->dump_dir[0] = '\0';
}

PcConfig *pcConfig(void) {
    if (!g_loaded) {
        pcConfigDefaults(&g_cfg);
        g_loaded = 1;
    }
    return &g_cfg;
}

static int load_ini(PcConfig *c, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = line, *v = eq + 1;
        trim(k); trim(v); strip_quotes(v);
        for (char *p = k; *p; p++) *p = (char)tolower((unsigned char)*p);
        apply(c, k, v);
    }
    fclose(f);
    return 0;
}

static void load_env(PcConfig *c) {
    static const char *keys[] = {
        "ROM", "BACKEND", "FRAMES", "HEADLESS", "WIDTH", "HEIGHT", "FULLSCREEN",
        "VSYNC", "MAX_FPS", "RESOLUTION_SCALE", "INTERNAL_WIDTH", "INTERNAL_HEIGHT",
        "ASPECT", "INTEGER_SCALE", "CULL_MODE", "DEPTH_FUNC", "WIREFRAME",
        "CLEAR_COLOR", "ANISOTROPY", "MSAA", "LOG_LEVEL", "SIM_BTN", "DUMP_DIR",
    };
    char name[64];
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        snprintf(name, sizeof name, "SSB_%s", keys[i]);
        const char *v = getenv(name);
        if (v) {
            /* legacy env names map onto canonical keys */
            const char *k = keys[i];
            if (!strcmp(k, "SIM_BTN")) k = "sim_buttons";
            apply(c, k, v);
        }
    }
}

int pcConfigInit(int argc, char **argv) {
    PcConfig *c = pcConfig();
    pcConfigDefaults(c);
    g_loaded = 1;

    /* pass 1: config file (--config wins over SSB_CONFIG wins over ssb_config.ini) */
    const char *cfgpath = NULL;
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--config")) cfgpath = argv[i + 1];
    if (!cfgpath) cfgpath = getenv("SSB_CONFIG");
    if (cfgpath) {
        if (load_ini(c, cfgpath) != 0)
            pclog("config: file '%s' not found, using defaults", cfgpath);
        else
            pclog("config: loaded '%s'", cfgpath);
    } else {
        load_ini(c, "ssb_config.ini"); /* optional, silent */
    }

    /* pass 2: environment overrides */
    load_env(c);

    /* pass 3: command line (highest precedence) */
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) return 1;
        if (a[0] != '-' || a[1] != '-') continue;
        const char *key = a + 2;
        const char *val = "";
        char buf[512];
        const char *eq = strchr(key, '=');
        if (eq) { /* --key=value form */
            size_t n = (size_t)(eq - key);
            if (n >= sizeof buf) n = sizeof buf - 1;
            memcpy(buf, key, n); buf[n] = '\0';
            key = buf;
            val = eq + 1;
        } else if (i + 1 < argc && argv[i + 1][0] != '-') {
            val = argv[++i];
        } else { /* boolean flag with no value */
            val = "1";
        }
        char lk[64];
        size_t j = 0;
        for (; key[j] && j < sizeof lk - 1; j++) lk[j] = (char)tolower((unsigned char)key[j]);
        lk[j] = '\0';
        apply(c, lk, val);
    }
    return 0;
}

int pcConfigSave(const char *path) {
    PcConfig *c = pcConfig();
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "# ssb_pc configuration (generated by launcher)\n");
    fprintf(f, "rom = %s\n", c->rom);
    fprintf(f, "backend = %s\n", c->backend);
    fprintf(f, "frames = %d\n", c->frames);
    fprintf(f, "headless = %d\n", c->headless);
    fprintf(f, "width = %d\n", c->width);
    fprintf(f, "height = %d\n", c->height);
    fprintf(f, "fullscreen = %d\n", c->fullscreen);
    fprintf(f, "vsync = %d\n", c->vsync);
    fprintf(f, "max_fps = %d\n", c->max_fps);
    fprintf(f, "resolution_scale = %.3f\n", (double)c->resolution_scale);
    fprintf(f, "internal_width = %d\n", c->internal_width);
    fprintf(f, "internal_height = %d\n", c->internal_height);
    fprintf(f, "aspect = %.4f\n", (double)c->aspect);
    fprintf(f, "integer_scale = %d\n", c->integer_scale);
    fprintf(f, "cull_mode = %d\n", c->cull_mode);
    fprintf(f, "depth_func = %d\n", c->depth_func);
    fprintf(f, "wireframe = %d\n", c->wireframe);
    fprintf(f, "clear_color = 0x%08X\n", c->clear_color);
    fprintf(f, "anisotropy = %d\n", c->anisotropy);
    fprintf(f, "msaa = %d\n", c->msaa);
    fprintf(f, "log_level = %d\n", c->log_level);
    fprintf(f, "sim_buttons = 0x%X\n", c->sim_buttons);
    fprintf(f, "dump_dir = %s\n", c->dump_dir);
    fclose(f);
    return 0;
}

void pcConfigLog(const PcConfig *c) {
    pclog("config: backend=%s rom='%s' frames=%d headless=%d",
          c->backend, c->rom[0] ? c->rom : "(demo)", c->frames, c->headless);
    pclog("config: window=%dx%d fs=%d vsync=%d maxfps=%d scale=%.2f aspect=%.3f iscale=%d",
          c->width, c->height, c->fullscreen, c->vsync, c->max_fps,
          (double)c->resolution_scale, (double)c->aspect, c->integer_scale);
    pclog("config: cull=%d depth=%d wire=%d clear=0x%08X aniso=%d msaa=%d log=%d",
          c->cull_mode, c->depth_func, c->wireframe, c->clear_color,
          c->anisotropy, c->msaa, c->log_level);
}

void pcConfigFramebufferSize(const PcConfig *c, int *w, int *h) {
    int fw = c->internal_width  > 0 ? c->internal_width  : (int)(c->width  * c->resolution_scale);
    int fh = c->internal_height > 0 ? c->internal_height : (int)(c->height * c->resolution_scale);
    if (fw < 16) fw = 16;
    if (fh < 16) fh = 16;
    if (c->integer_scale) {
        /* snap to integer multiples of the N64 VI base resolution */
        int sx = fw / 320, sy = fh / 240;
        if (sx < 1) sx = 1;
        if (sy < 1) sy = 1;
        fw = sx * 320;
        fh = sy * 240;
    }
    *w = fw;
    *h = fh;
}
