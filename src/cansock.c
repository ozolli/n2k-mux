/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

#include "cansock.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/can.h>
#include <linux/can/raw.h>

int cansock_open(const char *ifname)
{
    int fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) {
        fprintf(stderr, "cansock : socket(PF_CAN) : %s\n", strerror(errno));
        return -1;
    }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        fprintf(stderr, "cansock : interface '%s' introuvable : %s\n",
                ifname, strerror(errno));
        close(fd);
        return -1;
    }
    struct sockaddr_can addr;
    memset(&addr, 0, sizeof addr);
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        fprintf(stderr, "cansock : bind '%s' : %s\n", ifname, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

int cansock_send_iso_request(int fd, int src_addr, int req_pgn)
{
    if (fd < 0)
        return -1;
    /* ISO Request = PGN 59904 (0xEA00), format PDU1 (PF=0xEA < 240) : le champ
     * PS de l'ID CAN porte l'adresse DESTINATION (255 = broadcast). Priorité 6.
     * ID 29 bits = (prio<<26)|(PF<<16)|(DA<<8)|SA. */
    unsigned sa = (unsigned)src_addr & 0xff;
    unsigned id = (6u << 26) | (0xEAu << 16) | (0xFFu << 8) | sa;

    struct can_frame f;
    memset(&f, 0, sizeof f);
    f.can_id = id | CAN_EFF_FLAG;          /* trame étendue 29 bits */
    f.can_dlc = 3;
    f.data[0] = (unsigned char)(req_pgn & 0xff);          /* PGN demandé, LSB */
    f.data[1] = (unsigned char)((req_pgn >> 8) & 0xff);
    f.data[2] = (unsigned char)((req_pgn >> 16) & 0xff);

    ssize_t w = write(fd, &f, sizeof f);
    return (w == (ssize_t)sizeof f) ? 0 : -1;
}

int cansock_recv_dlc(int fd, int *dlc)
{
    struct can_frame f;
    ssize_t r = recv(fd, &f, sizeof f, MSG_DONTWAIT);
    if (r == (ssize_t)sizeof f) {
        if (dlc) *dlc = f.can_dlc;
        return 1;
    }
    return 0;
}
