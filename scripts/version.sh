#!/usr/bin/env bash
set -euo pipefail

# GITHUB_REF_TYPE and GITHUB_REF_NAME are set by GitHub Actions
# when a workflow is triggered by a tag push.
#   GITHUB_REF_TYPE=tag
#   GITHUB_REF_NAME=v2.8.0
if [ "${GITHUB_REF_TYPE:-}" = "tag" ] && [ -n "${GITHUB_REF_NAME:-}" ]; then
  echo "${GITHUB_REF_NAME#v}"
  exit 0
fi

# git describe --long: v2.7.1-3-gabc1234 → 2.7.1.3 (commits-since-tag as tweak)
# On a tag: v2.7.1-0-gabc1234 → 2.7.1.0
# Exclude pre-release tags and non-version tags
DESC=$(git describe --tags --long --match 'v[12].[0-9]*.[0-9]*' --exclude '*-pre*' 2>/dev/null | sed -E 's/^v//;s/-([0-9]+)-.*/.\1/' || true)
if [ -n "$DESC" ]; then
  echo "$DESC"
  exit 0
fi

echo "0.0.0"
