// GTA IV Proper Grass - GPU instanced procedural vegetation.
// https://github.com/paulopatine/GTAIV.ProperGrass  (MIT License)
// Target: GTAIV.exe 1.2.0.59 (Complete Edition). Loaded as an .asi plugin (FusionFix / Ultimate ASI Loader).
// Build: i686-w64-mingw32-g++ -std=c++17 -O2 -shared -static -s src/ProperGrass.cpp -o GTAIV.ProperGrass.asi
#define PROPERGRASS_VERSION "1.0.0-beta"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <algorithm>

// ---------------------------------------------------------------- config / log
static struct {
    int enabled = 1, triCapacity = 16384, maxInstances = 60000, debug = 0, grassTag = 0, profile = 0, toggleKey = 0, frustumCull = 1, cullKey = 0;
    float drawDistance = 0.f;      // 0 = use the game's plant radius
    float densityMul = 1.f, radiusMul = 1.f, debugScale = 1.f;
    float normalTilt = 0.f; float edgeMargin = 0.35f, edgeSoft = 1.0f, fadeWidth = 12.f;
    std::string grassMaterials = "GRASS@EXT_GRASS_DRYPLANTS_SHORT,SHORT_GRASS@EXT_GRASS_DRYPLANTS_SHORT,GRASS_PATCHY@EXT_GRASS_DRYPLANTS_SHORT,GRASS_LONG@EXT_GRASS_DRYPLANTS_LONG";
} cfg;
static FILE* g_log;
static std::mutex g_logMx;
static void Log(const char* fmt, ...)
{
    if (!g_log) return;
    std::lock_guard<std::mutex> l(g_logMx);
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
}
#define DBG(...) do { if (cfg.debug) Log(__VA_ARGS__); } while (0)

// ---------------------------------------------------------------- runtime mode (performance comparisons) + profiler counters
// 0 = mod on; 1 = vanilla plant system (no extra grass, no instancing); 2 = extra grass generated but drawn as ordinary game entities
static volatile int g_mode = 0, g_modeReq = 0;
static const char* kModeName[3] = { "MOD", "VANILLA", "NO-INSTANCING" };
static LARGE_INTEGER g_qpf;
static inline LONGLONG Qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static inline double Ms(LONGLONG t) { return g_qpf.QuadPart ? t * 1000.0 / (double)g_qpf.QuadPart : 0.0; }
static volatile LONGLONG g_tCand = 0, g_tGameList = 0, g_tPacket = 0;
static volatile LONG g_nCand = 0, g_nEnt = 0, g_nUpd = 0;
static volatile float g_aspect = 16.f / 9.f;
static LONG g_renderFrame = 0;
static char g_ovl[2][256]; static std::mutex g_ovlMx;
static LONGLONG g_tRun = 0; static LONG g_dipAll = 0, g_dipOurs = 0; static LONGLONG g_instDrawn = 0;

// ---------------------------------------------------------------- game addresses (1.2.0.59)
namespace G {
    constexpr uintptr_t CandidateAdd   = 0xC093A0; // thiscall(mgr, pos*, nrm*, item*, tri*)
    constexpr uintptr_t TriRelease     = 0xC87010; // fastcall(tri)
    constexpr uintptr_t TriInit        = 0xC86540; // thiscall(tri,...) (recycles a tri slot)
    constexpr uintptr_t TriCapImm      = 0xC86C31; // imm32 = 0x200
    constexpr uintptr_t GBufferBuild   = 0xD685F0; // CRenderPhaseDeferredLighting_SceneToGBuffer::BuildRenderList
    constexpr uintptr_t DcAppend       = 0x5C08F0; // CBaseDC::Append (thiscall)
    constexpr uintptr_t DcAlloc        = 0x8DC3A0; // cdecl(size, 0)
    constexpr uintptr_t DcEntityCtor   = 0x8DACA0; // thiscall(model,b32,b33,b34,mtx*,b35,b36) ret 0x1c
    constexpr uintptr_t DcIdCounter    = 0x10327A0;
    constexpr uintptr_t ItemTable      = 0x16C8FB8; // procobj item table, stride 0x44
    constexpr uintptr_t ItemCount      = 0x16C8FB4;
    constexpr uintptr_t PlantMgr       = 0x16FB6A0;
    constexpr uintptr_t ModelInfoTable = 0x1295CD8;
    constexpr uintptr_t CamPos         = 0x128E340; // float x,y,z
    constexpr uintptr_t D3DDevicePtr   = 0x17ED8D8;
    constexpr uintptr_t DcVtableEntity = 0xE81878;
}
static uintptr_t g_base;
static inline uintptr_t A(uintptr_t va) { return va - 0x400000 + g_base; }

