#!/bin/sh
# Copies the Java half of SDL3 (SDLActivity and friends) into the app. Run once before building.
# The version must match the SDL3 that cmake/AndroidDeps.cmake builds.
set -e
SDL_TAG=release-3.4.20
here=$(cd "$(dirname "$0")" && pwd)
dest="$here/app/src/main/java"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
git clone --quiet --depth 1 --branch "$SDL_TAG" https://github.com/libsdl-org/SDL.git "$tmp/SDL"
rm -rf "$dest/org/libsdl"
mkdir -p "$dest"
cp -r "$tmp/SDL/android-project/app/src/main/java/org" "$dest/"
echo "SDL Java sources ($SDL_TAG) copied to $dest"
