#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
cc -std=c11 -O2 -Wall -Wextra -Wpedantic -fPIC -shared -pthread -o libtotkspawn.so libtotk_spawn.c -lm -lcrypto
python3 -m py_compile spawn.py
printf 'Compiled: %s\n' "$(pwd)/libtotkspawn.so"
