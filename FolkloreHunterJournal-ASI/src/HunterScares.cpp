// The Hunter's Journal: original in-process ImGui scare effects.
// No copyrighted assets, files, child processes or separate executables required.
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <mmsystem.h>
#include <imgui.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <cfloat>
#include <string>
#include <thread>
#include <vector>
#include "HunterScares.h"
#include "JournalDiagnostics.h"
#pragma comment(lib, "winmm.lib")

namespace {
    constexpr float PI=3.14159265358979323846f;
    enum class Mode : int {
        CryptidLunge, Eyes, DeathScreen, Radio, Watcher, Static, Heartbeat, Whisper, BloodMoon, Omen
    };
    constexpr const char* Titles[]={
        "CRYPTID LUNGE", "EYES BEHIND YOU", "FAKE DEATH SCREEN", "HAUNTED RADIO",
        "THE WATCHER", "STATIC BURST", "HEARTBEAT PANIC", "THE WHISPER",
        "BLOOD MOON", "CURSED OMEN"
    };
    struct State {
        Mode mode=Mode::CryptidLunge;
        ULONGLONG start=0;
        ULONGLONG delayUntil=0;
        bool armed=false;
        bool soundStarted=false;
        bool muted=false;
        bool visual=true;
        float duration=1.45f;
        float intensity=0.72f;
        float volume=0.55f;
        bool hideJournal=true;
    };
    State g;
    bool gHideRequest=false;
    std::atomic_bool gAudioBusy{false};

