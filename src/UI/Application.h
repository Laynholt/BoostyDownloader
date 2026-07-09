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

    struct TaskButton {
        int taskId = 0;
        UINT action = 0;
        RECT rect{};
        std::wstring text;
        bool primary = false;
    };

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    bool CreateMainWindow(int showCommand);
    void Initialize();
    void Layout();
    void Paint(HDC dc);
    void AddButtons();
    void UpdateTooltips();
    void InvalidateButton(int id);
    void InvalidateTaskButton(int taskId, UINT action);
    void EnqueueText(const std::wstring& text);
    void EnqueueFromFile(const std::filesystem::path& path);
    void ImportFile();
    void PasteUrl();
    void OpenDownloadFolder();
    void ShowSettings();
    void SaveConfigFromControls();
    void LoadDownloadQueue();
    void SaveDownloadQueue(bool forShutdown);
    bool RefreshStatus();
    void SetStatus(std::wstring value);
    bool ScrollQueue(int rows);
    Button* HitButton(POINT point);
    TaskButton* HitTaskButton(POINT point);
    void Click(UINT id);
    void ClickTask(int taskId, UINT action);

    HINSTANCE m_instance = nullptr;
    HWND m_window = nullptr;
    HWND m_urlEdit = nullptr;
    HWND m_folderEdit = nullptr;
    HWND m_tooltip = nullptr;
    std::unique_ptr<AppPaths> m_paths;
    AppConfig m_config;
    std::unique_ptr<Logger> m_logger;
    std::unique_ptr<DownloadQueue> m_queue;
    std::vector<Button> m_buttons;
    std::vector<TaskButton> m_taskButtons;
    std::wstring m_status;
    std::uint64_t m_lastRenderedQueueRevision = static_cast<std::uint64_t>(-1);
    std::uint64_t m_lastSavedQueueRevision = static_cast<std::uint64_t>(-1);
    int m_queueScrollOffset = 0;
    bool m_draggingQueueScroll = false;
    int m_queueScrollDragY = 0;
    int m_queueScrollDragStartOffset = 0;
    int m_hotButton = 0;
    int m_pressedButton = 0;
    int m_hotTaskId = 0;
    UINT m_hotTaskAction = 0;
    int m_pressedTaskId = 0;
    UINT m_pressedTaskAction = 0;
};
