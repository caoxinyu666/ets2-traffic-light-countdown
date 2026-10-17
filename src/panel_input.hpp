#pragma once
#include <windows.h>
#include <mutex>
#include <vector>
#include <algorithm>

// The game can repeatedly warp/lock its OS cursor. The panel therefore tracks
// raw relative motion in its own render-coordinate cursor, never GetCursorPos.
class PanelInput {
    std::mutex mutex;
    HWND window=nullptr;
    bool registered=false, previousFound=false;
    RAWINPUTDEVICE previous{};
    float x=200,y=150,width=1,height=1;
    bool left=false,right=false;
    int wheel=0;
public:
    struct Snapshot{float x,y;bool left,right;int wheel;};
    bool start(HWND hwnd,float w,float h){
        stop();window=hwnd;
        UINT count=0;
        if(GetRegisteredRawInputDevices(nullptr,&count,sizeof(RAWINPUTDEVICE))!=UINT(-1)&&count){
            std::vector<RAWINPUTDEVICE> devices(count);
            if(GetRegisteredRawInputDevices(devices.data(),&count,sizeof(RAWINPUTDEVICE))!=UINT(-1))
                for(const auto& d:devices)if(d.usUsagePage==1&&d.usUsage==2){previous=d;previousFound=true;break;}
        }
        {std::lock_guard<std::mutex> lock(mutex);width=std::max(1.f,w);height=std::max(1.f,h);x=std::min(240.f,width*.4f);y=std::min(150.f,height*.3f);left=right=false;wheel=0;}
        RAWINPUTDEVICE device{1,2,RIDEV_INPUTSINK|RIDEV_NOLEGACY,hwnd};
        registered=RegisterRawInputDevices(&device,1,sizeof(device))!=FALSE;
        return registered;
    }
    void stop(){
        if(registered){
            RAWINPUTDEVICE restore=previousFound?previous:RAWINPUTDEVICE{1,2,RIDEV_REMOVE,nullptr};
            RegisterRawInputDevices(&restore,1,sizeof(restore));
        }
        registered=false;previousFound=false;window=nullptr;
    }
    void updateBounds(float w,float h){std::lock_guard<std::mutex> lock(mutex);width=std::max(1.f,w);height=std::max(1.f,h);x=std::clamp(x,0.f,width-1);y=std::clamp(y,0.f,height-1);}
    void raw(const RAWMOUSE& mouse){
        std::lock_guard<std::mutex> lock(mutex);
        if(mouse.usFlags&MOUSE_MOVE_ABSOLUTE){
            int sx=mouse.usFlags&MOUSE_VIRTUAL_DESKTOP?GetSystemMetrics(SM_XVIRTUALSCREEN):0;
            int sy=mouse.usFlags&MOUSE_VIRTUAL_DESKTOP?GetSystemMetrics(SM_YVIRTUALSCREEN):0;
            int sw=GetSystemMetrics(mouse.usFlags&MOUSE_VIRTUAL_DESKTOP?SM_CXVIRTUALSCREEN:SM_CXSCREEN);
            int sh=GetSystemMetrics(mouse.usFlags&MOUSE_VIRTUAL_DESKTOP?SM_CYVIRTUALSCREEN:SM_CYSCREEN);
            POINT p{sx+static_cast<LONG>(double(mouse.lLastX)*sw/65535.),sy+static_cast<LONG>(double(mouse.lLastY)*sh/65535.)};
            if(window){ScreenToClient(window,&p);RECT client{};GetClientRect(window,&client);x=p.x*width/std::max(1L,client.right);y=p.y*height/std::max(1L,client.bottom);}else{x=static_cast<float>(mouse.lLastX)*width/65535;y=static_cast<float>(mouse.lLastY)*height/65535;}
        }else{x+=mouse.lLastX;y+=mouse.lLastY;}
        x=std::clamp(x,0.f,width-1);y=std::clamp(y,0.f,height-1);
        if(mouse.usButtonFlags&RI_MOUSE_LEFT_BUTTON_DOWN)left=true;
        if(mouse.usButtonFlags&RI_MOUSE_LEFT_BUTTON_UP)left=false;
        if(mouse.usButtonFlags&RI_MOUSE_RIGHT_BUTTON_DOWN)right=true;
        if(mouse.usButtonFlags&RI_MOUSE_RIGHT_BUTTON_UP)right=false;
        if(mouse.usButtonFlags&RI_MOUSE_WHEEL)wheel+=static_cast<SHORT>(mouse.usButtonData);
    }
    Snapshot snapshot(){std::lock_guard<std::mutex> lock(mutex);auto s=Snapshot{x,y,left,right,wheel};wheel=0;return s;}
};
