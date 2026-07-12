# Download folder dialog

Replace the legacy `SHBrowseForFolderW` download-directory picker with the native Windows `IFileOpenDialog` in folder-selection mode.

- Keep the current download directory as the initial folder when Windows can resolve it.
- Update and save the configured path only after a successful selection.
- Cancellation and dialog creation failures leave the configuration unchanged.
- Add no dependencies; reuse the COM setup already used by the application.
- Verify with the existing self-test and a Release build.
