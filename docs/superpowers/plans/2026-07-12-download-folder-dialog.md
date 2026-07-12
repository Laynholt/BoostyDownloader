# Download Folder Dialog Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the legacy download-folder picker with the modern native Windows folder dialog.

**Architecture:** Change only the `kBtnBrowse` handler in `Application.cpp`. Reuse the COM initialization and `IFileOpenDialog` pattern already used by the FFmpeg picker; no new abstraction or dependency is needed.

**Tech Stack:** C++20, Win32, COM Shell API, CMake/CTest

## Global Constraints

- Keep the current download directory as the initial folder when Windows can resolve it.
- Update and save the configured path only after a successful selection.
- Cancellation and dialog creation failures leave the configuration unchanged.
- Add no dependencies.
- Verify with the existing self-test and a Release build.

---

### Task 1: Replace the download folder picker

**Files:**
- Modify: `src/UI/Application.cpp:3199-3213`

**Interfaces:**
- Consumes: `m_window`, `m_folderEdit`, `SaveConfigFromControls()`, and the application's COM apartment.
- Produces: no new interface; `kBtnBrowse` updates the existing edit control after selection.

- [x] **Step 1: Record the approved UI-test exception**

The approved specification limits verification to the existing self-test and Release build because the changed behavior is a modal Windows shell UI with no headless test seam. Do not add an abstraction solely for testing.

- [x] **Step 2: Implement the minimal native dialog**

Replace `BROWSEINFOW`/`SHBrowseForFolderW` with `IFileOpenDialog`, set `FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM`, use `SHCreateItemFromParsingName(GetText(m_folderEdit))` plus `SetFolder` for the initial folder, and copy the successful `SIGDN_FILESYSPATH` result into `m_folderEdit` before saving.

- [x] **Step 3: Run verification**

Run: `ctest --test-dir build -C Release --output-on-failure`

Expected: `100% tests passed`.

Run: `cmake --build build --config Release`

Expected: exit code 0 and `BoostyDownloader.exe` linked in `build/bin/Release`.

- [x] **Step 4: Review and commit**

Run `git diff --check`, inspect `git diff`, then commit only the implementation and this plan with message `Use modern download folder dialog`.
