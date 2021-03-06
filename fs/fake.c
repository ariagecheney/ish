#include <stdarg.h>
#include <limits.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sqlite3.h>

#include "debug.h"
#include "kernel/errno.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "fs/dev.h"

// TODO document database

struct ish_stat {
    dword_t mode;
    dword_t uid;
    dword_t gid;
    dword_t rdev;
};

static void db_check_error(struct mount *mount) {
    int errcode = sqlite3_errcode(mount->db);
    switch (errcode) {
        case SQLITE_OK:
        case SQLITE_ROW:
        case SQLITE_DONE:
            break;

        default:
            die("sqlite error: %s", sqlite3_errmsg(mount->db));
    }
}

static sqlite3_stmt *db_prepare(struct mount *mount, const char *stmt) {
    sqlite3_stmt *statement;
    sqlite3_prepare_v2(mount->db, stmt, strlen(stmt) + 1, &statement, NULL);
    db_check_error(mount);
    return statement;
}

static bool db_exec(struct mount *mount, sqlite3_stmt *stmt) {
    int err = sqlite3_step(stmt);
    db_check_error(mount);
    return err == SQLITE_ROW;
}
static void db_reset(struct mount *mount, sqlite3_stmt *stmt) {
    sqlite3_reset(stmt);
    db_check_error(mount);
}
static void db_exec_reset(struct mount *mount, sqlite3_stmt *stmt) {
    db_exec(mount, stmt);
    db_reset(mount, stmt);
}

static void db_begin(struct mount *mount) {
    lock(&mount->lock);
    db_exec_reset(mount, mount->stmt.begin);
}
static void db_commit(struct mount *mount) {
    db_exec_reset(mount, mount->stmt.commit);
    unlock(&mount->lock);
}
static void db_rollback(struct mount *mount) {
    db_exec_reset(mount, mount->stmt.rollback);
    unlock(&mount->lock);
}

static void bind_path(sqlite3_stmt *stmt, int i, const char *path) {
    sqlite3_bind_blob(stmt, i, path, strlen(path), SQLITE_TRANSIENT);
}

static ino_t path_get_inode(struct mount *mount, const char *path) {
    // select inode from paths where path = ?
    bind_path(mount->stmt.path_get_inode, 1, path);
    ino_t inode = 0;
    if (db_exec(mount, mount->stmt.path_get_inode))
        inode = sqlite3_column_int64(mount->stmt.path_get_inode, 0);
    db_reset(mount, mount->stmt.path_get_inode);
    return inode;
}
static bool path_read_stat(struct mount *mount, const char *path, struct ish_stat *stat, ino_t *inode) {
    // select inode, stat from stats natural join paths where path = ?
    bind_path(mount->stmt.path_read_stat, 1, path);
    bool exists = db_exec(mount, mount->stmt.path_read_stat);
    if (exists) {
        if (inode)
            *inode = sqlite3_column_int64(mount->stmt.path_read_stat, 0);
        if (stat)
            *stat = *(struct ish_stat *) sqlite3_column_blob(mount->stmt.path_read_stat, 1);
    }
    db_reset(mount, mount->stmt.path_read_stat);
    return exists;
}
static void path_create(struct mount *mount, const char *path, struct ish_stat *stat) {
    // insert into stats (stat) values (?)
    sqlite3_bind_blob(mount->stmt.path_create_stat, 1, stat, sizeof(*stat), SQLITE_TRANSIENT);
    db_exec_reset(mount, mount->stmt.path_create_stat);
    // insert into paths values (?, last_insert_rowid())
    bind_path(mount->stmt.path_create_path, 1, path);
    db_exec_reset(mount, mount->stmt.path_create_path);
}

static void inode_read_stat(struct mount *mount, ino_t inode, struct ish_stat *stat) {
    // select stat from stats where inode = ?
    sqlite3_bind_int64(mount->stmt.inode_read_stat, 1, inode);
    if (!db_exec(mount, mount->stmt.inode_read_stat))
        die("inode_read_stat(%llu): missing inode", (unsigned long long) inode);
    *stat = *(struct ish_stat *) sqlite3_column_blob(mount->stmt.inode_read_stat, 0);
    db_reset(mount, mount->stmt.inode_read_stat);
}
static void inode_write_stat(struct mount *mount, ino_t inode, struct ish_stat *stat) {
    // update stats set stat = ? where inode = ?
    sqlite3_bind_blob(mount->stmt.inode_write_stat, 1, stat, sizeof(*stat), SQLITE_TRANSIENT);
    sqlite3_bind_int64(mount->stmt.inode_write_stat, 2, inode);
    db_exec_reset(mount, mount->stmt.inode_write_stat);
}

