// mali-shadercache: compile AIR with libair2valhall on the host and write an MVP1 pack for the plug-in.
//
//   mali-shadercache -o OUT.mvpack [--metallib FILE]... [--air DIR]... [--merge OLD.mvpack]
//
// --air DIR takes <key>.air + <key>.txt pairs dumped by the plug-in on a cache miss.
#include <dirent.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "air2vh_api.h"
#include "metallib.h"
#include "mvpack.h"

namespace {

struct Request {
    std::string label;
    std::vector<uint8_t> air;
    std::string entry;
    uint32_t stage = AIR2VH_STAGE_ANY;
    uint32_t flags = 0;
    std::vector<air2vh_constant> constants;
    std::vector<std::string> constant_names;
    std::string expect_key;
};

struct Entry {
    mvp_index index{};
    std::vector<uint8_t> record;
};

bool ReadFile(const std::string& path, std::vector<uint8_t>* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

uint32_t StageOf(uint8_t mtl_type) {
    switch (mtl_type) {
        case 0: return AIR2VH_STAGE_VERTEX;
        case 1: return AIR2VH_STAGE_FRAGMENT;
        case 2: return AIR2VH_STAGE_KERNEL;
        default: return AIR2VH_STAGE_ANY;
    }
}

bool AddMetallib(const std::string& path, std::vector<Request>* reqs) {
    MtlLibrary lib;
    std::string err;
    if (!metallib_load(path.c_str(), &lib, &err)) {
        fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str());
        return false;
    }
    // Per-function bitcode, not the container: libair2valhall's MTLB walker rejects Xcode 26
    // metallibs whose function-list size excludes the count word.
    for (const MtlFunction& f : lib.functions) {
        Request r;
        r.label = path + ":" + f.name;
        r.air.assign(lib.bitcode(f), lib.bitcode(f) + f.bc_size);
        r.entry = f.name;
        r.stage = StageOf(f.type);
        reqs->push_back(std::move(r));
    }
    return true;
}

bool ParseMeta(const std::string& path, Request* r) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "entry") r->entry = v;
        else if (k == "stage") r->stage = (uint32_t)strtoul(v.c_str(), nullptr, 0);
        else if (k == "flags") r->flags = (uint32_t)strtoul(v.c_str(), nullptr, 0);
        else if (k == "constant") {
            std::stringstream ss(v);
            std::string tok;
            std::vector<std::string> parts;
            while (std::getline(ss, tok, ',')) parts.push_back(tok);
            if (parts.size() < 3) return false;
            air2vh_constant c{};
            c.index = atoi(parts[0].c_str());
            r->constant_names.push_back(parts[1]);
            c.num_values = std::min<uint32_t>((uint32_t)strtoul(parts[2].c_str(), nullptr, 0), 4);
            for (uint32_t i = 0; i < c.num_values && 3 + i < parts.size(); i++) c.values[i] = strtod(parts[3 + i].c_str(), nullptr);
            r->constants.push_back(c);
        }
    }
    for (size_t i = 0; i < r->constants.size(); i++)
        r->constants[i].name = r->constant_names[i].empty() ? nullptr : r->constant_names[i].c_str();
    return true;
}

bool AddAirDir(const std::string& dir, std::vector<Request>* reqs) {
    DIR* d = opendir(dir.c_str());
    if (!d) {
        fprintf(stderr, "%s: cannot open\n", dir.c_str());
        return false;
    }
    while (dirent* de = readdir(d)) {
        std::string name = de->d_name;
        if (name.size() != 64 + 4 || name.compare(64, 4, ".air")) continue;
        Request r;
        r.expect_key = name.substr(0, 64);
        r.label = dir + "/" + name;
        if (!ReadFile(r.label, &r.air) || !ParseMeta(dir + "/" + r.expect_key + ".txt", &r)) {
            fprintf(stderr, "%s: missing AIR or metadata\n", r.label.c_str());
            continue;
        }
        reqs->push_back(std::move(r));
    }
    closedir(d);
    return true;
}

template <typename T>
void Append(std::vector<uint8_t>* v, const T* p, size_t n) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(p);
    v->insert(v->end(), b, b + n);
}

