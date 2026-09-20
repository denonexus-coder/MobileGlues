// MobileGlues - gl/glsl/program_binary_cache.cpp
#include "program_binary_cache.h"
#include "../shader_classifier.h"
#include "../log.h"
#include "../../gles/loader.h"
#include "../../includes.h"
#include "../mg.h"
#include "../../config/settings.h"
#include <GL/gl.h>

#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <algorithm>

namespace MG {

// ============================================================================
// SHA-256
// ============================================================================
namespace {

struct SHA256_CTX {
    uint32_t data[8];
    uint64_t datalen;
    uint8_t  block[64];
    size_t   blocklen;
};

static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define ROTR(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define CH(x,y,z) (((x)&(y))^(~(x)&(z)))
#define MAJ(x,y,z) (((x)&(y))^((x)&(z))^((y)&(z)))
#define EP0(x) (ROTR(x,2)^ROTR(x,13)^ROTR(x,22))
#define EP1(x) (ROTR(x,6)^ROTR(x,11)^ROTR(x,25))
#define SIG0(x) (ROTR(x,7)^ROTR(x,18)^((x)>>3))
#define SIG1(x) (ROTR(x,17)^ROTR(x,19)^((x)>>10))

static void sha256_transform(SHA256_CTX* ctx, const uint8_t* d) {
    uint32_t a,b,c,dd,e,f,g,h,i,j,t1,t2,m[64];
    for(i=0,j=0;i<16;i++,j+=4)
        m[i]=(uint32_t)d[j]<<24|(uint32_t)d[j+1]<<16|(uint32_t)d[j+2]<<8|d[j+3];
    for(;i<64;i++) m[i]=SIG1(m[i-2])+m[i-7]+SIG0(m[i-15])+m[i-16];
    a=ctx->data[0];b=ctx->data[1];c=ctx->data[2];dd=ctx->data[3];
    e=ctx->data[4];f=ctx->data[5];g=ctx->data[6];h=ctx->data[7];
    for(i=0;i<64;i++){
        t1=h+EP1(e)+CH(e,f,g)+K[i]+m[i];
        t2=EP0(a)+MAJ(a,b,c);
        h=g;g=f;f=e;e=dd+t1;dd=c;c=b;b=a;a=t1+t2;
    }
    ctx->data[0]+=a;ctx->data[1]+=b;ctx->data[2]+=c;ctx->data[3]+=dd;
    ctx->data[4]+=e;ctx->data[5]+=f;ctx->data[6]+=g;ctx->data[7]+=h;
}

static void sha256_init(SHA256_CTX* ctx) {
    ctx->datalen=0;ctx->blocklen=0;
    ctx->data[0]=0x6a09e667;ctx->data[1]=0xbb67ae85;ctx->data[2]=0x3c6ef372;ctx->data[3]=0xa54ff53a;
    ctx->data[4]=0x510e527f;ctx->data[5]=0x9b05688c;ctx->data[6]=0x1f83d9ab;ctx->data[7]=0x5be0cd19;
}

static void sha256_update(SHA256_CTX* ctx, const uint8_t* d, size_t len) {
    for(size_t i=0;i<len;i++){
        ctx->block[ctx->blocklen++]=d[i];
        if(ctx->blocklen==64){
            sha256_transform(ctx,ctx->block);
            ctx->datalen+=512;ctx->blocklen=0;
        }
    }
}

static void sha256_final(SHA256_CTX* ctx, uint8_t hash[32]) {
    uint32_t i=ctx->blocklen;
    if(ctx->blocklen<56){
        ctx->block[i++]=0x80; while(i<56)ctx->block[i++]=0x00;
    } else {
        ctx->block[i++]=0x80; while(i<64)ctx->block[i++]=0x00;
        sha256_transform(ctx,ctx->block);
        memset(ctx->block,0,56);
    }
    ctx->datalen+=ctx->blocklen*8;
    ctx->block[63]=ctx->datalen;ctx->block[62]=ctx->datalen>>8;
    ctx->block[61]=ctx->datalen>>16;ctx->block[60]=ctx->datalen>>24;
    ctx->block[59]=ctx->datalen>>32;ctx->block[58]=ctx->datalen>>40;
    ctx->block[57]=ctx->datalen>>48;ctx->block[56]=ctx->datalen>>56;
    sha256_transform(ctx,ctx->block);
    for(i=0;i<4;i++)for(uint32_t j=0;j<8;j++)
        hash[i+4*j]=(ctx->data[j]>>(24-i*8))&0xff;
}
} // namespace

// ============================================================================
// ProgramBinaryCache
// ============================================================================

ProgramBinaryCache& ProgramBinaryCache::get_instance() {
    static ProgramBinaryCache inst;
    return inst;
}

std::array<uint8_t, 32> ProgramBinaryCache::compute_key(
    const std::string& vs_essl,
    const std::string& fs_essl,
    const std::vector<std::pair<GLuint, std::string>>& attrib_bindings)
{
    SHA256_CTX ctx; sha256_init(&ctx);
    const char* tag = "\n//MG_PROGRAM_BIN_v1\n";
    sha256_update(&ctx, (const uint8_t*)tag, strlen(tag));
    sha256_update(&ctx, (const uint8_t*)vs_essl.data(), vs_essl.size());
    sha256_update(&ctx, (const uint8_t*)"\n|\n", 3);
    sha256_update(&ctx, (const uint8_t*)fs_essl.data(), fs_essl.size());
    sha256_update(&ctx, (const uint8_t*)"\n|\n", 3);
    
    auto sorted = attrib_bindings;
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b){ return a.first < b.first; });
    for (const auto& p : sorted) {
        char buf[256];
        int n = snprintf(buf, sizeof(buf), "%u=%s;", p.first, p.second.c_str());
        sha256_update(&ctx, (const uint8_t*)buf, n);
    }
    std::array<uint8_t, 32> out{}; sha256_final(&ctx, out.data()); return out;
}

