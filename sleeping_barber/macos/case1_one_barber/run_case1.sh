#!/bin/bash
cd "$(dirname "$0")"
clang -Wall -Wextra -pthread case1.c -o case1
./case1
