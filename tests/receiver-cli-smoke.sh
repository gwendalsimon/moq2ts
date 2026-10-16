#!/usr/bin/env bash
set -euo pipefail
binary="${1:?receiver binary required}"
"$binary" --help 2>&1 | grep -q 'moq2ts-receiver'
if "$binary" --url moqt://127.0.0.1:4433 --namespace live//bad --out /tmp/unused.ts 2>&1; then
  echo 'empty namespace component was accepted' >&2
  exit 1
fi
if "$binary" --url moqt://127.0.0.1:4433 --namespace live/ok --out /tmp/unused.ts --draft 21 2>&1; then
  echo 'unsupported draft was accepted' >&2
  exit 1
fi
