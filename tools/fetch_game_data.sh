#!/usr/bin/env bash
# Downloads the game data the CI puts into a build: assets.blob (and other *.blob) and mods (*.mod).
#
#   GAME_DATA_URLS   one entry per line (blank lines and lines starting with # are ignored):
#                      https://example.com/files/assets.blob
#                      https://example.com/files/my_mod.mod
#                      renamed.mod = https://example.com/download?id=7      (name = url: the name the file gets)
#                    A name must end in .blob or .mod.
#   GAME_DATA_TOKEN  optional: sent as "Authorization: Bearer" (a private GitHub release: use the asset's API URL
#                    https://api.github.com/repos/<owner>/<repo>/releases/assets/<id>, which is fetched as a binary)
#
# Usage: tools/fetch_game_data.sh [output dir]   (default: game-data)
# Writes the files and, in the output dir, `bundled_files.txt` (their names, one per line: the Android build reads it).
# Exit status 0 with no files when GAME_DATA_URLS is empty: a build without data is a valid build.
set -euo pipefail

out="${1:-game-data}"
mkdir -p "$out"
: > "$out/bundled_files.txt"

urls="${GAME_DATA_URLS:-}"
count=0
fail=0

while IFS= read -r line || [ -n "$line" ]; do
    line="${line%$'\r'}"
    # trim
    line="$(printf '%s' "$line" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')"
    [ -z "$line" ] && continue
    case "$line" in \#*) continue ;; esac

    name=""
    url="$line"
    if [[ "$line" == *"="* && "$line" != http*"?"*"="* && "${line%%=*}" != *"/"* && "${line%%=*}" != *":"* ]]; then
        name="$(printf '%s' "${line%%=*}" | sed -e 's/[[:space:]]*$//')"
        url="$(printf '%s' "${line#*=}" | sed -e 's/^[[:space:]]*//')"
    fi
    [ -n "${GITHUB_ACTIONS:-}" ] && echo "::add-mask::$url"
    if [ -z "$name" ]; then
        name="$(basename "${url%%\?*}")"
    fi
    case "$name" in
        */* | *\\* | "" | .*) echo "bad file name '$name' (a plain file name is needed)" >&2; fail=1; continue ;;
        *.blob | *.mod) ;;
        *) echo "'$name': only *.blob and *.mod files can be bundled (name = url to give it a name)" >&2; fail=1; continue ;;
    esac

    echo "fetching $name"
    args=(--fail --location --silent --show-error --retry 3 --retry-delay 5 --output "$out/$name.part")
    if [ -n "${GAME_DATA_TOKEN:-}" ]; then
        args+=(--header "Authorization: Bearer ${GAME_DATA_TOKEN}")
        case "$url" in */releases/assets/*) args+=(--header "Accept: application/octet-stream") ;; esac
    fi
    if ! curl "${args[@]}" "$url"; then
        echo "download of $name failed" >&2
        rm -f "$out/$name.part"
        fail=1
        continue
    fi
    if [ ! -s "$out/$name.part" ]; then
        echo "$name is empty" >&2
        rm -f "$out/$name.part"
        fail=1
        continue
    fi
    mv "$out/$name.part" "$out/$name"
    echo "$name" >> "$out/bundled_files.txt"
    count=$((count + 1))
    echo "  $(wc -c < "$out/$name") bytes"
done <<< "$urls"

echo "$count file(s) in $out"
[ -n "${GITHUB_OUTPUT:-}" ] && echo "count=$count" >> "$GITHUB_OUTPUT"
exit "$fail"
