#pragma once
#include <string>

#include "analysis.h"

class Emitter {
public:
    Emitter(const Image& img, const Analysis& an, const Config& cfg) : img_(img), an_(an), cfg_(cfg) {}
    bool write(const std::string& outDir, int files);

private:
    using u32 = uint32_t;
    std::string fn(u32 a) const;
    std::string callTo(u32 t, u32 pc, bool needJb) const;
    void function(const Function& f, std::string& o, const std::string& name) const;
    bool insn(const Function& f, u32 pc, u32 w, std::string& o, bool& usesJb) const;
    bool op31(u32 pc, u32 w, std::string& o) const;
    bool fp59(u32 w, std::string& o) const;
    bool fp63(u32 w, std::string& o) const;
    bool ps(const Function& f, u32 pc, u32 w, std::string& o) const;
    static bool writeFile(const std::string& path, const std::string& text);

    const Image& img_;
    const Analysis& an_;
    const Config& cfg_;
};
