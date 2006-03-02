/*
 * This file is part of libtrace
 *
 * Copyright (c) 2004 The University of Waikato, Hamilton, New Zealand.
 * Authors: Daniel Lawson
 *          Perry Lorier
 *	    Shane Alcock
 *
 * All rights reserved.
 *
 * This code has been developed by the University of Waikato WAND
 * research group. For further information please see http://www.wand.net.nz/
 *
 * libtrace is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * libtrace is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with libtrace; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * $Id$
 *
 */

#define _GNU_SOURCE

#include "config.h"
#include "common.h"
#include "libtrace.h"
#include "libtrace_int.h"
#include "format_helper.h"
#include "parse_cmd.h"
#include "rt_protocol.h"

#ifdef HAVE_INTTYPES_H
#  include <inttypes.h>
#else
#  error "Can't find inttypes.h - this needs to be fixed"
#endif

#ifdef HAVE_STDDEF_H
#  include <stddef.h>
#else
# error "Can't find stddef.h - do you define ptrdiff_t elsewhere?"
#endif
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <netdb.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define RT_INFO ((struct rt_format_data_t*)libtrace->format_data)

int reliability = 0;

char *rt_deny_reason(uint8_t reason) {
	char *string = 0;

	switch(reason) {
		case RT_DENY_WRAPPER:
			string = "Rejected by TCP Wrappers";
			break;
		case RT_DENY_FULL:
			string = "Max connections reached on server";
			break;
		case RT_DENY_AUTH:
			string = "Authentication failed";
			break;
		default:
			string = "Unknown reason";
	}

	return string;
}


struct rt_format_data_t {
	char *hostname;
	int port;
	int input_fd;
	int reliable;

	struct libtrace_t *dummy_erf;
	struct libtrace_t *dummy_pcap;
	struct libtrace_t *dummy_wag;
};

static struct libtrace_format_t rt;

