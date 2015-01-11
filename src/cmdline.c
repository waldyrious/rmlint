/*
 *  This file is part of rmlint.
 *
 *  rmlint is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  rmlint is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with rmlint.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *
 *  - Christopher <sahib> Pahl 2010-2014 (https://github.com/sahib)
 *  - Daniel <SeeSpotRun> T.   2014-2014 (https://github.com/SeeSpotRun)
 *
 * Hosted on http://github.com/sahib/rmlint
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>

#include <errno.h>
#include <search.h>
#include <sys/time.h>
#include <glib.h>

#include "cmdline.h"
#include "treemerge.h"
#include "traverse.h"
#include "preprocess.h"
#include "shredder.h"
#include "utilities.h"
#include "formats.h"


#define RM_ERROR_DOMAIN (g_quark_from_static_string("rmlint"))


/* exit and return to calling method */
static void rm_cmd_die(RmSession *session, int status) {
    rm_session_clear(session);
    exit(status);
}

static void rm_cmd_show_version(void) {
    fprintf(
        stderr,
        "version %s compiled: %s at [%s] \"%s\" (rev %s)\n",
        RMLINT_VERSION, __DATE__, __TIME__,
        RMLINT_VERSION_NAME,
        RMLINT_VERSION_GIT_REVISION
    );

    struct {
        bool enabled : 1;
        const char * name;
    } features[] = {
        {.name="mounts",      .enabled=HAVE_BLKID & (HAVE_GETMNTENT | HAVE_GETMNTINFO)},
        {.name="nonstripped", .enabled=HAVE_LIBELF},
        {.name="fiemap",      .enabled=HAVE_FIEMAP},
        {.name="sha512",      .enabled=HAVE_SHA512},
        {.name="bigfiles",    .enabled=HAVE_BIGFILES},
        {.name="intl",        .enabled=HAVE_LIBINTL},
        {.name="json-cache",  .enabled=HAVE_JSON_GLIB},
        {.name="xattr",       .enabled=HAVE_XATTR}
    };

    int n_features = sizeof(features) / sizeof(features[0]);

    fprintf(stderr, "compiled with: ");

    for(int i = 0; i < n_features; ++i) {
        if(features[i].enabled) {
            fprintf(stderr, "+%s ", features[i].name);
        } else {
            fprintf(stderr, "-%s ", features[i].name);
        }
    }

    fprintf(stderr, RESET"\n");
    exit(0);
}

static void rm_cmd_show_help(void) {
    static const char *commands[] = {
        "man %s docs/rmlint.1.gz 2> /dev/null",
        "man %s rmlint",
        NULL
    };

    bool found_manpage = false;

    for(int i = 0; commands[i] && !found_manpage; ++i) {
        char *command = g_strdup_printf(commands[i], "-P cat");
        if(system(command) == 0) {
            found_manpage = true;
        }

        g_free(command);
    }

    if(!found_manpage) {
        rm_log_warning_line(_("You seem to have no manpage for rmlint."));
    }

    exit(0);
}

static const struct FormatSpec {
    const char *id;
    unsigned base;
    unsigned exponent;
} SIZE_FORMAT_TABLE[] = {
    /* This list is sorted, so bsearch() can be used */
    {.id = "b"  , .base = 512  , .exponent = 1},
    {.id = "c"  , .base = 1    , .exponent = 1},
    {.id = "e"  , .base = 1000 , .exponent = 6},
    {.id = "eb" , .base = 1024 , .exponent = 6},
    {.id = "g"  , .base = 1000 , .exponent = 3},
    {.id = "gb" , .base = 1024 , .exponent = 3},
    {.id = "k"  , .base = 1000 , .exponent = 1},
    {.id = "kb" , .base = 1024 , .exponent = 1},
    {.id = "m"  , .base = 1000 , .exponent = 2},
    {.id = "mb" , .base = 1024 , .exponent = 2},
    {.id = "p"  , .base = 1000 , .exponent = 5},
    {.id = "pb" , .base = 1024 , .exponent = 5},
    {.id = "t"  , .base = 1000 , .exponent = 4},
    {.id = "tb" , .base = 1024 , .exponent = 4},
    {.id = "w"  , .base = 2    , .exponent = 1}
};

typedef struct FormatSpec FormatSpec;

static const int SIZE_FORMAT_TABLE_N = sizeof(SIZE_FORMAT_TABLE) / sizeof(FormatSpec);

static int rm_cmd_size_format_error(const char **error, const char *msg) {
    if(error) {
        *error = msg;
    }
    return 0;
}

static int rm_cmd_compare_spec_elem(const void *fmt_a, const void *fmt_b) {
    return strcasecmp(((FormatSpec *)fmt_a)->id, ((FormatSpec *)fmt_b)->id);
}

static RmOff rm_cmd_size_string_to_bytes(const char *size_spec, const char **error) {
    if (size_spec == NULL) {
        return rm_cmd_size_format_error(error, _("Input size is empty"));
    }

    char *format = NULL;
    long double decimal = strtold(size_spec, &format);

    if (decimal == 0 && format == size_spec) {
        return rm_cmd_size_format_error(error, _("This does not look like a number"));
    } else if (decimal < 0) {
        return rm_cmd_size_format_error(error, _("Negativ sizes are no good idea"));
    } else if (*format) {
        format = g_strstrip(format);
    } else {
        return round(decimal);
    }

    FormatSpec key = {.id = format};
    FormatSpec *found = bsearch(
                            &key, SIZE_FORMAT_TABLE,
                            SIZE_FORMAT_TABLE_N, sizeof(FormatSpec),
                            rm_cmd_compare_spec_elem
                        );

    if (found != NULL) {
        /* No overflow check */
        return decimal * pow(found->base, found->exponent);
    } else {
        return rm_cmd_size_format_error(error, _("Given format specifier not found"));
    }
}

