#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "JournalDiagnostics.h"
#include <imgui.h>
#include <filesystem>
#include <fstream>
#include <deque>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <mutex>
#include <string>

namespace {
    HMODULE gThisModule = nullptr;
    size_t gModuleSize = 0;
    std::wstring gDirectory;
    std::wstring gLogPath;
    std::wstring gIniPath;
    HANDLE gCrashLog = INVALID_HANDLE_VALUE;
    std::atomic<const char*> gStage{"DllMain"};
    std::atomic<unsigned> gFaultCount{0};
    std::mutex gFileMutex;
    bool gConsoleReady = false;
    std::atomic_bool gNativeConsoleVisible{true};
    std::atomic_bool gOverlayVisible{true};
    std::mutex gOverlayMutex;
    std::deque<std::string> gOverlayLines;
    std::filesystem::path gGameLogPath;
    std::uintmax_t gGameLogPosition = 0;
    ULONGLONG gLastGameLogPoll = 0;

    LONG CALLBACK ObserveException(EXCEPTION_POINTERS* ep) {
        // First-chance observer. NEVER swallow an exception or replace the game's crash handler.
        if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
        const DWORD code = ep->ExceptionRecord->ExceptionCode;
        if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
            code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_IN_PAGE_ERROR)
            return EXCEPTION_CONTINUE_SEARCH;
        if (gFaultCount.fetch_add(1, std::memory_order_relaxed) >= 12)
            return EXCEPTION_CONTINUE_SEARCH;
        const uintptr_t address = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
        const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(gThisModule);
        const bool ours = gModuleSize && address >= moduleBase &&
            address - moduleBase < gModuleSize;
        const uintptr_t rva = ours ? address - moduleBase : 0;
        const uintptr_t invalidAddress = ep->ExceptionRecord->NumberParameters >= 2
            ? ep->ExceptionRecord->ExceptionInformation[1] : 0;
        char record[512]{};
        int chars = _snprintf_s(record, sizeof(record), _TRUNCATE,
            "FIRST_CHANCE code=0x%08lX source=%s fault=0x%llX asi_rva=0x%llX read_or_write=%llu invalid_address=0x%llX stage=%s tid=%lu\r\n",
            static_cast<unsigned long>(code), ours ? "this_asi" : "other_module",
            static_cast<unsigned long long>(address), static_cast<unsigned long long>(rva),
            static_cast<unsigned long long>(ep->ExceptionRecord->NumberParameters ? ep->ExceptionRecord->ExceptionInformation[0] : 0),
            static_cast<unsigned long long>(invalidAddress), gStage.load(std::memory_order_relaxed),
            static_cast<unsigned long>(GetCurrentThreadId()));
        if (gCrashLog != INVALID_HANDLE_VALUE && chars > 0) {
            DWORD written = 0;
            WriteFile(gCrashLog, record, static_cast<DWORD>(chars), &written, nullptr);
            FlushFileBuffers(gCrashLog);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

void FHJDiag::Log(const wchar_t* message) {
    if (!message) return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t line[1600]{};
    _snwprintf_s(line, _countof(line), _TRUNCATE,
        L"[%02hu:%02hu:%02hu.%03hu] [HunterJournal] %s\r\n",
        now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, message);
    OutputDebugStringW(line);
    {
        std::lock_guard<std::mutex> lock(gOverlayMutex);
        int n = WideCharToMultiByte(CP_UTF8,0,line,-1,nullptr,0,nullptr,nullptr);
        if (n > 1) {
            std::string copy(static_cast<size_t>(n), '\0');
            if (WideCharToMultiByte(CP_UTF8,0,line,-1,copy.data(),n,nullptr,nullptr) > 1) {
                copy.resize(static_cast<size_t>(n - 1));
                gOverlayLines.push_back(std::move(copy));
                while (gOverlayLines.size() > 650) gOverlayLines.pop_front();
            }
        }
    }
    if (gConsoleReady) {
        DWORD written = 0;
        WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), line, static_cast<DWORD>(wcslen(line)), &written, nullptr);
    }
    if (gLogPath.empty()) return;
    std::lock_guard<std::mutex> lock(gFileMutex);
    HANDLE h = CreateFileW(gLogPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    const int bytesRequired = WideCharToMultiByte(CP_UTF8, 0, line, -1, nullptr, 0, nullptr, nullptr);
    if (bytesRequired > 1) {
        std::string utf8(static_cast<size_t>(bytesRequired), '\0');
        int len = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8.data(), bytesRequired, nullptr, nullptr);
        if (len > 1) {
            DWORD written = 0;
            WriteFile(h, utf8.data(), static_cast<DWORD>(len - 1), &written, nullptr);
        }
    }
    CloseHandle(h);
}

void FHJDiag::SetStage(const char* stage) {
    gStage.store(stage ? stage : "unknown", std::memory_order_relaxed);
}

