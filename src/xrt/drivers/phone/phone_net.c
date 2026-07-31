// Copyright 2026, Tom F.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Phone HMD network implementation.
 * @author Tom F. <tom.fucik@email.cz>
 * @ingroup drv_phone
 */

#include <arpa/inet.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "phone_internals.h"
#include "util/u_logging.h"

#define PHONE_MCAST_ADDR "239.1.1.1"
#define PHONE_MCAST_PORT 5500
#define PHONE_DISCOVER_MSG_PHONE "MONADO_PHONE_DISCOVER_PHONE"
#define PHONE_DISCOVER_MSG_PC "MONADO_PHONE_DISCOVER_PC"

static void
sock_opt(int sock, int opt, int val)
{
	if (setsockopt(sock, SOL_SOCKET, opt, &val, sizeof(val)) < 0) {
		U_LOG_E("Failed to set socket option %d", opt);
	}
}

bool
phone_discover(struct sockaddr_in *out_addr)
{
	// Create a UDP socket for multicast discovery.
	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		U_LOG_W("phone: socket() failed: %d", errno);
		return false;
	};
	sock_opt(sock, SO_REUSEADDR, 1);
	sock_opt(sock, SO_REUSEPORT, 1);

	// Bind the socket to the multicast address.
	struct sockaddr_in bind_addr = {0};
	bind_addr.sin_family = AF_INET;
	bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	bind_addr.sin_port = htons(PHONE_MCAST_PORT);

	if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
		U_LOG_W("phone: bind() failed: %d", errno);
		close(sock);
		return false;
	}

	// Join the multicast group.
	struct ip_mreq mreq = {0};
	mreq.imr_multiaddr.s_addr = inet_addr(PHONE_MCAST_ADDR);
	mreq.imr_interface.s_addr = htonl(INADDR_ANY);

	if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
		U_LOG_W("phone: IP_ADD_MEMBERSHIP failed: %d", errno);
		close(sock);
		return false;
	}

	char recvBuffer[256];
	struct sockaddr_in from = {0};
	socklen_t from_len = sizeof(from);
	while (true) {
		U_LOG_W("phone: recvfrom() waiting...");
		ssize_t n = recvfrom(sock, recvBuffer, sizeof(recvBuffer) - 1, 0, (struct sockaddr *)&from, &from_len);
		if (n < 0) {
			U_LOG_W("phone: recvfrom() failed: %d", errno);
			continue;
		}
		recvBuffer[n] = '\0';
		U_LOG_I("phone: received: %s", recvBuffer);
		if (strcmp(recvBuffer, PHONE_DISCOVER_MSG_PHONE) == 0) {
			*out_addr = from;
			sendto(sock, PHONE_DISCOVER_MSG_PC, strlen(PHONE_DISCOVER_MSG_PC), 0, (struct sockaddr *)&from,
			       from_len);
			close(sock);
			break;
		}
	}
	return true;
}