/* Size spec parsing implemented by qitta (http://github.com/qitta)
 * Thanks and go blame him if this breaks!
 */
static gboolean rm_cmd_size_range_string_to_bytes(const char *range_spec, RmOff *min, RmOff *max, const char **error) {
    *min = 0;
    *max = G_MAXULONG;

    const char *tmp_error = NULL;
    gchar **split = g_strsplit(range_spec, "-", 2);

    if(split[0] != NULL) {
        *min = rm_cmd_size_string_to_bytes(split[0], &tmp_error);
    }

    if(split[1] != NULL && tmp_error == NULL) {
        *max = rm_cmd_size_string_to_bytes(split[1], &tmp_error);
    }

    g_strfreev(split);

    if(*max < *min) {
        tmp_error = _("Max is smaller than min");
    }

    if(error != NULL) {
        *error = tmp_error;
    }

    return (tmp_error == NULL);
}

static gboolean rm_cmd_parse_limit_sizes(
    _U const char *option_name,
    const gchar *range_spec,
    RmSession *session,
    GError **error
) {
    const char *error_msg = NULL;
    if(!rm_cmd_size_range_string_to_bytes(
                range_spec,
                &session->settings->minsize,
                &session->settings->maxsize,
                &error_msg
            )) {
        
        g_set_error(error, RM_ERROR_DOMAIN, 0,  _("cannot parse --limit: %s"), error_msg);
        return false;
    } else {
        session->settings->limits_specified = true;
        return true;
    }
}

static GLogLevelFlags VERBOSITY_TO_LOG_LEVEL[] = {
    [0] = G_LOG_LEVEL_CRITICAL,
    [1] = G_LOG_LEVEL_ERROR,
    [2] = G_LOG_LEVEL_WARNING,
    [3] = G_LOG_LEVEL_MESSAGE | G_LOG_LEVEL_INFO,
    [4] = G_LOG_LEVEL_DEBUG
};

static bool rm_cmd_add_path(RmSession *session, bool is_prefd, int index, const char *path) {
    RmSettings *settings = session->settings;
    if(faccessat(AT_FDCWD, path, R_OK, AT_EACCESS) != 0) {
        rm_log_warning_line(_("Can't open directory or file \"%s\": %s"), path, strerror(errno));
        return FALSE;
    } else {
        settings->is_prefd = g_realloc(settings->is_prefd, sizeof(char) * (index + 1));
        settings->is_prefd[index] = is_prefd;
        settings->paths = g_realloc(settings->paths, sizeof(char *) * (index + 2));

        char *abs_path = realpath(path, NULL);
        settings->paths[index + 0] = abs_path ? abs_path : g_strdup(path);
        settings->paths[index + 1] = NULL;
        return TRUE;
    }
}

static int rm_cmd_read_paths_from_stdin(RmSession *session,  bool is_prefd, int index) {
    int paths_added = 0;
    char path_buf[PATH_MAX];

    while(fgets(path_buf, PATH_MAX, stdin)) {
        paths_added += rm_cmd_add_path(session, is_prefd, index + paths_added, strtok(path_buf, "\n"));
    }

    return paths_added;
}

static bool rm_cmd_parse_output_pair(RmSession *session, const char *pair, GError **error) {
    g_assert(session);
    g_assert(pair);

    char *separator = strchr(pair, ':');
    char *full_path = NULL;
    char format_name[100];
    memset(format_name, 0, sizeof(format_name));

    if(separator == NULL) {
        /* default to stdout */
        full_path = "stdout";
        strncpy(format_name, pair, strlen(pair));
    } else {
        full_path = separator + 1;
        strncpy(format_name, pair, MIN((long)sizeof(format_name), separator - pair));
    }

    if(!rm_fmt_add(session->formats, format_name, full_path)) {
        g_set_error(error, RM_ERROR_DOMAIN, 0, _("Adding -o %s as output failed."), pair);
        return false;
    }

    return true;
}

static bool rm_cmd_parse_config_pair(RmSession *session, const char *pair) {
    char *domain = strchr(pair, ':');
    if(domain == NULL) {
        rm_log_warning_line(_("No format (format:key[=val]) specified in '%s'."), pair);
        return false;
    }

    char *key = NULL, *value = NULL;
    char **key_val = g_strsplit(&domain[1], "=", 2);
    int len = g_strv_length(key_val);

    if(len < 1) {
        rm_log_warning_line(_("Missing key (format:key[=val]) in '%s'."), pair);
        g_strfreev(key_val);
        return false;
    }

    key = g_strdup(key_val[0]);
    if(len == 2) {
        value = g_strdup(key_val[1]);
    } else {
        value = g_strdup("1");
    }

    char *formatter = g_strndup(pair, domain - pair);
    rm_fmt_set_config_value(session->formats, formatter, key, value);
    g_free(formatter);

    g_strfreev(key_val);
    return true;
}

static gboolean rm_cmd_parse_config(
        _U const char *option_name,
        const char *pair,
        RmSession *session,
        _U GError **error
) {
    /* rm_cmd_parse_config_pair() may warn but never fail */
    rm_cmd_parse_config_pair(session, pair);
    return true;
}

static double rm_cmd_parse_clamp_factor(RmSession *session, const char *string) {
    char *error_loc = NULL;
    gdouble factor = g_strtod(string, &error_loc);

    if(error_loc != NULL && *error_loc != '\0' && *error_loc != '%') {
        rm_log_error_line(
            _("Unable to parse factor \"%s\": error begins at %s"),
            string, error_loc
        );
        rm_cmd_die(session, EXIT_FAILURE);
    }

    if(error_loc != NULL && *error_loc == '%') {
        factor /= 100;
    }

    if(0 > factor || factor > 1) {
        rm_log_error_line(
            _("factor value is not in range [0-1]: %f"),
            factor
        );
        rm_cmd_die(session, EXIT_FAILURE);
    }

    return factor;
}

