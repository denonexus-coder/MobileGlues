#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PATH 512
#define MAX_FILE 262144
#define MAX_INCLUDES 32

static const char* INCLUDE_DIR = "/storage/emulated/0/MG/shaders/include";

typedef struct { char name[128]; char* content; } Include;
static Include includes[MAX_INCLUDES];
static int num_includes = 0;

static char* read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* data = malloc(size + 1);
    fread(data, 1, size, f);
    data[size] = '\0';
    fclose(f);
    return data;
}

// Remove TODAS as linhas #version de um bloco
static char* strip_all_versions(const char* content) {
    char* out = malloc(strlen(content) + 1);
    size_t out_pos = 0;
    const char* p = content;
    while (*p) {
        if (*p == '#' && strncmp(p, "#version", 8) == 0) {
            const char* eol = strchr(p, '\n');
            if (!eol) eol = p + strlen(p);
            p = (*eol == '\n') ? eol + 1 : eol;
            continue;
        }
        out[out_pos++] = *p;
        p++;
    }
    out[out_pos] = '\0';
    return out;
}

static const char* get_include(const char* name) {
    for (int i = 0; i < num_includes; i++)
        if (strcmp(includes[i].name, name) == 0) return includes[i].content;
    if (num_includes >= MAX_INCLUDES) return NULL;

    char path[MAX_PATH];
    snprintf(path, MAX_PATH, "%s/%s.glsl", INCLUDE_DIR, name);
    char* content = read_file(path);
    if (!content) return NULL;

    // Remove TODOS os #version dos includes
    char* cleaned = strip_all_versions(content);
    free(content);

    strncpy(includes[num_includes].name, name, sizeof(includes[0].name) - 1);
    includes[num_includes].content = cleaned;
    return includes[num_includes++].content;
}

static char* resolve_moj_imports(const char* source) {
    char* out = malloc(MAX_FILE);
    size_t out_pos = 0;
    const char* p = source;
    int first_version_kept = 0;

    while (*p) {
        // Para o MAIN source, mantém só o primeiro #version
        if (*p == '#' && strncmp(p, "#version", 8) == 0) {
            const char* eol = strchr(p, '\n');
            if (!eol) eol = p + strlen(p);

            if (first_version_kept) {
                p = (*eol == '\n') ? eol + 1 : eol;
                continue;
            }
            first_version_kept = 1;
        }

        if (*p == '#' && strncmp(p, "#moj_import", 11) == 0) {
            const char* lt = strchr(p, '<');
            const char* gt = lt ? strchr(lt, '>') : NULL;
            if (lt && gt && gt > lt) {
                char name[128] = {0};
                size_t nlen = gt - lt - 1;
                if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
                memcpy(name, lt + 1, nlen);

                const char* clean = name;
                if (strncmp(name, "minecraft:", 10) == 0) clean = name + 10;

                char base[128];
                strncpy(base, clean, sizeof(base) - 1);
                char* dot = strstr(base, ".glsl");
                if (dot) *dot = '\0';

                fprintf(stderr, "  [import] %s\n", base);

                const char* content = get_include(base);
                if (content) {
                    size_t clen = strlen(content);
                    if (out_pos + clen < MAX_FILE - 1) {
                        memcpy(out + out_pos, content, clen);
                        out_pos += clen;
                    }
                }

                p = gt + 1;
                while (*p && *p != '\n') p++;
                if (*p == '\n') p++;
                continue;
            }
        }

        if (out_pos < MAX_FILE - 1) out[out_pos++] = *p;
        p++;
    }
    out[out_pos] = '\0';
    return out;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "Usage: %s <in> <out>\n", argv[0]); return 1; }
    char* src = read_file(argv[1]);
    if (!src) { fprintf(stderr, "erro lendo\n"); return 1; }
    fprintf(stderr, "=== %s ===\n", argv[1]);
    char* res = resolve_moj_imports(src);
    FILE* out = fopen(argv[2], "wb");
    fwrite(res, 1, strlen(res), out);
    fclose(out);
    fprintf(stderr, "  %zu bytes\n", strlen(res));
    free(src); free(res);
    return 0;
}
