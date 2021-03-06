#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>

#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#include <pwd.h>
#include <grp.h>

#include <linux/kdev_t.h>
#include <limits.h>

// dynamic arrays
typedef struct {
    int count;
    int capacity;
    void** items;
} dynarray_t;

#define DYNARRAY_INITIALIZER  { 0, 0, NULL }

static void dynarray_init( dynarray_t *a )
{
    a->count = a->capacity = 0;
    a->items = NULL;
}

static void dynarray_reserve_more( dynarray_t *a, int count )
{
    int old_cap = a->capacity;
    int new_cap = old_cap;
    const int max_cap = INT_MAX/sizeof(void*);
    void** new_items;
    int new_count = a->count + count;

    if (count <= 0)
        return;

    if (count > max_cap - a->count)
        abort();

    new_count = a->count + count;

    while (new_cap < new_count) {
        old_cap = new_cap;
        new_cap += (new_cap >> 2) + 4;
        if (new_cap < old_cap || new_cap > max_cap) {
            new_cap = max_cap;
        }
    }
    new_items = realloc(a->items, new_cap*sizeof(void*));
    if (new_items == NULL)
        abort();

    a->items = new_items;
    a->capacity = new_cap;
}

static void dynarray_append( dynarray_t *a, void* item )
{
    if (a->count >= a->capacity)
        dynarray_reserve_more(a, 1);

    a->items[a->count++] = item;
}

static void dynarray_done( dynarray_t *a )
{
    free(a->items);
    a->items = NULL;
    a->count = a->capacity = 0;
}

#define DYNARRAY_FOREACH_TYPE(_array,_item_type,_item,_stmnt) \
    do { \
        int _nn_##__LINE__ = 0; \
        for (;_nn_##__LINE__ < (_array)->count; ++ _nn_##__LINE__) { \
            _item_type _item = (_item_type)(_array)->items[_nn_##__LINE__]; \
            _stmnt; \
        } \
    } while (0)

#define DYNARRAY_FOREACH(_array,_item,_stmnt) \
    DYNARRAY_FOREACH_TYPE(_array,void *,_item,_stmnt)

// string arrays

typedef dynarray_t  strlist_t;

#define  STRLIST_INITIALIZER  DYNARRAY_INITIALIZER

#define  STRLIST_FOREACH(_list,_string,_stmnt) \
    DYNARRAY_FOREACH_TYPE(_list,char *,_string,_stmnt)

static void strlist_init( strlist_t *list )
{
    dynarray_init(list);
}

static void strlist_append_b( strlist_t *list, const void* str, size_t  slen )
{
    char *copy = malloc(slen+1);
    memcpy(copy, str, slen);
    copy[slen] = '\0';
    dynarray_append(list, copy);
}

static void strlist_append_dup( strlist_t *list, const char *str)
{
    strlist_append_b(list, str, strlen(str));
}

static void strlist_done( strlist_t *list )
{
    STRLIST_FOREACH(list, string, free(string));
    dynarray_done(list);
}

static int strlist_compare_strings(const void* a, const void* b)
{
    const char *sa = *(const char **)a;
    const char *sb = *(const char **)b;
    return strcmp(sa, sb);
}

static void strlist_sort( strlist_t *list )
{
    if (list->count > 0) {
        qsort(list->items, 
              (size_t)list->count,
              sizeof(void*),
              strlist_compare_strings);
    }
}

// bits for flags argument
#define LIST_LONG           (1 << 0)
#define LIST_ALL            (1 << 1)
#define LIST_RECURSIVE      (1 << 2)
#define LIST_DIRECTORIES    (1 << 3)
#define LIST_SIZE           (1 << 4)

// fwd
static int listpath(const char *name, int flags);

static char mode2kind(unsigned mode)
{
    switch(mode & S_IFMT){
    case S_IFSOCK: return 's';
    case S_IFLNK: return 'l';
    case S_IFREG: return '-';
    case S_IFDIR: return 'd';
    case S_IFBLK: return 'b';
    case S_IFCHR: return 'c';
    case S_IFIFO: return 'p';
    default: return '?';
    }
}

static void mode2str(unsigned mode, char *out)
{
    *out++ = mode2kind(mode);
    
    *out++ = (mode & 0400) ? 'r' : '-';
    *out++ = (mode & 0200) ? 'w' : '-';
    if(mode & 04000) {
        *out++ = (mode & 0100) ? 's' : 'S';
    } else {
        *out++ = (mode & 0100) ? 'x' : '-';
    }
    *out++ = (mode & 040) ? 'r' : '-';
    *out++ = (mode & 020) ? 'w' : '-';
    if(mode & 02000) {
        *out++ = (mode & 010) ? 's' : 'S';
    } else {
        *out++ = (mode & 010) ? 'x' : '-';
    }
    *out++ = (mode & 04) ? 'r' : '-';
    *out++ = (mode & 02) ? 'w' : '-';
    if(mode & 01000) {
        *out++ = (mode & 01) ? 't' : 'T';
    } else {
        *out++ = (mode & 01) ? 'x' : '-';
    }
    *out = 0;
}

static void user2str(unsigned uid, char *out)
{
    struct passwd *pw = getpwuid(uid);
    if(pw) {
        strcpy(out, pw->pw_name);
    } else {
        sprintf(out, "%d", uid);
    }
}

static void group2str(unsigned gid, char *out)
{
    struct group *gr = getgrgid(gid);
    if(gr) {
        strcpy(out, gr->gr_name);
    } else {
        sprintf(out, "%d", gid);
    }
}

static int show_total_size(const char *dirname, DIR *d, int flags)
{
    struct dirent *de;
    char tmp[1024];
    struct stat s;
    int sum = 0;

    /* run through the directory and sum up the file block sizes */
    while ((de = readdir(d)) != 0) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (de->d_name[0] == '.' && (flags & LIST_ALL) == 0)
            continue;

        if (strcmp(dirname, "/") == 0)
            snprintf(tmp, sizeof(tmp), "/%s", de->d_name);
        else
            snprintf(tmp, sizeof(tmp), "%s/%s", dirname, de->d_name);

        if (lstat(tmp, &s) < 0) {
            fprintf(stderr, "stat failed on %s: %s\n", tmp, strerror(errno));
            rewinddir(d);
            return -1;
        }

        sum += s.st_blocks / 2;
    }

    printf("total %d\n", sum);
    rewinddir(d);
    return 0;
}

