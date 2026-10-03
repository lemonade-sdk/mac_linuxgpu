#!/bin/sh
# tools/config-check.sh — compare mk/kernel_config.mk against the autoconf.h
# shadow header. Prints MISMATCH lines for any divergence; prints OK when
# consistent. Usage: sh tools/config-check.sh <autoconf.h>
autoconf="$1"

bad=0
while read -r sym val; do
	defd=$(grep -cE "^#define[[:space:]]+${sym}[[:space:]]" "$autoconf")
	unfd=$(grep -cE "^#undef[[:space:]]+${sym}[[:space:]]*$" "$autoconf")
	if [ "$unfd" -gt 0 ]; then defd=0; fi
	if [ "$val" = "y" ] && [ "$defd" -eq 0 ]; then
		echo "  MISMATCH: $sym = y in mk/kernel_config.mk but not #define'd in $autoconf"
		bad=1
	elif [ "$val" = "n" ] && [ "$defd" -gt 0 ]; then
		echo "  MISMATCH: $sym = n in mk/kernel_config.mk but #define'd in $autoconf"
		bad=1
	fi
done <<EOF
$(sh tools/config-pairs.sh)
EOF

if [ "$bad" -eq 0 ]; then
	echo "  OK: all CONFIG entries consistent with $autoconf"
fi
exit 0
