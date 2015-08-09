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
#include <libgevent.h>
#include <liblog.h>
#include "uthash.h"

#define WATCH_MOVED     1
#define WATCH_MODIFY    1

#define INOTIFYEVENTBUFSIZE (1024)
#define FTP_ROOT_DIR		"/tmp/ftp"

static int g_ifd;
static struct gevent_base *g_evbase;

typedef struct wd_path {
    int wd;
    char path[PATH_MAX];
    UT_hash_handle hh;
} wd_path_t;

wd_path_t *dict = NULL;

int add_path_list(int wd, const char *path)
{
    wd_path_t *user = (wd_path_t *) malloc(sizeof(wd_path_t));
    if (user == NULL) {
        loge("malloc wd_path_t error\n");
        return -1;
    }
    strcpy(user->path, path);
    user->wd = wd;
    HASH_ADD_INT(dict, wd, user);
    return 0;
}

int del_path_list(int wd)
{
    wd_path_t *p = NULL;
    HASH_FIND_INT(dict, &wd, p);
    if (!p) {
        loge("can't find %d in hash table\n", wd);
        return -1;
    }
    HASH_DEL(dict, p);
    return 0;
}

int fw_add_watch(int fd, const char *path, uint32_t mask)
{
    int wd = inotify_add_watch(fd, path, mask);
    if (wd == -1) {
        loge("inotify_add_watch %s failed: %d\n", path, errno);
        return -1;
    }
    logd("inotify_add_watch %d:%s success\n", wd, path);
    add_path_list(wd, path);
    return 0;
}

int fw_del_watch(int fd, const char *path)
{
    wd_path_t *p;
    for (p = dict; p != NULL; p = (wd_path_t *) (p->hh.next)) {
        loge("visit p->path=%d:%s, path=%d:%s\n", strlen(p->path), p->path, strlen(path), path);
        if (!strncmp(p->path, path, strlen(path))) {
            break;
        }
    }
    if (!p) {
        loge("can't find %d:%s in hash table!\n", fd, path);
        return -1;
    }
    int ret = inotify_rm_watch(fd, p->wd);
    if (ret == -1) {
        logw("inotify_rm_watch %d failed: %d\n", p->wd, errno);
    }
    del_path_list(p->wd);
    return 0;
}

int fw_add_watch_recursive(int fd, const char *path)
{
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
        loge("opendir %s failed: %d\n", path, errno);
        return -1;
    }

    while (NULL != (ent = readdir(pdir))) {
        if (ent->d_type == DT_DIR) {
            if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
                continue;
            sprintf(full_path, "%s/%s", path, ent->d_name);
            fw_add_watch_recursive(fd, full_path);
        } else if (ent->d_type == DT_REG){
            sprintf(full_path, "%s/%s", path, ent->d_name);
            fw_add_watch(fd, full_path, mask);
        }
    }
    fw_add_watch(fd, path, mask);
    closedir(pdir);
    return 0;
}

int fw_del_watch_recursive(int fd, const char *path)
{
    wd_path_t *p;
    for (p = dict; p != NULL; p = (wd_path_t *) (p->hh.next)) {
        if (!strncmp(p->path, path, strlen(path))) {
            fw_del_watch(fd, path);
        }
    }
    return 0;
}

