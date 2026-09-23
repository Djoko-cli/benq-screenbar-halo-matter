#!/bin/sh
# Tests hote du protocole Halo 1 (src/halo1_proto.*, src/halo1_map.*), sans carte.
# A lancer depuis n'importe ou : sh tools/test_halo1.sh
set -e
cd "$(dirname "$0")/.."
clang++ -std=c++17 -Wall -Wextra -Werror -Isrc \
  src/halo1_proto.cpp src/halo1_map.cpp tools/host_tests/test_halo1.cpp \
  -o "${TMPDIR:-/tmp}/test_halo1" && "${TMPDIR:-/tmp}/test_halo1"
