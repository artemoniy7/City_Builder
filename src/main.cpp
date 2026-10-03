#include "core/Application.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int commandShow) {
    city::Application application(instance);
    return application.Run(commandShow);
}

