#include "LibretroLoader.hpp"
#include "core/romx/RomxGameEntryAdapter.hpp"
#include "RomxVfs.hpp"
#if defined(__APPLE__) && !defined(__SWITCH__)
#include "LibretroVulkanHost.hpp"
#endif
#include <romx/romx.h>

#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <algorithm>
#include <array>
#include <fstream>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#elif defined(__PSV__) || defined(__psp2__) || defined(__SWITCH__)
// PSVita / Nintendo Switch 不支持 POSIX 动态链接器。
#else
#  include <dlfcn.h>
#endif

// ============================================================
// 静态链接核心的外部符号声明
//
// mGBA now uses the native source API. The remaining libretro cores are
// compiled with -Dretro_xxx=prefix_retro_xxx to avoid symbol collisions.
// ============================================================
extern "C" {

// ---- FCEUmm（NES）重命名符号 -------------------------------
void fceumm_retro_init(void);
void fceumm_retro_deinit(void);
unsigned fceumm_retro_api_version(void);
void fceumm_retro_get_system_info(struct retro_system_info*);
void fceumm_retro_get_system_av_info(struct retro_system_av_info*);
void fceumm_retro_set_environment(retro_environment_t);
void fceumm_retro_set_video_refresh(retro_video_refresh_t);
void fceumm_retro_set_audio_sample(retro_audio_sample_t);
void fceumm_retro_set_audio_sample_batch(retro_audio_sample_batch_t);
void fceumm_retro_set_input_poll(retro_input_poll_t);
void fceumm_retro_set_input_state(retro_input_state_t);
void fceumm_retro_set_controller_port_device(unsigned, unsigned);
void fceumm_retro_reset(void);
void fceumm_retro_run(void);
size_t fceumm_retro_serialize_size(void);
bool fceumm_retro_serialize(void*, size_t);
bool fceumm_retro_unserialize(const void*, size_t);
bool fceumm_retro_load_game(const struct retro_game_info*);
void fceumm_retro_unload_game(void);
void* fceumm_retro_get_memory_data(unsigned);
size_t fceumm_retro_get_memory_size(unsigned);
void fceumm_retro_cheat_reset(void);
void fceumm_retro_cheat_set(unsigned, bool, const char*);
unsigned fceumm_retro_get_region(void);

// ---- Nestopia（NES）重命名符号 -----------------------------
void nestopia_retro_init(void);
void nestopia_retro_deinit(void);
unsigned nestopia_retro_api_version(void);
void nestopia_retro_get_system_info(struct retro_system_info*);
void nestopia_retro_get_system_av_info(struct retro_system_av_info*);
void nestopia_retro_set_environment(retro_environment_t);
void nestopia_retro_set_video_refresh(retro_video_refresh_t);
void nestopia_retro_set_audio_sample(retro_audio_sample_t);
void nestopia_retro_set_audio_sample_batch(retro_audio_sample_batch_t);
void nestopia_retro_set_input_poll(retro_input_poll_t);
void nestopia_retro_set_input_state(retro_input_state_t);
void nestopia_retro_set_controller_port_device(unsigned, unsigned);
void nestopia_retro_reset(void);
void nestopia_retro_run(void);
size_t nestopia_retro_serialize_size(void);
bool nestopia_retro_serialize(void*, size_t);
bool nestopia_retro_unserialize(const void*, size_t);
bool nestopia_retro_load_game(const struct retro_game_info*);
void nestopia_retro_unload_game(void);
void* nestopia_retro_get_memory_data(unsigned);
size_t nestopia_retro_get_memory_size(unsigned);
void nestopia_retro_cheat_reset(void);
void nestopia_retro_cheat_set(unsigned, bool, const char*);
unsigned nestopia_retro_get_region(void);

// ---- Gambatte（GB/GBC）重命名符号 -------------------------
void gambatte_retro_init(void);
void gambatte_retro_deinit(void);
unsigned gambatte_retro_api_version(void);
void gambatte_retro_get_system_info(struct retro_system_info*);
void gambatte_retro_get_system_av_info(struct retro_system_av_info*);
void gambatte_retro_set_environment(retro_environment_t);
void gambatte_retro_set_video_refresh(retro_video_refresh_t);
void gambatte_retro_set_audio_sample(retro_audio_sample_t);
void gambatte_retro_set_audio_sample_batch(retro_audio_sample_batch_t);
void gambatte_retro_set_input_poll(retro_input_poll_t);
void gambatte_retro_set_input_state(retro_input_state_t);
void gambatte_retro_set_controller_port_device(unsigned, unsigned);
void gambatte_retro_reset(void);
void gambatte_retro_run(void);
size_t gambatte_retro_serialize_size(void);
bool gambatte_retro_serialize(void*, size_t);
bool gambatte_retro_unserialize(const void*, size_t);
bool gambatte_retro_load_game(const struct retro_game_info*);
void gambatte_retro_unload_game(void);
void* gambatte_retro_get_memory_data(unsigned);
size_t gambatte_retro_get_memory_size(unsigned);
void gambatte_retro_cheat_reset(void);
void gambatte_retro_cheat_set(unsigned, bool, const char*);
unsigned gambatte_retro_get_region(void);

// ---- Snes9x（SNES）重命名符号 ------------------------------
void snes9x_retro_init(void);
void snes9x_retro_deinit(void);
unsigned snes9x_retro_api_version(void);
void snes9x_retro_get_system_info(struct retro_system_info*);
void snes9x_retro_get_system_av_info(struct retro_system_av_info*);
void snes9x_retro_set_environment(retro_environment_t);
void snes9x_retro_set_video_refresh(retro_video_refresh_t);
void snes9x_retro_set_audio_sample(retro_audio_sample_t);
void snes9x_retro_set_audio_sample_batch(retro_audio_sample_batch_t);
void snes9x_retro_set_input_poll(retro_input_poll_t);
void snes9x_retro_set_input_state(retro_input_state_t);
void snes9x_retro_set_controller_port_device(unsigned, unsigned);
void snes9x_retro_reset(void);
void snes9x_retro_run(void);
size_t snes9x_retro_serialize_size(void);
bool snes9x_retro_serialize(void*, size_t);
bool snes9x_retro_unserialize(const void*, size_t);
bool snes9x_retro_load_game(const struct retro_game_info*);
void snes9x_retro_unload_game(void);
void* snes9x_retro_get_memory_data(unsigned);
size_t snes9x_retro_get_memory_size(unsigned);
void snes9x_retro_cheat_reset(void);
void snes9x_retro_cheat_set(unsigned, bool, const char*);
unsigned snes9x_retro_get_region(void);

// ---- Snes9x 2005（SNES）重命名符号 -------------------------
void snes9x2005_retro_init(void);
void snes9x2005_retro_deinit(void);
unsigned snes9x2005_retro_api_version(void);
void snes9x2005_retro_get_system_info(struct retro_system_info*);
void snes9x2005_retro_get_system_av_info(struct retro_system_av_info*);
void snes9x2005_retro_set_environment(retro_environment_t);
void snes9x2005_retro_set_video_refresh(retro_video_refresh_t);
void snes9x2005_retro_set_audio_sample(retro_audio_sample_t);
void snes9x2005_retro_set_audio_sample_batch(retro_audio_sample_batch_t);
void snes9x2005_retro_set_input_poll(retro_input_poll_t);
void snes9x2005_retro_set_input_state(retro_input_state_t);
void snes9x2005_retro_set_controller_port_device(unsigned, unsigned);
void snes9x2005_retro_reset(void);
void snes9x2005_retro_run(void);
size_t snes9x2005_retro_serialize_size(void);
bool snes9x2005_retro_serialize(void*, size_t);
bool snes9x2005_retro_unserialize(const void*, size_t);
bool snes9x2005_retro_load_game(const struct retro_game_info*);
void snes9x2005_retro_unload_game(void);
void* snes9x2005_retro_get_memory_data(unsigned);
size_t snes9x2005_retro_get_memory_size(unsigned);
void snes9x2005_retro_cheat_reset(void);
void snes9x2005_retro_cheat_set(unsigned, bool, const char*);
unsigned snes9x2005_retro_get_region(void);

} // extern "C"

// ---- 像素格式辅助函数 -------------------------------------------

/// 构造 RGBA8888 像素（小端 uint32：字节序 [R,G,B,A]）。
static inline uint32_t makeRGBA8888(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint32_t>(r)
         | (static_cast<uint32_t>(g) << 8)
         | (static_cast<uint32_t>(b) << 16)
         | 0xFF000000u;
}

// ---- Libretro 日志接口回调 --------------------------------

