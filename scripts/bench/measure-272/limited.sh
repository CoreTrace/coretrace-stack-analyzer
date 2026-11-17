#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Runs a command as the calling user in a cgroup of 4 CPUs and 6 GiB, without swap.
#   limited.sh <command> [argument...]
set -euo pipefail

exec sudo systemd-run --scope --quiet \
    -p AllowedCPUs=0-3 -p MemoryMax=6G -p MemorySwapMax=0 \
    --uid="$(id -u)" --gid="$(id -g)" -- env PATH="${PATH}" "$@"
