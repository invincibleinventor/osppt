#!/bin/bash
set -euo pipefail

for source in \
    sleeping_barber/variant1_single_barber/sleeping_barber_v1.c \
    sleeping_barber/variant2_two_barbers/sleeping_barber_v2.c
do
    grep -q 'usleep(rand() % 1000000);' "$source"
    ! grep -q 'sleep(1 + rand() % 2);' "$source"
done
