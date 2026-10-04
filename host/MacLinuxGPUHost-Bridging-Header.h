/* Swift bridging header for the host app: the C firmware servicer that
 * answers the dext's on-demand firmware requests during InitDevice. */
#include "fw_mailbox_service.h"
/* The display agent's virtual displays (display-agent --create). */
#include "CGVirtualDisplayPrivate.h"
/* Darwin notifications between the display daemon and the menu bar. */
#include <notify.h>
