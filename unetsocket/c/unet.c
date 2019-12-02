#define _BSD_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "fjage.h"
#include "unet.h"
#include <sys/time.h>
#include <errno.h>

typedef struct
{
    fjage_gw_t gw;
    pthread_t tid;
    pthread_mutex_t rxlock, txlock;
    int local_protocol;
    int remote_address;
    int remote_protocol;
    long timeout;
    fjage_aid_t provider;
    bool quit;
} _unetsocket_t;

static long long _time_in_ms(void) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (((long long)tv.tv_sec)*1000)+(tv.tv_usec/1000);
}

static void *monitor(void *p)
{
	_unetsocket_t *usock = p;
	long deadline = -1;
	int rv;
	if (usock->timeout == 0) {
		deadline = 0;
	}
	else if (usock->timeout > 0) {
		deadline = _time_in_ms() + usock->timeout;
	}
	const char *list[] = {"org.arl.unet.DatagramNtf", "org.arl.unet.phy.RxFrameNtf"};
	while (!usock->quit) {
		int time_remaining = -1;
		if (usock->timeout == 0) {
			time_remaining = 0;
		}
		else if (usock->timeout > 0) {
			time_remaining = deadline - _time_in_ms();
			if (time_remaining <= 0) {
				usock->quit = true;
			}
		}
		pthread_mutex_lock(&usock->rxlock);
		fjage_msg_t msg = fjage_receive_any(usock->gw, list, 2, time_remaining);
		pthread_mutex_unlock(&usock->rxlock);
		if (msg != NULL) {
			printf("%s\n", msg);
			rv = fjage_msg_get_int(msg, "protocol", 0);
			if (rv == DATA || rv >= USER) {
				if (usock->local_protocol < 0) return msg;
				if (usock->local_protocol == rv) return msg;
			}
			fjage_msg_destroy(msg);
		}
	}
	return NULL;
}

unetsocket_t unetsocket_open(const char* hostname, int port)
{
	_unetsocket_t *usock = malloc(sizeof(_unetsocket_t));
	if (usock == NULL) return NULL;
	usock->gw = fjage_tcp_open(hostname, port);
	usock->timeout = -1;
	if (usock->gw == NULL) {
		free(usock);
		return NULL;
	}
	int nagents = fjage_agents_for_service(usock->gw, "org.arl.unet.Services.DATAGRAM", NULL, 0);
	fjage_aid_t agents[nagents];
	if (fjage_agents_for_service(usock->gw, "org.arl.unet.Services.DATAGRAM", agents, nagents) < 0) {
		free(usock);
		return NULL;
	}
	for(int i = 0; i < nagents; i++) {
		fjage_subscribe_agent(usock->gw, agents[i]);
	}
    return usock;
}

unetsocket_t unetsocket_rs232_open(const char* devname, int baud, const char* settings)
{
	_unetsocket_t *usock = malloc(sizeof(_unetsocket_t));
	if (usock == NULL) return NULL;
	usock->gw = fjage_rs232_open(devname, baud, settings);
	if (usock->gw == NULL) {
		free(usock);
		return NULL;
	}
	int nagents = fjage_agents_for_service(usock->gw, "org.arl.unet.Services.DATAGRAM", NULL, 0);
	fjage_aid_t agents[nagents];
	if (fjage_agents_for_service(usock->gw, "org.arl.unet.Services.DATAGRAM", agents, nagents) < 0) {
		free(usock);
		return NULL;
	}
	for(int i = 0; i < nagents; i++) {
		fjage_subscribe_agent(usock->gw, agents[i]);
	}
    return usock;
}

int unetsocket_close(unetsocket_t sock)
{
	if (sock == NULL) return -1;
	_unetsocket_t *usock = sock;
	usock->quit = true;
	fjage_interrupt(usock->gw);
	fjage_close(usock->gw);
	free(usock);
	return 0;
}

int unetsocket_is_closed(unetsocket_t sock)
{
	if (sock == NULL) return -1;
	return 0;
}

