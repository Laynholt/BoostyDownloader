#pragma once

#include "AppPaths.h"
#include "BoostyClient.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

bool ShowBoostyLoginDialog(HWND owner, HINSTANCE instance, const AppPaths& paths, BoostyAuth& auth);
