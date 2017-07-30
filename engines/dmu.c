#include "../fio.h"
#include "../optgroup.h"
#include <ctype.h>
#include <libzfs/libzfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/dmu.h>
#include <sys/dmu_objset.h>
#include <sys/dmu_tx.h>
#include <sys/dsl_dataset.h>
#include <sys/dsl_pool.h>
#include <sys/dsl_synctask.h>
#include <sys/fs/zfs.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev.h>
#include <sys/zap.h>
#include <sys/zfeature.h>
#include <sys/zfs_context.h>
#include <sys/zfs_znode.h>
#include <sys/zio_checksum.h>
#include <sys/zio_compress.h>

pthread_mutex_t init_mutex = PTHREAD_MUTEX_INITIALIZER;
static int initialized = 0;
objset_t *os;
libzfs_handle_t *g_zfs;
spa_t *spa;
char *g_name;
static importargs_t g_importargs;
struct dmu_opts {
    void *pad;
    char *pool;
};

#define ZVOL_OBJ 1ULL

static void
fatal(spa_t *spa, void *tag, const char *fmt, ...) {
    va_list ap;

    if (spa != NULL) {
        spa_close(spa, tag);
        (void)spa_export(g_name, NULL, B_TRUE, B_FALSE);
    }

    va_start(ap, fmt);
    (void)vfprintf(stderr, fmt, ap);
    va_end(ap);
    (void)fprintf(stderr, "\n");

    exit(1);
}

static void
pool_import(char *target, boolean_t readonly) {
    nvlist_t *config = NULL;
    nvlist_t *props = NULL;
    int error;

    kernel_init(readonly ? FREAD : (FREAD | FWRITE));
    g_zfs = libzfs_init();
    ASSERT(g_zfs != NULL);

    g_importargs.unique = B_TRUE;
    g_importargs.can_be_active = readonly;

    error = spa_import(target, config, props, ZFS_IMPORT_NORMAL);
    if (error == EEXIST)
        error = 0;

    if (error)
        fatal(NULL, FTAG, "can't import '%s': %s", target, strerror(error));
}

static void
user_spa_open(char *target, boolean_t readonly, void *tag, spa_t **spa) {
    int err;

    pool_import(target, readonly);
    err = spa_open(target, spa, tag);
    if (err != 0)
        fatal(*spa, FTAG, "cannot open '%s': %s", target, strerror(err));
}

static int
fio_dmu_queue(struct thread_data *td, struct io_u *io_u) {
    int error;
    if (io_u->ddir == DDIR_WRITE) {
        dmu_tx_t *tx = dmu_tx_create(os);
        dmu_tx_hold_write(tx, ZVOL_OBJ, io_u->offset, io_u->xfer_buflen);
        error = dmu_tx_assign(tx, TXG_WAIT);
        if (error != 0) {
            dmu_tx_abort(tx);
            io_u->error = error;
            td_verror(td, io_u->error, "xfer");
            return FIO_Q_COMPLETED;
        }
        dmu_write(os, ZVOL_OBJ, io_u->offset, io_u->xfer_buflen, io_u->xfer_buf,
                  tx);
        dmu_tx_commit(tx);
        return FIO_Q_COMPLETED;
    } else if (io_u->ddir == DDIR_READ) {
        error = dmu_read(os, ZVOL_OBJ, io_u->offset, io_u->xfer_buflen,
                         io_u->xfer_buf, DMU_READ_NO_PREFETCH);
        if (error != 0) {
            io_u->error = error;
            td_verror(td, io_u->error, "rfer");
        }
        return FIO_Q_COMPLETED;
    } else {
        return FIO_Q_COMPLETED;
    }
    /*  NOT REACHED */
}

void
print_stats(void *args) {

//    for (;;) {
//        system("clear");
//        show_pool_stats(spa);
//        sleep(1);
//    }
//
    zk_thread_exit();
}

kthread_t *stats_thread;
pthread_t tid;

static int
fio_dmu_init(struct thread_data *td) {
    int error = 0;

    /*  when we run multiple process we need to prevent
     *  multiple pool imports
     */

    pthread_mutex_lock(&init_mutex);
    if (initialized == 1) {
        printf("TID %d\n", td->thread_number);
        pthread_mutex_unlock(&init_mutex);
        return 0;
    } else {
        struct dmu_opts *opts = td->eo;
        if (opts->pool) {
            g_name = opts->pool;
        } else {
            exit(1);
        }

        user_spa_open(g_name, B_FALSE, FTAG, &spa);
        spa->spa_debug = 1;
        if ((error = dmu_objset_own(g_name, DMU_OST_ZVOL, B_FALSE, (void *)1,
                                    &os)) != 0) {
            dmu_objset_disown(os, (void *)1);
            pthread_mutex_unlock(&init_mutex);
            return 1;
        }
        initialized = 1;
        stats_thread =
            zk_thread_create(NULL, 0, (thread_func_t)print_stats, NULL, 0, NULL,
                             TS_RUN, 0, PTHREAD_CREATE_JOINABLE);
        tid = stats_thread->t_tid;
        pthread_mutex_unlock(&init_mutex);
    }

    printf("Pool imported!\n");
    return 0;
}

static void
fio_dmu_cleanup(struct thread_data *td) {}

static int
fio_dmu_open(struct thread_data *td, struct fio_file *f) {
    /*  we cant open multiple datasets so things are hard coded
     *  the reason that we cant do this is that internally,
     *  threads interacting with the ZFS datastructures are
     *  asserted for certain pthread_t attributes. I really dont want
     *  to change FIO *that* much. Also, its not needed as we
     *  can compare single to single threaded workloads
     */
    return 0;
}

static int
fio_dmu_close(struct thread_data *td, struct fio_file *f) {
    zk_thread_join(tid);
    dmu_objset_disown(os, (void *)1);
    spa_close(spa, FTAG);
    libzfs_fini(g_zfs);
    kernel_fini();
    return 0;
}

struct fio_option options[] = {{
                                   .name = "zvol",
                                   .type = FIO_OPT_STR_STORE,
                                   .def = NULL,
                                   .help = "selected pool",
                                   .off1 = offsetof(struct dmu_opts, pool),
                               },
                               {
                                   .name = NULL,
                               }};

struct ioengine_ops ioengine = {
    .name = "dmu",
    .version = FIO_IOOPS_VERSION,
    .init = fio_dmu_init,
    .queue = fio_dmu_queue,
    .cleanup = fio_dmu_cleanup,
    .open_file = fio_dmu_open,
    .close_file = fio_dmu_close,
    .options = options,
    .option_struct_size = sizeof(struct dmu_opts),
    .flags = FIO_SYNCIO | FIO_DISKLESSIO | FIO_NOEXTEND | FIO_NODISKUTIL,
};

static void fio_init
fio_dmu_register(void) {
    register_ioengine(&ioengine);
}

static void fio_exit
fio_dmu_unregister(void) {
    unregister_ioengine(&ioengine);
}