static RmOff rm_cmd_parse_clamp_offset(RmSession *session, const char *string) {
    const char *error_msg = NULL;
    RmOff offset = rm_cmd_size_string_to_bytes(string, &error_msg);

    if(error_msg != NULL) {
        rm_log_error_line(
            _("Unable to parse offset \"%s\": %s"),
            string, error_msg
        );
        rm_cmd_die(session, EXIT_FAILURE);
    }

    return offset;
}

static void rm_cmd_parse_clamp_option(RmSession *session, const char *string, bool start_or_end) {
    if(strchr(string, '.') || g_str_has_suffix(string, "%")) {
        gdouble factor = rm_cmd_parse_clamp_factor(session, string);
        if(start_or_end) {
            session->settings->use_absolute_start_offset = false;
            session->settings->skip_start_factor = factor;
        } else {
            session->settings->use_absolute_end_offset = false;
            session->settings->skip_end_factor = factor;
        }
    } else {
        RmOff offset = rm_cmd_parse_clamp_offset(session, string);
        if(start_or_end) {
            session->settings->use_absolute_start_offset = true;
            session->settings->skip_start_offset = offset;
        } else {
            session->settings->use_absolute_end_offset = true;
            session->settings->skip_end_offset = offset;
        }
    }
}

/* parse comma-separated strong of lint types and set settings accordingly */
typedef struct RmLintTypeOption {
    const char **names;
    gboolean **enable;
} RmLintTypeOption;

/* compare function for parsing lint type arguments */
int rm_cmd_find_line_type_func(const void *v_input, const void *v_option) {
    const char *input = v_input;
    const RmLintTypeOption *option = v_option;

    for(int i = 0; option->names[i]; ++i) {
        if(strcmp(option->names[i], input) == 0) {
            return 0;
        }
    }
    return 1;
}

#define OPTS  (gboolean *[])
#define NAMES (const char *[])

static char rm_cmd_find_lint_types_sep(const char *lint_string) {
    if(*lint_string == '+' || *lint_string == '-') {
        lint_string++;
    }

    while(isalpha(*lint_string)) {
        lint_string++;
    }

    return *lint_string;
}

static gboolean rm_cmd_parse_lint_types(
        _U const char *option_name,
        const char *lint_string,
        RmSession *session,
        _U GError **error
) {
    RmSettings *settings = session->settings;

    RmLintTypeOption option_table[] = {{
            .names = NAMES{"all", 0},
            .enable = OPTS{
                &settings->findbadids,
                &settings->findbadlinks,
                &settings->findemptydirs,
                &settings->listemptyfiles,
                &settings->nonstripped,
                &settings->searchdup,
                &settings->merge_directories,
                0
            }
        }, {
            .names = NAMES{"minimal", 0},
            .enable = OPTS{
                &settings->findbadids,
                &settings->findbadlinks,
                &settings->searchdup,
                0
            },
        }, {
            .names = NAMES{"minimaldirs", 0},
            .enable = OPTS{
                &settings->findbadids,
                &settings->findbadlinks,
                &settings->merge_directories,
                0
            },
        }, {
            .names = NAMES{"defaults", 0},
            .enable = OPTS{
                &settings->findbadids,
                &settings->findbadlinks,
                &settings->findemptydirs,
                &settings->listemptyfiles,
                &settings->searchdup,
                0
            },
        }, {
            .names = NAMES{"none", 0},
            .enable = OPTS{0},
        }, {
            .names = NAMES{"badids", "bi", 0},
            .enable = OPTS{&settings->findbadids, 0}
        }, {
            .names = NAMES{"badlinks", "bl", 0},
            .enable = OPTS{&settings->findbadlinks, 0}
        }, {
            .names = NAMES{"emptydirs", "ed", 0},
            .enable = OPTS{&settings->findemptydirs, 0}
        }, {
            .names = NAMES{"emptyfiles", "ef", 0},
            .enable = OPTS{&settings->listemptyfiles, 0}
        }, {
            .names = NAMES{"nonstripped", "ns", 0},
            .enable = OPTS{&settings->nonstripped, 0}
        }, {
            .names = NAMES{"duplicates", "df", "dupes", 0},
            .enable = OPTS{&settings->searchdup, 0}
        }, {
            .names = NAMES{"duplicatedirs", "dd", "dupedirs", 0},
            .enable = OPTS{&settings->merge_directories, 0}
        }
    };

    RmLintTypeOption *all_opts = &option_table[0];

    /* split the comma-separates list of options */
    char lint_sep[2] = {0, 0};
    lint_sep[0] = rm_cmd_find_lint_types_sep(lint_string);
    if(lint_sep[0] == 0) {
        lint_sep[0] = ',';
    }

    char **lint_types = g_strsplit(lint_string, lint_sep, -1);

    /* iterate over the separated option strings */
    for(int index = 0; lint_types[index]; index++) {
        char *lint_type = lint_types[index];
        char sign = 0;

        if(*lint_type == '+') {
            sign = +1;
        } else if(*lint_type == '-') {
            sign = -1;
        }

        if(index > 0 && sign == 0) {
            rm_log_warning(_("lint types after first should be prefixed with '+' or '-'"));
            rm_log_warning(_("or they would over-ride previously set options: [%s]"), lint_type);
            continue;
        } else {
            lint_type += ABS(sign);
        }

        /* use lfind to find matching option from array */
        size_t elems = sizeof(option_table) / sizeof(RmLintTypeOption);
        RmLintTypeOption *option = lfind(
                                       lint_type, &option_table,
                                       &elems, sizeof(RmLintTypeOption),
                                       rm_cmd_find_line_type_func
                                   );

        /* apply the found option */
        if(option == NULL) {
            rm_log_warning(_("lint type '%s' not recognised"), lint_type);
            continue;
        }

        if (sign == 0) {
            /* not a + or - option - reset all options to off */
            for (int i = 0; all_opts->enable[i]; i++) {
                *all_opts->enable[i] = false;
            }
        }

        /* enable options as appropriate */
        for(int i = 0; option->enable[i]; i++) {
            *option->enable[i] = (sign == -1) ? false : true;
        }
    }

    if(settings->merge_directories) {
        settings->ignore_hidden = false;
        settings->find_hardlinked_dupes = true;
    }

    /* clean up */
    g_strfreev(lint_types);
    return true;
}

