#!/bin/bash
set -e
cd "$(dirname "$0")"
clang -Wall -pthread -o barber_v1 sleeping_barber_v1.c
./barber_v1
