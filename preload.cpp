// Reads the first frame of every video cut in the background when a project opens,
// so the first playback does not stop at each join to open a file.
// Also swaps videos for light editing copies (a keyframe every 2 frames), because moving the cursor
// in a video with distant keyframes costs up to a second each time.
// Times are written to a log file, because the host only saves errors to its own log files.
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "plugin2.h"
#include "logger2.h"

static const wchar_t* LINK_FOLDER = L"_カット別リンク";
static const wchar_t* COPY_FOLDER = L"_編集用";
static const int DEFAULT_FRAMES_PER_CUT = 1;
static const int LOAD_DELAY_MS = 2000;

enum Mode { PRELOAD, MEASURE, COPIES, ORIGINALS };

static LOG_HANDLE* g_logger = nullptr;
static EDIT_HANDLE* g_edit = nullptr;
static std::thread g_worker;
static std::mutex g_lock;
static std::condition_variable g_wake;
static bool g_quit = false;
static unsigned g_request = 0;
static int g_request_delay = 0;
static Mode g_request_mode = PRELOAD;

// A file in the folder named by AVIUTL2_PRELOAD_DIR, or else the temp folder
static std::wstring data_path(const wchar_t* name) {
    wchar_t folder[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"AVIUTL2_PRELOAD_DIR", folder, MAX_PATH))
        GetTempPathW(MAX_PATH, folder);
    std::wstring path = folder;
    if (!path.empty() && path.back() != L'\\')
        path += L'\\';
    return path + name;
}

// Frames kept for each cut, from AVIUTL2_PRELOAD_FRAMES when set
static int frames_per_cut() {
    wchar_t value[16] = {};
    int frames = GetEnvironmentVariableW(L"AVIUTL2_PRELOAD_FRAMES", value, 16) ? _wtoi(value) : 0;
    return frames > 0 ? frames : DEFAULT_FRAMES_PER_CUT;
}

static COMMON_PLUGIN_TABLE common_plugin_table = {
    L"カット先読み",
    L"各カットの先頭を先読みし、動画を編集用の軽いコピーに差し替えて、プレビューの待ちを減らします",
};

struct RenderState {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
};

static void on_rendered(void* param, int, const void*, int, int, int) {
    auto* holder = static_cast<std::shared_ptr<RenderState>*>(param);
    {
        std::lock_guard<std::mutex> lk((*holder)->m);
        (*holder)->done = true;
    }
    (*holder)->cv.notify_all();
    delete holder;
}

static void collect_cuts(void* param, EDIT_SECTION* edit) {
    auto* cuts = static_cast<std::vector<int>*>(param);
    EDIT_INFO info{};
    g_edit->get_edit_info(&info, sizeof(info));
    for (int layer = 0; layer <= info.layer_max; layer++) {
        std::string last_file;
        OBJECT_HANDLE object = edit->find_object(layer, 0);
        while (object) {
            OBJECT_LAYER_FRAME place = edit->get_object_layer_frame(object);
            if (edit->count_object_effect(object, L"動画ファイル") > 0) {
                // Objects in a row on one path share a decoder, which should wait at the first of them
                const char* file = edit->get_object_item_value(object, L"動画ファイル", L"ファイル");
                if (!file || last_file != file)
                    cuts->push_back(place.start);
                last_file = file ? file : "";
            }
            object = edit->find_object(layer, place.end + 1);
        }
    }
}

// True when this run should give way to a newer request or to shutdown
static bool cancelled(unsigned request) {
    std::lock_guard<std::mutex> lk(g_lock);
    return g_quit || g_request != request;
}

// Milliseconds to render one frame, or -1 if it was refused or the run was cancelled.
// The state stays alive through the shared pointer, so a late callback is harmless.
static double render(int frame, unsigned request) {
    auto started = std::chrono::steady_clock::now();
    auto state = std::make_shared<RenderState>();
    auto* holder = new std::shared_ptr<RenderState>(state);
    if (!g_edit->rendering_scene_video(frame, holder, on_rendered)) {
        delete holder;
        return -1;
    }
    std::unique_lock<std::mutex> lk(state->m);
    for (int waited = 0; waited < 300; waited++) {
        if (state->cv.wait_for(lk, std::chrono::milliseconds(100), [&] { return state->done; }))
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        lk.unlock();
        bool stop = cancelled(request);
        lk.lock();
        if (stop)
            return -1;
    }
    return -1;
}

