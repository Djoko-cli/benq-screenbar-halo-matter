#!/bin/sh
# Tests de halo-routes sans rien installer : decision (test_logique.c), puis
# compilation du demon et un passage en essai (-n : lit le noyau, ne change rien).
#   sh tools/macos/halo-routes/tests.sh
set -eu
cd "$(dirname "$0")"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
clang -std=c11 -Wall -Wextra -Werror -o "$OUT/test_logique" logique.c test_logique.c
"$OUT/test_logique"
clang -std=c11 -O2 -Wall -Wextra -Werror -o "$OUT/halo-routes" halo-routes.c logique.c
"$OUT/halo-routes" -n -1 -v
