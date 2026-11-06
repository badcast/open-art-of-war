#!/usr/bin/env bash
# Updates the gettext catalogs in po/:
#   1. collects every _("...") literal of the sources with xgettext for each project:
#      - openaow (src/ and include/) -> po/openaow_<lang>.po, po/openaow.pot
#      - openaow_character_editor (openaow-charactereditor/) -> po/openaow_character_editor_<lang>.po, po/openaow_character_editor.pot
#      - openaow_map_editor (openaow-mapeditor/) -> po/openaow_map_editor_<lang>.po, po/openaow_map_editor.pot
#   2. adds the new strings to each catalog (existing translations are kept, nothing is deleted);
#   3. <domain>_en.po gets msgstr = msgid for the new strings; <domain>.pot is rebuilt (empty msgstr);
#   4. checks every catalog with msgfmt -c and lists the strings still untranslated.
#
# Usage: tools/localegen.sh [--check] [--mo] [domain ...] [lang ...]
#   --check   change nothing: only list the strings missing from po/ and the untranslated ones
#   --mo      also compile assets/locale/<lang>/LC_MESSAGES/<domain>.mo (CMake does it on every build, too)
#   domain    project domain(s) to update: openaow, openaow_character_editor, openaow_map_editor (default: all)
#   lang      catalogs to update (default: en ru); a new language is started from <domain>.pot
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
po="$root/po"
check=0
mo=0
args=()
for arg in "$@"; do
    case "$arg" in
        --check) check=1 ;;
        --mo) mo=1 ;;
        -h | --help)
            sed -n '2,15p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        -*)
            echo "unknown option: $arg" >&2
            exit 2
            ;;
        *) args+=("$arg") ;;
    esac
done

for tool in xgettext msgcat msgen msgfilter msgfmt msgattrib; do
    command -v "$tool" > /dev/null || {
        echo "localegen: $tool not found (install gettext)" >&2
        exit 1
    }
done

all_domains=(openaow openaow_character_editor openaow_map_editor)
domains=()
langs=()

for a in "${args[@]}"; do
    case "$a" in
        openaow|openaow_character_editor|openaow_map_editor)
            domains+=("$a")
            ;;
        character-editor|character_editor)
            domains+=(openaow_character_editor)
            ;;
        map-editor|map_editor)
            domains+=(openaow_map_editor)
            ;;
        *)
            langs+=("$a")
            ;;
    esac
done

