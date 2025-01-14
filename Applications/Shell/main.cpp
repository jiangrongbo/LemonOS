#include <stdint.h>

#include <Lemon/Core/Keyboard.h>
#include <Lemon/Core/SharedMemory.h>
#include <Lemon/Core/Shell.h>
#include <Lemon/GUI/Window.h>
#include <Lemon/Graphics/Graphics.h>
#include <Lemon/System/Framebuffer.h>
#include <Lemon/System/IPC.h>
#include <Lemon/System/Info.h>
#include <Lemon/System/Spawn.h>
#include <Lemon/System/Util.h>
#include <Lemon/System/Waitable.h>
#include <fcntl.h>
#include <lemon/syscall.h>
#include <map>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "shell.h"

#define MENU_ITEM_HEIGHT 24

Lemon::GUI::Window* taskbar;
ShellInstance* shell;
surface_t menuButton;

bool showMenu = true;

char versionString[80];

lemon_sysinfo_t sysInfo;
char memString[128];

class WindowButton : public Lemon::GUI::Button {
    ShellWindow* win;

  public:
    WindowButton(ShellWindow* win, rect_t bounds) : Button(win->title.c_str(), bounds) {
        this->win = win;
        labelAlignment = Lemon::GUI::TextAlignment::Left;
    }

    void Paint(surface_t* surface) {
        this->label = win->title;
        if(win->state == Lemon::WindowState_Active || pressed){
                Lemon::Graphics::DrawRect(fixedBounds, Lemon::colours[Lemon::Colour::ForegroundDim], surface);
        }

        DrawButtonLabel(surface, false);
    }

    void OnMouseUp(vector2i_t mousePos) {
        pressed = false;

        if(win->lastState == Lemon::WindowState_Active){
            window->Minimize(win->id, true);
        } else {
            window->Minimize(win->id, false);
        }
    }
};

struct WindowEntry {
    ShellWindow* win;
    WindowButton* button;
};

std::map<int64_t, WindowEntry> shellWindows;
Lemon::GUI::LayoutContainer* taskbarWindowsContainer;

bool paintTaskbar = true;
void OnAddWindow(int64_t windowID, uint32_t flags, const std::string& name) {
    if(flags & WINDOW_FLAGS_NOSHELL){
        return;
    }

    ShellWindow* win = new ShellWindow(windowID, name, Lemon::WindowState_Active);
    WindowButton* btn = new WindowButton(win, {0, 0, 0, 0} /* The LayoutContainer will handle bounds for us*/);
    
    shellWindows[windowID] = { win, btn };

    taskbarWindowsContainer->AddWidget(btn);
    paintTaskbar = true;
}

void OnRemoveWindow(int64_t windowID) {
    auto it = shellWindows.find(windowID);
    if(it == shellWindows.end()){
        return;
    }

    taskbarWindowsContainer->RemoveWidget(it->second.button);
    delete it->second.button;
    delete it->second.win;

    shellWindows.erase(it);
    paintTaskbar = true;
}

void OnWindowStateChanged(int64_t windowID, uint32_t flags, int32_t state){
    auto it = shellWindows.find(windowID);
    if(it == shellWindows.end()){
        return;
    }

    if(flags & WINDOW_FLAGS_NOSHELL){
        OnRemoveWindow(windowID);
        return;
    }

    it->second.win->state = state;
    paintTaskbar = true;
}

void OnWindowTitleChanged(int64_t windowID, const std::string& name){
    auto it = shellWindows.find(windowID);
    if(it == shellWindows.end()){
        return;
    }

    it->second.win->title = name;
    paintTaskbar = true;
}

