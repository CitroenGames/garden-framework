#!/bin/bash
cd "$(dirname "$0")"

if [ "$(uname)" = "Darwin" ]; then
    ./sighmake_macos project.buildscript -g makefile
else
    ./sighmake project.buildscript -g makefile
fi