static int listfile_size(const char *path, const char *filename, int flags)
{
    struct stat s;

    if (lstat(path, &s) < 0) {
        fprintf(stderr, "lstat '%s' failed: %s\n", path, strerror(errno));
        return -1;
    }

    /* blocks are 512 bytes, we want output to be KB */
    printf("%lld %s\n", s.st_blocks / 2, filename);
    return 0;
}

static int listfile_long(const char *path, int flags)
{
    struct stat s;
    char date[32];
    char mode[16];
    char user[16];
    char group[16];
    const char *name;

    /* name is anything after the final '/', or the whole path if none*/
    name = strrchr(path, '/');
    if(name == 0) {
        name = path;
    } else {
        name++;
    }

    if(lstat(path, &s) < 0) {
        return -1;
    }

    mode2str(s.st_mode, mode);
    user2str(s.st_uid, user);
    group2str(s.st_gid, group);

    strftime(date, 32, "%Y-%m-%d %H:%M", localtime((const time_t*)&s.st_mtime));
    date[31] = 0;
    
// 12345678901234567890123456789012345678901234567890123456789012345678901234567890
// MMMMMMMM UUUUUUUU GGGGGGGGG XXXXXXXX YYYY-MM-DD HH:MM NAME (->LINK)

    switch(s.st_mode & S_IFMT) {
    case S_IFBLK:
    case S_IFCHR:
        printf("%s %-8s %-8s %3d, %3d %s %s\n",
               mode, user, group, 
               (int) MAJOR(s.st_rdev), (int) MINOR(s.st_rdev),
               date, name);
        break;
    case S_IFREG:
        printf("%s %-8s %-8s %8d %s %s\n",
               mode, user, group, (int) s.st_size, date, name);
        break;
    case S_IFLNK: {
        char linkto[256];
        int len;

        len = readlink(path, linkto, 256);
        if(len < 0) return -1;
        
        if(len > 255) {
            linkto[252] = '.';
            linkto[253] = '.';
            linkto[254] = '.';
            linkto[255] = 0;
        } else {
            linkto[len] = 0;
        }
        
        printf("%s %-8s %-8s          %s %s -> %s\n",
               mode, user, group, date, name, linkto);
        break;
    }
    default:
        printf("%s %-8s %-8s          %s %s\n",
               mode, user, group, date, name);

    }
    return 0;
}

