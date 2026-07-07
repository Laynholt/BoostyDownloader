#include "Application.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    Application app;
    return app.Run(instance, showCommand);
}
