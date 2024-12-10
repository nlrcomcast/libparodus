/**
 * Copyright 2016 Comcast Cable Communications Management, LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <nanomsg/nn.h>
#include <nanomsg/pipeline.h>
#include "libparodus.h"
#include "libparodus_private.h"
#include "libparodus_time.h"
#include "libparodus_test_timing.h"
#include <pthread.h>
#include "libparodus_queues.h"

//#define PARODUS_SERVICE_REQUIRES_REGISTRATION 1

#define PARODUS_SERVICE_URL "tcp://127.0.0.1:6666"
//#define PARODUS_URL "ipc:///tmp/parodus_server.ipc"

#define PARODUS_CLIENT_URL "tcp://127.0.0.1:6667"
//#define CLIENT_URL "ipc:///tmp/parodus_client.ipc"

#define DEFAULT_KEEPALIVE_TIMEOUT_SECS 65

#define URL_SIZE 32

void libpd_log1(int level, const char *msg, ...);

typedef struct {
	int run_state;
	const char *parodus_url;
	const char *client_url;
	int keep_alive_count;
	int reconnect_count;
	libpd_cfg_t cfg;
	bool connect_on_every_send; // always false, currently
	int rcv_sock;
	int stop_rcv_sock;
	int send_sock;
	char *wrp_queue_name;
	libpd_mq_t wrp_queue;
	extra_err_info_t rcv_err_info;
	pthread_t wrp_receiver_tid;
	pthread_mutex_t send_mutex;
	bool auth_received;
} __instance_t;

#define SOCK_SEND_TIMEOUT_MS 2000

#define MAX_RECONNECT_RETRY_DELAY_SECS 63

#define END_MSG "---END-PARODUS---\n"
static const char *end_msg = END_MSG;

static char *closed_msg = "---CLOSED---\n";

typedef struct {
	int len;
	char *msg;
} raw_msg_t;

#define WRP_QUEUE_SEND_TIMEOUT_MS	2000
#define WRP_QNAME_HDR "/LIBPD_WRP_QUEUE"
#define WRP_QUEUE_SIZE 50

const char *wrp_qname_hdr = WRP_QNAME_HDR;

int flush_wrp_queue (libpd_mq_t wrp_queue, uint32_t delay_ms, int *exterr);
static int wrp_sock_send (__instance_t *inst, wrp_msg_t *msg, extra_err_info_t *err_info);
static void *wrp_receiver_thread (void *arg);
static void libparodus_shutdown__ (__instance_t *inst, extra_err_info_t *err_info);

#define RUN_STATE_RUNNING		1234
#define RUN_STATE_DONE			-1234

typedef struct {
	libpd_error_t error_code;
	const char *error_msg;
} err_table_item_t;

err_table_item_t error_msg_table[] = {

		{ LIBPD_ERROR_INIT_INST,
			 "Error on libparodus init. Could not create new instance."},
		{ LIBPD_ERROR_INIT_CFG,
			 "Error on libparodus init. Invalid config parameter."},
		{ LIBPD_ERROR_INIT_CONNECT,
			 "Error on libparodus init. Could not connect."},
		{ LIBPD_ERROR_INIT_RCV_THREAD,
			 "Error on libparodus init. Could not create receiver thread."},
		{ LIBPD_ERROR_INIT_QUEUE,
			 "Error on libparodus init. Could not create receive queue."},
		{ LIBPD_ERROR_INIT_REGISTER,
			 "Error on libparodus init. Registration failed."},
		{ LIBPD_ERROR_RCV_NULL_INST,
			 "Error on libparodus receive. Null instance given."},
		{ LIBPD_ERROR_RCV_STATE,
			 "Error on libparodus receive. Run state error."},
		{ LIBPD_ERROR_RCV_CFG,
			 "Error on libparodus receive. Not configured for receive."},
		{ LIBPD_ERROR_RCV_RCV,
			 "Error on libparodus receive. Error receiveing from receive queue."},
		{ LIBPD_ERROR_RCV_THR_LIMIT,
			 "Error on libparodus receive. Thread limit exceeded."},
		{ LIBPD_ERROR_CLOSE_RCV_NULL_INST,
			 "Error on libparodus close receiver. Null instance given."},
		{ LIBPD_ERROR_CLOSE_RCV_STATE,
			 "Error on libparodus close receiver. Run state error."},
		{ LIBPD_ERROR_CLOSE_RCV_CFG,
			 "Error on libparodus close receiver. Not configured for receive."},
		{ LIBPD_ERROR_CLOSE_RCV_TIMEDOUT,
			 "Error on libparodus close receiver. Timded out waiting to enqueue close msg."},
		{ LIBPD_ERROR_CLOSE_RCV_SEND,
			 "Error on libparodus close receiver. Unable to enqueue close msg."},
		{ LIBPD_ERROR_CLOSE_RCV_THR_LIMIT,
			 "Error on libparodus close receiver. Thread limit exceeded."},
		{ LIBPD_ERROR_SEND_NULL_INST,
			 "Error on libparodus send. Null instance given."},
		{ LIBPD_ERROR_SEND_STATE,
			 "Error on libparodus send. Run state error."},
		{ LIBPD_ERROR_SEND_WRP_MSG,
			 "Error on libparodus send. Invalid WRP Message."},
		{ LIBPD_ERROR_SEND_SOCKET,
			 "Error on libparodus send. Socket send error."},
		{ LIBPD_ERROR_SEND_THR_LIMIT,
			 "Error on libparodus send. Thread limit exceeded."}
};


#define NUM_ERROR_MSGS ( (sizeof error_msg_table) / sizeof(err_table_item_t) )

char Service_Name[128]={0};
const char *libparodus_strerror (libpd_error_t err)
{
	unsigned i;
	
	if (0 == err)
	  return "libparodus success";
	  
	for (i=0; i<NUM_ERROR_MSGS; i++) {
		if (err == error_msg_table[i].error_code)
			return error_msg_table[i].error_msg;
	}
	return "Unknown libparodus error";
}


static void getParodusUrl(__instance_t *inst)
{
	inst->parodus_url = inst->cfg.parodus_url;
	inst->client_url = inst->cfg.client_url;
	if (NULL == inst->parodus_url)
		inst->parodus_url = PARODUS_SERVICE_URL;
	if (NULL == inst->client_url)
		inst->client_url = PARODUS_CLIENT_URL;
	// to test connect_on_every_send, start the parodus_url with "test:"
	// which will be stripped off the url.
	if (strncmp (inst->parodus_url, "test:", 5) == 0) {
		inst->connect_on_every_send = true;
		inst->parodus_url += 5;
	}
  libpd_log (LEVEL_INFO, ("LIBPARODUS: parodus url is  %s\n", inst->parodus_url));
  libpd_log (LEVEL_INFO, ("LIBPARODUS: client url is  %s\n", inst->client_url));
}

static __instance_t *make_new_instance (libpd_cfg_t *cfg)
{
	size_t qname_len;
	char *wrp_queue_name;
	__instance_t *inst = (__instance_t*) malloc (sizeof (__instance_t));
	if (NULL == inst)
		return NULL;
	qname_len = strlen(wrp_qname_hdr) + strlen(cfg->service_name) + 1;
	wrp_queue_name = (char*) malloc (qname_len+1);
	if (NULL == wrp_queue_name) {
		free (inst);
		return NULL;
	}
	memset ((void*) inst, 0, sizeof(__instance_t));
	inst->wrp_queue_name = wrp_queue_name;
	pthread_mutex_init (&inst->send_mutex, NULL);
	//inst->cfg = *cfg;
	memcpy (&inst->cfg, cfg, sizeof(libpd_cfg_t));
	getParodusUrl (inst);
	sprintf (inst->wrp_queue_name, "%s.%s", wrp_qname_hdr, cfg->service_name);
	return inst;
}

static void destroy_instance (libpd_instance_t *instance)
{
	__instance_t *inst;
	if (NULL != instance) {
		inst = (__instance_t *) *instance;
		if (NULL != inst) {
			if (NULL != inst->wrp_queue_name)
				free (inst->wrp_queue_name);
			pthread_mutex_destroy (&inst->send_mutex);
			free (inst);
			*instance = NULL;
		}
	}
}

bool is_auth_received (libpd_instance_t instance)
{
	__instance_t *inst = (__instance_t *) instance;
	return inst->auth_received;
}

#define SHUTDOWN_SOCKET(sock) \
	if ((sock) != -1) \
		nn_shutdown ((sock), 0); \
	(sock) = 0;

void shutdown_socket (int *sock)
{
	if (*sock >= 0) {
		nn_shutdown (*sock, 0);
		nn_close (*sock);
	}
	*sock = -1;
}

typedef enum {
	/** 
	 * @brief Error on connect_receiver
	 * null url given
	 */
	CONN_RCV_ERR_NULL_URL = -1,
	/** 
	 * @brief Error on connect_receiver
	 * error creating socket
	 */
	CONN_RCV_ERR_CREATE = -0x40,
	/** 
	 * @brief Error on connect_receiver
	 * error setting socket option
	 */
	CONN_RCV_ERR_SETOPT = -0x80,
	/** 
	 * @brief Error on connect_receiver
	 * error binding to socket
	 */
	CONN_RCV_ERR_BIND = -0xC0
} conn_rcv_error_t;

