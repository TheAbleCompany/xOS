.POSIX:

CC = gcc
CFLAGS = -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
         -fno-strict-aliasing -fno-stack-protector -ffreestanding \
         -I. -I../../lib -I../../drivers

NET_OBJS = ixn_netdev.o ixn_skb.o ixn_l2l3.o ixn_l4.o ixn_dhcp_dns.o \
           ixn_http_ws.o ixn_socket_api.o ixn_pci.o ixn_e1000.o \
           ixn_rtl8139.o ixn_eth_misc.o ixn_usbnet.o ixn_wifi.o ixn_tls.o ixn_net.o

all: $(NET_OBJS)

clean:
	rm -f *.o

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