int unetsocket_bind(unetsocket_t sock, int protocol)
{
	if (sock == NULL) return -1;
	_unetsocket_t *usock = sock;
	if (protocol == DATA || (protocol >= USER && protocol <= MAX)) {
		usock->local_protocol = protocol;
		return 0;
	}
	return -1;
}

void unetsocket_unbind(unetsocket_t sock)
{
	_unetsocket_t *usock = sock;
	usock->local_protocol = -1;
}

int unetsocket_is_bound(unetsocket_t sock)
{
	if (sock == NULL) return -1;
	_unetsocket_t *usock = sock;
	if (usock->local_protocol >= 0) return 0;
	return -1;
}

int unetsocket_connect(unetsocket_t sock, int to, int protocol)
{
	if (sock == NULL) return -1;
	_unetsocket_t *usock = sock;
	if ((to >= 0) && (protocol == DATA || (protocol >= USER && protocol <= MAX))) {
		usock->remote_address = to;
		usock->remote_protocol = protocol;
		return 0;
	}
	return -1;
}

void unetsocket_disconnect(unetsocket_t sock)
{
	_unetsocket_t *usock = sock;
	usock->remote_address = -1;
	usock->remote_protocol = 0;
}

int unetsocket_is_connected(unetsocket_t sock)
{
	if (sock == NULL) return -1;
	_unetsocket_t *usock = sock;
	if (usock->remote_address >= 0) return 0;
	return -1;
}

int unetsocket_get_local_address(unetsocket_t sock)
{
	if (sock == NULL) return -1;
	_unetsocket_t *usock = sock;
	fjage_msg_t msg;
	fjage_aid_t node;
	int rv;
	node = fjage_agent_for_service(usock->gw, "org.arl.unet.Services.NODE_INFO");
	if (node == NULL) return -1;
	msg = fjage_msg_create("org.arl.unet.ParameterReq", FJAGE_REQUEST);
	fjage_msg_set_recipient(msg, node);
	fjage_msg_add_int(msg, "index", -1);
	fjage_msg_add_string(msg, "param", "address");
	msg = fjage_request(usock->gw, msg, 5*TIMEOUT);
	if (msg != NULL && fjage_msg_get_performative(msg) == FJAGE_INFORM) {
		rv = fjage_msg_get_int(msg, "value", 0);
		free(msg);
		free(node);
		return rv;
	}
	free(msg);
	free(node);
	return -1;
}

int unetsocket_get_local_protocol(unetsocket_t sock)
{
	if (sock == NULL) return -1;
	_unetsocket_t *usock = sock;
	return usock->local_protocol;
}

int unetsocket_get_remote_address(unetsocket_t sock)
{
	if (sock == NULL) return -1;
	_unetsocket_t *usock = sock;
	return usock->remote_address;
}

int unetsocket_get_remote_protocol(unetsocket_t sock)
{
	if (sock == NULL) return -1;
	_unetsocket_t *usock = sock;
	return usock->remote_protocol;
}

void unetsocket_set_timeout(unetsocket_t sock, long ms)
{
	_unetsocket_t *usock = sock;
	if (ms < 0) {
		ms = -1;
	}
	usock->timeout = ms;
}

long unetsocket_get_timeout(unetsocket_t sock)
{
	if (sock == NULL) return -1;
	_unetsocket_t *usock = sock;
	return usock->timeout;
}

// NOTE: changed const uint8_t* to uint8_t*
int unetsocket_send(unetsocket_t sock, uint8_t* data, int len, int to, int protocol)
{
	if (sock == NULL) return -1;
	_unetsocket_t *usock = sock;
	fjage_msg_t msg;
	int rv;
	msg = fjage_msg_create("org.arl.unet.DatagramReq", FJAGE_REQUEST);
	if (data != NULL) fjage_msg_add_byte_array(msg, "data", data, len);
	fjage_msg_add_int(msg, "to", to);
	fjage_msg_add_int(msg, "protocol", protocol);
	rv = unetsocket_send_request(usock, msg);
	return rv;
}

