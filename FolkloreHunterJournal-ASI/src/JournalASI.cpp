// Folklore Hunter — The Hunter's Journal
// Native 64-bit ASI plugin: no UE4SS dependency and no gameplay writes.
// DirectX 11 Present/ResizeBuffers rendering proof-of-concept.
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <d3d11on12.h>
#include <MinHook.h>
#include <imgui.h>
#include <backends/imgui_impl_win32.h>
// Dear ImGui intentionally omits the HWND-dependent handler declaration in its header.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#include <backends/imgui_impl_dx11.h>
#include "JournalUI.h"
#include "JournalDiagnostics.h"
#include "HunterNative.h"
#include "HunterScares.h"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <deque>
#include <utility>
#include <mutex>
#include <string>
#include <cwctype>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

static HMODULE gJournalModule = nullptr;
static HANDLE gOverlayOwnerMutex = nullptr;
namespace {
    using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    PresentFn gOriginalPresent = nullptr;
    PresentFn gOriginalPresent12 = nullptr;
    ResizeFn gOriginalResize12 = nullptr;
    using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*);
    ExecuteFn gOriginalExecute = nullptr;
    ID3D12CommandQueue* gQueue = nullptr;
    std::mutex gQueueLock;
    ID3D11On12Device* gOn12 = nullptr;
    ID3D11Resource* gWrapped[16]{};
    ID3D11RenderTargetView* gDX12RTV[16]{};
    UINT gBufferCount12 = 0;
    bool gIsDX12 = false;
    std::atomic_bool gWaitingForQueueLogged{false};
    ResizeFn gOriginalResize = nullptr;
    HWND gGameWindow = nullptr;
    WNDPROC gOriginalWndProc = nullptr;
    ID3D11Device* gDevice = nullptr;
    ID3D11DeviceContext* gContext = nullptr;
    ID3D11RenderTargetView* gRenderTarget = nullptr;
    IDXGISwapChain* gCurrentSwapChain = nullptr; // non-owning: pointer identity only
    std::atomic_bool gShowMenu{false};
    std::atomic_bool gRenderReady{false};
    std::atomic_bool gLoggedUnsupported{false};
    std::mutex gInputMutex;
    struct QueuedMessage { HWND hwnd; UINT message; WPARAM wparam; LPARAM lparam; };
    std::deque<QueuedMessage> gInputQueue;
    RECT gClipBeforeJournal{};
    bool gClipSaved = false;

    // One DXGI hook owner, separate module callbacks. Both plugins may
    // coexist without a second MinHook patch of the same Present address.
    using BridgeAttach=void (*)(void*,HWND);
    using BridgeFrame=void (*)(void*,bool);
    using BridgeMessage=void (*)(UINT);
    using BridgeMouse=void (*)(LPARAM);
    using BridgeBool=bool (*)();
    using BridgeVoid=void (*)();
    HMODULE gCompanionModule=nullptr;
    BridgeAttach gBridgeAttach=nullptr;
    BridgeFrame gBridgeFrame=nullptr;
    BridgeMessage gBridgeMessage=nullptr;
    BridgeMouse gBridgeMouse=nullptr;
    BridgeBool gBridgeMonsterActive=nullptr;
    BridgeVoid gBridgeRestoreHuman=nullptr;
    std::atomic_bool gBridgeReady{false};

    void TryConnectCompanion() {
        if(gBridgeReady.load(std::memory_order_acquire) ||
           !gGameWindow || !ImGui::GetCurrentContext())return;
#ifdef FCH_STANDALONE
        constexpr const wchar_t* other=L"FolkloreHunterJournal.asi";
#else
        constexpr const wchar_t* other=L"FolkloreCryptidHunt.asi";
#endif
        const auto module=GetModuleHandleW(other);
        if(!module || module==gJournalModule)return;
        auto attach=reinterpret_cast<BridgeAttach>(
            GetProcAddress(module,"FHJ_ModuleBridgeAttach"));
        auto frame=reinterpret_cast<BridgeFrame>(
            GetProcAddress(module,"FHJ_ModuleBridgeFrame"));
        auto message=reinterpret_cast<BridgeMessage>(
            GetProcAddress(module,"FHJ_ModuleBridgeMessage"));
        auto mouse=reinterpret_cast<BridgeMouse>(
            GetProcAddress(module,"FHJ_ModuleBridgeMouse"));
        auto active=reinterpret_cast<BridgeBool>(
            GetProcAddress(module,"FHJ_ModuleBridgeMonsterActive"));
        auto restore=reinterpret_cast<BridgeVoid>(
            GetProcAddress(module,"FHJ_ModuleBridgeRestoreHuman"));
        if(!attach || !frame || !message || !mouse || !active || !restore)
            return;
        // Only publish pointers to WndProc after the second module has
        // received the owning ImGui context and game HWND.
        gCompanionModule=module;
        gBridgeAttach=attach;
        gBridgeFrame=frame;
        gBridgeMessage=message;
        gBridgeMouse=mouse;
        gBridgeMonsterActive=active;
        gBridgeRestoreHuman=restore;
        attach(static_cast<void*>(ImGui::GetCurrentContext()),gGameWindow);
        gBridgeReady.store(true,std::memory_order_release);
        Log(L"Shared hook bridge: companion ASI registered independent UI/engine callbacks; no second DXGI or WndProc patch.");
    }
    bool CompanionMonsterActive() {
        return gBridgeReady.load(std::memory_order_acquire) &&
               gBridgeMonsterActive && gBridgeMonsterActive();
    }

    void ReleaseGamePointer(HWND hwnd) {
        // Unreal may use a confined or relative mouse while the game is playing.
        // The journal needs a free cursor, and will draw its own pointer via ImGui.
        if (GetClipCursor(&gClipBeforeJournal)) gClipSaved = true;
        ClipCursor(nullptr);
        if (GetCapture() == hwnd) ReleaseCapture();
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        FHJDiag::Log(L"Journal cursor unlocked; game mouse capture released.");
    }
    void RestoreGamePointer() {
        if (gClipSaved) ClipCursor(&gClipBeforeJournal);
        gClipSaved = false;
        FHJDiag::Log(L"Journal closed; restored previous game cursor confinement.");
    }


    void Log(const wchar_t* message) {
        FHJDiag::Log(message);
    }

    void ReleaseDX12Targets() {
        if (gContext) gContext->Flush();
        for (UINT i=0;i<16;++i) {
            if (gDX12RTV[i]) { gDX12RTV[i]->Release(); gDX12RTV[i]=nullptr; }
            if (gWrapped[i]) { gWrapped[i]->Release(); gWrapped[i]=nullptr; }
        }
        gBufferCount12=0;
    }
    bool CreateDX12Targets(IDXGISwapChain* chain) {
        if (!gOn12 || !gDevice) return false;
        ReleaseDX12Targets();
        DXGI_SWAP_CHAIN_DESC desc{};
        if (FAILED(chain->GetDesc(&desc)) || !desc.BufferCount || desc.BufferCount>16) return false;
        for (UINT i=0;i<desc.BufferCount;++i) {
            ID3D12Resource* resource=nullptr;
            if (FAILED(chain->GetBuffer(i,IID_PPV_ARGS(&resource))) || !resource) {
                ReleaseDX12Targets(); return false;
            }
            D3D11_RESOURCE_FLAGS flags{};
            flags.BindFlags = D3D11_BIND_RENDER_TARGET;
            HRESULT hr=gOn12->CreateWrappedResource(resource,&flags,
                D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT,
                IID_PPV_ARGS(&gWrapped[i]));
            resource->Release();
            if (FAILED(hr) || !gWrapped[i] ||
                FAILED(gDevice->CreateRenderTargetView(gWrapped[i],nullptr,&gDX12RTV[i]))) {
                ReleaseDX12Targets(); return false;
            }
        }
        gBufferCount12=desc.BufferCount;
        return true;
    }
    void STDMETHODCALLTYPE ExecuteHook(ID3D12CommandQueue* queue,UINT n,ID3D12CommandList* const* lists) {
        if (queue && queue->GetDesc().Type==D3D12_COMMAND_LIST_TYPE_DIRECT) {
            std::lock_guard<std::mutex> lock(gQueueLock);
            if (!gQueue) {
                queue->AddRef();
                gQueue=queue;
                Log(L"DX12 direct command queue captured.");
            }
        }
        if (gOriginalExecute) gOriginalExecute(queue,n,lists);
    }
    void ReleaseRenderTarget() {
        if (gRenderTarget) { gRenderTarget->Release(); gRenderTarget = nullptr; }
    }

    bool CreateRenderTarget(IDXGISwapChain* chain) {
        ReleaseRenderTarget();
        if (!gDevice) return false;
        if (gIsDX12) return CreateDX12Targets(chain);
        ID3D11Texture2D* backBuffer = nullptr;
        if (FAILED(chain->GetBuffer(0, __uuidof(ID3D11Texture2D),
             reinterpret_cast<void**>(&backBuffer)))) return false;
        HRESULT result = gDevice->CreateRenderTargetView(backBuffer, nullptr, &gRenderTarget);
        backBuffer->Release();
        return SUCCEEDED(result);
    }

    LRESULT CALLBACK JournalWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        if (msg == WM_INPUT &&
            (HunterNative::MonsterModeActive() || CompanionMonsterActive())) {
            if(HunterNative::MonsterModeActive())
                HunterNative::FeedRawMouse(lparam);
            if(CompanionMonsterActive())gBridgeMouse(lparam);
            return DefWindowProcW(hwnd,msg,wparam,lparam);
        }
        if (msg == WM_KEYUP && wparam == VK_F11 &&
            (HunterNative::MonsterModeActive() || CompanionMonsterActive())) {
            if(HunterNative::MonsterModeActive())HunterNative::RequestRestoreHuman();
            if(CompanionMonsterActive())gBridgeRestoreHuman();
            return 0;
        }
        if (msg == HunterNative::kGameTick || msg == HunterNative::kGameAction) {
            HunterNative::HandleWindowMessage(msg);
            if(gBridgeReady.load(std::memory_order_acquire) &&
               gBridgeMessage)gBridgeMessage(msg);
            return 0;
        }
        if (msg == WM_KEYUP && wparam == VK_F9) { FHJDiag::ToggleOverlay(); return 0; }
        if (msg == WM_KEYUP && wparam == VK_F10) { FHJDiag::ToggleNativeConsole(); return 0; }
        if (msg == WM_KEYUP && wparam == VK_F8) {
            FHJDiag::Stage("F8 pressed", L"F8 key detected; toggling journal and mouse ownership.");
            const bool opened = !gShowMenu.load(std::memory_order_relaxed);
            gShowMenu.store(opened, std::memory_order_release);
            if (opened) ReleaseGamePointer(hwnd);
            else RestoreGamePointer();
            return 0;
        }
        // Only puppet mode takes the human pawn's controls. Noclip keeps the
        // normal game input pipeline active so walking keys are not starved.
        if(!gShowMenu.load(std::memory_order_acquire) &&
           (HunterNative::MonsterModeActive() || CompanionMonsterActive())) {
            // Q/E/R ability presses are polled as key-down edges by the
            // native cryptid tick. Do not dispatch again on WM_KEYUP: holding
            // E used to activate and then deactivate invisibility on release.
            if(msg==WM_KEYUP && (wparam=='Q'||wparam=='E'||wparam=='R'))
                return 0;
#ifdef FCH_STANDALONE
            if((msg==WM_KEYDOWN || msg==WM_KEYUP) &&
               (wparam=='C' || wparam=='V'))
                return 0; // Wendigo's private watchpoint shortcuts
#endif
            if((HunterNative::MonsterModeActive() || CompanionMonsterActive()) &&
                (msg==WM_LBUTTONDOWN || msg==WM_LBUTTONUP))return 0;
            if((msg==WM_KEYDOWN || msg==WM_KEYUP) &&
               (wparam=='W'||wparam=='A'||wparam=='S'||wparam=='D'||
                wparam==VK_SPACE || wparam==VK_CONTROL || wparam==VK_SHIFT ||
                ((HunterNative::MonsterModeActive() || CompanionMonsterActive()) &&
                (wparam=='Q' || wparam=='E' || wparam=='R' ||
                 wparam=='C' || wparam=='V'))))return 0;
        }
        if (gShowMenu.load(std::memory_order_acquire) && msg == WM_SETCURSOR) {
            SetCursor(nullptr); // Dear ImGui draws the visible cursor above the game.
            return TRUE;
        }
        if (gShowMenu.load(std::memory_order_acquire) && msg == WM_INPUT) {
            // Avoid passing relative raw mouse input to the game while editing the journal.
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        }
        if (gRenderReady.load(std::memory_order_acquire) && gShowMenu.load(std::memory_order_acquire)) {
            // Never call Dear ImGui from a different thread than Present.
            // Queue input messages; the render thread consumes them before NewFrame().
            const bool mouse = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) ||
                msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL || msg == WM_MOUSELEAVE;
            const bool keyboard = (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||
                msg == WM_CHAR || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP;
            if (mouse || keyboard) {
                std::lock_guard lock(gInputMutex);
                if (gInputQueue.size() >= 512) gInputQueue.pop_front();
                gInputQueue.push_back({hwnd, msg, wparam, lparam});
                return 1; // avoid triggering gameplay controls while the journal is open
            }
        }
        return gOriginalWndProc ? CallWindowProcW(gOriginalWndProc, hwnd, msg, wparam, lparam) : DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    void ConsumeQueuedInput() {
        std::deque<QueuedMessage> pending;
        {
            std::lock_guard lock(gInputMutex);
            pending.swap(gInputQueue);
        }
        for (const QueuedMessage& e : pending) {
            ImGui_ImplWin32_WndProcHandler(e.hwnd, e.message, e.wparam, e.lparam);
        }
    }

    bool InitializeRenderer(IDXGISwapChain* chain) {
        if (gCurrentSwapChain && chain != gCurrentSwapChain) return false;
        ID3D11Device* device=nullptr;
        gIsDX12=false;
        if (FAILED(chain->GetDevice(IID_PPV_ARGS(&device)))) {
            ID3D12Device* device12=nullptr;
            if (FAILED(chain->GetDevice(IID_PPV_ARGS(&device12))) || !device12) return false;
            ID3D12CommandQueue* queue=nullptr;
            {
                std::lock_guard<std::mutex> lock(gQueueLock);
                if (gQueue) {
                    ID3D12Device* source=nullptr;
                    if (SUCCEEDED(gQueue->GetDevice(IID_PPV_ARGS(&source)))) {
                        IUnknown *a=nullptr,*b=nullptr;
                        source->QueryInterface(IID_PPV_ARGS(&a));
                        device12->QueryInterface(IID_PPV_ARGS(&b));
                        if (a && b && a==b) { queue=gQueue; queue->AddRef(); }
                        if (a) a->Release();
                        if (b) b->Release();
                        source->Release();
                    }
                }
            }
            if (!queue) {
                if (!gWaitingForQueueLogged.exchange(true))
                    Log(L"DX12 swapchain detected: waiting for game's direct command queue.");
                device12->Release(); return false;
            }
            FHJDiag::Stage("DX12 bridge",L"Initializing D3D11On12 with captured DX12 queue.");
            D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_11_0;
            IUnknown* commandQueues[]={queue};
            ID3D11DeviceContext* temporaryContext=nullptr;
            HRESULT hr=D3D11On12CreateDevice(device12,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                &level,1,commandQueues,1,0,&device,&temporaryContext,nullptr);
            queue->Release(); device12->Release();
            if (FAILED(hr) || !device || !temporaryContext) {
                if (temporaryContext) temporaryContext->Release();
                if (device) device->Release();
                Log(L"DX12: D3D11On12CreateDevice failed.");
                return false;
            }
            hr=device->QueryInterface(IID_PPV_ARGS(&gOn12));
            temporaryContext->Release();
            if (FAILED(hr) || !gOn12) {
                device->Release(); Log(L"DX12: ID3D11On12Device unavailable.");
                return false;
            }
            gIsDX12=true;
        } else FHJDiag::Stage("DX11 renderer",L"Native DX11 swapchain acquired.");
        DXGI_SWAP_CHAIN_DESC desc{};
        if (FAILED(chain->GetDesc(&desc)) || !desc.OutputWindow || !IsWindow(desc.OutputWindow)) {
            device->Release();
            return false;
        }
        DWORD owningPid = 0;
        GetWindowThreadProcessId(desc.OutputWindow, &owningPid);
        if (owningPid != GetCurrentProcessId()) { device->Release(); return false; }

        gGameWindow = desc.OutputWindow;
        gDevice = device;
        gDevice->GetImmediateContext(&gContext);
        if (!gContext || !CreateRenderTarget(chain)) {
            ReleaseRenderTarget();
            if (gContext) { gContext->Release(); gContext = nullptr; }
            gDevice->Release(); gDevice = nullptr;
            if (gOn12) { gOn12->Release(); gOn12=nullptr; }
            gIsDX12=false;
            return false;
        }

        FHJDiag::Stage("create ImGui context", L"DX11 swapchain acquired; initializing ImGui.");
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        ImGuiStyle& hunterStyle = ImGui::GetStyle();
        hunterStyle.WindowRounding = 5.0f;
        hunterStyle.WindowBorderSize = 1.0f;
        hunterStyle.Colors[ImGuiCol_TitleBg] = ImVec4(.09f, .12f, .09f, 1.f);
        hunterStyle.Colors[ImGuiCol_TitleBgActive] = ImVec4(.18f, .23f, .15f, 1.f);
        hunterStyle.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(.07f, .09f, .07f, 1.f);
        hunterStyle.Colors[ImGuiCol_Border] = ImVec4(.43f, .34f, .22f, 1.f);
        hunterStyle.Colors[ImGuiCol_CheckMark] = ImVec4(.75f, .60f, .38f, 1.f);
        hunterStyle.Colors[ImGuiCol_ResizeGrip] = ImVec4(.38f, .40f, .28f, .65f);
        hunterStyle.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(.65f, .55f, .32f, .82f);
        hunterStyle.Colors[ImGuiCol_ResizeGripActive] = ImVec4(.79f, .65f, .36f, 1.f);
        ImGuiIO& io = ImGui::GetIO();
        // Installed Windows font; not bundled or redistributed.
        if (GetFileAttributesA("C:\\Windows\\Fonts\\georgia.ttf") != INVALID_FILE_ATTRIBUTES)
            io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\georgia.ttf", 18.0f);
        if (io.Fonts->Fonts.empty()) io.Fonts->AddFontDefault();
        io.IniFilename = nullptr; // journal manages its own settings
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        if (!ImGui_ImplWin32_Init(gGameWindow) || !ImGui_ImplDX11_Init(gDevice, gContext)) {
            Log(L"ImGui initialization failed");
            ImGui::DestroyContext();
            ReleaseRenderTarget();
            gContext->Release(); gContext = nullptr;
            gDevice->Release(); gDevice = nullptr;
            return false;
        }
        SetLastError(0);
        auto prev = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(gGameWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&JournalWndProc)));
        if (!prev) {
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            ReleaseRenderTarget();
            gContext->Release(); gContext = nullptr;
            gDevice->Release(); gDevice = nullptr;
            Log(L"Window procedure hook failed");
            return false;
        }
        gOriginalWndProc = prev;
        HunterNative::AttachWindow(gGameWindow);
        FHJDiag::Stage("window procedure installed", L"Game window procedure hook installed; waiting for F8.");
        gCurrentSwapChain = chain;
        gRenderReady.store(true, std::memory_order_release);
        Log(gIsDX12 ? L"Journal initialized via DX12/D3D11On12." : L"Journal initialized via DX11.");
        return true;
    }

    HRESULT STDMETHODCALLTYPE PresentCore(IDXGISwapChain* chain, UINT sync, UINT flags, PresentFn original) {
        if (!original) return E_FAIL;
        if ((flags & DXGI_PRESENT_TEST) != 0) return original(chain, sync, flags);
        if (!gRenderReady.load(std::memory_order_acquire)) InitializeRenderer(chain);
        if (gRenderReady.load(std::memory_order_acquire) && chain == gCurrentSwapChain && gContext) {
            // Always progress the frame, including when the journal is hidden.
            FHJDiag::SetStage("rendering ImGui DX11 frame");
            TryConnectCompanion();
            ConsumeQueuedInput();
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            const bool editingJournal = gShowMenu.load(std::memory_order_acquire);
            ImGui::GetIO().MouseDrawCursor = editingJournal;
            HunterNative::SetJournalOpen(editingJournal);
            // The game can re-lock the pointer every tick: keep it unconstrained while editing.
            // Do not affect desktop input when Folklore Hunter is not the foreground window.
            if (editingJournal && GetForegroundWindow() == gGameWindow)
                ClipCursor(nullptr);
            ImGui::NewFrame();
            HunterNative::RequestTick();
            HunterNative::DrawESP(ImGui::GetIO().DisplaySize);
#ifdef FCH_STANDALONE
            FHJ::DrawCryptidHuntLoadBanner();
            HunterNative::DrawCryptidHuntDaylightVision(ImGui::GetIO().DisplaySize);
#endif
            HunterNative::DrawMonsterHUD(ImGui::GetIO().DisplaySize);
            if(gBridgeReady.load(std::memory_order_acquire) && gBridgeFrame)
                gBridgeFrame(static_cast<void*>(ImGui::GetCurrentContext()),
                             editingJournal);
            FHJDiag::PollGameLog();
            if (gShowMenu.load(std::memory_order_relaxed)) FHJ::DrawJournal();
            if (HunterScares::ConsumeHideJournal() &&
                gShowMenu.exchange(false,std::memory_order_acq_rel))
                RestoreGamePointer();
            FHJDiag::DrawOverlay(gIsDX12 ? "DX12 (11on12)" : "DX11", ImGui::GetIO().Framerate);
            HunterScares::DrawFrame(ImGui::GetIO().DisplaySize);
            ImGui::Render();
            // World ESP and custom monster HUD must be presented without the journal open.
            if (gShowMenu.load(std::memory_order_relaxed) || FHJDiag::OverlayVisible() ||
                HunterNative::ESPEnabled() || HunterNative::MonsterModeActive() ||
                HunterScares::IsScareActive() ||
                gBridgeReady.load(std::memory_order_acquire)
#ifdef FCH_STANDALONE
                || true // standalone mode badge on original main menu
#endif
                ) {
                if (!gIsDX12 && !gRenderTarget) CreateRenderTarget(chain);
                if (gIsDX12) {
                    IDXGISwapChain3* chain3=nullptr;
                    if (gOn12 && SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&chain3)))) {
                        UINT i=chain3->GetCurrentBackBufferIndex();
                        if (i<gBufferCount12 && gWrapped[i] && gDX12RTV[i]) {
                            gOn12->AcquireWrappedResources(&gWrapped[i],1);
                            gContext->OMSetRenderTargets(1,&gDX12RTV[i],nullptr);
                            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
                            gContext->OMSetRenderTargets(0,nullptr,nullptr);
                            gOn12->ReleaseWrappedResources(&gWrapped[i],1);
                            gContext->Flush();
                        }
                        chain3->Release();
                    }
                } else if (gRenderTarget) {
                    ID3D11RenderTargetView* previousRTV = nullptr;
                    ID3D11DepthStencilView* previousDSV = nullptr;
                    gContext->OMGetRenderTargets(1, &previousRTV, &previousDSV);
                    gContext->OMSetRenderTargets(1, &gRenderTarget, nullptr);
                    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
                    gContext->OMSetRenderTargets(1, &previousRTV, previousDSV);
                    if (previousRTV) previousRTV->Release();
                    if (previousDSV) previousDSV->Release();
                }
            }
        }
        FHJDiag::SetStage("call original Present");
        return original(chain, sync, flags);
    }

    HRESULT STDMETHODCALLTYPE JournalPresent(IDXGISwapChain* c,UINT s,UINT f) { return PresentCore(c,s,f,gOriginalPresent); }
    HRESULT STDMETHODCALLTYPE JournalPresent12(IDXGISwapChain* c,UINT s,UINT f) { return PresentCore(c,s,f,gOriginalPresent12); }
    HRESULT STDMETHODCALLTYPE ResizeCore(IDXGISwapChain* c,UINT n,UINT w,UINT h,DXGI_FORMAT fmt,UINT f,ResizeFn original) {
        if (!original) return E_FAIL;
        bool ours=gRenderReady.load(std::memory_order_acquire) && c==gCurrentSwapChain;
        if (ours) {
            if (gIsDX12) ReleaseDX12Targets();
            else ReleaseRenderTarget();
        }
        HRESULT hr=original(c,n,w,h,fmt,f);
        if (ours && SUCCEEDED(hr) && !CreateRenderTarget(c))
            Log(L"Render targets failed to rebuild after resize.");
        return hr;
    }
    HRESULT STDMETHODCALLTYPE JournalResize(IDXGISwapChain* c,UINT n,UINT w,UINT h,DXGI_FORMAT fmt,UINT f) { return ResizeCore(c,n,w,h,fmt,f,gOriginalResize); }
    HRESULT STDMETHODCALLTYPE JournalResize12(IDXGISwapChain* c,UINT n,UINT w,UINT h,DXGI_FORMAT fmt,UINT f) { return ResizeCore(c,n,w,h,fmt,f,gOriginalResize12); }

    LRESULT CALLBACK DummyWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    bool InstallDX11Hooks() {
        FHJDiag::Stage("install DX11 hooks", L"Creating temporary DX11 swapchain to resolve Present/ResizeBuffers.");
        WNDCLASSW wc{};
        wc.lpfnWndProc = DummyWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"FolkloreHunterJournalDX11Dummy";
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) { Log(L"DX11 hook: RegisterClassW failed."); return false; }
        HWND window = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
                                      0, 0, 200, 200, nullptr, nullptr, wc.hInstance, nullptr);
        if (!window) return false;

        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount = 2;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = window;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        IDXGISwapChain* dummy = nullptr;
        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* context = nullptr;
        D3D_FEATURE_LEVEL level{};
        HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                    nullptr, 0, D3D11_SDK_VERSION, &sd, &dummy, &device, &level, &context);
        if (FAILED(hr)) {
            hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                    nullptr, 0, D3D11_SDK_VERSION, &sd, &dummy, &device, &level, &context);
        }
        if (FAILED(hr) || !dummy) {
            Log(L"DX11 hook: could not create dummy swapchain; no hooks installed.");
            DestroyWindow(window);
            return false;
        }
        void** vtable = *reinterpret_cast<void***>(dummy);
        void* presentAddr = vtable[8];
        void* resizeAddr = vtable[13];
        MH_STATUS init = MH_Initialize();
        const bool weInitializedMinHook = init == MH_OK;
        bool success = weInitializedMinHook || init == MH_ERROR_ALREADY_INITIALIZED;
        bool hookedPresent = false, hookedResize = false;
        if (success) {
            hookedPresent = MH_CreateHook(presentAddr, &JournalPresent,
                            reinterpret_cast<void**>(&gOriginalPresent)) == MH_OK;
            success = hookedPresent;
        }
        if (success) {
            hookedResize = MH_CreateHook(resizeAddr, &JournalResize,
                            reinterpret_cast<void**>(&gOriginalResize)) == MH_OK;
            success = hookedResize;
        }
        if (success) success = MH_EnableHook(presentAddr) == MH_OK;
        if (success) success = MH_EnableHook(resizeAddr) == MH_OK;
        if (!success) {
            // Never remove other plug-ins' hooks or tear down their MinHook runtime.
            if (hookedPresent) { MH_DisableHook(presentAddr); MH_RemoveHook(presentAddr); }
            if (hookedResize) { MH_DisableHook(resizeAddr); MH_RemoveHook(resizeAddr); }
            if (weInitializedMinHook) MH_Uninitialize();
        }
        // DX12 swapchains may use a distinct DXGI vtable. Discover both with dummy devices.
        ID3D12Device* d12=nullptr;
        if (SUCCEEDED(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d12)))) {
            D3D12_COMMAND_QUEUE_DESC cd{};
            cd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
            ID3D12CommandQueue* queue=nullptr;
            if (SUCCEEDED(d12->CreateCommandQueue(&cd,IID_PPV_ARGS(&queue)))) {
                void** qvt=*reinterpret_cast<void***>(queue);
                if (MH_CreateHook(qvt[10],&ExecuteHook,reinterpret_cast<void**>(&gOriginalExecute))==MH_OK)
                    MH_EnableHook(qvt[10]);
                else Log(L"DX12: command queue hook failed.");
                IDXGIFactory4* factory=nullptr;
                if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
                    // Use a separate hidden HWND and a flip-model swapchain for DX12.
                    HWND hwnd12=CreateWindowExW(0,wc.lpszClassName,L"",WS_OVERLAPPEDWINDOW,
                        0,0,200,200,nullptr,nullptr,wc.hInstance,nullptr);
                    if (hwnd12) {
                        DXGI_SWAP_CHAIN_DESC1 desc12{};
                        desc12.Width=200;
                        desc12.Height=200;
                        desc12.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
                        desc12.SampleDesc.Count=1;
                        desc12.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
                        desc12.BufferCount=2;
                        desc12.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
                        IDXGISwapChain1* swap12=nullptr;
                        if (SUCCEEDED(factory->CreateSwapChainForHwnd(queue,hwnd12,&desc12,
                            nullptr,nullptr,&swap12)) && swap12) {
                            void** vt=*reinterpret_cast<void***>(swap12);
                            if (vt[8]!=presentAddr) {
                                if (MH_CreateHook(vt[8],&JournalPresent12,
                                    reinterpret_cast<void**>(&gOriginalPresent12))==MH_OK)
                                    MH_EnableHook(vt[8]);
                                else Log(L"DX12: Present hook creation failed.");
                            }
                            if (vt[13]!=resizeAddr) {
                                if (MH_CreateHook(vt[13],&JournalResize12,
                                    reinterpret_cast<void**>(&gOriginalResize12))==MH_OK)
                                    MH_EnableHook(vt[13]);
                                else Log(L"DX12: ResizeBuffers hook creation failed.");
                            }
                            Log(L"DX12 flip-model swapchain entrypoints discovered.");
                            swap12->Release();
                        } else Log(L"DX12 flip-model swapchain creation failed.");
                        DestroyWindow(hwnd12);
                    } else Log(L"DX12 dummy window creation failed.");
                    factory->Release();
                }
                queue->Release();
            }
            d12->Release();
        } else Log(L"DX12 dummy device unavailable; DX11 hooks remain active.");

        context->Release(); device->Release(); dummy->Release(); DestroyWindow(window);
        Log(success ? L"DX11 hook: Present and ResizeBuffers hooks enabled." : L"DX11 hook: MinHook setup failed.");
        return success;
    }

    DWORD WINAPI Startup(void*) {
        FHJDiag::Start(gJournalModule);
        FHJDiag::Stage("validate host process", L"Starting diagnostics on ASI worker thread.");
        wchar_t gamePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, gamePath, MAX_PATH);
        std::wstring name = std::filesystem::path(gamePath).filename().wstring();
        for (auto& c : name) c = static_cast<wchar_t>(towlower(c));
        if (name.find(L"folklorehunter") == std::wstring::npos) {
            Log(L"Host is not Folklore Hunter; disabling plugin without installing hooks.");
            return 0;
        }
        Log(L"Folklore Hunter executable confirmed. Native ASI loaded successfully.");
        // Only the first loaded plugin owns MinHook/DXGI. The second
        // registers UI/engine callbacks with that first module, using
        // a different module API rather than patching Present twice.
        gOverlayOwnerMutex=CreateMutexW(nullptr,TRUE,
                              L"Local\\FolkloreHunter_Journal_CryptidHunt_OneOverlay_v1");
        if(!gOverlayOwnerMutex || GetLastError()==ERROR_ALREADY_EXISTS) {
            Log(L"Companion ASI mode: skipping duplicate graphics/window hooks. Existing DXGI owner will discover and render this module through independent callbacks.");
            if(gOverlayOwnerMutex) {
                CloseHandle(gOverlayOwnerMutex);
                gOverlayOwnerMutex=nullptr;
            }
            return 0;
        }
        Log(GetModuleHandleW(L"d3d11.dll") ? L"Module: d3d11.dll loaded" : L"Module: d3d11.dll not loaded yet");
        Log(GetModuleHandleW(L"d3d12.dll") ? L"Module: d3d12.dll loaded" : L"Module: d3d12.dll not loaded yet");
        Log(L"Loaded graphics modules do not prove which Unreal rendering API is active.");
        if (FHJDiag::SafeMode()) {
            FHJDiag::Stage("safe mode ready", L"Safe mode initialized successfully; NO graphics hooks installed, so F8 does not open a menu.");
            Log(L"If this run is stable, edit %LOCALAPPDATA%\\FolkloreHunterJournal\\journal.ini and set SafeMode=0, then restart.");
            return 0;
        }
        FHJDiag::Stage("waiting for graphics DLLs", L"Waiting for dxgi.dll and d3d11.dll before installing experimental overlay hooks.");
        for (int retry = 0; retry < 80; ++retry) {
            if (GetModuleHandleW(L"dxgi.dll") && GetModuleHandleW(L"d3d11.dll")) break;
            Sleep(250);
        }
        FHJDiag::Stage("DX11 hook installation", L"Attempting to hook D3D11 Present/ResizeBuffers.");
        if (InstallDX11Hooks())
            Log(L"DX11/DX12 hook discovery finished. F8 journal, F9 in-game debug, F10 native console.");
        else
            Log(L"Graphics hook initialization failed; inspect journal.log.");
        return 0;
    }
}