/**
 * Open receive socket and bind to it.
 */
int connect_receiver (const char *rcv_url, int keepalive_timeout_secs, int *oserr)
{
	int rcv_timeout;
	int sock;

	*oserr = 0;
	if (NULL == rcv_url) {
		return CONN_RCV_ERR_NULL_URL;
	}
  sock = nn_socket (AF_SP, NN_PULL);
	if (sock < 0) {
		*oserr = errno;
		libpd_log_err (LEVEL_ERROR, errno, ("Unable to create rcv socket %s\n", rcv_url));
 		return CONN_RCV_ERR_CREATE;
	}
	if (keepalive_timeout_secs > 0) { 
		rcv_timeout = keepalive_timeout_secs * 1000;
		if (nn_setsockopt (sock, NN_SOL_SOCKET, NN_RCVTIMEO, 
					&rcv_timeout, sizeof (rcv_timeout)) < 0) {
			*oserr = errno;
			libpd_log_err (LEVEL_ERROR, errno, ("Unable to set socket timeout: %s\n", rcv_url));
			shutdown_socket (&sock);
 			return CONN_RCV_ERR_SETOPT;
		}
	}
  if (nn_bind (sock, rcv_url) < 0) {
		*oserr = errno;
		libpd_log_err (LEVEL_ERROR, errno, ("Unable to bind to receive socket %s\n", rcv_url));
		shutdown_socket (&sock);
		return CONN_RCV_ERR_BIND;
	}
	return sock;
}

typedef enum {
	/** 
	 * @brief Error on connect_sender
	 * null url specified
	 */
	CONN_SEND_ERR_NULL = -0x01,
	/** 
	 * @brief Error on connect_sender
	 * error creating socket
	 */
	CONN_SEND_ERR_CREATE = -0x40,
	/** 
	 * @brief Error on connect_sender
	 * error setting socket option
	 */
	CONN_SEND_ERR_SETOPT = -0x80,
	/** 
	 * @brief Error on connect_sender
	 * error connecting to socket
	 */
	CONN_SEND_ERR_CONN = -0xC0
} conn_send_error_t;

/**
 * Open send socket and connect to it.
 */
