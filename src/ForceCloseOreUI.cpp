#include <filesystem>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <string>
#include <vector>
#include <pthread.h>
#include <unistd.h>
#include <android/log.h>
#include <dobby.h> 

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "ForceCloseOreUI", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ForceCloseOreUI", __VA_ARGS__)

namespace fs = std::filesystem;

class OreUIConfig {
public:
    void *mUnknown1;
    void *mUnknown2;
    std::function<bool()> mUnknown3;
    std::function<bool()> mUnknown4;
};

class OreUi {
public:
    std::unordered_map<std::string, OreUIConfig> mConfigs;
};

// --- Dynamically detect package name without JNI ---
std::string getPackageName() {
    std::ifstream cmdline("/proc/self/cmdline");
    std::string pkgName;
    if (std::getline(cmdline, pkgName, '\0') && !pkgName.empty()) {
        return pkgName;
    }
    return "com.mojang.minecraftpe"; 
}

std::string getConfigDir() {
    std::string pkgName = getPackageName();
    std::string primary = "/sdcard/0/Android/data/" + pkgName + "/files/mods/ForceCloseOreUI/";
    std::error_code ec;
    fs::create_directories(primary, ec); 
    return primary;
}

nlohmann::json outputJson;
std::string dirPath = "";
std::string filePath = "";
bool updated = false;

void saveJson(const std::string &path, const nlohmann::json &j) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    FILE *f = std::fopen(path.c_str(), "w");
    if (!f) {
        LOGE("Failed to open config file for writing.");
        return;
    }
    std::string jsonStr = j.dump(4);
    std::fwrite(jsonStr.data(), 1, jsonStr.size(), f);
    std::fclose(f);
}

// --- UPDATED FOR NEW MOJANG PARAMETERS (6 Args, OreUi is first) ---
void (*orig_OreUi_init)(OreUi&, void*, void*, void*, void*, void*);

void hook_OreUi_init(OreUi &a1, void *a2, void *a3, void *a4, void *a5, void *a6) {
    // 1. Let the game initialize the UI first so it populates the config map
    orig_OreUi_init(a1, a2, a3, a4, a5, a6);

    // 2. Load our JSON and overwrite the game's values
    dirPath = getConfigDir();
    filePath = dirPath + "config.json";

    if (fs::exists(filePath)) {
        std::ifstream inFile(filePath);
        if (inFile.is_open()) {
            inFile >> outputJson;
            inFile.close();
        }
    }

    for (auto &data : a1.mConfigs) {
        bool value = false;
        if (outputJson.contains(data.first) && outputJson[data.first].is_boolean()) {
            value = outputJson[data.first];
        } else {
            outputJson[data.first] = false;
            updated = true;
        }
        data.second.mUnknown3 = [value]() { return value; };
        data.second.mUnknown4 = [value]() { return value; };
    }

    if (updated || !fs::exists(filePath)) {
        saveJson(filePath, outputJson);
    }
}

// --- THE NEWEST ARM64 SIGNATURES ---
const std::vector<const char*> OREUI_PATTERNS = {
    "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? D5 FB 03 00 AA F5 03 07 AA"
};

static uintptr_t ResolveSignature(const char* sig) {
    std::vector<int> pattern;
    const char* p = sig;
    while (*p) {
        if (*p == ' ') { p++; continue; }
        if (*p == '?') { pattern.push_back(-1); p++; if(*p=='?') p++; continue; }
        pattern.push_back(strtol(p, nullptr, 16));
        p += 2;
    }

    FILE* fp = fopen("/proc/self/maps", "r");
    if (!fp) return 0;

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        if (!strstr(line, "libminecraftpe.so") || !strstr(line, "r-x")) continue; 
        
        uintptr_t start, end;
        if (sscanf(line, "%lx-%lx", &start, &end) != 2) continue;

        uint8_t* scan_base = (uint8_t*)start;
        size_t size = end - start;
        if (size < pattern.size()) continue;

        for (size_t i = 0; i < size - pattern.size(); i++) {
            bool found = true;
            for (size_t j = 0; j < pattern.size(); j++) {
                if (pattern[j] != -1 && scan_base[i + j] != pattern[j]) {
                    found = false;
                    break;
                }
            }
            if (found) {
                fclose(fp);
                return (uintptr_t)(scan_base + i);
            }
        }
    }
    fclose(fp);
    return 0;
}

void* InjectionThread(void* arg) {
    LOGI("ForceCloseOreUI Turbo Thread started.");

    bool isLoaded = false;
    while (!isLoaded) {
        FILE* fp = fopen("/proc/self/maps", "r");
        if (fp) {
            char line[512];
            while (fgets(line, sizeof(line), fp)) {
                if (strstr(line, "libminecraftpe.so") && strstr(line, "r-x")) {
                    isLoaded = true;
                    break;
                }
            }
            fclose(fp);
        }
        if (!isLoaded) usleep(10000); 
    }

    LOGI("libminecraftpe.so mapped! Scanning memory instantly...");

    bool hookApplied = false;
    for (int attempts = 1; attempts <= 100; attempts++) {
        for (const char* sig : OREUI_PATTERNS) {
            uintptr_t addr = ResolveSignature(sig);
            if (addr != 0) {
                LOGI("SUCCESS: Found OreUI signature! Applying DobbyHook...");
                DobbyHook((void*)addr, (void*)hook_OreUi_init, (void**)&orig_OreUi_init);
                hookApplied = true;
                break;
            }
        }
        if (hookApplied) break;
        usleep(50000); 
    }

    if (!hookApplied) {
        LOGE("FATAL: Could not find OreUI pattern in memory.");
    }

    return nullptr;
}

__attribute__((constructor))
void ForceCloseOreUI_Init() {
    pthread_t thread;
    pthread_create(&thread, nullptr, InjectionThread, nullptr);
    pthread_detach(thread);
}
