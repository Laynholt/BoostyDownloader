# BoostyDownloader

BoostyDownloader - портативное Win32-приложение для скачивания видео из доступных пользователю постов Boosty. В приложении есть нативный Windows-интерфейс, очередь загрузок, сохранение токена авторизации, выбор папки загрузки и поддержка нескольких параллельных загрузок.

## Стек

- C++20
- CMake 3.20+
- Нативный Win32 UI: GDI+, DWM, Common Controls
- WinHTTP для HTTP-запросов и скачивания файлов
- WebView2 для входа в Boosty и получения cookies
- `nlohmann/json` single-header library в `third_party/`
- MSVC/Visual Studio toolchain под Windows

## Сборка

Требования:

- Windows
- Visual Studio 2022 с workload `Desktop development with C++`
- CMake 3.20 или новее

Сконфигурировать и собрать release-версию:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

При сборке через Visual Studio generator исполняемый файл будет лежать в `build/bin/Release/`.

Запуск тестов:

```powershell
ctest --test-dir build -C Release --output-on-failure
```

## Runtime

Для скачивания платных постов нужен доступный аккаунту Boosty-токен. Его можно сохранить через окно входа WebView2 или вставить вручную; приложение использует cookies и `Authorization` header при запросе поста и скачивании видео.