static bool rm_cmd_timestamp_is_plain(const char *stamp) {
    return strchr(stamp, 'T') ? false : true;
}

static gboolean rm_cmd_parse_timestamp(
    _U const char *option_name, const gchar *string, RmSession *session, GError **error
) {
    time_t result = 0;
    bool plain = rm_cmd_timestamp_is_plain(string);
    session->settings->filter_mtime = false;

    tzset();

    if(plain) {
        /* A simple integer is expected, just parse it as time_t */
        result = strtoll(string, NULL, 10);
    } else {
        /* Parse ISO8601 timestamps like 2006-02-03T16:45:09.000Z */
        result = rm_iso8601_parse(string);

        /* debug */
        {
            char time_buf[256];
            memset(time_buf, 0, sizeof(time_buf));
            rm_iso8601_format(time(NULL), time_buf, sizeof(time_buf));
            rm_log_debug("timestamp %ld understood as %s\n", result, time_buf);
        }
    }

    if(result <= 0) {
        g_set_error(
            error, RM_ERROR_DOMAIN, 0,
            _("Unable to parse time spec \"%s\""), string
        );
        return false;
    }

    /* Some sort of success. */
    session->settings->filter_mtime = true;

    if(result > time(NULL)) {
        /* Not critical, maybe there are some uses for this,
         * but print at least a small warning as indication.
         * */
        if(plain) {
            rm_log_warning_line(
                _("-n %lu is newer than current time (%lu)."),
                (long)result, (long)time(NULL)
            );
        } else {
            char time_buf[256];
            memset(time_buf, 0, sizeof(time_buf));
            rm_iso8601_format(time(NULL), time_buf, sizeof(time_buf));

            rm_log_warning_line(
                "-N %s is newer than current time (%s).",
                string, time_buf
            );
        }
    }

    /* Remember our result */
    session->settings->min_mtime = result;
    return true;
}

static gboolean rm_cmd_parse_timestamp_file(
    const char *option_name, const gchar *timestamp_path, RmSession *session, GError **error
) {
    bool plain = true, success = false;
    FILE *stamp_file = fopen(timestamp_path, "r");

    /* Assume failure */
    session->settings->filter_mtime = false;

    if(stamp_file) {
        char stamp_buf[1024];
        memset(stamp_buf, 0, sizeof(stamp_buf));

        if(fgets(stamp_buf, sizeof(stamp_buf), stamp_file) != NULL) {
            success = rm_cmd_parse_timestamp(
                option_name, g_strstrip(stamp_buf), session, error
            );
            plain = rm_cmd_timestamp_is_plain(stamp_buf);
        }

        fclose(stamp_file);
    } else {
        /* Cannot read... */
        plain = false;
    }

    if(!success) {
        return false;
    }

    rm_fmt_add(session->formats, "stamp", timestamp_path);
    if(!plain) {
        /* Enable iso8601 timestamp output */
        rm_fmt_set_config_value(
            session->formats, "stamp", g_strdup("iso8601"), g_strdup("true")
        );
    }

    return success;
}

static void rm_cmd_set_verbosity_from_cnt(RmSettings *settings, int verbosity_counter) {
    settings->verbosity = VERBOSITY_TO_LOG_LEVEL[CLAMP(
                              verbosity_counter,
                              0,
                              (int)(sizeof(VERBOSITY_TO_LOG_LEVEL) / sizeof(GLogLevelFlags)) - 1
                          )];
}

static void rm_cmd_set_paranoia_from_cnt(RmSettings *settings, int paranoia_counter, GError **error) {
    /* Handle the paranoia option */
    switch(paranoia_counter) {
    case -2:
        settings->checksum_type = RM_DIGEST_SPOOKY32;
        break;
    case -1:
        settings->checksum_type = RM_DIGEST_SPOOKY64;
        break;
    case 0:
        /* leave users choice of -a (default) */
        break;
    case 1:
        settings->checksum_type = RM_DIGEST_BASTARD;
        break;
    case 2:
#if HAVE_SHA512
        settings->checksum_type = RM_DIGEST_SHA512;
#else
        settings->checksum_type = RM_DIGEST_SHA256;
#endif
        break;
    case 3:
        settings->checksum_type = RM_DIGEST_PARANOID;
        break;
    default:
        g_set_error(error, RM_ERROR_DOMAIN, 0, _("Only up to -ppp or down to -P flags allowed."));
        break;
    }
}

static void rm_cmd_on_error(_U GOptionContext *context, _U GOptionGroup *group, RmSession *session, GError **error) {
    if(error != NULL) {
        rm_log_error_line("%s.", (*error)->message);
        rm_cmd_die(session, EXIT_FAILURE);
    }
}

static gboolean rm_cmd_parse_algorithm(
    _U const char *option_name,
    const gchar *value,
    RmSession *session,
    GError **error
) {
    RmSettings *settings = session->settings;
    settings->checksum_type = rm_string_to_digest_type(value);

    if(settings->checksum_type == RM_DIGEST_UNKNOWN) {
        g_set_error(error, RM_ERROR_DOMAIN, 0, _("Unknown hash algorithm: '%s'"), value);
    } else if(settings->checksum_type == RM_DIGEST_BASTARD) {
        session->hash_seed1 = time(NULL) * (GPOINTER_TO_UINT(session));
        session->hash_seed2 = GPOINTER_TO_UINT(&session);
    }

    return true;
}

