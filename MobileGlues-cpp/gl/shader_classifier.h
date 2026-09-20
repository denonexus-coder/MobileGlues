// MobileGlues - gl/shader_classifier.h
// Classifica shaders que NÃO devem usar cache binário na PowerVR.
#pragma once

#include <string>
#include <cstring>

namespace MG {

inline bool IsTextOrGuiShader(const std::string& essl_source) {
    if (essl_source.find("ColorModulator") == std::string::npos) {
        return false;
    }
    
    const bool has_sampler0 = essl_source.find("Sampler0") != std::string::npos;
    const bool has_sampler2 = essl_source.find("Sampler2") != std::string::npos;
    const bool has_uv2      = essl_source.find("ivec2 UV2") != std::string::npos;
    const bool has_normal   = essl_source.find("vec3 Normal") != std::string::npos;
    
    // Texto FS: usa fonte, sem lightmap UV2, sem Normal
    if (has_sampler0 && !has_uv2 && !has_normal) return true;
    
    // Texto VS: usa lightmap, sem Normal, sem Sampler0
    if (has_sampler2 && has_uv2 && !has_normal && !has_sampler0) return true;
    
    // GUI: sem texturas
    if (!has_sampler0 && !has_sampler2 && !has_uv2 && !has_normal) return true;
    
    return false;
}

inline bool IsTextOrGuiShaderByName(const char* shader_name) {
    if (!shader_name) return false;
    static const char* patterns[] = {
        "rendertype_text", "gui", "position_color",
        "position_tex_color", "animate_sprite", "blit_screen",
        "screenquad", nullptr
    };
    for (int i = 0; patterns[i]; i++) {
        if (strstr(shader_name, patterns[i])) return true;
    }
    return false;
}

} // namespace MG
