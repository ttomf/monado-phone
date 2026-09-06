// Copyright 2026, Tom F.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Networking for Phone HMD driver.
 * @author Tom F. <tom.fucik@email.cz>
 * @ingroup drv_phone
 */

#include "phone_config.h"
#include "phone_internals.h"


// Fast socket options setter
static void
sock_opt(int sock, int opt, int val)
{
	if (setsockopt(sock, SOL_SOCKET, opt, &val, sizeof(val)) < 0) {
		U_LOG_E("phone: failed to set socket option %d: %d", opt, errno);
	}
}

// Discovery via UDP multicast
bool
phone_discover(struct sockaddr_in *out_addr)
{
	// Create a UDP socket for multicast discovery
	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		U_LOG_W("phone: socket() failed: %d", errno);
		return false;
	}
	sock_opt(sock, SO_REUSEADDR, 1);
	sock_opt(sock, SO_REUSEPORT, 1);

	// Wait max 5 seconds
	struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
	if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
		U_LOG_W("phone: SO_RCVTIMEO failed: %d", errno);
	}

	// Bind the socket to the multicast address.
	struct sockaddr_in bind_addr = {0};
	bind_addr.sin_family = AF_INET;
	bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	bind_addr.sin_port = htons(atoi(config_get("port")));

	if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
		U_LOG_W("phone: bind() failed: %d", errno);
		close(sock);
		return false;
	}

	// Join the multicast group.
	struct ip_mreq mreq = {0};
	mreq.imr_multiaddr.s_addr = inet_addr(config_get("multicast_addr"));
	mreq.imr_interface.s_addr = htonl(INADDR_ANY);

	if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
		U_LOG_W("phone: IP_ADD_MEMBERSHIP failed: %d", errno);
		close(sock);
		return false;
	}

	char recv_buffer[256];
	struct sockaddr_in from = {0};
	socklen_t from_len = sizeof(from);
	while (true) {
		U_LOG_I("phone: recvfrom() waiting...");
		ssize_t n =
		    recvfrom(sock, recv_buffer, sizeof(recv_buffer) - 1, 0, (struct sockaddr *)&from, &from_len);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				U_LOG_W("phone: recvfrom() timed out, no phone found");
				close(sock);
				return false;
			}
			U_LOG_W("phone: recvfrom() failed: %d", errno);
			continue;
		}
		recv_buffer[n] = '\0';
		U_LOG_D("phone: received: %s", recv_buffer);
		if (strcmp(recv_buffer, PHONE_DISCOVER_MSG) == 0) {
			*out_addr = from;
			sendto(sock, PC_DISCOVER_MSG, strlen(PC_DISCOVER_MSG), 0, (struct sockaddr *)&from, from_len);
			close(sock);
			break;
		}
	}
	return true;
}


struct net_config
{
	// TCP socket the phone sends config to
	int sock;
	// Thread
	struct os_thread_helper thread;
	// Thread stop flag
	volatile bool running;
};

static struct net_config *g_net_config = NULL;

static void *
net_config_thread(void *ptr)
{
	struct net_config *nc = (struct net_config *)ptr;
	char buf[4096];
	while (nc->running) {
		ssize_t n = recv(nc->sock, buf, sizeof(buf), 0);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				continue;
			}
			U_LOG_W("phone: config recv() failed: %d", errno);
			continue;
		}
		buf[n] = '\0';
		U_LOG_I("phone: received config: %s", buf);
		// Phone sent screen size, update config and restart Monado
		if (strncmp(buf, "screen ", 7) == 0) {
			char screen_w[16];
			char screen_h[16];

			if (sscanf(buf, "screen %15s %15s", screen_w, screen_h) == 2) {
				if (strcmp(screen_w, config_get("screen_w")) == 0 &&
				    strcmp(screen_h, config_get("screen_h")) == 0) {
					continue;
				}
				config_set("screen_w", screen_w);
				config_set("screen_h", screen_h);
				U_LOG_E("phone: phone screen size changed, please restart Monado");
				exit(0);
			}
		}
	}
	return NULL;
}