std::string ProgramBinaryCache::to_hex(const std::array<uint8_t, 32>& h) {
    static const char* hex = "0123456789abcdef";
    std::string s; s.reserve(64);
    for (uint8_t b : h) { s.push_back(hex[b >> 4]); s.push_back(hex[b & 0xF]); }
    return s;
}

std::string ProgramBinaryCache::bin_path_for(const std::array<uint8_t, 32>& key) {
    return cache_dir_ + "/" + to_hex(key) + ".bin";
}
std::string ProgramBinaryCache::meta_path_for(const std::array<uint8_t, 32>& key) {
    return cache_dir_ + "/" + to_hex(key) + ".meta";
}

void ProgramBinaryCache::set_cache_dir(const std::string& dir) {
    cache_dir_ = dir;
    mkdir(dir.c_str(), 0755);
}

void ProgramBinaryCache::detect_and_configure(const char* gpu_renderer) {
    if (!gpu_renderer) return;
    // PowerVR: driver may reject binaries between sessions.
    if (strstr(gpu_renderer, "PowerVR")) {
        LOG_D("ProgramBinaryCache: PowerVR detected. Binary cache will be filtered for text/GUI.");
    }
}

bool ProgramBinaryCache::load(GLuint program, const std::array<uint8_t, 32>& key) {
    if (!enabled_ || cache_dir_.empty() || !GLES.glProgramBinary || !GLES.glGetProgramiv) return false;
    
    std::string bp = bin_path_for(key);
    std::string mp = meta_path_for(key);
    
    FILE* fm = fopen(mp.c_str(), "rb");
    if (!fm) { stats.misses++; return false; }
    uint32_t meta[3] = {0};
    if (fread(meta, 4, 3, fm) != 3) { fclose(fm); stats.misses++; return false; }
    fclose(fm);
    
    GLenum format = meta[0];
    uint32_t size = meta[1];
    if (size == 0 || size > 16*1024*1024) { stats.misses++; return false; }
    
    FILE* fb = fopen(bp.c_str(), "rb");
    if (!fb) { stats.misses++; return false; }
    std::vector<uint8_t> data(size);
    if (fread(data.data(), 1, size, fb) != size) { fclose(fb); stats.misses++; return false; }
    fclose(fb);
    
    GLES.glProgramBinary(program, format, data.data(), size);
    GLint link_ok = 0;
    GLES.glGetProgramiv(program, GL_LINK_STATUS, &link_ok);
    
    if (!link_ok) {
        LOG_D("ProgramBinaryCache: driver rejected binary (key=%s)", to_hex(key).c_str());
        stats.failures++;
        return false;
    }
    stats.hits++;
    LOG_D("ProgramBinaryCache: HIT key=%s", to_hex(key).c_str());
    return true;
}

void ProgramBinaryCache::save(GLuint program, const std::array<uint8_t, 32>& key) {
    if (!enabled_ || cache_dir_.empty() || !GLES.glGetProgramBinary || !GLES.glGetProgramiv) return;
    
    std::string bp = bin_path_for(key);
    struct stat st;
    if (stat(bp.c_str(), &st) == 0 && st.st_size > 0) return;
    
    GLint bl = 0;
    GLES.glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &bl);
    if (bl <= 0 || bl > 16*1024*1024) return;
    
    std::vector<uint8_t> data(bl);
    GLenum fmt = 0; GLsizei written = 0;
    GLES.glGetProgramBinary(program, bl, &written, &fmt, data.data());
    if (written <= 0 || fmt == 0) return;
    
    FILE* fb = fopen(bp.c_str(), "wb");
    if (!fb) return;
    fwrite(data.data(), 1, written, fb);
    fclose(fb);
    
    FILE* fm = fopen(meta_path_for(key).c_str(), "wb");
    if (!fm) return;
    uint32_t meta[3] = { (uint32_t)fmt, (uint32_t)written, 0 };
    fwrite(meta, 4, 3, fm);
    fclose(fm);
    
    stats.saves++;
    LOG_D("ProgramBinaryCache: SAVE key=%s size=%d", to_hex(key).c_str(), written);
}

} // namespace MG
