#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

python3 tools/check_file_integrity.py

while IFS= read -r -d '' script; do
  bash -n "$script"
done < <(find . -path './.git' -prune -o -type f -name '*.sh' -print0)

echo "verify.sh: OK"
