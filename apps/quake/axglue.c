/*
 * axglue.c -- единственный экземпляр состояния IPC-соединения
 * с WindowServer'ом (см. комментарий в axglue.h).
 */
#include "axglue.h"

int      axg_srv_port = -1;
int      axg_cli_port = -1;
int      axg_pid      = -2;
ax_msg_t axg_ring[AXG_RING_MAX];
int      axg_ring_n;