static int rt_connect(struct libtrace_t *libtrace) {
        struct hostent *he;
        struct sockaddr_in remote;
	rt_header_t connect_msg;
	rt_deny_conn_t deny_hdr;	
	rt_hello_t hello_opts;
	uint8_t reason;
	
	if ((he=gethostbyname(RT_INFO->hostname)) == NULL) {
                perror("gethostbyname");
                return -1;
        }
        if ((RT_INFO->input_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
                perror("socket");
                return -1;
        }

        remote.sin_family = AF_INET;
        remote.sin_port = htons(RT_INFO->port);
        remote.sin_addr = *((struct in_addr *)he->h_addr);
        bzero(&(remote.sin_zero), 8);

        if (connect(RT_INFO->input_fd, (struct sockaddr *)&remote,
                                sizeof(struct sockaddr)) == -1) {
                perror("connect (inet)");
                return -1;
        }
	
	/* We are connected, now receive message from server */
	
	if (recv(RT_INFO->input_fd, &connect_msg, sizeof(rt_header_t), 0) != sizeof(rt_header_t) ) {
		printf("An error occured while connecting to %s\n", RT_INFO->hostname);
		return -1;
	}

	switch (connect_msg.type) {
		case RT_DENY_CONN:
			
			if (recv(RT_INFO->input_fd, &deny_hdr, 
						sizeof(rt_deny_conn_t),
						0) != sizeof(rt_deny_conn_t)) {
				reason = 0;
			}	
			reason = deny_hdr.reason;
			printf("Connection attempt is denied by the server: %s\n",
					rt_deny_reason(reason));
			return -1;
		case RT_HELLO:
			/* do something with options */
			printf("Hello\n");	
			if (recv(RT_INFO->input_fd, &hello_opts, 
						sizeof(rt_hello_t), 0)
					!= sizeof(rt_hello_t)) {
				printf("Failed to read hello options\n");
				return 0;
			}
			reliability = hello_opts.reliable;
			
			return 0;
		default:
			printf("Unexpected message type: %d\n", connect_msg.type);
			return -1;
	}
	
        return -1;
}


static int rt_init_input(struct libtrace_t *libtrace) {
        char *scan;
        char *uridata = libtrace->uridata;
        libtrace->format_data = malloc(sizeof(struct rt_format_data_t));

	RT_INFO->dummy_erf = NULL;
	RT_INFO->dummy_pcap = NULL;
	RT_INFO->dummy_wag = NULL;
	
        if (strlen(uridata) == 0) {
                RT_INFO->hostname =
                        strdup("localhost");
                RT_INFO->port =
                        COLLECTOR_PORT;
        } else {
                if ((scan = strchr(uridata,':')) == NULL) {
                        RT_INFO->hostname =
                                strdup(uridata);
                        RT_INFO->port =
                                COLLECTOR_PORT;
                } else {
                        RT_INFO->hostname =
                                (char *)strndup(uridata,
                                                (scan - uridata));
                        RT_INFO->port =
                                atoi(++scan);
                }
        }

	return rt_connect(libtrace);
}
	
static int rt_start_input(struct libtrace_t *libtrace) {
	rt_header_t start_msg;

	start_msg.type = RT_START;
	start_msg.length = sizeof(rt_start_t);

	printf("Sending start - len: %d\n", start_msg.length);
	
	/* Need to send start message to server */
	if (send(RT_INFO->input_fd, &start_msg, sizeof(rt_header_t) +
				start_msg.length, 0) != sizeof(rt_header_t)) {
		printf("Failed to send start message to server\n");
		return -1;
	}

	return 0;
}

static int rt_fin_input(struct libtrace_t *libtrace) {
        rt_header_t close_msg;

	close_msg.type = RT_CLOSE;
	close_msg.length = sizeof(rt_close_t);
	
	/* Send a close message to the server */
	if (send(RT_INFO->input_fd, &close_msg, sizeof(rt_header_t) + 
				close_msg.length, 0) != sizeof(rt_header_t)) {
		printf("Failed to send close message to server\n");
	
	}
	if (RT_INFO->dummy_erf) 
		trace_destroy_dead(RT_INFO->dummy_erf);
		
	if (RT_INFO->dummy_pcap)
		trace_destroy_dead(RT_INFO->dummy_pcap);

	if (RT_INFO->dummy_wag)
		trace_destroy_dead(RT_INFO->dummy_wag);
	close(RT_INFO->input_fd);
	free(libtrace->format_data);
        return 0;
}

static int rt_read(struct libtrace_t *libtrace, void *buffer, size_t len) {
        int numbytes;

        while(1) {
#ifndef MSG_NOSIGNAL
#  define MSG_NOSIGNAL 0
#endif
                if ((numbytes = recv(RT_INFO->input_fd,
                                                buffer,
                                                len,
                                                MSG_NOSIGNAL)) == -1) {
                        if (errno == EINTR) {
                                /* ignore EINTR in case
                                 * a caller is using signals
				 */
                                continue;
                        }
                        perror("recv");
                        return -1;
                }
                break;

        }
        return numbytes;
}


static int rt_set_format(libtrace_t *libtrace, libtrace_packet_t *packet) 
{
	switch (packet->type) {
		case RT_DATA_ERF:
			if (!RT_INFO->dummy_erf) {
				RT_INFO->dummy_erf = trace_create_dead("erf:-");
			}
			packet->trace = RT_INFO->dummy_erf;
			break;
		case RT_DATA_PCAP:
			if (!RT_INFO->dummy_pcap) {
				RT_INFO->dummy_pcap = trace_create_dead("pcap:-");
			}
			packet->trace = RT_INFO->dummy_pcap;
			break;
		case RT_DATA_WAG:
			if (!RT_INFO->dummy_wag) {
				RT_INFO->dummy_wag = trace_create_dead("wtf:-");
			}
			packet->trace = RT_INFO->dummy_wag;
			break;
		case RT_DATA_LEGACY_ETH:
		case RT_DATA_LEGACY_ATM:
		case RT_DATA_LEGACY_POS:
			printf("Sending legacy over RT is currently not supported\n");
			return -1;
		default:
			printf("Unrecognised format: %d\n", packet->type);
			return -1;
	}
	return 0; /* success */
}		

static void rt_set_payload(struct libtrace_packet_t *packet) {
	dag_record_t *erfptr;
	
	switch (packet->type) {
		case RT_DATA_ERF:
			erfptr = (dag_record_t *)packet->header;
			
			if (erfptr->flags.rxerror == 1) {
				packet->payload = NULL;
				break;
			}
			/* else drop into the default case */
		default:
			packet->payload = (char *)packet->buffer +
				trace_get_framing_length(packet);
			break;
	}
}

static int rt_send_ack(struct libtrace_t *libtrace, 
		uint32_t seqno)  {
	
	static char *ack_buffer = 0;
	char *buf_ptr;
	int numbytes = 0;
	int to_write = 0;
	rt_header_t *hdr;
	rt_ack_t *ack_hdr;
	
	if (!ack_buffer) {
		ack_buffer = malloc(sizeof(rt_header_t) + sizeof(rt_ack_t));
	}
	
	hdr = (rt_header_t *) ack_buffer;
	ack_hdr = (rt_ack_t *) (ack_buffer + sizeof(rt_header_t));
	
	hdr->type = RT_ACK;
	hdr->length = sizeof(rt_ack_t);

	ack_hdr->sequence = seqno;
	
	to_write = hdr->length + sizeof(rt_header_t);
	buf_ptr = ack_buffer;

	
	while (to_write > 0) {
		numbytes = send(RT_INFO->input_fd, buf_ptr, to_write, 0); 
		if (numbytes == -1) {
			if (errno == EINTR || errno == EAGAIN) {
				continue;
			}
			else {
				printf("Error sending ack\n");
				return -1;
			}
		}
		to_write = to_write - numbytes;
		buf_ptr = buf_ptr + to_write;
		
	}

	return 1;
}
	
static int rt_read_packet(libtrace_t *libtrace, libtrace_packet_t *packet) {
        
	rt_header_t pkt_hdr;
	int pkt_size = 0;
	
        void *buffer = 0;

        if (packet->buf_control == TRACE_CTRL_EXTERNAL || !packet->buffer) {
                packet->buf_control = TRACE_CTRL_PACKET;
                packet->buffer = malloc(LIBTRACE_PACKET_BUFSIZE);
        } 

        buffer = packet->buffer;
        packet->header = packet->buffer;

	/* FIXME: Better error handling required */
	if (rt_read(libtrace, &pkt_hdr, sizeof(rt_header_t)) !=
			sizeof(rt_header_t)) {
		printf("Error receiving rt header\n");
		return -1;
	}

	packet->type = pkt_hdr.type;
	pkt_size = pkt_hdr.length;

	switch(packet->type) {
		case RT_DATA_ERF:
		case RT_DATA_PCAP:
		case RT_DATA_WAG:
		case RT_DATA_LEGACY_ETH:
		case RT_DATA_LEGACY_POS:
		case RT_DATA_LEGACY_ATM:
			if (rt_read(libtrace, buffer, pkt_size) != pkt_size) {
				printf("Error receiving packet\n");
				return -1;
			}
			
			if (rt_set_format(libtrace, packet) < 0) {
                        	return -1;
                        }
                       	rt_set_payload(packet);

                        if (reliability > 0) {
	                        
				if (rt_send_ack(libtrace, pkt_hdr.sequence) 
						== -1)
				{
                                	return -1;
                                }
			}
			break;
		case RT_STATUS:
		case RT_DUCK:
			if (rt_read(libtrace, buffer, pkt_size) !=
					pkt_size) {
				printf("Error receiving status packet\n");
				return -1;
			}
			packet->header = 0;
			packet->payload = buffer;
			break;
		case RT_END_DATA:
			return 0;
		case RT_PAUSE_ACK:
			/* FIXME: Do something useful */
			break;
		case RT_OPTION:
			/* FIXME: Do something useful here as well */
			break;
		default:
			printf("Bad rt type for client receipt: %d\n",
					pkt_hdr.type);
	}
	return trace_get_capture_length(packet)+trace_get_framing_length(packet);
}

static int rt_get_capture_length(const struct libtrace_packet_t *packet) {
	switch (packet->type) {
		case RT_DUCK:
			return sizeof(rt_duck_t);
		case RT_STATUS:
			return sizeof(rt_status_t);
		case RT_HELLO:
			return sizeof(rt_hello_t);
		case RT_START:
			return sizeof(rt_start_t);
		case RT_ACK:
			return sizeof(rt_ack_t);
		case RT_END_DATA:
			return sizeof(rt_end_data_t);
		case RT_CLOSE:
			return sizeof(rt_close_t);
		case RT_DENY_CONN:
			return sizeof(rt_deny_conn_t);
		case RT_PAUSE:
			return sizeof(rt_pause_t);
		case RT_PAUSE_ACK:
			return sizeof(rt_pause_ack_t);
		case RT_OPTION:
			return sizeof(rt_option_t);
	}
	printf("Unknown type: %d\n", packet->type);
	return 0;
}
			
static int rt_get_framing_length(const libtrace_packet_t *packet) {
	return 0;
}


static int rt_get_fd(const libtrace_t *trace) {
        return ((struct rt_format_data_t *)trace->format_data)->input_fd;
}



static void rt_help() {
        printf("rt format module\n");
        printf("Supported input URIs:\n");
        printf("\trt:hostname:port\n");
        printf("\trt:hostname (connects on default port)\n");
        printf("\n");
        printf("\te.g.: rt:localhost\n");
        printf("\te.g.: rt:localhost:32500\n");
        printf("\n");

}


static struct libtrace_format_t rt = {
        "rt",
        "$Id$",
        TRACE_FORMAT_RT,
        rt_init_input,            	/* init_input */
        NULL,                           /* config_input */
        rt_start_input,           	/* start_input */
        NULL,                           /* init_output */
        NULL,                           /* config_output */
        NULL,                           /* start_output */
	NULL,				/* pause_output */
        rt_fin_input,             	/* fin_input */
        NULL,                           /* fin_output */
        rt_read_packet,           	/* read_packet */
        NULL,                           /* write_packet */
        NULL,		                /* get_link_type */
        NULL,  		            	/* get_direction */
        NULL,              		/* set_direction */
        NULL,          			/* get_erf_timestamp */
        NULL,                           /* get_timeval */
        NULL,                           /* get_seconds */
	NULL,				/* seek_erf */
	NULL,				/* seek_timeval */
	NULL,				/* seek_seconds */
        rt_get_capture_length,        	/* get_capture_length */
        NULL,            		/* get_wire_length */
        rt_get_framing_length, 		/* get_framing_length */
        NULL,         			/* set_capture_length */
        rt_get_fd,                	/* get_fd */
        trace_event_device,             /* trace_event */
        rt_help                   /* help */
};

void __attribute__((constructor)) rt_constructor() {
	register_format(&rt);
}