if [ ${#domains[@]} -eq 0 ]; then
    domains=("${all_domains[@]}")
fi

if [ ${#langs[@]} -eq 0 ]; then
    langs=(en ru)
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

status=0

for domain in "${domains[@]}"; do
    echo "=== Domain: $domain ==="
    case "$domain" in
        openaow)
            mapfile -t sources < <(cd "$root" && find src include -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) 2> /dev/null | sort)
            ;;
        openaow_character_editor)
            mapfile -t sources < <(cd "$root" && find openaow-charactereditor -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) 2> /dev/null | sort)
            ;;
        openaow_map_editor)
            mapfile -t sources < <(cd "$root" && find openaow-mapeditor -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) 2> /dev/null | sort)
            ;;
        *)
            echo "Unknown domain: $domain" >&2
            continue
            ;;
    esac

    # 1. extract strings of the sources
    (cd "$root" && LC_ALL=C.UTF-8 xgettext --language=C++ --from-code=UTF-8 --keyword=_ --no-location --no-wrap \
        --omit-header -o "$tmp/${domain}_sources.pot" "${sources[@]}")
    touch "$tmp/${domain}_sources.pot"

    if [ "$domain" = "openaow" ]; then
        # Not messages: the key of a JSON field that is passed to _() (MissionController)
        printf 'msgid "title"\nmsgstr ""\n' > "$tmp/exclude.po"
        msgcomm --less-than=2 --no-wrap "$tmp/${domain}_sources.pot" "$tmp/exclude.po" -o "$tmp/${domain}_sources.filtered.pot"
        mv "$tmp/${domain}_sources.filtered.pot" "$tmp/${domain}_sources.pot"
    fi

    # New languages start from the template
    for lang in "${langs[@]}"; do
        file="$po/${domain}_${lang}.po"
        if [ ! -f "$file" ]; then
            if [ "$check" = 1 ]; then
                echo "localegen: $file does not exist (it would be created from ${domain}.pot)"
                continue
            fi
            if [ -f "$po/${domain}.pot" ]; then
                cp "$po/${domain}.pot" "$file"
                echo "localegen: started $file from ${domain}.pot"
            fi
        fi
    done

    for lang in "${langs[@]}"; do
        file="$po/${domain}_${lang}.po"
        [ -f "$file" ] || continue

        # 2. union: the catalog first (its translations win), the new strings after it
        msgcat --use-first --to-code=UTF-8 --no-wrap --no-location "$file" "$tmp/${domain}_sources.pot" -o "$tmp/${domain}_${lang}.po"
        if [ "$lang" = en ]; then
            msgen --no-wrap "$tmp/${domain}_${lang}.po" -o "$tmp/${domain}_${lang}.new.po"
            sed -i 's/charset=ASCII/charset=UTF-8/' "$tmp/${domain}_${lang}.new.po"
            mv "$tmp/${domain}_${lang}.new.po" "$tmp/${domain}_${lang}.po"
        fi

        added="$(msgcomm --no-wrap --unique "$tmp/${domain}_${lang}.po" "$file" 2> /dev/null | grep -c '^msgid "[^"]' || true)"
        if [ "$check" = 1 ]; then
            echo "localegen: $domain ($lang): $added string(s) of the sources missing from $(basename "$file")"
        else
            cp "$tmp/${domain}_${lang}.po" "$file"
            echo "localegen: $domain ($lang): updated ($added new string(s))"
        fi

        # 4. checks
        msgfmt --check --statistics -o /dev/null "$file" 2>&1 | grep -v 'Last-Translator\|Language-Team\|Language' || true
        if [ "$lang" != en ]; then
            untranslated="$(msgattrib --untranslated --no-wrap "$tmp/${domain}_${lang}.po" | grep -c '^msgid "[^"]' || true)"
            if [ "$untranslated" -gt 0 ]; then
                echo "localegen: $domain ($lang): $untranslated untranslated string(s):"
                msgattrib --untranslated --no-wrap "$tmp/${domain}_${lang}.po" | grep '^msgid "[^"]' | sed 's/^msgid /    /'
            fi
        fi
    done

    # 3. the template: en.po without translations
    if [ "$check" = 0 ] && [ -f "$po/${domain}_en.po" ]; then
        sed -i 's/charset=ASCII/charset=UTF-8/' "$po/${domain}_en.po"
        LC_ALL=C.UTF-8 msgfilter --no-wrap --keep-header -i "$po/${domain}_en.po" -o "$tmp/${domain}_template.pot" tee /dev/null
        sed -i 's/charset=ASCII/charset=UTF-8/' "$tmp/${domain}_template.pot"
        cp "$tmp/${domain}_template.pot" "$po/${domain}.pot"
        echo "localegen: ${domain}.pot rebuilt"
    fi

    # --mo: compiled catalogs next to the assets
    if [ "$mo" = 1 ] && [ "$check" = 0 ]; then
        for lang in "${langs[@]}"; do
            file="$po/${domain}_${lang}.po"
            [ -f "$file" ] || continue
            mkdir -p "$root/assets/locale/$lang/LC_MESSAGES"
            msgfmt -o "$root/assets/locale/$lang/LC_MESSAGES/${domain}.mo" "$file"
            echo "localegen: assets/locale/$lang/LC_MESSAGES/${domain}.mo"
        done
    fi
done

exit $status
