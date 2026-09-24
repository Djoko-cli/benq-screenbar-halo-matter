#!/bin/sh
# Tests hote du protocole Halo 1 (src/halo1_proto.*, src/halo1_map.*), de la
# surveillance du BM5602 (src/halo1_watch.*), de la logique de la LED d'etat
# (src/status_led.*, sans ARDUINO) et des briques du protocole JSON
# (src/json_out.*), sans carte. Les messages JSON produits par les tests sont
# ensuite verifies par tools/json_check.py, comme les exemples de
# docs/PROTOCOLE-JSON.md.
# A lancer depuis n'importe ou : sh tools/test_halo1.sh
set -e
cd "$(dirname "$0")/.."
OUT="${TMPDIR:-/tmp}"
clang++ -std=c++17 -Wall -Wextra -Werror -Isrc \
  src/halo1_proto.cpp src/halo1_map.cpp src/halo1_watch.cpp src/status_led.cpp tools/host_tests/test_halo1.cpp \
  -o "$OUT/test_halo1" && "$OUT/test_halo1"
clang++ -std=c++17 -Wall -Wextra -Werror -Isrc \
  src/halo1_proto.cpp src/halo1_map.cpp src/halo1_watch.cpp src/json_out.cpp tools/host_tests/test_json.cpp \
  -o "$OUT/test_json" && "$OUT/test_json" "$OUT/test_json_lignes.txt"
python3 tools/json_check.py -q --strict --independantes "$OUT/test_json_lignes.txt"
python3 tools/json_check.py -q --strict --exemples docs/PROTOCOLE-JSON.md