void OnTaskbarPaint(surface_t* surface) {
    Lemon::Graphics::DrawGradientVertical(0, 0, surface->width, surface->height, {0x1d, 0x1c, 0x1b, 255},
                                          {0x1b, 0x1b, 0x1b, 255}, surface);

    if (showMenu) {
        Lemon::Graphics::surfacecpyTransparent(surface, &menuButton,
                                               {18 - menuButton.width / 2, 18 - menuButton.height / 4},
                                               {0, menuButton.height / 2, menuButton.width, 30});
    } else {
        Lemon::Graphics::surfacecpyTransparent(surface, &menuButton,
                                               {18 - menuButton.width / 2, 18 - menuButton.height / 4},
                                               {0, 0, menuButton.width, 30});
    }

    sprintf(memString, "Used Memory: %lu/%lu KB", sysInfo.usedMem, sysInfo.totalMem);
    Lemon::Graphics::DrawString(memString, surface->width - Lemon::Graphics::GetTextLength(memString) - 8, 10, 255, 255,
                                255, surface);
}

int main() {
    if (chdir("/system")) {
        return 1;
    }

    Lemon::Handle svc = Lemon::Handle(Lemon::CreateService("lemon.shell"));
    shell = new ShellInstance(svc, "Instance");

    syscall(SYS_UNAME, (uintptr_t)versionString, 0, 0, 0, 0);

    Lemon::Graphics::LoadImage("/system/lemon/resources/menubuttons.png", &menuButton);

    handle_t tempEndpoint = 0;
    while (tempEndpoint <= 0) {
        tempEndpoint = Lemon::InterfaceConnect("lemon.lemonwm/Instance");
    } // Wait for LemonWM to create the interface (if not already created)
    Lemon::DestroyKObject(tempEndpoint);

	vector2i_t screenBounds = Lemon::WindowServer::Instance()->GetScreenBounds();

    taskbar = new Lemon::GUI::Window("", {static_cast<int>(screenBounds.x), 36},
                                     WINDOW_FLAGS_NODECORATION | WINDOW_FLAGS_NOSHELL, Lemon::GUI::WindowType::GUI,
                                     {0, static_cast<int>(screenBounds.y) - 36});
    taskbar->OnPaint = OnTaskbarPaint;
    taskbar->rootContainer.background = {0, 0, 0, 0};
    taskbarWindowsContainer = new Lemon::GUI::LayoutContainer(
        {40, 0, static_cast<int>(screenBounds.x) - 104, static_cast<int>(screenBounds.y)}, {160, 36 - 4});
    taskbarWindowsContainer->background = {0, 0, 0, 0};
    taskbar->AddWidget(taskbarWindowsContainer);

    Lemon::WindowServer* ws = Lemon::WindowServer::Instance();
    ws->OnWindowCreatedHandler = OnAddWindow;
    ws->OnWindowDestroyedHandler = OnRemoveWindow;
    ws->OnWindowStateChangedHandler = OnWindowStateChanged;
    ws->OnWindowTitleChangedHandler = OnWindowTitleChanged;
    ws->SubscribeToWindowEvents();

    Lemon::GUI::Window* menuWindow = InitializeMenu();
    shell->SetMenu(menuWindow);

    Lemon::Waiter waiter;
    waiter.WaitOnAll(&shell->GetInterface());
    waiter.WaitOn(Lemon::WindowServer::Instance());

    for (;;) {
        Lemon::WindowServer::Instance()->Poll();
        shell->Poll();

        Lemon::LemonEvent ev;
        while (taskbar->PollEvent(ev)) {
            if (ev.event == Lemon::EventMouseReleased) {
                if (ev.mousePos.x < 50) {
                    MinimizeMenu(showMenu); // Toggle whether window is minimized or not
                } else {
                    taskbar->GUIHandleEvent(ev);
                }
            } else {
                taskbar->GUIHandleEvent(ev);
            }
            paintTaskbar = true;
        }

        PollMenu();

        uint64_t usedMemLast = sysInfo.usedMem;
        sysInfo = Lemon::SysInfo();

        if (sysInfo.usedMem != usedMemLast)
            paintTaskbar = true;

        if (paintTaskbar) {
            taskbar->Paint();

            paintTaskbar = false;
        }

        waiter.Wait();
    }

    for (;;)
        ;
}