bool
net_config_create(const struct sockaddr_in *addr)
{
	if (g_net_config != NULL) {
		return true;
	}

	struct net_config *nc = U_TYPED_CALLOC(struct net_config);
	int server_sock = socket(AF_INET, SOCK_STREAM, 0);
	if (server_sock < 0) {
		U_LOG_W("phone: config socket() failed: %d", errno);
		free(nc);
		return false;
	}
	sock_opt(server_sock, SO_REUSEADDR, 1);

	struct sockaddr_in bind_addr = {0};
	bind_addr.sin_family = AF_INET;
	bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	bind_addr.sin_port = htons(atoi(config_get("config_port")));

	if (bind(server_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
		U_LOG_W("phone: config bind() failed: %d", errno);
		close(server_sock);
		free(nc);
		return false;
	}

	if (listen(server_sock, 10) < 0) {
		U_LOG_W("phone: config listen() failed: %d", errno);
		close(server_sock);
		free(nc);
		return false;
	}

	nc->sock = accept(server_sock, NULL, NULL);
	if (nc->sock < 0) {
		U_LOG_W("phone: config accept() failed: %d", errno);
		close(server_sock);
		free(nc);
		return false;
	}

	close(server_sock);

	nc->running = true;

	os_thread_helper_init(&nc->thread);
	if (os_thread_helper_start(&nc->thread, net_config_thread, nc) != 0) {
		U_LOG_E("phone: failed to start config thread");
		nc->running = false;
		close(nc->sock);
		os_thread_helper_destroy(&nc->thread);
		free(nc);
		return false;
	}

	g_net_config = nc;

	U_LOG_I("phone: config handler started on port %s", config_get("config_port"));
	return true;
}

void
net_config_destroy(void)
{
	struct net_config *nc = g_net_config;
	if (nc == NULL) {
		return;
	}
	g_net_config = NULL;

	nc->running = false;
	close(nc->sock);
	os_thread_helper_stop_and_wait(&nc->thread);
	os_thread_helper_destroy(&nc->thread);

	free(nc);
}

struct net_stream
{
	struct xrt_frame_context xfctx;
	struct gstreamer_pipeline *gp;
	struct gstreamer_sink *gs;
	struct xrt_frame_sink *xfs;
};

static struct net_stream *g_net_stream = NULL;

bool
net_stream_create(const struct sockaddr_in *addr, struct xrt_frame_sink **out_xfs)
{
	if (g_net_stream != NULL) {
		*out_xfs = g_net_stream->xfs;
		return true;
	}

	struct net_stream *ns = U_TYPED_CALLOC(struct net_stream);
	char ip_str[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &addr->sin_addr, ip_str, sizeof(ip_str));

	char pipeline[1024];
	snprintf(pipeline, sizeof(pipeline),
	         "appsrc name=xr_src format=time is-live=true do-timestamp=true ! "
	         "videoconvert ! "
	         "videoscale ! "
	         "video/x-raw,format=NV12,width=%s,height=%s ! "
	         "queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 leaky=downstream ! "
	         "vah265enc target-usage=7 rate-control=cqp min-qp=22 max-qp=22 ! "
	         "h265parse config-interval=1 ! "
	         "video/x-h265,stream-format=byte-stream,alignment=nal ! "
	         "rtph265pay config-interval=1 ! "
	         "udpsink host=%s port=%s sync=false async=false",
	         config_get("stream_w"), config_get("stream_h"), ip_str, config_get("stream_port"));
	U_LOG_I("phone: stream pipeline: %s", pipeline);

	gstreamer_pipeline_create_from_string(&ns->xfctx, pipeline, &ns->gp);
	gstreamer_sink_create_with_pipeline(ns->gp, atoi(config_get("stream_w")), atoi(config_get("stream_h")),
	                                    XRT_FORMAT_R8G8B8A8, "xr_src", &ns->gs, &ns->xfs);
	gstreamer_pipeline_play(ns->gp);
	g_net_stream = ns;
	*out_xfs = ns->xfs;

	U_LOG_I("phone: stream sink created");

	return true;
}

void
net_stream_destroy(void)
{
	struct net_stream *ns = g_net_stream;
	if (ns == NULL) {
		return;
	}
	g_net_stream = NULL;

	gstreamer_pipeline_stop(ns->gp);
	xrt_frame_context_destroy_nodes(&ns->xfctx);

	free(ns);
}


struct hand_tracker
{
	// UDP socket the phone sends hand data to
	int sock;
	// Thread
	struct os_thread_helper thread;
	// Thread stop flag
	volatile bool running;
	// Packed read by hmd
	struct hand_packet *packet;
	// Protects packet, shared with the hmd
	struct os_mutex *lock;
};

static struct hand_tracker *g_hand = NULL;

static void *
net_hand_thread(void *ptr)
{
	struct hand_tracker *ht = (struct hand_tracker *)ptr;

	uint8_t buf[513];
	while (ht->running) {
		ssize_t n = recvfrom(ht->sock, buf, sizeof(buf), 0, NULL, NULL);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				continue;
			}
			if (!ht->running) {
				break;
			}
			U_LOG_W("phone: hand recvfrom() failed: %d", errno);
			continue;
		}
		struct hand_packet pp = {0};

		if (n < (ssize_t)(sizeof(pp.timestamp_ns) + sizeof(pp.flags))) {
			U_LOG_W("phone: hand packet size too small");
			continue;
		}

		memcpy(&pp.timestamp_ns, &buf[0], sizeof(pp.timestamp_ns));
		memcpy(&pp.flags, &buf[sizeof(pp.timestamp_ns)], sizeof(pp.flags));
		if (pp.flags & (1 << 0) && pp.flags & (1 << 1)) { // both
			if (n != sizeof(pp.timestamp_ns) + sizeof(pp.flags) + sizeof(pp.left) + sizeof(pp.right)) {
				U_LOG_W("phone: hand packet size mismatch");
				continue;
			}
			memcpy(&pp.left, &buf[sizeof(pp.timestamp_ns) + sizeof(pp.flags)], sizeof(pp.left));
			memcpy(&pp.right, &buf[sizeof(pp.timestamp_ns) + sizeof(pp.flags) + sizeof(pp.left)],
			       sizeof(pp.right));
		} else if (pp.flags & (1 << 0)) { // left
			if (n != sizeof(pp.timestamp_ns) + sizeof(pp.flags) + sizeof(pp.left)) {
				U_LOG_W("phone: hand packet size mismatch");
				continue;
			}
			memcpy(&pp.left, &buf[sizeof(pp.timestamp_ns) + sizeof(pp.flags)], sizeof(pp.left));
		} else if (pp.flags & (1 << 1)) { // right
			if (n != sizeof(pp.timestamp_ns) + sizeof(pp.flags) + sizeof(pp.right)) {
				U_LOG_W("phone: hand packet size mismatch");
				continue;
			}
			memcpy(&pp.right, &buf[sizeof(pp.timestamp_ns) + sizeof(pp.flags)], sizeof(pp.right));
		}

		// Publish under the lock so the hmd never reads a torn packet
		os_mutex_lock(ht->lock);
		memcpy(ht->packet, &pp, sizeof(pp));
		os_mutex_unlock(ht->lock);
	}
	return NULL;
}