bool Compile(air2vh_compiler* comp, Request& req, Entry* out) {
    air2vh_compile_info info{};
    info.struct_size = sizeof(info);
    info.air = req.air.data();
    info.air_size = req.air.size();
    info.entry = req.entry.empty() ? nullptr : req.entry.c_str();
    info.stage = req.stage;
    info.flags = req.flags;
    info.num_constants = (uint32_t)req.constants.size();
    info.constants = req.constants.empty() ? nullptr : req.constants.data();

    char key[65];
    mvp_key(&info, key);
    if (!req.expect_key.empty() && req.expect_key != key) {
        fprintf(stderr, "%s: key mismatch (file %s, computed %s)\n", req.label.c_str(), req.expect_key.c_str(), key);
        return false;
    }

    air2vh_result res{};
    if (air2vh_compile(comp, &info, &res) != 0) {
        fprintf(stderr, "skip %s: %s\n", req.label.c_str(), res.error ? res.error : "compile failed");
        air2vh_result_free(&res);
        return false;
    }

    air2vh_program prog = *res.program;
    prog.vs.layout.blob = nullptr;
    const void* layout = res.vs_layout_out.blob;
    uint32_t layout_size = layout ? res.vs_layout_out.blob_size : 0;

    mvp_record rec{(uint32_t)sizeof(prog), (uint32_t)res.code_size, (uint32_t)res.sidecar_size, layout_size};
    Append(&out->record, &rec, sizeof(rec));
    Append(&out->record, &prog, sizeof(prog));
    Append(&out->record, (const uint8_t*)res.code, res.code_size);
    Append(&out->record, res.sidecar_json, res.sidecar_size);
    if (layout_size) Append(&out->record, (const uint8_t*)layout, layout_size);

    memcpy(out->index.key, key, 64);
    strncpy(out->index.entry, req.entry.c_str(), MVP_ENTRY_MAX);
    out->index.stage = res.program->stage;
    out->index.flags = req.flags;
    out->index.num_constants = info.num_constants;
    printf("ok   %s -> %.16s (%zu bytes code)\n", req.label.c_str(), key, res.code_size);
    air2vh_result_free(&res);
    return true;
}

bool LoadPack(const std::string& path, std::map<std::string, Entry>* entries) {
    std::vector<uint8_t> b;
    if (!ReadFile(path, &b) || b.size() < sizeof(mvp_header)) return false;
    mvp_header h;
    memcpy(&h, b.data(), sizeof(h));
    size_t idx = (sizeof(h) + h.id_len + 7) & ~size_t(7);
    if (memcmp(h.magic, MVP_MAGIC, 4) || h.version != MVP_VERSION || idx + (size_t)h.count * sizeof(mvp_index) > b.size())
        return false;
    for (uint32_t i = 0; i < h.count; i++) {
        Entry e;
        memcpy(&e.index, b.data() + idx + i * sizeof(mvp_index), sizeof(mvp_index));
        if (e.index.offset + e.index.length > b.size()) return false;
        e.record.assign(b.begin() + (long)e.index.offset, b.begin() + (long)(e.index.offset + e.index.length));
        (*entries)[std::string(e.index.key, 64)] = std::move(e);
    }
    return true;
}

bool WritePack(const std::string& path, std::map<std::string, Entry>& entries, const std::string& id) {
    std::vector<uint8_t> out;
    mvp_header h{};
    memcpy(h.magic, MVP_MAGIC, 4);
    h.version = MVP_VERSION;
    h.count = (uint32_t)entries.size();
    h.id_len = (uint32_t)id.size();
    Append(&out, &h, sizeof(h));
    Append(&out, id.data(), id.size());
    out.resize((out.size() + 7) & ~size_t(7));
    size_t idx = out.size();
    out.resize(idx + entries.size() * sizeof(mvp_index));
    size_t i = 0;
    for (auto& [key, e] : entries) {
        out.resize((out.size() + 7) & ~size_t(7));
        e.index.offset = out.size();
        e.index.length = e.record.size();
        Append(&out, e.record.data(), e.record.size());
        memcpy(out.data() + idx + i++ * sizeof(mvp_index), &e.index, sizeof(mvp_index));
    }
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(out.data()), (std::streamsize)out.size());
    return (bool)f;
}

}  // namespace

int main(int argc, char** argv) {
    std::string out, merge;
    std::vector<Request> reqs;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-o" && i + 1 < argc) out = argv[++i];
        else if (a == "--metallib" && i + 1 < argc) { if (!AddMetallib(argv[++i], &reqs)) return 2; }
        else if (a == "--air" && i + 1 < argc) { if (!AddAirDir(argv[++i], &reqs)) return 2; }
        else if (a == "--merge" && i + 1 < argc) merge = argv[++i];
        else {
            fprintf(stderr, "usage: %s -o OUT.mvpack [--metallib FILE]... [--air DIR]... [--merge OLD.mvpack]\n", argv[0]);
            return 2;
        }
    }
    if (out.empty()) return 2;

    std::map<std::string, Entry> entries;
    if (!merge.empty() && !LoadPack(merge, &entries)) {
        fprintf(stderr, "%s: not an MVP1 pack\n", merge.c_str());
        return 2;
    }
    char* err = nullptr;
    air2vh_compiler* comp = air2vh_compiler_create(0xb8a21020ull, 7, 0, &err);
    if (!comp) {
        fprintf(stderr, "air2vh_compiler_create: %s\n", err ? err : "failed");
        return 1;
    }
    unsigned ok = 0, failed = 0;
    for (Request& r : reqs) {
        Entry e;
        if (Compile(comp, r, &e)) {
            entries[std::string(e.index.key, 64)] = std::move(e);
            ok++;
        } else {
            failed++;
        }
    }
    std::string id = std::string("air2vh ") + air2vh_compiler_id();
    air2vh_compiler_destroy(comp);
    if (!WritePack(out, entries, id)) return 1;
    printf("%s: %zu entries (%u compiled, %u failed)\n", out.c_str(), entries.size(), ok, failed);
    return 0;
}
