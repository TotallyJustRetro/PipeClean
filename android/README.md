# PipeClean for Android (development scaffold)

PipeClean now has SDL multi-touch input, a saved setting to disable the touch overlay, a color wheel in the desktop/local-co-op settings, and a platform-shared Online/profile screen. This directory starts the Android Studio project; the APK still needs a build with the Android SDK/NDK and playtesting on a real device.

## Requirements

- Android Studio with Android SDK 35 and NDK 28.2.13676358
- Git and PowerShell (Windows) or a POSIX shell (Linux/macOS)
- A JDK compatible with Android Gradle Plugin 8.7.3

## Prepare dependencies

From the repository root, run one of:

    .\android\setup-android.ps1

    ./android/setup-android.sh

The scripts fetch SDL2 release-2.32.10 and SDL2_image release-2.8.12. Source checkouts live in ignored android/deps/ rather than PipeClean's source history. The project compiles SDL's Android Activity glue and creates libmain.so with CMake.

Open the android/ folder in Android Studio, let Gradle sync, and build/run the app configuration. This project currently has no checked-in Gradle wrapper; Android Studio supplies compatible Gradle tooling.

The native build creates a synthetic build-only ROM because PipeClean's generator expects ROM-shaped input at compile time. The generated stub is not playable and is not packaged as game content. At runtime, select your own legally obtained ROM through the Android document picker.

## What's present vs. still pending

- Present in code: desktop player color wheel, persistent RGB colors for local extra players, shared Online/profile tab, Android project/manifest scaffold, SDL2 CMake Android target, storage-access-framework file picker, writable Android preference/save directory, and multi-touch gameplay overlay.
- Touch control preference: enabled by default. Use Online -> On-screen touch controls to disable it when using a controller. The UI and wheel are also touch-driven on Android.
- Still pending: verify that the Gradle/NDK build produces an APK, test on Android hardware, handle lifecycle/audio/storage edge cases, and make session hosting/joining real.
- Online isn't connected yet: the host/join buttons are deliberately marked offline. Session codes, a public relay, game-state synchronization, player roster, and chat are not implemented by the current branch.
- Folder selection: Android SAF selections are copied into the app's private picked-files area so PipeClean's current path-based native APIs can use them. Export-to-user-selected-folder semantics need a dedicated SAF writer before treating that behavior as final.

Do not treat this development branch as a release build until CI/native Android packaging and live gameplay are verified.