bool
net_hand_create(struct hand_packet *out_packet, struct os_mutex *hand_lock)
{
	if (g_hand != NULL) {
		return true;
	}

	struct hand_tracker *ht = U_TYPED_CALLOC(struct hand_tracker);

	ht->sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (ht->sock < 0) {
		U_LOG_W("phone: hand socket() failed: %d", errno);
		free(ht);
		return false;
	}
	sock_opt(ht->sock, SO_REUSEADDR, 1);

	struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};
	if (setsockopt(ht->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
		U_LOG_W("phone: pose SO_RCVTIMEO failed: %d", errno);
	}

	struct sockaddr_in bind_addr = {0};
	bind_addr.sin_family = AF_INET;
	bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	bind_addr.sin_port = htons(atoi(config_get("hand_port")));

	if (bind(ht->sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
		U_LOG_W("phone: hand bind() failed: %d", errno);
		close(ht->sock);
		free(ht);
		return false;
	}

	ht->packet = out_packet;
	ht->lock = hand_lock;
	ht->running = true;

	os_thread_helper_init(&ht->thread);
	if (os_thread_helper_start(&ht->thread, net_hand_thread, ht) != 0) {
		U_LOG_E("phone: failed to start config thread");
		ht->running = false;
		close(ht->sock);
		os_thread_helper_destroy(&ht->thread);
		free(ht);
		return false;
	}

	g_hand = ht;

	U_LOG_I("phone: hand tracker started on port %s", config_get("hand_port"));

	return true;
}

void
net_hand_destroy(void)
{
	struct hand_tracker *ht = g_hand;
	if (ht == NULL) {
		return;
	}
	g_hand = NULL;

	// Wake up the thread blocked in recvfrom()
	ht->running = false;
	close(ht->sock);
	os_thread_helper_stop_and_wait(&ht->thread);
	os_thread_helper_destroy(&ht->thread);

	free(ht);
}


struct pose_receiver
{
	// UDP socket the phone sends poses to
	int sock;
	// Receiver thread
	struct os_thread_helper thread;
	// History the received poses are pushed into, owned by the HMD device
	struct m_relation_history *rh;
	// Thread stop flag
	volatile bool running;
};

struct pose_packet
{
	int64_t timestamp_ns;
	int32_t tracking_state;
	float qx, qy, qz;
	float qw, tx, ty, tz;
};

static struct pose_receiver *g_pose = NULL;

static void *
net_pose_thread(void *ptr)
{
	struct pose_receiver *pr = (struct pose_receiver *)ptr;

	uint8_t buf[40];
	while (pr->running) {
		ssize_t n = recvfrom(pr->sock, buf, sizeof(buf), 0, NULL, NULL);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				continue;
			}
			if (!pr->running) {
				break;
			}
			U_LOG_W("phone: pose recvfrom() failed: %d", errno);
			continue;
		}

		struct pose_packet *pp = (struct pose_packet *)buf;
		if (pp->tracking_state != 0) {
			continue;
		}

		struct xrt_space_relation rel;
		rel.pose.orientation.x = pp->qx;
		rel.pose.orientation.y = pp->qy;
		rel.pose.orientation.z = pp->qz;
		rel.pose.orientation.w = pp->qw;
		rel.pose.position.x = pp->tx;
		rel.pose.position.y = pp->ty;
		rel.pose.position.z = pp->tz;
		math_quat_normalize(&rel.pose.orientation);

		rel.relation_flags = (enum xrt_space_relation_flags)(
		    XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT | XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
		    XRT_SPACE_RELATION_POSITION_TRACKED_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT);

		m_relation_history_push(pr->rh, &rel, os_monotonic_get_ns());
	}

	return NULL;
}

