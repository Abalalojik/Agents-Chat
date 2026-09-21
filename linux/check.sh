#!/usr/bin/env bash
# Compiles every source file of Agents Chat on its own under Linux (g++, C++20) and prints
# which ones pass. Run inside the image built from linux/Dockerfile, sources mounted at /src.
# Nothing is written to /src.
set -u
SRC=${1:-/src}
OUT=/tmp/linux-check
mkdir -p "$OUT"

FLAGS="-std=c++20 -fsyntax-only -Wall -Wextra -I$SRC/src -I/deps/imgui -I/deps/imgui/backends -I/deps/imgui/misc/cpp \
-I/deps/nlohmann_json/include $(pkg-config --cflags libsecret-1 sdl2) -DAGENTCHATS_VERSION=\"linux-check\""

pass=0
fail=0
printf '%-32s %s\n' "FICHIER" "RÉSULTAT"
for file in "$SRC"/src/*.cpp "$SRC"/tests/*.cpp "$SRC"/tools/*.cpp; do
    [ -f "$file" ] || continue
    name=${file#"$SRC"/}
    case "$name" in
        src/main.cpp|tools/ReleaseTool.cpp) continue ;; # Windows-only by design (Win32 host, DPAPI key tool)
    esac
    log="$OUT/$(echo "$name" | tr '/' '_').log"
    if g++ $FLAGS "$file" >"$log" 2>&1; then
        printf '%-32s OK\n' "$name"
        pass=$((pass + 1))
    else
        first=$(grep -m1 -E 'error|fatal' "$log" | sed "s|$SRC/||" | cut -c1-150)
        printf '%-32s ÉCHEC  %s\n' "$name" "$first"
        fail=$((fail + 1))
    fi
done
echo
echo "$pass fichier(s) compilent sous Linux, $fail échouent."
echo "Journaux complets dans le conteneur : $OUT"

# --full: real CMake build of the tests (and whatever else builds on Linux), then run them.
if [ "${2:-}" = "--full" ]; then
    echo
    echo "=== Construction complète (CMake + Ninja) ==="
    if cmake -S "$SRC" -B /tmp/build -G Ninja -DCMAKE_BUILD_TYPE=Release >/tmp/build-configure.log 2>&1 &&
       cmake --build /tmp/build 2>&1 | tee /tmp/build.log | grep -E "error|warning" | head -40; [ "${PIPESTATUS[0]}" -eq 0 ]; then
        echo "Construction réussie. Tests :"
        (cd /tmp && /tmp/build/AgentChatsTests)
        exit $?
    else
        tail -20 /tmp/build-configure.log
        echo "Construction échouée."
        exit 1
    fi
fi
[ "$fail" -eq 0 ]
