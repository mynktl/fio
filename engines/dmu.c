#include "../fio.h"
#include "../optgroup.h"
#include <ctype.h>
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
#include <sys/zfs_context.h>
#include <sys/zfs_znode.h>
#include <sys/zio_checksum.h>
#include <sys/zio_compress.h>
#include <sys/kstat.h>
#include <libzfs.h>

const char *hold_tag = "fio_hold_tag";
#define HOLD_TAG	((void *) hold_tag)

pthread_mutex_t init_mutex = PTHREAD_MUTEX_INITIALIZER;
static int initialized_spa[10] = { 0 };
static int initialized_os[10] = { 0 };
objset_t *os[10] = { NULL };
spa_t *spa[10] = { NULL } ;
struct dmu_opts {
    void *pad;
    char *pool;
    unsigned int kstats;
};

struct dmu_data {
    int spa_index;
    int os_index;
};
 
static importargs_t g_importargs = {0};
libzfs_handle_t *g_zfs;

#define ZVOL_OBJ 1ULL

static void
fatal(spa_t *spa, void *tag, const char *fmt, ...) {
    va_list ap;

    if (spa != NULL) {
        spa_close(spa, tag);
    }

    va_start(ap, fmt);
    (void)vfprintf(stderr, fmt, ap);
    va_end(ap);
    (void)fprintf(stderr, "\n");

    exit(1);
}

static void
pool_import(char *target, boolean_t readonly) {

	nvlist_t *config;
        nvlist_t *props;
        int error;

        g_zfs = libzfs_init();
        ASSERT(g_zfs != NULL);

	/* making unique true will require scanning
	 * of the namespace avl
	 */

        g_importargs.unique = B_FALSE;
        g_importargs.can_be_active = B_TRUE;

        error = zpool_tryimport(g_zfs, target, &config, &g_importargs);
        if (error)
                fatal(NULL, FTAG, "cannot import '%s': %s", target,
                    libzfs_error_description(g_zfs));

        props = NULL;
        if (readonly) {
                VERIFY(nvlist_alloc(&props, NV_UNIQUE_NAME, 0) == 0);
                VERIFY(nvlist_add_uint64(props,
                    zpool_prop_to_name(ZPOOL_PROP_READONLY), 1) == 0);
        }

        error = spa_import(target, config, props,
            (readonly ?  ZFS_IMPORT_SKIP_MMP : ZFS_IMPORT_NORMAL));
        if (error == EEXIST)
                error = 0;

        if (error)
                fatal(NULL, FTAG, "can't import '%s': %s", target,
                    strerror(error));

}

static spa_t *
user_spa_open(char *target, boolean_t readonly, void *tag) {
    int err;
    spa_t *spa = NULL;

    pool_import(target, readonly);
    err = spa_open(target, &spa, tag);
    if (err != 0)
        fatal(NULL, HOLD_TAG, "cannot open '%s': %s", target, strerror(err));
    return spa;
}

