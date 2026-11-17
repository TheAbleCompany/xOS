/*
 * net_artyom.c -- сетевая часть для ArtyomX64.
 *
 * Сеть в этой ОС не реализована, поэтому в сборке только loopback
 * драйвер (NQ/net_none.c).  Здесь лежит то немногое из net_dgrm.c,
 * что нужно остальному коду (sv_user.c регистрирует команду "ban").
 */

#include "client.h"
#include "console.h"
#include "quakedef.h"
#include "server.h"
#include "sys.h"

void
NET_Ban_f(client_t *client)
{
    (void)client;
    Con_Printf("ban: no network support\n");
}
