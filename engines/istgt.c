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
#include <sys/eventfd.h>
#include <poll.h>

const char *hold_tag = "fio_hold_tag";
#define HOLD_TAG	((void *) hold_tag)

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
	rte_smempool_t cmdq;
	int fd;
        struct io_u **io_completed;
};

struct netio_data *global_nd[FIO_MAX_JOBS];

extern ISTGT g_istgt;
extern CONN_Ptr *g_conns;

pthread_mutex_t init_mutex = PTHREAD_MUTEX_INITIALIZER;

struct dmu_opts {
    void *pad;
    char *pool;
    unsigned int kstats;
};

/*
 * Generate ID for IO message. Must be unique and monotonically increasing.
 */
static uint64_t gen_sequence_num(void) {
	static uint64_t seq_num;
	return (__atomic_fetch_add(&seq_num, 1, __ATOMIC_SEQ_CST));
}

void
send_reply_to_fio(ISTGT_LU_TASK_Ptr lu_task) {
	ISCSI_PDU_Ptr pdu = lu_task->lu_cmd.pdu;
	uint8_t *cp;
	uint32_t *CmdSN;
	struct netio_data *nd;
	uint64_t data = 1;
	int rc;

	assert(pdu != NULL);
	cp = (uint8_t *) &pdu->bhs;
	CmdSN = malloc(sizeof (*CmdSN));
	*CmdSN = DGET32(&cp[24]);

	nd = global_nd[pdu->dummy];
	put_to_mempool(&nd->cmdq, CmdSN);

        rc = write(nd->fd, &data, sizeof (data));
	assert(rc == sizeof (data));
	return;
}

static int
fio_istgt_queue(struct thread_data *td, struct io_u *io_u) {
	ISCSI_PDU_Ptr pdu;
	uint8_t *cp;
	uint8_t *cdb;
	int ret;
	struct netio_data *nd = td->io_ops_data;
	io_list_entry_t *io_ent;

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

	//ATTR 0x01
	/* ATTR */
	BDSET8W(&cp[1], 3, 2, 3);


//	BSET8(&cp[1], 7);	//F_bit
	BSET8(&cp[1], 5);	//W_bit
//	BSET8(&cp[0], 6);	//I_bit
//	BSET8(&cp[1], 6);	//R_bit

	DSET64(&cp[8], 0);

	/* TASK TAG */
	DSET32(&cp[16], 12);

	/* TRANSFER LEN */
	DSET32(&cp[20], 4096);
	pdu->data_segment_len = 4096;

#if 0
	/* COMMAND SEQ */
	DSET32(&cp[24], io_ent->io_num);
	io_ent->io_num = gen_sequence_num();
	g_conns[0]->sess->ExpCmdSN = io_ent->io_num;
	g_conns[0]->sess->MaxCmdSN= io_ent->io_num + 1;
#endif

	/* EXP STAT SEQ */
	DSET32(&cp[28], 12);

	/* CDB */
	cdb = &cp[32];
	cdb[0] = SBC_WRITE_16;

	/* LBA */
	DSET64(&cdb[2], 100);

	/* TRANSFER LEN */
	DSET32(&cdb[10], 1);

	g_conns[0]->full_feature = 1;
	pdu->dummy = td->thread_number - 1;

	pthread_mutex_lock(&init_mutex);
	/* COMMAND SEQ */
	io_ent->io_num = gen_sequence_num();
	DSET32(&cp[24], io_ent->io_num);
	g_conns[0]->sess->ExpCmdSN = io_ent->io_num;
	g_conns[0]->sess->MaxCmdSN= io_ent->io_num + 1;

	ret = istgt_iscsi_op_scsi(g_conns[0], pdu);
	pthread_mutex_unlock(&init_mutex);

/*
	if (io_u->ddir == DDIR_WRITE) {
		return FIO_Q_QUEUED;
	} else if (io_u->ddir == DDIR_READ) {
		return FIO_Q_QUEUED;
	} else {
		return FIO_Q_QUEUED;
	}
*/
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

static io_list_entry_t *update_reply(struct thread_data *td, uint32_t CmdSN) {
	struct netio_data *nd = td->io_ops_data;
	io_list_entry_t *iter, *last;

	iter = nd->io_inprog;
	last = NULL;

	while (iter != NULL) {
		if (iter->io_num == CmdSN)
			break;
		last = iter;
		iter = iter->io_next;
	}

	if (iter == NULL) {
		td_verror(td, ENOENT, "unknown IO number");
		return (NULL);
	}
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
	int ret, read_error = 0, count = 0;
	unsigned int i;
	int timeout = -1;
	CONN_Ptr conn = g_conns[0];
	ISTGT_LU_TASK_Ptr lu_task;
	time_t now;
	int rc;
	ISCSI_PDU_Ptr pdu = NULL;
	io_list_entry_t *ent;
	int entry_count = 0;
	uint32_t *CN;
	struct timespec ts;
	uint64_t poll_data;
	struct pollfd fds;

	ts.tv_sec = 1;
	ts.tv_nsec =  0;

	while (count < min) {
		fds.fd = nd->fd;
                fds.events = POLLIN;
                fds.revents = 0;

                rc = ppoll(&fds, 1, &ts, NULL);
                if (rc < 0) {
                } else if (rc > 0 && fds.revents == POLLIN) {
                        rc = read(nd->fd, &poll_data,
                            sizeof (poll_data));
                        ASSERT3P(rc, ==, sizeof (poll_data));

			entry_count = get_num_entries_from_mempool(&nd->cmdq);
			while (entry_count) {
				CN = (uint32_t *) get_from_mempool(&nd->cmdq);
				ent = update_reply(td, *CN);
				if (ent == NULL) {
					read_error = 1;
				} else {
					assert(nd->io_completed[count] == NULL);
					nd->io_completed[count++] = ent->io_u;
					free(ent);
					free(CN);
				}
				entry_count--;
			}
		}
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

static fio_istgt_init(struct thread_data *td) {
        struct netio_data *nd;

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
	nd->fd = eventfd(0, EFD_NONBLOCK);
	if (nd->fd < 0) {
		 log_err("Failed to open file\n");
		 return (-1);
	 }

	if (init_mempool(&nd->cmdq, td->o.iodepth + 1, 0, 0,
            "replica_cmd_mempool", NULL, NULL, NULL, false)) {
		log_err("Failed to init mempool for fio thread\n");
		return (1);
	}

        td->io_ops_data = nd;
	global_nd[td->thread_number - 1] = nd;
        return (0);
}

static fio_istgt_cleanup(struct thread_data *td) {
	struct netio_data *nd = td->io_ops_data;

	if (nd) {
		close(nd->fd);
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
	.option_struct_size = sizeof(struct dmu_opts),
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