static void run(Mode mode, int delay_ms, unsigned request) {
    for (int waited = 0; waited < delay_ms; waited += 100) {
        if (cancelled(request))
            return;
        Sleep(100);
    }
    std::vector<int> cuts;
    if (!g_edit->call_edit_section_param(&cuts, collect_cuts))
        return;
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    std::vector<double> times;
    auto started = std::chrono::steady_clock::now();
    FILE* log = _wfopen(data_path(L"aviutl2_preload.log").c_str(), L"a");
    for (int cut : cuts) {
        double first = -1;
        // A measurement plays across the join, as playback does: the frame before it, then the new cut
        for (int i = mode == PRELOAD ? 0 : -1; i < (mode == PRELOAD ? frames_per_cut() : 2); i++) {
            if (cancelled(request)) {
                if (log) {
                    fprintf(log, "cancelled after %zu of %zu cuts\n", times.size(), cuts.size());
                    fclose(log);
                }
                return;
            }
            if (cut + i < 0)
                continue;
            double ms = render(cut + i, request);
            if (i == 0)
                first = ms;
        }
        if (first >= 0)
            times.push_back(first);
        if (log)
            fprintf(log, "%s frame=%d first_frame_ms=%.1f\n", mode == PRELOAD ? "preload" : "measure", cut, first);
    }
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    double median = 0, slowest = 0;
    if (!times.empty()) {
        std::sort(times.begin(), times.end());
        median = times[times.size() / 2];
        slowest = times.back();
    }
    if (log) {
        fprintf(log, "%s done: %zu of %zu cuts in %.1f s, median %.0f ms, slowest %.0f ms\n", mode == PRELOAD ? "preload" : "measure", times.size(), cuts.size(), seconds, median, slowest);
        fclose(log);
    }
    wchar_t message[160];
    swprintf(message, 160, L"カット先読み: %zu / %zu か所 %.1f 秒(中央値 %.0f ms、最大 %.0f ms)", times.size(), cuts.size(), seconds, median, slowest);
    if (g_logger)
        g_logger->info(g_logger, message);
}

static std::string test_mode();
static void make_copies(unsigned request);
static void on_originals_menu(EDIT_SECTION* edit);

static void worker_main() {
    unsigned seen = 0;
    for (;;) {
        Mode mode;
        int delay;
        {
            std::unique_lock<std::mutex> lk(g_lock);
            g_wake.wait(lk, [&] { return g_quit || g_request != seen; });
            if (g_quit)
                return;
            seen = g_request;
            mode = g_request_mode;
            delay = g_request_delay;
        }
        if (mode == ORIGINALS) {
            g_edit->call_edit_section(on_originals_menu);  // leaves a preload request of its own
            continue;
        }
        if (mode == COPIES) {
            make_copies(seen);
            mode = PRELOAD;
        }
        run(mode, delay, seen);
        if (mode == PRELOAD && test_mode().rfind("preloaded", 0) == 0 && !cancelled(seen))
            run(MEASURE, 0, seen);
    }
}

// Only leaves a request: these handlers run on the host's thread, which the worker may be waiting on
static void request_run(Mode mode, int delay_ms) {
    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_request++;
        g_request_mode = mode;
        g_request_delay = delay_ms;
    }
    g_wake.notify_all();
}


static std::wstring to_wide(const char* text);
static std::string to_utf8(const std::wstring& wide);

static void say(const wchar_t* message) {
    if (g_logger)
        g_logger->info(g_logger, message);
    if (FILE* log = _wfopen(data_path(L"aviutl2_preload.log").c_str(), L"a")) {
        fprintf(log, "%s\n", to_utf8(message).c_str());
        fclose(log);
    }
}

