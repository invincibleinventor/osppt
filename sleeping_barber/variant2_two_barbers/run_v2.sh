#!/bin/bash
set -e
cd "$(dirname "$0")"
clang -Wall -pthread -o barber_v2 sleeping_barber_v2.c
./barber_v2