static int listfile(const char *dirname, const char *filename, int flags)
{
    if ((flags & (LIST_LONG | LIST_SIZE)) == 0) {
        printf("%s\n", filename);
        return 0;
    }

    char tmp[4096];
    const char* pathname = filename;

    if (dirname != NULL) {
        snprintf(tmp, sizeof(tmp), "%s/%s", dirname, filename);
        pathname = tmp;
    } else {
        pathname = filename;
    }

    if ((flags & LIST_LONG) != 0) {
        return listfile_long(pathname, flags);
    } else /*((flags & LIST_SIZE) != 0)*/ {
        return listfile_size(pathname, filename, flags);
    }
}

static int listdir(const char *name, int flags)
{
    char tmp[4096];
    DIR *d;
    struct dirent *de;
    strlist_t  files = STRLIST_INITIALIZER;
    
    d = opendir(name);
    if(d == 0) {
        fprintf(stderr, "opendir failed, %s\n", strerror(errno));
        return -1;
    }

    if ((flags & LIST_SIZE) != 0) {
        show_total_size(name, d, flags);
    }

    while((de = readdir(d)) != 0){
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if(de->d_name[0] == '.' && (flags & LIST_ALL) == 0) continue;

        strlist_append_dup(&files, de->d_name);
    }

    strlist_sort(&files);
    STRLIST_FOREACH(&files, filename, listfile(name, filename, flags));
    strlist_done(&files);

    if (flags & LIST_RECURSIVE) {
        strlist_t subdirs = STRLIST_INITIALIZER;

        rewinddir(d);

        while ((de = readdir(d)) != 0) {
            struct stat s;
            int err;

            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                continue;
            if (de->d_name[0] == '.' && (flags & LIST_ALL) == 0)
                continue;

            if (!strcmp(name, "/"))
                snprintf(tmp, sizeof(tmp), "/%s", de->d_name);
            else
                snprintf(tmp, sizeof(tmp), "%s/%s", name, de->d_name);

            /*
             * If the name ends in a '/', use stat() so we treat it like a
             * directory even if it's a symlink.
             */
            if (tmp[strlen(tmp)-1] == '/')
                err = stat(tmp, &s);
            else
                err = lstat(tmp, &s);

            if (err < 0) {
                perror(tmp);
                closedir(d);
                return -1;
            }

            if (S_ISDIR(s.st_mode)) {
                strlist_append_dup(&subdirs, tmp);
            }
        }
        strlist_sort(&subdirs);
        STRLIST_FOREACH(&subdirs, path, {
            printf("\n%s:\n", path);
            listdir(path, flags);
        });
        strlist_done(&subdirs);
    }

    closedir(d);
    return 0;
}

static int listpath(const char *name, int flags)
{
    struct stat s;
    int err;

    /*
     * If the name ends in a '/', use stat() so we treat it like a
     * directory even if it's a symlink.
     */
    if (name[strlen(name)-1] == '/')
        err = stat(name, &s);
    else
        err = lstat(name, &s);

    if (err < 0) {
        perror(name);
        return -1;
    }

    if ((flags & LIST_DIRECTORIES) == 0 && S_ISDIR(s.st_mode)) {
        if (flags & LIST_RECURSIVE)
            printf("\n%s:\n", name);
        return listdir(name, flags);
    } else {
        /* yeah this calls stat() again*/
        return listfile(NULL, name, flags);
    }
}

int ls_main(int argc, char **argv)
{
    int flags = 0;
    int listed = 0;
    
    if(argc > 1) {
        int i;
        int err = 0;
        strlist_t  files = STRLIST_INITIALIZER;

        for (i = 1; i < argc; i++) {
            if (argv[i][0] == '-') {
                /* an option ? */
                const char *arg = argv[i]+1;
                while (arg[0]) {
                    switch (arg[0]) {
                    case 'l': flags |= LIST_LONG; break;
                    case 's': flags |= LIST_SIZE; break;
                    case 'R': flags |= LIST_RECURSIVE; break;
                    case 'd': flags |= LIST_DIRECTORIES; break;
                    case 'a': flags |= LIST_ALL; break;
                    default:
                        fprintf(stderr, "%s: Unknown option '-%c'. Aborting.\n", "ls", arg[0]);
                        exit(1);
                    }
                    arg++;
                }
            } else {
                /* not an option ? */
                strlist_append_dup(&files, argv[i]);
            }
        }

        if (files.count > 0) {
            STRLIST_FOREACH(&files, path, {
                if (listpath(path, flags) != 0) {
                    err = EXIT_FAILURE;
                }
            });
            strlist_done(&files);
            return err;
        }
    }
    
    // list working directory if no files or directories were specified    
    return listpath(".", flags);
}