void FHJDiag::Stage(const char* stage, const wchar_t* explanation) {
    SetStage(stage);
    Log(explanation);
}

void FHJDiag::Start(HMODULE self) {
    gThisModule = self;
    if (self) {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const BYTE*>(self) + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE)
                gModuleSize = nt->OptionalHeader.SizeOfImage;
        }
    }
    wchar_t base[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n && n < MAX_PATH) {
#ifdef FCH_STANDALONE
        gDirectory = std::wstring(base) + L"\\FolkloreCryptidHunt";
#else
        gDirectory = std::wstring(base) + L"\\FolkloreHunterJournal";
#endif
        CreateDirectoryW(gDirectory.c_str(), nullptr);
        gLogPath = gDirectory + L"\\journal.log";
        gIniPath = gDirectory + L"\\journal.ini";
        const std::wstring crashPath = gDirectory + L"\\crash-trace.log";
        gCrashLog = CreateFileW(crashPath.c_str(), FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    // A separate console is created at game startup; file log works even if the console fails.
    if (AllocConsole() || GetConsoleWindow()) {
        gConsoleReady = true;
        SetConsoleTitleW(L"Folklore Hunter - Hunter's Journal Diagnostics");
        SetConsoleOutputCP(CP_UTF8);
    }
    AddVectoredExceptionHandler(1, ObserveException);
    if (!gIniPath.empty()) {
        gNativeConsoleVisible = GetPrivateProfileIntW(L"Journal", L"Console", 1, gIniPath.c_str()) != 0;
        gOverlayVisible.store(GetPrivateProfileIntW(L"Journal", L"DebugOverlay", 1, gIniPath.c_str()) != 0);
    }
    if (!gNativeConsoleVisible && GetConsoleWindow())
        ShowWindow(GetConsoleWindow(), SW_HIDE);
    Log(L"=== Hunter's Journal native x64 ASI diagnostic build ===");
    Log(L"Console and file diagnostics initialized; exceptions are observed, not intercepted.");
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(self, path, MAX_PATH)) {
        wchar_t msg[700]{};
        _snwprintf_s(msg, _countof(msg), _TRUNCATE, L"ASI module: %s", path);
        Log(msg);
    }
    if (GetModuleFileNameW(nullptr, path, MAX_PATH)) {
        wchar_t msg[700]{};
        _snwprintf_s(msg, _countof(msg), _TRUNCATE, L"Host executable: %s", path);
        Log(msg);
    }
    if (!gLogPath.empty()) {
        wchar_t msg[700]{};
        _snwprintf_s(msg, _countof(msg), _TRUNCATE, L"Session log: %s", gLogPath.c_str());
        Log(msg);
    }
}