int unetsocket_send_request(unetsocket_t sock, fjage_msg_t req)
{
	if (sock == NULL) return -1;
	_unetsocket_t *usock = sock;
	int protocol;
	const char* recipient;
	protocol = fjage_msg_get_int(req, "protocol", 0);
	if (protocol != DATA && (protocol < USER || protocol > MAX)) return -1;
	recipient = fjage_msg_get_string(req, "recipient");
	if (recipient == NULL) {
		if (usock->provider == NULL) usock->provider = fjage_agent_for_service(usock->gw, "org.arl.unet.Services.TRANSPORT");
		if (usock->provider == NULL) usock->provider = fjage_agent_for_service(usock->gw, "org.arl.unet.Services.ROUTING");
		if (usock->provider == NULL) usock->provider = fjage_agent_for_service(usock->gw, "org.arl.unet.Services.LINK");
		if (usock->provider == NULL) usock->provider = fjage_agent_for_service(usock->gw, "org.arl.unet.Services.PHYSICAL");
		if (usock->provider == NULL) usock->provider = fjage_agent_for_service(usock->gw, "org.arl.unet.Services.DATAGRAM");
		if (usock->provider == NULL) return -1;
		fjage_msg_set_recipient(req, usock->provider);
	}
	req = fjage_request(usock->gw, req, TIMEOUT);
	if (req != NULL && fjage_msg_get_performative(req) == FJAGE_AGREE) {
		free(req);
		return 0;
	}
	free(req);
    return -1;
}

fjage_msg_t unetsocket_receive(unetsocket_t sock)
{
	if (sock == NULL) return NULL;
	_unetsocket_t *usock = sock;
	pthread_mutex_init(&usock->rxlock, NULL);
    pthread_mutex_init(&usock->txlock, NULL);
    if (pthread_create(&usock->tid, NULL, monitor, usock) < 0) {
    	pthread_mutex_destroy(&usock->rxlock);
    	pthread_mutex_destroy(&usock->txlock);
    	fjage_close(usock->gw);
        free(usock);
        return NULL;
    }
    usock->quit = false;
    pthread_join(usock->tid, NULL);
    return NULL;
}

void unetsocket_cancel(unetsocket_t sock)
{
	_unetsocket_t *usock = sock;
	usock->quit = true;
}

fjage_gw_t unetsocket_get_gateway(unetsocket_t sock)
{
	if (sock == NULL) return NULL;
	_unetsocket_t *usock = sock;
	return usock->gw;
}

fjage_aid_t unetsocket_agent_for_service(unetsocket_t sock, const char* svc)
{
	if (sock == NULL) return NULL;
	_unetsocket_t *usock = sock;
	return fjage_agent_for_service(usock->gw, svc);
}

int unetsocket_agents_for_service(unetsocket_t sock, const char* svc, fjage_aid_t* agents, int max)
{
	if (sock == NULL) return -1;
	_unetsocket_t *usock = sock;
	return fjage_agents_for_service(usock->gw, svc, agents, max);
}

int unetsocket_host(unetsocket_t sock, const char* node_name)
{
	if (sock == NULL) return -1;
	_unetsocket_t *usock = sock;
	fjage_msg_t msg;
	fjage_aid_t arp;
	int rv;
	arp = fjage_agent_for_service(usock->gw, "org.arl.unet.Services.ADDRESS_RESOLUTION");
	if (arp == NULL) return -1;
	msg = fjage_msg_create("org.arl.unet.addr.AddressResolutionReq", FJAGE_REQUEST);
	fjage_msg_set_recipient(msg, arp);
	fjage_msg_add_int(msg, "index", -1);
	fjage_msg_add_string(msg, "param", "name");
	fjage_msg_add_string(msg, "value", node_name);
	msg = fjage_request(usock->gw, msg, 5*TIMEOUT);
	if (msg != NULL && fjage_msg_get_performative(msg) == FJAGE_INFORM) {
		rv = fjage_msg_get_int(msg, "address", 0);
		free(msg);
		free(arp);
		return rv;
	}
	return -1;
}
