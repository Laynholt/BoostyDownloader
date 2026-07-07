#pragma once

#include "AppPaths.h"
#include "Config.h"
#include "DownloadQueue.h"
#include "Logger.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <memory>
#include <vector>

class Application {
public:
    int Run(HINSTANCE instance, int showCommand);

private:
    struct Button {
        UINT id = 0;
        RECT rect{};
        std::wstring text;
        bool primary = false;
        bool hot = false;
        bool pressed = false;
    };

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    bool CreateMainWindow(int showCommand);
    void Initialize();
    void Layout();
    void Paint(HDC dc);
    void AddButtons();
    void InvalidateButton(int id);
    void EnqueueText(const std::wstring& text);
    void EnqueueFromFile(const std::filesystem::path& path);
    void ImportFile();
    void SaveConfigFromControls();
    bool RefreshStatus();
    void SetStatus(std::wstring value);
    Button* HitButton(POINT point);
    void Click(UINT id);

    HINSTANCE m_instance = nullptr;
    HWND m_window = nullptr;
    HWND m_urlEdit = nullptr;
    HWND m_folderEdit = nullptr;
    HWND m_workersEdit = nullptr;
    std::unique_ptr<AppPaths> m_paths;
    AppConfig m_config;
    std::unique_ptr<Logger> m_logger;
    std::unique_ptr<DownloadQueue> m_queue;
    std::vector<Button> m_buttons;
    std::wstring m_status;
    std::uint64_t m_lastRenderedQueueRevision = static_cast<std::uint64_t>(-1);
    int m_hotButton = 0;
    int m_pressedButton = 0;
};