int fw_update_watch(int fd, struct inotify_event *iev)
{
    wd_path_t *p = NULL;
    char full_path[PATH_MAX];
    uint32_t mask = IN_CREATE | IN_DELETE;
#if WATCH_MOVED
    mask |= IN_MOVE | IN_MOVE_SELF;
#endif
#if WATCH_MODIFY
    mask |= IN_MODIFY;
#endif
    HASH_FIND_INT(dict, &iev->wd, p);
    if (!p) {
        loge("can not find fd=%d in hash table\n", iev->wd);
        return -1;
    }
    memset(full_path, 0, sizeof(full_path));
    sprintf(full_path, "%s/%s", p->path, iev->name);
    logd("fw_update_watch iev->mask = %p, path=%s\n", iev->mask, full_path);
    if (iev->mask & IN_CREATE) {
        if (iev->mask & IN_ISDIR) {
            logd("create dir %s\n", full_path);
            fw_add_watch_recursive(fd, full_path);
        } else {
            logd("create file %s\n", full_path);
            fw_add_watch(fd, full_path, mask);
        }
    } else if (iev->mask & IN_DELETE) {
        if (iev->mask & IN_ISDIR) {
            logd("delete dir %s\n", full_path);
            fw_del_watch_recursive(fd, full_path);
        } else {
            logd("delete file %s\n", full_path);
            fw_del_watch(fd, full_path);
        }
    } else if (iev->mask & IN_MOVED_FROM){
        if (iev->mask & IN_ISDIR) {
            logi("dir moved_from %s\n", full_path);
            fw_del_watch_recursive(fd, full_path);
        } else {
            logi("file moved_from %s\n", full_path);
            fw_del_watch(fd, full_path);
        }
    } else if (iev->mask & IN_MOVED_TO){
        if (iev->mask & IN_ISDIR) {
            logi("dir moved_to %s\n", full_path);
            fw_add_watch_recursive(fd, full_path);
        } else {
            logi("file moved_to %s\n", full_path);
            fw_add_watch(fd, full_path, mask);
        }
    } else if (iev->mask & IN_IGNORED){
        logi("ignored, file watch was removed\n");
    } else {
        logi("unknown inotify_event:%p\n", iev->mask);
    }
    return 0;
}

int fw_init()
{
    int ifd = inotify_init();
    if (ifd == -1) {
        loge("inotify_init failed: %d\n", errno);
        return -1;
    }
    g_evbase = gevent_base_create();
    if (!g_evbase) {
        loge("gevent_base_create failed\n");
        return -1;
    }
    return ifd;
}

void on_read_ops(int fd, void *arg)
{
    int i, len;
    char ibuf[INOTIFYEVENTBUFSIZE] __attribute__ ((aligned(4))) = {0};
    struct inotify_event *iev;

    logv("on_read_ops\n");
retry:
    len = read(fd, ibuf, INOTIFYEVENTBUFSIZE);
    if (len == 0) {
        loge("read inofity event buffer error: %s\n", strerror(errno));
    } else if (len == -1) {
        loge("read inofity event buffer error: %s\n", strerror(errno));
        if (errno == EINTR) goto retry;
    } else {
        i = 0;
        while (i < len) {
            iev = (struct inotify_event *)(ibuf + i);
            fw_update_watch(fd, iev);
            i += sizeof(struct inotify_event) + iev->len;
        }
    }
}

int fw_dispatch()
{
    struct gevent *ev;
    struct gevent_cbs *evcb = (struct gevent_cbs *)calloc(1, sizeof(struct gevent_cbs));
    evcb->ev_in = on_read_ops;
    evcb->ev_out = NULL;
    evcb->ev_err = NULL;
    ev = gevent_create(g_ifd, on_read_ops, NULL, NULL, NULL);
    gevent_add(g_evbase, ev);
    logi("file watcher start running...\n");
    gevent_base_loop(g_evbase);
    return 0;
}

void ctrl_c_op(int signo)
{
    wd_path_t *user;
    for (user = dict; user != NULL; user = (wd_path_t *) (user->hh.next)) {
        logd("user %d, path %s\n", user->wd, user->path);
        inotify_rm_watch(g_ifd, user->wd);
        free(user);
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
            log_init(LOG_FILE, NULL);
        }
    } else {
        log_init(LOG_STDERR, NULL);
    }
    log_set_level(LOG_INFO);
    signal(SIGINT , ctrl_c_op);
    g_ifd = fw_init();
    if (g_ifd == -1) {
        loge("fw_init failed!\n");
        return -1;
    }
    fw_add_watch_recursive(g_ifd, FTP_ROOT_DIR);
    fw_dispatch();
    return 0;
}
