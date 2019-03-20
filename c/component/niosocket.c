//
// Created by hujianzhe on 18-8-13.
//

#include "../syslib/error.h"
#include "niosocket.h"
#include <stdlib.h>
#include <string.h>

enum {
	NIO_SOCKET_USER_MESSAGE,
	NIO_SOCKET_CLOSE_MESSAGE,
	NIO_SOCKET_SHUTDOWN_MESSAGE,
	NIO_SOCKET_REG_MESSAGE,
	NIO_SOCKET_STREAM_WRITEABLE_MESSAGE,
	NIO_SOCKET_RELIABLE_MESSAGE
};

enum {
	HDR_SYN,
	HDR_SYN_ACK,
	HDR_SYN_ACK_ACK,
	HDR_FIN,
	HDR_FIN_ACK,
	HDR_DATA,
	HDR_ACK
};
enum {
	IDLE_STATUS = 0,
	LISTENED_STATUS,
	SYN_SENT_STATUS,
	ESTABLISHED_STATUS,
	FIN_WAIT_1_STATUS,
	FIN_WAIT_2_STATUS,
	CLOSE_WAIT_STATUS,
	TIME_WAIT_STATUS,
	LAST_ACK_STATUS,
	CLOSED_STATUS
};
#define	RELIABLE_HDR_LEN	5
#define	MSL					30000

typedef struct Packet_t {
	NioMsg_t msg;
	struct sockaddr_storage saddr;
	NioSocket_t* s;
	size_t offset;
	size_t len;
	unsigned char data[1];
} Packet_t;

typedef struct ReliableHalfConnect_t {
	ListNode_t m_listnode;
	FD_t sockfd;
	long long timestamp_msec;
	unsigned short resend_times;
	unsigned short local_port;
	struct sockaddr_storage peer_addr;
} ReliableHalfConnect_t;

typedef struct ReliableDataPacket_t {
	NioMsg_t msg;
	long long resend_timestamp_msec;
	unsigned int resendtimes;
	struct sockaddr_storage saddr;
	NioSocket_t* s;
	unsigned int seq;
	size_t len;
	unsigned char data[1];
} ReliableDataPacket_t;