int connect_sender (const char *send_url, int *oserr)
{
	int sock;
	int send_timeout = SOCK_SEND_TIMEOUT_MS;

	*oserr = 0;
	if (NULL == send_url) {
		return CONN_SEND_ERR_NULL;
	}
  sock = nn_socket (AF_SP, NN_PUSH);
	if (sock < 0) {
		*oserr = errno;
		libpd_log_err (LEVEL_ERROR, errno, ("Unable to create send socket: %s\n", send_url));
		libpd_log1 (LEVEL_ERROR, "Unable to create send socket: %s\n", send_url);
 		return CONN_SEND_ERR_CREATE;
	}
	if (nn_setsockopt (sock, NN_SOL_SOCKET, NN_SNDTIMEO, 
				&send_timeout, sizeof (send_timeout)) < 0) {
		*oserr = errno;
		libpd_log_err (LEVEL_ERROR, errno, ("Unable to set socket timeout: %s\n", send_url));
		libpd_log1 (LEVEL_ERROR, "Unable to set socket timeout: %s\n", send_url);
		shutdown_socket (&sock);
 		return CONN_SEND_ERR_SETOPT;
	}
  if (nn_connect (sock, send_url) < 0) {
		*oserr = errno;
		libpd_log_err (LEVEL_ERROR, errno, ("Unable to connect to send socket %s\n",
			send_url));
		libpd_log1 (LEVEL_ERROR, "Unable to connect to send socket %s\n",send_url);
		shutdown_socket (&sock);
		return CONN_SEND_ERR_CONN;
	}
	return sock;
}

static int create_thread (pthread_t *tid, void *(*thread_func) (void*),
	__instance_t *inst)
{
	int rtn = pthread_create (tid, NULL, thread_func, (void*) inst);
	if (rtn != 0) {
		libpd_log_err (LEVEL_ERROR, rtn, ("Unable to create thread\n"));
	}
	return rtn; 
}

static bool is_closed_msg (wrp_msg_t *msg)
{
	return (msg->msg_type == WRP_MSG_TYPE__REQ) &&
		(strcmp (msg->u.req.dest, closed_msg) == 0);
}

static void wrp_free (void *msg)
{
	wrp_msg_t *wrp_msg;
	if (NULL == msg)
		return;
	wrp_msg = (wrp_msg_t *) msg;
	if (is_closed_msg (wrp_msg))
		free (wrp_msg);
	else
		wrp_free_struct (wrp_msg);
}

typedef enum {
	/** 
	 * @brief Error on sock_send
	 * not all bytes sent
	 */
	SOCK_SEND_ERR_BYTE_CNT = -0x01,
	/** 
	 * @brief Error on sock_send
	 * nn_send error
	 */
	SOCK_SEND_ERR_NN = -0x40
} sock_send_error_t;

typedef enum {
	/** 
	 * @brief Error on wrp_sock_send
	 * convert to struct error
	 */
	WRP_SEND_ERR_CONVERT = -0x01,
	/** 
	 * @brief Error on wrp_sock_send
	 * connect sender error
	 * only applies if connect_on_every_send
	 */
	WRP_SEND_ERR_CONNECT = -0x200,
	/** 
	 * @brief Error on wrp_sock_send
	 * socket send error
	 */
	WRP_SEND_ERR_SOCK_SEND = -0x800,
	/** 
	 * @brief Error on sock_send
	 * not all bytes sent
	 */
	WRP_SEND_ERR_BYTE_CNT = -0x801,
	/** 
	 * @brief Error on sock_send
	 * nn_send error
	 */
	WRP_SEND_ERR_NN = -0x840
} wrp_sock_send_error_t;

static int send_registration_msg (__instance_t *inst, extra_err_info_t *err)
{
	wrp_msg_t reg_msg;
	reg_msg.msg_type = WRP_MSG_TYPE__SVC_REGISTRATION;
	reg_msg.u.reg.service_name = (char *) inst->cfg.service_name;
	reg_msg.u.reg.url = (char *) inst->client_url;
	return wrp_sock_send (inst, &reg_msg, err);
}

static bool show_options (libpd_cfg_t *cfg)
{
	libpd_log (LEVEL_DEBUG, 
		("LIBPARODUS Options: Rcv: %d, KA Timeout: %d\n",
		cfg->receive, cfg->keepalive_timeout_secs));
	return cfg->receive;
}

// define ABORT FLAGS
#define ABORT_RCV_SOCK	1
#define ABORT_QUEUE			2
#define ABORT_SEND_SOCK	4
#define ABORT_STOP_RCV_SOCK	8


static void abort_init (__instance_t *inst, unsigned opt)
{
	if (opt & ABORT_RCV_SOCK)
		shutdown_socket (&inst->rcv_sock);
	if (opt & ABORT_QUEUE)
		libpd_qdestroy (&inst->wrp_queue, &wrp_free);
	if (opt & ABORT_SEND_SOCK)
		shutdown_socket(&inst->send_sock);
	if (opt & ABORT_STOP_RCV_SOCK)
			shutdown_socket(&inst->stop_rcv_sock);
}