// ---------------------------------------------------------------- tiny inline hook
static void* MakeHook(uintptr_t target, size_t len, void* detour)
{
    BYTE* t = (BYTE*)target;
    if (t[0] == 0xE9) {      // already hooked by another plugin (e.g. FusionFix): chain through it
        uintptr_t dest = target + 5 + *(int32_t*)(t + 1);
        BYTE* tr = (BYTE*)VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        tr[0] = 0xE9; *(int32_t*)(tr + 1) = (int32_t)(dest - ((uintptr_t)tr + 5));
        DWORD old2; VirtualProtect(t, 5, PAGE_EXECUTE_READWRITE, &old2);
        *(int32_t*)(t + 1) = (int32_t)((uintptr_t)detour - (target + 5));
        VirtualProtect(t, 5, old2, &old2); FlushInstructionCache(GetCurrentProcess(), t, 5);
        Log("  chained existing jmp at %p -> %p", t, (void*)dest);
        return tr;
    }
    BYTE* tramp = (BYTE*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return nullptr;
    memcpy(tramp, t, len);
    tramp[len] = 0xE9; *(int32_t*)(tramp + len + 1) = (int32_t)((uintptr_t)t + len - ((uintptr_t)tramp + len + 5));
    DWORD old; VirtualProtect(t, len, PAGE_EXECUTE_READWRITE, &old);
    t[0] = 0xE9; *(int32_t*)(t + 1) = (int32_t)((uintptr_t)detour - (target + 5));
    for (size_t i = 5; i < len; i++) t[i] = 0x90;
    VirtualProtect(t, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, len);
    return tramp;
}

// ---------------------------------------------------------------- vegetation item classification
static char g_mname[4096][48];
struct ItemInfo { bool veg = false; char name[48] = {}; char tag[48] = {}; };
static std::vector<ItemInfo> g_items;       // by table index
static bool g_itemsValid = false;

static bool IsVegetationModel(const char* n)
{
    return !_strnicmp(n, "CJ_PROC_PLANT", 13) || !_strnicmp(n, "SM_WEED", 7) || !_strnicmp(n, "SM_BUSH", 7) ||
           !_strnicmp(n, "SM_PLANT", 8) || !_strnicmp(n, "CJ_DAISY", 8);
}

static void ParseProcedural(const char* path)
{
    FILE* f = fopen(path, "r");
    if (!f) { Log("procedural.dat not found at %s", path); return; }
    char line[512]; bool in = false; std::vector<std::string> tagOrder; std::unordered_map<std::string, std::vector<ItemInfo>> byTag;
    while (fgets(line, sizeof line, f)) {
        char* p = line; while (*p == ' ' || *p == '\t') p++;
        if (!strncmp(p, "PROCOBJ_DATA_START", 18)) { in = true; continue; }
        if (!strncmp(p, "PROCOBJ_DATA_END", 16)) break;
        if (!in || *p == '#' || *p == '\r' || *p == '\n' || !*p) continue;
        char tag[96], model[96];
        if (sscanf(p, "%95s %95s", tag, model) != 2) continue;
        if (!byTag.count(tag)) tagOrder.push_back(tag);
        ItemInfo it; strncpy(it.name, model, 47); strncpy(it.tag, tag, 47); it.veg = IsVegetationModel(model);
        byTag[tag].push_back(it);
    }
    fclose(f);
    g_items.clear();
    for (auto& t : tagOrder) for (auto& it : byTag[t]) g_items.push_back(it);
    uint32_t cnt = *(uint32_t*)A(G::ItemCount);
    int veg = 0; for (auto& i : g_items) veg += i.veg;
    Log("procedural.dat: %zu items parsed (table has %u), %d vegetation items", g_items.size(), cnt, veg);
    g_itemsValid = true;
}

static inline int ItemIndex(const void* item)
{
    intptr_t d = (intptr_t)item - (intptr_t)A(G::ItemTable);
    if (d < 0 || d % 0x44) return -1;
    return (int)(d / 0x44);
}

// ---------------------------------------------------------------- instance store
struct Inst {                 // compact; world matrix is built at frame time
    float pos[3];
    float cs, sn;             // cos/sin of rotation
    float sxy, sz, zoff;
    float nrm[3];
    uint16_t model; uint8_t align; uint8_t pad;
    float base[9];            // X, Y, Z rows of the world matrix at full size (scaled per frame by fade * edge factor)
    float t0z;                // pos.z + zoff
    float es;                 // cached edge factor (<0: removed by the edge margin)
};
// rows of the instance matrix at scale 1 (they are linear in the per-frame scale factor)
static void ComputeBase(Inst& in)
{
    const float sxy = in.sxy, szz = in.sz;
    float X[3] = { in.cs * sxy, in.sn * sxy, 0 };
    float Y[3] = { -in.sn * sxy, in.cs * sxy, 0 };
    float Z[3] = { 0, 0, szz };
    if (in.align) {       // align up-axis to surface normal
        const float* n = in.nrm;
        float yd[3] = { Y[0], Y[1], 0 };
        float cx = yd[1] * n[2] - yd[2] * n[1], cy = yd[2] * n[0] - yd[0] * n[2], cz = yd[0] * n[1] - yd[1] * n[0];
        float l = sqrtf(cx * cx + cy * cy + cz * cz); if (l > 1e-5f) { float s = sxy / l; X[0] = cx * s; X[1] = cy * s; X[2] = cz * s; }
        float yx = n[1] * X[2] - n[2] * X[1], yy = n[2] * X[0] - n[0] * X[2], yz = n[0] * X[1] - n[1] * X[0];
        l = sqrtf(yx * yx + yy * yy + yz * yz); if (l > 1e-5f) { float s = sxy / l; Y[0] = yx * s; Y[1] = yy * s; Y[2] = yz * s; }
        Z[0] = n[0] * szz; Z[1] = n[1] * szz; Z[2] = n[2] * szz;
    }
    memcpy(in.base, X, 12); memcpy(in.base + 3, Y, 12); memcpy(in.base + 6, Z, 12);
    in.t0z = in.pos[2] + in.zoff; in.es = 1.f;
}
struct TriData { std::vector<Inst> v; int sig = -1; float bmin[3] = { 1e30f, 1e30f, 1e30f }, bmax[3] = { -1e30f, -1e30f, -1e30f }; };      // sig: which edges were boundary edges when es was cached
static std::mutex g_instMx;
static std::unordered_map<uintptr_t, TriData> g_byTri;     // tri ptr -> instances
static std::unordered_set<int> g_badModels;                          // models we could not instance (fall back to game entities)
static std::unordered_set<int> g_seenModels;
static volatile LONG g_candCount = 0, g_candSuppressed = 0;
static std::mutex g_badMx;
static bool g_vegSeen[4096];
static bool Rd(uintptr_t p, uintptr_t* o) { if (!p || IsBadReadPtr((void*)p, 4)) return false; *o = *(uintptr_t*)p; return true; }
static bool ModelResident(int m) {
    uintptr_t mi, a, p;
    if (!Rd(A(0x1295CD8) + m * 4, &mi) || !mi) return false;
    if (!Rd(mi + 8, &a)) return false;
    if (a == 0) { uintptr_t q; if (!Rd(mi + 0xC, &q) || !q) return false; return Rd(q, &p) && p != 0; }
    return Rd(a + 0xB4, &p) && p != 0;
}
static int GrassTagFor(int mat);
static std::unordered_map<std::string, int> g_tagNum;
static LONG g_failFrame[4096];
static uint8_t g_mstate[4096];   // 0 unknown (game entities + probing), 1 instancing OK (candidates suppressed), 2 gave up
static int g_mtries[4096];
static int g_tableChecked = 0;   // 0 unknown, 1 ok, -1 mismatch

static inline uint32_t Hash32(uint32_t x) { x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16; return x; }
static inline float Rnd(uint32_t& s) { s = Hash32(s + 0x9e3779b9u); return (s & 0xFFFFFF) / 16777216.f; }

typedef void(__fastcall* CandAddFn)(void* mgr, void*, const float* pos, const float* nrm, const void* item, const void* tri);
static CandAddFn oCandAdd;

static void hCandAddImpl(void* mgr, void* edx, const float* pos, const float* nrm, const void* item, const void* tri);
static void __fastcall hCandAdd(void* mgr, void* edx, const float* pos, const float* nrm, const void* item, const void* tri)
{
    if (!cfg.profile) { hCandAddImpl(mgr, edx, pos, nrm, item, tri); return; }
    LONGLONG t0 = Qpc();
    hCandAddImpl(mgr, edx, pos, nrm, item, tri);
    InterlockedExchangeAdd64(&g_tCand, Qpc() - t0); InterlockedIncrement(&g_nCand);
}
static void hCandAddImpl(void* mgr, void* edx, const float* pos, const float* nrm, const void* item, const void* tri)
{
    InterlockedIncrement(&g_candCount);
    int idx = ItemIndex(item);
    if (g_tableChecked == 0) {
        uint32_t n = *(uint32_t*)A(G::ItemCount);
        if (n) { g_tableChecked = (n == g_items.size()) ? 1 : -1;
            if (g_tableChecked == 1) { std::string lst; for (size_t i = 0; i < g_items.size(); i++) { int tn = (int)*(uint32_t*)((uint8_t*)A(G::ItemTable) + i * 0x44); if (!g_tagNum.count(g_items[i].tag)) { g_tagNum[g_items[i].tag] = tn; lst += " " + std::to_string(tn) + "=" + g_items[i].tag; } } Log("tag numbers:%s", lst.c_str()); }
            Log("item table check: game=%u file=%zu -> %s", n, g_items.size(), g_tableChecked == 1 ? "OK" : "MISMATCH, instancing disabled");
            if (g_tableChecked == 1 && cfg.densityMul != 1.f) { int cnt = 0; for (size_t i = 0; i < g_items.size(); i++) if (g_items[i].veg) { float* d = (float*)((uint8_t*)A(G::ItemTable) + i * 0x44 + 0xC); *d *= cfg.densityMul; cnt++; } Log("density x%.2f applied to %d vegetation items", cfg.densityMul, cnt); } }
    }
    if (g_tableChecked == 1 && g_itemsValid && idx >= 0 && idx < (int)g_items.size() && g_items[idx].veg) {
        const uint8_t* it = (const uint8_t*)item;
        int model = *(const int*)(it + 4);
        if (model >= 0 && model < 4096) { g_vegSeen[model] = true; if (!g_mname[model][0]) strncpy(g_mname[model], g_items[idx].name, 47); }
        if (model >= 0 && model < 4096 && g_mstate[model] != 1) { std::lock_guard<std::mutex> l(g_instMx); g_seenModels.insert(model); }
        static LONG s_pass[4096];
        bool doSupp = false; if (g_mode == 0 && model >= 0 && model < 4096 && g_mstate[model] != 2 && ModelResident(model)) { LONG pc = InterlockedIncrement(&s_pass[model]); doSupp = (pc & 31) != 0; /* 1 in 32 stays a real game entity: keeps the model referenced/streamed in */ }
        if (doSupp) {
            float rotMin = *(float*)(it + 0x14), rotMax = *(float*)(it + 0x18);
            float sclMin = *(float*)(it + 0x1C), sclMax = *(float*)(it + 0x20);
            float szMin  = *(float*)(it + 0x24), szMax  = *(float*)(it + 0x28);
            float zoMin  = *(float*)(it + 0x2C), zoMax  = *(float*)(it + 0x30);
            uint32_t s = Hash32(*(uint32_t*)&pos[0]) ^ Hash32(*(uint32_t*)&pos[1] + 17u) ^ Hash32(*(uint32_t*)&pos[2] + 91u) ^ (uint32_t)idx * 2654435761u;
            Inst in;
            float rot = rotMin + Rnd(s) * (rotMax - rotMin);
            in.cs = cosf(rot); in.sn = sinf(rot);
            in.sxy = (sclMin + Rnd(s) * (sclMax - sclMin)) * cfg.debugScale;
            in.sz  = (szMin + Rnd(s) * (szMax - szMin)) * cfg.debugScale;
            in.zoff = zoMin + Rnd(s) * (zoMax - zoMin);
            memcpy(in.pos, pos, 12); memcpy(in.nrm, nrm, 12);
            in.model = (uint16_t)model; in.align = it[0x34];
            ComputeBase(in);
            {
                std::lock_guard<std::mutex> l(g_instMx);
                TriData& td = g_byTri[(uintptr_t)tri]; td.v.push_back(in); td.sig = -1;
                for (int k = 0; k < 3; k++) { if (pos[k] < td.bmin[k]) td.bmin[k] = pos[k]; if (pos[k] > td.bmax[k]) td.bmax[k] = pos[k]; }
                g_seenModels.insert(model);
            }
            InterlockedIncrement(&g_candSuppressed);
            return;               // do not create a game entity
        }
    }
    InterlockedIncrement(&g_nEnt);
    oCandAdd(mgr, edx, pos, nrm, item, tri);
}

// edge bookkeeping for grass-material triangles: an edge used by only one grass triangle borders some other surface
struct TriEdges { uint64_t key[3]; float p[3][4]; };
static std::unordered_map<uint64_t, uint8_t> g_edgeCnt;
static std::unordered_map<uintptr_t, TriEdges> g_triEdges;
static inline uint64_t QPt(float x, float y) { int32_t a = (int32_t)lrintf(x * 20.f), b = (int32_t)lrintf(y * 20.f); return ((uint64_t)(uint32_t)a << 32) | (uint32_t)b; }
static inline uint64_t EdgeKey(uint64_t a, uint64_t b) { if (a > b) std::swap(a, b); uint64_t h = a * 0x9E3779B97F4A7C15ull; h ^= (h >> 29); h += b * 0xC2B2AE3D27D4EB4Full; h ^= (h >> 31); return h; }
static void EdgesRemove(uintptr_t tri)   // caller holds g_instMx
{
    auto it = g_triEdges.find(tri); if (it == g_triEdges.end()) return;
    for (int e = 0; e < 3; e++) { auto c = g_edgeCnt.find(it->second.key[e]); if (c != g_edgeCnt.end() && --c->second == 0) g_edgeCnt.erase(c); }
    g_triEdges.erase(it);
}
typedef void(__fastcall* TriReleaseFn)(void* tri);
static TriReleaseFn oTriRelease;
static void __fastcall hTriRelease(void* tri)
{
    { std::lock_guard<std::mutex> l(g_instMx); g_byTri.erase((uintptr_t)tri); EdgesRemove((uintptr_t)tri); }
    oTriRelease(tri);
}
typedef void(__cdecl* ResetFn)();
static ResetFn oReset;
static void __cdecl hReset2() { { std::lock_guard<std::mutex> l(g_instMx); g_byTri.clear(); g_edgeCnt.clear(); g_triEdges.clear(); } oReset(); }

// ---------------------------------------------------------------- D3DX (runtime-loaded) and VS patching
struct ID3DXBufferLite : IUnknown { virtual void* STDMETHODCALLTYPE GetBufferPointer() = 0; virtual DWORD STDMETHODCALLTYPE GetBufferSize() = 0; };
typedef HRESULT(WINAPI* PFN_Assemble)(const char*, UINT, const void*, void*, DWORD, ID3DXBufferLite**, ID3DXBufferLite**);
typedef HRESULT(WINAPI* PFN_Disasm)(const DWORD*, BOOL, const char*, ID3DXBufferLite**);
static PFN_Assemble pAssemble; static PFN_Disasm pDisasm; static HMODULE g_d3dx;
static bool LoadD3DX()
{
    if (pAssemble) return true;
    for (int v = 43; v >= 24; --v) {
        char n[32]; sprintf(n, "d3dx9_%d.dll", v);
        HMODULE m = GetModuleHandleA(n); if (!m) m = LoadLibraryA(n);
        if (m) { g_d3dx = m; pAssemble = (PFN_Assemble)GetProcAddress(m, "D3DXAssembleShader"); pDisasm = (PFN_Disasm)GetProcAddress(m, "D3DXDisassembleShader"); if (pAssemble && pDisasm) { Log("using %s", n); return true; } }
    }
    Log("d3dx9 not available"); return false;
}

static const char* kPrologue =
    "    dcl_texcoord6 v12\n    dcl_texcoord7 v13\n    dcl_texcoord8 v14\n    dcl_texcoord9 v15\n"
    "    mov r28, v12\n    mov r29, v13\n    mov r30, v14\n    mov r31, v15\n"
    "    mul r24, r28.x, c8\n    mad r24, r28.y, c9, r24\n    mad r24, r28.z, c10, r24\n"
    "    mul r25, r29.x, c8\n    mad r25, r29.y, c9, r25\n    mad r25, r29.z, c10, r25\n"
    "    mul r26, r30.x, c8\n    mad r26, r30.y, c9, r26\n    mad r26, r30.z, c10, r26\n"
    "    mad r27, c250.x, c8, c11\n    mad r27, c250.y, c9, r27\n    mad r27, c250.z, c10, r27\n"
    "    mad r27, r31.x, c8, r27\n    mad r27, r31.y, c9, r27\n    mad r27, r31.z, c10, r27\n";

static bool PatchVsText(const std::string& src, std::string& out)
{
    // reject if shader defines constants we will virtualise, or uses r24+ temps
    // constants the shader defines itself in c0..c3 / c8..c11 are renamed (c240+N), so the registers can be virtualised for the instance matrices
    bool renamed[16] = {};
    { size_t p2 = 0; while (p2 < src.size()) { size_t e2 = src.find('\n', p2); if (e2 == std::string::npos) e2 = src.size(); std::string ln = src.substr(p2, e2 - p2); p2 = e2 + 1;
        size_t f2 = ln.find_first_not_of(" \t\r"); if (f2 == std::string::npos) continue; int n2 = -1; if (sscanf(ln.c_str() + f2, "def c%d", &n2) == 1 && ((n2 >= 0 && n2 <= 3) || (n2 >= 8 && n2 <= 11))) renamed[n2] = true; } }
    std::string res; size_t pos = 0; bool prologueDone = false; bool usesWorld = false;
    while (pos < src.size()) {
        size_t e = src.find('\n', pos); if (e == std::string::npos) e = src.size();
        std::string line = src.substr(pos, e - pos); pos = e + 1;
        size_t f = line.find_first_not_of(" \t\r");
        std::string t = f == std::string::npos ? "" : line.substr(f);
        bool isDcl = t.rfind("dcl_", 0) == 0;
        bool isDef = t.rfind("def ", 0) == 0 || t.rfind("defi ", 0) == 0 || t.rfind("defb ", 0) == 0;
        if (t.rfind("//", 0) == 0 || t.empty() || t.rfind("vs_", 0) == 0) { res += line + "\n"; continue; }
        if (isDef) {
            // def cN
            int n = -1;
            if (sscanf(t.c_str(), "def c%d", &n) == 1 && ((n >= 0 && n <= 3) || (n >= 8 && n <= 11))) { size_t cpos = t.find("c" + std::to_string(n)); res += "    def c" + std::to_string(240 + n) + t.substr(cpos + 1 + std::to_string(n).size()) + "\n"; continue; }
            res += line + "\n"; continue;
        }
        if (!isDcl && !prologueDone) { res += kPrologue; prologueDone = true; }
        if (isDcl) { res += line + "\n"; continue; }
        // temp usage check
        for (size_t i = 0; i + 2 < t.size(); i++)
            if (t[i] == 'r' && isdigit((unsigned char)t[i + 1]) && (i == 0 || !isalnum((unsigned char)t[i - 1]))) { int r = atoi(t.c_str() + i + 1); if (r >= 24) return false; }
        // replace operands c0..c3 -> r28..r31, c8..c11 -> r24..r27 (not followed by digit; not 'cN[' relative)
        std::string o; size_t i = 0;
        while (i < t.size()) {
            if (t[i] == 'c' && (i == 0 || !(isalnum((unsigned char)t[i - 1]) || t[i - 1] == '_' || t[i - 1] == '.'))) {
                size_t j = i + 1; while (j < t.size() && isdigit((unsigned char)t[j])) j++;
                if (j > i + 1 && !(j < t.size() && isalnum((unsigned char)t[j]))) {
                    int n = atoi(t.c_str() + i + 1);
                    if (j < t.size() && t[j] == '[') return false;
                    if (((n >= 0 && n <= 3) || (n >= 8 && n <= 11)) && renamed[n]) { o += "c" + std::to_string(240 + n); i = j; continue; }
                    if (n >= 0 && n <= 3) { o += "r" + std::to_string(28 + n); i = j; usesWorld = true; continue; }
                    if (n >= 8 && n <= 11) { o += "r" + std::to_string(24 + n - 8); i = j; usesWorld = true; continue; }
                }
            }
            o += t[i++];
        }
        res += "    " + o + "\n";
    }
    if (!prologueDone || !usesWorld) return false;
    if (cfg.normalTilt > 0.f) {   // grass blades: tilt the lighting normal towards the sky so headlights/low sun do not make them glow
        const std::string key = "    mul o2.xyz, r0, r0.w\n";
        size_t kp = res.find(key);
        bool r1used = false; for (size_t i = 0; i + 1 < res.size(); i++) if (res[i] == 'r' && res[i + 1] == '1' && (i + 2 >= res.size() || !isdigit((unsigned char)res[i + 2])) && (i == 0 || !isalnum((unsigned char)res[i - 1]))) { r1used = true; break; }
        if (kp != std::string::npos && !r1used && res.find("c231") == std::string::npos) {
            char rep[256]; snprintf(rep, sizeof rep, "    mad r1.xyz, r0, r0.w, c231.xyz\n    dp3 r1.w, r1, r1\n    rsq r1.w, r1.w\n    mul o2.xyz, r1, r1.w\n");
            res.replace(kp, key.size(), rep);
            size_t vp = res.find("vs_3_0"); size_t le = res.find('\n', vp);
            char df[96]; snprintf(df, sizeof df, "    def c231, 0, 0, %.3f, 0\n", cfg.normalTilt);
            res.insert(le + 1, df);
        }
    }
    out = res; return true;
}

// ---------------------------------------------------------------- D3D device hooks
static IDirect3DDevice9* g_dev;
static void* g_vtblOrig[120];
static IDirect3DVertexBuffer9* g_instVB; static UINT g_instVBCap;
struct VsPatch { IDirect3DVertexShader9* vs = nullptr; bool failed = false; };
static std::unordered_map<IDirect3DVertexShader9*, VsPatch> g_vsCache;
static std::unordered_map<IDirect3DVertexDeclaration9*, IDirect3DVertexDeclaration9*> g_declCache;

static bool g_ownCtor;
struct Batch { int bucket = -1; float dist = 0; float negT[4] = {0,0,0,0}; int failedDips = 0; int anyDip = 0; bool on = false; UINT count = 0; UINT byteOffset = 0; int model = -1; int draws = 0; bool failed = false; } g_batch;

static IDirect3DVertexShader9* GetPatchedVS(IDirect3DVertexShader9* vs)
{
    auto it = g_vsCache.find(vs);
    if (it != g_vsCache.end()) return it->second.vs;
    VsPatch p; p.failed = true;
    UINT size = 0;
    if (LoadD3DX() && SUCCEEDED(vs->GetFunction(nullptr, &size)) && size) {
        std::vector<DWORD> code(size / 4 + 1);
        vs->GetFunction(code.data(), &size);
        ID3DXBufferLite* dis = nullptr;
        if (SUCCEEDED(pDisasm(code.data(), FALSE, nullptr, &dis)) && dis) {
            std::string src((const char*)dis->GetBufferPointer(), dis->GetBufferSize()), patched;
            dis->Release();
            if (PatchVsText(src, patched)) {
                ID3DXBufferLite* bin = nullptr; ID3DXBufferLite* err = nullptr;
                HRESULT hr = pAssemble(patched.c_str(), (UINT)patched.size(), nullptr, nullptr, 0, &bin, &err);
                if (SUCCEEDED(hr) && bin) {
                    IDirect3DVertexShader9* nvs = nullptr;
                    if (SUCCEEDED(g_dev->CreateVertexShader((const DWORD*)bin->GetBufferPointer(), &nvs))) { p.vs = nvs; p.failed = false; }
                    bin->Release();
                } else Log("assemble failed hr=%08x: %s", (unsigned)hr, err ? (const char*)err->GetBufferPointer() : "?");
                if (err) err->Release();
            } else { DBG("VS %p not patchable", vs); static int dumped = 0; if (cfg.debug && dumped < 6) { dumped++; FILE* f = fopen("ProperGrass.vs.txt", "a"); if (f) { fprintf(f, "==== %p\n%s\n", vs, src.c_str()); fclose(f); } } }
        }
    }
    g_vsCache[vs] = p; DBG("VS %p -> patched %p (%s)", vs, p.vs, p.failed ? "FAILED" : "ok");
    return p.vs;
}

static IDirect3DVertexDeclaration9* GetInstDecl(IDirect3DVertexDeclaration9* d)
{
    auto it = g_declCache.find(d); if (it != g_declCache.end()) return it->second;
    UINT n = 0; d->GetDeclaration(nullptr, &n);
    std::vector<D3DVERTEXELEMENT9> el(n);
    d->GetDeclaration(el.data(), &n);
    // drop END, add 4 instance elements on stream 1
    while (!el.empty() && el.back().Stream == 0xFF) el.pop_back();
    for (int i = 0; i < 4; i++) { D3DVERTEXELEMENT9 e = { 1, (WORD)(i * 16), D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, (BYTE)(6 + i) }; el.push_back(e); }
    D3DVERTEXELEMENT9 endE = D3DDECL_END(); el.push_back(endE);
    IDirect3DVertexDeclaration9* nd = nullptr;
    if (FAILED(g_dev->CreateVertexDeclaration(el.data(), &nd))) nd = nullptr;
    g_declCache[d] = nd; return nd;
}

typedef HRESULT(STDMETHODCALLTYPE* DIP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

// DrawIndexedPrimitive: while our batch is active, turn the game's single draw into a hardware-instanced one
static HRESULT STDMETHODCALLTYPE hDIP(IDirect3DDevice9* d, D3DPRIMITIVETYPE pt, INT bv, UINT mi, UINT nv, UINT si, UINT pc)
{
    DIP_t orig = (DIP_t)g_vtblOrig[82];
    if (g_batch.on) g_batch.anyDip++;
    if (!g_batch.on) { g_dipAll++; return orig(d, pt, bv, mi, nv, si, pc); }
    if (g_batch.failed || !g_instVB) {      // our sample draw must never reach the screen un-instanced (it sits right in front of the camera)
        static LONG leaks = 0; if (cfg.debug && leaks++ < 20) Log("sample draw swallowed (failed=%d vb=%p model=%d)", (int)g_batch.failed, (void*)g_instVB, g_batch.model);
        return S_OK;
    }
    IDirect3DVertexShader9* vs = nullptr; IDirect3DVertexDeclaration9* decl = nullptr;
    d->GetVertexShader(&vs); d->GetVertexDeclaration(&decl);
    IDirect3DVertexShader9* pvs = vs ? GetPatchedVS(vs) : nullptr;
    IDirect3DVertexDeclaration9* pdecl = decl ? GetInstDecl(decl) : nullptr;
    if (cfg.debug) {      // which meshes/shaders does the sample draw issue, and how does that depend on the view?
        static int nl = 0; static float lastPitch = 9; const float* rr = (const float*)A(0x128E310);
        bool steep = rr[6] < -0.5f; static bool lastSteep = false; if (steep != lastSteep) { lastSteep = steep; nl = 0; }
        if (nl < 40) { nl++; uint32_t h = 2166136261u; UINT sz2 = 0; if (vs && SUCCEEDED(vs->GetFunction(nullptr, &sz2)) && sz2) { std::vector<uint8_t> cc(sz2); vs->GetFunction(cc.data(), &sz2); for (UINT i = 0; i < sz2; i++) { h ^= cc[i]; h *= 16777619u; } }
            IDirect3DPixelShader9* ps = nullptr; d->GetPixelShader(&ps); uint32_t ph = 2166136261u; UINT psz = 0; if (ps && SUCCEEDED(ps->GetFunction(nullptr, &psz)) && psz) { std::vector<uint8_t> cc(psz); ps->GetFunction(cc.data(), &psz); for (UINT i = 0; i < psz; i++) { ph ^= cc[i]; ph *= 16777619u; } } if (ps) ps->Release();
            Log("[dip] %s model=%d bucket=%d dist=%.1f vs=%08x ps=%08x prims=%u verts=%u patched=%d", steep ? "STEEP" : "level", g_batch.model, g_batch.bucket, g_batch.dist, h, ph, pc, nv, pvs && pdecl ? 1 : 0); }
        (void)lastPitch;
    }
    HRESULT hr;
    if (!pvs || !pdecl) {
        g_batch.failedDips++;      // this mesh cannot be instanced: skip only it, the other meshes of the model still draw
        hr = S_OK;
    } else {
        d->SetVertexShaderConstantF(250, g_batch.negT, 1);
        d->SetVertexShader(pvs); d->SetVertexDeclaration(pdecl);
        d->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | g_batch.count);
        d->SetStreamSource(1, g_instVB, g_batch.byteOffset, 64);
        d->SetStreamSourceFreq(1, D3DSTREAMSOURCE_INSTANCEDATA | 1u);
        hr = orig(d, pt, bv, mi, nv, si, pc);
        d->SetStreamSourceFreq(0, 1); d->SetStreamSourceFreq(1, 1);
        d->SetStreamSource(1, nullptr, 0, 0);
        d->SetVertexShader(vs); d->SetVertexDeclaration(decl);
        g_batch.draws++; g_dipOurs++; g_instDrawn += g_batch.count;
    }
    if (vs) vs->Release(); if (decl) decl->Release();
    return hr;
}

// GPU time of the grass draws: timestamp pairs around each run, read back 3 frames later
struct GpuSlot { IDirect3DQuery9* q[8] = {}; int n = 0; };
static GpuSlot g_gs[4]; static IDirect3DQuery9* g_freqQ; static UINT64 g_gpuFreq; static double g_gpuMs = 0; static int g_gpuFrames = 0;
static void GpuReleaseAll() { for (auto& s : g_gs) { for (auto*& q : s.q) if (q) { q->Release(); q = nullptr; } s.n = 0; } if (g_freqQ) { g_freqQ->Release(); g_freqQ = nullptr; } g_gpuFreq = 0; }
// on-screen panel (D3DX font, drawn just before Present with the game's state saved and restored)
struct ID3DXFontLite : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetDevice(void**) = 0; virtual HRESULT STDMETHODCALLTYPE GetDescA(void*) = 0; virtual HRESULT STDMETHODCALLTYPE GetDescW(void*) = 0;
    virtual BOOL STDMETHODCALLTYPE GetTextMetricsA(void*) = 0; virtual BOOL STDMETHODCALLTYPE GetTextMetricsW(void*) = 0; virtual HDC STDMETHODCALLTYPE GetDC() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetGlyphData(UINT, void**, RECT*, POINT*) = 0; virtual HRESULT STDMETHODCALLTYPE PreloadCharacters(UINT, UINT) = 0; virtual HRESULT STDMETHODCALLTYPE PreloadGlyphs(UINT, UINT) = 0;
    virtual HRESULT STDMETHODCALLTYPE PreloadTextA(LPCSTR, INT) = 0; virtual HRESULT STDMETHODCALLTYPE PreloadTextW(LPCWSTR, INT) = 0;
    virtual INT STDMETHODCALLTYPE DrawTextA(void*, LPCSTR, INT, LPRECT, DWORD, D3DCOLOR) = 0; virtual INT STDMETHODCALLTYPE DrawTextW(void*, LPCWSTR, INT, LPRECT, DWORD, D3DCOLOR) = 0;
    virtual HRESULT STDMETHODCALLTYPE OnLostDevice() = 0; virtual HRESULT STDMETHODCALLTYPE OnResetDevice() = 0;
};
typedef HRESULT(WINAPI* PFN_CreateFont)(IDirect3DDevice9*, INT, UINT, UINT, UINT, BOOL, DWORD, DWORD, DWORD, DWORD, LPCSTR, ID3DXFontLite**);
static ID3DXFontLite* g_font; static IDirect3DStateBlock9* g_sb; static bool g_fontFailed;
static void DrawOverlay(IDirect3DDevice9* d)
{
    if (g_fontFailed) return;
    if (!g_font) {
        PFN_CreateFont cf = (LoadD3DX() && g_d3dx) ? (PFN_CreateFont)GetProcAddress(g_d3dx, "D3DXCreateFontA") : nullptr;
        IDirect3DSurface9* bb = nullptr; D3DSURFACE_DESC bd = {}; if (SUCCEEDED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) { bb->GetDesc(&bd); bb->Release(); }
        int h = bd.Height ? (int)(bd.Height / 1080.0 * 20) : 20; if (h < 14) h = 14;
        if (!cf || FAILED(cf(d, h, 0, FW_BOLD, 1, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Consolas", &g_font)) || !g_font) { g_fontFailed = true; g_font = nullptr; Log("overlay: font not available"); return; }
    }
    if (!g_sb && FAILED(d->CreateStateBlock(D3DSBT_ALL, &g_sb))) { g_sb = nullptr; return; }
    char l0[256], l1[256]; { std::lock_guard<std::mutex> l(g_ovlMx); memcpy(l0, g_ovl[0], 256); memcpy(l1, g_ovl[1], 256); }
    if (!l0[0]) return;
    static bool first = true;
    g_sb->Capture();
    IDirect3DSurface9* rt[4] = {}; IDirect3DSurface9* ds = nullptr; IDirect3DSurface9* bb = nullptr;
    for (int i = 0; i < 4; i++) d->GetRenderTarget(i, &rt[i]);
    d->GetDepthStencilSurface(&ds);
    if (SUCCEEDED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        d->SetRenderTarget(0, bb); for (int i = 1; i < 4; i++) d->SetRenderTarget(i, nullptr); d->SetDepthStencilSurface(nullptr);
        D3DSURFACE_DESC bd = {}; bb->GetDesc(&bd);
        int y = (int)(bd.Height * 0.10), lh = (int)(bd.Height / 1080.0 * 24);
        if (first) { first = false; Log("overlay: first draw on %ux%u back buffer", bd.Width, bd.Height); }
        if (SUCCEEDED(d->BeginScene())) {
            const char* ls[2] = { l0, l1 };
            for (int i = 0; i < 2; i++) {
                RECT r = { 22, y + i * lh + 2, (LONG)bd.Width, y + i * lh + lh + 2 }; g_font->DrawTextA(nullptr, ls[i], -1, &r, DT_LEFT | DT_NOCLIP, 0xC0000000);
                RECT q = { 20, y + i * lh, (LONG)bd.Width, y + i * lh + lh }; g_font->DrawTextA(nullptr, ls[i], -1, &q, DT_LEFT | DT_NOCLIP, i ? 0xFFFFFFFF : 0xFF7CFC7C);
            }
            d->EndScene();
        }
        bb->Release();
    }
    for (int i = 0; i < 4; i++) { d->SetRenderTarget(i, rt[i]); if (rt[i]) rt[i]->Release(); }
    d->SetDepthStencilSurface(ds); if (ds) ds->Release();
    g_sb->Apply();
}
typedef HRESULT(STDMETHODCALLTYPE* Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
static LONG g_ovlDrawnFrame = -1; static LONG g_presDev = 0, g_presSc = 0;
static void OverlayOnce(IDirect3DDevice9* d) { if (!cfg.profile || g_ovlDrawnFrame == g_renderFrame) return; g_ovlDrawnFrame = g_renderFrame; DrawOverlay(d); }
static HRESULT STDMETHODCALLTYPE hPresent(IDirect3DDevice9* d, const RECT* a, const RECT* b, HWND w, const RGNDATA* r)
{
    if (g_presDev++ == 0) Log("overlay: device Present called");
    OverlayOnce(d);
    return ((Present_t)g_vtblOrig[17])(d, a, b, w, r);
}
typedef HRESULT(STDMETHODCALLTYPE* ScPresent_t)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
static ScPresent_t oScPresent;
static HRESULT STDMETHODCALLTYPE hScPresent(IDirect3DSwapChain9* sc, const RECT* a, const RECT* b, HWND w, const RGNDATA* r, DWORD fl)
{
    if (g_presSc++ == 0) Log("overlay: swapchain Present called");
    IDirect3DDevice9* d = nullptr; if (SUCCEEDED(sc->GetDevice(&d)) && d) { OverlayOnce(d); d->Release(); }
    return oScPresent(sc, a, b, w, r, fl);
}
static HRESULT STDMETHODCALLTYPE hReset(IDirect3DDevice9* d, D3DPRESENT_PARAMETERS* pp)
{
    GpuReleaseAll();
    if (g_sb) { g_sb->Release(); g_sb = nullptr; }
    if (g_font) g_font->OnLostDevice();
    if (g_instVB) { g_instVB->Release(); g_instVB = nullptr; }
    for (auto& v : g_vsCache) if (v.second.vs) v.second.vs->Release();
    g_vsCache.clear();
    for (auto& v : g_declCache) if (v.second) v.second->Release();
    g_declCache.clear();
    HRESULT hr = ((Reset_t)g_vtblOrig[16])(d, pp);
    if (g_font && SUCCEEDED(hr)) g_font->OnResetDevice();
    if (SUCCEEDED(hr) && pp && pp->BackBufferHeight) g_aspect = (float)pp->BackBufferWidth / pp->BackBufferHeight;
    return hr;
}

static bool PatchVtable(int idx, void* fn)
{
    void** vt = *(void***)g_dev;
    DWORD old; if (!VirtualProtect(&vt[idx], 4, PAGE_READWRITE, &old)) return false;
    g_vtblOrig[idx] = vt[idx]; vt[idx] = fn; VirtualProtect(&vt[idx], 4, old, &old);
    return true;
}

static bool EnsureDevice()
{
    if (g_dev) return true;
    IDirect3DDevice9* d = *(IDirect3DDevice9**)A(G::D3DDevicePtr);
    if (!d || IsBadReadPtr(d, 4)) return false;
    g_dev = d;
    { IDirect3DSurface9* bb = nullptr; D3DSURFACE_DESC bd = {}; if (SUCCEEDED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) { bb->GetDesc(&bd); bb->Release(); if (bd.Height) g_aspect = (float)bd.Width / bd.Height; } }
    bool ok = PatchVtable(82, (void*)&hDIP) && PatchVtable(16, (void*)&hReset) && (!cfg.profile || PatchVtable(17, (void*)&hPresent));
    if (cfg.profile) {      // some setups present through the swap chain object instead of the device
        IDirect3DSwapChain9* sc = nullptr;
        if (SUCCEEDED(d->GetSwapChain(0, &sc)) && sc) {
            void** vt = *(void***)sc; DWORD old;
            if (VirtualProtect(&vt[3], 4, PAGE_READWRITE, &old)) { oScPresent = (ScPresent_t)vt[3]; vt[3] = (void*)&hScPresent; VirtualProtect(&vt[3], 4, old, &old); }
            sc->Release();
        }
    }
    Log("device %p hooks %s", d, ok ? "ok" : "FAILED");
    return ok;
}

// ---------------------------------------------------------------- frame packets
struct Group { int model; uint32_t first; uint32_t count; };
static int g_preCnt[4096]; static float g_fSum[4096];
struct Packet { float samplePos[3] = {0,0,0}; std::vector<Group> groups; std::vector<float> data; };       // data: 16 floats / instance
static Packet g_packets[4];
static volatile int g_goodDirPub = -1;
static volatile LONG g_frameNo = 0;
static int g_logFrames = 0;
static volatile LONG g_edAll = 0;

struct BItem { const Inst* in; float f; };
static void BuildPacket(Packet& pk)
{
    pk.groups.clear(); pk.data.clear();
    memset(g_preCnt, 0, sizeof g_preCnt); memset(g_fSum, 0, sizeof g_fSum);
    const float* cam = (const float*)A(G::CamPos);
    const float* fwd = (const float*)A(0x128E320);
    for (int i = 0; i < 3; i++) pk.samplePos[i] = cam[i] + fwd[i] * 4.0f;
    float R = cfg.drawDistance > 0 ? cfg.drawDistance : *(float*)(A(G::PlantMgr) + 0x14);
    if (R < 10.f) R = 10.f;
    float R2 = R * R;
    float fw = cfg.fadeWidth; if (fw > R * 0.6f) fw = R * 0.6f;
    // view cone: a circular cone around the camera's forward axis that contains the whole screen (vertical FOV up to 70 deg,
    // current aspect ratio) with 15% spare for the one-frame lag; plants within CullMargin of the cone are kept
    const bool cull = cfg.frustumCull != 0;
    const float tv = 0.70f, th = tv * g_aspect, tanC = sqrtf(th * th + tv * tv) * 1.15f, cullMargin = 2.5f;
    static std::vector<BItem> bucket[4096];
    static std::vector<int> used;
    used.clear();
    std::lock_guard<std::mutex> l(g_instMx);
    size_t total = 0;
    const uintptr_t arrBase = *(uint32_t*)(A(G::PlantMgr) + 0xF30);
    const uintptr_t arrEnd = arrBase + (uintptr_t)(*(uint32_t*)(A(G::PlantMgr) + 0x18)) * 0x60;
        for (auto it = g_byTri.begin(); it != g_byTri.end();) {
        const uint8_t* tri = (const uint8_t*)it->first;
        if (!arrBase || it->first < arrBase || it->first >= arrEnd || !(tri[0x55] & 4)) { it = g_byTri.erase(it); continue; }      // slot no longer in "spawned" state
        TriData& td0 = it->second;
        if (!td0.v.empty()) {      // whole triangle out of range / out of view: skip all its plants at once
            float c[3], ext2 = 0.f; for (int k = 0; k < 3; k++) { c[k] = 0.5f * (td0.bmin[k] + td0.bmax[k]); float e = td0.bmax[k] - td0.bmin[k]; ext2 += e * e; }
            const float rad = 0.5f * sqrtf(ext2);
            float dx = c[0] - cam[0], dy = c[1] - cam[1], d2 = dx * dx + dy * dy;
            if (d2 > (R + rad) * (R + rad)) { if (cfg.debug) for (const Inst& in : td0.v) if (in.model < 4096) g_preCnt[in.model]++; ++it; continue; }
            if (cull) {
                float dz = c[2] - cam[2], z = dx * fwd[0] + dy * fwd[1] + dz * fwd[2];
                float lim = z * tanC + cullMargin + rad * (1.f + tanC);
                if (lim <= 0.f || d2 + dz * dz - z * z > lim * lim) { if (cfg.debug) for (const Inst& in : td0.v) if (in.model < 4096) g_preCnt[in.model]++; ++it; continue; }
            }
        }
        // boundary edges of this triangle (edges not shared with another grass triangle)
        float bnd[3][4]; int nb = 0, sig = 0;
        if (cfg.edgeMargin > 0.f) { auto te = g_triEdges.find(it->first); if (te != g_triEdges.end()) for (int e = 0; e < 3; e++) { auto c = g_edgeCnt.find(te->second.key[e]); if (c != g_edgeCnt.end() && c->second == 1) { memcpy(bnd[nb++], te->second.p[e], 16); sig |= 1 << e; } } }
        TriData& td = it->second;
        if (td.sig != sig) {      // the edge situation changed (or new plants): recompute the cached edge factors of this triangle
            td.sig = sig;
            for (Inst& in : td.v) {
                float es = 1.f;
                if (nb) {
                    float dmin2 = 1e30f;
                    for (int e = 0; e < nb; e++) {
                        float ax = bnd[e][0], ay = bnd[e][1], ex = bnd[e][2] - ax, ey = bnd[e][3] - ay;
                        float px = in.pos[0] - ax, py = in.pos[1] - ay, L = ex * ex + ey * ey;
                        float t = L > 1e-8f ? (px * ex + py * ey) / L : 0.f; t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
                        float qx = px - ex * t, qy = py - ey * t;
                        float q2 = qx * qx + qy * qy; if (q2 < dmin2) dmin2 = q2;
                    }
                    float dm = sqrtf(dmin2);
                    if (dm < cfg.edgeMargin) es = -1.f;
                    else { float m1 = cfg.edgeMargin + cfg.edgeSoft; if (dm < m1 && cfg.edgeSoft > 0.01f) { float u = (dm - cfg.edgeMargin) / cfg.edgeSoft; u = u * u * (3.f - 2.f * u); es = 0.25f + 0.75f * u; } }
                }
                in.es = es;
            }
        }
        for (const Inst& in : td.v) {
            if (cfg.debug && in.model < 4096) g_preCnt[in.model]++;
            if (in.es < 0.f) continue;
            float dx = in.pos[0] - cam[0], dy = in.pos[1] - cam[1];
            float d2 = dx * dx + dy * dy;
            if (d2 > R2) continue;
            if (cull) {
                float dz = in.pos[2] - cam[2];
                float z = dx * fwd[0] + dy * fwd[1] + dz * fwd[2];
                float lim = z * tanC + cullMargin;
                if (lim <= 0.f) continue;
                float perp2 = d2 + dz * dz - z * z;
                if (perp2 > lim * lim) continue;
            }
            if (total >= (size_t)cfg.maxInstances) break;
            float f = in.es;
            if (fw > 0.5f) { float u = (R - sqrtf(d2)) / fw; u = u < 0.f ? 0.f : (u > 1.f ? 1.f : u); f *= u * u * (3.f - 2.f * u); }
            if (f <= 0.02f) continue;
            if (bucket[in.model].empty()) used.push_back(in.model);
            g_fSum[in.model] += f;
            bucket[in.model].push_back({ &in, f }); total++;
        }
        ++it;
    }
    for (int m : used) {
        auto& b = bucket[m];
        const uint8_t* mi = *(const uint8_t**)A(G::ModelInfoTable + 4 * m);
        float minZ = mi ? *(const float*)(mi + 0x28) : 0.f;
        if (cfg.debug && g_logFrames < 3) Log("model %d: modelinfo=%p minZ=%.3f count=%zu", m, mi, minZ, b.size());
        Group g{ m, (uint32_t)(pk.data.size() / 16), (uint32_t)b.size() };
        size_t o = pk.data.size(); pk.data.resize(o + b.size() * 16);
        float* dst = &pk.data[o];
        for (const BItem& bi : b) {
            const Inst* in = bi.in; const float f = bi.f; const float* B = in->base;
            dst[0] = B[0] * f; dst[1] = B[1] * f; dst[2] = B[2] * f; dst[3] = 0.f;
            dst[4] = B[3] * f; dst[5] = B[4] * f; dst[6] = B[5] * f; dst[7] = 0.f;
            dst[8] = B[6] * f; dst[9] = B[7] * f; dst[10] = B[8] * f; dst[11] = 0.f;
            dst[12] = in->pos[0]; dst[13] = in->pos[1]; dst[14] = in->t0z - minZ * in->sz * f; dst[15] = 1.f;
            dst += 16;
        }
        b.clear();
        pk.groups.push_back(g);
    }
}

// ---------------------------------------------------------------- draw commands
// Strategy: we do not insert our own entity commands into the draw list (they were culled / ran with the wrong state).
// Instead, right after the game executes a *real* entity command inside the deferred G-buffer pass (so all render
// state, viewport and technique are valid), we run our own sample entity draw for each vegetation model and turn its
// DrawIndexedPrimitive into a single instanced draw.
typedef void*(__cdecl* DcAlloc_t)(size_t, int);
typedef void(__fastcall* DcAppend_t)(void*, void*);
typedef void*(__fastcall* DcEntityCtor_t)(void* self, void*, int model, int b32, int b33, int b34, const void* mtx, int b35, int b36);
typedef void(__fastcall* EntDraw_t)(void*);
static EntDraw_t oEntDraw;

struct CbDc { void** vt; uint32_t id; int kind; Packet* pk; int group; };
static void* g_cbVt[3];
static void __fastcall CbDtor(void* self, void*, unsigned) {}
static int __fastcall CbSize(void*, void*) { return 0x18; }
static void __fastcall CbDraw(CbDc* dc, void*);

static void AppendMarker(Packet* pk)
{
    CbDc* dc = (CbDc*)((DcAlloc_t)A(G::DcAlloc))(0x18, 0);
    if (!dc) return;
    uint32_t* idc = (uint32_t*)A(G::DcIdCounter);
    dc->vt = g_cbVt; dc->id = dc->id ^ ((dc->id ^ *idc) & 0x3fff); (*idc)++;
    dc->kind = 2; dc->pk = pk; dc->group = 0;
    ((DcAppend_t)A(G::DcAppend))(dc, nullptr);
}

static const float kIdentity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
struct SampleDc { alignas(16) uint8_t mem[0x40]; bool made = false; };
static std::unordered_map<int, SampleDc*> g_samples;

static Packet* g_curPacket;               // set by the marker command (render thread)
static LONG g_fbRuns = 0, g_fbSkip = 0;
static unsigned g_ranMask = 0;
static bool g_inRun = false;


static int g_goodDir = -1;               // (mirrored to g_goodDirPub) calibrated sample direction (axis*2+sign) in camera basis
static float g_zeroInst[16] = {0};

// Draws the sample entity of `model` (one attempt) with the instance buffer already uploaded; returns number of instanced draws.
// b32 = entity alpha (LOD fade, sent to the shader as globalScalars.x) -> instanced grass is always fully opaque.
// b33 = second globalScalars component, copied from fully visible real vegetation commands.
static uint8_t g_ambB32 = 255, g_ambB33 = 254;
static float g_goodDist = 4.0f; static long g_dbgAttempts = 0, g_dbgAnyDip = 0, g_dbgFailed = 0, g_dbgUploadFail = 0;
static int SampleAttempt(int model, int dirChoice, int bucket, float dist = 4.0f)
{
    SampleDc*& sd = g_samples[model];
    if (!sd) sd = new SampleDc();
    const float* camp = (const float*)A(G::CamPos);
    const float* rows = (const float*)A(0x128E310);
    int axis = dirChoice >> 1; float sg = (dirChoice & 1) ? 1.f : -1.f;
    float p[3]; for (int k = 0; k < 3; k++) p[k] = camp[k] + sg * dist * rows[axis * 4 + k];
    for (int k = 0; k < 3; k++) g_batch.negT[k] = -p[k];
    g_batch.failed = false; g_batch.draws = 0; g_batch.anyDip = 0; g_batch.failedDips = 0; g_batch.on = true; g_batch.model = model; g_batch.bucket = bucket; g_batch.dist = dist;
    float m[16]; memcpy(m, kIdentity, sizeof m); m[12] = p[0]; m[13] = p[1]; m[14] = p[2];
    if (!sd->made) {
        g_ownCtor = true;
        ((DcEntityCtor_t)A(G::DcEntityCtor))(sd->mem, nullptr, model, g_ambB32, g_ambB33, 0, m, bucket, 0);
        g_ownCtor = false; sd->made = true;
    } else {
        memcpy(sd->mem + 0x20, &m[12], 12);
    }
    sd->mem[0x35] = (uint8_t)bucket; sd->mem[0x32] = g_ambB32; sd->mem[0x33] = g_ambB33;
    oEntDraw(sd->mem);
    g_batch.on = false;
    g_dbgAttempts++; g_dbgAnyDip += g_batch.anyDip; if (g_batch.failedDips) g_dbgFailed++;
    return g_batch.draws;
}

static bool UploadInstances(const float* data, UINT count)
{
    UINT need = count * 64;
    if (!g_instVB) {
        g_instVBCap = (UINT)cfg.maxInstances * 64 * 2;
        if (FAILED(g_dev->CreateVertexBuffer(g_instVBCap, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &g_instVB, nullptr))) { g_instVB = nullptr; return false; }
    }
    static UINT cursor = 0;
    DWORD flags = D3DLOCK_NOOVERWRITE;
    if (cursor + need > g_instVBCap) { cursor = 0; flags = D3DLOCK_DISCARD; }
    void* p = nullptr;
    if (FAILED(g_instVB->Lock(cursor, need, &p, flags)) || !p) return false;
    memcpy(p, data, need);
    g_instVB->Unlock();
    g_batch.count = count; g_batch.byteOffset = cursor;
    cursor += need;
    return true;
}

static uint8_t g_bucketMask[4096];      // buckets in which a model has ever drawn (a model usually has meshes in one bucket only)
static int g_frameDraws[4096]; static volatile LONG g_lastDraws = 0, g_lastDrawFrames = 0;
static void RunGroups(Packet* pk, int bucket)
{
    if (!EnsureDevice()) return;
    static int logged = 0;
    // 1) models already confirmed: draw all their instances
    for (size_t gi = 0; gi < pk->groups.size(); gi++) {
        const Group& g = pk->groups[gi];
        if (g_mstate[g.model & 4095] != 1) continue;
        const uint8_t mk = g_bucketMask[g.model & 4095]; const bool lite = mk && !(mk & (1u << bucket));      // bucket where the model has not drawn so far: one cheap attempt only
        if (lite && g_goodDir >= 0) {      // only every 64th frame: uploading and drawing for a bucket the model has no meshes in is wasted work
            if (((g_renderFrame + g.model) & 63) != 0) continue;
            if (!UploadInstances(&pk->data[(size_t)g.first * 16], g.count)) { g_dbgUploadFail++; return; }
            int dr = SampleAttempt(g.model, g_goodDir, bucket, g_goodDist);
            if (dr > 0) { g_frameDraws[g.model & 4095] += dr; g_bucketMask[g.model & 4095] |= (uint8_t)(1u << bucket); }
            continue;
        }
        if (!UploadInstances(&pk->data[(size_t)g.first * 16], g.count)) { g_dbgUploadFail++; return; }
        int order[6], n = 0; if (g_goodDir >= 0) order[n++] = g_goodDir; for (int c = 0; c < 6; c++) if (c != g_goodDir) order[n++] = c;
        int draws = 0;
        {   // the sample must sit where the game does not cull it: first try other distances along the calibrated direction (steep downward views cull the usual 4 m)
            static const float dists[5] = { 4.0f, 2.5f, 1.5f, 7.0f, 10.0f }; int first = 0; float gd = g_goodDist;
            for (int k = 0; k < 5 && draws <= 0; k++) { float d = k == 0 ? gd : dists[k - 1 < 0 ? 0 : k - 1]; if (k > 0 && d == gd) continue; draws = SampleAttempt(g.model, order[0], bucket, d); if (draws > 0) g_goodDist = d; (void)first; }
        }
        for (int a = 0; a < n && draws <= 0; a++) { draws = SampleAttempt(g.model, order[a], bucket); if (draws > 0 && g_goodDir != order[a]) { g_goodDir = order[a]; g_goodDirPub = g_goodDir; Log("sample direction re-calibrated to %d", g_goodDir); } }
        if (draws > 0) { g_frameDraws[g.model & 4095] += draws; g_bucketMask[g.model & 4095] |= (uint8_t)(1u << bucket); }
        else if (draws < 0) { g_mstate[g.model & 4095] = 2; g_failFrame[g.model & 4095] = (LONG)g_renderFrame; Log("model %d (%s): a mesh cannot be instanced -> back to game entities", g.model, g_mname[g.model & 4095]); continue; }
        if (logged < 30 && draws > 0) { logged++; Log("run: bucket=%d model=%d count=%u draws=%d goodDir=%d", bucket, g.model, g.count, draws, g_goodDir); }
    }
    // 2) probe models still in the unknown state (a few per frame)
    static size_t rr = 0; std::vector<int> unknown;
    { std::lock_guard<std::mutex> l(g_instMx); for (int m : g_seenModels) if (g_mstate[m] == 0) unknown.push_back(m); }
    if (!unknown.empty()) {
        if (!UploadInstances(g_zeroInst, 1)) return;
        for (int k = 0; k < 4 && k < (int)unknown.size(); k++) {
            int m = unknown[(rr++) % unknown.size()];
            int dir = g_goodDir >= 0 ? g_goodDir : (g_mtries[m] % 6);
            int draws = SampleAttempt(m, dir, bucket);
            if (g_batch.failedDips && draws > 0) draws = 0;      // a model is only confirmed when ALL its meshes instance cleanly
            if (draws > 0) { g_mstate[m] = 1; g_mtries[m] = 0; if (g_goodDir < 0) { g_goodDir = dir; g_goodDirPub = dir; } Log("model %d (%s): probe OK (bucket %d, dir %d) -> instancing enabled", m, g_mname[m], bucket, dir); }
            else if (++g_mtries[m] > 6000) { g_mstate[m] = 2; g_failFrame[m] = (LONG)g_renderFrame; Log("model %d (%s): probe failed (draws=%d, tries=%d, resident=%d) -> staying on game entities", m, g_mname[m], draws, g_mtries[m], (int)ModelResident(m)); }
        }
    }
}

static void GpuStamp(GpuSlot& s)
{
    if (s.n >= 8) return;
    if (!s.q[s.n] && FAILED(g_dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &s.q[s.n]))) { s.q[s.n] = nullptr; return; }
    s.q[s.n]->Issue(D3DISSUE_END); s.n++;
}
static void TimedRun(Packet* pk, int B)
{
    if (!cfg.profile) { g_inRun = true; RunGroups(pk, B); g_inRun = false; return; }
    GpuSlot& s = g_gs[g_renderFrame & 3];
    bool gpu = s.n <= 6; if (gpu) GpuStamp(s);
    if (gpu && (s.n & 1) == 0) gpu = false;      // begin stamp failed
    LONGLONG t0 = Qpc();
    g_inRun = true; RunGroups(pk, B); g_inRun = false;
    g_tRun += Qpc() - t0;
    if (gpu) { GpuStamp(s); if (s.n & 1) s.n--; }
}
// once per rendered frame (render thread): frame time, GPU readback, periodic report
static void PerfFrame()
{
    static LONGLONG last = 0, winStart = 0, modeSince = 0; static std::vector<float> ft; static int dipFrames = 0; static int lastMode = -1;
    LONGLONG now = Qpc();
    static int lastCull = -1; if (cfg.frustumCull != lastCull) { lastCull = cfg.frustumCull; lastMode = -1; }
    if (g_mode != lastMode) {      // new mode: drop the partial window so modes never mix
        lastMode = g_mode; modeSince = now; ft.clear(); winStart = now; dipFrames = 0; g_tRun = 0; g_dipAll = g_dipOurs = 0; g_instDrawn = 0; g_gpuMs = 0; g_gpuFrames = 0;
        InterlockedExchange(&g_nUpd, 0); InterlockedExchange64(&g_tGameList, 0); InterlockedExchange64(&g_tPacket, 0); InterlockedExchange64(&g_tCand, 0); InterlockedExchange(&g_nCand, 0); InterlockedExchange(&g_nEnt, 0);
        std::lock_guard<std::mutex> l(g_ovlMx); snprintf(g_ovl[1], sizeof g_ovl[1], "medindo...");
    }
    { std::lock_guard<std::mutex> l(g_ovlMx); snprintf(g_ovl[0], sizeof g_ovl[0], "Proper Grass  |  modo: %s  |  culling: %s  |  %.0f s neste modo%s", kModeName[g_mode], cfg.frustumCull ? "on" : "off", Ms(now - modeSince) / 1000.0, Ms(now - modeSince) < 6000.0 ? "  (aguarde, regenerando)" : ""); }
    if (last) ft.push_back((float)Ms(now - last));
    last = now; if (!winStart) winStart = now;
    dipFrames++;
    if (!g_freqQ) { if (SUCCEEDED(g_dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &g_freqQ))) g_freqQ->Issue(D3DISSUE_END); }
    else if (!g_gpuFreq) { UINT64 f = 0; if (g_freqQ->GetData(&f, sizeof f, 0) == S_OK) g_gpuFreq = f; }
    GpuSlot& rs = g_gs[(g_renderFrame + 1) & 3];      // the slot the next frame reuses (written 3 frames ago)
    if (rs.n >= 2 && g_gpuFreq) {
        UINT64 acc = 0; bool ok = true;
        for (int i = 0; i + 1 < rs.n && ok; i += 2) { UINT64 a = 0, b = 0; ok = rs.q[i]->GetData(&a, sizeof a, 0) == S_OK && rs.q[i + 1]->GetData(&b, sizeof b, 0) == S_OK; if (ok && b > a) acc += b - a; }
        if (ok) { g_gpuMs += acc * 1000.0 / (double)g_gpuFreq; g_gpuFrames++; }
    }
    rs.n = 0;
    if (Ms(now - winStart) < 2000.0 || ft.size() < 10) return;
    std::vector<float> v = ft; std::sort(v.begin(), v.end());
    double sum = 0; for (float x : v) sum += x;
    double avg = sum / v.size(), p99 = v[(size_t)(v.size() * 0.99)], mx = v.back();
    LONG nU = InterlockedExchange(&g_nUpd, 0); if (!nU) nU = 1;
    double tl = Ms(InterlockedExchange64(&g_tGameList, 0)) / nU, tp = Ms(InterlockedExchange64(&g_tPacket, 0)) / nU, tc = Ms(InterlockedExchange64(&g_tCand, 0)) / nU;
    LONG nc = InterlockedExchange(&g_nCand, 0), ne = InterlockedExchange(&g_nEnt, 0);
    double secs = Ms(now - winStart) / 1000.0; int nf = dipFrames;
    Log("[perf] %s cull=%d presents=%ld/%ld | frames=%d fps=%.1f frametime avg=%.2f p99=%.2f max=%.2f ms | render: grass cpu=%.3f ms gpu=%s%.3f ms, draws/frame=%.0f (grass %.1f, %.0f inst) | update: game render list=%.3f ms, grass packet=%.3f ms, plant candidates=%.3f ms (%.0f/s, %.0f new game entities/s)",
        kModeName[g_mode], cfg.frustumCull, g_presDev, g_presSc, nf, v.size() / (sum / 1000.0), avg, p99, mx, Ms(g_tRun) / nf, g_gpuFrames ? "" : "n/a ", g_gpuFrames ? g_gpuMs / g_gpuFrames : 0.0, (double)g_dipAll / nf, (double)g_dipOurs / nf, (double)g_instDrawn / nf, tl, tp, tc, nc / secs, ne / secs);
    { std::lock_guard<std::mutex> l(g_ovlMx); snprintf(g_ovl[1], sizeof g_ovl[1], "FPS %.0f (1%% pior %.0f)  |  grama: GPU %s%.2f ms, CPU %.2f ms, %.0f plantas  |  draws/frame %.0f  |  update %.2f ms, %.0f objetos novos/s",
        v.size() / (sum / 1000.0), 1000.0 / p99, g_gpuFrames ? "" : "n/d ", g_gpuFrames ? g_gpuMs / g_gpuFrames : 0.0, Ms(g_tRun) / nf, (double)g_instDrawn / nf, (double)g_dipAll / nf, tl + tp + tc, ne / secs); }
    ft.clear(); winStart = now; dipFrames = 0; g_tRun = 0; g_dipAll = g_dipOurs = 0; g_instDrawn = 0; g_gpuMs = 0; g_gpuFrames = 0;
}

static void __fastcall hEntDraw(void* dc)
{
    const uint8_t* b = (const uint8_t*)dc;
    {   // b33 of fully visible real vegetation commands (b32 is the entity's fade alpha and is never copied)
        unsigned model = *(uint16_t*)(b + 0x30);
        if (model < 4096 && g_vegSeen[model] && b[0x32] == 255) {
            static uint8_t ring[64]; static unsigned rn = 0;
            ring[rn++ & 63] = b[0x33];
            if (rn >= 64 && (rn & 63) == 0) {
                int cnt[256] = {0}; int bi = g_ambB33;
                for (unsigned i = 0; i < 64; i++) cnt[ring[i]]++;
                for (int k = 0; k < 256; k++) if (cnt[k] > cnt[bi]) bi = k;
                if (cnt[bi] >= 40) g_ambB33 = (uint8_t)bi;   // clear majority only: no flicker between values
            }
        }
    }
    oEntDraw(dc);
    if (g_inRun || !g_curPacket || g_mode != 0) return;
    // Draw all grass at the first real entity command of the deferred G-buffer pass. The end-of-list marker is too
    // late: by then the depth used by lighting / upscalers is already final, and grass drawn there looks smeared and
    // translucent (that happened whenever the camera looked down and few entities were on screen).
    if ((g_ranMask & 0x9u) == 0x9u) return;
    if (!EnsureDevice()) return;
    IDirect3DSurface9* rt1 = nullptr; g_dev->GetRenderTarget(1, &rt1);
    if (!rt1) return;                       // not the deferred G-buffer pass
    rt1->Release();
    static const int order[2] = { 0, 3 };
    for (int k = 0; k < 2; k++) {
        int B = order[k]; if (g_ranMask & (1u << B)) continue;
        g_ranMask |= (1u << B);
        TimedRun(g_curPacket, B);
    }
}

static void __fastcall CbDraw(CbDc* dc, void*)
{
    if (dc->kind == 2) {
        // fallback: buckets that had no entity command of their own this frame still need their instances drawn
        if (g_curPacket && cfg.enabled && g_mode == 0 && !g_inRun && EnsureDevice()) {
            IDirect3DSurface9* rt1 = nullptr; g_dev->GetRenderTarget(1, &rt1);
            if (!rt1) g_fbSkip++;
            if (rt1) {
                rt1->Release();
                static const int fb[2] = { 0, 3 };
                for (int k = 0; k < 2; k++) if (!(g_ranMask & (1u << fb[k]))) { g_ranMask |= (1u << fb[k]); TimedRun(g_curPacket, fb[k]); g_fbRuns++; }
            }
        }
        // evaluate the previous frame: confirmed models that never drew are counted as failures
        if (g_curPacket && (g_ranMask != 0)) {
            for (const Group& g : g_curPacket->groups) {
                int m = g.model & 4095;
                if (g_mstate[m] == 1) { if (g_frameDraws[m] > 0) g_mtries[m] = 0; else if (++g_mtries[m] > 600) { g_mstate[m] = 0; g_mtries[m] = 0; Log("model %d: lost (no draws for 600 frames) -> back to probing", m); } }
            }
        }
        if (cfg.debug && g_curPacket) {      // frames that had instanced plants to draw but drew nothing
            bool have = false; long total = 0;
            for (const Group& g : g_curPacket->groups) if (g_mstate[g.model & 4095] == 1 && g.count > 0) { have = true; total += g_frameDraws[g.model & 4095]; }
            static DWORD lastMiss = 0; static int missCount = 0; DWORD now = GetTickCount();
            if (have && total == 0) { missCount++; if (now - lastMiss > 700 && missCount < 400) { lastMiss = now; const float* c = (const float*)A(G::CamPos); const float* r = (const float*)A(0x128E310);
                Log("[no-draw frame] attempts=%ld anyDip=%ld failed=%ld uploadFail=%ld groups=%zu inst=%zu ranMask=%02x cam=%.1f,%.1f,%.1f r0=%.2f,%.2f,%.2f r1=%.2f,%.2f,%.2f r2=%.2f,%.2f,%.2f missFrames=%d", g_dbgAttempts, g_dbgAnyDip, g_dbgFailed, g_dbgUploadFail, g_curPacket->groups.size(), g_curPacket->data.size() / 16, g_ranMask, c[0], c[1], c[2], r[0], r[1], r[2], r[4], r[5], r[6], r[8], r[9], r[10], missCount); } }
        }
        if (cfg.debug) { g_dbgAttempts = g_dbgAnyDip = g_dbgFailed = g_dbgUploadFail = 0; long t = 0; for (int i = 0; i < 4096; i++) t += g_frameDraws[i]; g_lastDraws = t; if (t > 0) g_lastDrawFrames++; }
        if (cfg.profile && EnsureDevice()) PerfFrame();
        memset(g_frameDraws, 0, sizeof g_frameDraws);
        g_ranMask = 0; g_curPacket = dc->pk; g_renderFrame++;
        if ((g_renderFrame % 600) == 0) memset(g_bucketMask, 0, sizeof g_bucketMask);     // relearn, a model may have meshes in several buckets
    }
}

typedef void(__fastcall* GBuild_t)(void*);
static GBuild_t oGBuild;
static DWORD WINAPI BeepThread(LPVOID p) { int n = (int)(intptr_t)p; for (int i = 0; i < n; i++) { Beep(n == 2 ? 520 : 900, 110); Sleep(70); } return 0; }
static DWORD WINAPI ToneThread(LPVOID p) { Beep((DWORD)(intptr_t)p, 150); return 0; }
static bool GameIsForeground() { DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid); return pid == GetCurrentProcessId(); }
static void __fastcall hGBuild(void* self)
{
    if (cfg.toggleKey) {      // hotkey: MOD -> VANILLA -> NO-INSTANCING -> MOD (1/2/3 beeps)
        static bool down = false; bool d = (GetAsyncKeyState(cfg.toggleKey) & 0x8000) != 0;
        if (d && !down && GameIsForeground()) g_modeReq = (g_mode + 1) % 3;
        down = d;
    }
    if (cfg.cullKey) {        // hotkey: view-cone culling on/off (high beep = on, low beep = off)
        static bool down = false; bool d = (GetAsyncKeyState(cfg.cullKey) & 0x8000) != 0;
        if (d && !down && GameIsForeground()) { cfg.frustumCull = !cfg.frustumCull; Log("[cull] %s", cfg.frustumCull ? "on" : "off"); HANDLE h = CreateThread(NULL, 0, ToneThread, (LPVOID)(intptr_t)(cfg.frustumCull ? 1200 : 400), 0, NULL); if (h) CloseHandle(h); }
        down = d;
    }
    if (g_modeReq != g_mode) {
        g_mode = g_modeReq;
        ((void(__cdecl*)())A(0xC874C0))();      // release every plant triangle (as the game does on teleports): everything regenerates under the new mode
        { std::lock_guard<std::mutex> l(g_instMx); g_byTri.clear(); g_edgeCnt.clear(); g_triEdges.clear(); }
        Log("[mode] %s", kModeName[g_mode]);
        HANDLE h = CreateThread(NULL, 0, BeepThread, (LPVOID)(intptr_t)(g_mode + 1), 0, NULL); if (h) CloseHandle(h);
    }
    LONGLONG t0 = cfg.profile ? Qpc() : 0;
    oGBuild(self);
    LONGLONG t1 = cfg.profile ? Qpc() : 0;
    static bool inited = false;
    if (!inited) { inited = true; g_cbVt[0] = (void*)&CbDtor; g_cbVt[1] = (void*)&CbDraw; g_cbVt[2] = (void*)&CbSize; }
    LONG f = InterlockedIncrement(&g_frameNo);
    if (cfg.radiusMul != 1.f) {
        float* mgr = (float*)A(G::PlantMgr);
        float detail = *(float*)A(0x103F714);
        float R = (detail * 0.5f + 20.f) * (g_mode == 1 ? 1.f : cfg.radiusMul);
        mgr[5] = R; mgr[7] = R + 40.f; mgr[8] = R * R;
    }
    Packet& pk = g_packets[f & 3];
    BuildPacket(pk);
    if (cfg.profile) { LONGLONG t2 = Qpc(); InterlockedExchangeAdd64(&g_tGameList, t1 - t0); InterlockedExchangeAdd64(&g_tPacket, t2 - t1); InterlockedIncrement(&g_nUpd); }
    if (cfg.debug && g_logFrames < 3 && !pk.groups.empty()) { Log("frame %ld: %zu groups, %zu instances (cand=%ld suppressed=%ld)", f, pk.groups.size(), pk.data.size() / 16, g_candCount, g_candSuppressed); g_logFrames++; }
    if (cfg.debug) {
        static DWORD last = 0; DWORD now = GetTickCount();
        if (now - last > 2000) {
            last = now; size_t tris; { std::lock_guard<std::mutex> l(g_instMx); tris = g_byTri.size(); }
            const float* cam = (const float*)A(G::CamPos);
            const float* rr = (const float*)A(0x128E310);
            Log("[stat] amb=%u,%u cam=%.0f,%.0f,%.0f pitchZ=%.2f tris=%zu groups=%zu inst=%zu | cand=%ld supp=%ld | fallbackRuns=%ld lastFrameDraws=%ld framesWithDraws=%ld/%ld", (unsigned)g_ambB32, (unsigned)g_ambB33, cam[0], cam[1], cam[2], rr[6], tris, pk.groups.size(), pk.data.size() / 16, g_candCount, g_candSuppressed, g_fbRuns, (long)g_lastDraws, (long)g_lastDrawFrames, (long)g_renderFrame);
            { std::string gs; char t[64]; for (const Group& g : pk.groups) { sprintf(t, " %d:%d/%d f=%.2f", g.model, g.count, g_preCnt[g.model], g.count ? g_fSum[g.model] / g.count : 0.f); gs += t; } Log("[groups]%s", gs.c_str()); }
        }
    }
    {   // models that failed their instancing probe are retried every ~30 s of rendering
        static DWORD lastRetry = 0; DWORD now = GetTickCount();
        if (now - lastRetry > 5000) { lastRetry = now; for (int m = 0; m < 4096; m++) if (g_mstate[m] == 2 && (LONG)g_renderFrame - g_failFrame[m] > 1800) { g_mstate[m] = 0; g_mtries[m] = 0; } }
    }
    AppendMarker(&pk);
}

// ---------------------------------------------------------------- init
// ===================== material / procedural-tag diagnostics (and, later, grass-material enabling) =====================
typedef uint32_t(__attribute__((thiscall)) * BoundFn1)(void*, uint32_t);
static std::vector<std::string> g_matNames;
static void LoadMaterialNames(const std::string& path)
{
    FILE* f = fopen(path.c_str(), "r"); if (!f) { Log("materials.dat not found"); return; }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char* p = line; while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\r' || *p == '\n' || !*p) continue;
        char name[96]; if (sscanf(p, "%95s", name) != 1) continue;
        if (isdigit((unsigned char)name[0]) || name[0] == '-') continue;      // version line
        g_matNames.push_back(name);
    }
    fclose(f); Log("materials.dat: %zu materials", g_matNames.size());
}
// ---------------------------------------------------------------- grass-material enabling
// The game bakes a "procedural tag" into every collision polygon.  On PC the tag of almost all grass materials was left empty,
// so lawns, medians and parks generate no plants.  We give such polygons a vegetation tag (set per material in the INI).
static bool g_grassMat[256];
static std::string g_matTagName[256]; static int g_matTagNum[256];
static volatile LONG g_flagTag[256][256];       // flagged polys: material -> tag histogram (fallback when the INI gives no tag)
static int GrassTagFor(int mat)
{
    if (g_matTagNum[mat] > 0) return g_matTagNum[mat];
    if (!g_matTagName[mat].empty()) { auto it = g_tagNum.find(g_matTagName[mat]); if (it != g_tagNum.end()) { g_matTagNum[mat] = it->second; return it->second; } return 0; }
    if (cfg.grassTag > 0) return cfg.grassTag;
    LONG best = 0; int bt = 0; for (int t = 1; t < 256; t++) if (g_flagTag[mat][t] > best) { best = g_flagTag[mat][t]; bt = t; }
    return best >= 20 ? bt : 0;
}
static inline void Words(void* bound, uint32_t poly, uint32_t* w)
{
    BoundFn1 f0 = *(BoundFn1*)(*(uintptr_t*)bound + 0xD4);
    BoundFn1 f1 = *(BoundFn1*)(*(uintptr_t*)bound + 0x5C);
    *w = f1(bound, f0(bound, poly));
}
// called from the patch inside the game's polygon scan (see the asm below); returns the "has procedural object" flag
extern "C" int __cdecl GrassPoly(void* bound, int poly, int b11, int b12, uintptr_t ebpv)
{
    (void)ebpv;
    uint32_t w; Words(bound, (uint32_t)poly, &w);
    int mat = w & 0xff, tag = (w >> 8) & 0xff;
    if (b11 || b12) { if (b12 && tag) InterlockedIncrement(&g_flagTag[mat][tag]); return b12; }
    if (g_mode != 1 && tag == 0 && g_grassMat[mat] && GrassTagFor(mat)) return 1;
    return b12;
}
extern "C" uintptr_t g_caveRet;
uintptr_t g_caveRet;
asm(R"(
.text
.intel_syntax noprefix
.globl _grassCave
_grassCave:
    pushfd
    pushad
    movzx eax, al
    push ebp
    push eax
    movzx edx, byte ptr [esp+0x20+36+8]
    push edx
    push dword ptr [esp+0x5c+36+12]
    push dword ptr [esp+0x2c+36+16]
    call _GrassPoly
    add esp, 20
    mov byte ptr [esp+28], al
    popad
    popfd
    cmp byte ptr [esp+0x20], 0
    jmp dword ptr [_g_caveRet]
.att_syntax prefix
)");
extern "C" void grassCave();
static void InstallGrassCave()
{
    BYTE* t = (BYTE*)A(0xC87E27);
    static const BYTE expect[5] = { 0x80, 0x7C, 0x24, 0x20, 0x00 };
    if (memcmp(t, expect, 5)) { Log("grass cave: unexpected bytes at 0xC87E27, not installed"); return; }
    g_caveRet = A(0xC87E2C);
    DWORD old; VirtualProtect(t, 5, PAGE_EXECUTE_READWRITE, &old);
    t[0] = 0xE9; *(int32_t*)(t + 1) = (int32_t)((uintptr_t)&grassCave - (uintptr_t)(t + 5));
    VirtualProtect(t, 5, old, &old); FlushInstructionCache(GetCurrentProcess(), t, 5);
}

