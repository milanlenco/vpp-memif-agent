#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <getopt.h>

#define _BSD_SOURCE
#include <endian.h>

#include <vlibapi/api.h>
#include <vlibmemory/api.h>

#define vl_typedefs
#include <vpp/api/vpe_all_api_h.h>
#include <memif/memif_all_api_h.h>
#undef vl_typedefs

#define vl_endianfun
#include <vpp/api/vpe_all_api_h.h>
#include <memif/memif_all_api_h.h>
#undef vl_endianfun

#define vl_print(handle, ...)
#define vl_printfun
#include <vpp/api/vpe_all_api_h.h>
#include <memif/memif_all_api_h.h>
#undef vl_printfun

/* Get CRC codes of the messages */
#define vl_msg_name_crc_list
#include <vpp/api/vpe_all_api_h.h>
#include <memif/memif_all_api_h.h>
#undef vl_msg_name_crc_list

#define vl_api_version(n,v) static u32 vpe_api_version = (v);
#include <vpp/api/vpe.api.h>
#undef vl_api_version

/**
 * @brief Setups message handler to provided VPP API call. Needs to be called
 * for each VPP API function that can arrive as response from VPP.
 *
 * @param[in] ID VPP API function ID.
 * @param[in] NAME VPP API function name.
 */
