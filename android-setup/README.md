# Open Art of War — Android Build Setup

В этой папке подготовлена базовая структура стандартного Android-проекта (Gradle + Android NDK + CMake + SDL3) для сборки **Open Art of War** под платформу Android.

---

## Структура каталога

* **`build.gradle.kts`**, **`settings.gradle.kts`** — корневые конфигурации Gradle-проекта.
* **`app/build.gradle.kts`** — конфигурация сборщика приложения:
  * Целевой SDK: 34 (Android 14)
  * Минимальный SDK: 24 (Android 7.0)
  * Архитектуры (ABIs): `arm64-v8a`, `x86_64`
  * Подключение C++17 и нативного CMake.
* **`app/src/main/AndroidManifest.xml`** — манифест с ландшафтной ориентацией (`screenOrientation="landscape"`), правами доступа к сети/звуку и запуском нативной библиотеки `libopenaow.so` через стандартный класс `SDLActivity`.
* **`app/src/main/cpp/CMakeLists.txt`** — конфигурация сборки нативной части:
  * Автоматическая подгрузка зависимостей под Android через `FetchContent` (`SDL3`, `SDL3_ttf`, `nlohmann_json`, `yaml-cpp`).
  * Сборка исходников движка, ImGui, enet, ronin-engine, stb_vorbis в единую разделяемую библиотеку `libopenaow.so`.
  * Линковка с системными библиотеками Android: `libandroid`, `liblog`, `libGLESv3`, `libOpenSLES`.

---

## Требования для сборки

1. **Android Studio** (рекомендуется последняя версия Iguana / Jellyfish / Koala) или консольные инструменты Android SDK.
2. **Android NDK** (версии 25.x / 26.x / 27.x).
3. **CMake** 3.22.1+.
4. **Java Development Kit (JDK)** 17+.

---

## Сборка через Android Studio

1. Откройте Android Studio.
2. Выберите **File -> Open...** и укажите папку `android-setup`.
3. Дождитесь завершения Gradle Sync (CMake автоматически загрузит `SDL3` и зависимости).
4. Запустите сборку: **Build -> Make Project** или **Build -> Build Bundle(s) / APK(s) -> Build APK(s)**.

---

## Сборка из командной строки

Если установлены `ANDROID_HOME` и Gradle:
```bash
cd android-setup
gradle assembleDebug
```
Итоговый APK будет находиться по пути:
`android-setup/app/build/outputs/apk/debug/app-debug.apk`

---

## Ассеты (Ресурсы игры)

В `app/build.gradle.kts` путь к ассетам настроен так, чтобы напрямую считывать корневую папку `assets/`:
```kotlin
sourceSets {
    getByName("main") {
        assets.srcDirs("src/main/assets", "../../../assets")
    }
}
```
Файлы игры попадут внутрь APK в раздел `assets/` и будут прозрачно доступны через чтение SDL3 (`SDL_IOStream` / `SDL_GetBasePath`).
