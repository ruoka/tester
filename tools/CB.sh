#!/usr/bin/env bash
# Copyright (c) 2025-2026 Kaius Ruokonen. All rights reserved.
# SPDX-License-Identifier: MIT
# See the LICENSE file in the project root for full license text.

set -e

TOOLS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$TOOLS_DIR/.." && pwd)"

CB_TOOLS_DIR="$TOOLS_DIR"
CB_PROJECT_ROOT="$PROJECT_ROOT"
CB_CB_SOURCE="$TOOLS_DIR/cb.c++"
CB_TESTER_ROOT="$PROJECT_ROOT"

CB_INCLUDE_DIRS=("$PROJECT_ROOT/tester")
CB_INCLUDE_EXAMPLES_MODE=always
CB_RESPECT_CXX_ENV=0

source "$TOOLS_DIR/CB.sh.core"