int libparodus_init_dbg (libpd_instance_t *instance, libpd_cfg_t *libpd_cfg,
    extra_err_info_t *err_info)
{
	bool need_to_send_registration;
	int err;
	int oserr = 0;
	__instance_t *inst = make_new_instance (libpd_cfg);
#define SETERR(oserr_,err_) \
	err_info->err_detail = err_; \
	err_info->oserr = oserr_; \
	// errno = oserr
#define CONNECT_ERR(oserr) \
	(oserr == EINVAL) ? LIBPD_ERROR_INIT_CFG : LIBPD_ERROR_INIT_CONNECT

	// err_list->num_threads will now be 1
	if (NULL == inst) {
		libpd_log (LEVEL_ERROR, ("LIBPARODUS: unable to allocate new instance\n"));
		libpd_log1 (LEVEL_ERROR, "LIBPARODUS: unable to allocate new instance\n");
		SETERR (0, LIBPD_ERR_INIT_INST);
		return LIBPD_ERROR_INIT_INST;
	}
	*instance = (libpd_instance_t) inst;

	if (inst->cfg.test_flags & CFG_TEST_CONNECT_ON_EVERY_SEND)
		inst->connect_on_every_send = true;

	show_options (libpd_cfg);
	if (inst->cfg.receive) {
		libpd_log (LEVEL_INFO, ("LIBPARODUS: connecting receiver to %s\n",  inst->client_url));
		libpd_log1 (LEVEL_INFO, "LIBPARODUS: connecting receiver to %s\n",  inst->client_url);
		err = connect_receiver (inst->client_url, inst->cfg.keepalive_timeout_secs, &oserr);
		if (err < 0) {
			SETERR(oserr, LIBPD_ERR_INIT_RCV + err); 
			return CONNECT_ERR (oserr);
		}
		inst->rcv_sock = err;
	}
	if (!inst->connect_on_every_send) {
		//libpd_log (LEVEL_INFO, ("LIBPARODUS: connecting sender to %s\n", inst->parodus_url));
		err = connect_sender (inst->parodus_url, &oserr);
		if (err < 0) {
			abort_init (inst, ABORT_RCV_SOCK);
			SETERR (oserr, LIBPD_ERR_INIT_SEND + err); 
			return CONNECT_ERR (oserr);
		}
		inst->send_sock = err;
		libpd_log (LEVEL_INFO, ("LIBPARODUS: connected sender to %s (%d)\n", 
			inst->parodus_url, inst->send_sock));
		libpd_log1 (LEVEL_INFO, "LIBPARODUS: connected sender to %s (%d)\n", 
			inst->parodus_url, inst->send_sock);			
	}
	if (inst->cfg.receive) {
		// We use the stop_rcv_sock to send a stop msg to our own receive socket.
		err = connect_sender (inst->client_url, &oserr);
		if (err < 0) {
			abort_init (inst, ABORT_RCV_SOCK | ABORT_SEND_SOCK);
			SETERR (oserr, LIBPD_ERR_INIT_TERMSOCK + err); 
			return CONNECT_ERR (oserr);
		}
		inst->stop_rcv_sock = err;
		libpd_log (LEVEL_INFO, ("LIBPARODUS: Opened sockets\n"));
		libpd_log1 (LEVEL_INFO, "LIBPARODUS: Opened sockets\n");		
		err = libpd_qcreate (&inst->wrp_queue, inst->wrp_queue_name, WRP_QUEUE_SIZE, &oserr);
		if (err != 0) {
			abort_init (inst, ABORT_RCV_SOCK | ABORT_SEND_SOCK | ABORT_STOP_RCV_SOCK);
			SETERR (oserr, LIBPD_ERR_INIT_QUEUE + err); 
			return LIBPD_ERROR_INIT_QUEUE;
		}
		libpd_log (LEVEL_INFO, ("LIBPARODUS: Created queues\n"));
		libpd_log1 (LEVEL_INFO, "LIBPARODUS: Created queues\n");
		err = create_thread (&inst->wrp_receiver_tid, wrp_receiver_thread,
				inst);
		if (err != 0) {
			abort_init (inst, ABORT_RCV_SOCK | ABORT_QUEUE | ABORT_SEND_SOCK | ABORT_STOP_RCV_SOCK); 
			SETERR (err, LIBPD_ERR_INIT_RCV_THREAD_PCR);
			return LIBPD_ERROR_INIT_RCV_THREAD;
		}
	}


#ifdef TEST_SOCKET_TIMING
	sst_init_totals ();
#endif

	inst->run_state = RUN_STATE_RUNNING;

#ifdef PARODUS_SERVICE_REQUIRES_REGISTRATION
	need_to_send_registration = true;
#else
	need_to_send_registration = inst->cfg.receive;
#endif

	if (!inst->cfg.receive) {
		libpd_log (LEVEL_DEBUG, ("LIBPARODUS: Init without receiver\n"));
		libpd_log1 (LEVEL_DEBUG, "LIBPARODUS: Init without receiver\n");
	}

	if (need_to_send_registration) {
		libpd_log (LEVEL_INFO, ("LIBPARODUS: sending registration msg\n"));
		err = send_registration_msg (inst, err_info);
		if (err != 0) {
			libpd_log (LEVEL_ERROR, ("LIBPARODUS: error sending registration msg\n"));
			oserr = err_info->oserr;
			libparodus_shutdown__ (inst, err_info);
			// put back err_info from send_registration_msg
			err_info->err_detail = LIBPD_ERR_INIT_REGISTER + err;
			err_info->oserr = oserr;
			return LIBPD_ERROR_INIT_REGISTER;
		}
		libpd_log (LEVEL_DEBUG, ("LIBPARODUS: Sent registration message\n"));
		libpd_log1 (LEVEL_DEBUG, "LIBPARODUS: Sent registration message\n");
	}
	SETERR (0, 0);
	strncpy(Service_Name,libpd_cfg->service_name,128);
	libpd_log1(LEVEL_INFO,"%s",Service_Name);
	return 0;
}

int libparodus_init (libpd_instance_t *instance, libpd_cfg_t *libpd_cfg)
{
  extra_err_info_t err;
  return libparodus_init_dbg (instance, libpd_cfg, &err);
}

// When msg_len is given as -1, then msg is a null terminated string
static int sock_send (int sock, const char *msg, int msg_len, int *oserr)
{
  int bytes;
	*oserr = 0;
	if (msg_len < 0)
		msg_len = strlen (msg) + 1; // include terminating null
	bytes = nn_send (sock, msg, msg_len, 0);
	libpd_log1(LEVEL_INFO,"nn_send: msg_len=%d no.of bytes sent=%d",msg_len,bytes);
  if (bytes < 0) {
		*oserr = errno; 
		libpd_log_err (LEVEL_ERROR, errno, ("Error sending msg\n"));
		libpd_log1(LEVEL_ERROR,"Error sending msg");
		return -0x40;
	}
  if (bytes != msg_len) {
		libpd_log (LEVEL_ERROR, ("Not all bytes sent, just %d\n", bytes));
		libpd_log1(LEVEL_ERROR,"Not all bytes sent, just %d", bytes);
		return -1;
	}
	return 0;
}

