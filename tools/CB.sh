#!/usr/bin/env bash
# Copyright (c) 2025 Kaius Ruokonen. All rights reserved.
# SPDX-License-Identifier: MIT
# See the LICENSE file in the project root for full license text.

# tools/CB.sh — Bootstrap script for C++ Builder & Tester
# Builds cb from tools/cb.c++ and executes it
# Works for standalone tester project

set -e

TOOLS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$TOOLS_DIR/.." && pwd)"
SRC="$TOOLS_DIR/cb.c++"
BIN="$TOOLS_DIR/cb"

# If we're about to run tests in JSONL mode, keep stdout machine-parseable by
# sending wrapper logs to stderr.
JSONL_MODE=false
for arg in "$@"; do
  if [[ "$arg" == "--output=jsonl" || "$arg" == "--output=JSONL" ]]; then
    JSONL_MODE=true
    break
  fi
done

log() {
  if [[ "$JSONL_MODE" == "true" ]]; then
    echo "$@" >&2
  else
    echo "$@"
  fi
}

# Detect OS and set compiler/LLVM paths
UNAME_OUT="$(uname -s)"
case "$UNAME_OUT" in
    Linux)
    CXX_COMPILER="clang++-21"
    LLVM_PREFIX="/usr/lib/llvm-21"
        STD_CPPM_DEFAULT="/usr/lib/llvm-21/share/libc++/v1/std.cppm"
        ;;
    Darwin)
    CXX_COMPILER="/usr/local/llvm/bin/clang++"
    LLVM_PREFIX="/usr/local/llvm"
        STD_CPPM_DEFAULT="/usr/local/llvm/share/libc++/v1/std.cppm"
        ;;
    *)
    echo "ERROR: Unsupported OS '$UNAME_OUT'"
    exit 1
        ;;
esac

# Ensure we always search the LLVM lib dir when linking CB itself
export LDFLAGS="-Wl,-rpath,$LLVM_PREFIX/lib ${LDFLAGS}"

# Check if binary exists and was built for the correct OS
NEEDS_REBUILD=false
if [[ ! -x "$BIN" ]]; then
    NEEDS_REBUILD=true
elif [[ "$SRC" -nt "$BIN" ]]; then
    NEEDS_REBUILD=true
else
    # Check binary format using file command (portable across Linux and macOS)
    if [[ -f "$BIN" ]] && command -v file >/dev/null 2>&1; then
        FILE_TYPE=$(file "$BIN" 2>/dev/null || echo "")
        case "$UNAME_OUT" in
            Linux)
                if [[ "$FILE_TYPE" != *"ELF"* ]]; then
                    echo "CB binary was built for a different OS (not Linux), rebuilding..."
                    NEEDS_REBUILD=true
                fi
                ;;
            Darwin)
                if [[ "$FILE_TYPE" != *"Mach-O"* ]]; then
                    echo "CB binary was built for a different OS (not macOS), rebuilding..."
                    NEEDS_REBUILD=true
                fi
                ;;
        esac
    fi
fi

# Rebuild if needed
if [[ "$NEEDS_REBUILD" == "true" ]]; then
    log "Building C++ Builder & Tester with $CXX_COMPILER..."
    # Use -B to tell clang++ where to find binaries (like the linker)
    "$CXX_COMPILER" \
        -B"$LLVM_PREFIX/bin" \
        -std=c++23 -O3 -pthread \
        -fuse-ld=lld \
        -stdlib=libc++ \
        -I"$LLVM_PREFIX/include/c++/v1" \
        -L"$LLVM_PREFIX/lib" \
        -Wl,-rpath,"$LLVM_PREFIX/lib" \
        "$SRC" -o "$BIN"
    log "C++ Builder & Tester compiled successfully → $BIN"
fi

# Resolve std.cppm path: explicit argument (with slash or .cppm suffix) wins,
# otherwise use LLVM_PATH or defaults per OS.
STD_CPPM=""
if [[ -n "$1" && ("$1" == *.cppm || "$1" == */*) ]]; then
        STD_CPPM="$1"
        shift
fi

if [[ -z "$STD_CPPM" ]]; then
    STD_CPPM="${LLVM_PATH:-$STD_CPPM_DEFAULT}"
fi

if [[ ! -f "$STD_CPPM" ]]; then
    log "ERROR: std.cppm not found at '$STD_CPPM'."
    log "Pass the path as the first argument or set LLVM_PATH."
    exit 1
fi

# Detect project structure and build include flags
# Get the current working directory (where cb will be invoked from)
CURRENT_DIR="$(pwd)"
INCLUDE_FLAGS=()
INCLUDE_EXAMPLES_FLAG=()

# Check if we're in the tester project root (has tester/ subdirectory)
if [[ -d "$PROJECT_ROOT/tester" ]]; then
    # Tester project structure
    INCLUDE_FLAGS=(-I "$PROJECT_ROOT/tester")
    
    # Check if we're building as part of fixer (in deps/tester)
    if [[ "$PROJECT_ROOT" == *"/deps/tester" ]]; then
        # We're in fixer's deps/tester - check if test command is used
        # When running tests, include examples even if in submodule
        for arg in "$@"; do
            if [[ "$arg" == "test" ]]; then
                INCLUDE_EXAMPLES_FLAG=(--include-examples)
                break
            fi
        done
    else
        # We're building tester standalone - include examples
        INCLUDE_EXAMPLES_FLAG=(--include-examples)
    fi
fi

# Allow override via environment variable
if [[ -n "$CB_INCLUDE_FLAGS" ]]; then
    # Parse space-separated paths from environment variable
    INCLUDE_FLAGS=()
    for path in $CB_INCLUDE_FLAGS; do
        INCLUDE_FLAGS+=(-I "$path")
    done
fi

# Run it with resolved std.cppm path and include flags
exec "$BIN" "$STD_CPPM" "${INCLUDE_FLAGS[@]}" "${INCLUDE_EXAMPLES_FLAG[@]}" "$@"