static gboolean rm_cmd_parse_small_output(
    _U const char *option_name, const gchar *output_pair, RmSession *session, _U GError **error
) {
    session->output_cnt[0] = MAX(session->output_cnt[0], 0);
    session->output_cnt[0] += rm_cmd_parse_output_pair(session, output_pair, error);
    return true;
}

static gboolean rm_cmd_parse_large_output(
    _U const char *option_name, const gchar *output_pair, RmSession *session, _U GError **error
) {
    session->output_cnt[1] = MAX(session->output_cnt[1], 0);
    session->output_cnt[1] += rm_cmd_parse_output_pair(session, output_pair, error);
    return true;
}

static gboolean rm_cmd_parse_paranoid_mem(
    _U const char *option_name, const gchar *size_spec, RmSession *session, GError **error
) {
    const char *parse_error = NULL;
    RmOff size = rm_cmd_size_string_to_bytes(size_spec, &parse_error);

    if(parse_error != NULL) {
        g_set_error(
            error, RM_ERROR_DOMAIN, 0, 
            _("Invalid size description \"%s\": %s"), size_spec, parse_error
        );
        return false;
    } else {
        session->settings->paranoid_mem = size;
        return true;
    }
}

static gboolean rm_cmd_parse_clamp_low(
    _U const char *option_name, const gchar *spec, RmSession *session, _U GError **error
) {
    rm_cmd_parse_clamp_option(session, spec, true);
    return true;
}

static gboolean rm_cmd_parse_clamp_top(
    _U const char *option_name, const gchar *spec, RmSession *session, _U GError **error
) {
    rm_cmd_parse_clamp_option(session, spec, false);
    return true;
}

static gboolean rm_cmd_parse_cache(
    _U const char *option_name, const gchar *cache_path, RmSession *session, GError **error
) {
    if(!g_file_test(cache_path, G_FILE_TEST_IS_REGULAR)) {
        g_set_error(error, RM_ERROR_DOMAIN, 0, "There is no cache at `%s'", cache_path);
        return false;
    }
    
    g_queue_push_tail(&session->cache_list, g_strdup(cache_path));
    return true;
}

static gboolean rm_cmd_parse_progress(
    _U const char *option_name, _U const gchar *value, RmSession *session, _U GError **error
) {
    rm_fmt_clear(session->formats);
    rm_fmt_add(session->formats, "progressbar", "stdout");
    rm_fmt_add(session->formats, "summary", "stdout");
    rm_fmt_add(session->formats, "sh", "rmlint.sh");
    return true;
}

static gboolean rm_cmd_parse_no_progress(
    _U const char *option_name, _U const gchar *value, RmSession *session, _U GError **error
) {
    rm_fmt_clear(session->formats);
    rm_fmt_add(session->formats, "pretty", "stdout");
    rm_fmt_add(session->formats, "summary", "stdout");
    rm_fmt_add(session->formats, "sh", "rmlint.sh");
    return true;
}

static gboolean rm_cmd_parse_loud(
    _U const char *option_name, _U const gchar *count, RmSession *session, _U GError **error
) {
    rm_cmd_set_verbosity_from_cnt(session->settings, ++session->verbosity_count);
    return true;
}

static gboolean rm_cmd_parse_quiet(
    _U const char *option_name, _U const gchar *count, RmSession *session, _U GError **error
) {
    rm_cmd_set_verbosity_from_cnt(session->settings, --session->verbosity_count);
    return true;
}

static gboolean rm_cmd_parse_paranoid(
    _U const char *option_name, _U const gchar *count, RmSession *session, _U GError **error
) {
    rm_cmd_set_paranoia_from_cnt(session->settings, ++session->paranoia_count, error);
    return true;
}

static gboolean rm_cmd_parse_less_paranoid(
    _U const char *option_name, _U const gchar *count, RmSession *session, _U GError **error
) {
    rm_cmd_set_paranoia_from_cnt(session->settings, --session->paranoia_count, error);
    return true;
}

static gboolean rm_cmd_parse_merge_directories(
    _U const char *option_name, _U const gchar *count, RmSession *session, _U GError **error
) {
    RmSettings *settings = session->settings;
    settings->merge_directories = true;

    /* Pull in some options for convinience,
     * duplicate dir detection works better with them.
     *
     * They may be disabled explicitly though.
     */
    settings->find_hardlinked_dupes = true;
    settings->ignore_hidden = false;

    return true;
}

static bool rm_cmd_set_cwd(RmSettings *settings) {
    /* Get current directory */
    char cwd_buf[PATH_MAX + 1];
    memset(cwd_buf, 0, sizeof(cwd_buf));

    if(getcwd(cwd_buf, PATH_MAX) == NULL) {
        rm_log_perror("");
        return false;
    }

    settings->iwd = g_strdup_printf("%s%s", cwd_buf, G_DIR_SEPARATOR_S);
    return true;
}


static bool rm_cmd_set_cmdline(RmSettings *settings, int argc, char **argv) {
    /* Copy commandline rmlint was invoked with by copying argv into a
     * NULL padded array and join that with g_strjoinv. GLib made me lazy.
     */
    const char *argv_nul[argc + 1];
    memset(argv_nul, 0, sizeof(argv_nul));
    memcpy(argv_nul, argv, argc * sizeof(const char *));
    settings->joined_argv = g_strjoinv(" ", (gchar **)argv_nul);

    /* This cannot fail currently */
    return true;
}

