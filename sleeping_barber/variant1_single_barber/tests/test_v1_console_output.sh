#!/bin/bash
set -euo pipefail

script_dir="$(cd "$(dirname "$0")/.." && pwd)"
cd "$script_dir"

clang -Wall -Wextra -pthread -o /tmp/barber_v1_console_test sleeping_barber_v1.c
output="$(printf '1\n3\n3\n' | /tmp/barber_v1_console_test)"

grep -q "Simulation details" <<<"$output"
grep -q "Customer 1 arrives at the shop" <<<"$output"
grep -q "Barber is sleeping (no customers)" <<<"$output"
grep -q "Chairs:" <<<"$output"
