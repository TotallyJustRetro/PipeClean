#!/usr/bin/env sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DEPS="$ROOT/android/deps"
mkdir -p "$DEPS"
if [ ! -f "$DEPS/SDL/CMakeLists.txt" ]; then
  git clone --depth 1 --branch release-2.32.10 https://github.com/libsdl-org/SDL.git "$DEPS/SDL"
fi
if [ ! -f "$DEPS/SDL_image/CMakeLists.txt" ]; then
  git clone --depth 1 --branch release-2.8.12 https://github.com/libsdl-org/SDL_image.git "$DEPS/SDL_image"
fi
# Reuse SDL's Gradle wrapper, but use Gradle 8.9 for Android Gradle Plugin 8.7.
cp "$DEPS/SDL/android-project/gradlew" "$ROOT/android/gradlew"
cp "$DEPS/SDL/android-project/gradlew.bat" "$ROOT/android/gradlew.bat"
rm -rf "$ROOT/android/gradle"
cp -R "$DEPS/SDL/android-project/gradle" "$ROOT/android/gradle"
sed -i 's/gradle-8\.1\.1-bin\.zip/gradle-8.9-bin.zip/' "$ROOT/android/gradle/wrapper/gradle-wrapper.properties"
chmod +x "$ROOT/android/gradlew"
printf '%s\n' "SDL2 and SDL2_image are prepared under android/deps." "Open android/ in Android Studio and build the debug APK."
