#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

python3 tools/check_file_integrity.py

while IFS= read -r -d '' script; do
  bash -n "$script"
done < <(find . \( -path './.git' -o -path './.claude' \) -prune -o -type f -name '*.sh' -print0)

# The loop above only PARSES the shell scripts. It does not run them, which meant
# scripts/test_check_es8311_gate.sh reported six passing cases in a PR body while
# CI had never executed a single assertion in it: the green tick proved only that
# bash could read the file. Both checks below are offline (the gate test mocks gh)
# and take under a second, so there is no reason for CI not to actually run them.
bash scripts/test_check_es8311_gate.sh
bash scripts/check_es8311_readiness.sh

echo "verify.sh: OK"
