#!/bin/sh
# Tests hote du protocole Halo 1 (src/halo1_proto.*, src/halo1_map.*), de la
# surveillance du BM5602 (src/halo1_watch.*) et de la logique de la LED d'etat
# (src/status_led.*, sans ARDUINO), sans carte.
# A lancer depuis n'importe ou : sh tools/test_halo1.sh
set -e
cd "$(dirname "$0")/.."
clang++ -std=c++17 -Wall -Wextra -Werror -Isrc \
  src/halo1_proto.cpp src/halo1_map.cpp src/halo1_watch.cpp src/status_led.cpp tools/host_tests/test_halo1.cpp \
  -o "${TMPDIR:-/tmp}/test_halo1" && "${TMPDIR:-/tmp}/test_halo1"
