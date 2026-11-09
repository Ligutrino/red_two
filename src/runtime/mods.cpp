// Texture packs in Dolphin's format. Aurora does the work (lib/gfx/texture_replacement.cpp):
// it names each texture the game loads like Dolphin does - tex1_<w>x<h>[_m]_<XXH64 of the
// level-0 data>[_<XXH64 of the palette range it uses>]_<GX format>[_arb] - looks the name up in
// the registered folders (.dds or .png, _mipN files, "$" wildcards), and streams the files in on
// worker threads (a 64-pixel mip tail first, then the full texture). The bridge hands it each
// texture's guest memory (gx_bridge.cpp, LOAD_TEXOBJ), so the hashes match Dolphin's.
#include "mods.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>
#include <dolphin/gx/GXStruct.h>
#include <aurora/texture.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "runtime.h"
#include "settings.h"

// Aurora internals (lib/gx/texture.hpp, which needs Aurora's private include paths): the same
// definition, so the replacement counters can be logged.
namespace aurora::gx {
struct TextureStats {
  uint64_t uploadBytes = 0;
  uint64_t hashedBytes = 0;
  uint64_t objectHits = 0;
  uint64_t contentHits = 0;
  uint64_t replacementHits = 0;
  uint64_t misses = 0;
  uint64_t evictions = 0;
  uint64_t contentCacheBytes = 0;
  uint64_t contentCacheEntries = 0;
  uint64_t copyCacheBytes = 0;
  uint64_t copyCacheEntries = 0;
  uint64_t copyCacheHits = 0;
  uint64_t copyCacheMisses = 0;
  uint64_t copyCacheEvictions = 0;
  uint64_t currentCopyBytes = 0;
  uint64_t pendingLoads = 0;
  uint64_t publishes = 0;
  uint64_t publishBytes = 0;
};
const TextureStats& texture_stats() noexcept;
}  // namespace aurora::gx

namespace rt::mods {
namespace fs = std::filesystem;

namespace {

fs::path u8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string str(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}
bool ieq(const std::string& a, const char* b) {
    size_t i = 0;
    for (; i < a.size() && b[i]; i++)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return i == a.size() && !b[i];
}

bool automated() { return envFlag("R2_BACKGROUND") || envFlag("R2_HEADLESS") || getenv("R2_INPUT"); }

std::vector<PackInfo> s_packs;
std::vector<aurora::texture::ReplacementGroup> s_groups;
bool s_aurora = false;  // Aurora is up (textures can be registered)

// the textures root this run uses ("" = none)
std::string texturesRoot() {
    if (const char* d = getenv("R2_TEXTURES_DIR")) return d;
    if (automated() && !envFlag("R2_TEXTURES")) return {};
    return texturesDir();
}

void unload() {
    for (auto& g : s_groups) aurora::texture::unregister_replacements(g);
    s_groups.clear();
    s_packs.clear();
}

void load() {
    unload();
    const std::string root = texturesRoot();
    if (root.empty()) return;
    for (const std::string& dir : packFolders(root)) {
        auto group = aurora::texture::load_replacement_directory(u8(dir));
        PackInfo info{dir, group.registrations.size()};
        logf("mods", "texture pack %s: %zu textures", dir.c_str(), info.textures);
        s_groups.push_back(std::move(group));
        s_packs.push_back(std::move(info));
    }
}

}  // namespace

std::string userDir() {
    static const std::string dir = [] {
        std::string d;
        if (char* p = SDL_GetPrefPath(nullptr, "Red Two")) {
            d = p;
            SDL_free(p);
        }
        return d;
    }();
    return dir;
}

std::string texturesDir() {
    const std::string u = userDir();
    return u.empty() ? std::string("textures\\") : u + "textures\\";
}

std::vector<std::string> packFolders(const std::string& root) {
    std::vector<std::string> out;
    std::error_code ec;
    const fs::path r = u8(root);
    if (!fs::is_directory(r, ec)) return out;
    auto add = [&](const fs::path& p) {
        const std::string s = str(p.lexically_normal());
        for (const auto& o : out)
            if (ieq(o, s.c_str())) return;
        out.push_back(s);
    };
    if (fs::is_directory(r / "GSWE64", ec)) add(r / "GSWE64");
    else if (fs::is_directory(r / "GSW", ec)) add(r / "GSW");
    // marker files: <root>/<pack>/.../GSWE64.txt (or GSW.txt, all.txt) claims <root>/<pack>
    for (fs::recursive_directory_iterator it(r, fs::directory_options::skip_permission_denied, ec), end; it != end;
         it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string name = str(it->path().filename());
        if (!ieq(name, "GSWE64.txt") && !ieq(name, "GSW.txt") && !ieq(name, "all.txt")) continue;
        const fs::path rel = it->path().lexically_relative(r);
        if (std::distance(rel.begin(), rel.end()) < 2) continue;  // a marker in the root itself
        add(r / *rel.begin());
    }
    return out;
}

void texturesInit() {
    s_aurora = true;
    if (settings::get().customTextures) load();
}

void setTexturesEnabled(bool on) {
    if (!s_aurora) return;
    if (on) load();
    else unload();
}

void reloadTextures() {
    if (s_aurora && settings::get().customTextures) load();
}

const std::vector<PackInfo>& loadedPacks() { return s_packs; }

size_t loadedTextureCount() {
    size_t n = 0;
    for (const auto& p : s_packs) n += p.textures;
    return n;
}

namespace {
u64 s_hits = 0, s_published = 0, s_publishedBytes = 0, s_pending = 0;
}

void textureFrame(bool ended) {
    if (s_packs.empty()) return;
    const auto& st = aurora::gx::texture_stats();  // (the GX thread is drained by now)
    if (!ended) {
        s_hits += st.replacementHits;
    } else {
        s_published += st.publishes;
        s_publishedBytes += st.publishBytes;
        s_pending = st.pendingLoads;
    }
}

std::string textureStats() {
    char b[160];
    snprintf(b, sizeof b, "replacements: %llu bound, %llu loaded (%llu MiB), %llu loading",
             (unsigned long long)s_hits, (unsigned long long)s_published,
             (unsigned long long)(s_publishedBytes >> 20), (unsigned long long)s_pending);
    return b;
}

}  // namespace rt::mods
