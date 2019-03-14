#include "../fio.h"
#include "../optgroup.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include "istgt.h"
#include "replication_log.h"
#include "config.h"
#include "istgt_proto.h"
#include "istgt_scsi.h"
#include "istgt_misc.h"
#include <execinfo.h>
#include <pthread.h>

struct netio_data *global_nd[FIO_MAX_JOBS];

/*
 * List of IOs in progress.
 */
struct io_list_entry;
typedef struct io_list_entry {
        uint64_t io_num;
        struct io_u *io_u;
        struct io_list_entry *io_next;
} io_list_entry_t;

/*
 * Engine per thread data
 */
struct netio_data {
        io_list_entry_t *io_inprog;
        struct io_u **io_completed;
	CONN_Ptr conn;
};

extern ISTGT g_istgt;
extern CONN_Ptr *g_conns;
extern int istgt_iscsi_op_scsi(CONN_Ptr conn, ISCSI_PDU_Ptr pdu);
extern int fio_istgt_create_conn(int id);
pthread_mutex_t init_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * Generate ID for IO message. Must be unique and monotonically increasing.
 */
static uint64_t gen_sequence_num(void) {
	static uint64_t seq_num;
	return (__atomic_fetch_add(&seq_num, 1, __ATOMIC_SEQ_CST));
}

static int
fio_istgt_queue(struct thread_data *td, struct io_u *io_u) {
	ISCSI_PDU_Ptr pdu;
	uint8_t *cp, *cdb;
	struct netio_data *nd = td->io_ops_data;
	io_list_entry_t *io_ent;
	uint64_t offset;
	int ret;

	io_ent = malloc(sizeof (*io_ent));
	if (io_ent == NULL) {
		io_u->error = ENOMEM;
		goto end;
	}
	io_ent->io_u = io_u;
	io_ent->io_next = NULL;

	pdu = malloc(sizeof (ISCSI_PDU));
	memset(pdu, 0, sizeof (ISCSI_PDU));

	pdu->bhs.opcode = ISCSI_OP_TEXT;

	cp = (uint8_t *) &pdu->bhs;
	BSET8(&cp[0], 6);
	BDSET8W(&cp[1], 3, 2, 3);	//ATTR
//	BSET8(&cp[1], 7);	//F_bit
//	BSET8(&cp[0], 6);	//I_bit
	DSET64(&cp[8], 0);
	DSET32(&cp[16], 12);		//TASK TAG
	DSET32(&cp[20], 4096);		//TRANSFER TAG
	DSET32(&cp[28], 12);		// EXP STAT SEQ
	cdb = &cp[32];			// CDB

	offset = io_u->offset / nd->conn->sess->lu->blocklen; 
	DSET64(&cdb[2], offset);		// LBA

	if ((io_u->offset % nd->conn->sess->lu->blocklen) ||
	    (io_u->xfer_buflen % nd->conn->sess->lu->blocklen)) {
		printf("invalid offset %llu blksize:%d\n", io_u->offset, nd->conn->sess->lu->blocklen);
		return -1;
	}

	if (io_u->ddir == DDIR_WRITE) {
		BSET8(&cp[1], 5);	//W_bit
		cdb[0] = SBC_WRITE_16;		// OPCODE
		pdu->data = malloc(io_u->xfer_buflen);
		memcpy(pdu->data, io_u->xfer_buf, io_u->xfer_buflen);
		pdu->data_segment_len = io_u->xfer_buflen;	//DATA SEGMENT LENGTH
		DSET32(&cdb[10], io_u->xfer_buflen / nd->conn->sess->lu->blocklen);	//TRANSFER LEN
	} else if (io_u->ddir == DDIR_READ) {
		BSET8(&cp[1], 6);	//R_bit
		cdb[0] = SBC_READ_16;		// OPCODE
		pdu->data_segment_len = io_u->xfer_buflen;	//DATA SEGMENT LENGTH
		DSET32(&cdb[10], io_u->xfer_buflen / nd->conn->sess->lu->blocklen);	//TRANSFER LEN
	} else {
		return FIO_Q_COMPLETED;
	}

	pdu->dummy = td->thread_number - 1;		// DUMMY

	pthread_mutex_lock(&init_mutex);
	io_ent->io_num = gen_sequence_num();
	DSET32(&cp[24], io_ent->io_num);	// SEQ NUMBER
	nd->conn->sess->ExpCmdSN = io_ent->io_num;
	nd->conn->sess->MaxCmdSN = io_ent->io_num + 1;

	ret = istgt_iscsi_op_scsi(nd->conn, pdu);
	pthread_mutex_unlock(&init_mutex);
	free(pdu);

	if (ret) {
		free(nd);
		return FIO_Q_COMPLETED;
	}

end:
	io_ent->io_next = nd->io_inprog;
	nd->io_inprog = io_ent;
	return FIO_Q_QUEUED;
}