// returns 0 OK, 1 timedout, -1 error
static int sock_receive (int rcv_sock, raw_msg_t *msg, int *oserr)
{
	char *buf = NULL;
  msg->len = nn_recv (rcv_sock, &buf, NN_MSG, 0);

	*oserr = 0;
  if (msg->len < 0) {
		libpd_log_err (LEVEL_ERROR, errno, ("Error receiving msg\n"));
		if (errno == ETIMEDOUT)
			return 1;
		*oserr = errno; 
		return -1;
	}
	msg->msg = buf;
	return 0;
}

static void libparodus_shutdown__ (__instance_t *inst, extra_err_info_t *err_info)
{
	int rtn;
#ifdef TEST_SOCKET_TIMING
	sst_display_totals ();
#endif

	inst->run_state = RUN_STATE_DONE;
	libpd_log (LEVEL_INFO, ("LIBPARODUS: Shutting Down\n"));
	if (inst->cfg.receive) {
		sock_send (inst->stop_rcv_sock, end_msg, -1, &err_info->oserr);
	 	rtn = pthread_join (inst->wrp_receiver_tid, NULL);
		if (rtn != 0) {
			libpd_log_err (LEVEL_ERROR, rtn, ("Error terminating wrp receiver thread\n"));
		}
		shutdown_socket(&inst->rcv_sock);
		libpd_log (LEVEL_INFO, ("LIBPARODUS: Flushing wrp queue\n"));
		flush_wrp_queue (inst->wrp_queue, 5, &err_info->oserr);
		libpd_qdestroy (&inst->wrp_queue, &wrp_free);
	}
	libpd_log (LEVEL_DEBUG, ("LIBPARODUS: Shut down send sock %d\n", inst->send_sock));
	shutdown_socket(&inst->send_sock);
	if (inst->cfg.receive) {
		shutdown_socket(&inst->stop_rcv_sock);
	}
	inst->run_state = 0;
	inst->auth_received = false;
}

int libparodus_shutdown_dbg (libpd_instance_t *instance,
    extra_err_info_t *err_info)
{
	__instance_t *inst;

	err_info->err_detail = 0;
	err_info->oserr = 0;
	if (NULL == instance)
		return 0;
	inst = (__instance_t *) *instance;
	if (NULL == inst)
		return 0;

	if (RUN_STATE_RUNNING != inst->run_state) {
		libpd_log (LEVEL_DEBUG, ("LIBPARODUS: not running at shutdown\n"));
		libpd_log1 (LEVEL_DEBUG, "LIBPARODUS: not running at shutdown\n");
		err_info->err_detail = LIBPD_ERR_SHUTDOWN_STATE;
		destroy_instance (instance);
		return 0;
	}

	libparodus_shutdown__ (inst, err_info);
	destroy_instance (instance);
	return 0;
}

int libparodus_shutdown (libpd_instance_t *instance)
{
  extra_err_info_t err;
  libpd_log1 (LEVEL_DEBUG, "LIBPARODUS: shutdown\n");
  return libparodus_shutdown_dbg (instance, &err);
}


// returns 0 OK
//  1 timed out
static int timed_wrp_queue_receive (libpd_mq_t wrp_queue,	wrp_msg_t **msg, 
	unsigned timeout_ms, int *oserr)
{
	int rtn;
	void *raw_msg;

	rtn = libpd_qreceive (wrp_queue, &raw_msg, timeout_ms, oserr);
	if (rtn == 1) // timed out
		return 1;
	if (rtn != 0) {
		libpd_log (LEVEL_ERROR, ("Unable to receive on queue /WRP_QUEUE\n"));
		return rtn;
	}
	*msg = (wrp_msg_t *) raw_msg;
	libpd_log (LEVEL_DEBUG, ("LIBPARODUS: receive msg on WRP QUEUE\n"));
	return 0;
}

static wrp_msg_t *make_closed_msg (void)
{
	wrp_msg_t *msg = (wrp_msg_t *) malloc (sizeof (wrp_msg_t));
	if (NULL == msg)
		return NULL;
	msg->msg_type = WRP_MSG_TYPE__REQ;
	msg->u.req.transaction_uuid = closed_msg;
	msg->u.req.source = closed_msg;
	msg->u.req.dest = closed_msg;
	msg->u.req.payload = (void*) closed_msg;
	msg->u.req.payload_size = strlen(closed_msg);
	return msg;
}

// returns 0 OK
//  2 closed msg received
//  1 timed out
//  LIBPD_ERR_RCV_ ... on error
int libparodus_receive__ (libpd_mq_t wrp_queue, wrp_msg_t **msg, 
	uint32_t ms, int *oserr)
{
	int err;
	wrp_msg_t *msg__;

	err = timed_wrp_queue_receive (wrp_queue, msg, ms, oserr);
	if (err == 1) // timed out
		return 1;
	if (err != 0)
		return LIBPD_ERR_RCV_QUEUE + err;
	msg__ = *msg;
	if (msg__ == NULL) {
		libpd_log (LEVEL_DEBUG, ("LIBPARODOS: NULL msg from wrp queue\n"));
		return LIBPD_ERR_RCV_NULL_MSG;
	}
	libpd_log (LEVEL_DEBUG, ("LIBPARODUS: received msg type %d\n", msg__->msg_type));
	if (is_closed_msg (msg__)) {
		wrp_free (msg__);
		libpd_log (LEVEL_INFO, ("LIBPARODUS: closed msg received\n"));
		return 2;
	}
	return 0;
}