#ifdef __cplusplus
extern "C" {
#endif

static void update_timestamp(long long* dst, long long timestamp) {
	if (0 == *dst || *dst > timestamp)
		*dst = timestamp;
}

static NioLoop_t* nioloop_exec_msg(NioLoop_t* loop, ListNode_t* msgnode) {
	int need_wake;
	criticalsectionEnter(&loop->m_msglistlock);
	need_wake = !loop->m_msglist.head;
	listInsertNodeBack(&loop->m_msglist, loop->m_msglist.tail, msgnode);
	criticalsectionLeave(&loop->m_msglistlock);
	if (need_wake) {
		char c;
		socketWrite(loop->m_socketpair[1], &c, sizeof(c), 0, NULL);
	}
	else {
		nioloopWake(loop);
	}
	return loop;
}

static NioLoop_t* nioloop_exec_msglist(NioLoop_t* loop, List_t* msglist) {
	int need_wake;
	criticalsectionEnter(&loop->m_msglistlock);
	need_wake = !loop->m_msglist.head;
	listMerge(&loop->m_msglist, msglist);
	criticalsectionLeave(&loop->m_msglistlock);
	if (need_wake) {
		char c;
		socketWrite(loop->m_socketpair[1], &c, sizeof(c), 0, NULL);
	}
	else {
		nioloopWake(loop);
	}
	return loop;
}

static int reactorsocket_read(NioSocket_t* s) {
	struct sockaddr_storage saddr;
	int opcode;
	if (!s->valid)
		return 0;
	else if (s->accept_callback && SOCK_STREAM == s->socktype) {
		opcode = REACTOR_ACCEPT;
		saddr.ss_family = s->domain;
	}
	else {
		opcode = REACTOR_READ;
	}
	if (!s->m_readOl) {
		s->m_readOl = reactorMallocOverlapped(opcode, NULL, 0, SOCK_STREAM != s->socktype ? 65000 : 0);
		if (!s->m_readOl) {
			s->valid = 0;
			return 0;
		}
	}
	if (reactorCommit(&s->m_loop->m_reactor, s->fd, opcode, s->m_readOl, &saddr))
		return 1;
	s->valid = 0;
	return 0;
}

static void send_fin_packet(NioLoop_t* loop, NioSocket_t* s, long long timestamp_msec) {
	unsigned char fin = HDR_FIN;
	socketWrite(s->fd, &fin, sizeof(fin), 0, &s->reliable.peer_saddr);
	s->reliable.m_fin_msec = timestamp_msec + s->reliable.rto;
	if (ESTABLISHED_STATUS == s->reliable.m_status) {
		s->reliable.m_status = FIN_WAIT_1_STATUS;
		update_timestamp(&loop->m_checkexpire_msec, s->reliable.m_fin_msec);
	}
	else if (CLOSE_WAIT_STATUS == s->reliable.m_status) {
		s->reliable.m_status = LAST_ACK_STATUS;
		update_timestamp(&loop->m_checkexpire_msec, s->reliable.m_fin_msec);
	}
}

static int reactor_socket_reliable_read(NioSocket_t* s, unsigned char* buffer, int len, const struct sockaddr_storage* saddr) {
	unsigned char hdr_type;
	if (TIME_WAIT_STATUS == s->reliable.m_status || CLOSED_STATUS == s->reliable.m_status)
		return 1;
	hdr_type = buffer[0];
	if (s->accept_callback) {
		ListNode_t* cur, *next;
		ReliableHalfConnect_t* halfcon;
		if (HDR_SYN == hdr_type) {
			unsigned char syn_ack[3];
			halfcon = NULL;
			for (cur = s->m_recvpacketlist.head; cur; cur = next) {
				next = cur->next;
				halfcon = pod_container_of(cur, ReliableHalfConnect_t, m_listnode);
				if (!memcmp(&halfcon->peer_addr, saddr, sizeof(halfcon->peer_addr)))
					break;
				halfcon = NULL;
			}
			if (halfcon) {
				syn_ack[0] = HDR_SYN_ACK;
				*(unsigned short*)(syn_ack + 1) = htons(halfcon->local_port);
			}
			else {
				FD_t new_fd;
				IPString_t ipstr;
				unsigned short local_port;
				struct sockaddr_storage local_saddr = s->local_listen_saddr;
				if (!sockaddrSetPort(&local_saddr, 0))
					return 1;
				new_fd = socket(s->domain, s->socktype, s->protocol);
				if (new_fd == INVALID_FD_HANDLE)
					return 1;
				if (!socketBindAddr(new_fd, &local_saddr)) {
					socketClose(new_fd);
					return 1;
				}
				if (!socketGetLocalAddr(new_fd, &local_saddr)) {
					socketClose(new_fd);
					return 1;
				}
				if (!sockaddrDecode(&local_saddr, ipstr, &local_port)) {
					socketClose(new_fd);
					return 1;
				}
				if (!socketNonBlock(new_fd, TRUE)) {
					socketClose(new_fd);
					return 1;
				}
				halfcon = (ReliableHalfConnect_t*)malloc(sizeof(ReliableHalfConnect_t));
				if (!halfcon) {
					socketClose(new_fd);
					return 1;
				}
				halfcon->local_port = local_port;
				halfcon->resend_times = 0;
				halfcon->peer_addr = *saddr;
				halfcon->sockfd = new_fd;
				halfcon->timestamp_msec = gmtimeMillisecond() + s->reliable.rto;

				listInsertNodeBack(&s->m_recvpacketlist, s->m_recvpacketlist.tail, &halfcon->m_listnode);
				update_timestamp(&s->m_loop->m_checkexpire_msec, halfcon->timestamp_msec);

				syn_ack[0] = HDR_SYN_ACK;
				*(unsigned short*)(syn_ack + 1) = htons(local_port);
			}
			socketWrite(s->fd, syn_ack, sizeof(syn_ack), 0, saddr);
		}
		else if (HDR_SYN_ACK_ACK == hdr_type) {
			struct sockaddr_storage peer_addr;
			for (cur = s->m_recvpacketlist.head; cur; cur = next) {
				halfcon = pod_container_of(cur, ReliableHalfConnect_t, m_listnode);
				next = cur->next;
				if (memcmp(&halfcon->peer_addr, saddr, sizeof(halfcon->peer_addr)))
					continue;
				if (socketRead(halfcon->sockfd, NULL, 0, 0, &peer_addr))
					break;
				listRemoveNode(&s->m_recvpacketlist, cur);
				s->accept_callback(s, halfcon->sockfd, &peer_addr);
				free(halfcon);
				break;
			}
		}
	}
	else if (HDR_SYN_ACK == hdr_type) {
		unsigned char syn_ack_ack;
		if (len < 3)
			return 1;
		if (memcmp(saddr, &s->peer_listen_saddr, sizeof(s->peer_listen_saddr)))
			return 1;
		syn_ack_ack = HDR_SYN_ACK_ACK;
		socketWrite(s->fd, &syn_ack_ack, sizeof(syn_ack_ack), 0, saddr);
		if (SYN_SENT_STATUS == s->reliable.m_status) {
			unsigned short peer_port;
			s->reliable.m_status = ESTABLISHED_STATUS;
			peer_port = *(unsigned short*)(buffer + 1);
			peer_port = ntohs(peer_port);
			sockaddrSetPort(&s->reliable.peer_saddr, peer_port);
			if (s->connect_callback) {
				s->connect_callback(s, 0);
				s->connect_callback = NULL;
			}
		}
		socketWrite(s->fd, NULL, 0, 0, &s->reliable.peer_saddr);
	}
	else if (HDR_FIN == hdr_type) {
		if (memcmp(saddr, &s->reliable.peer_saddr, sizeof(*saddr)))
			return 1;
		else {
			unsigned char fin_ack = HDR_FIN_ACK;
			socketWrite(s->fd, &fin_ack, sizeof(fin_ack), 0, &s->reliable.peer_saddr);
			if (ESTABLISHED_STATUS == s->reliable.m_status) {
				s->reliable.m_status = CLOSE_WAIT_STATUS;
				s->m_shutwr = 1;
				dataqueuePush(s->m_loop->m_msgdq, &s->m_shutdownmsg.m_listnode);
				if (0 == _cmpxchg16(&s->m_shutdown, 2, 0) && !s->m_sendpacketlist.head) {
					send_fin_packet(s->m_loop, s, gmtimeMillisecond());
				}
			}
			else if (FIN_WAIT_1_STATUS == s->reliable.m_status ||
				FIN_WAIT_2_STATUS == s->reliable.m_status)
			{
				s->reliable.m_status = TIME_WAIT_STATUS;
				s->m_lastactive_msec = gmtimeMillisecond();
				s->timeout_msec = MSL + MSL;
				dataqueuePush(s->m_loop->m_msgdq, &s->m_shutdownmsg.m_listnode);
			}
		}
	}
	else if (HDR_FIN_ACK == hdr_type) {
		if (memcmp(saddr, &s->reliable.peer_saddr, sizeof(*saddr)))
			return 1;
		else if (LAST_ACK_STATUS == s->reliable.m_status) {
			s->reliable.m_status = CLOSED_STATUS;
			s->m_lastactive_msec = gmtimeMillisecond();
			s->timeout_msec = MSL + MSL;
		}
		else if (FIN_WAIT_1_STATUS == s->reliable.m_status) {
			s->reliable.m_status = FIN_WAIT_2_STATUS;
		}
	}
	else if (HDR_ACK == hdr_type) {
		ListNode_t* cur;
		unsigned int seq, cwnd_skip, ack_valid;
		if (len < RELIABLE_HDR_LEN)
			return 1;
		if (ESTABLISHED_STATUS > s->reliable.m_status)
			return 1;
		if (memcmp(saddr, &s->reliable.peer_saddr, sizeof(*saddr)))
			return 1;
		seq = *(unsigned int*)(buffer + 1);
		seq = ntohl(seq);
		cwnd_skip = 0;
		ack_valid = 0;
		for (cur = s->m_sendpacketlist.head; cur; cur = cur->next) {
			ReliableDataPacket_t* packet = pod_container_of(cur, ReliableDataPacket_t, msg.m_listnode);
			if (seq < packet->seq)
				break;
			if (packet->seq == seq) {
				ListNode_t* next = cur->next;
				listRemoveNode(&s->m_sendpacketlist, cur);
				free(packet);
				if (seq == s->reliable.m_cwndseq) {
					if (next) {
						packet = pod_container_of(next, ReliableDataPacket_t, msg.m_listnode);
						s->reliable.m_cwndseq = packet->seq;
						cwnd_skip = 1;
					}
					else
						++s->reliable.m_cwndseq;
				}
				ack_valid = 1;
				break;
			}
		}
		if (cwnd_skip) {
			for (cur = s->m_sendpacketlist.head; cur; cur = cur->next) {
				ReliableDataPacket_t* packet = pod_container_of(cur, ReliableDataPacket_t, msg.m_listnode);
				if (packet->seq < s->reliable.m_cwndseq ||
					packet->seq - s->reliable.m_cwndseq >= s->reliable.cwndsize)
				{
					break;
				}
				socketWrite(s->fd, packet->data, packet->len, 0, &packet->saddr);
				packet->resend_timestamp_msec = gmtimeMillisecond() + s->reliable.rto;
				update_timestamp(&s->m_loop->m_checkexpire_msec, packet->resend_timestamp_msec);
			}
		}
		if (ack_valid && !s->m_sendpacketlist.head && s->m_shutwr) {
			send_fin_packet(s->m_loop, s, gmtimeMillisecond());
		}
	}
	else if (HDR_DATA == hdr_type) {
		ListNode_t* cur, *next;
		ReliableDataPacket_t* packet;
		unsigned int seq;
		unsigned char ack[RELIABLE_HDR_LEN];
		if (len < RELIABLE_HDR_LEN)
			return 1;
		if (ESTABLISHED_STATUS > s->reliable.m_status)
			return 1;
		if (memcmp(saddr, &s->reliable.peer_saddr, sizeof(*saddr)))
			return 1;

		seq = *(unsigned int*)(buffer + 1);
		ack[0] = HDR_ACK;
		*(unsigned int*)(ack + 1) = seq;
		socketWrite(s->fd, ack, sizeof(ack), 0, saddr);

		seq = ntohl(seq);
		if (seq < s->reliable.m_recvseq)
			return 1;
		else if (seq == s->reliable.m_recvseq) {
			NioMsg_t* msgptr;
			s->reliable.m_recvseq++;
			msgptr = NULL;
			len -= RELIABLE_HDR_LEN;
			buffer = len ? buffer + RELIABLE_HDR_LEN : NULL;
			if (s->decode_packet(s, buffer, len, saddr, &msgptr) < 0) {
				//s->valid = 0;
				//return 0;
			}
			if (msgptr) {
				msgptr->type = NIO_SOCKET_USER_MESSAGE;
				dataqueuePush(s->m_loop->m_msgdq, &msgptr->m_listnode);
			}
			for (cur = s->m_recvpacketlist.head; cur; cur = next) {
				packet = pod_container_of(cur, ReliableDataPacket_t, msg.m_listnode);
				if (packet->seq != s->reliable.m_recvseq)
					break;
				next = cur->next;
				s->reliable.m_recvseq++;
				msgptr = NULL;
				if (s->decode_packet(s, packet->len ? packet->data : NULL, packet->len, &packet->saddr, &msgptr) < 0) {
					//s->valid = 0;
					//return 0;
				}
				listRemoveNode(&s->m_recvpacketlist, cur);
				free(packet);
				if (msgptr) {
					msgptr->type = NIO_SOCKET_USER_MESSAGE;
					dataqueuePush(s->m_loop->m_msgdq, &msgptr->m_listnode);
				}
			}
		}
		else {
			for (cur = s->m_recvpacketlist.head; cur; cur = cur->next) {
				packet = pod_container_of(cur, ReliableDataPacket_t, msg.m_listnode);
				if (packet->seq > seq)
					break;
				else if (packet->seq == seq)
					return 1;
			}
			packet = (ReliableDataPacket_t*)malloc(sizeof(ReliableDataPacket_t) + len - RELIABLE_HDR_LEN);
			if (!packet) {
				//s->valid = 0;
				return 0;
			}
			packet->msg.type = NIO_SOCKET_USER_MESSAGE;
			packet->saddr = *saddr;
			packet->s = s;
			packet->seq = seq;
			packet->len = len - RELIABLE_HDR_LEN;
			memcpy(packet->data, buffer + RELIABLE_HDR_LEN, len - RELIABLE_HDR_LEN);
			if (cur)
				listInsertNodeFront(&s->m_recvpacketlist, cur, &packet->msg.m_listnode);
			else
				listInsertNodeBack(&s->m_recvpacketlist, s->m_recvpacketlist.tail, &packet->msg.m_listnode);
		}
	}
	return 1;
}

static void reactor_socket_reliable_update(NioLoop_t* loop, NioSocket_t* s, long long timestamp_msec) {
	if (LISTENED_STATUS == s->reliable.m_status) {
		ListNode_t* cur, *next;
		for (cur = s->m_recvpacketlist.head; cur; cur = next) {
			ReliableHalfConnect_t* halfcon = pod_container_of(cur, ReliableHalfConnect_t, m_listnode);
			next = cur->next;
			if (halfcon->timestamp_msec > timestamp_msec) {
				update_timestamp(&loop->m_checkexpire_msec, halfcon->timestamp_msec);
			}
			else if (halfcon->resend_times >= s->reliable.resend_maxtimes) {
				socketClose(halfcon->sockfd);
				listRemoveNode(&s->m_recvpacketlist, cur);
				free(halfcon);
			}
			else {
				unsigned char syn_ack[3];
				syn_ack[0] = HDR_SYN_ACK;
				*(unsigned short*)(syn_ack + 1) = htons(halfcon->local_port);
				socketWrite(s->fd, syn_ack, sizeof(syn_ack), 0, &halfcon->peer_addr);
				++halfcon->resend_times;
				halfcon->timestamp_msec = timestamp_msec + s->reliable.rto;
				update_timestamp(&loop->m_checkexpire_msec, halfcon->timestamp_msec);
			}
		}
	}
	else if (SYN_SENT_STATUS == s->reliable.m_status) {
		if (s->reliable.m_synsent_msec > timestamp_msec) {
			update_timestamp(&loop->m_checkexpire_msec, s->reliable.m_synsent_msec);
		}
		else if (s->reliable.m_synsent_times >= s->reliable.resend_maxtimes) {
			s->reliable.m_status = TIME_WAIT_STATUS;
			s->m_lastactive_msec = timestamp_msec;
			s->timeout_msec = MSL + MSL;
			s->connect_callback(s, ETIMEDOUT);
			update_timestamp(&loop->m_checkexpire_msec, s->m_lastactive_msec + s->timeout_msec);
		}
		else {
			unsigned char syn = HDR_SYN;
			socketWrite(s->fd, &syn, sizeof(syn), 0, &s->peer_listen_saddr);
			++s->reliable.m_synsent_times;
			s->reliable.m_synsent_msec = timestamp_msec + s->reliable.rto;
			update_timestamp(&loop->m_checkexpire_msec, s->reliable.m_synsent_msec);
		}
	}
	else if (FIN_WAIT_1_STATUS == s->reliable.m_status || LAST_ACK_STATUS == s->reliable.m_status) {
		if (s->reliable.m_fin_msec > timestamp_msec) {
			update_timestamp(&loop->m_checkexpire_msec, s->reliable.m_fin_msec);
		}
		else if (s->reliable.m_fin_times >= s->reliable.resend_maxtimes) {
			s->m_lastactive_msec = timestamp_msec;
			s->timeout_msec = MSL + MSL;
			update_timestamp(&loop->m_checkexpire_msec, s->m_lastactive_msec + s->timeout_msec);
		}
		else {
			unsigned char fin = HDR_FIN;
			socketWrite(s->fd, &fin, sizeof(fin), 0, &s->reliable.peer_saddr);
			++s->reliable.m_fin_times;
			s->reliable.m_fin_msec = timestamp_msec + s->reliable.rto;
			update_timestamp(&loop->m_checkexpire_msec, s->reliable.m_fin_msec);
		}
	}
	else if (ESTABLISHED_STATUS == s->reliable.m_status || CLOSE_WAIT_STATUS == s->reliable.m_status) {
		ListNode_t* cur;
		for (cur = s->m_sendpacketlist.head; cur; cur = cur->next) {
			ReliableDataPacket_t* packet = pod_container_of(cur, ReliableDataPacket_t, msg.m_listnode);
			if (packet->seq < s->reliable.m_cwndseq ||
				packet->seq - s->reliable.m_cwndseq >= s->reliable.cwndsize)
			{
				break;
			}
			if (packet->resend_timestamp_msec > timestamp_msec) {
				update_timestamp(&loop->m_checkexpire_msec, packet->resend_timestamp_msec);
				continue;
			}
			if (packet->resendtimes >= s->reliable.resend_maxtimes) {
				s->m_lastactive_msec = timestamp_msec;
				s->timeout_msec = MSL + MSL;
				update_timestamp(&loop->m_checkexpire_msec, s->m_lastactive_msec + s->timeout_msec);
				break;
			}
			socketWrite(s->fd, packet->data, packet->len, 0, &packet->saddr);
			packet->resendtimes++;
			packet->resend_timestamp_msec = timestamp_msec + s->reliable.rto;
			update_timestamp(&loop->m_checkexpire_msec, packet->resend_timestamp_msec);
		}
	}
}

static void reactor_socket_do_read(NioSocket_t* s) {
	if (SOCK_STREAM == s->socktype) {
		struct sockaddr_storage saddr;
		if (s->accept_callback) {
			FD_t connfd;
			for (connfd = reactorAcceptFirst(s->fd, s->m_readOl, &saddr);
				connfd != INVALID_FD_HANDLE;
				connfd = reactorAcceptNext(s->fd, &saddr))
			{
				s->accept_callback(s, connfd, &saddr);
			}
		}
		else {
			int res = socketTcpReadableBytes(s->fd);
			do {
				unsigned char *p;
				size_t offset = 0;

				if (res <= 0) {
					s->valid = 0;
					break;
				}

				p = (unsigned char*)realloc(s->m_inbuf, s->m_inbuflen + res);
				if (!p) {
					free(s->m_inbuf);
					s->m_inbuf = NULL;
					s->m_inbuflen = 0;
					s->valid = 0;
					break;
				}
				s->m_inbuf = p;
				res = socketRead(s->fd, s->m_inbuf + s->m_inbuflen, res, 0, &saddr);
				if (res <= 0) {
					free(s->m_inbuf);
					s->m_inbuf = NULL;
					s->m_inbuflen = 0;
					s->valid = 0;
					break;
				}
				else {
					s->m_inbuflen += res;
				}

				while (offset < s->m_inbuflen) {
					NioMsg_t* msgptr = NULL;
					int len = s->decode_packet(s, s->m_inbuf + offset, s->m_inbuflen - offset, &saddr, &msgptr);
					if (0 == len)
						break;
					if (len < 0) {
						s->valid = 0;
						offset = s->m_inbuflen;
						break;
					}
					offset += len;
					if (msgptr) {
						msgptr->type = NIO_SOCKET_USER_MESSAGE;
						dataqueuePush(s->m_loop->m_msgdq, &msgptr->m_listnode);
					}
				}

				if (offset) {
					if (offset >= s->m_inbuflen) {
						free(s->m_inbuf);
						s->m_inbuf = NULL;
						s->m_inbuflen = 0;
					}
					else {
						size_t n = offset, start;
						for (start = 0; offset < s->m_inbuflen; ++start, ++offset) {
							s->m_inbuf[start] = s->m_inbuf[offset];
						}
						s->m_inbuflen -= n;
					}
				}
			} while (0);
		}
	}
	else if (SOCK_DGRAM == s->socktype) {
		struct sockaddr_storage saddr;
		unsigned char buffer[0xffff], *p_data;
		unsigned int readtimes, readmaxtimes = s->m_recvpacket_maxcnt;
		for (readtimes = 0; readtimes < readmaxtimes; ++readtimes) {
			int res;
			if (0 == readtimes) {
				Iobuf_t iov;
				reactorEventOverlappedData(s->m_readOl, &iov, &saddr);
				res = iobufLen(&iov);
				if (res <= 0) {
					++readmaxtimes;
					continue;
				}
				p_data = (unsigned char*)iobufPtr(&iov);
			}
			else {
				p_data = buffer;
				res = socketRead(s->fd, buffer, sizeof(buffer), 0, &saddr);
			}

			if (res < 0) {
				if (errnoGet() != EWOULDBLOCK)
					s->valid = 0;
				break;
			}
			else if (s->reliable.m_status) {
				if (0 == res)
					continue;
				if (!reactor_socket_reliable_read(s, p_data, res, &saddr))
					break;
			}
			else if (0 == res) {
				NioMsg_t* msgptr = NULL;
				if (s->decode_packet(s, NULL, 0, &saddr, &msgptr) < 0) {
					s->valid = 0;
					break;
				}
				if (msgptr) {
					msgptr->type = NIO_SOCKET_USER_MESSAGE;
					dataqueuePush(s->m_loop->m_msgdq, &msgptr->m_listnode);
				}
			}
			else {
				int offset = 0, len = -1;
				while (offset < res) {
					NioMsg_t* msgptr = NULL;
					len = s->decode_packet(s, p_data + offset, res - offset, &saddr, &msgptr);
					if (len < 0) {
						s->valid = 0;
						break;
					}
					if (0 == len)
						break;
					offset += len;
					if (msgptr) {
						msgptr->type = NIO_SOCKET_USER_MESSAGE;
						dataqueuePush(s->m_loop->m_msgdq, &msgptr->m_listnode);
					}
				}
				if (len < 0)
					break;
			}
		}
	}
}

static int reactorsocket_write(NioSocket_t* s) {
	struct sockaddr_storage saddr;
	if (!s->m_writeOl) {
		s->m_writeOl = reactorMallocOverlapped(REACTOR_WRITE, NULL, 0, 0);
		if (!s->m_writeOl) {
			s->valid = 0;
			return 0;
		}
	}
	if (reactorCommit(&s->m_loop->m_reactor, s->fd, REACTOR_WRITE, s->m_writeOl, &saddr))
		return 1;
	s->valid = 0;
	return 0;
}

static void reactor_socket_do_write(NioSocket_t* s) {
	if (SOCK_STREAM != s->socktype)
		return;
	if (s->connect_callback) {
		int errnum = reactorConnectCheckSuccess(s->fd) ? 0 : errnoGet();
		if (errnum) {
			s->valid = 0;
		}
		else if (!reactorsocket_read(s)) {
			errnum = errnoGet();
			s->valid = 0;
		}
		s->connect_callback(s, errnum);
		if (s->valid)
			s->connect_callback = NULL;
		return;
	}
	if (!s->valid)
		return;

	dataqueuePush(&s->m_loop->m_sender->m_dq, &s->m_sendmsg.m_listnode);
}

NioSocket_t* niosocketSend(NioSocket_t* s, const void* data, unsigned int len, const struct sockaddr_storage* saddr) {
	if (!s->valid || s->m_shutdown)
		return NULL;
	if (!data || !len) {
		if (SOCK_STREAM == s->socktype)
			return s;
		len = 0;
	}
	if (ESTABLISHED_STATUS == s->reliable.m_status) {
		ReliableDataPacket_t* packet = (ReliableDataPacket_t*)malloc(sizeof(ReliableDataPacket_t) + RELIABLE_HDR_LEN + len);
		if (!packet)
			return NULL;
		packet->msg.type = NIO_SOCKET_RELIABLE_MESSAGE;
		packet->saddr = s->reliable.peer_saddr;
		packet->s = s;
		packet->resendtimes = 0;
		packet->len = RELIABLE_HDR_LEN + len;
		memcpy(packet->data + RELIABLE_HDR_LEN, data, len);
		packet->data[0] = HDR_DATA;
		nioloop_exec_msg(s->m_loop, &packet->msg.m_listnode);
	}
	else {
		Packet_t* packet = (Packet_t*)malloc(sizeof(Packet_t) + len);
		if (!packet)
			return NULL;
		packet->msg.type = NIO_SOCKET_USER_MESSAGE;
		if (saddr && SOCK_STREAM != s->socktype)
			packet->saddr = *saddr;
		else
			packet->saddr.ss_family = AF_UNSPEC;
		packet->s = s;
		packet->offset = 0;
		packet->len = len;
		memcpy(packet->data, data, len);
		dataqueuePush(&s->m_loop->m_sender->m_dq, &packet->msg.m_listnode);
	}
	return s;
}

NioSocket_t* niosocketSendv(NioSocket_t* s, Iobuf_t iov[], unsigned int iovcnt, const struct sockaddr_storage* saddr) {
	unsigned int i, nbytes;
	if (!s->valid || s->m_shutdown)
		return NULL;
	if (!iov || !iovcnt) {
		if (SOCK_STREAM == s->socktype)
			return s;
		iovcnt = 0;
		nbytes = 0;
	}
	else {
		for (nbytes = 0, i = 0; i < iovcnt; ++i)
			nbytes += iobufLen(iov + i);
		if (0 == nbytes) {
			if (SOCK_STREAM == s->socktype)
				return s;
			iovcnt = 0;
		}
	}
	if (ESTABLISHED_STATUS == s->reliable.m_status) {
		ReliableDataPacket_t* packet = (ReliableDataPacket_t*)malloc(sizeof(ReliableDataPacket_t) + RELIABLE_HDR_LEN + nbytes);
		if (!packet)
			return NULL;
		packet->msg.type = NIO_SOCKET_RELIABLE_MESSAGE;
		packet->saddr = s->reliable.peer_saddr;
		packet->resendtimes = 0;
		packet->s = s;
		packet->len = RELIABLE_HDR_LEN + nbytes;
		for (nbytes = RELIABLE_HDR_LEN, i = 0; i < iovcnt; ++i) {
			memcpy(packet->data + nbytes, iobufPtr(iov + i), iobufLen(iov + i));
			nbytes += iobufLen(iov + i);
		}
		packet->data[0] = HDR_DATA;
		nioloop_exec_msg(s->m_loop, &packet->msg.m_listnode);
	}
	else {
		Packet_t* packet = (Packet_t*)malloc(sizeof(Packet_t) + nbytes);
		if (!packet)
			return NULL;
		packet->msg.type = NIO_SOCKET_USER_MESSAGE;
		if (saddr && SOCK_STREAM != s->socktype)
			packet->saddr = *saddr;
		else
			packet->saddr.ss_family = AF_UNSPEC;
		packet->s = s;
		packet->offset = 0;
		packet->len = nbytes;
		for (nbytes = 0, i = 0; i < iovcnt; ++i) {
			memcpy(packet->data + nbytes, iobufPtr(iov + i), iobufLen(iov + i));
			nbytes += iobufLen(iov + i);
		}
		dataqueuePush(&s->m_loop->m_sender->m_dq, &packet->msg.m_listnode);
	}
	return s;
}

void niosocketShutdown(NioSocket_t* s) {
	if (SOCK_STREAM == s->socktype && s->accept_callback) {
		s->valid = 0;
		s->m_shutdown = 1;
		if (INFTIM == s->timeout_msec)
			s->timeout_msec = 5000;
	}
	else if (0 == _cmpxchg16(&s->m_shutdown, 1, 0)) {
		if (s->reliable.m_status)
			nioloop_exec_msg(s->m_loop, &s->m_shutdownmsg.m_listnode);
		else
			dataqueuePush(&s->m_loop->m_sender->m_dq, &s->m_shutdownmsg.m_listnode);
	}
}

NioSocket_t* niosocketCreate(FD_t fd, int domain, int socktype, int protocol, NioSocket_t*(*pmalloc)(void), void(*pfree)(NioSocket_t*)) {
	NioSocket_t* s = pmalloc();
	if (!s)
		return NULL;
	if (INVALID_FD_HANDLE == fd) {
		fd = socket(domain, socktype, protocol);
		if (INVALID_FD_HANDLE == fd) {
			if (pfree)
				pfree(s);
			return NULL;
		}
	}
	if (!socketNonBlock(fd, TRUE)) {
		if (pfree)
			pfree(s);
		return NULL;
	}
	s->fd = fd;
	s->domain = domain;
	s->socktype = socktype;
	s->protocol = protocol;
	s->timeout_msec = INFTIM;
	s->userdata = NULL;
	s->local_listen_saddr.ss_family = AF_UNSPEC;
	s->peer_listen_saddr.ss_family = AF_UNSPEC;
	s->accept_callback = NULL;
	s->connect_callback = NULL;
	s->reg_callback = NULL;
	s->decode_packet = NULL;
	s->close = NULL;
	s->valid = 1;
	s->m_shutdown = 0;
	s->m_shutwr = 0;
	s->m_regmsg.type = NIO_SOCKET_REG_MESSAGE;
	s->m_shutdownmsg.type = NIO_SOCKET_SHUTDOWN_MESSAGE;
	s->m_closemsg.type = NIO_SOCKET_CLOSE_MESSAGE;
	s->m_sendmsg.type = NIO_SOCKET_STREAM_WRITEABLE_MESSAGE;
	s->m_hashnode.key = &s->fd;
	s->m_loop = NULL;
	s->m_free = pfree;
	s->m_readOl = NULL;
	s->m_writeOl = NULL;
	s->m_lastactive_msec = 0;
	s->m_inbuf = NULL;
	s->m_inbuflen = 0;
	s->m_recvpacket_maxcnt = 8;
	listInit(&s->m_recvpacketlist);
	listInit(&s->m_sendpacketlist);

	s->reliable.peer_saddr.ss_family = AF_UNSPEC;
	s->reliable.rto = 4;
	s->reliable.resend_maxtimes = 5;
	s->reliable.cwndsize = 10;
	s->reliable.enable = 0;
	s->reliable.m_status = IDLE_STATUS;
	s->reliable.m_synsent_times = 0;
	s->reliable.m_fin_times = 0;
	s->reliable.m_synsent_msec = 0;
	s->reliable.m_fin_msec = 0;
	s->reliable.m_cwndseq = 0;
	s->reliable.m_recvseq = 0;
	s->reliable.m_sendseq = 0;
	return s;
}

void niosocketFree(NioSocket_t* s) {
	ListNode_t *cur, *next;
	if (!s)
		return;

	socketClose(s->fd);
	reactorFreeOverlapped(s->m_readOl);
	reactorFreeOverlapped(s->m_writeOl);

	if (s->m_inbuf) {
		free(s->m_inbuf);
		s->m_inbuf = NULL;
		s->m_inbuflen = 0;
	}

	if (s->reliable.enable) {
		for (cur = s->m_recvpacketlist.head; cur; cur = next) {
			next = cur->next;
			if (s->accept_callback) {
				ReliableHalfConnect_t* halfcon = pod_container_of(cur, ReliableHalfConnect_t, m_listnode);
				socketClose(halfcon->sockfd);
				free(halfcon);
			}
			else {
				free(pod_container_of(cur, ReliableDataPacket_t, msg.m_listnode));
			}
		}
	}
	for (cur = s->m_sendpacketlist.head; cur; cur = next) {
		next = cur->next;
		free(cur);
	}

	if (s->m_free)
		s->m_free(s);
}

static int sockht_keycmp(const struct HashtableNode_t* node, const void* key) {
	return pod_container_of(node, NioSocket_t, m_hashnode)->fd != *(FD_t*)key;
}

static unsigned int sockht_keyhash(const void* key) { return *(FD_t*)key; }

static List_t sockht_expire(NioLoop_t* loop, long long timestamp_msec) {
	List_t expirelist;
	HashtableNode_t *cur, *next;
	listInit(&expirelist);
	for (cur = hashtableFirstNode(&loop->m_sockht); cur; cur = next) {
		NioSocket_t* s;
		next = hashtableNextNode(cur);
		s = pod_container_of(cur, NioSocket_t, m_hashnode);
		if (s->valid) {
			int timeout_msec = s->timeout_msec;
			if (timeout_msec <= 0 || s->m_lastactive_msec + timeout_msec > timestamp_msec) {
				if (timeout_msec > 0)
					update_timestamp(&loop->m_checkexpire_msec, s->m_lastactive_msec + timeout_msec);
				reactor_socket_reliable_update(loop, s, timestamp_msec);
				continue;
			}
			s->valid = 0;
		}
		hashtableRemoveNode(&loop->m_sockht, cur);
		listInsertNodeBack(&expirelist, expirelist.tail, &s->m_closemsg.m_listnode);
	}
	return expirelist;
}

int nioloopHandler(NioLoop_t* loop, NioEv_t e[], int n, long long timestamp_msec, int wait_msec) {
	ListNode_t *cur, *next;
	if (loop->m_checkexpire_msec > timestamp_msec) {
		int checkexpire_wait_msec = loop->m_checkexpire_msec - timestamp_msec;
		if (checkexpire_wait_msec < wait_msec || wait_msec < 0)
			wait_msec = checkexpire_wait_msec;
	}
	else if (loop->m_checkexpire_msec) {
		wait_msec = 0;
	}

	n = reactorWait(&loop->m_reactor, e, n, wait_msec);
	if (n < 0) {
		return n;
	}
	else if (n > 0) {
		int i;
		for (i = 0; i < n; ++i) {
			HashtableNode_t* find_node;
			NioSocket_t* s;
			int event;
			void* ol;
			FD_t fd = reactorEventFD(e + i);
			if (fd == loop->m_socketpair[0]) {
				struct sockaddr_storage saddr;
				char c[512];
				socketRead(fd, c, sizeof(c), 0, NULL);
				reactorCommit(&loop->m_reactor, fd, REACTOR_READ, loop->m_readOl, &saddr);
				_xchg16(&loop->m_wake, 0);
				continue;
			}
			find_node = hashtableSearchKey(&loop->m_sockht, &fd);
			if (!find_node)
				continue;
			s = pod_container_of(find_node, NioSocket_t, m_hashnode);
			s->m_lastactive_msec = timestamp_msec;

			reactorEventOpcodeAndOverlapped(e + i, &event, &ol);
			switch (event) {
				case REACTOR_READ:
					reactor_socket_do_read(s);
					reactorsocket_read(s);
					break;
				case REACTOR_WRITE:
					reactor_socket_do_write(s);
					break;
				default:
					s->valid = 0;
			}
			if (s->valid)
				continue;

			hashtableRemoveNode(&loop->m_sockht, &s->m_hashnode);
			if (s->accept_callback || s->connect_callback)
				niosocketFree(s);
			else
				dataqueuePush(loop->m_msgdq, &s->m_closemsg.m_listnode);
		}
	}

	criticalsectionEnter(&loop->m_msglistlock);
	cur = loop->m_msglist.head;
	listInit(&loop->m_msglist);
	criticalsectionLeave(&loop->m_msglistlock);

	for (; cur; cur = next) {
		NioMsg_t* message;
		next = cur->next;
		message = pod_container_of(cur, NioMsg_t, m_listnode);
		if (NIO_SOCKET_CLOSE_MESSAGE == message->type) {
			NioSocket_t* s = pod_container_of(message, NioSocket_t, m_closemsg);
			niosocketFree(s);
		}
		else if (NIO_SOCKET_SHUTDOWN_MESSAGE == message->type) {
			NioSocket_t* s = pod_container_of(message, NioSocket_t, m_shutdownmsg);
			s->m_shutwr = 1;
			if (ESTABLISHED_STATUS == s->reliable.m_status || CLOSE_WAIT_STATUS == s->reliable.m_status) {
				if (!s->m_sendpacketlist.head) {
					send_fin_packet(loop, s, timestamp_msec);
				}
			}
		}
		else if (NIO_SOCKET_RELIABLE_MESSAGE == message->type) {
			ReliableDataPacket_t* packet = pod_container_of(message, ReliableDataPacket_t, msg);
			NioSocket_t* s = packet->s;
			if (!s->valid || s->m_shutwr) {
				free(packet);
				continue;
			}
			*(unsigned int*)(packet->data + 1) = htonl(s->reliable.m_sendseq);
			packet->seq = s->reliable.m_sendseq++;
			if (packet->seq >= s->reliable.m_cwndseq &&
				packet->seq - s->reliable.m_cwndseq < s->reliable.cwndsize)
			{
				socketWrite(s->fd, packet->data, packet->len, 0, &packet->saddr);
				packet->resend_timestamp_msec = timestamp_msec + s->reliable.rto;
				update_timestamp(&loop->m_checkexpire_msec, packet->resend_timestamp_msec);
			}
			listInsertNodeBack(&s->m_sendpacketlist, s->m_sendpacketlist.tail, &packet->msg.m_listnode);
		}
		else if (NIO_SOCKET_REG_MESSAGE == message->type) {
			NioSocket_t* s = pod_container_of(message, NioSocket_t, m_regmsg);
			int reg_ok = 0;
			do {
				int timeout_msec;
				if (!reactorReg(&loop->m_reactor, s->fd))
					break;
				s->m_loop = loop;
				s->m_lastactive_msec = timestamp_msec;
				if (SOCK_STREAM == s->socktype) {
					if (s->connect_callback) {
						if (!s->m_writeOl) {
							s->m_writeOl = reactorMallocOverlapped(REACTOR_CONNECT, NULL, 0, 0);
							if (!s->m_writeOl)
								break;
						}
						if (!reactorCommit(&loop->m_reactor, s->fd, REACTOR_CONNECT, s->m_writeOl, &s->peer_listen_saddr))
							break;
					}
					else {
						if (s->accept_callback) {
							if (AF_UNSPEC == s->local_listen_saddr.ss_family) {
								if (!socketGetLocalAddr(s->fd, &s->local_listen_saddr))
									break;
							}
						}
						if (!reactorsocket_read(s))
							break;
					}
				}
				else {
					if (s->reliable.enable) {
						if (s->connect_callback) {
							unsigned char syn = HDR_SYN;
							if (socketWrite(s->fd, &syn, sizeof(syn), 0, &s->peer_listen_saddr) < 0)
								break;
							s->reliable.m_status = SYN_SENT_STATUS;
							s->reliable.peer_saddr = s->peer_listen_saddr;
							s->reliable.m_synsent_msec = timestamp_msec + s->reliable.rto;
							update_timestamp(&loop->m_checkexpire_msec, s->reliable.m_synsent_msec);
						}
						else if (s->accept_callback) {
							s->reliable.m_status = LISTENED_STATUS;
							if (s->m_recvpacket_maxcnt < 200)
								s->m_recvpacket_maxcnt = 200;
							if (AF_UNSPEC == s->local_listen_saddr.ss_family) {
								if (!socketGetLocalAddr(s->fd, &s->local_listen_saddr))
									break;
							}
						}
						else {
							s->reliable.m_status = ESTABLISHED_STATUS;
						}
					}
					if (!reactorsocket_read(s))
						break;
				}
				hashtableReplaceNode(hashtableInsertNode(&loop->m_sockht, &s->m_hashnode), &s->m_hashnode);
				reg_ok = 1;
				timeout_msec = s->timeout_msec;
				if (timeout_msec > 0) {
					update_timestamp(&loop->m_checkexpire_msec, s->m_lastactive_msec + timeout_msec);
				}
			} while (0);
			if (reg_ok) {
				if (!s->connect_callback && s->reg_callback)
					s->reg_callback(s, 0);
			}
			else {
				if (s->connect_callback)
					s->connect_callback(s, errnoGet());
				else if (s->reg_callback)
					s->reg_callback(s, errnoGet());
				niosocketFree(s);
			}
		}
	}
	timestamp_msec = gmtimeMillisecond();
	if (loop->m_checkexpire_msec && timestamp_msec >= loop->m_checkexpire_msec) {
		List_t expirelist;
		loop->m_checkexpire_msec = 0;
		expirelist = sockht_expire(loop, timestamp_msec);
		dataqueuePushList(loop->m_msgdq, &expirelist);
	}
	return n;
}

NioLoop_t* nioloopCreate(NioLoop_t* loop, DataQueue_t* msgdq, NioSender_t* sender) {
	struct sockaddr_storage saddr;
	loop->initok = 0;

	if (!socketPair(SOCK_STREAM, loop->m_socketpair))
		return NULL;

	loop->m_readOl = reactorMallocOverlapped(REACTOR_READ, NULL, 0, 0);
	if (!loop->m_readOl) {
		socketClose(loop->m_socketpair[0]);
		socketClose(loop->m_socketpair[1]);
		return NULL;
	}

	if (!reactorCreate(&loop->m_reactor)) {
		reactorFreeOverlapped(loop->m_readOl);
		socketClose(loop->m_socketpair[0]);
		socketClose(loop->m_socketpair[1]);
		return NULL;
	}

	if (!socketNonBlock(loop->m_socketpair[0], TRUE) ||
		!socketNonBlock(loop->m_socketpair[1], TRUE) ||
		!reactorReg(&loop->m_reactor, loop->m_socketpair[0]) ||
		!reactorCommit(&loop->m_reactor, loop->m_socketpair[0], REACTOR_READ, loop->m_readOl, &saddr))
	{
		reactorFreeOverlapped(loop->m_readOl);
		socketClose(loop->m_socketpair[0]);
		socketClose(loop->m_socketpair[1]);
		reactorClose(&loop->m_reactor);
		return NULL;
	}

	if (!criticalsectionCreate(&loop->m_msglistlock)) {
		reactorFreeOverlapped(loop->m_readOl);
		socketClose(loop->m_socketpair[0]);
		socketClose(loop->m_socketpair[1]);
		reactorClose(&loop->m_reactor);
		return NULL;
	}

	loop->m_msgdq = msgdq;
	loop->m_sender = sender;
	listInit(&loop->m_msglist);
	hashtableInit(&loop->m_sockht,
		loop->m_sockht_bulks, sizeof(loop->m_sockht_bulks) / sizeof(loop->m_sockht_bulks[0]),
		sockht_keycmp, sockht_keyhash);
	loop->initok = 1;
	loop->m_wake = 0;
	loop->m_checkexpire_msec = 0;
	return loop;
}

NioLoop_t* nioloopWake(NioLoop_t* loop) {
	if (0 == _cmpxchg16(&loop->m_wake, 1, 0)) {
		char c;
		socketWrite(loop->m_socketpair[1], &c, sizeof(c), 0, NULL);
	}
	return loop;
}

void nioloopReg(NioLoop_t* loop, NioSocket_t* s[], size_t n) {
	size_t i;
	List_t list;
	listInit(&list);
	for (i = 0; i < n; ++i) {
		listInsertNodeBack(&list, list.tail, &s[i]->m_regmsg.m_listnode);
	}
	nioloop_exec_msglist(loop, &list);
}

void nioloopDestroy(NioLoop_t* loop) {
	if (loop && loop->initok) {
		reactorFreeOverlapped(loop->m_readOl);
		socketClose(loop->m_socketpair[0]);
		socketClose(loop->m_socketpair[1]);
		reactorClose(&loop->m_reactor);
		criticalsectionClose(&loop->m_msglistlock);
		do {
			ListNode_t* cur, *next;
			for (cur = loop->m_msglist.head; cur; cur = next) {
				NioMsg_t* msgbase = pod_container_of(cur, NioMsg_t, m_listnode);
				next = cur->next;
				if (NIO_SOCKET_RELIABLE_MESSAGE == msgbase->type)
					free(cur);
			}
		} while (0);
		do {
			HashtableNode_t *cur, *next;
			for (cur = hashtableFirstNode(&loop->m_sockht); cur; cur = next) {
				next = hashtableNextNode(cur);
				niosocketFree(pod_container_of(cur, NioSocket_t, m_hashnode));
			}
		} while (0);
	}
}

static int niosocket_send(NioSocket_t* s, Packet_t* packet) {
	int res = 0, is_empty = !s->m_sendpacketlist.head;
	if (SOCK_STREAM != s->socktype || is_empty) {
		struct sockaddr_storage* saddrptr = (packet->saddr.ss_family != AF_UNSPEC ? &packet->saddr : NULL);
		res = socketWrite(s->fd, packet->data, packet->len, 0, saddrptr);
		if (res < 0) {
			if (errnoGet() != EWOULDBLOCK) {
				s->valid = 0;
				free(packet);
				return 0;
			}
			res = 0;
		}
		else if (res >= packet->len) {
			if (NIO_SOCKET_USER_MESSAGE == packet->msg.type)
				free(packet);
			return 0;
		}
	}
	if (SOCK_STREAM == s->socktype) {
		packet->offset = res;
		listInsertNodeBack(&s->m_sendpacketlist, s->m_sendpacketlist.tail, &packet->msg.m_listnode);
		if (is_empty)
			reactorsocket_write(s);
	}
	return 1;
}

NioSender_t* niosenderCreate(NioSender_t* sender) {
	sender->initok = 0;
	if (!dataqueueInit(&sender->m_dq))
		return NULL;
	sender->m_resend_msec = 0;
	sender->initok = 1;
	return sender;
}

void niosenderHandler(NioSender_t* sender, long long timestamp_msec, int wait_msec) {
	ListNode_t *cur, *next;
	if (sender->m_resend_msec > timestamp_msec) {
		int resend_wait_msec = sender->m_resend_msec - timestamp_msec;
		if (resend_wait_msec < wait_msec || wait_msec < 0)
			wait_msec = resend_wait_msec;
	}
	else if (sender->m_resend_msec) {
		wait_msec = 0;
	}
	for (cur = dataqueuePop(&sender->m_dq, wait_msec, ~0); cur; cur = next) {
		NioMsg_t* msgbase = pod_container_of(cur, NioMsg_t, m_listnode);
		next = cur->next;
		if (NIO_SOCKET_SHUTDOWN_MESSAGE == msgbase->type) {
			NioSocket_t* s = pod_container_of(msgbase, NioSocket_t, m_shutdownmsg);
			s->m_shutwr = 1;
			if (SOCK_STREAM == s->socktype) {
				socketShutdown(s->fd, SHUT_WR);
			}
		}
		else if (NIO_SOCKET_CLOSE_MESSAGE == msgbase->type) {
			NioSocket_t* s = pod_container_of(msgbase, NioSocket_t, m_closemsg);
			nioloop_exec_msg(s->m_loop, cur);
		}
		else if (NIO_SOCKET_USER_MESSAGE == msgbase->type) {
			Packet_t* packet = pod_container_of(msgbase, Packet_t, msg);
			NioSocket_t* s = packet->s;
			if (!s->valid || s->m_shutwr)
				free(packet);
			else
				niosocket_send(packet->s, packet);
		}
		else if (NIO_SOCKET_STREAM_WRITEABLE_MESSAGE == msgbase->type) {
			NioSocket_t* s = pod_container_of(msgbase, NioSocket_t, m_sendmsg);
			ListNode_t* cur, *next;
			if (!s->valid || s->m_shutwr)
				continue;
			for (cur = s->m_sendpacketlist.head; cur; cur = next) {
				int res;
				Packet_t* packet = pod_container_of(cur, Packet_t, msg.m_listnode);
				next = cur->next;
				res = socketWrite(s->fd, packet->data + packet->offset, packet->len - packet->offset, 0, NULL);
				if (res < 0) {
					if (errnoGet() != EWOULDBLOCK) {
						s->valid = 0;
						break;
					}
					res = 0;
				}
				packet->offset += res;
				if (packet->offset >= packet->len) {
					listRemoveNode(&s->m_sendpacketlist, cur);
					if (NIO_SOCKET_USER_MESSAGE == packet->msg.type)
						free(packet);
					continue;
				}
				else if (s->valid)
					reactorsocket_write(s);
				break;
			}
		}
	}
	timestamp_msec = gmtimeMillisecond();
}

void niosenderDestroy(NioSender_t* sender) {
	if (sender && sender->initok) {
		ListNode_t *cur, *next;
		for (cur = dataqueuePop(&sender->m_dq, 0, ~0); cur; cur = next) {
			NioMsg_t* msgbase = pod_container_of(cur, NioMsg_t, m_listnode);
			next = cur->next;
			if (NIO_SOCKET_USER_MESSAGE == msgbase->type)
				free(pod_container_of(msgbase, Packet_t, msg));
		}
		dataqueueDestroy(&sender->m_dq, NULL);
	}
}

void niomsgHandler(DataQueue_t* dq, int max_wait_msec, void (*user_msg_callback)(NioMsg_t*, void*), void* arg) {
	ListNode_t* cur, *next;
	for (cur = dataqueuePop(dq, max_wait_msec, ~0); cur; cur = next) {
		NioMsg_t* message = pod_container_of(cur, NioMsg_t, m_listnode);
		next = cur->next;
		if (NIO_SOCKET_SHUTDOWN_MESSAGE == message->type) {
			NioSocket_t* s = pod_container_of(message, NioSocket_t, m_shutdownmsg);
			if (s->close) {
				s->close(s);
				s->close = NULL;
			}
		}
		else if (NIO_SOCKET_CLOSE_MESSAGE == message->type) {
			NioSocket_t* s = pod_container_of(message, NioSocket_t, m_closemsg);
			if (s->close) {
				s->close(s);
				s->close = NULL;
			}
			if (s->reliable.m_status)
				nioloop_exec_msg(s->m_loop, cur);
			else
				dataqueuePush(&s->m_loop->m_sender->m_dq, cur);
		}
		else if (NIO_SOCKET_USER_MESSAGE == message->type) {
			user_msg_callback(message, arg);
		}
	}
}

void niomsgClean(DataQueue_t* dq, void(*deleter)(NioMsg_t*)) {
	ListNode_t *cur = dataqueuePop(dq, 0, ~0);
	if (deleter) {
		ListNode_t *next;
		for (; cur; cur = next) {
			NioMsg_t* message = pod_container_of(cur, NioMsg_t, m_listnode);
			next = cur->next;
			if (NIO_SOCKET_USER_MESSAGE == message->type)
				deleter(message);
		}
	}
}

#ifdef __cplusplus
}
#endif