static bool exists(const std::wstring& file) {
    return GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static ULONGLONG written_at(const std::wstring& file) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data))
        return 0;
    return (ULONGLONG(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
}

// The original a copy was made from, kept in a small text file beside the copy; empty if there is none
static std::wstring original_of(const std::wstring& copy) {
    char text[2048] = {};
    FILE* file = _wfopen((copy + L".src").c_str(), L"rb");
    if (!file)
        return L"";
    size_t size = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    text[size] = 0;
    return to_wide(text);
}

static bool in_copy_folder(const std::wstring& file) {
    size_t slash = file.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return false;
    std::wstring folder = file.substr(0, slash);
    size_t tail = wcslen(COPY_FOLDER);
    return folder.size() > tail && folder.compare(folder.size() - tail, std::wstring::npos, COPY_FOLDER) == 0 && wcschr(L"\\/", folder[folder.size() - tail - 1]);
}

// Where the editing copy of a video goes: COPY_FOLDER beside it, same name, always .mp4.
// Two videos with one name and different extensions get different copies.
static std::wstring copy_path(const std::wstring& file) {
    size_t slash = file.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return L"";
    std::wstring folder = file.substr(0, slash) + L"\\" + COPY_FOLDER;
    std::wstring name = file.substr(slash + 1);
    size_t dot = name.find_last_of(L'.');
    std::wstring stem = name.substr(0, dot), extension = dot == std::wstring::npos ? L"" : name.substr(dot + 1);
    std::wstring copy = folder + L"\\" + stem + L".mp4";
    std::wstring owner = original_of(copy);
    if (!owner.empty() && _wcsicmp(owner.c_str(), file.c_str()) != 0)
        copy = folder + L"\\" + stem + L"_" + extension + L".mp4";
    return copy;
}

// The exit code of a hidden command, -1 if it could not start, -2 if the run was cancelled
static int run_command(std::wstring command, unsigned request) {
    STARTUPINFOW start{};
    start.cb = sizeof(start);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, &command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &start, &process))
        return -1;
    DWORD code = 1;
    bool stopped = false;
    while (WaitForSingleObject(process.hProcess, 200) == WAIT_TIMEOUT) {
        if (cancelled(request)) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 5000);
            stopped = true;
            break;
        }
    }
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return stopped ? -2 : int(code);
}

