// mg_compile_cli.cpp — CLI para converter shaders do Bedrock
// Compila com: clang++ mg_compile_cli.cpp \
//    gl/glsl/glsl_for_es.cpp gl/glsl/cache.cpp config/settings.cpp \
//    config/config.cpp config/cJSON.c \
//    -o mg_compile

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <GL/gl.h>

#include "gl/glsl/glsl_for_es.h"
#include "config/settings.h"

extern "C" {
    // Stubs que o glsl_for_es.cpp precisa (normalmente providos por outros módulos)
    // Adapta pro que o seu build espera
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <input.shader> <vertex|fragment> <output.essl>\n", argv[0]);
        return 1;
    }
    
    // 1. Lê source
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror("fopen"); return 1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* source = (char*)malloc(size + 1);
    fread(source, 1, size, f);
    source[size] = '\0';
    fclose(f);
    
    // 2. Determina tipo
    GLenum type = (strcmp(argv[2], "vertex") == 0) ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER;
    
    // 3. Detecta versão do GLSL
    int glsl_ver = getGLSLVersion(source);
    printf("GLSL version detectada: %d\n", glsl_ver);
    
    // 4. Chama a conversão
    int return_code = 0;
    std::string essl = GLSLtoGLSLES(source, type, 300, glsl_ver, return_code);
    
    printf("Return code: %d\n", return_code);
    printf("ESSL size: %zu bytes\n", essl.size());
    
    // 5. Salva
    FILE* out = fopen(argv[3], "wb");
    if (!out) { perror("fopen out"); return 1; }
    fwrite(essl.data(), 1, essl.size(), out);
    fclose(out);
    
    printf("Salvo em %s\n", argv[3]);
    free(source);
    return 0;
}
