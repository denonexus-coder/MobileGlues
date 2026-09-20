#include "gl/shader_classifier.h"
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <dirent.h>

static char* read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* data = (char*)malloc(size + 1);
    fread(data, 1, size, f);
    data[size] = '\0';
    fclose(f);
    return data;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "Usage: %s <dir_essl>\n", argv[0]); return 1; }
    
    DIR* d = opendir(argv[1]);
    if (!d) return 1;
    
    struct dirent* ent;
    int total = 0, blacklisted = 0;
    
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        size_t len = strlen(ent->d_name);
        if (len < 5 || strcmp(ent->d_name + len - 5, ".essl") != 0) continue;
        
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", argv[1], ent->d_name);
        
        char* essl = read_file(path);
        if (!essl) continue;
        
        total++;
        std::string s(essl);
        if (MG::IsTextOrGuiShader(s)) {
            blacklisted++;
            printf("BLACKLIST | %s\n", ent->d_name);
        }
        free(essl);
    }
    closedir(d);
    
    printf("\n===== RESULTADO =====\n");
    printf("Total:       %d\n", total);
    printf("Blacklisted: %d (%.0f%%)\n", blacklisted, 100.0 * blacklisted / total);
    printf("Usam bin:    %d\n", total - blacklisted);
    return 0;
}
