#include <sdkddkver.h>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "D3D12Engine.h"

// Глобальный указатель на движок (для обработки сообщений)
D3D12Engine* g_engine = nullptr;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    if (msg == WM_KEYDOWN && wParam == VK_ESCAPE) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}
int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    // 1. Регистрация класса окна
    WNDCLASSEXW wcex = {};
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = CS_OWNDC;  // кэшируем DC для производительности 
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.lpszClassName = L"MyEngineWindow";
    RegisterClassExW(&wcex);

    // 2. Создание окна
    RECT rect = { 0, 0, 1280, 720 };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    
    HWND hwnd = CreateWindowExW(
        0, L"MyEngineWindow", L"My Engine (DirectX 12)",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, hInstance, nullptr
    );

    // 3. Инициализация движка
    g_engine = new D3D12Engine();
    g_engine->Init(hwnd, 1280, 720);

    ShowWindow(hwnd, nCmdShow);

    // 4. Цикл сообщений + рендер
    MSG msg = {};
    while (msg.message != WM_QUIT)
    {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else {
            g_engine->Render();  // рисуем кадр
        }
    }

    g_engine->Shutdown();
    delete g_engine;
    return 0;
//g++ main.cpp D3D12Engine.cpp -o engine.exe -I"C:/Program Files (x86)/Windows Kits/10/Include/10.0.22621.0/um" -I"C:/Program Files (x86)/Windows Kits/10/Include/10.0.22621.0/shared" -L"C:/Program Files (x86)/Windows Kits/10/Lib/10.0.22621.0/um/x64" -L"C:/Program Files (x86)/Windows Kits/10/Lib/10.0.22621.0/ucrt/x64" -ld3d12 -ldxgi -ld3dcompiler