// returns 0 OK
//  2 closed msg received
//  1 timed out
// LIBPD_ERR_RCV_ ... on error
int libparodus_receive_dbg (libpd_instance_t instance, wrp_msg_t **msg, 
    uint32_t ms, extra_err_info_t *err_info)
{
	int rtn;
	__instance_t *inst = (__instance_t *) instance;

	err_info->err_detail = 0;
	err_info->oserr = 0;
	if (NULL == inst) {
		libpd_log (LEVEL_ERROR, ("Null instance on libparodus_receive\n"));
		err_info->err_detail = LIBPD_ERR_RCV_NULL_INST;
		return LIBPD_ERROR_RCV_NULL_INST;
	}

	if (!inst->cfg.receive) {
		libpd_log (LEVEL_ERROR, ("No receive option on libparodus_receive\n"));
		err_info->err_detail = LIBPD_ERR_RCV_CFG;
		return LIBPD_ERROR_RCV_CFG;
	}
	if (RUN_STATE_RUNNING != inst->run_state) {
		libpd_log (LEVEL_ERROR, ("LIBPARODUS: not running at receive\n"));
		err_info->err_detail = LIBPD_ERR_RCV_STATE;
		return LIBPD_ERROR_RCV_STATE;
	}
	rtn = libparodus_receive__ (inst->wrp_queue, msg, ms, &err_info->oserr);
	if (rtn >= 0)
		return rtn;
	err_info->err_detail = rtn;
	// errno = inst->exterr;
	return LIBPD_ERROR_RCV_RCV;
}

int libparodus_receive (libpd_instance_t instance, wrp_msg_t **msg, uint32_t ms)
{
  extra_err_info_t err;
  return libparodus_receive_dbg (instance, msg, ms, &err);
}

int libparodus_close_receiver__ (libpd_mq_t wrp_queue, int *oserr)
{
	wrp_msg_t *closed_msg_ptr =	make_closed_msg ();
	int rtn = libpd_qsend (wrp_queue, (void *) closed_msg_ptr, 
				WRP_QUEUE_SEND_TIMEOUT_MS, oserr);
	if (rtn == 1) // timed out
		return 1;
	if (rtn != 0) {
		return LIBPD_ERR_CLOSE_RCV + rtn;
	}
	libpd_log (LEVEL_INFO, ("LIBPARODUS: Sent closed msg\n"));
	return 0;
}

int libparodus_close_receiver_dbg (libpd_instance_t instance,
    extra_err_info_t *err_info)
{
	int rtn;
	__instance_t *inst = (__instance_t *) instance;

	err_info->err_detail = 0;
	err_info->oserr = 0;
	if (NULL == inst) {
		libpd_log (LEVEL_ERROR, ("Null instance on libparodus_close_receiver\n"));
		err_info->err_detail = LIBPD_ERR_CLOSE_RCV_NULL_INST;
		return LIBPD_ERROR_CLOSE_RCV_NULL_INST;
	}
	if (!inst->cfg.receive) {
		libpd_log (LEVEL_ERROR, ("No receive option on libparodus_close_receiver\n"));
		err_info->err_detail = LIBPD_ERR_CLOSE_RCV_CFG;
		return LIBPD_ERROR_CLOSE_RCV_CFG;
	}
	if (RUN_STATE_RUNNING != inst->run_state) {
		libpd_log (LEVEL_ERROR, ("LIBPARODUS: not running at close receiver\n"));
		err_info->err_detail = LIBPD_ERR_CLOSE_RCV_STATE;
		return LIBPD_ERROR_CLOSE_RCV_STATE;
	}
	rtn = libparodus_close_receiver__ (inst->wrp_queue, &err_info->oserr);
	if (rtn == 0)
		return 0;
	if (rtn == 1) {
		err_info->err_detail = LIBPD_ERR_CLOSE_RCV_TIMEDOUT;
		return LIBPD_ERROR_CLOSE_RCV_TIMEDOUT;
	}
	err_info->err_detail = rtn;
	return LIBPD_ERROR_CLOSE_RCV_SEND;
}

int libparodus_close_receiver (libpd_instance_t instance)
{
  extra_err_info_t err;
  return libparodus_close_receiver_dbg (instance, &err);
}

static int wrp_sock_send (__instance_t *inst, wrp_msg_t *msg, extra_err_info_t *err_info)
{
	int rtn;
	ssize_t msg_len;
	void *msg_bytes;
#ifdef TEST_SOCKET_TIMING
	sst_times_t sst_times;
#define SST(func) func
#else
#define SST(func)
#endif

	err_info->err_detail = 0;
	err_info->oserr = 0;
	pthread_mutex_lock (&inst->send_mutex);
	msg_len = wrp_struct_to (msg, WRP_BYTES, &msg_bytes);
	if (msg_len < 1) {
		libpd_log (LEVEL_ERROR, ("LIBPARODUS: error converting WRP to bytes\n"));
		libpd_log1(LEVEL_ERROR,"LIBPARODUS: error converting WRP to bytes");
		pthread_mutex_unlock (&inst->send_mutex);
		return -0x1001;
	}

	SST (sst_start_total_timing (&sst_times);)

	if (inst->connect_on_every_send) {
		rtn = connect_sender (inst->parodus_url, &err_info->oserr);
		if (rtn < 0) {
			free (msg_bytes);
			pthread_mutex_unlock (&inst->send_mutex);
			return -0x1200 + rtn;
		}
		inst->send_sock = rtn;
	}
	libpd_log1(LEVEL_INFO,"ParodusURL:%s msg_len=%d",inst->parodus_url,msg_len);
	SST (sst_start_send_timing (&sst_times);)
	rtn = sock_send (inst->send_sock, (const char *)msg_bytes, msg_len, 
	    &err_info->oserr);
	SST (sst_update_send_time (&sst_times);)

	if (inst->connect_on_every_send) {
		shutdown_socket (&inst->send_sock);
	}
	SST (sst_update_total_time (&sst_times);)

	free (msg_bytes);
	pthread_mutex_unlock (&inst->send_mutex);
	if (rtn == 0)
		return 0;
	return -0x1800 + rtn;
}

