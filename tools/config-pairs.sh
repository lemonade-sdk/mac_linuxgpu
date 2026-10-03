#!/bin/sh
# tools/config-pairs.sh — emit "CONFIG_SYM y|n" pairs from mk/kernel_config.mk
# (top-level `CONFIG_FOO := y|n` lines only). Used by `make config-check`.
awk '/^CONFIG_/ {
	line = $0
	sub(/^CONFIG_/, "", line)
	n = split(line, b, " := ")
	if (n < 2) next
	n2 = split(b[2], v, " ")
	if (v[1] == "y" || v[1] == "n")
		print "CONFIG_" b[1], v[1]
}' mk/kernel_config.mk
