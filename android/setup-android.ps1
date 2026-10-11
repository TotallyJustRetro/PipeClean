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
Write-Host "SDL2 and SDL2_image are prepared under android/deps."
Write-Host "Open android/ in Android Studio and build the debug APK."
