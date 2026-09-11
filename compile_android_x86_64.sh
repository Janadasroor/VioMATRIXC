#!/bin/bash
# Forwarder script for x86_64 Android build
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$SCRIPT_DIR/compile_android.sh" x86_64 "$@"
