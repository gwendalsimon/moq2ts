#!/usr/bin/env bash
# The Windows CI job builds every file that CMakeLists.txt does not exclude on
# WIN32. A POSIX header in such a file breaks that build, and no Linux build
# notices. Checks that POSIX headers appear only in files compiled on UNIX
# alone, or under a preprocessor guard.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CMAKE="$REPO_ROOT/CMakeLists.txt"

fail() { printf '%s\n' "$1" >&2; exit 1; }

# Files that CMakeLists.txt adds only on UNIX (SRT ingest, MPTS per-program).
UNIX_ONLY=(
  src/media/SrtSource.cpp
  src/media/MptsFanOut.cpp
)

core_sources="$(awk '/^set\(CORE_SOURCES/{on=1; next} on && /^\)/{exit} on' "$CMAKE")"
for f in "${UNIX_ONLY[@]}" src/media/PacketQueueDevice.cpp; do
  if grep -qF "$f" <<<"$core_sources"; then
    fail "$f is in the unconditional CORE_SOURCES, which the Windows build compiles"
  fi
done

grep -q 'if(MOQ2TS_ENABLE_MPTS AND UNIX)' "$CMAKE" \
  || fail "CMakeLists.txt must add the MPTS sources only on UNIX"

posix='^[[:space:]]*#[[:space:]]*include[[:space:]]*<(unistd|poll|fcntl|netdb|pthread|dlfcn|termios|sys/[^>]*|arpa/[^>]*|netinet/[^>]*|linux/[^>]*)[.]h>'
while IFS= read -r file; do
  rel="${file#"$REPO_ROOT"/}"
  skip=0
  for u in "${UNIX_ONLY[@]}"; do [[ "$rel" == "$u" ]] && skip=1; done
  [[ $skip -eq 1 ]] && continue
  # An include at preprocessor depth 0 is compiled everywhere.
  awk -v re="$posix" -v f="$rel" '
    /^[[:space:]]*#[[:space:]]*if/    { depth++ }
    /^[[:space:]]*#[[:space:]]*endif/ { depth-- }
    depth == 0 && $0 ~ re { printf "%s:%d: unguarded POSIX header: %s\n", f, NR, $0; bad = 1 }
    END { exit bad }
  ' "$file" || fail "POSIX headers must be guarded or in a UNIX-only file"
done < <(find "$REPO_ROOT/src" \( -name '*.cpp' -o -name '*.h' -o -name '*.mm' \) -not -path '*/receive/*')

printf 'posix-only-sources guards passed\n'
