/*****************************************************************************
 * Copyright (C) 2014-2015
 * file:    file_watcher.c
 * author:  gozfree <gozfree@163.com>
 * created: 2015-04-29 00:20
 * updated: 2015-05-02 00:49
 *****************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <libgzf.h>
#include <libdict.h>
#include <libgevent.h>
#include <libglog.h>
#include "uthash.h"

#define WATCH_MOVED     1
#define WATCH_MODIFY    1

#define FTP_ROOT_DIR		"/tmp"

typedef struct path_dict {
    int wd;
    char path[PATH_MAX];
    UT_hash_handle hh;
} path_dict_t;

typedef struct fw {
    int fd;
    struct gevent_base *evbase;
    struct path_dict *pdict;
    dict *dict_path;
} fw_t;

static struct fw *_fw = NULL;

int add_path_list(struct fw *fw, int wd, const char *path)
{
    char key[9];
    memset(key, 0, sizeof(key));
    snprintf(key, 9, "%d", wd);
    char *val = CALLOC(PATH_MAX, char);
    strncpy(val, path, PATH_MAX);
    logd("dict_add key:val = %s:%s\n", key, val);
    dict_add(fw->dict_path, key, val);
    return 0;
}

int del_path_list(struct fw *fw, int wd)
{
    char key[9];
    memset(key, 0, sizeof(key));
    snprintf(key, sizeof(key), "%d", wd);
    logd("dict_del key = %s\n", key);
    dict_del(fw->dict_path, key);
    return 0;
}

int fw_add_watch(struct fw *fw, const char *path, uint32_t mask)
{
    int fd = fw->fd;
    int wd = inotify_add_watch(fd, path, mask);
    if (wd == -1) {
        loge("inotify_add_watch %s failed(%d): %s\n", path, errno, strerror(errno));
        if (errno == ENOSPC) {
            return -2;
        }
        return -1;
    }
    logd("inotify_add_watch %d:%s success\n", wd, path);
    add_path_list(fw, wd, path);
    return 0;
}

int fw_del_watch(struct fw *fw, const char *path)
{
    int fd = fw->fd;
    int rank = 0;
    char *key, *val;
    while (1) {
        rank = dict_enumerate(fw->dict_path, rank, &key, &val);
        if (rank < 0) {
            logd("dict_enumerate end\n");
            return -1;
        }
        if (!strncmp(path, val, strlen(path))) {
            logd("find key:val = %s:%s\n", key, val);
            break;
        }
    }
    int wd = atoi(key);
    logd("atoi(%s) = %d\n", key, wd);
    if (-1 == inotify_rm_watch(fd, wd)) {
        logd("inotify_rm_watch %d failed(%d):%s\n", wd, errno, strerror(errno));
    }
    del_path_list(fw, wd);
    return 0;
}

int fw_add_watch_recursive(struct fw *fw, const char *path)
{
    int res;
    int fd = fw->fd;
    struct dirent *ent = NULL;
    DIR *pdir = NULL;
    char full_path[PATH_MAX];
    uint32_t mask = IN_CREATE | IN_DELETE;
#if WATCH_MOVED
    mask |= IN_MOVE | IN_MOVE_SELF;
#endif
#if WATCH_MODIFY
    mask |= IN_MODIFY;
#endif
    if (fd == -1 || path == NULL) {
        loge("invalid paraments\n");
        return -1;
    }
    pdir = opendir(path);
    if (!pdir) {
        loge("opendir %s failed(%d): %s\n", path, errno, strerror(errno));
        if (errno == EMFILE) {
            return -2;//stop recursive
        } else {
            return -1;//continue recursive
        }
    }

    while (NULL != (ent = readdir(pdir))) {
        if (ent->d_type == DT_DIR) {
            if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
                continue;
            sprintf(full_path, "%s/%s", path, ent->d_name);
            res = fw_add_watch_recursive(fw, full_path);
            if (res == -2) {
                return -2;
            }
        } else if (ent->d_type == DT_REG){
            sprintf(full_path, "%s/%s", path, ent->d_name);
            res = fw_add_watch(fw, full_path, mask);
            if (res == -2) {
                return -2;
            }
        }
    }
    fw_add_watch(fw, path, mask);
    closedir(pdir);
    return 0;
}

int fw_del_watch_recursive(struct fw *fw, const char *path)
{
    int rank = 0;
    char *key, *val;
    while (1) {
        rank = dict_enumerate(fw->dict_path, rank, &key, &val);
        if (rank < 0) {
            logd("dict_enumerate end\n");
            break;
        }
        if (!strncmp(path, val, strlen(path))) {
            logd("find key:val = %s:%s\n", key, val);
            fw_del_watch(fw, path);
        }
    }
    return 0;
}

int fw_update_watch(struct fw *fw, struct inotify_event *iev)
{
    char full_path[PATH_MAX];
    uint32_t mask = IN_CREATE | IN_DELETE;
#if WATCH_MOVED
    mask |= IN_MOVE | IN_MOVE_SELF;
#endif
#if WATCH_MODIFY
    mask |= IN_MODIFY;
#endif
    char key[9];
    memset(key, 0, sizeof(key));
    snprintf(key, 9, "%d", iev->wd);
    char *path = (char *)dict_get(fw->dict_path, key, NULL);
    if (!path) {
        loge("dict_get NULL key=%s\n", key);
        return -1;
    }
    logd("dict_get key:val = %s:%s\n", key, path);
    memset(full_path, 0, sizeof(full_path));
    sprintf(full_path, "%s/%s", path, iev->name);
    logd("fw_update_watch iev->mask = %p, path=%s\n", iev->mask, full_path);
    if (iev->mask & IN_CREATE) {
        if (iev->mask & IN_ISDIR) {
            logi("[CREATE DIR] %s\n", full_path);
            fw_add_watch_recursive(fw, full_path);
        } else {
            logi("[CREATE FILE] %s\n", full_path);
            fw_add_watch(fw, full_path, mask);
        }
    } else if (iev->mask & IN_DELETE) {
        if (iev->mask & IN_ISDIR) {
            logi("[DELETE DIR] %s\n", full_path);
            fw_del_watch_recursive(fw, full_path);
        } else {
            logi("[DELETE FILE] %s\n", full_path);
            fw_del_watch(fw, full_path);
        }
    } else if (iev->mask & IN_MOVED_FROM){
        if (iev->mask & IN_ISDIR) {
            logi("[MOVE DIR FROM] %s\n", full_path);
            fw_del_watch_recursive(fw, full_path);
        } else {
            logi("[MOVE FILE FROM] %s\n", full_path);
            fw_del_watch(fw, full_path);
        }
    } else if (iev->mask & IN_MOVED_TO){
        if (iev->mask & IN_ISDIR) {
            logi("[MOVE DIR TO] %s\n", full_path);
            fw_add_watch_recursive(fw, full_path);
        } else {
            logi("[MOVE FILE TO] %s\n", full_path);
            fw_add_watch(fw, full_path, mask);
        }
    } else if (iev->mask & IN_IGNORED){
        logd("ignored, file %s watch was removed\n", path);
    } else if (iev->mask & IN_MODIFY){
        logi("[MODIFY FILE] %s\n", full_path);
    } else {
        logi("unknown inotify_event:%p\n", iev->mask);
    }
    return 0;
}

struct fw *fw_init()
{
    struct fw *fw = CALLOC(1, struct fw);
    if (!fw) {
        loge("malloc fw failed\n");
        goto err;
    }
    int fd = inotify_init();
    if (fd == -1) {
        loge("inotify_init failed(%d)\n", errno, strerror(errno));
        goto err;
    }
    struct gevent_base *evbase = gevent_base_create();
    if (!evbase) {
        loge("gevent_base_create failed\n");
        goto err;
    }
    fw->fd = fd;
    fw->evbase = evbase;
    fw->dict_path = dict_new();
    return fw;
err:
    if (fw) {
        free(fw);
    }
    return NULL;
}

void on_read_ops(int fd, void *arg)
{
#define INOTIFYEVENTBUFSIZE (1024)
    int i, len;
    struct inotify_event *iev;
    size_t iev_size;
    char ibuf[INOTIFYEVENTBUFSIZE] __attribute__ ((aligned(4))) = {0};
    struct fw *fw = (struct fw *)arg;

    logd("on_read_ops\n");
again:
    len = read(fd, ibuf, sizeof(ibuf));
    if (len == 0) {
        loge("read inofity event buffer 0, error: %s\n", strerror(errno));
    } else if (len == -1) {
        loge("read inofity event buffer error: %s\n", strerror(errno));
        if (errno == EINTR) {
            loge("errno=EINTR\n");
            goto again;
        }
    } else {
        logd("read len = %d\n", len);
        i = 0;
        while (i < len) {
            iev = (struct inotify_event *)(ibuf + i);
            if (iev->len > 0) {
                logd("iev->len=%d, name=%s\n", iev->len, iev->name);
                fw_update_watch(fw, iev);
            } else {
                logd("iev->len=%d\n", iev->len);
            }
            iev_size = sizeof(struct inotify_event) + iev->len;
            i += iev_size;
        }
    }
}

int fw_dispatch(struct fw *fw)
{
    int fd = fw->fd;
    struct gevent *ev;
    struct gevent_base *evbase = fw->evbase;
    ev = gevent_create(fd, on_read_ops, NULL, NULL, fw);
    gevent_add(evbase, ev);
    logi("file watcher start running...\n");
    gevent_base_loop(evbase);
    return 0;
}

void ctrl_c_op(int signo)
{
    int fd = _fw->fd;
    int rank = 0;
    char *key, *val;
    while (1) {
        rank = dict_enumerate(_fw->dict_path, rank, &key, &val);
        if (rank < 0) {
            logd("dict_enumerate end\n");
            break;
        }
        int wd = atoi(key);
        inotify_rm_watch(fd, wd);
        free(val);
    }
    logi("file_watcher exit\n");
    exit(0);
}

void usage()
{
    printf("usage: run as daemon: ./file_watcher -d\n"
            "      run for debug: ./file_watcher\n");
}

int main(int argc, char **argv)
{
    if (argc > 2) {
        usage();
        exit(-1);
    }
    if (argc == 2) {
        if (!strcmp(argv[1], "-d")) {
            daemon(0, 0);
            log_init(LOG_RSYSLOG, "local1");
        }
    } else {
        log_init(LOG_STDERR, NULL);
    }
    signal(SIGINT , ctrl_c_op);
    _fw = fw_init();
    if (!_fw) {
        loge("fw_init failed!\n");
        return -1;
    }
    fw_add_watch_recursive(_fw, FTP_ROOT_DIR);
    fw_dispatch(_fw);
    return 0;
}
