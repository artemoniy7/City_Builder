#pragma once

#include <Windows.h>

#include <memory>

namespace city {

class Renderer;

class Application final {
public:
    explicit Application(HINSTANCE instance);
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    int Run(int commandShow);

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void CreateMainWindow(int commandShow);
    void OnResize(UINT width, UINT height);
    void OnMouseWheel(short delta);

    HINSTANCE instance_{};
    HWND window_{};
    std::unique_ptr<Renderer> renderer_;
};

} // namespace city