static bool rm_cmd_set_paths(RmSession *session, char **paths) {
    int path_index = 0;
    bool is_prefd = false;
    bool not_all_paths_read = false;

    RmSettings *settings = session->settings;

    /* Check the directory to be valid */
    for(int i = 0; paths && paths[i]; ++i) {
        int read_paths = 0;
        const char *dir_path = paths[i];

        if(strncmp(dir_path, "-", 1) == 0) {
            read_paths = rm_cmd_read_paths_from_stdin(session, is_prefd, path_index);
        } else if(strncmp(dir_path, "//", 2) == 0) {
            is_prefd = !is_prefd;
        } else {
            read_paths = rm_cmd_add_path(session, is_prefd, path_index, paths[i]);
        }

        if(read_paths == 0) {
            not_all_paths_read = true;
        } else {
            path_index += read_paths;
        }
    }

    g_strfreev(paths);

    if(path_index == 0 && not_all_paths_read == false) {
        /* Still no path set? - use `pwd` */
        rm_cmd_add_path(session, is_prefd, path_index, settings->iwd);
    } else if(path_index == 0 && not_all_paths_read) {
        return false;
    }

    return true;
}

static bool rm_cmd_set_outputs(RmSession *session, GError **error) {
    if(session->output_cnt[0] >= 0 && session->output_cnt[1] >= 0) {
        g_set_error(error, RM_ERROR_DOMAIN, 0, _("Specifiyng both -o and -O is not allowed."));
        return false;
    } else if(session->output_cnt[0] < 0 && session->output_cnt[1] < 0 && !rm_fmt_len(session->formats)) {
        /* Set default outputs */
        rm_fmt_add(session->formats, "pretty", "stdout");
        rm_fmt_add(session->formats, "summary", "stdout");
        rm_fmt_add(session->formats, "sh", "rmlint.sh");
    }   

    return true;
}

