#!/bin/bash
set -e

cd "$(dirname "$0")"

# Pick the platform-appropriate sighmake binary once. Prefer repo-local
# binaries, but allow a PATH install for checkouts that do not carry them.
if [ -n "${SIGHMAKE:-}" ]; then
    :
elif [ "$(uname)" = "Darwin" ] && [ -x ./sighmake_macos ]; then
    SIGHMAKE=./sighmake_macos
elif [ -x ./sighmake ]; then
    SIGHMAKE=./sighmake
elif command -v sighmake >/dev/null 2>&1; then
    SIGHMAKE="$(command -v sighmake)"
else
    echo "sighmake not found. Install sighmake or place sighmake_macos/sighmake in the repo root." >&2
    exit 1
fi

# Main engine + tooling solution.
"$SIGHMAKE" project.buildscript -g makefile
