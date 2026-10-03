/* linuxu shim: atomic_long — atomic_long_* over C11 atomics; strategy
 * (REAL; the header defines most as macros, this
 * file carries anything the header left extern). */
#include <linux/atomic.h>

/* The linuxu/headers/linux/atomic.h shim defines the whole atomic_long
 * family as macros/builtins.  Nothing extra is required at link time;
 * this file exists to anchor the namespace and host any future
 * non-inline helpers. */

/*
 * TODO(linuxu): if linux/atomic.h moves atomic_long_* ops out of
 * inline macros, implement them here with __atomic_* builtins on
 * (atomic_long_t *)->counter.
 */
void linuxu_atomic_long_anchor(void)
{
}