bool
net_pose_create(struct m_relation_history *rh)
{
	if (g_pose != NULL) {
		return true;
	}

	struct pose_receiver *pr = U_TYPED_CALLOC(struct pose_receiver);

	pr->sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (pr->sock < 0) {
		U_LOG_W("phone: pose socket() failed: %d", errno);
		free(pr);
		return false;
	}
	sock_opt(pr->sock, SO_REUSEADDR, 1);

	struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};
	if (setsockopt(pr->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
		U_LOG_W("phone: pose SO_RCVTIMEO failed: %d", errno);
	}

	struct sockaddr_in bind_addr = {0};
	bind_addr.sin_family = AF_INET;
	bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	bind_addr.sin_port = htons(atoi(config_get("pose_port")));

	if (bind(pr->sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
		U_LOG_W("phone: pose bind() failed: %d", errno);
		close(pr->sock);
		free(pr);
		return false;
	}

	pr->rh = rh;
	pr->running = true;

	os_thread_helper_init(&pr->thread);
	if (os_thread_helper_start(&pr->thread, net_pose_thread, pr) != 0) {
		U_LOG_E("phone: failed to start pose thread");
		pr->running = false;
		close(pr->sock);
		os_thread_helper_destroy(&pr->thread);
		free(pr);
		return false;
	}

	g_pose = pr;

	U_LOG_I("phone: pose receiver started on port %s", config_get("pose_port"));

	return true;
}

void
net_pose_destroy(void)
{
	struct pose_receiver *pr = g_pose;
	if (pr == NULL) {
		return;
	}
	g_pose = NULL;

	// Wake up the thread blocked in recvfrom()
	pr->running = false;
	close(pr->sock);
	os_thread_helper_stop_and_wait(&pr->thread);
	os_thread_helper_destroy(&pr->thread);

	free(pr);
}
