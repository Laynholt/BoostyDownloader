#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

void CopyTextToClipboard(HWND owner, const std::wstring& text);
void EnableDarkTitleBar(HWND window);
HFONT CreateUiFont(int height = -16, int weight = FW_NORMAL);