bool FHJDiag::SafeMode() {
    if (gIniPath.empty()) {
        Log(L"No LOCALAPPDATA path. Safe mode enabled to avoid modifying the graphics pipeline.");
        return true;
    }
    if (GetFileAttributesW(gIniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        WritePrivateProfileStringW(L"Journal", L"SafeMode", L"0", gIniPath.c_str());
        WritePrivateProfileStringW(L"Journal", L"Console", L"1", gIniPath.c_str());
        WritePrivateProfileStringW(L"Journal", L"DebugOverlay", L"1", gIniPath.c_str());
    }
    const bool safe = GetPrivateProfileIntW(L"Journal", L"SafeMode", 0, gIniPath.c_str()) != 0;
    Log(safe ? L"SafeMode=1: graphics hooks are DISABLED for first diagnosis."
             : L"SafeMode=0: DX11 hooks enabled (experimental; only use in offline/private sessions).");
    return safe;
}


bool FHJDiag::NativeConsoleVisible() { return gNativeConsoleVisible.load(std::memory_order_relaxed); }

bool FHJDiag::OverlayVisible() { return gOverlayVisible.load(std::memory_order_relaxed); }

void FHJDiag::ToggleOverlay() {
    const bool now = !gOverlayVisible.load(std::memory_order_relaxed);
    gOverlayVisible.store(now, std::memory_order_relaxed);
    if (!gIniPath.empty()) WritePrivateProfileStringW(L"Journal", L"DebugOverlay", now ? L"1" : L"0", gIniPath.c_str());
    Log(now ? L"In-game diagnostics visible (F9)." : L"In-game diagnostics hidden (F9).");
}
void FHJDiag::ToggleNativeConsole() {
    gNativeConsoleVisible.store(!gNativeConsoleVisible.load(std::memory_order_relaxed), std::memory_order_relaxed);
    if (GetConsoleWindow()) ShowWindow(GetConsoleWindow(), gNativeConsoleVisible ? SW_SHOW : SW_HIDE);
    if (!gIniPath.empty()) WritePrivateProfileStringW(L"Journal", L"Console", gNativeConsoleVisible ? L"1" : L"0", gIniPath.c_str());
    Log(gNativeConsoleVisible ? L"Native console visible (F10)." : L"Native console hidden (F10).");
}
void FHJDiag::PollGameLog() {
    const auto now = GetTickCount64();
    if (now - gLastGameLogPoll < 1100) return;
    gLastGameLogPoll = now;
    if (gGameLogPath.empty()) {
        wchar_t local[MAX_PATH]{};
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
        if (!n || n >= MAX_PATH) return;
        const auto root = std::filesystem::path(local);
        const std::filesystem::path candidates[] = {
            root / "FolkloreHunter" / "Saved" / "Logs",
            root / "FolkloreHunter" / "Logs"
        };
        std::error_code ec;
        std::filesystem::file_time_type latest{};
        for (const auto& directory : candidates) {
            if (!std::filesystem::is_directory(directory, ec)) continue;
            for (std::filesystem::directory_iterator it(directory, std::filesystem::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec)) {
                if (!it->is_regular_file(ec) || it->path().extension() != ".log") continue;
                auto time = it->last_write_time(ec);
                if (!ec && (gGameLogPath.empty() || time > latest)) {
                    latest = time; gGameLogPath = it->path();
                }
            }
        }
        if (!gGameLogPath.empty()) Log((L"Game log found: " + gGameLogPath.wstring()).c_str());
    }
    if (gGameLogPath.empty()) return;
    std::error_code ec;
    const auto size = std::filesystem::file_size(gGameLogPath, ec);
    if (ec) return;
    if (size < gGameLogPosition) gGameLogPosition = 0;
    if (size == gGameLogPosition) return;
    if (gGameLogPosition == 0 && size > 65536) gGameLogPosition = size - 65536;
    std::ifstream file(gGameLogPath, std::ios::binary);
    if (!file) return;
    file.seekg(static_cast<std::streamoff>(gGameLogPosition));
    std::string bytes(static_cast<size_t>(std::min<std::uintmax_t>(16384, size - gGameLogPosition)), '\0');
    file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    const auto got = static_cast<size_t>(file.gcount());
    gGameLogPosition += got;
    bytes.resize(got);
    std::lock_guard<std::mutex> lock(gOverlayMutex);
    size_t p = 0;
    while (p < bytes.size()) {
        size_t end = bytes.find('\n', p);
        if (end == std::string::npos) end = bytes.size();
        if (end > p) {
            gOverlayLines.push_back(("[GAME] " + bytes.substr(p,end-p)).substr(0,900));
            while (gOverlayLines.size() > 650) gOverlayLines.pop_front();
        }
        p = end + 1;
    }
}
void FHJDiag::DrawOverlay(const char* renderer, float fps) {
    if (!OverlayVisible()) return;
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const float width = std::min(460.0f, std::max(230.0f, screen.x * 0.275f));
    const float height = std::min(310.0f, std::max(160.0f, screen.y * 0.30f));
    ImGui::SetNextWindowSize(ImVec2(width,height), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(12,12), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(.045f,.055f,.045f,.92f));
    ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(.07f,.095f,.06f,1));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(.16f,.20f,.12f,1));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(.61f,.48f,.29f,1));
    if (ImGui::Begin("FOLKLORE HUNTER // FIELD DIAGNOSTICS", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize)) {
        ImGui::TextColored(ImVec4(.85f,.72f,.46f,1), "HUNTER'S JOURNAL");
        ImGui::SameLine();
        ImGui::TextDisabled("API: %s  FPS: %.1f  PID: %lu", renderer, fps, GetCurrentProcessId());
        if (ImGui::Button("Hide diagnostics [F9]")) ToggleOverlay();
        ImGui::SameLine();
        if (ImGui::Button("Native console [F10]")) ToggleNativeConsole();
        ImGui::SameLine();
        if (ImGui::Button("Clear")) {
            std::lock_guard<std::mutex> lock(gOverlayMutex);
            gOverlayLines.clear();
        }
        static bool scroll=true;
        ImGui::Checkbox("Follow log", &scroll);
        ImGui::Separator();
        if (ImGui::BeginChild("##LiveLog", ImVec2(0,0), true, ImGuiWindowFlags_HorizontalScrollbar)) {
            std::lock_guard<std::mutex> lock(gOverlayMutex);
            for (const auto& line : gOverlayLines) {
                if (line.find("error")!=std::string::npos || line.find("failed")!=std::string::npos ||
                    line.find("ERROR")!=std::string::npos)
                    ImGui::TextColored(ImVec4(.91f,.42f,.35f,1), "%s", line.c_str());
                else if (line.find("[GAME]")!=std::string::npos)
                    ImGui::TextColored(ImVec4(.61f,.75f,.57f,1), "%s", line.c_str());
                else ImGui::TextUnformatted(line.c_str());
            }
            if (scroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()-24.f)
                ImGui::SetScrollHereY(1.f);
        }
        ImGui::EndChild();
    }
    ImGui::End();
    ImGui::PopStyleColor(4);
}