    ImU32 C(int r,int green,int b,int a=255) {
        return IM_COL32(r,green,b,std::clamp(a,0,255));
    }
    void Text(ImDrawList* d,ImVec2 size,const char* str,float y,float pixels,ImU32 tint) {
        ImFont* f=ImGui::GetFont();
        const ImVec2 measured=f->CalcTextSizeA(pixels,FLT_MAX,0.f,str);
        d->AddText(f,pixels,ImVec2((size.x-measured.x)*0.5f,y),tint,str);
    }
    void Eyes(ImDrawList* dl,ImVec2 c,float size,ImU32 color, bool wendigo) {
        for(int dir : {-1,1}) {
            ImVec2 eye(c.x+dir*size*.38f,c.y-size*.18f);
            dl->AddEllipseFilled(eye,ImVec2(size*.18f,size*.105f),C(13,7,7),0,20);
            dl->AddEllipseFilled(eye,ImVec2(size*.10f,size*.075f),color,0,20);
            dl->AddCircleFilled(ImVec2(eye.x-dir*size*.03f,eye.y),size*.025f,C(255,211,170),12);
        }
        if(wendigo) {
            // Abstract antlers and curved mouth suggest a woodland cryptid.
            for(int dir : {-1,1}) {
                ImVec2 x(c.x+dir*size*.35f,c.y-size*.68f);
                ImVec2 a(c.x+dir*size*.49f,c.y-size*1.10f);
                ImVec2 b(c.x+dir*size*.85f,c.y-size*1.3f);
                dl->AddLine(x,a,C(67,57,43),5);
                dl->AddLine(a,b,C(67,57,43),4);
                dl->AddLine(a,ImVec2(c.x+dir*size*.42f,c.y-size*1.45f),C(67,57,43),4);
                dl->AddLine(b,ImVec2(c.x+dir*size*1.02f,c.y-size*1.67f),C(67,57,43),3);
            }
            dl->AddEllipseFilled(ImVec2(c.x,c.y+size*.26f),ImVec2(size*.27f,size*.40f),C(7,3,4),0,32);
            for(int i=-3;i<=3;++i)
                dl->AddLine(ImVec2(c.x+i*size*.07f,c.y+size*.04f),
                    ImVec2(c.x+i*size*.045f,c.y+size*.27f),C(184,171,138),2);
        }
    }
    void Figure(ImDrawList* dl,ImVec2 c,float scale, bool antlers,ImU32 eyes) {
        dl->AddEllipseFilled(ImVec2(c.x,c.y+scale*.68f),ImVec2(scale*.70f,scale*1.05f),C(3,6,6,245));
        dl->AddEllipseFilled(ImVec2(c.x,c.y-scale*.06f),ImVec2(scale*.62f,scale*.76f),C(6,8,8,255));
        Eyes(dl,c,scale,eyes,antlers);
    }
    void Pulse(ImDrawList* dl,ImVec2 size,float p,float alpha) {
        const float radius=std::min(size.x,size.y)*(.14f+.55f*p);
        const ImU32 red=C(135,27,20,static_cast<int>(alpha*185.f));
        for(int i=0;i<3;++i)
            dl->AddCircle(ImVec2(size.x*.5f,size.y*.5f),
                radius+i*radius*.08f,red,72,1.0f);
    }
    // Single, original synthesized cinematic stinger, max ~0.6 seconds.
    // PlaySound with SND_MEMORY requires SND_SYNC; run it off the render thread.
    std::vector<unsigned char> MakeWav(Mode mode,float volume) {
        constexpr int Hz=22050;
        constexpr int length=13230;
        std::vector<unsigned char> wave(44+length*2,0);
        auto write16=[&](int ofs,uint16_t x){
            wave[static_cast<size_t>(ofs)]=static_cast<unsigned char>(x);
            wave[static_cast<size_t>(ofs+1)]=static_cast<unsigned char>(x>>8);
        };
        auto write32=[&](int ofs,uint32_t x){ write16(ofs,static_cast<uint16_t>(x));write16(ofs+2,static_cast<uint16_t>(x>>16)); };
        auto fourcc=[&](int ofs,const char* x){for(int i=0;i<4;++i)wave[static_cast<size_t>(ofs+i)]=static_cast<unsigned char>(x[i]);};
        fourcc(0,"RIFF");write32(4,36+length*2);fourcc(8,"WAVE");
        fourcc(12,"fmt ");write32(16,16);write16(20,1);write16(22,1);
        write32(24,Hz);write32(28,Hz*2);write16(32,2);write16(34,16);
        fourcc(36,"data");write32(40,length*2);
        uint32_t noise=0x62B7D8A1u;
        const int style=static_cast<int>(mode);
        for(int n=0;n<length;++n) {
            float t=static_cast<float>(n)/Hz;
            float fade=std::min(1.f,t*12.f)*std::min(1.f,(.6f-t)*4.f);
            noise^=noise<<13;noise^=noise>>17;noise^=noise<<5;
            float hiss=static_cast<float>(static_cast<int32_t>(noise))/2147483648.f;
            float base=65.f + style*7.f;
            float trem=std::sin(2.f*PI*(base*t+40.f*t*t));
            float rumble=std::sin(2.f*PI*(base*.53f*t));
            float result=(trem*.35f+rumble*.25f+hiss*.4f)*fade*std::clamp(volume,0.f,0.8f);
            result=std::clamp(result,-1.f,1.f);
            const auto sample=static_cast<int16_t>(result*26000.f);
            write16(44+n*2,static_cast<uint16_t>(sample));
        }
        return wave;
    }
    void Sound(Mode mode,float volume) {
        if(gAudioBusy.exchange(true))return;
        std::thread([mode,volume]() {
            auto data=MakeWav(mode,volume);
            PlaySoundA(reinterpret_cast<const char*>(data.data()),nullptr,
                SND_MEMORY|SND_SYNC|SND_NODEFAULT);
            gAudioBusy.store(false);
        }).detach();
    }
    void Queue(Mode mode) {
        g.mode=mode;
        g.start=0;
        g.delayUntil=GetTickCount64()+1100;
        g.armed=true;
        g.soundStarted=false;
        if(g.hideJournal)gHideRequest=true;
        FHJDiag::Log(L"LOCAL cinematic jump scare armed (1.1 second countdown).");
    }
}
bool HunterScares::IsScareActive() { return g.armed; }
bool HunterScares::ConsumeHideJournal() {
    bool hide=gHideRequest;
    gHideRequest=false;
    return hide;
}
void HunterScares::DrawControls() {
    ImGui::TextColored(ImVec4(.78f,.60f,.36f,1),"LOCAL JUMP SCARES / FIRST-PERSON HORROR EFFECTS");
    ImGui::TextWrapped("These visuals and original sound stingers play on YOUR computer. An unmodded remote player cannot see the mod's ImGui overlay.");
    static int selected=0;
    if(ImGui::BeginChild("##ScareModes",ImVec2(0,204),true)) {
        for(int i=0;i<static_cast<int>(std::size(Titles));++i) {
            ImGui::PushID(i);
            if(ImGui::Selectable(Titles[i],i==selected,0,ImVec2(0,29))) selected=i;
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::SliderFloat("Scare visual intensity",&g.intensity,.25f,1.f,"%.2f");
    ImGui::SliderFloat("Sound volume",&g.volume,0.f,.8f,"%.2f");
    ImGui::SliderFloat("Duration (seconds)",&g.duration,.6f,3.f,"%.1fs");
    ImGui::Checkbox("Mute audio",&g.muted);
    ImGui::SameLine();
    ImGui::Checkbox("Auto-hide journal when triggered",&g.hideJournal);
    if(ImGui::Button("TRIGGER LOCAL JUMP SCARE",ImVec2(-1,36)))
        Queue(static_cast<Mode>(selected));
    ImGui::TextDisabled("Single slow fade only; no fast white flashes or screen locking.");
    ImGui::Separator();
}
void HunterScares::DrawFrame(const ImVec2& screen) {
    if(!g.armed || screen.x<100 || screen.y<100) return;
    const auto now=GetTickCount64();
    if(now<g.delayUntil) return;
    if(!g.start)g.start=now;
    if(!g.soundStarted && !g.muted) {
        g.soundStarted=true;
        Sound(g.mode,g.volume);
    }
    const float age=static_cast<float>(now-g.start)*.001f;
    if(age>g.duration) {g.armed=false;return;}
    const float fraction=std::clamp(age/g.duration,0.f,1.f);
    const float arrive=std::min(1.f,age*5.f);
    const float disappear=std::clamp((g.duration-age)*2.2f,0.f,1.f);
    const float strength=arrive*disappear*g.intensity;
    ImDrawList* dl=ImGui::GetForegroundDrawList();
    const int alpha=static_cast<int>(strength*216.f);
    const float w=screen.x,h=screen.y;
    const ImVec2 center{w*.5f,h*.50f};
    const float base=std::min(w,h)*(.32f+fraction*.22f);
    if(g.mode==Mode::BloodMoon) {
        dl->AddRectFilled(ImVec2(0,0),screen,C(29,2,2,alpha));
        dl->AddCircleFilled(ImVec2(w*.77f,h*.25f),std::min(w,h)*.17f,C(157,24,23,static_cast<int>(strength*190)),64);
        Text(dl,screen,"THE BLOOD MOON RISES",h*.68f,34.f,C(218,111,87,alpha));
        return;
    }
    dl->AddRectFilled(ImVec2(0,0),screen,C(4,3,5,alpha));
    switch(g.mode) {
        case Mode::CryptidLunge:
            Figure(dl,center,base,true,C(241,61,34,alpha));
            Text(dl,screen,"IT FOUND YOU",h*.83f,34.f,C(206,161,110,alpha));
            break;
        case Mode::Eyes:
            Eyes(dl,center,std::min(w,h)*.68f,C(252,78,44,alpha),false);
            Text(dl,screen,"DON'T TURN AROUND",h*.83f,31.f,C(199,159,122,alpha));
            break;
        case Mode::DeathScreen:
            Text(dl,screen,"YOUR HUNT IS OVER",h*.40f,48.f,C(196,43,34,alpha));
            Text(dl,screen,"... OR IS IT?",h*.55f,25.f,C(203,169,132,alpha));
            break;
        case Mode::Radio:
            for(int i=0;i<42;++i) {
                const float y=static_cast<float>(i)*h/42.f;
                if((i+(now/47u)%7u)%4u==0)
                    dl->AddLine(ImVec2(0,y),ImVec2(w,y),C(114,143,119,static_cast<int>(strength*68)),1.f);
            }
            Text(dl,screen,"EMERGENCY FREQUENCY",h*.37f,38.f,C(178,217,160,alpha));
            Text(dl,screen,"SOMETHING IS BEHIND YOU",h*.49f,28.f,C(218,189,149,alpha));
            break;
        case Mode::Watcher:
            Figure(dl,ImVec2(w*.82f,h*.47f),base*.52f,false,C(234,36,26,alpha));
            Text(dl,screen,"SOMEONE IS WATCHING",h*.75f,28.f,C(191,166,135,alpha));
            break;
        case Mode::Static:
            for(int i=0;i<80;++i) {
                const uint32_t seed=static_cast<uint32_t>(i*7193+now/31u*1973);
                const float x=static_cast<float>(seed%1000u)*w/1000.f;
                const float y=static_cast<float>((seed*13u)%997u)*h/997.f;
                const float len=w*(.05f+((seed>>8u)%80u)*.006f);
                dl->AddLine(ImVec2(x,y),ImVec2(std::min(w,x+len),y),C(196,198,171,static_cast<int>(strength*111)),1.f);
            }
            Text(dl,screen,"SIGNAL LOST",h*.46f,53.f,C(187,206,165,alpha));
            break;
        case Mode::Heartbeat:
            Pulse(dl,screen,fraction,strength);
            Text(dl,screen,"I CAN HEAR YOUR HEART",h*.51f,29.f,C(191,77,58,alpha));
            break;
        case Mode::Whisper:
            Text(dl,screen,"HUSH...",h*.35f,51.f,C(161,177,144,alpha));
            Text(dl,screen,"I KNOW WHERE YOU ARE",h*.58f,28.f,C(202,187,148,alpha));
            break;
        case Mode::Omen:
            Figure(dl,ImVec2(w*.25f,h*.46f),base*.57f,true,C(196,31,25,alpha));
            Text(dl,screen,"YOU SHOULDN'T BE HERE",h*.72f,31.f,C(216,180,133,alpha));
            break;
        default: break;
    }
}