/* Parse the commandline and set arguments in 'settings' (glob. var accordingly) */
bool rm_cmd_parse_args(int argc, char **argv, RmSession *session) {
    /* Normally we call this variable `settings`. 
     * But cfg is shorter for that table below. 
     */
    RmSettings *cfg = session->settings;

    /* List of paths we got passed (or NULL) */
    char **paths = NULL;

    /* Result of option parsing */
    bool result = true;

    #define DISABLE G_OPTION_FLAG_REVERSE
    #define FUNC(name) ((GOptionArgFunc)rm_cmd_parse_##name)
    #define EMPTY G_OPTION_FLAG_NO_ARG

    /* Free/Used Options:
       Free:  B DEFGHI KLMNOPQRST VWX   abcdefghi klmnopqrstuvwx 
       Used: A C      J               Z          j              yz
    */
    const GOptionEntry option_entries[] = {
        /* Option with required arguments */
        // TODO: More helpful help text
        {"threads"          ,  't' ,  0 ,  G_OPTION_ARG_INT      ,  &cfg->threads        ,  "Specify max number of threads" ,  "N"          },
        {"max-depth"        ,  'd' ,  0 ,  G_OPTION_ARG_INT      ,  &cfg->depth          ,  "Specify max traversal depth"   ,  "N"          },
        {"sortcriteria"     ,  'S' ,  0 ,  G_OPTION_ARG_STRING   ,  &cfg->sort_criteria  ,  "Original criteria"             ,  "[amp]"      },
        {"types"            ,  'T' ,  0 ,  G_OPTION_ARG_CALLBACK ,  FUNC(lint_types)     ,  "Specify lint types"            ,  "T"          },
        {"size"             ,  's' ,  0 ,  G_OPTION_ARG_CALLBACK ,  FUNC(limit_sizes)    ,  "Specify size limits"           ,  "m-M"        },
        {"algorithm"        ,  'a' ,  0 ,  G_OPTION_ARG_CALLBACK ,  FUNC(algorithm)      ,  "Choose hash algorithm"         ,  "A"          },
        {"output"           ,  'o' ,  0 ,  G_OPTION_ARG_CALLBACK ,  FUNC(small_output)   ,  "Add output (override default)" ,  "FMT[:PATH]" },
        {"add-output"       ,  'O' ,  0 ,  G_OPTION_ARG_CALLBACK ,  FUNC(large_output)   ,  "Add output (add to defaults)"  ,  "FMT[:PATH]" },
        {"max-paranoid-mem" ,  'u' ,  0 ,  G_OPTION_ARG_CALLBACK ,  FUNC(paranoid_mem)   ,  "TODO"                          ,  "S"          },
        {"newer-than-stamp" ,  'n' ,  0 ,  G_OPTION_ARG_CALLBACK ,  FUNC(timestamp_file) ,  "Newer than stamp file"         ,  "PATH"       },
        {"newer-than"       ,  'N' ,  0 ,  G_OPTION_ARG_CALLBACK ,  FUNC(timestamp)      ,  "Newer than timestamp"          ,  "STAMP"      },
        {"clamp-low"        ,  'q' ,  0 ,  G_OPTION_ARG_CALLBACK ,  FUNC(clamp_low)      ,  "Limit lower reading barrier"   ,  "P"          },
        {"clamp-top"        ,  'Q' ,  0 ,  G_OPTION_ARG_CALLBACK ,  FUNC(clamp_top)      ,  "Limit upper reading barrier"   ,  "P"          },
        {"config"           ,  'c' ,  0 ,  G_OPTION_ARG_CALLBACK ,  FUNC(config)         ,  "Configure a formatter"         ,  "FMT:K[=V]"  },
        {"cache"            ,  'C' ,  0 ,  G_OPTION_ARG_CALLBACK ,  FUNC(cache)          ,  "Add json cache file"           ,  "PATH"       },

        /* Non-trvial switches */
        {"progress"    , 'g' , EMPTY , G_OPTION_ARG_CALLBACK , FUNC(progress)    , "Enable progressbar"              , NULL},
        {"no-progress" , 'G' , EMPTY , G_OPTION_ARG_CALLBACK , FUNC(no_progress) , "Disable progressbar"             , NULL}, // TODO: hidden?
        {"loud"        , 'v' , EMPTY , G_OPTION_ARG_CALLBACK , FUNC(loud)        , "Be more verbose (-vvv for more)" , NULL},
        {"quiet"       , 'V' , EMPTY , G_OPTION_ARG_CALLBACK , FUNC(quiet)       , "Be less verbose (-VVV for less)" , NULL},

        /* Trivial boolean options */
        {"with-color"                 ,  'w' ,  0       ,  G_OPTION_ARG_NONE     ,  &cfg->color                   ,  "[x] Be colorful like a unicorn"                     ,  NULL},
        {"no-with-color"              ,  'W' ,  DISABLE ,  G_OPTION_ARG_NONE     ,  &cfg->color                   ,  "Be not that colorful"                               ,  NULL},
        {"hidden"                     ,  'r' ,  DISABLE ,  G_OPTION_ARG_NONE     ,  &cfg->ignore_hidden           ,  "Find hidden files"                                  ,  NULL},
        {"no-hidden"                  ,  'R' ,  0       ,  G_OPTION_ARG_NONE     ,  &cfg->ignore_hidden           ,  "[x] Ignore hidden files"                            ,  NULL},
        {"followlinks"                ,  'f' ,  0       ,  G_OPTION_ARG_NONE     ,  &cfg->followlinks             ,  "Follow symlinks"                                    ,  NULL},
        {"no-followlinks"             ,  'F' ,  DISABLE ,  G_OPTION_ARG_NONE     ,  &cfg->followlinks             ,  "Ignore symlinks"                                    ,  NULL},
        {"see-symlinks"               ,  '@' ,  0       ,  G_OPTION_ARG_NONE     ,  &cfg->see_symlinks            ,  "[x] Treat symlinks a regular files"                 ,  NULL},
        {"crossdev"                   ,  'x' ,  0       ,  G_OPTION_ARG_NONE     ,  &cfg->samepart                ,  "[x] Do not cross mounpoints"                        ,  NULL},
        {"no-crossdev"                ,  'X' ,  DISABLE ,  G_OPTION_ARG_NONE     ,  &cfg->samepart                ,  "Cross mounpoints"                                   ,  NULL},
        {"paranoid"                   ,  'p' ,  EMPTY   ,  G_OPTION_ARG_CALLBACK ,  FUNC(paranoid)                ,  "Use more paranoid hashing"                          ,  NULL},
        {"less-paranoid"              ,  'P' ,  EMPTY   ,  G_OPTION_ARG_CALLBACK ,  FUNC(less_paranoid)           ,  "Use less paranoid hashing"                          ,  NULL},
        {"keep-all-tagged"            ,  'k' ,  0       ,  G_OPTION_ARG_NONE     ,  &cfg->keep_all_tagged         ,  "Keep all tagged files"                              ,  NULL},
        {"keep-all-untagged"          ,  'K' ,  0       ,  G_OPTION_ARG_NONE     ,  &cfg->keep_all_untagged       ,  "Keep all untagged files"                            ,  NULL},
        {"must-match-tagged"          ,  'm' ,  0       ,  G_OPTION_ARG_NONE     ,  &cfg->must_match_tagged       ,  "Must have twin in tagged dir"                       ,  NULL},
        {"must-match-untagged"        ,  'M' ,  0       ,  G_OPTION_ARG_NONE     ,  &cfg->must_match_untagged     ,  "Must have twin in untagged dir"                     ,  NULL},
        {"hardlinked"                 ,  'l' ,  0       ,  G_OPTION_ARG_NONE     ,  &cfg->find_hardlinked_dupes   ,  "Report hardlinks as duplicates"                     ,  NULL},
        {"no-hardlinked"              ,  'L' ,  DISABLE ,  G_OPTION_ARG_NONE     ,  &cfg->find_hardlinked_dupes   ,  "[x] Ignore hardlinks"                               ,  NULL},
        {"match-basename"             ,  'b' ,  0       ,  G_OPTION_ARG_NONE     ,  &cfg->match_basename          ,  "Only find twins with same basename"                 ,  NULL},
        {"no-match-basename"          ,  'B' ,  DISABLE ,  G_OPTION_ARG_NONE     ,  &cfg->match_basename          ,  "Only find twins with same basename"                 ,  NULL},
        {"match-extension"            ,  'e' ,  0       ,  G_OPTION_ARG_NONE     ,  &cfg->match_with_extension    ,  "Only find twins with same extension"                ,  NULL},
        {"no-match-extension"         ,  'E' ,  DISABLE ,  G_OPTION_ARG_NONE     ,  &cfg->match_with_extension    ,  "Only find twins with same extension"                ,  NULL},
        {"match-without-extension"    ,  'i' ,  0       ,  G_OPTION_ARG_NONE     ,  &cfg->match_without_extension ,  "Only find twins with same basename minus extension" ,  NULL},
        {"no-match-without-extension" ,  'I' ,  DISABLE ,  G_OPTION_ARG_NONE     ,  &cfg->match_without_extension ,  "Only find twins with same extension"                ,  NULL},
        {"merge-directories"          ,  'D' ,  EMPTY   ,  G_OPTION_ARG_CALLBACK ,  FUNC(merge_directories)       ,  "Find duplicate directories"                         ,  NULL},
        {"xattr-write"                ,  0   ,  0       ,  G_OPTION_ARG_NONE     ,  &cfg->write_cksum_to_xattr    ,  "Cache checksum in file attributes"                  ,  NULL},
        {"no-xattr-write"             ,  0   ,  DISABLE ,  G_OPTION_ARG_NONE     ,  &cfg->write_cksum_to_xattr    ,  ""                                                   ,  NULL},
        {"xattr-read"                 ,  0   ,  0       ,  G_OPTION_ARG_NONE     ,  &cfg->read_cksum_from_xattr   ,  "Read cached checksums from file attributes"         ,  NULL},
        {"no-xattr-read"              ,  0   ,  DISABLE ,  G_OPTION_ARG_NONE     ,  &cfg->read_cksum_from_xattr   ,  ""                                                   ,  NULL},
        {"write-unfinished"           ,  'U' ,  0       ,  G_OPTION_ARG_NONE     ,  &cfg->write_unfinished        ,  "Output unfinished checksums"                        ,  NULL},

        /* Callback */
        {"show-man" ,  'H' ,  EMPTY ,  G_OPTION_ARG_CALLBACK ,  rm_cmd_show_help    ,  "Show the manpage"            ,  NULL} ,
        {"version"  ,  0   ,  EMPTY ,  G_OPTION_ARG_CALLBACK ,  rm_cmd_show_version ,  "Show the version & features" ,  NULL} ,

        /* Special case: accumulate leftover args (paths) in &paths */
        {G_OPTION_REMAINING ,  0 ,  0 ,  G_OPTION_ARG_FILENAME_ARRAY ,  &paths ,  ""   ,  NULL},
        {NULL               ,  0 ,  0 ,  0                           ,  NULL   ,  NULL ,  NULL}
    };

    /* Initialize default verbosity */
    rm_cmd_set_verbosity_from_cnt(cfg, session->verbosity_count);

    if(!rm_cmd_set_cwd(cfg)) {
        return false;
    }

    if(!rm_cmd_set_cmdline(cfg, argc, argv)) {
        return false;
    }

    GError *error = NULL;
    GOptionContext *option_parser = g_option_context_new(
        "rmlint [TARGET_DIR_OR_FILES ...] [//] [TARGET_DIR_OR_FILES ...] [-] [OPTIONS]"
    );
    GOptionGroup *main_group = g_option_group_new(
        "rmlint", "some options?", "uh name help", session, NULL
    );

    g_option_context_set_main_group(option_parser, main_group);
    g_option_context_set_summary(option_parser, "The summary");
    g_option_context_set_description(
        option_parser,
        "       See the manpage (man 1 rmlint or rmlint --help) for more detailed usage information.\n"
        "       or http://rmlint.rtfd.org/en/latest/rmlint.1.html for the online manpage for an online version\n\n"
    );

    g_option_group_add_entries(main_group, option_entries);
    g_option_group_set_error_hook(main_group, (GOptionErrorFunc)rm_cmd_on_error);

    if(!g_option_context_parse(option_parser, &argc, &argv, &error)) {
        rm_cmd_on_error(option_parser, main_group, session, &error);
    }

    /* Silent fixes of invalid numberic input */
    cfg->threads = CLAMP(cfg->threads, 1, 128);
    cfg->depth = CLAMP(cfg->depth, 1, PATH_MAX / 2 + 1);

    /* Overwrite color if we do not print to a terminal directly */
    cfg->color = isatty(fileno(stdout)) && isatty(fileno(stderr));
    
    if(cfg->keep_all_tagged && cfg->keep_all_untagged) {
        error = g_error_new(
            RM_ERROR_DOMAIN, 0,
            _("can't specify both --keep-all-tagged and --keep-all-untagged")
        );

    } else if(cfg->skip_start_factor >= cfg->skip_end_factor) {
        error = g_error_new(
            RM_ERROR_DOMAIN, 0,
            _("-q (--clamp-low) should be lower than -Q (--clamp-top)!")
        );
    } else if(!rm_cmd_set_paths(session, paths)) {
        error = g_error_new(
            RM_ERROR_DOMAIN, 0,
            _("No valid paths given.")
        );
    } else if(!rm_cmd_set_outputs(session, &error)) {
        /* Something wrong with the outputs */
    }

    if(error != NULL) {
        rm_cmd_on_error(option_parser, main_group, session, &error);
        result = false;
        goto failure;
    }


failure:
    g_option_context_free(option_parser);
    return result;
}

