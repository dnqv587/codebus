#!/bin/bash

INPUT=$(cat)
if ! command -v jq >/dev/null 2>&1; then
  echo "guardrail: jq missing, cannot parse command — allowing (install jq for enforcement)" >&2
  exit 0
fi
COMMAND=$(echo "$INPUT" | jq -r '.tool_input.command')

DANGEROUS_PATTERNS=(
  "git push"
  "git rebase"
  "git reset --hard"
  "git clean -fd"
  "git clean -f"
  "git branch -D"
  "git checkout \.[[:space:]]*$"
  "git restore \.[[:space:]]*$"
  "push --force"
  "reset --hard"
)

for pattern in "${DANGEROUS_PATTERNS[@]}"; do
  if echo "$COMMAND" | grep -qE "$pattern"; then
    echo "BLOCKED: '$COMMAND' matches dangerous pattern '$pattern'. The user has prevented you from doing this." >&2
    exit 2
  fi
done

exit 0
