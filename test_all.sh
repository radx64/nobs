#!/bin/bash
set -e

BASE_DIR="./tests"

find "$BASE_DIR" -type d | while read -r dir; do
    if [ -x "$dir/test.sh" ]; then
        echo "Running test.sh in: $dir"
        (cd "$dir" && ./test.sh)
    fi
done