#include "core/Application.h"

#include "graphics/Renderer.h"

#include <stdexcept>

namespace {
constexpr wchar_t kWindowClassName[] = L"CityBuilderDX12Window";
constexpr wchar_t kWindowTitle[] = L"City Builder — DirectX 12";
}

namespace city {

Application::Application(HINSTANCE instance) : instance_(instance) {}

Application::~Application() = default;

int Application::Run(int commandShow) {
    CreateMainWindow(commandShow);
    renderer_ = std::make_unique<Renderer>(window_);

    MSG message{};
    while (message.message != WM_QUIT) {
        if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        } else {
            renderer_->Render();
        }
    }
    return static_cast<int>(message.wParam);
}

void Application::CreateMainWindow(int commandShow) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kWindowClassName;

    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        throw std::runtime_error("Unable to register the Win32 window class.");
    }

    // WS_POPUP deliberately removes title bar and all standard window borders.
    window_ = CreateWindowExW(0, kWindowClassName, kWindowTitle, WS_POPUP,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720, nullptr, nullptr, instance_, this);
    if (window_ == nullptr) {
        throw std::runtime_error("Unable to create the Win32 window.");
    }

    ShowWindow(window_, commandShow);
    UpdateWindow(window_);
}

void Application::OnResize(UINT width, UINT height) {
    if (renderer_ && width != 0 && height != 0) {
        renderer_->Resize(width, height);
    }
}

LRESULT CALLBACK Application::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* application = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        application = static_cast<Application*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(application));
    }

    switch (message) {
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_SIZE:
        if (application) {
            application->OnResize(LOWORD(lParam), HIWORD(lParam));
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace city

