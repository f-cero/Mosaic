/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic Studio — small POSIX/Windows shims. The engine core is plain
 * C99 with no platform calls; only the editor needs these. */
#ifndef MOSAIC_STUDIO_PLATFORM_H
#define MOSAIC_STUDIO_PLATFORM_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <direct.h>
#  include <io.h>
#  define MS_POPEN  _popen
#  define MS_PCLOSE _pclose
#  define MS_CHDIR   _chdir
#  define MS_GETCWD  _getcwd
#  define MS_MKDIR(p) _mkdir(p)
#else
#  include <unistd.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  define MS_POPEN  popen
#  define MS_PCLOSE pclose
#  define MS_CHDIR   chdir
#  define MS_GETCWD  getcwd
#  define MS_MKDIR(p) mkdir((p), 0777)
#endif

static int ms_mkdir1(const char *path)
{
    if (!path || !path[0]) return -1;
    MS_MKDIR(path);
    return 0;
}

/* mkdir -p for the "<dir>/build" form the Studio uses. */
static void ms_mkdir_p(const char *path)
{
    char tmp[512];
    const size_t n = strlen(path);
    if (n >= sizeof(tmp)) return;
    memcpy(tmp, path, n + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            const char c = *p;
            *p = 0;
            ms_mkdir1(tmp);
            *p = c;
        }
    }
    ms_mkdir1(tmp);
}

#endif