// a collision object is only considered by the plant system if one of its materials carries a tag: let grass materials count too
typedef char(__fastcall* BoundGate_t)(void*, void*, void*);
static BoundGate_t oBoundGate;
static char __fastcall hBoundGate(void* self, void* edx, void* bound)
{
    char r = oBoundGate(self, edx, bound);
    if (r || g_mode == 1) return r;
    typedef int(__attribute__((thiscall)) * Cnt_t)(void*);
    BoundFn1 fcount = *(BoundFn1*)(*(uintptr_t*)bound + 0x54);
    int n = ((Cnt_t)fcount)(bound);
    BoundFn1 f1 = *(BoundFn1*)(*(uintptr_t*)bound + 0x5C);
    for (int i = 0; i < n; i++) { uint32_t w = f1(bound, (uint32_t)i); if (((w >> 8) & 0xff) == 0 && g_grassMat[w & 0xff]) return 1; }
    return 0;
}
// triangle creation: write the vegetation tag into the polygon's material word
typedef void(__fastcall* TriBuild_t)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
static TriBuild_t oTriBuild;
static void __fastcall hTriBuild(void* mgr, void* edx, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7, uint32_t a8, uint32_t a9, uint32_t a10, uint32_t a11)
{
    int mat = a11 & 0xff;
    if (g_mode != 1 && ((a11 >> 8) & 0xff) == 0 && g_grassMat[mat]) { int t = GrassTagFor(mat); if (t) a11 |= (uint32_t)t << 8; }
    oTriBuild(mgr, edx, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11);
}
// triangle created: remember its edges (an edge used by one grass triangle only borders another surface)
typedef float*(__fastcall* TriNew_t)(float*, void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
static TriNew_t oTriNew;
static float* __fastcall hTriNew(float* tri, void* edx, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7, uint32_t a8)
{
    float* r = oTriNew(tri, edx, a1, a2, a3, a4, a5, a6, a7, a8);
    if (r && g_grassMat[a4 & 0xff]) {
        TriEdges te; const float* v[3] = { tri, tri + 4, tri + 8 };
        uint64_t q[3]; for (int i = 0; i < 3; i++) q[i] = QPt(v[i][0], v[i][1]);
        for (int e = 0; e < 3; e++) { int a = e, b = (e + 1) % 3; te.key[e] = EdgeKey(q[a], q[b]); te.p[e][0] = v[a][0]; te.p[e][1] = v[a][1]; te.p[e][2] = v[b][0]; te.p[e][3] = v[b][1]; }
        std::lock_guard<std::mutex> l(g_instMx);
        EdgesRemove((uintptr_t)tri);
        for (int e = 0; e < 3; e++) g_edgeCnt[te.key[e]]++;
        g_triEdges[(uintptr_t)tri] = te;
    }
    return r;
}
static void InstallGrassHooks()
{
    memset(g_grassMat, 0, sizeof g_grassMat);
    const std::string& list = cfg.grassMaterials; size_t p = 0;      // "NAME[@TAG],NAME[@TAG],..."
    while (p <= list.size()) {
        size_t q = list.find(',', p); if (q == std::string::npos) q = list.size();
        std::string nm = list.substr(p, q - p); std::string tg; { size_t at = nm.find('@'); if (at != std::string::npos) { tg = nm.substr(at + 1); nm = nm.substr(0, at); } }
        auto trim = [](std::string& x) { while (!x.empty() && isspace((unsigned char)x.back())) x.pop_back(); while (!x.empty() && isspace((unsigned char)x[0])) x.erase(0, 1); };
        trim(nm); trim(tg);
        for (size_t i = 0; i < g_matNames.size() && i < 256; i++) if (!nm.empty() && !_stricmp(g_matNames[i].c_str(), nm.c_str())) {
            g_grassMat[i] = true; if (!tg.empty()) { if (isdigit((unsigned char)tg[0])) g_matTagNum[i] = atoi(tg.c_str()); else g_matTagName[i] = tg; }
            Log("grass material enabled: %s (id %zu) tag=%s", nm.c_str(), i, tg.empty() ? "auto" : tg.c_str());
        }
        p = q + 1;
    }
    if (*(BYTE*)A(0xC86C80) != 0x53 || *(BYTE*)A(0xC87F40) != 0x55) { Log("grass hooks: unexpected prologue, not installed"); return; }
    oBoundGate = (BoundGate_t)MakeHook(A(0xC86C80), 8, (void*)&hBoundGate);
    oTriBuild = (TriBuild_t)MakeHook(A(0xC87F40), 12, (void*)&hTriBuild);
    if (*(BYTE*)A(0xC86540) == 0x55) oTriNew = (TriNew_t)MakeHook(A(0xC86540), 9, (void*)&hTriNew); else Log("tri-new prologue mismatch, edge culling off");
}

static DWORD WINAPI InitThread(LPVOID)
{
    g_base = (uintptr_t)GetModuleHandleA(NULL);
    char path[MAX_PATH]; GetModuleFileNameA(NULL, path, MAX_PATH);
    char* sl = strrchr(path, '\\'); *sl = 0;
    std::string dir = path;
    g_log = fopen((dir + "\\plugins\\ProperGrass.log").c_str(), "w");
    std::string ini = dir + "\\plugins\\ProperGrass.ini";
    cfg.enabled = GetPrivateProfileIntA("Main", "Enabled", 1, ini.c_str());
    cfg.triCapacity = GetPrivateProfileIntA("Main", "TriCapacity", 16384, ini.c_str());
    cfg.maxInstances = GetPrivateProfileIntA("Main", "MaxInstances", 60000, ini.c_str());
    cfg.debug = GetPrivateProfileIntA("Main", "Debug", 0, ini.c_str());
    char buf[32]; GetPrivateProfileStringA("Main", "DrawDistance", "0", buf, 32, ini.c_str()); cfg.drawDistance = (float)atof(buf);
    GetPrivateProfileStringA("Main", "DensityMultiplier", "1.0", buf, 32, ini.c_str()); cfg.densityMul = (float)atof(buf);
    cfg.grassTag = GetPrivateProfileIntA("Main", "GrassTag", 0, ini.c_str());
    GetPrivateProfileStringA("Main", "EdgeMargin", "0.35", buf, 32, ini.c_str()); cfg.edgeMargin = (float)atof(buf);
    GetPrivateProfileStringA("Main", "NormalTilt", "0", buf, 32, ini.c_str()); cfg.normalTilt = (float)atof(buf);
    GetPrivateProfileStringA("Main", "EdgeSoft", "1.0", buf, 32, ini.c_str()); cfg.edgeSoft = (float)atof(buf);
    GetPrivateProfileStringA("Main", "FadeWidth", "12", buf, 32, ini.c_str()); cfg.fadeWidth = (float)atof(buf);
    { char b[512]; GetPrivateProfileStringA("Main", "GrassMaterials", cfg.grassMaterials.c_str(), b, sizeof b, ini.c_str()); cfg.grassMaterials = b;
    }
    GetPrivateProfileStringA("Main", "RadiusMultiplier", "1.5", buf, 32, ini.c_str()); cfg.radiusMul = (float)atof(buf);
    cfg.profile = GetPrivateProfileIntA("Main", "Profile", 0, ini.c_str());
    cfg.frustumCull = GetPrivateProfileIntA("Main", "FrustumCull", 1, ini.c_str());
    GetPrivateProfileStringA("Main", "CullKey", "0", buf, 32, ini.c_str()); cfg.cullKey = (int)strtol(buf, nullptr, 0);
    GetPrivateProfileStringA("Main", "ToggleKey", "0", buf, 32, ini.c_str()); cfg.toggleKey = (int)strtol(buf, nullptr, 0);
    QueryPerformanceFrequency(&g_qpf);
    Log("ProperGrass " PROPERGRASS_VERSION " starting (enabled=%d tri=%d maxInst=%d density=%.2f radius=%.2f)", cfg.enabled, cfg.triCapacity, cfg.maxInstances, cfg.densityMul, cfg.radiusMul);
    if (!cfg.enabled) return 0;

    // identity check
    {
        auto chk = [&](const char* n, bool ok) { Log("check %-12s %s", n, ok ? "ok" : "FAIL"); return ok; };
        Log("base=%08x delta=%08x", (unsigned)g_base, (unsigned)(g_base - 0x400000));
        bool ok = true;
        ok &= chk("thunk", *(BYTE*)A(0xC86C10) == 0xB9);
        ok &= chk("plantmgr", *(uint32_t*)A(0xC86C11) == A(G::PlantMgr));
        ok &= chk("capimm", *(uint32_t*)A(G::TriCapImm) == 0x200);
        ok &= chk("gbuild", *(BYTE*)A(0xD685F0) == 0x51 || *(BYTE*)A(0xD685F0) == 0xE9);
        ok &= chk("candadd", *(BYTE*)A(G::CandidateAdd) == 0x55);
        ok &= chk("reset", *(BYTE*)A(0xC87450) == 0xB9);
        if (!ok) { Log("unsupported executable (signature mismatch) - disabled"); return 0; }
    }
    LoadMaterialNames(dir + "\\common\\data\\materials\\materials.dat");
    ParseProcedural((dir + "\\common\\data\\materials\\procedural.dat").c_str());
    if (!g_itemsValid) return 0;

    // raise the active-triangle capacity (must happen before the manager initialises)
    { DWORD old; BYTE* p = (BYTE*)A(G::TriCapImm); VirtualProtect(p, 4, PAGE_EXECUTE_READWRITE, &old); *(uint32_t*)p = (uint32_t)cfg.triCapacity; VirtualProtect(p, 4, old, &old); }

    InstallGrassCave();
    InstallGrassHooks();
    oCandAdd = (CandAddFn)MakeHook(A(G::CandidateAdd), 8, (void*)&hCandAdd);
    oTriRelease = (TriReleaseFn)MakeHook(A(G::TriRelease), 7, (void*)&hTriRelease);
    oGBuild = (GBuild_t)MakeHook(A(G::GBufferBuild), 11, (void*)&hGBuild);
    oReset = (ResetFn)MakeHook(A(0xC87450), 5, (void*)&hReset2);
    oEntDraw = (EntDraw_t)MakeHook(A(0x8DCBF0), 6, (void*)&hEntDraw);
    Log("hooks installed: cand=%p release=%p gbuild=%p", oCandAdd, oTriRelease, oGBuild);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) { HANDLE h = CreateThread(NULL, 0, InitThread, NULL, 0, NULL); if (h) CloseHandle(h); }
    return TRUE;
}
