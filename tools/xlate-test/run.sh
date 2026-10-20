#!/bin/sh
# Build + run the protocol-translator host test (g++; on Windows run it in WSL).
set -e
cd "$(dirname "$0")"
g++ -std=c++17 -Wall -Wextra -O1 -I../../src -o xlate_test xlate_test.cpp ../../src/xlate.cpp
./xlate_test
