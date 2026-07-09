#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

void CopyTextToClipboard(HWND owner, const std::wstring& text);
void EnableDarkTitleBar(HWND window);
void CloseDialogWindow(HWND window);
bool DisableModalOwner(HWND owner);
void RestoreModalOwnerWindow(HWND owner, bool ownerWasEnabled);
HFONT CreateUiFont(int height = -16, int weight = FW_NORMAL);