static void path_link(struct mount *mount, const char *src, const char *dst) {
    ino_t inode = path_get_inode(mount, src);
    if (inode == 0)
        die("fakefs link(%s, %s): nonexistent src path", src, dst);
    // insert into paths (path, inode) values (?, ?)
    bind_path(mount->stmt.path_link, 1, dst);
    sqlite3_bind_int64(mount->stmt.path_link, 2, inode);
    db_exec_reset(mount, mount->stmt.path_link);
}
static void path_unlink(struct mount *mount, const char *path) {
    // delete from paths where path = ?
    bind_path(mount->stmt.path_unlink, 1, path);
    db_exec_reset(mount, mount->stmt.path_unlink);
}
static void path_rename(struct mount *mount, const char *src, const char *dst) {
    // update or replace paths set path = ? [dst] where path = ? [src];
    bind_path(mount->stmt.path_rename, 1, dst);
    bind_path(mount->stmt.path_rename, 2, src);
    db_exec_reset(mount, mount->stmt.path_rename);
}

static struct fd *fakefs_open(struct mount *mount, const char *path, int flags, int mode) {
    struct fd *fd = realfs.open(mount, path, flags, 0666);
    if (IS_ERR(fd))
        return fd;
    db_begin(mount);
    fd->fake_inode = path_get_inode(mount, path);
    if (flags & O_CREAT_) {
        struct ish_stat ishstat;
        ishstat.mode = mode | S_IFREG;
        ishstat.uid = current->euid;
        ishstat.gid = current->egid;
        ishstat.rdev = 0;
        if (fd->fake_inode == 0) {
            path_create(mount, path, &ishstat);
            fd->fake_inode = path_get_inode(mount, path);
        }
    }
    db_commit(mount);
    if (fd->fake_inode == 0) {
        // metadata for this file is missing
        // TODO unlink the real file
        fd_close(fd);
        return ERR_PTR(_ENOENT);
    }
    return fd;
}

static int fakefs_link(struct mount *mount, const char *src, const char *dst) {
    db_begin(mount);
    int err = realfs.link(mount, src, dst);
    if (err < 0) {
        db_rollback(mount);
        return err;
    }
    path_link(mount, src, dst);
    db_commit(mount);
    return 0;
}

static int fakefs_unlink(struct mount *mount, const char *path) {
    db_begin(mount);
    int err = realfs.unlink(mount, path);
    if (err < 0) {
        db_rollback(mount);
        return err;
    }
    path_unlink(mount, path);
    db_commit(mount);
    return 0;
}

static int fakefs_rmdir(struct mount *mount, const char *path) {
    db_begin(mount);
    int err = realfs.rmdir(mount, path);
    if (err < 0) {
        db_rollback(mount);
        return err;
    }
    path_unlink(mount, path);
    db_commit(mount);
    return 0;
}

static int fakefs_rename(struct mount *mount, const char *src, const char *dst) {
    db_begin(mount);
    int err = realfs.rename(mount, src, dst);
    if (err < 0) {
        db_rollback(mount);
        return err;
    }
    path_rename(mount, src, dst);
    db_commit(mount);
    return 0;
}

static int fakefs_symlink(struct mount *mount, const char *target, const char *link) {
    db_begin(mount);
    // create a file containing the target
    int fd = openat(mount->root_fd, fix_path(link), O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0) {
        db_rollback(mount);
        return errno_map();
    }
    ssize_t res = write(fd, target, strlen(target));
    close(fd);
    if (res < 0) {
        int saved_errno = errno;
        unlinkat(mount->root_fd, fix_path(link), 0);
        db_rollback(mount);
        errno = saved_errno;
        return errno_map();
    }

    // customize the stat info so it looks like a link
    struct ish_stat ishstat;
    ishstat.mode = S_IFLNK | 0777; // symlinks always have full permissions
    ishstat.uid = current->euid;
    ishstat.gid = current->egid;
    ishstat.rdev = 0;
    path_create(mount, link, &ishstat);
    db_commit(mount);
    return 0;
}

static int fakefs_mknod(struct mount *mount, const char *path, mode_t_ mode, dev_t_ dev) {
    mode_t_ real_mode = 0666;
    if (S_ISBLK(mode) || S_ISCHR(mode))
        real_mode |= S_IFREG;
    else
        real_mode |= mode & S_IFMT;
    db_begin(mount);
    int err = realfs.mknod(mount, path, real_mode, 0);
    if (err < 0) {
        db_rollback(mount);
        return err;
    }
    struct ish_stat stat;
    stat.mode = mode;
    stat.uid = current->euid;
    stat.gid = current->egid;
    stat.rdev = 0;
    if (S_ISBLK(mode) || S_ISCHR(mode))
        stat.rdev = dev;
    path_create(mount, path, &stat);
    db_commit(mount);
    return err;
}