/// 将核心日志输出到 stderr，便于查看时钟/RTC 错误。
static void RETRO_CALLCONV s_coreLogCallback(enum retro_log_level level,
                                              const char* fmt, ...)
{
    static const char* const levelStr[] = { "DEBUG", "INFO", "WARN", "ERROR" };
    const char* tag = (level >= RETRO_LOG_DEBUG && level <= RETRO_LOG_ERROR)
                      ? levelStr[level] : "?";
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[Core/%s] ", tag);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

// ---- Libretro 性能接口回调 -----------------------
// 通过 RETRO_ENVIRONMENT_GET_PERF_INTERFACE 提供给核心。
// get_time_usec 是核心用于 RTC 和计时的主时钟；
// counter/register/start/stop 回调支持可选的性能分析。

/// 返回自 Unix 纪元以来的当前墙钟时间（微秒）。
static retro_time_t RETRO_CALLCONV s_perfGetTimeUsec(void)
{
#if defined(_WIN32)
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    // FILETIME 以 100 纳秒为单位，起点为 1601-01-01。
    // 换算为自 Unix 纪元（1970-01-01）起的微秒数。
    // 两个纪元相差 134774 天 × 86400 秒 = 11644473600 秒。
    static const uint64_t k_fileTimeToUnixEpochSeconds = 11644473600ULL;
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (retro_time_t)(t / 10LL - (int64_t)(k_fileTimeToUnixEpochSeconds * 1000000ULL));
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (retro_time_t)ts.tv_sec * 1000000LL + (retro_time_t)(ts.tv_nsec / 1000LL);
#endif
}

/// 返回高精度单调计数器的当前 tick 值。
static retro_perf_tick_t RETRO_CALLCONV s_perfGetCounter(void)
{
#if defined(_WIN32)
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return (retro_perf_tick_t)li.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (retro_perf_tick_t)ts.tv_sec * 1000000000ULL
         + (retro_perf_tick_t)ts.tv_nsec;
#endif
}

/// 将性能计数器标记为已注册。
static void RETRO_CALLCONV s_perfRegister(struct retro_perf_counter* counter)
{
    if (counter) counter->registered = true;
}

/// 记录起始 tick 并递增调用次数。
static void RETRO_CALLCONV s_perfStart(struct retro_perf_counter* counter)
{
    if (counter) {
        counter->start = s_perfGetCounter();
        ++counter->call_cnt;
    }
}

/// 将已用 tick 累加到计数器总量。
static void RETRO_CALLCONV s_perfStop(struct retro_perf_counter* counter)
{
    if (counter) {
        counter->total += s_perfGetCounter() - counter->start;
    }
}

/// 空操作日志：宿主不打印核心性能数据。
static void RETRO_CALLCONV s_perfLog(void) {}

/// 返回 0——宿主不向核心通告任何 CPU 特性。
static uint64_t RETRO_CALLCONV s_perfGetCpuFeatures(void) { return 0; }

namespace beiklive {

namespace {
std::array<std::vector<LibretroLoader::CoreOptionDefinition>, 7> g_coreOptions;

size_t coreOptionIndex(CoreType type)
{
    return std::min<size_t>(static_cast<size_t>(type), g_coreOptions.size() - 1);
}

void registerCoreOptions(LibretroLoader* loader,
                         const retro_core_options_v2* options)
{
    if (!loader || !options || !options->definitions)
        return;
    auto& output = g_coreOptions[coreOptionIndex(loader->coreType())];
    output.clear();
    for (const retro_core_option_v2_definition* def = options->definitions;
         def && def->key; ++def) {
        LibretroLoader::CoreOptionDefinition item;
        item.key = def->key;
        item.title = def->desc ? def->desc : def->key;
        item.description = def->info ? def->info : "";
        item.category = def->category_key ? def->category_key : "general";
        item.defaultValue = def->default_value ? def->default_value : "";
        for (const retro_core_option_value* value = def->values;
             value && value->value; ++value) {
            item.values.push_back({value->value,
                value->label ? value->label : value->value});
        }
        if (loader->configManager() && !item.defaultValue.empty())
            loader->configManager()->SetDefault(
                "core." + item.key, ConfigValue(item.defaultValue));
        output.push_back(std::move(item));
    }
    if (loader->configManager())
        loader->configManager()->Save();
}
}

void LibretroLoader::discoverCoreOptions(CoreType coreType, ConfigManager* config)
{
    if (!g_coreOptions[coreOptionIndex(coreType)].empty())
        return;
    LibretroLoader loader;
    loader.setConfigManager(config);
    loader.load(coreType);
    loader.unload();
}

const std::vector<LibretroLoader::CoreOptionDefinition>&
LibretroLoader::coreOptions(CoreType coreType)
{
    return g_coreOptions[coreOptionIndex(coreType)];
}

// ---- 静态实例指针 ----------------------------------------
LibretroLoader* LibretroLoader::s_current = nullptr;

LibretroLoader::LibretroLoader() = default;

void LibretroLoader::clearRomxLaunchState(bool resetVfsRequest)
{
    if (m_romxVfsActive)
    {
        beiklive::romx_vfs::deactivate(this);
        m_romxVfsActive = false;
    }
    m_romxVirtualPath.clear();
    m_romxSession.reset();
    if (m_romxMapping)
    {
        romx_payload_mapping_close(m_romxMapping);
        m_romxMapping = nullptr;
    }
#if defined(__APPLE__) && !defined(__SWITCH__)
    m_romxPayloadData = nullptr;
    m_romxPayloadSize = 0;
#endif
    m_romxMaterializedPath.clear();
    if (resetVfsRequest)
        m_romxVfsRequested = false;
}

LibretroLoader::~LibretroLoader()
{
    unload();
}

// ============================================================
// 动态库辅助函数
// ============================================================

static void* dynOpen(const std::string& path)
{
#if defined(_WIN32)
    return reinterpret_cast<void*>(LoadLibraryA(path.c_str()));
#elif defined(__PSV__) || defined(__psp2__) || defined(__SWITCH__)
    (void)path;
    return nullptr; // PSVita / Switch 不支持动态加载
#else
    return dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
#endif
}

static void dynLoadError()
{
#if defined(_WIN32)
    DWORD err = GetLastError();
    char msg[256] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, 0, msg, sizeof(msg) - 1, nullptr);
    fprintf(stderr, "[LibretroLoader] LoadLibrary failed (%lu): %s\n", err, msg);
#elif defined(__PSV__) || defined(__psp2__) || defined(__SWITCH__)
    fprintf(stderr, "[LibretroLoader] dynamic loading not supported on this platform\n");
#else
    fprintf(stderr, "[LibretroLoader] dlopen failed: %s\n", dlerror());
#endif
}

static void dynClose(void* handle)
{
    if (!handle) return;
#if defined(_WIN32)
    FreeLibrary(reinterpret_cast<HMODULE>(handle));
#elif defined(__PSV__) || defined(__psp2__) || defined(__SWITCH__)
    (void)handle; // PSVita / Switch 上为空操作
#else
    dlclose(handle);
#endif
}

static void* dynSym(void* handle, const char* name)
{
#if defined(_WIN32)
    return reinterpret_cast<void*>(
        GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
#elif defined(__PSV__) || defined(__psp2__) || defined(__SWITCH__)
    (void)handle; (void)name;
    return nullptr; // PSVita / Switch 上为空操作
#else
    return dlsym(handle, name);
#endif
}

// ============================================================
// 符号解析辅助函数
// ============================================================

template<typename T>
bool LibretroLoader::resolveSymbol(T& fnPtr, const char* name)
{
    fnPtr = reinterpret_cast<T>(dynSym(m_handle, name));
    if (!fnPtr) {
        return false;
    }
    return true;
}

// ============================================================
// 加载 / 卸载
// ============================================================

bool LibretroLoader::load(CoreType coreType)
{
    unload();

    m_coreType = coreType;
    m_dynamicHandle = false;
    // Gambatte is built as RGB565. Keep this as its fallback in case a core
    // reload reaches video output before pixel negotiation.
    m_pixelFormat = coreType == CoreType::Gambatte
        ? RETRO_PIXEL_FORMAT_RGB565
        : RETRO_PIXEL_FORMAT_0RGB1555;
    m_lastVideoLogWidth = 0;
    m_lastVideoLogHeight = 0;
    m_lastVideoLogPitch = 0;
    m_lastVideoLogFormat = RETRO_PIXEL_FORMAT_0RGB1555;
    brls::Logger::debug("[LibretroLoader] load(CoreType={})", static_cast<int>(coreType));

    // 根据核心类型选择对应的符号集
    switch (coreType) {
        case CoreType::Mgba:
            brls::Logger::error("[LibretroLoader] mGBA libretro backend is disabled; use MgbaNativeCore");
            return false;

        case CoreType::Genesis:
            brls::Logger::error("[LibretroLoader] Genesis libretro backend is disabled; use GenesisCore");
            return false;

        case CoreType::Gambatte:
            fn_set_environment        = gambatte_retro_set_environment;
            fn_set_video_refresh      = gambatte_retro_set_video_refresh;
            fn_set_audio_sample       = gambatte_retro_set_audio_sample;
            fn_set_audio_sample_batch = gambatte_retro_set_audio_sample_batch;
            fn_set_input_poll         = gambatte_retro_set_input_poll;
            fn_set_input_state        = gambatte_retro_set_input_state;
            fn_init                   = gambatte_retro_init;
            fn_deinit                 = gambatte_retro_deinit;
            fn_api_version            = gambatte_retro_api_version;
            fn_get_system_info        = gambatte_retro_get_system_info;
            fn_get_system_av_info     = gambatte_retro_get_system_av_info;
            fn_set_controller_port_device = gambatte_retro_set_controller_port_device;
            fn_reset                  = gambatte_retro_reset;
            fn_run                    = gambatte_retro_run;
            fn_serialize_size         = gambatte_retro_serialize_size;
            fn_serialize              = gambatte_retro_serialize;
            fn_unserialize            = gambatte_retro_unserialize;
            fn_load_game              = gambatte_retro_load_game;
            fn_unload_game            = gambatte_retro_unload_game;
            fn_cheat_reset            = gambatte_retro_cheat_reset;
            fn_cheat_set              = gambatte_retro_cheat_set;
            fn_get_memory_data        = gambatte_retro_get_memory_data;
            fn_get_memory_size        = gambatte_retro_get_memory_size;
            break;

        case CoreType::Fceumm:
            fn_set_environment        = fceumm_retro_set_environment;
            fn_set_video_refresh      = fceumm_retro_set_video_refresh;
            fn_set_audio_sample       = fceumm_retro_set_audio_sample;
            fn_set_audio_sample_batch = fceumm_retro_set_audio_sample_batch;
            fn_set_input_poll         = fceumm_retro_set_input_poll;
            fn_set_input_state        = fceumm_retro_set_input_state;
            fn_init                   = fceumm_retro_init;
            fn_deinit                 = fceumm_retro_deinit;
            fn_api_version            = fceumm_retro_api_version;
            fn_get_system_info        = fceumm_retro_get_system_info;
            fn_get_system_av_info     = fceumm_retro_get_system_av_info;
            fn_set_controller_port_device = fceumm_retro_set_controller_port_device;
            fn_reset                  = fceumm_retro_reset;
            fn_run                    = fceumm_retro_run;
            fn_serialize_size         = fceumm_retro_serialize_size;
            fn_serialize              = fceumm_retro_serialize;
            fn_unserialize            = fceumm_retro_unserialize;
            fn_load_game              = fceumm_retro_load_game;
            fn_unload_game            = fceumm_retro_unload_game;
            fn_cheat_reset            = fceumm_retro_cheat_reset;
            fn_cheat_set              = fceumm_retro_cheat_set;
            fn_get_memory_data        = fceumm_retro_get_memory_data;
            fn_get_memory_size        = fceumm_retro_get_memory_size;
            break;

        case CoreType::Nestopia:
            fn_set_environment        = nestopia_retro_set_environment;
            fn_set_video_refresh      = nestopia_retro_set_video_refresh;
            fn_set_audio_sample       = nestopia_retro_set_audio_sample;
            fn_set_audio_sample_batch = nestopia_retro_set_audio_sample_batch;
            fn_set_input_poll         = nestopia_retro_set_input_poll;
            fn_set_input_state        = nestopia_retro_set_input_state;
            fn_init                   = nestopia_retro_init;
            fn_deinit                 = nestopia_retro_deinit;
            fn_api_version            = nestopia_retro_api_version;
            fn_get_system_info        = nestopia_retro_get_system_info;
            fn_get_system_av_info     = nestopia_retro_get_system_av_info;
            fn_set_controller_port_device = nestopia_retro_set_controller_port_device;
            fn_reset                  = nestopia_retro_reset;
            fn_run                    = nestopia_retro_run;
            fn_serialize_size         = nestopia_retro_serialize_size;
            fn_serialize              = nestopia_retro_serialize;
            fn_unserialize            = nestopia_retro_unserialize;
            fn_load_game              = nestopia_retro_load_game;
            fn_unload_game            = nestopia_retro_unload_game;
            fn_cheat_reset            = nestopia_retro_cheat_reset;
            fn_cheat_set              = nestopia_retro_cheat_set;
            fn_get_memory_data        = nestopia_retro_get_memory_data;
            fn_get_memory_size        = nestopia_retro_get_memory_size;
            break;

        case CoreType::Snes9x:
            fn_set_environment        = snes9x_retro_set_environment;
            fn_set_video_refresh      = snes9x_retro_set_video_refresh;
            fn_set_audio_sample       = snes9x_retro_set_audio_sample;
            fn_set_audio_sample_batch = snes9x_retro_set_audio_sample_batch;
            fn_set_input_poll         = snes9x_retro_set_input_poll;
            fn_set_input_state        = snes9x_retro_set_input_state;
            fn_init                   = snes9x_retro_init;
            fn_deinit                 = snes9x_retro_deinit;
            fn_api_version            = snes9x_retro_api_version;
            fn_get_system_info        = snes9x_retro_get_system_info;
            fn_get_system_av_info     = snes9x_retro_get_system_av_info;
            fn_set_controller_port_device = snes9x_retro_set_controller_port_device;
            fn_reset                  = snes9x_retro_reset;
            fn_run                    = snes9x_retro_run;
            fn_serialize_size         = snes9x_retro_serialize_size;
            fn_serialize              = snes9x_retro_serialize;
            fn_unserialize            = snes9x_retro_unserialize;
            fn_load_game              = snes9x_retro_load_game;
            fn_unload_game            = snes9x_retro_unload_game;
            fn_cheat_reset            = snes9x_retro_cheat_reset;
            fn_cheat_set              = snes9x_retro_cheat_set;
            fn_get_memory_data        = snes9x_retro_get_memory_data;
            fn_get_memory_size        = snes9x_retro_get_memory_size;
            break;

        case CoreType::Snes9x2005:
            fn_set_environment        = snes9x2005_retro_set_environment;
            fn_set_video_refresh      = snes9x2005_retro_set_video_refresh;
            fn_set_audio_sample       = snes9x2005_retro_set_audio_sample;
            fn_set_audio_sample_batch = snes9x2005_retro_set_audio_sample_batch;
            fn_set_input_poll         = snes9x2005_retro_set_input_poll;
            fn_set_input_state        = snes9x2005_retro_set_input_state;
            fn_init                   = snes9x2005_retro_init;
            fn_deinit                 = snes9x2005_retro_deinit;
            fn_api_version            = snes9x2005_retro_api_version;
            fn_get_system_info        = snes9x2005_retro_get_system_info;
            fn_get_system_av_info     = snes9x2005_retro_get_system_av_info;
            fn_set_controller_port_device = snes9x2005_retro_set_controller_port_device;
            fn_reset                  = snes9x2005_retro_reset;
            fn_run                    = snes9x2005_retro_run;
            fn_serialize_size         = snes9x2005_retro_serialize_size;
            fn_serialize              = snes9x2005_retro_serialize;
            fn_unserialize            = snes9x2005_retro_unserialize;
            fn_load_game              = snes9x2005_retro_load_game;
            fn_unload_game            = snes9x2005_retro_unload_game;
            fn_cheat_reset            = snes9x2005_retro_cheat_reset;
            fn_cheat_set              = snes9x2005_retro_cheat_set;
            fn_get_memory_data        = snes9x2005_retro_get_memory_data;
            fn_get_memory_size        = snes9x2005_retro_get_memory_size;
            break;

    }

    m_handle = reinterpret_cast<void*>(1); // 哨兵值：符号已绑定

    // 注册静态回调（须在 retro_init 前调用）
    s_current = this;
    fn_set_environment        (s_environmentCallback);
    fn_set_video_refresh      (s_videoRefreshCallback);
    fn_set_audio_sample       (s_audioSampleCallback);
    fn_set_audio_sample_batch (s_audioSampleBatchCallback);
    fn_set_input_poll         (s_inputPollCallback);
    fn_set_input_state        (s_inputStateCallback);

    brls::Logger::debug("[LibretroLoader] load(CoreType) OK, handle={}", m_handle != nullptr);
    return true;
}

bool LibretroLoader::load(const std::string& libPath)
{
    unload();

    m_coreType = CoreType::Mgba;
    m_dynamicHandle = true;
    m_handle = dynOpen(libPath);
    if (!m_handle) {
        dynLoadError();
        m_dynamicHandle = false;
        return false;
    }

    bool ok = true;
    ok &= resolveSymbol(fn_set_environment,         "retro_set_environment");
    ok &= resolveSymbol(fn_set_video_refresh,       "retro_set_video_refresh");
    ok &= resolveSymbol(fn_set_audio_sample,        "retro_set_audio_sample");
    ok &= resolveSymbol(fn_set_audio_sample_batch,  "retro_set_audio_sample_batch");
    ok &= resolveSymbol(fn_set_input_poll,          "retro_set_input_poll");
    ok &= resolveSymbol(fn_set_input_state,         "retro_set_input_state");
    ok &= resolveSymbol(fn_init,                    "retro_init");
    ok &= resolveSymbol(fn_deinit,                  "retro_deinit");
    ok &= resolveSymbol(fn_api_version,             "retro_api_version");
    ok &= resolveSymbol(fn_get_system_info,         "retro_get_system_info");
    ok &= resolveSymbol(fn_get_system_av_info,      "retro_get_system_av_info");
    ok &= resolveSymbol(fn_set_controller_port_device, "retro_set_controller_port_device");
    ok &= resolveSymbol(fn_reset,                   "retro_reset");
    ok &= resolveSymbol(fn_run,                     "retro_run");
    ok &= resolveSymbol(fn_serialize_size,          "retro_serialize_size");
    ok &= resolveSymbol(fn_serialize,               "retro_serialize");
    ok &= resolveSymbol(fn_unserialize,             "retro_unserialize");
    ok &= resolveSymbol(fn_load_game,               "retro_load_game");
    ok &= resolveSymbol(fn_unload_game,             "retro_unload_game");
    resolveSymbol(fn_cheat_reset,               "retro_cheat_reset");
    resolveSymbol(fn_cheat_set,                 "retro_cheat_set");
    resolveSymbol(fn_get_memory_data,           "retro_get_memory_data");
    resolveSymbol(fn_get_memory_size,           "retro_get_memory_size");

    if (!ok) {
        dynClose(m_handle);
        m_handle = nullptr;
        m_dynamicHandle = false;
        return false;
    }

    // 注册静态回调（须在 retro_init 前调用）
    s_current = this;
    fn_set_environment        (s_environmentCallback);
    fn_set_video_refresh      (s_videoRefreshCallback);
    fn_set_audio_sample       (s_audioSampleCallback);
    fn_set_audio_sample_batch (s_audioSampleBatchCallback);
    fn_set_input_poll         (s_inputPollCallback);
    fn_set_input_state        (s_inputStateCallback);

    return true;
}

void LibretroLoader::unload()
{
    brls::Logger::debug("[LibretroLoader] unload: gameLoaded={}, coreReady={}",
        m_gameLoaded, m_coreReady);
    unloadGame();
    m_romxVfsRequested = false;

    // Each loader owns its core session. Do not leave a statically linked core
    // initialized after its callbacks and function bindings have been removed.
    deinitCore();
    m_diskControl = {};
    m_diskControlExt = {};
    m_hasDiskControl = false;
    m_hasDiskControlExt = false;
    void* handle = m_handle;
    m_handle = nullptr;
    if (s_current == this) {
        s_current = nullptr;
    }
#if defined(__APPLE__) && !defined(__SWITCH__)
    std::memset(m_buttons, 0, sizeof(m_buttons));
    std::memset(m_analog, 0, sizeof(m_analog));
    std::memset(m_analogButtons, 0, sizeof(m_analogButtons));
#endif
    {
        std::lock_guard<std::mutex> lk(m_audioMutex);
        clearAudioBufferLocked();
    }
    // 清空函数指针
    fn_set_environment         = nullptr;
    fn_set_video_refresh       = nullptr;
    fn_set_audio_sample        = nullptr;
    fn_set_audio_sample_batch  = nullptr;
    fn_set_input_poll          = nullptr;
    fn_set_input_state         = nullptr;
    fn_init                    = nullptr;
    fn_deinit                  = nullptr;
    fn_api_version             = nullptr;
    fn_get_system_info         = nullptr;
    fn_get_system_av_info      = nullptr;
    fn_set_controller_port_device = nullptr;
    fn_reset                   = nullptr;
    fn_run                     = nullptr;
    fn_serialize_size          = nullptr;
    fn_serialize               = nullptr;
    fn_unserialize             = nullptr;
    fn_load_game               = nullptr;
    fn_unload_game             = nullptr;
    fn_cheat_reset             = nullptr;
    fn_cheat_set               = nullptr;
    fn_get_memory_data         = nullptr;
    fn_get_memory_size         = nullptr;

    if (m_dynamicHandle)
        dynClose(handle);
    m_dynamicHandle = false;
}

// ============================================================
// 核心生命周期
// ============================================================

// 跟踪哪些核心类型已经调用了 retro_init()，防止重复初始化。
// CoreType 是连续枚举，数量必须覆盖最后一个枚举值（Gambatte=6）。
constexpr size_t kCoreTypeCount = static_cast<size_t>(CoreType::Gambatte) + 1;
static std::array<bool, kCoreTypeCount> s_coreInitialized{};

bool LibretroLoader::initCore()
{
    if (!m_handle) { brls::Logger::debug("[LibretroLoader] initCore: no handle"); return false; }
    const size_t idx = static_cast<size_t>(m_coreType);
    if (idx >= s_coreInitialized.size()) {
        brls::Logger::error("[LibretroLoader] initCore: invalid core type {}",
                            static_cast<int>(m_coreType));
        return false;
    }
    if (s_coreInitialized[idx]) {
        brls::Logger::debug("[LibretroLoader] initCore: already initialized (idx={})", idx);
        m_coreReady = true;
        return true;
    }
    brls::Logger::debug("[LibretroLoader] initCore: calling retro_init()...");
    fn_init();
    s_coreInitialized[idx] = true;
    m_coreReady = true;
    brls::Logger::debug("[LibretroLoader] initCore: retro_init() OK");
    return true;
}

void LibretroLoader::deinitCore()
{
    if (!m_coreReady) return;
    const size_t idx = static_cast<size_t>(m_coreType);
    if (idx >= s_coreInitialized.size()) {
        brls::Logger::error("[LibretroLoader] deinitCore: invalid core type {}",
                            static_cast<int>(m_coreType));
        m_coreReady = false;
        return;
    }
    if (fn_deinit) {
        brls::Logger::debug("[LibretroLoader] deinitCore: calling retro_deinit()");
        fn_deinit();
    } else {
        brls::Logger::warning("[LibretroLoader] deinitCore: missing retro_deinit callback");
    }
    s_coreInitialized[idx] = false;
    m_coreReady = false;
}

unsigned LibretroLoader::apiVersion() const
{
    return m_handle ? fn_api_version() : 0;
}

void LibretroLoader::getSystemInfo(retro_system_info* info) const
{
    if (m_handle) fn_get_system_info(info);
}

void LibretroLoader::getSystemAvInfo(retro_system_av_info* info) const
{
    if (m_handle) fn_get_system_av_info(info);
}

void LibretroLoader::setControllerPortDevice(unsigned port, unsigned device)
{
    if (m_handle && fn_set_controller_port_device)
        fn_set_controller_port_device(port, device);
}

bool LibretroLoader::loadGame(const std::string& romPath)
{
    if (!m_coreReady) { brls::Logger::debug("[LibretroLoader] loadGame: core not ready"); return false; }

    brls::Logger::debug("[LibretroLoader] loadGame: path={}", romPath);
    retro_system_info systemInfo{};
    fn_get_system_info(&systemInfo);

    clearRomxLaunchState();
#if defined(__APPLE__) && !defined(__SWITCH__)
    m_hwRenderActive = false;
    m_vulkanNegotiationValid = false;
    m_hwRenderCallback = {};
    m_vulkanNegotiation = {};
#endif

    std::vector<uint8_t> romData;
    std::string launchPath = romPath;
#if defined(__APPLE__) && !defined(__SWITCH__)
    if (beiklive::romx::isRomxPath(romPath) && systemInfo.need_fullpath &&
        m_romxPayloadPreferred) {
        std::string error;
        m_romxSession = std::make_unique<beiklive::romx::LaunchSession>();
        if (!m_romxSession->open(romPath, &error)) {
            brls::Logger::error("[LibretroLoader] ROMX payload open failed: {}", error);
            m_romxSession.reset();
            return false;
        }
        const std::string entrypoint = m_romxSession->info().entrypointPath;
        const bool isZipEntrypoint = entrypoint.size() >= 4 &&
            (entrypoint.compare(entrypoint.size() - 4, 4, ".zip") == 0 ||
             entrypoint.compare(entrypoint.size() - 4, 4, ".ZIP") == 0);
        if (m_romxSession->info().entryCount != 1 || !isZipEntrypoint) {
            brls::Logger::error(
                "[LibretroLoader] FBNeo ROMX must contain exactly one ZIP entrypoint: {}",
                romPath);
            m_romxSession.reset();
            return false;
        }
        if (!m_romxSession->mapPayload(&m_romxPayloadData, &m_romxPayloadSize,
                                       &error) || !m_romxPayloadData ||
            m_romxPayloadSize == 0) {
            brls::Logger::error("[LibretroLoader] ROMX payload mapping failed: {}", error);
            m_romxSession.reset();
            m_romxPayloadData = nullptr;
            m_romxPayloadSize = 0;
            return false;
        }
        // FBNeo uses the .romx path to identify the driver and to locate the
        // mapped archive; the actual ZIP bytes are supplied through data.
        launchPath = romPath;
        brls::Logger::info("[LibretroLoader] ROMX ZIP payload mapped: {} bytes",
                           m_romxPayloadSize);
    }
    else
#endif
    if (beiklive::romx::isRomxPath(romPath) && systemInfo.need_fullpath) {
        std::string error;
        m_romxSession = std::make_unique<beiklive::romx::LaunchSession>();
        if (!m_romxSession->open(romPath, &error)) {
            brls::Logger::error("[LibretroLoader] ROMX open failed: {}", error);
            m_romxSession.reset();
            return false;
        }

        // A path-oriented core can consume the logical entrypoint through
        // Libretro VFS.  This keeps large/split ROMX payloads out of a
        // temporary extraction and lets companion RIDX files resolve by
        // their normal relative paths.  Cores that do not request VFS use
        // the ordinary materialized-path fallback below.
#if defined(__APPLE__) && !defined(__SWITCH__)
        if (m_romxVfsRequested && m_romxVfsAllowed) {
#else
        if (m_romxVfsRequested) {
#endif
            m_romxVirtualPath = beiklive::romx_vfs::makeVirtualPath(
                romPath, m_romxSession->info().entrypointPath);
            if (beiklive::romx_vfs::activate(this, m_romxVirtualPath,
                                             m_romxSession.get())) {
                m_romxVfsActive = true;
                launchPath = m_romxVirtualPath;
                brls::Logger::info("[LibretroLoader] ROMX entrypoint exposed through VFS: {}",
                                   launchPath);
            }
        }

        if (!m_romxVfsActive &&
            !m_romxSession->materializeEntrypoint(
                beiklive::romx::GameEntryAdapter::payloadCacheDirectory(),
                m_romxMaterializedPath, &error)) {
            brls::Logger::error("[LibretroLoader] ROMX path fallback failed: {}", error);
            m_romxSession.reset();
            return false;
        }
        if (!m_romxVfsActive)
            launchPath = m_romxMaterializedPath;
    }

    if (!systemInfo.need_fullpath && beiklive::romx::isRomxPath(romPath)) {
        romx_reader_t* reader = nullptr;
        romx_error_t error{};
        romx_result_t result = romx_reader_open_path(romPath.c_str(), nullptr, &reader, &error);
        if (result == ROMX_OK) {
            romx_info_t romxInfo = ROMX_INFO_INIT;
            result = romx_reader_get_info(reader, &romxInfo, &error);
            if (result == ROMX_OK && romxInfo.entry_count == 1)
                result = romx_reader_map_payload(reader, &m_romxMapping, &error);
            if (result == ROMX_OK && m_romxMapping) {
                brls::Logger::info("[LibretroLoader] ROMX payload mapped: {} bytes",
                                   romx_payload_mapping_size(m_romxMapping));
            } else {
                // A descriptor/multi-file entrypoint is small and is allowed
                // to use the explicit materialization fallback.
                beiklive::romx::LaunchSession session;
                std::string fallbackError;
                if (!session.open(romPath, &fallbackError) ||
                    !session.materializeEntrypoint(beiklive::romx::GameEntryAdapter::payloadCacheDirectory(),
                                                   m_romxMaterializedPath, &fallbackError)) {
                    if (reader) romx_reader_close(reader);
                    brls::Logger::error("[LibretroLoader] ROMX descriptor fallback failed: {}", fallbackError);
                    return false;
                }
                launchPath = m_romxMaterializedPath;
            }
            if (reader) romx_reader_close(reader);
        } else {
            brls::Logger::error("[LibretroLoader] ROMX open failed: {}", error.message);
            return false;
        }
    }

    if (!systemInfo.need_fullpath && !m_romxMapping) {
        std::ifstream file(launchPath, std::ios::binary);
        if (!file) {
            brls::Logger::error("[LibretroLoader] loadGame: failed to open ROM data: {}", romPath);
            return false;
        }
        file.seekg(0, std::ios::end);
        std::streamoff size = file.tellg();
        if (size <= 0) {
            brls::Logger::error("[LibretroLoader] loadGame: ROM data is empty: {}", romPath);
            return false;
        }
        file.seekg(0, std::ios::beg);
        romData.resize(static_cast<size_t>(size));
        if (!file.read(reinterpret_cast<char*>(romData.data()), size)) {
            brls::Logger::error("[LibretroLoader] loadGame: failed to read ROM data: {}", romPath);
            return false;
        }
    }

    retro_game_info info{};
    info.path = launchPath.c_str();
    info.data = m_romxMapping
        ? romx_payload_mapping_data(m_romxMapping)
#if defined(__APPLE__) && !defined(__SWITCH__)
        : (m_romxPayloadData
            ? m_romxPayloadData
            : (romData.empty() ? nullptr : romData.data()));
    info.size = m_romxMapping
        ? static_cast<size_t>(romx_payload_mapping_size(m_romxMapping))
        : (m_romxPayloadData ? static_cast<size_t>(m_romxPayloadSize)
                              : romData.size());
#else
        : (romData.empty() ? nullptr : romData.data());
    info.size = m_romxMapping ? static_cast<size_t>(romx_payload_mapping_size(m_romxMapping))
                              : romData.size();
#endif
    info.meta = nullptr;

    bool loaded = fn_load_game(&info);
    if (!loaded && m_romxVfsActive) {
        // A few older libretro cores request VFS for ordinary file I/O but
        // still reject URI-like paths before consulting their VFS callbacks
        // (for example, a path_is_valid() guard).  Keep ROMX transparent by
        // retrying once with the same entrypoint materialized in the cache.
        brls::Logger::warning(
            "[LibretroLoader] ROMX VFS path rejected; retrying materialized entrypoint");
        if (fn_unload_game)
            fn_unload_game();
        beiklive::romx_vfs::deactivate(this);
        m_romxVfsActive = false;
        m_romxVirtualPath.clear();
        std::string fallbackError;
        if (m_romxSession && m_romxSession->materializeEntrypoint(
                beiklive::romx::GameEntryAdapter::payloadCacheDirectory(),
                m_romxMaterializedPath, &fallbackError)) {
            launchPath = m_romxMaterializedPath;
            info.path = launchPath.c_str();
            loaded = fn_load_game(&info);
        } else {
            brls::Logger::error(
                "[LibretroLoader] ROMX VFS fallback extraction failed: {}",
                fallbackError);
        }
    }

    if (!loaded) {
        brls::Logger::error("[LibretroLoader] loadGame: retro_load_game failed");
        clearRomxLaunchState();
        return false;
    }

#if defined(__APPLE__) && !defined(__SWITCH__)
    if (m_hwRenderActive)
    {
        if (m_hwRenderCallback.context_type != RETRO_HW_CONTEXT_VULKAN ||
            !m_vulkanNegotiationValid)
        {
            brls::Logger::error(
                "[LibretroLoader] core requested Vulkan without a negotiation interface");
            fn_unload_game();
            clearRomxLaunchState();
            return false;
        }

        m_vulkanHost = std::make_unique<LibretroVulkanHost>();
        if (!m_vulkanHost->initialize(m_vulkanNegotiation) ||
            !m_hwRenderCallback.context_reset)
        {
            brls::Logger::error(
                "[LibretroLoader] failed to initialize the KosmicKrisp Vulkan host");
            m_vulkanHost.reset();
            fn_unload_game();
            clearRomxLaunchState();
            return false;
        }

        // context_reset is the point at which the core may query
        // RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE.
        m_gameLoaded = true;
        m_hwRenderCallback.context_reset();
    }
#endif

    fn_get_system_av_info(&m_avInfo);
    m_gameLoaded = true;
    brls::Logger::debug("[LibretroLoader] loadGame OK: {}x{} @ {:.2f}fps",
        m_avInfo.geometry.base_width, m_avInfo.geometry.base_height, m_avInfo.timing.fps);
    return true;
}

void LibretroLoader::unloadGame()
{
#if defined(__APPLE__) && !defined(__SWITCH__)
    if (m_vulkanHost && m_hwRenderActive && m_hwRenderCallback.context_destroy)
        m_hwRenderCallback.context_destroy();
#endif
    if (m_gameLoaded) {
        brls::Logger::debug("[LibretroLoader] unloadGame");
        fn_unload_game();
        m_gameLoaded = false;
    }
#if defined(__APPLE__) && !defined(__SWITCH__)
    if (m_vulkanHost)
    {
        m_vulkanHost->shutdown();
        m_vulkanHost.reset();
    }
    m_hwRenderCallback = {};
    m_vulkanNegotiation = {};
    m_hwRenderActive = false;
    m_vulkanNegotiationValid = false;
#endif
    clearRomxLaunchState();
    m_diskControl = {};
    m_diskControlExt = {};
    m_hasDiskControl = false;
    m_hasDiskControlExt = false;
    m_loggedFirstRun = false;
}

void LibretroLoader::run()
{
    if (!m_gameLoaded) return;
    const bool trace = m_coreType == CoreType::Fceumm && !m_loggedFirstRun;
    if (trace) {
        m_loggedFirstRun = true;
        brls::Logger::debug("[LibretroLoader] FCEUmm retro_run enter");
    }
    fn_run();
    if (trace)
        brls::Logger::debug("[LibretroLoader] FCEUmm retro_run return");
}

void LibretroLoader::reset()
{
    if (m_gameLoaded && fn_reset) fn_reset();
}

size_t LibretroLoader::serializeSize() const
{
    return (m_gameLoaded && fn_serialize_size) ? fn_serialize_size() : 0;
}

bool LibretroLoader::serialize(void* data, size_t size) const
{
    return (m_gameLoaded && fn_serialize) ? fn_serialize(data, size) : false;
}

bool LibretroLoader::unserialize(const void* data, size_t size)
{
    return (m_gameLoaded && fn_unserialize) ? fn_unserialize(data, size) : false;
}

// ============================================================
// 视频 / 音频访问器
// ============================================================

LibretroLoader::VideoFrame LibretroLoader::getVideoFrame() const
{
    std::lock_guard<std::mutex> lk(m_videoMutex);
    return m_videoFrame;
}

bool LibretroLoader::drainAudio(std::vector<int16_t>& out)
{
    std::lock_guard<std::mutex> lk(m_audioMutex);
    if (m_audioAvailable == 0) return false;

    out.resize(m_audioAvailable);
    const size_t cap = m_audioBuffer.size();
    const size_t first = std::min(m_audioAvailable, cap - m_audioReadPos);
    std::memcpy(out.data(), m_audioBuffer.data() + m_audioReadPos, first * sizeof(int16_t));
    if (first < m_audioAvailable) {
        std::memcpy(out.data() + first, m_audioBuffer.data(),
                    (m_audioAvailable - first) * sizeof(int16_t));
    }
    m_audioReadPos = m_audioWritePos;
    m_audioAvailable = 0;
    return true;
}

void LibretroLoader::clearAudioBufferLocked()
{
    m_audioReadPos = 0;
    m_audioWritePos = 0;
    m_audioAvailable = 0;
    if (!m_audioBuffer.empty())
        std::fill(m_audioBuffer.begin(), m_audioBuffer.end(), 0);
}

void LibretroLoader::pushAudioSamplesLocked(const int16_t* data, size_t samples)
{
    if (!data || samples == 0)
        return;

    if (m_audioBuffer.empty())
        m_audioBuffer.assign(AUDIO_BUFFER_CAPACITY, 0);

    const size_t cap = m_audioBuffer.size();
    if (samples >= cap) {
        data += samples - cap;
        samples = cap;
        m_audioReadPos = 0;
        m_audioWritePos = 0;
        m_audioAvailable = 0;
    }

    if (m_audioAvailable + samples > cap) {
        const size_t drop = (m_audioAvailable + samples) - cap;
        m_audioReadPos = (m_audioReadPos + drop) % cap;
        m_audioAvailable -= drop;
    }

    const size_t first = std::min(samples, cap - m_audioWritePos);
    std::memcpy(m_audioBuffer.data() + m_audioWritePos, data, first * sizeof(int16_t));
    if (first < samples) {
        std::memcpy(m_audioBuffer.data(), data + first, (samples - first) * sizeof(int16_t));
    }

    m_audioWritePos = (m_audioWritePos + samples) % cap;
    m_audioAvailable += samples;
}

// ============================================================
// 输入
// ============================================================

void LibretroLoader::setButtonState(unsigned port, unsigned id, bool pressed)
{
    if (port < kMaxInputPorts && id <= RETRO_DEVICE_ID_JOYPAD_R3)
        m_buttons[port][id] = pressed;
}

bool LibretroLoader::getButtonState(unsigned port, unsigned id) const
{
    return (port < kMaxInputPorts && id <= RETRO_DEVICE_ID_JOYPAD_R3) ? m_buttons[port][id] : false;
}

#if defined(__APPLE__) && !defined(__SWITCH__)
void LibretroLoader::setAnalogState(unsigned port, unsigned index, unsigned id, int16_t value)
{
    if (port >= kMaxInputPorts)
        return;
    if ((index == RETRO_DEVICE_INDEX_ANALOG_LEFT ||
         index == RETRO_DEVICE_INDEX_ANALOG_RIGHT) &&
        id <= RETRO_DEVICE_ID_ANALOG_Y)
    {
        m_analog[port][index][id] = value;
        return;
    }
    if (index == RETRO_DEVICE_INDEX_ANALOG_BUTTON &&
        id <= RETRO_DEVICE_ID_JOYPAD_R3)
    {
        m_analogButtons[port][id] = value;
    }
}

int16_t LibretroLoader::getAnalogState(unsigned port, unsigned index, unsigned id) const
{
    if (port >= kMaxInputPorts)
        return 0;
    if ((index == RETRO_DEVICE_INDEX_ANALOG_LEFT ||
         index == RETRO_DEVICE_INDEX_ANALOG_RIGHT) &&
        id <= RETRO_DEVICE_ID_ANALOG_Y)
    {
        return m_analog[port][index][id];
    }
    if (index == RETRO_DEVICE_INDEX_ANALOG_BUTTON &&
        id <= RETRO_DEVICE_ID_JOYPAD_R3)
    {
        return m_analogButtons[port][id];
    }
    return 0;
}
#endif

// ============================================================
// 内存（SRAM）
// ============================================================

void* LibretroLoader::getMemoryData(unsigned id) const
{
    if (!m_gameLoaded || !fn_get_memory_data) return nullptr;
    return fn_get_memory_data(id);
}

size_t LibretroLoader::getMemorySize(unsigned id) const
{
    if (!m_gameLoaded || !fn_get_memory_size) return 0;
    return fn_get_memory_size(id);
}

// ============================================================
// 金手指
// ============================================================

void LibretroLoader::cheatReset()
{
    if (m_gameLoaded && fn_cheat_reset) fn_cheat_reset();
}

void LibretroLoader::cheatSet(unsigned index, bool enabled, const std::string& code)
{
    if (m_gameLoaded && fn_cheat_set) fn_cheat_set(index, enabled, code.c_str());
}

// ============================================================
// 磁盘控制
// ============================================================

LibretroLoader::DiskControlState LibretroLoader::diskControlState() const
{
    DiskControlState state;
    const auto getNumImages = m_hasDiskControlExt ? m_diskControlExt.get_num_images : m_diskControl.get_num_images;
    const auto getImageIndex = m_hasDiskControlExt ? m_diskControlExt.get_image_index : m_diskControl.get_image_index;
    const auto getEjectState = m_hasDiskControlExt ? m_diskControlExt.get_eject_state : m_diskControl.get_eject_state;
    if (!m_gameLoaded || !m_hasDiskControl || !getNumImages || !getImageIndex || !getEjectState)
        return state;

    state.supported = true;
    state.ejected = getEjectState();
    state.currentIndex = getImageIndex();
    state.numImages = getNumImages();
    state.labels.reserve(state.numImages);
    for (unsigned i = 0; i < state.numImages; ++i)
    {
        std::string label;
        if (m_hasDiskControlExt && m_diskControlExt.get_image_label)
        {
            std::array<char, 256> buf{};
            if (m_diskControlExt.get_image_label(i, buf.data(), buf.size()) && buf[0] != '\0')
                label = buf.data();
        }
        if (label.empty() && m_hasDiskControlExt && m_diskControlExt.get_image_path)
        {
            std::array<char, 512> buf{};
            if (m_diskControlExt.get_image_path(i, buf.data(), buf.size()) && buf[0] != '\0')
            {
                std::string path = buf.data();
                const size_t pos = path.find_last_of("/\\");
                label = pos == std::string::npos ? path : path.substr(pos + 1);
            }
        }
        if (label.empty())
            label = "磁盘面 " + std::to_string(i + 1);
        state.labels.push_back(std::move(label));
    }
    return state;
}

bool LibretroLoader::setDiskEjected(bool ejected)
{
    const auto setEjectState = m_hasDiskControlExt ? m_diskControlExt.set_eject_state : m_diskControl.set_eject_state;
    return m_gameLoaded && m_hasDiskControl && setEjectState && setEjectState(ejected);
}

bool LibretroLoader::setDiskImageIndex(unsigned index)
{
    const auto setImageIndex = m_hasDiskControlExt ? m_diskControlExt.set_image_index : m_diskControl.set_image_index;
    return m_gameLoaded && m_hasDiskControl && setImageIndex && setImageIndex(index);
}

bool LibretroLoader::switchDiskImage(unsigned index, bool insertAfter)
{
    const auto setEjectState = m_hasDiskControlExt ? m_diskControlExt.set_eject_state : m_diskControl.set_eject_state;
    const auto setImageIndex = m_hasDiskControlExt ? m_diskControlExt.set_image_index : m_diskControl.set_image_index;
    const auto getNumImages = m_hasDiskControlExt ? m_diskControlExt.get_num_images : m_diskControl.get_num_images;
    if (!m_gameLoaded || !m_hasDiskControl || !setEjectState || !setImageIndex || !getNumImages)
        return false;
    if (index >= getNumImages())
        return false;
    if (!setEjectState(true))
        return false;
    if (!setImageIndex(index))
        return false;
    return insertAfter ? setEjectState(false) : true;
}

// ============================================================
// 静态回调（libretro C 接口）
// ============================================================

bool LibretroLoader::s_environmentCallback(unsigned cmd, void* data)
{
    if (!s_current) return false;

    switch (cmd) {
#if defined(__APPLE__) && !defined(__SWITCH__)
        case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER: {
            if (!s_current->m_vulkanPreferred || !data)
                return false;
            *static_cast<unsigned*>(data) = RETRO_HW_CONTEXT_VULKAN;
            return true;
        }
        case RETRO_ENVIRONMENT_SET_HW_RENDER: {
            const auto* callback = static_cast<const retro_hw_render_callback*>(data);
            if (!s_current->m_vulkanPreferred || !callback ||
                callback->context_type != RETRO_HW_CONTEXT_VULKAN)
                return false;
            s_current->m_hwRenderCallback = *callback;
            s_current->m_hwRenderActive = true;
            brls::Logger::info("[LibretroLoader] external core selected Vulkan rendering");
            return true;
        }
        case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE: {
            auto** interfacePtr =
                static_cast<const retro_hw_render_interface**>(data);
            if (!interfacePtr || !s_current->m_vulkanHost ||
                !s_current->m_vulkanHost->isInitialized())
                return false;
            *interfacePtr = s_current->m_vulkanHost->interfacePtr();
            return true;
        }
        case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE: {
            const auto* base =
                static_cast<const retro_hw_render_context_negotiation_interface*>(data);
            if (!base || !s_current->m_vulkanPreferred ||
                base->interface_type != RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN)
                return false;
            const auto* vulkan =
                static_cast<const retro_hw_render_context_negotiation_interface_vulkan*>(data);
            s_current->m_vulkanNegotiation = *vulkan;
            s_current->m_vulkanNegotiationValid = true;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT: {
            auto* base =
                static_cast<retro_hw_render_context_negotiation_interface*>(data);
            if (!base || base->interface_type !=
                             RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN)
                return false;
            // The host intentionally exposes the stable v1 device callback;
            // cores using the newer struct automatically use its v1 prefix.
            base->interface_version = 1;
            return true;
        }
#endif
        case RETRO_ENVIRONMENT_GET_LANGUAGE: {
            unsigned* language = static_cast<unsigned*>(data);
            if (language) *language = RETRO_LANGUAGE_CHINESE_SIMPLIFIED;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_CAN_DUPE: {
            bool* b = static_cast<bool*>(data);
            if (b) *b = true;
            return true;
        }
#if defined(__APPLE__) && !defined(__SWITCH__)
        case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
            // 支持 libretro 的按键位掩码接口。PPSSPP、mGBA、Genesis
            // 等核心会先询问这个能力，随后用一次 MASK 查询读取全部按键。
            return true;
#endif
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
            const retro_pixel_format* fmt = static_cast<const retro_pixel_format*>(data);
            if (!fmt) return false;
            if (*fmt == RETRO_PIXEL_FORMAT_XRGB8888 ||
                *fmt == RETRO_PIXEL_FORMAT_RGB565) {
                s_current->m_pixelFormat = *fmt;
                brls::Logger::debug("[LibretroLoader] pixel format core={} format={}",
                                    static_cast<int>(s_current->m_coreType),
                                    static_cast<int>(*fmt));
                return true;
            }
            return false;
        }
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: {
            const char** dir = static_cast<const char**>(data);
            if (dir) {
                *dir = s_current->m_systemDirectory.empty()
                     ? "." : s_current->m_systemDirectory.c_str();
            }
            return true;
        }
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: {
            const char** dir = static_cast<const char**>(data);
            if (dir) {
                *dir = s_current->m_saveDirectory.empty()
                     ? "." : s_current->m_saveDirectory.c_str();
            }
            return true;
        }
        case RETRO_ENVIRONMENT_GET_LIBRETRO_PATH: {
            // 返回当前目录
            const char** dir = static_cast<const char**>(data);
            if (dir) *dir = ".";
            return true;
        }
        case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION: {
            unsigned* version = static_cast<unsigned*>(data);
            if (version) *version = 1;
            return true;
        }
        case RETRO_ENVIRONMENT_SET_MESSAGE: {
            const retro_message* msg = static_cast<const retro_message*>(data);
            if (msg && msg->msg) {
                fprintf(stdout, "[Core] %s\n", msg->msg);
            }
            return true;
        }
        case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: {
            const retro_system_av_info* info =
                static_cast<const retro_system_av_info*>(data);
            if (!info) return false;
            s_current->m_avInfo = *info;
            brls::Logger::debug(
                "[LibretroLoader] SET_SYSTEM_AV_INFO: {}x{} (max {}x{}, aspect {:.3f}, fps {:.2f}, sampleRate {:.2f})",
                info->geometry.base_width,
                info->geometry.base_height,
                info->geometry.max_width,
                info->geometry.max_height,
                info->geometry.aspect_ratio,
                info->timing.fps,
                info->timing.sample_rate);
            return true;
        }
        case RETRO_ENVIRONMENT_SHUTDOWN:
            return true;
        // ---- 核心选项版本：返回 2 以使用 V2 接口 ----
        case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: {
            unsigned* ver = static_cast<unsigned*>(data);
            if (ver) *ver = 2;
            return true;
        }
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2: {
            registerCoreOptions(s_current,
                static_cast<const retro_core_options_v2*>(data));
            return true;
        }
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL: {
            const auto* intl = static_cast<const retro_core_options_v2_intl*>(data);
            registerCoreOptions(s_current,
                intl ? (intl->local ? intl->local : intl->us) : nullptr);
            return true;
        }
        // ---- 核心声明变量及默认值 ----------------------
        case RETRO_ENVIRONMENT_SET_VARIABLES: {
            const retro_variable* vars = static_cast<const retro_variable*>(data);
            if (!vars || !s_current->m_configManager) return false;
            beiklive::ConfigManager* cfg = s_current->m_configManager;
            for (const retro_variable* v = vars; v->key; ++v) {
                if (!v->value) continue;
                // 格式："描述文本; 默认值|选项2|选项3..."
                // "; " 后的第一个选项为默认值。
                std::string valStr(v->value);
                size_t semiPos = valStr.find("; ");
                if (semiPos == std::string::npos) continue;
                std::string optsPart = valStr.substr(semiPos + 2);
                size_t pipePos = optsPart.find('|');
                std::string defaultVal = (pipePos != std::string::npos)
                    ? optsPart.substr(0, pipePos) : optsPart;
                if (defaultVal.empty()) continue;
                std::string cfgKey = std::string("core.") + v->key;
                cfg->SetDefault(cfgKey, defaultVal);
            }
            cfg->Save();
            return true;
        }
        // ---- 核心查询变量值 ---------------------------------
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            retro_variable* var = static_cast<retro_variable*>(data);
            if (!var || !var->key) return false;
            var->value = nullptr;
            if (!s_current->m_configManager) return false;
            std::string cfgKey = std::string("core.") + var->key;
            auto entry = s_current->m_configManager->Get(cfgKey);
            if (!entry) return false;
            auto str = entry->AsString();
            if (!str) return false;
            // 存入持久 map，确保 c_str() 指针始终有效。
            auto& stored = s_current->m_coreVarStorage[var->key];
            stored = *str;
            var->value = stored.c_str();
            return true;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: {
            bool* b = static_cast<bool*>(data);
            if (b) *b = s_current->m_configChanged.exchange(false, std::memory_order_acq_rel);
            return true;
        }
        case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
        case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
        case RETRO_ENVIRONMENT_SET_ROTATION:
        case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
            return true;
        case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE: {
            const auto* cb = static_cast<const retro_disk_control_callback*>(data);
            if (!cb || !cb->set_eject_state || !cb->get_eject_state ||
                !cb->get_image_index || !cb->set_image_index || !cb->get_num_images)
                return false;
            s_current->m_diskControl = *cb;
            s_current->m_diskControlExt = {};
            s_current->m_hasDiskControl = true;
            s_current->m_hasDiskControlExt = false;
            brls::Logger::debug("[LibretroLoader] disk control interface registered");
            return true;
        }
        case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE: {
            const auto* cb = static_cast<const retro_disk_control_ext_callback*>(data);
            if (!cb || !cb->set_eject_state || !cb->get_eject_state ||
                !cb->get_image_index || !cb->set_image_index || !cb->get_num_images)
                return false;
            s_current->m_diskControlExt = *cb;
            s_current->m_hasDiskControl = true;
            s_current->m_hasDiskControlExt = true;
            brls::Logger::debug("[LibretroLoader] disk control ext interface registered");
            return true;
        }
        case RETRO_ENVIRONMENT_SET_GEOMETRY: {
            const retro_game_geometry* geometry =
                static_cast<const retro_game_geometry*>(data);
            if (!geometry) return false;
            s_current->m_avInfo.geometry = *geometry;
            brls::Logger::debug(
                "[LibretroLoader] SET_GEOMETRY: {}x{} (max {}x{}, aspect {:.3f})",
                geometry->base_width,
                geometry->base_height,
                geometry->max_width,
                geometry->max_height,
                geometry->aspect_ratio);
            return true;
        }
        case RETRO_ENVIRONMENT_GET_FASTFORWARDING: {
            bool* ff = static_cast<bool*>(data);
            if (ff) *ff = s_current->m_fastForwarding.load(std::memory_order_relaxed);
            return true;
        }
        case RETRO_ENVIRONMENT_GET_PERF_INTERFACE: {
            retro_perf_callback* cb = static_cast<retro_perf_callback*>(data);
            if (cb) {
                cb->get_time_usec    = s_perfGetTimeUsec;
                cb->get_cpu_features = s_perfGetCpuFeatures;
                cb->get_perf_counter = s_perfGetCounter;
                cb->perf_register    = s_perfRegister;
                cb->perf_start       = s_perfStart;
                cb->perf_stop        = s_perfStop;
                cb->perf_log         = s_perfLog;
            }
            return true;
        }
        case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
        case RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE:
        case RETRO_ENVIRONMENT_GET_CAMERA_INTERFACE:
        case RETRO_ENVIRONMENT_GET_LOCATION_INTERFACE:
            return false;
        case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE: {
            int* flags = static_cast<int*>(data);
            if (flags) *flags = (1 << 0) | (1 << 1); // VIDEO | AUDIO
            return true;
        }
        case RETRO_ENVIRONMENT_GET_VFS_INTERFACE: {
#if defined(__APPLE__) && !defined(__SWITCH__)
            if (!s_current->m_romxVfsAllowed)
                return false;
#endif
            auto* info = static_cast<retro_vfs_interface_info*>(data);
            // The bridge implements the complete libretro VFS v3 surface,
            // including stat/mkdir and directory enumeration.  Keep v1/v2
            // cores working while allowing multi-file cores that request v3.
            if (!info || info->required_interface_version > 3)
                return false;
            info->iface = beiklive::romx_vfs::interfacePtr();
            info->required_interface_version = 3;
            s_current->m_romxVfsRequested = info->iface != nullptr;
            return s_current->m_romxVfsRequested;
        }
        case RETRO_ENVIRONMENT_GET_LED_INTERFACE:
            // LED 接口不可用，核心不检查返回值
            return true;
        case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION: {
            unsigned* ver = static_cast<unsigned*>(data);
            if (ver) *ver = 1;
            return true;
        }
        case RETRO_ENVIRONMENT_SET_MESSAGE_EXT: {
            const retro_message_ext* msg = static_cast<const retro_message_ext*>(data);
            if (msg && msg->msg) {
                fprintf(stdout, "[Core] %s\n", msg->msg);
            }
            return true;
        }
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
            return true;
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
            retro_log_callback* log = static_cast<retro_log_callback*>(data);
            if (log) log->log = s_coreLogCallback;
            return true;
        }
        default:
            return false;
    }
}

void LibretroLoader::s_videoRefreshCallback(const void* data,
                                             unsigned width, unsigned height,
                                             size_t pitch)
{
#if defined(__APPLE__) && !defined(__SWITCH__)
    if (!s_current) return;
    if (data == RETRO_HW_FRAME_BUFFER_VALID)
    {
        if (!s_current->m_vulkanHost)
            return;

        std::vector<uint32_t> pixels;
        if (!s_current->m_vulkanHost->readbackFrame(width, height, pixels))
            return;

        std::lock_guard<std::mutex> lock(s_current->m_videoMutex);
        auto& frame = s_current->m_videoFrame;
        frame.width = width;
        frame.height = height;
        frame.pixels = std::move(pixels);
        return;
    }
    if (!data) return;
#else
    if (!s_current || !data) return;
#endif

    // 防御性检查：合理范围
    if (width < 16 || width > 720 || height < 16 || height > 576)
        return;
    const size_t bytesPerPixel = s_current->m_pixelFormat == RETRO_PIXEL_FORMAT_XRGB8888
        ? 4u : 2u;
    if (pitch < static_cast<size_t>(width) * bytesPerPixel)
        return;

    std::lock_guard<std::mutex> lk(s_current->m_videoMutex);
    auto& vf       = s_current->m_videoFrame;
    vf.width       = width;
    vf.height      = height;
    vf.pixels.resize(width * height);

    const uint8_t* src = static_cast<const uint8_t*>(data);

    if (s_current->m_pixelFormat == RETRO_PIXEL_FORMAT_XRGB8888) {
        // 源每行字节数为 pitch（可能宽于 width*4）
        // 将 XRGB8888 [B,G,R,X] 转换为 RGBA8888 [R,G,B,0xFF]
        for (unsigned row = 0; row < height; ++row) {
            const uint32_t* srcRow = reinterpret_cast<const uint32_t*>(src + row * pitch);
            uint32_t*       dstRow = vf.pixels.data() + row * width;
            for (unsigned col = 0; col < width; ++col) {
                uint32_t px = srcRow[col]; // 小端：byte0=B, byte1=G, byte2=R, byte3=X
                dstRow[col] = makeRGBA8888(
                    static_cast<uint8_t>((px >> 16) & 0xFF),
                    static_cast<uint8_t>((px >>  8) & 0xFF),
                    static_cast<uint8_t>( px        & 0xFF));
            }
        }
    } else if (s_current->m_pixelFormat == RETRO_PIXEL_FORMAT_RGB565) {
        // RGB565：16 位像素，用位移近似扩展为 RGBA8888
        // 位布局：RRRRR_GGGGGG_BBBBB（位 15-11=R，10-5=G，4-0=B）
        for (unsigned row = 0; row < height; ++row) {
            const uint16_t* srcRow = reinterpret_cast<const uint16_t*>(src + row * pitch);
            uint32_t*       dstRow = vf.pixels.data() + row * width;
            for (unsigned col = 0; col < width; ++col) {
                uint16_t px = srcRow[col];
                uint8_t r5 = (px >> 11) & 0x1F;
                uint8_t g6 = (px >>  5) & 0x3F;
                uint8_t b5 =  px        & 0x1F;
                // 5 位扩展为 8 位：(v << 3) | (v >> 2)
                // 6 位扩展为 8 位：(v << 2) | (v >> 4)
                dstRow[col] = makeRGBA8888(
                    static_cast<uint8_t>((r5 << 3) | (r5 >> 2)),
                    static_cast<uint8_t>((g6 << 2) | (g6 >> 4)),
                    static_cast<uint8_t>((b5 << 3) | (b5 >> 2)));
            }
        }
    } else {
        // RETRO_PIXEL_FORMAT_0RGB1555（libretro 默认格式）：
        // 16 位像素，位布局：0_RRRRR_GGGGG_BBBBB（位15=0，14-10=R，9-5=G，4-0=B）
        for (unsigned row = 0; row < height; ++row) {
            const uint16_t* srcRow = reinterpret_cast<const uint16_t*>(src + row * pitch);
            uint32_t*       dstRow = vf.pixels.data() + row * width;
            for (unsigned col = 0; col < width; ++col) {
                uint16_t px = srcRow[col];
                uint8_t r5 = (px >> 10) & 0x1F;  // 位 14-10
                uint8_t g5 = (px >>  5) & 0x1F;  // 位 9-5
                uint8_t b5 =  px        & 0x1F;  // 位 4-0
                // 5 位扩展为 8 位：(v << 3) | (v >> 2)
                dstRow[col] = makeRGBA8888(
                    static_cast<uint8_t>((r5 << 3) | (r5 >> 2)),
                    static_cast<uint8_t>((g5 << 3) | (g5 >> 2)),
                    static_cast<uint8_t>((b5 << 3) | (b5 >> 2)));
            }
        }
    }

    if (s_current->m_lastVideoLogWidth != width ||
        s_current->m_lastVideoLogHeight != height ||
        s_current->m_lastVideoLogPitch != pitch ||
        s_current->m_lastVideoLogFormat != s_current->m_pixelFormat) {
        uint32_t sourcePixel = 0;
        if (s_current->m_pixelFormat == RETRO_PIXEL_FORMAT_XRGB8888)
            std::memcpy(&sourcePixel, src, sizeof(sourcePixel));
        else
            std::memcpy(&sourcePixel, src, sizeof(uint16_t));

        brls::Logger::debug(
            "[LibretroLoader] video frame: core={} format={} {}x{} pitch={} raw=0x{:08X} rgba=0x{:08X}",
            static_cast<int>(s_current->m_coreType),
            static_cast<int>(s_current->m_pixelFormat),
            width, height, pitch, sourcePixel, vf.pixels.front());
        s_current->m_lastVideoLogWidth = width;
        s_current->m_lastVideoLogHeight = height;
        s_current->m_lastVideoLogPitch = pitch;
        s_current->m_lastVideoLogFormat = s_current->m_pixelFormat;
    }
}

void LibretroLoader::s_audioSampleCallback(int16_t left, int16_t right)
{
    if (!s_current) return;
    std::lock_guard<std::mutex> lk(s_current->m_audioMutex);
    const int16_t samples[2] = {left, right};
    s_current->pushAudioSamplesLocked(samples, 2);
}

size_t LibretroLoader::s_audioSampleBatchCallback(const int16_t* data, size_t frames)
{
    if (!s_current || !data) return frames;
    std::lock_guard<std::mutex> lk(s_current->m_audioMutex);
    const size_t samples = frames * 2; // 立体声
    s_current->pushAudioSamplesLocked(data, samples);
    return frames;
}

void LibretroLoader::s_inputPollCallback()
{
    // 输入状态由主线程在 retro_run() 前更新，此处无需处理。
}

int16_t LibretroLoader::s_inputStateCallback(unsigned port, unsigned device,
#if defined(__APPLE__) && !defined(__SWITCH__)
                                               unsigned index,
#else
                                               unsigned /*index*/,
#endif
                                               unsigned id)
{
    if (!s_current || port >= kMaxInputPorts) return 0;
#if defined(__APPLE__) && !defined(__SWITCH__)
    if (device == RETRO_DEVICE_JOYPAD)
    {
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
        {
            uint16_t mask = 0;
            for (unsigned button = 0;
                 button <= RETRO_DEVICE_ID_JOYPAD_R3; ++button)
            {
                if (s_current->m_buttons[port][button])
                    mask |= static_cast<uint16_t>(1u << button);
            }
            return static_cast<int16_t>(mask);
        }
        if (id > RETRO_DEVICE_ID_JOYPAD_R3) return 0;
        return s_current->m_buttons[port][id] ? 1 : 0;
    }
    if (device == RETRO_DEVICE_ANALOG)
        return s_current->getAnalogState(port, index, id);
    return 0;
#else
    // Keep the original Switch callback contract: digital joypad queries only.
    if (device != RETRO_DEVICE_JOYPAD) return 0;
    if (id > RETRO_DEVICE_ID_JOYPAD_R3) return 0;
    return s_current->m_buttons[port][id] ? 1 : 0;
#endif
}

} // namespace beiklive
