#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Andrew Spode
# Run Ghidra with the JDK 25 it needs, leaving the system default (JDK 21) alone.
# Usage: tools/ghidra.sh            -> GUI
#        tools/ghidra.sh headless … -> analyzeHeadless with the remaining arguments
G=$HOME/tools/ghidra_12.2_PUBLIC
export JAVA_HOME=$HOME/jdk/jdk-25.0.5+8
if [ "$1" = headless ]; then shift; exec "$G/support/analyzeHeadless" "$@"; fi
exec "$G/ghidraRun" "$@"