static int
fio_dmu_queue(struct thread_data *td, struct io_u *io_u) {
    int error;
    int os_index = (int)((struct dmu_data *)(td->io_ops_data))->os_index;
    if (io_u->ddir == DDIR_WRITE) {
        dmu_tx_t *tx = dmu_tx_create(os[os_index]);
        dmu_tx_hold_write(tx, ZVOL_OBJ, io_u->offset, io_u->xfer_buflen);
        error = dmu_tx_assign(tx, TXG_WAIT);
        if (error != 0) {
            dmu_tx_abort(tx);
            io_u->error = error;
            td_verror(td, io_u->error, "xfer");
            return FIO_Q_COMPLETED;
        }
        dmu_write(os[os_index], ZVOL_OBJ, io_u->offset, io_u->xfer_buflen, io_u->xfer_buf,
                  tx);
        dmu_tx_commit(tx);
        return FIO_Q_COMPLETED;
    } else if (io_u->ddir == DDIR_READ) {
        error = dmu_read(os[os_index], ZVOL_OBJ, io_u->offset, io_u->xfer_buflen,
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

static int
fio_dmu_init(struct thread_data *td) {
    int error = 0;
    struct dmu_opts *opts = td->eo;
    char *dsname, *c;
    int i;
    char poolname[MAXPATHLEN];
    char osname[ZFS_MAX_DATASET_NAME_LEN + 1];

    dsname = opts->pool;
    /*
     * when we run multiple process we need to prevent
     * multiple pool imports
     */
    pthread_mutex_lock(&init_mutex);

    strncpy(poolname, dsname, sizeof(poolname));
    c = strchr(poolname, '/');
    if (c != NULL)
        *c = '\0';

    for (i=0; i<10 && spa[i] != NULL; i++)
    {
        if(strcmp(spa[i]->spa_name, poolname) == 0)
        {
            initialized_spa[i]++;
            td->io_ops_data = (struct dmu_data *)malloc(sizeof(struct dmu_data));
            ((struct dmu_data *)(td->io_ops_data))->spa_index = i;
	    goto hold_os;
        }
    }

    if (i == 10) {
        fprintf(stderr, "too many pools\n");
        pthread_mutex_unlock(&init_mutex);
        exit(1);
    }

    if (!opts->pool) {
        fprintf(stderr, "No zfs pool name\n");
        pthread_mutex_unlock(&init_mutex);
        exit(1);
    }

    spa[i] = user_spa_open(poolname, B_FALSE, HOLD_TAG);
    spa[i]->spa_debug = 1;
    
    initialized_spa[i] = 1;
    td->io_ops_data = (struct dmu_data *)malloc(sizeof(struct dmu_data));
    ((struct dmu_data *)(td->io_ops_data))->spa_index = i;

hold_os:
    for (i=0; i<10 && os[i] != NULL; i++)
    {
        dmu_objset_name(os[i], osname);
	if(strcmp(osname, dsname) == 0)
        {
            initialized_os[i]++;
            ((struct dmu_data *)(td->io_ops_data))->os_index = i;
	    goto end;
        }
    }

    if (i == 10) {
        fprintf(stderr, "too many datasets\n");
        pthread_mutex_unlock(&init_mutex);
        exit(1);
    }

    if ((error = dmu_objset_own(dsname, DMU_OST_ZVOL, B_FALSE, HOLD_TAG,
                                &os[i])) != 0) {
	printf("No dataset with name %s\n", dsname);
        pthread_mutex_unlock(&init_mutex);
        return 1;
    }

    initialized_os[i] = 1;
    ((struct dmu_data *)(td->io_ops_data))->os_index = i;
end:
    pthread_mutex_unlock(&init_mutex);

    printf("Pool imported %s %s! %d %d %d\n\n\n", dsname, td->o.name, i, td->thread_number, td->subjob_number);
    return 0;
}

static void
fio_dmu_cleanup(struct thread_data *td) {
}

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
    struct dmu_opts *opts = td->eo;
    int spa_index = (int)((struct dmu_data *)(td->io_ops_data))->spa_index;
    int os_index = (int)((struct dmu_data *)(td->io_ops_data))->os_index;
    pthread_mutex_lock(&init_mutex);
    initialized_os[os_index]--;
    if(initialized_os[os_index] == 0) {
         if (opts->kstats != 0) {
             kstat_dump_all();
         }
        dmu_objset_disown(os[os_index], HOLD_TAG);
    }
    initialized_spa[spa_index]--;
    if(initialized_spa[spa_index] == 0)
        spa_close(spa[spa_index], HOLD_TAG);
    pthread_mutex_unlock(&init_mutex);
    printf("closed %s %d %d\n", td->o.name, td->thread_number, td->subjob_number);
    return 0;
}

struct fio_option options[] = {{
                                   .name = "zvol",
                                   .lname = "zvol to use for IO test",
                                   .type = FIO_OPT_STR_STORE,
                                   .def = NULL,
                                   .help = "selected pool",
                                   .off1 = offsetof(struct dmu_opts, pool),
                                   .category = FIO_OPT_C_ENGINE,
                                   .group = FIO_OPT_G_INVALID,
                               },
                               {
                                   .name = "kstats",
                                   .lname = "Print kstats when jobs finish",
                                   .type = FIO_OPT_BOOL,
                                   .def = "0",
                                   .help = "Print kstats when jobs finish",
                                   .off1 = offsetof(struct dmu_opts, kstats),
                                   .category = FIO_OPT_C_ENGINE,
                                   .group = FIO_OPT_G_INVALID,
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
