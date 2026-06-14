// Unit tests for func_sig — the GNU-v2 demangler that seeds type recovery with each function's
// this/parameter engine types (the signature half of the tailored-recomp type DB).
#include "../func_sig.h"

#include <cstdio>
#include <set>
#include <string>

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, msg) do { ++g_checks; if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__); ++g_fail; } } while (0)

// find the engine type assigned to a GPR in a signature, or "".
static std::string at_gpr(const FuncSig& s, int gpr) {
    for (const auto& a : s.ptr_args) if (a.gpr == gpr) return a.type;
    return "";
}

int main() {
    // TFishoid::perform(unsigned long, JDrama::TGraphics*)
    //   this->r3=TFishoid, ulong->r4 (GPR int), TGraphics*->r5
    {
        FuncSig s = demangle_signature("perform__8TFishoidFUlPQ26JDrama9TGraphics");
        CHECK(s.ok && s.is_method, "perform: parsed, method");
        CHECK(s.class_leaf == "TFishoid", "perform: this class TFishoid");
        CHECK(at_gpr(s, 3) == "TFishoid", "perform: this in r3");
        CHECK(at_gpr(s, 5) == "TGraphics", "perform: TGraphics* in r5 (after ulong in r4)");
        CHECK(at_gpr(s, 4) == "", "perform: r4 is the ulong (not a class pointer)");
    }
    // TRealoid::loadDefault(JSUMemoryInputStream&, const char*, int)
    //   this->r3, ref->r4 (JSUMemoryInputStream), const char*->r5, int->r6
    {
        FuncSig s = demangle_signature("loadDefault__8TRealoidFR20JSUMemoryInputStreamPCci");
        CHECK(s.class_leaf == "TRealoid", "loadDefault: this TRealoid");
        CHECK(at_gpr(s, 4) == "JSUMemoryInputStream", "loadDefault: ref param JSUMemoryInputStream in r4");
        // const char* (PCc) is a pointer-to-builtin, not a class -> not recorded, but consumes r5
        CHECK(at_gpr(s, 5) == "" && at_gpr(s, 6) == "", "loadDefault: char*/int are not engine classes");
    }
    // TRealoidActor::calcRootMatrix(TBoid*) -> this r3, TBoid* r4
    {
        FuncSig s = demangle_signature("calcRootMatrix__13TRealoidActorFP5TBoid");
        CHECK(at_gpr(s, 3) == "TRealoidActor" && at_gpr(s, 4) == "TBoid", "calcRootMatrix: this r3, TBoid* r4");
    }
    // ctor: TRealoidActor::TRealoidActor(MActor*) -> this r3, MActor* r4
    {
        FuncSig s = demangle_signature("__ct__13TRealoidActorFP6MActor");
        CHECK(s.is_method && at_gpr(s, 3) == "TRealoidActor", "ctor: this r3 TRealoidActor");
        CHECK(at_gpr(s, 4) == "MActor", "ctor: MActor* r4");
    }
    // namespaced this: MSoundSESystem::MSoundSE::checkMonoSound(unsigned long, JAIActor*)
    {
        FuncSig s = demangle_signature("checkMonoSound__Q214MSoundSESystem8MSoundSEFUlP8JAIActor");
        CHECK(s.class_name == "MSoundSESystem::MSoundSE", "checkMonoSound: qualified this");
        CHECK(at_gpr(s, 3) == "MSoundSE", "checkMonoSound: leaf this MSoundSE in r3");
        CHECK(at_gpr(s, 5) == "JAIActor", "checkMonoSound: JAIActor* in r5 (after ulong r4)");
    }
    // free function: no this. MixAudio(short*, short*, unsigned long) -> r3,r4 short*, r5 ulong
    {
        FuncSig s = demangle_signature("MixAudio__FPsPsUl");
        CHECK(s.ok && !s.is_method, "MixAudio: free function, no this");
        CHECK(s.ptr_args.empty(), "MixAudio: no class-pointer args");
    }
    // float-only free function does not crash and assigns no GPR class args
    {
        FuncSig s = demangle_signature("MsWrap<f>__Ffff");
        CHECK(s.ok && !s.is_method && s.ptr_args.empty(), "MsWrap: floats only, no class ptrs");
    }
    // a name that isn't a mangled C++ symbol
    {
        FuncSig s = demangle_signature("__start");
        CHECK(!s.ok, "__start: not a handled mangled symbol");
    }

    // Build real signatures from the symbol file, filtered to a small engine-type set.
    {
        std::set<std::string> eng = { "TGraphics", "JAIActor", "MActor", "TBoid" };
        auto sigs = build_signatures("reference/sms_gmse01_funcs.txt", eng);
        std::printf("[func_sig] built signatures for %zu funcs touching the engine-type set\n", sigs.size());
        CHECK(sigs.size() > 50, "build_signatures: found many funcs touching the engine types");
        // spot-check the known one
        auto it = sigs.find(0x80007218u);   // TFishoid::perform
        CHECK(it != sigs.end() && it->second.count(5) && it->second[5] == "TGraphics",
              "build_signatures: TFishoid::perform seeds TGraphics in r5");
    }

    std::printf("func_sig_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
