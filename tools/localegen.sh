#!/usr/bin/env bash
# Updates the gettext catalogs in po/:
#   1. collects every _("...") literal of the sources (src/ and include/) with xgettext;
#   2. adds the new strings to every po/<lang>.po (existing translations are kept, nothing is deleted:
#      many msgids come from data files, e.g. dialog.* keys and map titles, not from _() literals);
#   3. en.po gets msgstr = msgid for the new strings; openaow.pot is rebuilt from en.po (empty msgstr);
#   4. checks every catalog with msgfmt -c and lists the strings still untranslated.
#
# Usage: tools/localegen.sh [--check] [--mo] [lang ...]
#   --check   change nothing: only list the strings missing from po/ and the untranslated ones
#   --mo      also compile assets/locale/<lang>/LC_MESSAGES/openaow.mo (CMake does it on every build, too)
#   lang      catalogs to update (default: every po/*.po); a new language is started from openaow.pot
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
po="$root/po"
domain=openaow
check=0
mo=0
langs=()
for arg in "$@"; do
    case "$arg" in
        --check) check=1 ;;
        --mo) mo=1 ;;
        -h | --help)
            sed -n '2,13p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        -*)
            echo "unknown option: $arg" >&2
            exit 2
            ;;
        *) langs+=("$arg") ;;
    esac
done

for tool in xgettext msgcat msgen msgfilter msgfmt msgattrib; do
    command -v "$tool" > /dev/null || {
        echo "localegen: $tool not found (install gettext)" >&2
        exit 1
    }
done

if [ ${#langs[@]} -eq 0 ]; then
    for f in "$po"/*.po; do
        langs+=("$(basename "$f" .po)")
    done
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# 1. strings of the sources
mapfile -t sources < <(cd "$root" && find src include -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) 2> /dev/null | sort)
(cd "$root" && xgettext --language=C++ --from-code=UTF-8 --keyword=_ --no-location --no-wrap \
    --omit-header -o "$tmp/sources.pot" "${sources[@]}")
touch "$tmp/sources.pot"
# Not messages: the key of a JSON field that is passed to _() (MissionController)
printf 'msgid "title"\nmsgstr ""\n' > "$tmp/exclude.po"
msgcomm --less-than=2 --no-wrap "$tmp/sources.pot" "$tmp/exclude.po" -o "$tmp/sources.filtered.pot"
mv "$tmp/sources.filtered.pot" "$tmp/sources.pot"

# New languages start from the template
for lang in "${langs[@]}"; do
    if [ ! -f "$po/$lang.po" ]; then
        if [ "$check" = 1 ]; then
            echo "localegen: po/$lang.po does not exist (it would be created from $domain.pot)"
            continue
        fi
        cp "$po/$domain.pot" "$po/$lang.po"
        echo "localegen: started po/$lang.po from $domain.pot"
    fi
done

status=0
for lang in "${langs[@]}"; do
    file="$po/$lang.po"
    [ -f "$file" ] || continue
    # 2. union: the catalog first (its translations win), the new strings after it
    msgcat --use-first --to-code=UTF-8 --no-wrap --no-location "$file" "$tmp/sources.pot" -o "$tmp/$lang.po"
    if [ "$lang" = en ]; then
        msgen --no-wrap "$tmp/$lang.po" -o "$tmp/$lang.new.po" # English: the text is its own translation
        mv "$tmp/$lang.new.po" "$tmp/$lang.po"
    fi
    added="$(msgcomm --no-wrap --unique "$tmp/$lang.po" "$file" 2> /dev/null | grep -c '^msgid "[^"]' || true)"
    if [ "$check" = 1 ]; then
        echo "localegen: $lang: $added string(s) of the sources missing from po/$lang.po"
    else
        cp "$tmp/$lang.po" "$file"
        echo "localegen: $lang: updated ($added new string(s))"
    fi
    # 4. checks
    msgfmt --check --statistics -o /dev/null "$file" 2>&1 | grep -v 'Last-Translator\|Language-Team\|Language' || true
    if [ "$lang" != en ]; then
        untranslated="$(msgattrib --untranslated --no-wrap "$tmp/$lang.po" | grep -c '^msgid "[^"]' || true)"
        if [ "$untranslated" -gt 0 ]; then
            echo "localegen: $lang: $untranslated untranslated string(s):"
            msgattrib --untranslated --no-wrap "$tmp/$lang.po" | grep '^msgid "[^"]' | sed 's/^msgid /    /'
        fi
    fi
done

# 3. the template: en.po without translations
if [ "$check" = 0 ] && [ -f "$po/en.po" ]; then
    LC_ALL=C.UTF-8 msgfilter --no-wrap --keep-header -i "$po/en.po" -o "$tmp/template.pot" true
    sed -i 's/charset=ASCII/charset=UTF-8/' "$tmp/template.pot" # msgfilter writes ASCII for an all-English catalog
    cp "$tmp/template.pot" "$po/$domain.pot"
    echo "localegen: $domain.pot rebuilt"
fi

# --mo: compiled catalogs next to the assets
if [ "$mo" = 1 ] && [ "$check" = 0 ]; then
    for lang in "${langs[@]}"; do
        [ -f "$po/$lang.po" ] || continue
        mkdir -p "$root/assets/locale/$lang/LC_MESSAGES"
        msgfmt -o "$root/assets/locale/$lang/LC_MESSAGES/$domain.mo" "$po/$lang.po"
        echo "localegen: assets/locale/$lang/LC_MESSAGES/$domain.mo"
    done
fi
exit $status
