/* SPDX-License-Identifier: LGPL-2.1+ */
/***
  This file is part of systemd.

  Copyright 2014 Zbigniew Jędrzejewski-Szmek
***/

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "alloc-util.h"
#include "conf-files.h"
#include "dirent-util.h"
#include "dropin.h"
#include "escape.h"
#include "fd-util.h"
#include "fileio-label.h"
#include "fs-util.h"
#include "hashmap.h"
#include "log.h"
#include "macro.h"
#include "mkdir.h"
#include "path-util.h"
#include "set.h"
#include "string-util.h"
#include "strv.h"
#include "unit-name.h"

int drop_in_file(const char *dir, const char *unit, unsigned level,
                 const char *name, char **_p, char **_q) {

        char prefix[DECIMAL_STR_MAX(unsigned)];
        _cleanup_free_ char *b = NULL;
        char *p, *q;

        assert(unit);
        assert(name);
        assert(_p);
        assert(_q);

        sprintf(prefix, "%u", level);

        b = xescape(name, "/.");
        if (!b)
                return -ENOMEM;

        if (!filename_is_valid(b))
                return -EINVAL;

        p = strjoin(dir, "/", unit, ".d");
        if (!p)
                return -ENOMEM;

        q = strjoin(p, "/", prefix, "-", b, ".conf");
        if (!q) {
                free(p);
                return -ENOMEM;
        }

        *_p = p;
        *_q = q;
        return 0;
}

int write_drop_in(const char *dir, const char *unit, unsigned level,
                  const char *name, const char *data) {

        _cleanup_free_ char *p = NULL, *q = NULL;
        int r;

        assert(dir);
        assert(unit);
        assert(name);
        assert(data);

        r = drop_in_file(dir, unit, level, name, &p, &q);
        if (r < 0)
                return r;

        (void) mkdir_p(p, 0755);
        return write_string_file_atomic_label(q, data);
}

int write_drop_in_format(const char *dir, const char *unit, unsigned level,
                         const char *name, const char *format, ...) {
        _cleanup_free_ char *p = NULL;
        va_list ap;
        int r;

        assert(dir);
        assert(unit);
        assert(name);
        assert(format);

        va_start(ap, format);
        r = vasprintf(&p, format, ap);
        va_end(ap);

        if (r < 0)
                return -ENOMEM;

        return write_drop_in(dir, unit, level, name, p);
}

static int unit_file_find_dir(
                const char *original_root,
                const char *path,
                char ***dirs) {

        _cleanup_free_ char *chased = NULL;
        int r;

        assert(path);

        r = chase_symlinks(path, original_root, 0, &chased);
        if (r == -ENOENT) /* Ignore -ENOENT, after all most units won't have a drop-in dir. */
                return 0;
        if (r == -ENAMETOOLONG) {
                /* Also, ignore -ENAMETOOLONG but log about it. After all, users are not even able to create the
                 * drop-in dir in such case. This mostly happens for device units with an overly long /sys path. */
                log_debug_errno(r, "Path '%s' too long, couldn't canonicalize, ignoring.", path);
                return 0;
        }
        if (r < 0)
                return log_warning_errno(r, "Failed to canonicalize path '%s': %m", path);

        r = strv_push(dirs, chased);
        if (r < 0)
                return log_oom();

        chased = NULL;
        return 0;
}

static int unit_file_find_dirs(
                const char *original_root,
                Set *unit_path_cache,
                const char *unit_path,
                const char *name,
                const char *suffix,
                char ***dirs) {

        char *path;
        int r;

        assert(unit_path);
        assert(name);
        assert(suffix);

        path = strjoina(unit_path, "/", name, suffix);

        if (!unit_path_cache || set_get(unit_path_cache, path)) {
                r = unit_file_find_dir(original_root, path, dirs);
                if (r < 0)
                        return r;
        }

        if (unit_name_is_valid(name, UNIT_NAME_INSTANCE)) {
                /* Also try the template dir */

                _cleanup_free_ char *template = NULL;

                r = unit_name_template(name, &template);
                if (r < 0)
                        return log_error_errno(r, "Failed to generate template from unit name: %m");

                return unit_file_find_dirs(original_root, unit_path_cache, unit_path, template, suffix, dirs);
        }

        return 0;
}

int unit_file_find_dropin_paths(
                const char *original_root,
                char **lookup_path,
                Set *unit_path_cache,
                const char *dir_suffix,
                const char *file_suffix,
                Set *names,
                char ***ret) {

        _cleanup_strv_free_ char **dirs = NULL;
        Iterator i;
        char *t, **p;
        int r;

        assert(ret);

        SET_FOREACH(t, names, i)
                STRV_FOREACH(p, lookup_path)
                        unit_file_find_dirs(original_root, unit_path_cache, *p, t, dir_suffix, &dirs);

        if (strv_isempty(dirs)) {
                *ret = NULL;
                return 0;
        }

        r = conf_files_list_strv(ret, file_suffix, NULL, 0, (const char**) dirs);
        if (r < 0)
                return log_warning_errno(r, "Failed to create the list of configuration files: %m");

        return 1;
}