#define vpp_setup_handler(ID, NAME)                               \
    do {    /* TODO get message ID */                             \
        vl_msg_api_set_handlers(ID, #NAME,                        \
                               (void*)vpp_receive_msg_handler,    \
                               (void*)vl_noop_handler,            \
                               (void*)vl_api_##NAME##_t_endian,   \
                               (void*)vl_api_##NAME##_t_print,    \
                               sizeof(vl_api_##NAME##_t), 1);     \
    } while(0)

/**
 * @brief Data of a message sent between the agent and VPP.
 */
typedef struct ma_msg_s {
    void *msg;
    size_t size;
} ma_msg_t;

typedef struct ma_reply_s {
    ma_msg_t *msgs;
    size_t cnt;
    i32 retval;
    bool complete;
    bool multiple;
} ma_reply_t;

/**
 * @brief Memif Agent context structure.
 */
typedef struct ma_ctx_s {
    pthread_mutex_t lock;
    pthread_cond_t reply_cv;
    unix_shared_memory_queue_t *vlib_input_queue;  /**< VPP Library input queue. */
    u32 vlib_client_index;                         /**< VPP Library client index. */
    u32 req_ctx;
    ma_reply_t reply;
    u16 ping_msg_id;
    u16 ping_reply_msg_id;
} ma_ctx_t;

ma_ctx_t ma_ctx = {0};

/**
 * @brief Generic VPP request structure.
 */
typedef struct __attribute__ ((packed)) vl_generic_request_s {
    u16 _vl_msg_id;
    u32 client_index;
    u32 context;
} vl_generic_request_t;

/**
 * @brief Generic VPP reply structure (response with a single message).
 */
typedef struct __attribute__ ((packed)) vl_generic_reply_s {
    u16 _vl_msg_id;
    u32 context;
    i32 retval;
} vl_generic_reply_t;

/**
 * @brief Memif binary API to execute.
 */
typedef enum op_type_e {
    CREATE_MEMIF = 0,
    DELETE_MEMIF = 1,
    SET_SOCKET_FILENAME = 2,
    DUMP_MEMIF = 3
} op_type_t;

/**
 * @brief Not used, just to satisfy external references when -lvlib is not available.
 */
void
vlib_cli_output(struct vlib_main_t *vm, char *fmt, ...)
{
}

/**
 * @brief Sets correct VPP API version.
 */
void
vl_client_add_api_signatures(vl_api_memclnt_create_t *mp)
{
    /*
     * Send the main API signature in slot 0. This bit of code must
     * match the checks in ../vpe/api/api.c: vl_msg_api_version_check().
     */
    mp->api_versions[0] = clib_host_to_net_u32(vpe_api_version);
}

/**
 * @brief Internal callback automatically called by VPP library when a message
 * from VPP is received.
 */
void
vpp_receive_msg_handler(void *msg)
{
    vl_generic_reply_t *reply = NULL;
    msgbuf_t *msg_header = NULL;
    size_t msg_size = 0;
    int rv = 0;

    pthread_mutex_lock(&ma_ctx.lock);

    if (NULL == msg) {
        fprintf(stderr, "NULL message received, ignoring.\n");
        rv = -1;
        goto signal;
    }

    /* get message details */
    msg_header = (msgbuf_t *) (((u8 *) msg) - offsetof(msgbuf_t, data));
    msg_size = ntohl(msg_header->data_len);
    reply = (vl_generic_reply_t *) msg;
    printf("New message received from VPP (id=%d, size=%zu).\n", ntohs(reply->_vl_msg_id), msg_size);

    if (ma_ctx.req_ctx != reply->context) {
        fprintf(stderr, "Invalid request context for provided message, ignoring the message.\n");
        rv = -1;
        goto signal;
    }

    if (ma_ctx.ping_reply_msg_id != ntohs(reply->_vl_msg_id)) {
	/* store the message */
        rv = reply->retval;
        ma_ctx.reply.msgs = realloc(ma_ctx.reply.msgs, (++ma_ctx.reply.cnt) * sizeof(ma_msg_t));
        if (NULL == ma_ctx.reply.msgs) {
            fprintf(stderr, "Memory allocation has failed.\n");
            rv = -1;
            goto signal;
        }
    
        ma_ctx.reply.msgs[ma_ctx.reply.cnt-1].msg = malloc(msg_size);
        if (NULL == ma_ctx.reply.msgs[ma_ctx.reply.cnt-1].msg) {
            fprintf(stderr, "Memory allocation has failed.\n");
            rv = -1;
            goto signal;
        }
        memcpy(ma_ctx.reply.msgs[ma_ctx.reply.cnt-1].msg, msg, msg_size);
        ma_ctx.reply.msgs[ma_ctx.reply.cnt-1].size = msg_size;
    }

signal:
    /* signal the main thread */
    if (0 == ma_ctx.reply.retval) {
        ma_ctx.reply.retval = rv;
    }
    if (!ma_ctx.reply.multiple || ma_ctx.ping_reply_msg_id == ntohs(reply->_vl_msg_id)) {
	ma_ctx.reply.complete = true;
	pthread_cond_signal(&ma_ctx.reply_cv);
    }
    pthread_mutex_unlock(&ma_ctx.lock);
}

/**
 * @brief Allocate a VPP message.
 */
void *
vpp_alloc_msg(uint16_t msg_id, size_t msg_size)
{
    vl_generic_request_t *req = NULL;

    req = vl_msg_api_alloc(msg_size);
    if (NULL != req) {
        memset(req, 0, msg_size);
        req->_vl_msg_id = htons(msg_id);
        req->client_index = ma_ctx.vlib_client_index;
	req->context = 0;
    } else {
        fprintf(stderr, "Memory allocation has failed.\n");
    }

    return req;
}

/**
 * @brief Send request to VPP and wait for the response.
 */
int
vpp_send_request(void *request, bool multiple_replies)
{
    vl_generic_request_t *req = NULL;
    vl_api_control_ping_t *ping = NULL;
    int rv = 0, context = rand() % 20;

    req = (vl_generic_request_t *) request;
    req->context = context;
    printf("Sending a request to VPP (id=%d).\n", ntohs(req->_vl_msg_id));

    pthread_mutex_lock(&ma_ctx.lock);
    ma_ctx.req_ctx = context;
    ma_ctx.reply.multiple = multiple_replies;
    ma_ctx.reply.complete = false;

    vl_msg_api_send_shmem(ma_ctx.vlib_input_queue, (u8*)&req);

    if (multiple_replies) {
	ping = vpp_alloc_msg(ma_ctx.ping_msg_id, sizeof(*ping));
	ping->context = context;
        printf("Sending a (ping) request to VPP (id=%d).\n", ntohs(ping->_vl_msg_id));
        vl_msg_api_send_shmem(ma_ctx.vlib_input_queue, (u8*)&ping);
    }

    while (!ma_ctx.reply.complete) {
        pthread_cond_wait(&ma_ctx.reply_cv, &ma_ctx.lock);
    }
    rv = ma_ctx.reply.retval;

    pthread_mutex_unlock(&ma_ctx.lock);
    return rv;
}

/**
 * @brief Connects to VPP.
 */
static int
vpp_connect()
{
    api_main_t *am = &api_main;
    int rv = 0;

    printf("Connecting to VPP...\n");

    rv = vl_client_connect_to_vlib("/vpe-api", "memif-agent", 32);

    if (rv < 0) {
        fprintf(stderr, "Unable to connect to VPP, rv=%d.\n", rv);
    } else {
        printf("Connection to VPP established, client index=%d.\n", am->my_client_index);
        ma_ctx.vlib_client_index = am->my_client_index;
        ma_ctx.vlib_input_queue = am->shmem_hdr->vl_input_queue;
    }

    return rv;
}

/**
 * @brief Disconnects from VPP.
 */
static void
vpp_disconnect()
{
    vl_client_disconnect_from_vlib();
}

int
main (int argc, char **argv)
{
    int rv = 0;
    char *filename = NULL, c = 0;
    u8 role = 0;
    u64 key = 0;
    u32 ring_size = 0;
    op_type_t op = CREATE_MEMIF;

    struct option longopts[] = {
       { "filename",  required_argument, NULL, 'f' },
       { "role",      required_argument, NULL, 'r' },
       { "key",       required_argument, NULL, 'k' },
       { "ring-size", required_argument, NULL, 's' },
       { "mac",       required_argument, NULL, 'm' },
       { 0, 0, 0, 0 }
    };

    pthread_mutex_init(&ma_ctx.lock, NULL);
    pthread_cond_init(&ma_ctx.reply_cv, NULL);

    /* parse options */
    int curind = optind;
    while ((c = getopt_long(argc, argv, "f:r:k:s:m:", longopts, NULL)) != -1) {
        switch (c) {
            case 'f':
                filename = optarg;
                break;
	    case 'r':
                if (0 == strcmp(optarg, "master")) {
		    role = 0;
		} if (0 == strcmp(optarg, "slave")) {
		    role = 1;
		} else {
		    fprintf(stderr, "Invalid value for 'role' option.\n");
		    goto cleanup;
		}
                break;
	    case 'k':
                key = atoi(optarg);
                break;
	    case 's':
                ring_size = atoi(optarg);
                break;
	    case 'm':
		fprintf(stderr, "MAC not implemented!\n");
		goto cleanup;

            case ':':
                /* missing option argument */
                fprintf(stderr, "%s: Option '-%c' requires an argument.\n", argv[0], optopt);
                goto cleanup;
            case '?':
            default:
                /* invalid option */
                if ('\0' != optopt) {
                    fprintf(stderr, "%s: Unrecognized short option: '-%c'.\n", argv[0], optopt);
                } else {
                    fprintf(stderr, "%s: Unrecognized long option: '%s'.\n", argv[0], argv[curind]);
                }
                goto cleanup;
        }
        curind = optind;
    }

    /* parse non-option arguments (<operation name>) */
    if (optind < argc) {
        if ((argc - optind) != 1) {
            fprintf(stderr, "Too many non-option arguments given (%d). Exiting.\n", (argc - optind));
	    goto cleanup;
        }
        if (0 == strcmp(argv[optind], "create")) {
	    op = CREATE_MEMIF;
	} else if (0 == strcmp(argv[optind], "delete")) {
	    op = DELETE_MEMIF;
	} else if (0 == strcmp(argv[optind], "socket")) {
	    op = SET_SOCKET_FILENAME;
	}  else if (0 == strcmp(argv[optind], "dump")) {
	    op = DUMP_MEMIF;
        } else {
	    fprintf(stderr, "Unsupported binary API.\n");
	    goto cleanup;
	}
    }

    rv = vpp_connect();
    if (rv < 0) {
        goto cleanup;
    }

#define _(id,n,crc) \
    const char *id ## _CRC = #n "_" #crc; \
    const u32 id = vl_api_get_msg_index((u8 *)(id ## _CRC)); \
    (void )id;
    foreach_vl_msg_name_crc_vpe;
    foreach_vl_msg_name_crc_memif;
#undef _

    ma_ctx.ping_msg_id = VL_API_CONTROL_PING;
    ma_ctx.ping_reply_msg_id = VL_API_CONTROL_PING_REPLY;

    vpp_setup_handler(VL_API_MEMIF_SET_SOCKET_FILENAME_REPLY, memif_set_socket_filename);
    vpp_setup_handler(VL_API_MEMIF_CREATE_REPLY, memif_create);
    vpp_setup_handler(VL_API_MEMIF_DELETE_REPLY, memif_delete);
    vpp_setup_handler(VL_API_CONTROL_PING_REPLY, control_ping_reply);
    vpp_setup_handler(VL_API_MEMIF_DETAILS, memif_details);

    switch (op) {
        case CREATE_MEMIF:
        {
            vl_api_memif_create_t *create_req = NULL;
            vl_api_memif_create_reply_t *create_resp = NULL;
            create_req = vpp_alloc_msg(VL_API_MEMIF_CREATE, sizeof(*create_req));
	    create_req->key = htobe64(key);
	    create_req->role = role;
	    create_req->ring_size = htonl(ring_size);
            rv = vpp_send_request(create_req, false);
	    create_resp = (vl_api_memif_create_reply_t *)ma_ctx.reply.msgs[0].msg;
            if (rv < 0) {
        	fprintf(stderr, "Create-memif request has failed with rv=%d.\n", rv);
            } else {
                printf("Memif key=%lu, sw_if_index=%d was created, rv = %d\n",
		       key, ntohl(create_resp->sw_if_index), rv);
            }
	    break;
	}

	case DELETE_MEMIF:
        {
            vl_api_memif_delete_t *delete_req = NULL;
            delete_req = vpp_alloc_msg(VL_API_MEMIF_DELETE, sizeof(*delete_req));
	    delete_req->key = key;
            rv = vpp_send_request(delete_req, false);
            if (rv < 0) {
        	fprintf(stderr, "Delete-memif request has failed with rv=%d.\n", rv);
            } else {
                printf("Memif key=%lu was deleted, rv = %d\n", key, rv);
            }
	    break;
	}

        case SET_SOCKET_FILENAME:
        {
            vl_api_memif_set_socket_filename_t *sock_filename_req = NULL;
            sock_filename_req = vpp_alloc_msg(VL_API_MEMIF_SET_SOCKET_FILENAME,
                                              sizeof(*sock_filename_req));
	    if (NULL != filename) {
                strncpy((char *)sock_filename_req->filename, filename, 128);
	    }
            rv = vpp_send_request(sock_filename_req, false);
            if (rv < 0) {
        	fprintf(stderr, "Set-socket-filename request has failed with rv=%d.\n", rv);
            } else {
                printf("Memif socket filename was set to %s, rv = %d\n", filename, rv);
            }
	    break;
	}

	case DUMP_MEMIF:
        {
            vl_api_memif_dump_t *dump_req = NULL;
            dump_req = vpp_alloc_msg(VL_API_MEMIF_DUMP,
                                     sizeof(*dump_req));
            rv = vpp_send_request(dump_req, true);
            if (rv < 0) {
        	fprintf(stderr, "Dump request has failed with rv=%d.\n", rv);
            } else {
		for (size_t i = 0; i < ma_ctx.reply.cnt; ++i) {
		    vl_api_memif_details_t *details = ma_ctx.reply.msgs[i].msg;
		    printf("Memif %s:\n", details->if_name);
		    printf(" -> sw_if_index = %d\n", ntohl(details->sw_if_index));
		    printf(" -> MAC = %02X:%02X:%02X:%02X:%02X:%02X\n",
			   details->hw_addr[0], details->hw_addr[1], details->hw_addr[2],
			   details->hw_addr[3], details->hw_addr[4], details->hw_addr[5]);
		    printf(" -> key = %lu\n", be64toh(details->key));
		    printf(" -> role = %s\n", 0 == details->role ? "master" : "slave");
		    printf(" -> socket = %s\n", details->socket);
		    printf(" -> ring size = %d\n", ntohl(details->ring_size));
		    printf(" -> admin_up_down = %s\n", 0 == details->admin_up_down ? "down" : "up");
		    printf(" -> link_up_down = %s\n", 0 == details->link_up_down ? "down" : "up");
		    printf("\n");
		}
            }
	    break;
	}

    }

    vpp_disconnect();

cleanup:
    pthread_mutex_destroy(&ma_ctx.lock);
    pthread_cond_destroy(&ma_ctx.reply_cv);

    return rv;
}