int libparodus_send__ (libpd_instance_t instance, wrp_msg_t *msg, 
    extra_err_info_t *err_info)
{
	int rtn = wrp_sock_send ((__instance_t *) instance, msg, err_info);
	if (rtn == 0)
		return 0;
	return LIBPD_ERR_SEND + rtn;
}

int libparodus_send_dbg (libpd_instance_t instance, wrp_msg_t *msg,
    extra_err_info_t *err_info)
{
	int rtn;
	__instance_t *inst = (__instance_t *) instance;

	err_info->err_detail = 0;
	err_info->oserr = 0;
	if (NULL == inst) {
		libpd_log (LEVEL_ERROR, ("Null instance on libparodus_send\n"));
		libpd_log1(LEVEL_ERROR,"Null instance on libparodus_send");
		err_info->err_detail = LIBPD_ERR_SEND_NULL_INST;
		return LIBPD_ERROR_SEND_NULL_INST;
	}
	if (RUN_STATE_RUNNING != inst->run_state) {
		libpd_log (LEVEL_ERROR, ("LIBPARODUS: not running at send\n"));
		libpd_log1(LEVEL_ERROR,"LIBPARODUS: not running at send");
		err_info->err_detail = LIBPD_ERR_SEND_STATE;
		return LIBPD_ERR_SEND_STATE;
	}
    libpd_log1(LEVEL_INFO,"parodus url:%s, msg type:%d",inst->parodus_url,msg->msg_type);
	rtn = libparodus_send__ (instance, msg, err_info);
	if (rtn == 0)
		return 0;
	err_info->err_detail = rtn;
	if (rtn == LIBPD_ERR_SEND_CONVERT)
		return LIBPD_ERROR_SEND_WRP_MSG;
	// errno = inst->exterr;
  return LIBPD_ERROR_SEND_SOCKET;
}

int libparodus_send (libpd_instance_t instance, wrp_msg_t *msg)
{
  extra_err_info_t err;
  libpd_log1(LEVEL_INFO,"webpa message received from onewifi");
  return libparodus_send_dbg (instance, msg, &err);
}

static char *find_wrp_msg_dest (wrp_msg_t *wrp_msg)
{
	if (wrp_msg->msg_type == WRP_MSG_TYPE__REQ)
		return wrp_msg->u.req.dest;
	if (wrp_msg->msg_type == WRP_MSG_TYPE__EVENT)
		return wrp_msg->u.event.dest;
	if (wrp_msg->msg_type == WRP_MSG_TYPE__CREATE)
		return wrp_msg->u.crud.dest;
	if (wrp_msg->msg_type == WRP_MSG_TYPE__RETREIVE)
		return wrp_msg->u.crud.dest;
	if (wrp_msg->msg_type == WRP_MSG_TYPE__UPDATE)
		return wrp_msg->u.crud.dest;
	if (wrp_msg->msg_type == WRP_MSG_TYPE__DELETE)
		return wrp_msg->u.crud.dest;
	return NULL;
}

static void wrp_receiver_reconnect (__instance_t *inst, extra_err_info_t *err_info)
{
	int p = 2;
	int retry_delay = 0;

	while (true)
	{
		shutdown_socket (&inst->rcv_sock);
		if (retry_delay < MAX_RECONNECT_RETRY_DELAY_SECS) {
			p = p+p;
			retry_delay = p-1;
		}
		sleep (retry_delay);
		libpd_log (LEVEL_DEBUG, ("Retrying receiver connection\n"));
		inst->rcv_sock = connect_receiver 
			(inst->client_url, inst->cfg.keepalive_timeout_secs, 
			 &err_info->oserr);
		if (inst->rcv_sock < 0)
			continue;
		if (send_registration_msg (inst, err_info) != 0)
			continue;
		break;
	}
	inst->auth_received = false;
	inst->reconnect_count++;
	return;
}

