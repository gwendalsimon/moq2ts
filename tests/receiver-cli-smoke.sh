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
# Drafts 22 (the default) and 18 are accepted, any other is a usage error (2).
for draft in 16 21; do
  rc=0
  "$binary" --url moqt://127.0.0.1:9 --namespace live/ok --out /tmp/unused.ts --draft "$draft" > /dev/null 2>&1 || rc=$?
  if [ "$rc" -ne 2 ]; then
    echo "draft $draft was not refused as a usage error (exit $rc)" >&2
    exit 1
  fi
done
for draft in 18 22; do
  rc=0
  timeout 20 "$binary" --url moqt://127.0.0.1:9 --namespace live/ok --out /tmp/unused.ts --draft "$draft" --duration 1 > /dev/null 2>&1 || rc=$?
  if [ "$rc" -eq 2 ]; then
    echo "draft $draft was refused as a usage error" >&2
    exit 1
  fi
done