int rm_cmd_main(RmSession *session) {
    int exit_state = EXIT_SUCCESS;

    rm_fmt_set_state(session->formats, RM_PROGRESS_STATE_INIT);
    rm_fmt_set_state(session->formats, RM_PROGRESS_STATE_TRAVERSE);
    session->mounts = rm_mounts_table_new();
    if(session->mounts == NULL) {
        exit_state = EXIT_FAILURE;
        goto failure;
    }

    rm_traverse_tree(session);

    rm_log_debug(
        "List build finished at %.3f with %"LLU" files\n",
        g_timer_elapsed(session->timer, NULL), session->total_files
    );

    if(session->settings->merge_directories) {
        session->dir_merger = rm_tm_new(session);
    }

    if(session->total_files >= 1) {
        rm_fmt_set_state(session->formats, RM_PROGRESS_STATE_PREPROCESS);
        rm_preprocess(session);

        if(session->settings->searchdup || session->settings->merge_directories) {
            rm_shred_run(session);

            rm_log_debug("Dupe search finished at time %.3f\n", g_timer_elapsed(session->timer, NULL));
        }
    }

    if(session->settings->merge_directories) {
        rm_fmt_set_state(session->formats, RM_PROGRESS_STATE_MERGE);
        rm_tm_finish(session->dir_merger);
    }

    rm_fmt_set_state(session->formats, RM_PROGRESS_STATE_PRE_SHUTDOWN);
    rm_fmt_set_state(session->formats, RM_PROGRESS_STATE_SUMMARY);

failure:
    rm_session_clear(session);
    return exit_state;
}