static int
fio_istgt_open(struct thread_data *td, struct fio_file *f) {
	return 0;
}

static int
fio_istgt_close(struct thread_data *td, struct fio_file *f) {
	return 0;
}

static io_list_entry_t *update_reply(struct thread_data *td, ISTGT_LU_TASK_Ptr lu_task) {
	struct netio_data *nd = td->io_ops_data;
	io_list_entry_t *iter, *last;
	ISCSI_PDU_Ptr pdu = lu_task->lu_cmd.pdu;
	uint8_t *cp;
	uint32_t CmdSN;

	iter = nd->io_inprog;
	last = NULL;
	assert(pdu != NULL);
	cp = (uint8_t *) &pdu->bhs;
	CmdSN = DGET32(&cp[24]);

	while (iter != NULL) {
		if (iter->io_num == CmdSN)
			break;
		last = iter;
		iter = iter->io_next;
	}

	if (iter == NULL) {
		printf("here got iter null for nd:%p\n", nd);
		td_verror(td, ENOENT, "unknown IO number");
		return (NULL);
	}

	if (lu_task->lu_cmd.R_bit)
		memcpy(iter->io_u->xfer_buf, lu_task->lu_cmd.data, lu_task->lu_cmd.data_len);

	if (last == NULL)
		nd->io_inprog = iter->io_next;
	else
		last->io_next = iter->io_next;
	iter->io_next = NULL;
	return (iter);
}

static int fio_istgt_getevents(struct thread_data *td, unsigned int min,
		unsigned int max, const struct timespec fio_unused *t)
{
	struct netio_data *nd = td->io_ops_data;
	int count = 0;
	CONN_Ptr conn = nd->conn;
	ISTGT_LU_TASK_Ptr lu_task;
	struct timespec abstime;
	time_t now;
	int rc;
	io_list_entry_t *ent;

	memset(&abstime, 0, sizeof (abstime));
	while (count < min) {
		if (conn->state != CONN_STATE_RUNNING) {
			printf("conn state is %d ex:%d\n", conn->state, CONN_STATE_RUNNING);
			break;
		}

		MTX_LOCK(&conn->result_queue_mutex);
dequeue_result_queue:
		lu_task = istgt_queue_dequeue(&conn->result_queue);
		if (lu_task == NULL) {
			conn->sender_waiting = 1;
			now = time(NULL);
			abstime.tv_sec = now + conn->timeout;
			abstime.tv_nsec = 0;
			rc = pthread_cond_timedwait(&conn->result_queue_cond,
				&conn->result_queue_mutex, &abstime);
			conn->sender_waiting = 0;
			if (rc == ETIMEDOUT) {
				/* nothing */
			}
			if (conn->state != CONN_STATE_RUNNING) {
				MTX_UNLOCK(&conn->result_queue_mutex);
				break;
			} else {
				goto dequeue_result_queue;
			}
		}
		if (lu_task->lu_cmd.aborted == 1 || lu_task->lu_cmd.release_aborted == 1) {
			ISTGT_LOG("Aborted from result queue\n");
			MTX_UNLOCK(&conn->result_queue_mutex);
			rc = istgt_lu_destroy_task(lu_task);
			if (rc < 0)
				ISTGT_ERRLOG("lu_destroy_task failed\n");
			continue;
		}

		lu_task->lu_cmd.flags |= ISTGT_RESULT_Q_DEQUEUED;
		MTX_UNLOCK(&conn->result_queue_mutex);
		do {
			lu_task->lu_cmd.flags |= ISTGT_RESULT_Q_DEQUEUED;
			if (lu_task->lu_cmd.aborted == 1) {
				ISTGT_LOG("Aborted from result queue\n");
				rc = istgt_lu_destroy_task(lu_task);
				if (rc < 0)
					ISTGT_ERRLOG("lu_destroy_task failed\n");
				break;
			}
			lu_task->lock = 1;
			timediff(&lu_task->lu_cmd, 'r', __LINE__);
			if (lu_task->type == ISTGT_LU_TASK_RESPONSE) {
				/* send DATA-IN, SCSI status */
				ent = update_reply(td, lu_task);
				if (ent == NULL) {
					return (-1);
				} else {
					assert(nd->io_completed[count] == NULL);
					nd->io_completed[count++] = ent->io_u;
					free(ent);
				}
				rc = istgt_lu_destroy_task(lu_task);
				if (rc < 0) {
					ISTGT_ERRLOG("lu_destroy_task() failed\n");
					break;
				}
				if (count >= max)
					goto end;
				return (0);
			} else {
				ISTGT_ERRLOG("Unknown task type %x\n", lu_task->type);
				rc = -1;
			}
			// conn is running?
			if (conn->state != CONN_STATE_RUNNING) {
				// ISTGT_WARNLOG("exit thread\n");
				break;
			}
			MTX_LOCK(&conn->result_queue_mutex);
			lu_task = istgt_queue_dequeue(&conn->result_queue);
			MTX_UNLOCK(&conn->result_queue_mutex);
		} while (lu_task != NULL);
	}

end:
	return (count);
}