// Writes the editing copy with ffmpeg: H.264 with a keyframe every 2 frames and no B frames, at a quality
// close to the original. The graphics card encodes it when it can, the processor otherwise.
static bool encode_copy(const std::wstring& file, const std::wstring& copy, unsigned request) {
    wchar_t tool[MAX_PATH] = L"ffmpeg";
    GetEnvironmentVariableW(L"AVIUTL2_PRELOAD_FFMPEG", tool, MAX_PATH);
    size_t slash = copy.find_last_of(L"\\/");
    CreateDirectoryW(copy.substr(0, slash).c_str(), nullptr);
    std::wstring part = copy + L".part.mp4";
    std::wstring head = L"\"" + std::wstring(tool) + L"\" -v error -y -i \"" + file + L"\" -map 0:v:0 -map 0:a? -fps_mode passthrough ";
    const wchar_t* coders[] = {
        L"-c:v h264_nvenc -preset p5 -rc constqp -qp 16 -g 2 -bf 0 -pix_fmt yuv420p -c:a copy ",
        L"-c:v libx264 -preset veryfast -crf 14 -g 2 -bf 0 -pix_fmt yuv420p -c:a aac -b:a 256k ",
    };
    for (const wchar_t* coder : coders) {
        int code = run_command(head + coder + L"\"" + part + L"\"", request);
        if (code == 0 && MoveFileExW(part.c_str(), copy.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            if (FILE* note = _wfopen((copy + L".src").c_str(), L"wb")) {
                std::string text = to_utf8(file);
                fwrite(text.data(), 1, text.size(), note);
                fclose(note);
            }
            return true;
        }
        DeleteFileW(part.c_str());
        if (code == -1 || code == -2)
            return false;
    }
    return false;
}

static void collect_files(void* param, EDIT_SECTION* edit) {
    auto* files = static_cast<std::vector<std::wstring>*>(param);
    EDIT_INFO info{};
    g_edit->get_edit_info(&info, sizeof(info));
    for (int layer = 0; layer <= info.layer_max; layer++) {
        OBJECT_HANDLE object = edit->find_object(layer, 0);
        while (object) {
            OBJECT_LAYER_FRAME place = edit->get_object_layer_frame(object);
            const char* value = edit->count_object_effect(object, L"動画ファイル") > 0 ? edit->get_object_item_value(object, L"動画ファイル", L"ファイル") : nullptr;
            if (value && *value)
                files->push_back(to_wide(value));
            object = edit->find_object(layer, place.end + 1);
        }
    }
}

struct Swap {
    std::map<std::wstring, std::wstring> to;
    int changed = 0, failed = 0;
};

// Points every video object whose file is in the table at the table's other file
static void swap_files(void* param, EDIT_SECTION* edit) {
    auto* swap = static_cast<Swap*>(param);
    EDIT_INFO info{};
    g_edit->get_edit_info(&info, sizeof(info));
    for (int layer = 0; layer <= info.layer_max; layer++) {
        OBJECT_HANDLE object = edit->find_object(layer, 0);
        while (object) {
            OBJECT_LAYER_FRAME place = edit->get_object_layer_frame(object);
            const char* value = edit->count_object_effect(object, L"動画ファイル") > 0 ? edit->get_object_item_value(object, L"動画ファイル", L"ファイル") : nullptr;
            if (value && *value) {
                auto found = swap->to.find(to_wide(value));
                if (found != swap->to.end()) {
                    if (edit->set_object_item_value(object, L"動画ファイル", L"ファイル", to_utf8(found->second).c_str()))
                        swap->changed++;
                    else
                        swap->failed++;
                }
            }
            object = edit->find_object(layer, place.end + 1);
        }
    }
}

// Makes an editing copy of every video in the project that has none yet (or an older one), then points the
// video objects at the copies. Sound objects keep reading the originals.
static void make_copies(unsigned request) {
    std::vector<std::wstring> files;
    if (!g_edit->call_edit_section_param(&files, collect_files))
        return;
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    Swap swap;
    int made = 0, missed = 0, number = 0;
    auto started = std::chrono::steady_clock::now();
    wchar_t message[600];
    for (const std::wstring& file : files) {
        number++;
        if (in_copy_folder(file) && !original_of(file).empty())
            continue;
        std::wstring copy = copy_path(file);
        if (copy.empty() || !exists(file))
            continue;
        if (!exists(copy) || written_at(copy) < written_at(file)) {
            swprintf(message, 600, L"編集用コピーを作成中 (%d / %zu): %s", number, files.size(), file.c_str());
            say(message);
            if (!encode_copy(file, copy, request)) {
                if (cancelled(request)) {
                    say(L"編集用コピーの作成を中止しました");
                    return;
                }
                missed++;
                continue;
            }
            made++;
        }
        swap.to[file] = copy;
    }
    if (!swap.to.empty() && !g_edit->call_edit_section_param(&swap, swap_files))
        return;
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    swprintf(message, 600, L"編集用コピー: %d 本を作成(失敗 %d 本、%.0f 秒)、%d 個のオブジェクトを差し替えました。書き出す前に「元の動画に戻す」を実行してください", made, missed, seconds, swap.changed);
    say(message);
}

static void on_copies_menu(void*) {
    request_run(COPIES, 0);
}

// Points the video objects back at the originals their copies were made from
static void on_originals_menu(EDIT_SECTION* edit) {
    std::vector<std::wstring> files;
    collect_files(&files, edit);
    Swap swap;
    int lost = 0;
    for (const std::wstring& file : files) {
        std::wstring original = in_copy_folder(file) ? original_of(file) : L"";
        if (original.empty())
            continue;
        if (exists(original))
            swap.to[file] = original;
        else
            lost++;
    }
    swap_files(&swap, edit);
    wchar_t message[200];
    swprintf(message, 200, L"編集用コピー: %d 個のオブジェクトを元の動画に戻しました(元の動画が見つからない %d 本)", swap.changed, lost);
    say(message);
    request_run(PRELOAD, 0);
}

// A test file holding "cold" measures without preloading, "preloaded" measures right after it,
// and "copies" / "originals" swap the editing copies in or out when a project opens
static std::string test_mode() {
    char text[16] = {};
    FILE* file = _wfopen(data_path(L"aviutl2_preload_test.txt").c_str(), L"r");
    if (file) {
        fgets(text, sizeof(text), file);
        fclose(file);
    }
    return text;
}

static void on_project_load(PROJECT_FILE*) {
    if (test_mode().rfind("cold", 0) == 0)
        request_run(MEASURE, LOAD_DELAY_MS);
    else if (test_mode().rfind("copies", 0) == 0)
        request_run(COPIES, LOAD_DELAY_MS);
    else if (test_mode().rfind("originals", 0) == 0)
        request_run(ORIGINALS, LOAD_DELAY_MS);
    else
        request_run(PRELOAD, LOAD_DELAY_MS);
}

static void on_preload_menu(void*) {
    request_run(PRELOAD, 0);
}

static void on_measure_menu(void*) {
    request_run(MEASURE, 0);
}

static std::wstring to_wide(const char* text) {
    int size = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring wide(size > 0 ? size - 1 : 0, L'\0');
    if (size > 1)
        MultiByteToWideChar(CP_UTF8, 0, text, -1, &wide[0], size);
    return wide;
}

static std::string to_utf8(const std::wstring& wide) {
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string text(size > 0 ? size - 1 : 0, '\0');
    if (size > 1)
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &text[0], size, nullptr, nullptr);
    return text;
}

