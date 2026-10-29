#!/usr/bin/sh

# Format sources with Clang-Format using .clang-format configuration

function check_util() {
    if [ ! -f "$(command -v "$1")" ]; then
        echo "Utility \"$1\" is no install"
        exit 1
    fi
}

function format_file() {
    find "$1" -type f \( -name "*.cpp" -o -name "*.hpp" -o -name "*.h" -o -name "*.c" \) -exec sh -c 'truncate -s 0 /tmp/_______format && clang-format --assume-filename=.clang-format {} | tee /tmp/_______format > /dev/null && mv /tmp/_______format {}' \;
}

check_util dirname
check_util readlink
check_util clang-format
check_util find
check_util tee
check_util truncate

_script_path=$(dirname "$(readlink -f "$0")")

format_file "${_script_path}/include"
format_file "${_script_path}/src"

echo "format success"