static void *wrp_receiver_thread (void *arg)
{
	int rtn, msg_len;
	raw_msg_t raw_msg;
	wrp_msg_t *wrp_msg;
	int end_msg_len = strlen(end_msg);
	__instance_t *inst = (__instance_t*) arg;
	extra_err_info_t *rcv_err = &inst->rcv_err_info;
	char *msg_dest, *msg_service;

	libpd_log (LEVEL_INFO, ("LIBPARODUS: Starting wrp receiver thread\n"));
	while (1) {
		rtn = sock_receive (inst->rcv_sock, &raw_msg, &rcv_err->oserr);
		if (rtn != 0) {
			if (rtn == 1) { // timed out
				if (RUN_STATE_RUNNING != inst->run_state) {
					break;
				}
				wrp_receiver_reconnect (inst, rcv_err);
				continue;
			}
			break;
		}
		if (raw_msg.len >= end_msg_len) {
			if (strncmp (raw_msg.msg, end_msg, end_msg_len) == 0) {
				nn_freemsg (raw_msg.msg);
				break;
			}
		}
		if (RUN_STATE_RUNNING != inst->run_state) {
			nn_freemsg (raw_msg.msg);
			continue;
		}
		libpd_log (LEVEL_DEBUG, ("LIBPARODUS: Converting bytes to WRP\n")); 
 		msg_len = (int) wrp_to_struct (raw_msg.msg, raw_msg.len, WRP_BYTES, &wrp_msg);
		nn_freemsg (raw_msg.msg);
		if (msg_len < 1) {
			libpd_log (LEVEL_ERROR, ("LIBPARODUS: error converting bytes to WRP\n"));
			continue;
		}
		if (wrp_msg->msg_type == WRP_MSG_TYPE__AUTH) {
			libpd_log (LEVEL_INFO, ("LIBPARODUS: AUTH msg received\n"));
			inst->auth_received = true;
			wrp_free_struct (wrp_msg);
			continue;
		}

		if (wrp_msg->msg_type == WRP_MSG_TYPE__SVC_ALIVE) {
			libpd_log (LEVEL_DEBUG, ("LIBPARODUS: received keep alive message\n"));
			inst->keep_alive_count++;
			wrp_free_struct (wrp_msg);
			continue;
		}

		// Pass thru REQ, EVENT, and CRUD if dest matches the selected service
		msg_dest = find_wrp_msg_dest (wrp_msg);
		if (NULL == msg_dest) {
			libpd_log (LEVEL_ERROR, ("LIBPARADOS: Unprocessed msg type %d received\n",
				wrp_msg->msg_type));
			wrp_free_struct (wrp_msg);
			continue;
		}
		msg_service = strchr (msg_dest, '/');
		if (NULL == msg_service) {
			wrp_free_struct (wrp_msg);
			continue;
		}
                msg_service++;
                size_t len = strlen(msg_service);
                char *tmp = strchr (msg_service, '/');
                if (NULL != tmp) {
                    len = (uintptr_t)tmp - (uintptr_t)msg_service;
                }
		if (strncmp (msg_service, inst->cfg.service_name, len) != 0) {
			wrp_free_struct (wrp_msg);
			continue;
		}
		libpd_log (LEVEL_DEBUG, ("LIBPARODUS: received msg directed to service %s\n",
			inst->cfg.service_name));
		libpd_qsend (inst->wrp_queue, (void *) wrp_msg, WRP_QUEUE_SEND_TIMEOUT_MS, 
			&rcv_err->oserr);
	}
	libpd_log (LEVEL_INFO, ("Ended wrp receiver thread\n"));
	return NULL;
}


int flush_wrp_queue (libpd_mq_t wrp_queue, uint32_t delay_ms, int *oserr)
{
	wrp_msg_t *wrp_msg = NULL;
	int count = 0;
	int err;

	while (1) {
		err = timed_wrp_queue_receive (wrp_queue, &wrp_msg, delay_ms, oserr);
		if (err == 1)	// timed out
			break;
		if (err != 0)
			return err;
		count++;
		wrp_free (wrp_msg);
	}
	libpd_log (LEVEL_INFO, ("LIBPARODUS: flushed %d messages out of WRP Queue\n", 
		count));
	return count;
}

// Functions used by libpd_test.c

int test_create_wrp_queue (libpd_mq_t *wrp_queue, 
	const char *wrp_queue_name, int *oserr)
{
	return libpd_qcreate (wrp_queue, wrp_queue_name, 24, oserr);
}

void test_close_wrp_queue (libpd_mq_t *wrp_queue)
{
	libpd_qdestroy (wrp_queue, &wrp_free);
}

int test_send_wrp_queue_ok (libpd_mq_t wrp_queue, int *oserr)
{
	wrp_msg_t *reg_msg = (wrp_msg_t *) malloc (sizeof(wrp_msg_t));
	char *name;
	reg_msg->msg_type = WRP_MSG_TYPE__SVC_REGISTRATION;
	name = strdup("iot");
	reg_msg->u.reg.service_name = name;
	name = strdup(PARODUS_CLIENT_URL);
	reg_msg->u.reg.url = name;
	return libpd_qsend (wrp_queue, (void *) reg_msg, 
		WRP_QUEUE_SEND_TIMEOUT_MS, oserr);
}

int test_close_receiver (libpd_mq_t wrp_queue, int *oserr)
{
	return libparodus_close_receiver__ (wrp_queue, oserr);
}

void test_get_counts (libpd_instance_t instance, 
	int *keep_alive_count, int *reconnect_count)
{
	__instance_t *inst = (__instance_t *) instance;
	*keep_alive_count = inst->keep_alive_count;
	*reconnect_count = inst->reconnect_count;
}
#define MSG_BUF_SIZE 4096
#define LOG_FILE "/tmp/libparodus_log.txt"
void libpd_log1 ( int level, const char *msg, ...)
{
	char *pTempChar = NULL;
	int buf_limit=0, nbytes=0;
	
	va_list arg_ptr; 
	if(strcmp(Service_Name,"CcspWifiSsp"))
	{
		return;
	}
	pTempChar = (char *)malloc(MSG_BUF_SIZE);
	if(pTempChar)
	{
		buf_limit = MSG_BUF_SIZE;
		va_start(arg_ptr, msg); 
		nbytes = vsnprintf(pTempChar, buf_limit, msg, arg_ptr);
		if(nbytes < 0)
		{
			perror(pTempChar);
		}
		va_end(arg_ptr);
		
        FILE     *fp        = NULL;
        fp = fopen ( LOG_FILE, "a+");
        if (fp)
        {
                                            struct tm *tm_info;
                                            struct timeval tv_now;
                                            char tmp[128],time[150];
                                            gettimeofday(&tv_now, NULL);
                                            tm_info = (struct tm *)localtime(&tv_now.tv_sec);

                                            strftime(tmp, 128, "%y%m%d-%T", tm_info);

                                            snprintf(time, 150, "%s.%06lld", tmp, (long long)tv_now.tv_usec);			
			if(level == LEVEL_ERROR)
            fprintf(fp,"%s Error: %s\n",time,pTempChar);
			if(level == LEVEL_INFO)
            fprintf(fp,"%s Info: %s\n",time,pTempChar);
			if(level == LEVEL_DEBUG)
            fprintf(fp,"%s Debug: %s\n",time,pTempChar);						
            fclose(fp);
        }		
	
		if(pTempChar !=NULL)
		{
			free(pTempChar);
			pTempChar = NULL;
		}
			
	}
	return;	
}