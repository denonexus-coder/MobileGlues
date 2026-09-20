// MobileGlues - gl/glsl/program_binary_cache.h
//
// Persistent cache of GL program binaries.
// Controlled by `global_settings.use_program_binary_cache`.
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <GL/gl.h>

namespace MG {

class ProgramBinaryCache {
public:
    static ProgramBinaryCache& get_instance();
    
    static std::array<uint8_t, 32> compute_key(
        const std::string& vs_essl,
        const std::string& fs_essl,
        const std::vector<std::pair<GLuint, std::string>>& attrib_bindings);
    
    bool load(GLuint program, const std::array<uint8_t, 32>& key);
    void save(GLuint program, const std::array<uint8_t, 32>& key);
    
    // Runtime control
    void set_enabled(bool en) { enabled_ = en; }
    bool is_enabled() const { return enabled_; }
    
    void set_cache_dir(const std::string& dir);
    
    // Auto-detect GPU and adjust policy
    void detect_and_configure(const char* gpu_renderer);
    
    struct Stats {
        uint64_t hits = 0;
        uint64_t misses = 0;
        uint64_t saves = 0;
        uint64_t failures = 0;
        uint64_t skipped_text_gui = 0;
    } stats;
    
private:
    ProgramBinaryCache() = default;
    bool enabled_ = false;   // OFF by default, enabled by config
    std::string cache_dir_;
    std::string to_hex(const std::array<uint8_t, 32>& h);
    std::string bin_path_for(const std::array<uint8_t, 32>& key);
    std::string meta_path_for(const std::array<uint8_t, 32>& key);
};

} // namespace MG