static struct io_u *fio_istgt_event(struct thread_data *td, int event)
{
	struct netio_data *nd = td->io_ops_data;
	struct io_u *io_u = nd->io_completed[event];
	nd->io_completed[event] = NULL;
	return (io_u);
}

static int fio_istgt_setup(struct thread_data *td) {
	return (0);
}

static int fio_istgt_init(struct thread_data *td) {
        struct netio_data *nd;
	int connid = 0;

        if (!td->o.use_thread) {
                log_err("repl: must set thread=1 when using repl plugin\n");
                return (1);
        }

        nd = malloc(sizeof (*nd));
        if (nd == NULL) {
                log_err("repl: memory allocation failed\n");
                return (1);
        }
        memset(nd, 0, sizeof (*nd));
        nd->io_inprog = NULL;
        nd->io_completed = calloc(td->o.iodepth, sizeof (struct io_u *));

        td->io_ops_data = nd;
	global_nd[td->thread_number - 1] = nd;
	connid = fio_istgt_create_conn(td->thread_number - 1);
	if (connid < 0) {
		log_err("failed to create conn\n");
		return (1);
	}
	nd->conn = g_conns[connid];
	nd->conn->full_feature = 1;
        return (0);
}

static void fio_istgt_cleanup(struct thread_data *td) {
	struct netio_data *nd = td->io_ops_data;

	if (nd) {
		free(nd->io_completed);
		free(nd);
	}
}

struct fio_option options[] = {
                               {
                                   .name = NULL,
                               },};

struct ioengine_ops ioengine = {
	.name = "istgt",
	.version = FIO_IOOPS_VERSION,
	.init = fio_istgt_init,
	.queue = fio_istgt_queue,
	.cleanup = fio_istgt_cleanup,
	.setup = fio_istgt_setup,
	.getevents = fio_istgt_getevents,
	.event = fio_istgt_event,
	.open_file = fio_istgt_open,
	.close_file = fio_istgt_close,
	.options = options,
	.option_struct_size = 0,
	.flags = FIO_SYNCIO | FIO_DISKLESSIO | FIO_NOEXTEND | FIO_NODISKUTIL,
};

static void fio_init
fio_istgt_register(void) {
    register_ioengine(&ioengine);
}

static void fio_exit
fio_istgt_unregister(void) {
    unregister_ioengine(&ioengine);
}
