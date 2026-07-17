#!/usr/bin/env sh
set -eu

# Fork-only names must not return through copied files, generated templates or
# new public endpoints. Legal attribution lives in LICENSE, CREDITS and README.
for pattern in 'noctalia' 'dev\.gnil' 'gnil-dev/gnil' 'gnil\.dev'; do
  matches=$(rg -n -i \
    --glob '!LICENSE' \
    --glob '!CREDITS*' \
    --glob '!README.md' \
    --glob '!tests/branding_guard_test.sh' \
    --glob '!.git/**' \
    "$pattern" . || true)
  if [ -n "$matches" ]; then
    printf '%s\n%s\n' "obsolete branding matched /$pattern/:" "$matches" >&2
    exit 1
  fi
done
