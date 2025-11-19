#!/bin/bash
echo "Files: $@"

PYTHON_SCRIPT="analysis.py"


for file in "$@"; do
    echo "===== START: $(basename "$file") ====="
    python3 "$PYTHON_SCRIPT" "$file" 2>&1
    echo "===== END: $(basename "$file") ====="
    echo ""
done