static int fakefs_stat(struct mount *mount, const char *path, struct statbuf *fake_stat, bool follow_links) {
    db_begin(mount);
    struct ish_stat ishstat;
    ino_t inode;
    if (!path_read_stat(mount, path, &ishstat, &inode)) {
        db_rollback(mount);
        return _ENOENT;
    }
    int err = realfs.stat(mount, path, fake_stat, follow_links);
    db_commit(mount);
    if (err < 0)
        return err;
    fake_stat->inode = inode;
    fake_stat->mode = ishstat.mode;
    fake_stat->uid = ishstat.uid;
    fake_stat->gid = ishstat.gid;
    fake_stat->rdev = ishstat.rdev;
    return 0;
}

static int fakefs_fstat(struct fd *fd, struct statbuf *fake_stat) {
    int err = realfs.fstat(fd, fake_stat);
    if (err < 0)
        return err;
    db_begin(fd->mount);
    struct ish_stat ishstat;
    inode_read_stat(fd->mount, fd->fake_inode, &ishstat);
    db_commit(fd->mount);
    fake_stat->inode = fd->fake_inode;
    fake_stat->mode = ishstat.mode;
    fake_stat->uid = ishstat.uid;
    fake_stat->gid = ishstat.gid;
    fake_stat->rdev = ishstat.rdev;
    return 0;
}

static void fake_stat_setattr(struct ish_stat *ishstat, struct attr attr) {
    switch (attr.type) {
        case attr_uid:
            ishstat->uid = attr.uid;
            break;
        case attr_gid:
            ishstat->gid = attr.gid;
            break;
        case attr_mode:
            ishstat->mode = (ishstat->mode & S_IFMT) | (attr.mode & ~S_IFMT);
            break;
    }
}

static int fakefs_setattr(struct mount *mount, const char *path, struct attr attr) {
    if (attr.type == attr_size)
        return realfs.setattr(mount, path, attr);
    db_begin(mount);
    struct ish_stat ishstat;
    ino_t inode;
    if (!path_read_stat(mount, path, &ishstat, &inode)) {
        db_rollback(mount);
        return _ENOENT;
    }
    fake_stat_setattr(&ishstat, attr);
    inode_write_stat(mount, inode, &ishstat);
    db_commit(mount);
    return 0;
}

static int fakefs_fsetattr(struct fd *fd, struct attr attr) {
    if (attr.type == attr_size)
        return realfs.fsetattr(fd, attr);
    db_begin(fd->mount);
    struct ish_stat ishstat;
    inode_read_stat(fd->mount, fd->fake_inode, &ishstat);
    fake_stat_setattr(&ishstat, attr);
    inode_write_stat(fd->mount, fd->fake_inode, &ishstat);
    db_commit(fd->mount);
    return 0;
}

static int fakefs_mkdir(struct mount *mount, const char *path, mode_t_ mode) {
    db_begin(mount);
    int err = realfs.mkdir(mount, path, 0777);
    if (err < 0) {
        db_rollback(mount);
        return err;
    }
    struct ish_stat ishstat;
    ishstat.mode = mode | S_IFDIR;
    ishstat.uid = current->euid;
    ishstat.gid = current->egid;
    ishstat.rdev = 0;
    path_create(mount, path, &ishstat);
    db_commit(mount);
    return 0;
}

static ssize_t file_readlink(struct mount *mount, const char *path, char *buf, size_t bufsize) {
    // broken symlinks can't be included in an iOS app or else Xcode craps out
    int fd = openat(mount->root_fd, fix_path(path), O_RDONLY);
    if (fd < 0)
        return errno_map();
    int err = read(fd, buf, bufsize);
    close(fd);
    if (err < 0)
        return errno_map();
    return err;
}

static ssize_t fakefs_readlink(struct mount *mount, const char *path, char *buf, size_t bufsize) {
    db_begin(mount);
    struct ish_stat ishstat;
    if (!path_read_stat(mount, path, &ishstat, NULL)) {
        db_rollback(mount);
        return _ENOENT;
    }
    if (!S_ISLNK(ishstat.mode)) {
        db_rollback(mount);
        return _EINVAL;
    }

    ssize_t err = realfs.readlink(mount, path, buf, bufsize);
    if (err == _EINVAL)
        err = file_readlink(mount, path, buf, bufsize);
    db_commit(mount);
    return err;
}

int fakefs_rebuild(struct mount *mount);
int fakefs_migrate(struct mount *mount);

#if DEBUG_sql
static int trace_callback(unsigned why, void *fuck, void *stmt, void *sql) {
    printk("sql trace: %s\n", sqlite3_expanded_sql(stmt));
    return 0;
}
#endif