// A new hard link beside the original, in LINK_FOLDER, or an empty string if it can't be made.
// Links need the same drive as the original, and they take no extra space.
static std::wstring make_link(const std::wstring& file) {
    size_t slash = file.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return L"";
    std::wstring folder = file.substr(0, slash);
    std::wstring name = file.substr(slash + 1);
    size_t tail = wcslen(LINK_FOLDER);
    if (folder.size() < tail || folder.compare(folder.size() - tail, std::wstring::npos, LINK_FOLDER) != 0)
        folder += L"\\" + std::wstring(LINK_FOLDER);
    CreateDirectoryW(folder.c_str(), nullptr);
    size_t dot = name.find_last_of(L'.');
    std::wstring stem = name.substr(0, dot), extension = dot == std::wstring::npos ? L"" : name.substr(dot);
    for (int number = 1; number < 100000; number++) {
        std::wstring link = folder + L"\\" + stem + L"_" + std::to_wstring(number) + extension;
        if (GetFileAttributesW(link.c_str()) != INVALID_FILE_ATTRIBUTES)
            continue;
        return CreateHardLinkW(link.c_str(), file.c_str(), nullptr) ? link : L"";
    }
    return L"";
}

// Objects reading the same path share a decoder, which then seeks at every join between them.
// Every object after the first gets its own link, so each keeps its own decoder.
static void on_split_menu(EDIT_SECTION* edit) {
    EDIT_INFO info{};
    g_edit->get_edit_info(&info, sizeof(info));
    std::map<std::wstring, int> seen;
    int linked = 0, failed = 0;
    for (int layer = 0; layer <= info.layer_max; layer++) {
        OBJECT_HANDLE object = edit->find_object(layer, 0);
        while (object) {
            OBJECT_LAYER_FRAME place = edit->get_object_layer_frame(object);
            const char* value = edit->count_object_effect(object, L"動画ファイル") > 0 ? edit->get_object_item_value(object, L"動画ファイル", L"ファイル") : nullptr;
            if (value && *value) {
                std::wstring file = to_wide(value);
                if (seen[file]++ > 0) {
                    std::wstring link = make_link(file);
                    if (!link.empty() && edit->set_object_item_value(object, L"動画ファイル", L"ファイル", to_utf8(link).c_str()))
                        linked++;
                    else
                        failed++;
                }
            }
            object = edit->find_object(layer, place.end + 1);
        }
    }
    wchar_t message[160];
    swprintf(message, 160, L"カット先読み: %d 個のカットをリンクに分けました(失敗 %d 個)", linked, failed);
    if (g_logger)
        g_logger->info(g_logger, message);
    if (FILE* log = _wfopen(data_path(L"aviutl2_preload.log").c_str(), L"a")) {
        fprintf(log, "split done: linked %d, failed %d\n", linked, failed);
        fclose(log);
    }
    request_run(PRELOAD, 0);
}

EXTERN_C __declspec(dllexport) DWORD RequiredVersion() {
    return 2003300;
}

EXTERN_C __declspec(dllexport) void InitializeLogger(LOG_HANDLE* handle) {
    g_logger = handle;
}

EXTERN_C __declspec(dllexport) bool InitializePlugin(DWORD) {
    return true;
}

EXTERN_C __declspec(dllexport) void UninitializePlugin() {
    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_quit = true;
    }
    g_wake.notify_all();
    if (g_worker.joinable())
        g_worker.join();
}

EXTERN_C __declspec(dllexport) COMMON_PLUGIN_TABLE* GetCommonPluginTable() {
    return &common_plugin_table;
}

EXTERN_C __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE* host) {
    if (!host)
        return;
    g_edit = host->create_edit_handle();
    if (!g_edit)
        return;
    g_worker = std::thread(worker_main);
    host->register_project_load_handler(on_project_load);
    host->register_edit_menu_param(L"カットの先頭を先読み", nullptr, on_preload_menu);
    host->register_edit_menu_param(L"先読みの効果を計測", nullptr, on_measure_menu);
    host->register_edit_menu(L"カットごとにデコーダーを分ける", on_split_menu);
    host->register_edit_menu_param(L"編集用の軽い動画に差し替え", nullptr, on_copies_menu);
    host->register_edit_menu(L"元の動画に戻す", on_originals_menu);
}