// Companion ABI: both DLLs export these routines. Exactly one module
// owns Win32/DXGI hooks; the other accepts the shared ImGui context and
// HWND without hooking either. The native engines stay module-private.
extern "C" __declspec(dllexport) void FHJ_ModuleBridgeAttach(
    void* context,HWND hwnd) {
    if(!context || !hwnd)return;
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(context));
    HunterNative::AttachWindow(hwnd);
}
extern "C" __declspec(dllexport) void FHJ_ModuleBridgeFrame(
    void* context,bool menuOpen) {
    if(!context)return;
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(context));
    HunterNative::SetJournalOpen(menuOpen);
    HunterNative::RequestTick();
    HunterNative::DrawESP(ImGui::GetIO().DisplaySize);
#ifdef FCH_STANDALONE
    FHJ::DrawCryptidHuntLoadBanner();
    HunterNative::DrawCryptidHuntDaylightVision(ImGui::GetIO().DisplaySize);
#endif
    HunterNative::DrawMonsterHUD(ImGui::GetIO().DisplaySize);
    if(menuOpen)FHJ::DrawJournal();
}
extern "C" __declspec(dllexport) void FHJ_ModuleBridgeMessage(UINT message) {
    if(message==HunterNative::kGameTick ||
       message==HunterNative::kGameAction)
        HunterNative::HandleWindowMessage(message);
}
extern "C" __declspec(dllexport) void FHJ_ModuleBridgeMouse(LPARAM input) {
    HunterNative::FeedRawMouse(input);
}
extern "C" __declspec(dllexport) bool FHJ_ModuleBridgeMonsterActive() {
    return HunterNative::MonsterModeActive();
}
extern "C" __declspec(dllexport) void FHJ_ModuleBridgeRestoreHuman() {
    HunterNative::RequestRestoreHuman();
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        gJournalModule = module;
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, Startup, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}