static int fakefs_mount(struct mount *mount) {
    char db_path[PATH_MAX];
    strcpy(db_path, mount->source);
    char *basename = strrchr(db_path, '/') + 1;
    assert(strcmp(basename, "data") == 0);
    strcpy(basename, "meta.db");

    // check if it is in fact a sqlite database
    char buf[16] = {};
    int dbf = open(db_path, O_RDONLY);
    if (dbf < 0)
        return errno_map();
    read(dbf, buf, sizeof(buf));
    close(dbf);
    if (strncmp(buf, "SQLite format 3", 15) != 0)
        return _EINVAL;

    int err = sqlite3_open_v2(db_path, &mount->db, SQLITE_OPEN_READWRITE, NULL);
    if (err != SQLITE_OK) {
        printk("error opening database: %s\n", sqlite3_errmsg(mount->db));
        sqlite3_close(mount->db);
        return _EINVAL;
    }

    // let's do WAL mode
    sqlite3_stmt *statement = db_prepare(mount, "pragma journal_mode=wal");
    db_check_error(mount);
    sqlite3_step(statement);
    db_check_error(mount);
    sqlite3_finalize(statement);

#if DEBUG_sql
    sqlite3_trace_v2(mount->db, SQLITE_TRACE_STMT, trace_callback, NULL);
#endif

    // do this now so fakefs_rebuild can use mount->root_fd
    err = realfs.mount(mount);
    if (err < 0)
        return err;

    err = fakefs_migrate(mount);
    if (err < 0)
        return err;

    // after the filesystem is compressed, transmitted, and uncompressed, the
    // inode numbers will be different. to detect this, the inode of the
    // database file is stored inside the database and compared with the actual
    // database file inode, and if they're different we rebuild the database.
    struct stat statbuf;
    if (stat(db_path, &statbuf) < 0) ERRNO_DIE("stat database");
    ino_t db_inode = statbuf.st_ino;
    statement = db_prepare(mount, "select db_inode from meta");
    if (sqlite3_step(statement) == SQLITE_ROW) {
        if ((uint64_t) sqlite3_column_int64(statement, 0) != db_inode) {
            sqlite3_finalize(statement);
            statement = NULL;
            int err = fakefs_rebuild(mount);
            if (err < 0) {
                close(mount->root_fd);
                return err;
            }
        }
    }
    if (statement != NULL)
        sqlite3_finalize(statement);

    // save current inode
    statement = db_prepare(mount, "update meta set db_inode = ?");
    sqlite3_bind_int64(statement, 1, (int64_t) db_inode);
    db_check_error(mount);
    sqlite3_step(statement);
    db_check_error(mount);
    sqlite3_finalize(statement);

    // delete orphaned stats
    statement = db_prepare(mount, "delete from stats where not exists (select 1 from paths where inode = stats.inode)");
    db_check_error(mount);
    sqlite3_step(statement);
    db_check_error(mount);
    sqlite3_finalize(statement);

    lock_init(&mount->lock);
    mount->stmt.begin = db_prepare(mount, "begin");
    mount->stmt.commit = db_prepare(mount, "commit");
    mount->stmt.rollback = db_prepare(mount, "rollback");
    mount->stmt.path_get_inode = db_prepare(mount, "select inode from paths where path = ?");
    mount->stmt.path_read_stat = db_prepare(mount, "select inode, stat from stats natural join paths where path = ?");
    mount->stmt.path_create_stat = db_prepare(mount, "insert into stats (stat) values (?)");
    mount->stmt.path_create_path = db_prepare(mount, "insert into paths values (?, last_insert_rowid())");
    mount->stmt.inode_read_stat = db_prepare(mount, "select stat from stats where inode = ?");
    mount->stmt.inode_write_stat = db_prepare(mount, "update stats set stat = ? where inode = ?");
    mount->stmt.path_link = db_prepare(mount, "insert into paths (path, inode) values (?, ?)");
    mount->stmt.path_unlink = db_prepare(mount, "delete from paths where path = ?");
    mount->stmt.path_rename = db_prepare(mount, "update or replace paths set path = ? where path = ?;");

    return 0;
}

static int fakefs_umount(struct mount *mount) {
    if (mount->db)
        sqlite3_close(mount->db);
    /* return realfs.umount(mount); */
    return 0;
}

const struct fs_ops fakefs = {
    .magic = 0x66616b65,
    .mount = fakefs_mount,
    .umount = fakefs_umount,
    .statfs = realfs_statfs,
    .open = fakefs_open,
    .readlink = fakefs_readlink,
    .link = fakefs_link,
    .unlink = fakefs_unlink,
    .rename = fakefs_rename,
    .symlink = fakefs_symlink,
    .mknod = fakefs_mknod,

    .close = realfs_close,
    .stat = fakefs_stat,
    .fstat = fakefs_fstat,
    .flock = realfs_flock,
    .setattr = fakefs_setattr,
    .fsetattr = fakefs_fsetattr,
    .getpath = realfs_getpath,
    .utime = realfs_utime,

    .mkdir = fakefs_mkdir,
    .rmdir = fakefs_rmdir,
};
