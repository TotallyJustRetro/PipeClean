$ErrorActionPreference = "Stop"
$Deps = Join-Path $PSScriptRoot "deps"
New-Item -ItemType Directory -Force $Deps | Out-Null

$SDL = Join-Path $Deps "SDL"
$Image = Join-Path $Deps "SDL_image"
if (-not (Test-Path (Join-Path $SDL "CMakeLists.txt"))) {
    git clone --depth 1 --branch release-2.32.10 https://github.com/libsdl-org/SDL.git $SDL
}
if (-not (Test-Path (Join-Path $Image "CMakeLists.txt"))) {
    git clone --depth 1 --branch release-2.8.12 https://github.com/libsdl-org/SDL_image.git $Image
}
# Reuse SDL's wrapper scripts/JAR, while pinning Gradle for AGP 8.7.
Copy-Item (Join-Path $SDL "android-project/gradlew") (Join-Path $PSScriptRoot "gradlew") -Force
Copy-Item (Join-Path $SDL "android-project/gradlew.bat") (Join-Path $PSScriptRoot "gradlew.bat") -Force
Copy-Item (Join-Path $SDL "android-project/gradle") (Join-Path $PSScriptRoot "gradle") -Recurse -Force
$WrapperProps = Join-Path $PSScriptRoot "gradle/wrapper/gradle-wrapper.properties"
(Get-Content $WrapperProps -Raw).Replace("gradle-8.1.1-bin.zip", "gradle-8.9-bin.zip") | Set-Content -NoNewline $WrapperProps
Write-Host "SDL2 and SDL2_image are prepared under android/deps."
Write-Host "Open android/ in Android Studio and build the debug APK."
