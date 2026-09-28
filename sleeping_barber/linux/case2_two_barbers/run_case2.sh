#!/bin/bash
cd "$(dirname "$0")"
gcc -Wall -Wextra -pthread case2.c -o case2
./case2
