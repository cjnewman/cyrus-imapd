/* jmap_ical.c --Routines to convert calendar events between JMAP and iCalendar
 *
 * Copyright (c) 1994-2016 Carnegie Mellon University.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The name "Carnegie Mellon University" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For permission or any legal
 *    details, please contact
 *      Carnegie Mellon University
 *      Center for Technology Transfer and Enterprise Creation
 *      4615 Forbes Avenue
 *      Suite 302
 *      Pittsburgh, PA  15213
 *      (412) 268-7393, fax: (412) 268-7395
 *      innovation@andrew.cmu.edu
 *
 * 4. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by Computing Services
 *     at Carnegie Mellon University (http://www.cmu.edu/computing/)."
 *
 * CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
 * THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE
 * FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include <config.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <ctype.h>
#include <string.h>
#include <syslog.h>
#include <assert.h>

#include "acl.h"
#include "annotate.h"
#include "append.h"
#include "caldav_db.h"
#include "carddav_db.h"
#include "global.h"
#include "hash.h"
#include "httpd.h"
#include "http_caldav.h"
#include "http_carddav.h"
#include "http_caldav_sched.h"
#include "http_dav.h"
#include "http_jmap.h"
#include "http_proxy.h"
#include "ical_support.h"
#include "json_support.h"
#include "mailbox.h"
#include "mboxlist.h"
#include "mboxname.h"
#include "parseaddr.h"
#include "seen.h"
#include "statuscache.h"
#include "stristr.h"
#include "times.h"
#include "util.h"
#include "vcard_support.h"
#include "version.h"
#include "xmalloc.h"
#include "xsha1.h"
#include "xstrlcat.h"
#include "xstrlcpy.h"
#include "zoneinfo_db.h"

/* for sasl_encode64 */
#include <sasl/sasl.h>
#include <sasl/saslutil.h>

/* generated headers are not necessarily in current directory */
#include "imap/http_err.h"
#include "imap/imap_err.h"

#include "jmap_ical.h"

#define JMAPICAL_READ_MODE       0
#define JMAPICAL_WRITE_MODE      (1<<0)
#define JMAPICAL_UPDATE_MODE     (1<<1)
#define JMAPICAL_EXC_MODE        (1<<8)

typedef struct context {
    jmapical_err_t *err;    /* conversion error, if any */
    jmapical_err_t *_err;   /* conversion error owned by context */

    int mode;               /* Flags indicating the current context mode. */

    /* Property context */
    json_t *invalid;        /* A JSON array of any invalid properties. */
    strarray_t propstr;
    struct buf propbuf;

    /* Conversion to JMAP context */
    json_t *wantprops;         /* which properties to fetch */
    icalcomponent *master;     /* the main event of an exception */
    const char *tzid_start;
    int is_allday;
    const char *uid;

    /* Conversion to iCalendar context */
    icalcomponent *comp;       /* The current main event of an exception. */

    icaltimezone *tzstart_old; /* The former startTimeZone. */
    icaltimezone *tzstart;     /* The current startTimeZone. */
    icaltimezone *tzend_old;   /* The former endTimeZone. */
    icaltimezone *tzend;       /* The current endTimeZone. */
} context_t;

/* Forward declarations */
static json_t *calendarevent_from_ical(context_t *, icalcomponent *);
static void calendarevent_to_ical(context_t *, icalcomponent *, json_t*);

static int JNOTNULL(json_t *item)
{
   if (!item) return 0;
   if (json_is_null(item)) return 0;
   return 1;
}

static char *hexkey(const char *val)
{
    unsigned char dest[SHA1_DIGEST_LENGTH];
    char idbuf[2*SHA1_DIGEST_LENGTH+1];
    int r;

    xsha1((const unsigned char *) val, strlen(val), dest);
    r = bin_to_hex(dest, SHA1_DIGEST_LENGTH, idbuf, BH_LOWER);
    assert(r == 2*SHA1_DIGEST_LENGTH);
    idbuf[2*SHA1_DIGEST_LENGTH] = '\0';
    return xstrdup(idbuf);
}

static context_t *context_new(json_t *wantprops,
                              jmapical_err_t *err,
                              int mode)
{
    context_t *ctx = xzmalloc(sizeof(struct context));
    if (!err) {
        ctx->_err = xzmalloc(sizeof(jmapical_err_t));
    }
    ctx->err = err ? err : ctx->_err;
    ctx->wantprops = wantprops;
    ctx->invalid = json_pack("{}");
    ctx->mode = mode;
    return ctx;
}

static void context_free(context_t *ctx)
{
    if (ctx->_err) {
        free(ctx->_err);
    }
    if (ctx->invalid) {
        json_decref(ctx->invalid);
    }
    strarray_fini(&ctx->propstr);
    buf_free(&ctx->propbuf);
    free(ctx);
}

static int wantprop(context_t *ctx, const char *name)
{
    if (!ctx->wantprops) {
        return 1;
    }
    return json_object_get(ctx->wantprops, name) != NULL;
}

static void beginprop_key(context_t *ctx, const char *name, const char *key)
{
    struct buf *buf = &ctx->propbuf;
    strarray_t *str = &ctx->propstr;

    if (json_pointer_needsencode(name)) {
        char *tmp = json_pointer_encode(name);
        buf_setcstr(buf, tmp);
        free(tmp);
    } else {
        buf_setcstr(buf, name);
    }

    buf_appendcstr(buf, "/");

    if (json_pointer_needsencode(key)) {
        char *tmp = json_pointer_encode(key);
        buf_appendcstr(buf, tmp);
        free(tmp);
    } else {
        buf_appendcstr(buf, key);
    }

    strarray_push(str, buf_cstring(buf));
    buf_reset(buf);
}

static void beginprop_idx(context_t *ctx, const char *name, size_t idx)
{
    struct buf *buf = &ctx->propbuf;
    strarray_t *str = &ctx->propstr;

    if (json_pointer_needsencode(name)) {
        char *tmp = json_pointer_encode(name);
        buf_setcstr(buf, tmp);
        free(tmp);
    } else {
        buf_setcstr(buf, name);
    }

    buf_appendcstr(buf, "/");
    buf_printf(buf, "%zu", idx);

    strarray_push(str, buf_cstring(buf));
    buf_reset(buf);
}

static void beginprop(context_t *ctx, const char *name)
{
    strarray_t *str = &ctx->propstr;

    if (json_pointer_needsencode(name)) {
        char *tmp = json_pointer_encode(name);
        strarray_push(str, tmp);
        free(tmp);
    } else {
        strarray_push(str, name);
    }
}

static void endprop(context_t *ctx)
{
    strarray_t *str = &ctx->propstr;
    assert(strarray_size(str));
    free(strarray_pop(str));
}

static char* encodeprop(context_t *ctx, const char *name)
{
    struct buf *buf = &ctx->propbuf;
    strarray_t *str = &ctx->propstr;
    int i;

    if (!name && !strarray_size(str)) {
        return NULL;
    }

    if (name) beginprop(ctx, name);

    buf_setcstr(buf, strarray_nth(str, 0));
    for (i = 1; i < strarray_size(str); i++) {
        buf_appendcstr(buf, "/");
        buf_appendcstr(buf, strarray_nth(str, i));
    }

    if (name) endprop(ctx);

    return buf_newcstring(buf);
}

static void invalidprop(context_t *ctx, const char *name)
{
    char *tmp = encodeprop(ctx, name);
    json_object_set_new(ctx->invalid, tmp, json_null());
    free(tmp);
}

static void invalidprop_append(context_t *ctx, json_t *props)
{
    size_t i;
    json_t *val;
    struct buf buf = BUF_INITIALIZER;

    json_array_foreach(props, i, val) {
        const char *raw;
        char *tmp;

        raw = json_string_value(val);
        tmp = encodeprop(ctx, NULL);
        buf_setcstr(&buf, tmp);
        buf_appendcstr(&buf, "/");
        buf_appendcstr(&buf, raw);
        json_object_set_new(ctx->invalid, buf_cstring(&buf), json_null());
        free(tmp);
    }

    buf_free(&buf);
}

static int have_invalid_props(context_t *ctx)
{
    return json_object_size(ctx->invalid) > 0;
}

static size_t invalid_prop_count(context_t *ctx)
{
    return json_object_size(ctx->invalid);
}

static json_t* get_invalid_props(context_t *ctx)
{
    json_t *props = json_pack("[]");
    const char *key;
    json_t *val;

    if (!ctx->invalid)
        return NULL;

    json_object_foreach(ctx->invalid, key, val) {
        json_array_append_new(props, json_string(key));
    }

    if (!json_array_size(props)) {
        json_decref(props);
        props = NULL;
    }

    return props;
}

/* Read the property named name into dst, formatted according to the json
 * unpack format fmt. Report missing or erroneous properties.
 *
 * Return a negative value for a missing or invalid property.
 * Return a positive value if a property was read, zero otherwise. */
static int readprop(context_t *ctx, json_t *from, const char *name,
                    int is_mandatory, const char *fmt, void *dst)
{
    int r = 0;
    json_t *jval = json_object_get(from, name);
    if (!jval && is_mandatory) {
        r = -1;
    } else if (jval) {
        json_error_t err;
        if (json_unpack_ex(jval, &err, 0, fmt, dst)) {
            r = -2;
        } else {
            r = 1;
        }
    }
    if (r < 0) {
        invalidprop(ctx, name);
    }
    return r;
}

static char *mailaddr_from_uri(const char *uri)
{
    if (!uri || strncasecmp(uri, "mailto:", 7)) {
        return NULL;
    }
    uri += 7;
    return address_canonicalise(uri);
}

static char *mailaddr_to_uri(const char *addr)
{
    struct buf buf = BUF_INITIALIZER;
    buf_setcstr(&buf, "mailto:");
    buf_appendcstr(&buf, addr);
    return buf_release(&buf);
}

static char*
encode_base64_uri(const char *data, size_t len, const char *type)
{
    /* base64 encode data */
    size_t len64 = (4 * ((len + 3) / 3)) + 1;
    char *data64 = xzmalloc(len64);
    sasl_encode64(data, len, data64, len64, NULL);

    /* Make data URI */
    char *uri = strconcat("data:", type, ";base64,", data64, NULL);
    free(data64);
    return uri;
}


static char*
encode_base64_json(json_t *src)
{
    /* base64 encode JSON */
    char *data = json_dumps(src, JSON_COMPACT);
    char *uri = encode_base64_uri(data, strlen(data), "application/json");
    free(data);
    return uri;
}

static char*
decode_base64_uri(const char *uri)
{
    const char *data = strstr(uri, ";base64,");
    if (!data) {
        return NULL;
    }
    data += 8;
    struct buf buf = BUF_INITIALIZER;
    if (charset_decode(&buf, data, strlen(data), ENCODING_BASE64) < 0) {
        buf_free(&buf);
        return NULL;
    }
    return buf_release(&buf);
}

static json_t *
decode_base64_json(const char *uri)
{
    char *raw = decode_base64_uri(uri);
    if (!raw) return NULL;
    json_t *jdata = json_loads(raw, 0, NULL);
    free(raw);
    return jdata;
}

static void remove_icalxparam(icalproperty *prop, const char *name)
{
    icalparameter *param, *next;

    for (param = icalproperty_get_first_parameter(prop, ICAL_X_PARAMETER);
         param;
         param = next) {

        next = icalproperty_get_next_parameter(prop, ICAL_X_PARAMETER);
        if (strcasecmp(icalparameter_get_xname(param), name)) {
            continue;
        }
        icalproperty_remove_parameter_by_ref(prop, param);
    }
}


static const char*
get_icalxparam_value(icalproperty *prop, const char *name)
{
    icalparameter *param;

    for (param = icalproperty_get_first_parameter(prop, ICAL_X_PARAMETER);
         param;
         param = icalproperty_get_next_parameter(prop, ICAL_X_PARAMETER)) {

        if (strcasecmp(icalparameter_get_xname(param), name)) {
            continue;
        }
        return icalparameter_get_xvalue(param);
    }

    return NULL;
}

static void
set_icalxparam(icalproperty *prop, const char *name, const char *val, int purge)
{
    icalparameter *param;

    if (purge) remove_icalxparam(prop, name);

    param = icalparameter_new(ICAL_X_PARAMETER);
    icalparameter_set_xname(param, name);
    icalparameter_set_xvalue(param, val);
    icalproperty_add_parameter(prop, param);
}

/* Compare the value of the first occurences of property kind in components
 * a and b. Return 0 if they match or if both do not contain kind. Note that
 * this function does not define an order on property values, so it can't be
 * used for sorting. */
int compare_icalprop(icalcomponent *a, icalcomponent *b,
                     icalproperty_kind kind) {
    icalproperty *pa, *pb;
    icalvalue *va, *vb;

    pa = icalcomponent_get_first_property(a, kind);
    pb = icalcomponent_get_first_property(b, kind);
    if (!pa && !pb) {
        return 0;
    }

    va = icalproperty_get_value(pa);
    vb = icalproperty_get_value(pb);
    enum icalparameter_xliccomparetype cmp = icalvalue_compare(va, vb);
    return cmp != ICAL_XLICCOMPARETYPE_EQUAL;
}

static const char*
get_icalxprop_value(icalcomponent *comp, const char *name)
{
    icalproperty *prop;

    for (prop = icalcomponent_get_first_property(comp, ICAL_X_PROPERTY);
         prop;
         prop = icalcomponent_get_next_property(comp, ICAL_X_PROPERTY)) {

        if (strcasecmp(icalproperty_get_x_name(prop), name)) {
            continue;
        }
        return icalproperty_get_value_as_string(prop);
    }

    return NULL;
}

/* Remove and deallocate any x-properties with name in comp. */
static void remove_icalxprop(icalcomponent *comp, const char *name)
{
    icalproperty *prop, *next;
    icalproperty_kind kind = ICAL_X_PROPERTY;

    for (prop = icalcomponent_get_first_property(comp, kind);
         prop;
         prop = next) {

        next = icalcomponent_get_next_property(comp, kind);

        if (strcasecmp(icalproperty_get_x_name(prop), name))
            continue;

        icalcomponent_remove_property(comp, prop);
        icalproperty_free(prop);
    }
}

static char *xjmapid_from_ical(icalproperty *prop)
{
    char *id = (char *) get_icalxparam_value(prop, JMAPICAL_XPARAM_ID);
    if (!id) {
        id = hexkey(icalproperty_as_ical_string(prop));
    } else {
        id = xstrdup(id);
    }
    return id;
}

static void xjmapid_to_ical(icalproperty *prop, const char *id)
{
    struct buf buf = BUF_INITIALIZER;
    icalparameter *param;

    buf_setcstr(&buf, JMAPICAL_XPARAM_ID);
    buf_appendcstr(&buf, "=");
    buf_appendcstr(&buf, id);
    param = icalparameter_new_from_string(buf_cstring(&buf));
    icalproperty_add_parameter(prop, param);

    buf_free(&buf);
}

static icaltimezone *tz_from_tzid(const char *tzid)
{
    if (!tzid)
        return NULL;

    /* libical doesn't return the UTC singleton for Etc/UTC */
    if (!strcmp(tzid, "Etc/UTC") || !strcmp(tzid, "UTC"))
        return icaltimezone_get_utc_timezone();

    return icaltimezone_get_builtin_timezone(tzid);
}

/* Determine the Olson TZID, if any, of the ical property prop. */
static const char *tzid_from_icalprop(icalproperty *prop, int guess) {
    const char *tzid = NULL;
    icalparameter *param = NULL;

    if (prop) param = icalproperty_get_first_parameter(prop, ICAL_TZID_PARAMETER);
    if (param) tzid = icalparameter_get_tzid(param);
    /* Check if the tzid already corresponds to an Olson name. */
    if (tzid) {
        icaltimezone *tz = tz_from_tzid(tzid);
        if (!tz && guess) {
            /* Try to guess the timezone. */
            icalvalue *val = icalproperty_get_value(prop);
            icaltimetype dt = icalvalue_get_datetime(val);
            tzid = dt.zone ? icaltimezone_get_location((icaltimezone*) dt.zone) : NULL;
            tzid = tzid && tz_from_tzid(tzid) ? tzid : NULL;
        }
    } else {
        icalvalue *val = icalproperty_get_value(prop);
        icaltimetype dt = icalvalue_get_datetime(val);
        if (icaltime_is_valid_time(dt) && icaltime_is_utc(dt)) {
            tzid = "Etc/UTC";
        }
    }
    return tzid;
}

/* Determine the Olson TZID, if any, of the ical property kind in component comp. */
static const char *tzid_from_ical(icalcomponent *comp,
                                  icalproperty_kind kind) {
    icalproperty *prop = icalcomponent_get_first_property(comp, kind);
    if (!prop) {
        return NULL;
    }
    return tzid_from_icalprop(prop, 1/*guess*/);
}

static struct icaltimetype dtstart_from_ical(icalcomponent *comp)
{
    struct icaltimetype dt;
    const char *tzid;

    dt = icalcomponent_get_dtstart(comp);
    if (dt.zone) return dt;

    if ((tzid = tzid_from_ical(comp, ICAL_DTSTART_PROPERTY))) {
        dt.zone = tz_from_tzid(tzid);
    }

    return dt;
}

static struct icaltimetype dtend_from_ical(icalcomponent *comp)
{
    struct icaltimetype dt;
    icalproperty *prop;
    const char *tzid;

    /* Handles DURATION vs DTEND */
    dt = icalcomponent_get_dtend(comp);
    if (dt.zone) return dt;

    prop = icalcomponent_get_first_property(comp, ICAL_DTEND_PROPERTY);
    if (prop) {
        if ((tzid = tzid_from_icalprop(prop, 1))) {
            dt.zone = tz_from_tzid(tzid);
        }
    } else {
        dt.zone = dtstart_from_ical(comp).zone;
    }

    return dt;
}


/* Convert time t to a RFC3339 formatted localdate string. Return the number
 * of bytes written to buf sized size, excluding the terminating null byte. */
static int timet_to_localdate(time_t t, char* buf, size_t size) {
    int n = time_to_rfc3339(t, buf, size);
    if (n && buf[n-1] == 'Z') {
        buf[n-1] = '\0';
        n--;
    }
    return n;
}

/* Convert icaltime to a RFC3339 formatted localdate string.
 * The returned string is owned by the caller or NULL on error.
 */
static char* localdate_from_icaltime_r(icaltimetype icaltime) {
    char *s;
    time_t t;

    s = xzmalloc(RFC3339_DATETIME_MAX);
    if (!s) {
        return NULL;
    }

    t = icaltime_as_timet(icaltime);
    if (!timet_to_localdate(t, s, RFC3339_DATETIME_MAX)) {
        return NULL;
    }
    return s;
}

/* Convert icaltime to a RFC3339 formatted string.
 *
 * The returned string is owned by the caller or NULL on error.
 */
static char* utcdate_from_icaltime_r(icaltimetype icaltime) {
    char *s;
    time_t t;
    int n;

    s = xzmalloc(RFC3339_DATETIME_MAX);
    if (!s) {
        return NULL;
    }

    t = icaltime_as_timet(icaltime);

    n = time_to_rfc3339(t, s, RFC3339_DATETIME_MAX);
    if (!n) {
        free(s);
        return NULL;
    }
    return s;
}

/* Compare int in ascending order. */
static int compare_int(const void *aa, const void *bb)
{
    const int *a = aa, *b = bb;
    return (*a < *b) ? -1 : (*a > *b);
}

/* Return the identity of i. This is a helper for recur_byX. */
static int identity_int(int i) {
    return i;
}

/*
 * Conversion from iCalendar to JMAP
 */

/* Convert at most nmemb entries in the ical recurrence byDay/Month/etc array
 * named byX using conv. Return a new JSON array, sorted in ascending order. */
static json_t* recurrence_byX_fromical(short byX[], size_t nmemb, int (*conv)(int)) {
    json_t *jbd = json_pack("[]");

    size_t i;
    int tmp[nmemb];
    for (i = 0; i < nmemb && byX[i] != ICAL_RECURRENCE_ARRAY_MAX; i++) {
        tmp[i] = conv(byX[i]);
    }

    size_t n = i;
    qsort(tmp, n, sizeof(int), compare_int);
    for (i = 0; i < n; i++) {
        json_array_append_new(jbd, json_pack("i", tmp[i]));
    }

    return jbd;
}

/* Convert the ical recurrence recur to a JMAP recurrenceRule */
static json_t*
recurrence_from_ical(context_t *ctx, icalcomponent *comp)
{
    char *s = NULL;
    size_t i;
    json_t *recur;
    struct buf buf = BUF_INITIALIZER;
    icalproperty *prop;
    struct icalrecurrencetype rrule;
    const char *tzid = ctx->tzid_start;

    prop = icalcomponent_get_first_property(comp, ICAL_RRULE_PROPERTY);
    if (!prop) {
        return json_null();
    }
    rrule = icalproperty_get_rrule(prop);

    recur = json_pack("{}");
    /* frequency */
    s = xstrdup(icalrecur_freq_to_string(rrule.freq));
    s = lcase(s);
    json_object_set_new(recur, "frequency", json_string(s));
    free(s);

    if (rrule.interval > 1) {
        json_object_set_new(recur, "interval", json_pack("i", rrule.interval));
    }

#ifdef HAVE_RSCALE
    /* rscale */
    if (rrule.rscale) {
        s = xstrdup(rrule.rscale);
        s = lcase(s);
        json_object_set_new(recur, "rscale", json_string(s));
        free(s);
    }

    /* skip */
    switch (rrule.skip) {
        case ICAL_SKIP_BACKWARD:
            s = "backward";
            break;
        case ICAL_SKIP_FORWARD:
            s = "forward";
            break;
        case ICAL_SKIP_OMIT:
            /* fall through */
        default:
            s = NULL;
    }
    if (s) json_object_set_new(recur, "skip", json_string(s));
#endif

    /* firstDayOfWeek */
    s = xstrdup(icalrecur_weekday_to_string(rrule.week_start));
    s = lcase(s);
    if (strcmp(s, "mo")) {
        json_object_set_new(recur, "firstDayOfWeek", json_string(s));
    }
    free(s);

    /* byDay */
    json_t *jbd = json_pack("[]");
    for (i = 0; i < ICAL_BY_DAY_SIZE; i++) {
        json_t *jday;
        icalrecurrencetype_weekday weekday;
        int pos;

        if (rrule.by_day[i] == ICAL_RECURRENCE_ARRAY_MAX) {
            break;
        }

        jday = json_pack("{}");
        weekday = icalrecurrencetype_day_day_of_week(rrule.by_day[i]);

        s = xstrdup(icalrecur_weekday_to_string(weekday));
        s = lcase(s);
        json_object_set_new(jday, "day", json_string(s));
        free(s);

        pos = icalrecurrencetype_day_position(rrule.by_day[i]);
        if (pos) {
            json_object_set_new(jday, "nthOfPeriod", json_integer(pos));
        }

        if (json_object_size(jday)) {
            json_array_append_new(jbd, jday);
        } else {
            json_decref(jday);
        }
    }
    if (json_array_size(jbd)) {
        json_object_set_new(recur, "byDay", jbd);
    } else {
        json_decref(jbd);
    }

    /* byMonth */
    json_t *jbm = json_pack("[]");
    for (i = 0; i < ICAL_BY_MONTH_SIZE; i++) {
        short bymonth;

        if (rrule.by_month[i] == ICAL_RECURRENCE_ARRAY_MAX) {
            break;
        }

        bymonth = rrule.by_month[i];
        buf_printf(&buf, "%d", icalrecurrencetype_month_month(bymonth));
        if (icalrecurrencetype_month_is_leap(bymonth)) {
            buf_appendcstr(&buf, "L");
        }
        json_array_append_new(jbm, json_string(buf_cstring(&buf)));
        buf_reset(&buf);

    }
    if (json_array_size(jbm)) {
        json_object_set_new(recur, "byMonth", jbm);
    } else {
        json_decref(jbm);
    }

    if (rrule.by_month_day[0] != ICAL_RECURRENCE_ARRAY_MAX) {
        json_object_set_new(recur, "byDate",
                recurrence_byX_fromical(rrule.by_month_day,
                    ICAL_BY_MONTHDAY_SIZE, &identity_int));
    }
    if (rrule.by_year_day[0] != ICAL_RECURRENCE_ARRAY_MAX) {
        json_object_set_new(recur, "byYearDay",
                recurrence_byX_fromical(rrule.by_year_day,
                    ICAL_BY_YEARDAY_SIZE, &identity_int));
    }
    if (rrule.by_week_no[0] != ICAL_RECURRENCE_ARRAY_MAX) {
        json_object_set_new(recur, "byWeekNo",
                recurrence_byX_fromical(rrule.by_week_no,
                    ICAL_BY_WEEKNO_SIZE, &identity_int));
    }
    if (rrule.by_hour[0] != ICAL_RECURRENCE_ARRAY_MAX) {
        json_object_set_new(recur, "byHour",
                recurrence_byX_fromical(rrule.by_hour,
                    ICAL_BY_HOUR_SIZE, &identity_int));
    }
    if (rrule.by_minute[0] != ICAL_RECURRENCE_ARRAY_MAX) {
        json_object_set_new(recur, "byMinute",
                recurrence_byX_fromical(rrule.by_minute,
                    ICAL_BY_MINUTE_SIZE, &identity_int));
    }
    if (rrule.by_second[0] != ICAL_RECURRENCE_ARRAY_MAX) {
        json_object_set_new(recur, "bySecond",
                recurrence_byX_fromical(rrule.by_second,
                    ICAL_BY_SECOND_SIZE, &identity_int));
    }
    if (rrule.by_set_pos[0] != ICAL_RECURRENCE_ARRAY_MAX) {
        json_object_set_new(recur, "bySetPosition",
                recurrence_byX_fromical(rrule.by_set_pos,
                    ICAL_BY_SETPOS_SIZE, &identity_int));
    }

    if (rrule.count != 0) {
        /* Recur count takes precedence over until. */
        json_object_set_new(recur, "count", json_integer(rrule.count));
    } else if (!icaltime_is_null_time(rrule.until)) {
        icaltimezone *tz = tz_from_tzid(tzid);
        icaltimetype dtloc = icaltime_convert_to_zone(rrule.until, tz);
        char *until = localdate_from_icaltime_r(dtloc);
        if (until == NULL) {
            ctx->err->code = JMAPICAL_ERROR_MEMORY;
            return NULL;
        }
        json_object_set_new(recur, "until", json_string(until));
        free(until);
    }

    if (!json_object_size(recur)) {
        json_decref(recur);
        recur = json_null();
    }

    buf_free(&buf);
    return recur;
}

static json_t*
override_rdate_from_ical(context_t *ctx __attribute__((unused)),
                         icalproperty *prop)
{
    /* returns a JSON object with a single key value pair */
    json_t *override = json_pack("{}");
    json_t *o = json_pack("{}");
    struct icaldatetimeperiodtype rdate = icalproperty_get_rdate(prop);
    icaltimetype id;

    if (!icaltime_is_null_time(rdate.time)) {
        id = rdate.time;
    } else {
        /* PERIOD */
        struct icaldurationtype dur;
        id = rdate.period.start;

        /* Determine duration */
        if (!icaltime_is_null_time(rdate.period.end)) {
            dur = icaltime_subtract(rdate.period.end, id);
        } else {
            dur = rdate.period.duration;
        }

        json_object_set_new(o, "duration",
                json_string(icaldurationtype_as_ical_string(dur)));
    }

    if (!icaltime_is_null_time(id)) {
        char *t = localdate_from_icaltime_r(id);
        json_object_set_new(override, t, o);
        free(t);
    }

    if (!json_object_size(override)) {
        json_decref(override);
        json_decref(o);
        override = NULL;
    }
    return override;
}

static json_t*
override_exdate_from_ical(context_t *ctx, icalproperty *prop)
{
    json_t *override = json_pack("{}");
    icaltimetype id = icalproperty_get_exdate(prop);
    const char *tzid_xdate;

    tzid_xdate = tzid_from_icalprop(prop, 1);
    if (ctx->tzid_start && tzid_xdate && strcmp(ctx->tzid_start, tzid_xdate)) {
        icaltimezone *tz_xdate = tz_from_tzid(tzid_xdate);
        icaltimezone *tz_start = tz_from_tzid(ctx->tzid_start);
        if (tz_xdate && tz_start) {
            if (id.zone) id.zone = tz_xdate;
            id = icaltime_convert_to_zone(id, tz_start);
        }
    }

    if (!icaltime_is_null_time(id)) {
        char *t = localdate_from_icaltime_r(id);
        json_object_set_new(override, t, json_pack("{s:b}", "excluded", 1));
        free(t);
    }

    if (!json_object_size(override)) {
        json_decref(override);
        override = NULL;
    }

    return override;
}

static json_t*
overrides_from_ical(context_t *ctx, icalcomponent *comp, json_t *event)
{
    icalproperty *prop;
    json_t *overrides = json_pack("{}");

    /* RDATE */
    for (prop = icalcomponent_get_first_property(comp, ICAL_RDATE_PROPERTY);
         prop;
         prop = icalcomponent_get_next_property(comp, ICAL_RDATE_PROPERTY)) {

        json_t *override = override_rdate_from_ical(ctx, prop);
        if (override) {
            json_object_update(overrides, override);
            json_decref(override);
        }
    }

    /* EXDATE */
    for (prop = icalcomponent_get_first_property(comp, ICAL_EXDATE_PROPERTY);
         prop;
         prop = icalcomponent_get_next_property(comp, ICAL_EXDATE_PROPERTY)) {

        json_t *override = override_exdate_from_ical(ctx, prop);
        if (override) {
            json_object_update(overrides, override);
            json_decref(override);
        }
    }

    /* VEVENT exceptions */
    json_t *exceptions = json_pack("{}");
    icalcomponent *excomp, *ical;

    ical = icalcomponent_get_parent(comp);
    for (excomp = icalcomponent_get_first_component(ical, ICAL_VEVENT_COMPONENT);
         excomp;
         excomp = icalcomponent_get_next_component(ical, ICAL_VEVENT_COMPONENT)) {

        if (excomp == comp) continue; /* skip toplevel promoted object */

        context_t *myctx;
        json_t *ex, *diff;
        struct icaltimetype recurid;
        char *s;
        const char *exstart;

        /* Convert VEVENT exception to JMAP */
        myctx = context_new(ctx->wantprops, ctx->err, JMAPICAL_READ_MODE);
        myctx->master = comp;
        ex = calendarevent_from_ical(myctx, excomp);
        context_free(myctx);
        if (!ex) {
            continue;
        }
        json_object_del(ex, "updated");
        json_object_del(ex, "created");

        /* Determine recurrence id */
        recurid = icalcomponent_get_recurrenceid(excomp);
        s = localdate_from_icaltime_r(recurid);
        exstart = json_string_value(json_object_get(ex, "start"));
        if (exstart && !strcmp(exstart, s)) {
            json_object_del(ex, "start");
        }

        /* Create override patch */
        diff = jmap_patchobject_create(event, ex);
        json_decref(ex);

        /* Set override at recurrence id */
        json_object_set_new(exceptions, s, diff);
        free(s);
    }

    json_object_update(overrides, exceptions);
    json_decref(exceptions);

    if (!json_object_size(overrides)) {
        json_decref(overrides);
        overrides = json_null();
    }

    return overrides;
}

static json_t*
replyto_from_ical(context_t *ctx __attribute__((unused)), icalcomponent *comp)
{
    icalproperty *prop;
    json_t *replyto = json_pack("{}");

    prop = icalcomponent_get_first_property(comp, ICAL_ORGANIZER_PROPERTY);
    if (prop) {
        const char *org, *uri;

        if ((org = icalproperty_get_organizer(prop))) {
            json_object_set_new(replyto, "imip", json_string(org));
        }
        /* XXX: let's see if we can use the new PARTICIPANT component */
        if ((uri = get_icalxparam_value(prop, JMAPICAL_XPARAM_RSVP_URI))) {
            json_object_set_new(replyto, "web", json_string(uri));
        }
    }

    if (!json_object_size(replyto)) {
        json_decref(replyto);
        replyto = json_null();
    }

    return replyto;
}

static json_t *participant_from_ical(icalproperty *prop, hash_table *hatts,
                                     icalproperty *orga)
{
    json_t *p = json_object();
    icalparameter *param;
    struct buf buf = BUF_INITIALIZER;

    /* FIXME invitedBy */

    /* email */
    char *email = mailaddr_from_uri(icalproperty_get_value_as_string(prop));
    if (!email) {
        json_decref(p);
        return NULL;
    }
    json_object_set_new(p, "email", json_string(email));
    free(email);

    /* name */
    const char *name = NULL;
    param = icalproperty_get_first_parameter(prop, ICAL_CN_PARAMETER);
    if (param) {
        name = icalparameter_get_cn(param);
    }
    json_object_set_new(p, "name", json_string(name ? name : ""));

    /* kind */
    const char *kind = NULL;
    param = icalproperty_get_first_parameter(prop, ICAL_CUTYPE_PARAMETER);
    if (param) {
        icalparameter_cutype cutype = icalparameter_get_cutype(param);
        switch (cutype) {
            case ICAL_CUTYPE_INDIVIDUAL:
                kind = "individual";
                break;
            case ICAL_CUTYPE_GROUP:
                kind = "group";
                break;
            case ICAL_CUTYPE_RESOURCE:
                kind = "resource";
                break;
            case ICAL_CUTYPE_ROOM:
                kind = "location";
                break;
            default:
                kind = "unknown";
        }
    }
    if (kind) {
        json_object_set_new(p, "kind", json_string(kind));
    }

    /* participation */
    const char *participation = NULL;
    icalparameter_role ical_role = ICAL_ROLE_REQPARTICIPANT;
    param = icalproperty_get_first_parameter(prop, ICAL_ROLE_PARAMETER);
    if (param) {
        ical_role = icalparameter_get_role(param);
        switch (ical_role) {
            case ICAL_ROLE_REQPARTICIPANT:
                participation = "required";
                break;
            case ICAL_ROLE_OPTPARTICIPANT:
                participation = "optional";
                break;
            case ICAL_ROLE_NONPARTICIPANT:
                participation = "non-participant";
                break;
            case ICAL_ROLE_CHAIR:
                /* fall through */
            default:
                participation = "required";
        }
    }
    if (participation) {
        json_object_set_new(p, "participation", json_string(participation));
    }

    /* roles */
    json_t *roles = json_array();
    int seen_owner = 0;
    if (ical_role == ICAL_ROLE_CHAIR)
        json_array_append_new(roles, json_string("chair"));
    for (param = icalproperty_get_first_parameter(prop, ICAL_X_PARAMETER);
         param;
         param = icalproperty_get_next_parameter(prop, ICAL_X_PARAMETER)) {

        if (strcmp(icalparameter_get_xname(param), JMAPICAL_XPARAM_ROLE))
            continue;

        buf_setcstr(&buf, icalparameter_get_xvalue(param));
        json_array_append_new(roles, json_string(buf_lcase(&buf)));
        if (!seen_owner) seen_owner = !strcmp(buf_cstring(&buf), "owner");
    }
    if (!seen_owner && orga) {
        const char *o = icalproperty_get_organizer(orga);
        const char *a = icalproperty_get_attendee(prop);
        if (!strcasecmp(o, a))
            json_array_append_new(roles, json_string("owner"));
    }
    if (!json_array_size(roles)) {
        json_array_append_new(roles, json_string("attendee"));
    }
    json_object_set_new(p, "roles", roles);

    /* locationId */
    const char *locid;
    if ((locid = get_icalxparam_value(prop, JMAPICAL_XPARAM_LOCATIONID))) {
        json_object_set_new(p, "locationId", json_string(locid));
    }

    /* rsvpResponse */
    const char *rsvp = NULL;
    short depth = 0;
    icalproperty *rsvp_prop = prop;
    while (!rsvp) {
        param = icalproperty_get_first_parameter(rsvp_prop, ICAL_PARTSTAT_PARAMETER);
        if (!param) {
            rsvp = "needs-action";
            break;
        }
        icalparameter_partstat pst = icalparameter_get_partstat(param);
        switch (pst) {
            case ICAL_PARTSTAT_ACCEPTED:
                rsvp = "accepted";
                break;
            case ICAL_PARTSTAT_DECLINED:
                rsvp = "declined";
                break;
            case ICAL_PARTSTAT_TENTATIVE:
                rsvp = "tentative";
                break;
            case ICAL_PARTSTAT_DELEGATED:
                /* Follow the delegate chain */
                param = icalproperty_get_first_parameter(prop, ICAL_DELEGATEDTO_PARAMETER);
                if (param) {
                    const char *to = icalparameter_get_delegatedto(param);
                    if (!to) continue;
                    rsvp_prop = hash_lookup(to, hatts);
                    if (rsvp_prop) {
                        /* Determine PARTSTAT from delegate. */
                        if (++depth > 64) {
                            /* This is a pathological case: libical does
                             * not check for infinite DELEGATE chains, so we
                             * make sure not to fall in an endless loop. */
                            rsvp = "needs-action";
                        }
                        continue;
                    }
                }
                /* fallthrough */
            default:
                rsvp = "needs-action";
        }
    }
    if (rsvp) {
        json_object_set_new(p, "rsvpResponse", json_string(rsvp));
    }

    /* rsvpWanted */
    param = icalproperty_get_first_parameter(prop, ICAL_RSVP_PARAMETER);
    if (param) {
        icalparameter_rsvp val = icalparameter_get_rsvp(param);
        json_object_set_new(p, "rsvpWanted",
                json_boolean(val == ICAL_RSVP_TRUE));
    }

    /* delegatedTo */
    json_t *delegatedTo = json_array();
    for (param = icalproperty_get_first_parameter(prop, ICAL_DELEGATEDTO_PARAMETER);
         param;
         param = icalproperty_get_next_parameter(prop, ICAL_DELEGATEDTO_PARAMETER)) {

        char *tmp = mailaddr_from_uri(icalparameter_get_delegatedto(param));
        if (!tmp)
            continue;
        json_array_append_new(delegatedTo, json_string(tmp));
        free(tmp);
    }
    if (json_array_size(delegatedTo)) {
        json_object_set_new(p, "delegatedTo", delegatedTo);
    }
    else {
        json_decref(delegatedTo);
    }

    /* delegatedFrom */
    json_t *delegatedFrom = json_array();
    for (param = icalproperty_get_first_parameter(prop, ICAL_DELEGATEDFROM_PARAMETER);
         param;
         param = icalproperty_get_next_parameter(prop, ICAL_DELEGATEDFROM_PARAMETER)) {

        char *tmp = mailaddr_from_uri(icalparameter_get_delegatedfrom(param));
        if (!tmp)
            continue;
        json_array_append_new(delegatedFrom, json_string(tmp));
        free(tmp);
    }
    if (json_array_size(delegatedFrom)) {
        json_object_set_new(p, "delegatedFrom", delegatedFrom);
    }
    else {
        json_decref(delegatedFrom);
    }

    /* memberof */
    json_t *memberOf = json_pack("[]");
    for (param = icalproperty_get_first_parameter(prop, ICAL_MEMBER_PARAMETER);
         param;
         param = icalproperty_get_next_parameter(prop, ICAL_MEMBER_PARAMETER)) {

        char *tmp = mailaddr_from_uri(icalparameter_get_member(param));
        if (!tmp) continue;
        json_array_append_new(memberOf, json_string(tmp));
        free(tmp);
    }
    if (json_array_size(memberOf)) {
        json_object_set_new(p, "memberOf", memberOf);
    } else {
        json_decref(memberOf);
    }

    /* linkIds */
    json_t *linkIds = json_array();
    for (param = icalproperty_get_first_parameter(prop, ICAL_X_PARAMETER);
         param;
         param = icalproperty_get_next_parameter(prop, ICAL_X_PARAMETER)) {

        if (strcmp(icalparameter_get_xname(param), JMAPICAL_XPARAM_LINKID))
            continue;

        buf_setcstr(&buf, icalparameter_get_xvalue(param));
        json_array_append_new(linkIds, json_string(buf_lcase(&buf)));
    }
    if (json_array_size(linkIds)) {
        json_object_set_new(p, "linkIds", linkIds);
    }
    else {
        json_decref(linkIds);
    } 

    /* scheduleSequence */
    const char *xval = get_icalxparam_value(prop, JMAPICAL_XPARAM_SEQUENCE);
    if (xval) {
        bit64 res;
        if (parsenum(xval, &xval, strlen(xval), &res) == 0) {
            json_object_set_new(p, "scheduleSequence", json_integer(res));
        }
    }

    /* scheduleUpdated */
    if ((xval = get_icalxparam_value(prop, JMAPICAL_XPARAM_DTSTAMP))) {
        icaltimetype dtstamp = icaltime_from_string(xval);
        if (!icaltime_is_null_time(dtstamp) && !dtstamp.is_date &&
                dtstamp.zone == icaltimezone_get_utc_timezone()) {
            char *tmp = utcdate_from_icaltime_r(dtstamp);
            json_object_set_new(p, "scheduleUpdated", json_string(tmp));
            free(tmp);
        }
    }

    buf_free(&buf);
    return p;
}

/* Convert the ical ORGANIZER/ATTENDEEs in comp to CalendarEvent participants */
static json_t*
participants_from_ical(context_t *ctx __attribute__((unused)),
                       icalcomponent *comp)
{
    struct hash_table attmap;
    icalproperty *prop;
    json_t *participants = json_object();

    /* Collect all attendees in a map to lookup delegates. */
    construct_hash_table(&attmap, 32, 0);
    for (prop = icalcomponent_get_first_property(comp, ICAL_ATTENDEE_PROPERTY);
         prop;
         prop = icalcomponent_get_next_property(comp, ICAL_ATTENDEE_PROPERTY)) {

        hash_insert(icalproperty_get_value_as_string(prop), prop, &attmap);
    }
    if (!hash_numrecords(&attmap)) {
        goto done;
    }


    /* Traverse ATTENDEES - find organizer first to not mess up the iterator */
    icalproperty *orga = icalcomponent_get_first_property(comp, ICAL_ORGANIZER_PROPERTY);
    for (prop = icalcomponent_get_first_property(comp, ICAL_ATTENDEE_PROPERTY);
         prop;
         prop = icalcomponent_get_next_property(comp, ICAL_ATTENDEE_PROPERTY)) {

        json_t *p = participant_from_ical(prop, &attmap, orga);
        char *id = xstrdupnull(get_icalxparam_value(prop, JMAPICAL_XPARAM_ID));
        if (!id) id = mailaddr_from_uri(icalproperty_get_attendee(prop));
        json_object_set_new(participants, id, p);
        free(id);
    }

done:
    if (!json_object_size(participants)) {
        json_decref(participants);
        participants = json_null();
    }
    free_hash_table(&attmap, NULL);
    return participants;
}

static json_t*
link_from_ical(context_t *ctx __attribute__((unused)), icalproperty *prop)
{
    /* href */
    const char *href = NULL;
    if (icalproperty_isa(prop) == ICAL_ATTACH_PROPERTY) {
        icalattach *attach = icalproperty_get_attach(prop);
        /* Ignore ATTACH properties with value BINARY. */
        if (!attach || !icalattach_get_is_url(attach)) {
            return NULL;
        }
        href = icalattach_get_url(attach);
    }
    else if (icalproperty_isa(prop) == ICAL_X_PROPERTY) {
        href = icalproperty_get_value_as_string(prop);
    }
    if (!href || *href == '\0') return NULL;

    json_t *link = json_pack("{s:s}", "href", href);
    icalparameter *param = NULL;
    const char *s;

    /* cid */
    if ((s = get_icalxparam_value(prop, JMAPICAL_XPARAM_CID))) {
        json_object_set_new(link, "cid", json_string(s));
    }

    /* type */
    param = icalproperty_get_first_parameter(prop, ICAL_FMTTYPE_PARAMETER);
    if (param && ((s = icalparameter_get_fmttype(param)))) {
        json_object_set_new(link, "type", json_string(s));
    }

    /* title - reuse the same x-param as Apple does for their locations  */
    if ((s = get_icalxparam_value(prop, JMAPICAL_XPARAM_TITLE))) {
        json_object_set_new(link, "title", json_string(s));
    }

    /* properties */
    if ((s = get_icalxparam_value(prop, JMAPICAL_XPARAM_PROPERTIES))) {
        json_t *p = decode_base64_json(s);
        json_object_set_new(link, "properties", p ? p : json_null());
    }

    /* size */
    json_int_t size = -1;
    param = icalproperty_get_size_parameter(prop);
    if (param) {
        if ((s = icalparameter_get_size(param))) {
            char *ptr;
            size = strtol(s, &ptr, 10);
            json_object_set_new(link, "size",
                    ptr && *ptr == '\0' ? json_integer(size) : json_null());
        }
    }

    /* rel */
    if ((s = get_icalxparam_value(prop, JMAPICAL_XPARAM_REL))) {
        json_object_set_new(link, "rel", json_string(s));
    }

    return link;
}

static json_t*
links_from_ical(context_t *ctx, icalcomponent *comp, const char *idprefix)
{
    icalproperty* prop;
    json_t *ret = json_pack("{}");
    struct buf buf = BUF_INITIALIZER;

    /* Read iCalendar ATTACH properties */
    for (prop = icalcomponent_get_first_property(comp, ICAL_ATTACH_PROPERTY);
         prop;
         prop = icalcomponent_get_next_property(comp, ICAL_ATTACH_PROPERTY)) {

        const char *id;
        if (!(id = get_icalxparam_value(prop, JMAPICAL_XPARAM_ID))) {
            buf_reset(&buf);
            buf_printf(&buf, "%s%zu", idprefix, json_object_size(ret) + 1);
            id = buf_cstring(&buf);
        }

        beginprop_key(ctx, "links", id);
        json_t *link = link_from_ical(ctx, prop);
        if (!link) continue;
        endprop(ctx);

        json_object_set_new(ret, id, link);
    }

    /* Read iCalendar X-ATTACH properties. They look the same as ATTACH,
     * but might occur at places where ATTACH is forbidden or restricted
     * to a single occurence. */
    for (prop = icalcomponent_get_first_property(comp, ICAL_X_PROPERTY);
         prop;
         prop = icalcomponent_get_next_property(comp, ICAL_X_PROPERTY)) {

        if (strcasecmp(icalproperty_get_x_name(prop), JMAPICAL_XPROP_ATTACH)) {
            continue;
        }

        const char *id;
        if (!(id = get_icalxparam_value(prop, JMAPICAL_XPARAM_ID))) {
            buf_reset(&buf);
            buf_printf(&buf, "%s%zu", idprefix, json_object_size(ret) + 1);
            id = buf_cstring(&buf);
        }

        beginprop_key(ctx, "links", id);
        json_t *link = link_from_ical(ctx, prop);
        if (!link) continue;
        endprop(ctx);

        json_object_set_new(ret, id, link);
    }

    if (!json_object_size(ret)) {
        json_decref(ret);
        ret = json_null();
    }

    buf_free(&buf);
    return ret;
}

static json_t*
htmldescription_from_ical(context_t *ctx __attribute__((unused)), icalcomponent *comp)
{
   icalproperty *prop =icalcomponent_get_first_property(comp, ICAL_DESCRIPTION_PROPERTY);
   if (!prop) return json_null();

    icalparameter *altrep = icalproperty_get_first_parameter(prop, ICAL_ALTREP_PARAMETER);
    if (!altrep) return json_null();

    const char *uri = icalparameter_get_altrep(altrep);
    if (strncasecmp(uri, "data:text/html,", 15)) return json_null();
    return json_string(uri + 15);
}

static json_t *alert_emailaction_from_ical(context_t *ctx, icalcomponent *alarm)
{
    json_t *to = json_pack("[]");
    icalproperty *prop = NULL;
    icalparameter *param = NULL;
    json_t *action = NULL;
    const char *s = NULL;

    for (prop = icalcomponent_get_first_property(alarm, ICAL_ATTENDEE_PROPERTY);
         prop;
         prop = icalcomponent_get_next_property(alarm, ICAL_ATTENDEE_PROPERTY)) {

        const char *name = NULL;
        char *email;

        /* email */
        email = mailaddr_from_uri(icalproperty_get_value_as_string(prop));
        if (!email) {
            continue;
        }

        /* name */
        param = icalproperty_get_first_parameter(prop, ICAL_CN_PARAMETER);
        if (param) {
            name = icalparameter_get_cn(param);
        }

        json_array_append_new(to, json_pack("{s:s s:s}", "name", name ? name : "", "email", email));
        free(email);
    }
    if (!json_array_size(to)) {
        json_decref(to);
        goto done;
    }
    action = json_pack("{s:s s:o}", "type", "email", "to", to);

    /* subject */
    prop = icalcomponent_get_first_property(alarm, ICAL_SUMMARY_PROPERTY);
    if (prop && (s = icalproperty_get_summary(prop))) {
        json_object_set_new(action, "subject", json_string(s));
    }
    /* textBody */
    prop = icalcomponent_get_first_property(alarm, ICAL_DESCRIPTION_PROPERTY);
    if (prop && (s = icalproperty_get_description(prop))) {
        json_object_set_new(action, "textBody", json_string(s));
    }

    /* htmlBody */
    json_t *htmlBody = htmldescription_from_ical(ctx, alarm);
    if (JNOTNULL(htmlBody)) {
        json_object_set_new(action, "htmlBody", htmlBody);
    }

    /* attachments */
    json_t *attachments = links_from_ical(ctx, alarm, "alertAttachment");
    if (JNOTNULL(attachments)) {
        json_object_set_new(action, "attachments", attachments);
    }

done:
    return action;
}

static json_t *alertaction_from_ical(context_t *ctx,
                                     hash_table *snoozes,
                                     icalcomponent *alarm)
{
    json_t *action = NULL;
    icalproperty* prop;
    icalvalue* val;
    icalproperty_action icalaction;
    const char *uid;

    prop = icalcomponent_get_first_property(alarm, ICAL_ACTION_PROPERTY);
    if (!prop) goto done;

    val = icalproperty_get_value(prop);
    if (!val) goto done;

    icalaction = icalvalue_get_action(val);

    if (icalaction == ICAL_ACTION_EMAIL) {
        action = alert_emailaction_from_ical(ctx, alarm);
    } else if (icalaction == ICAL_ACTION_DISPLAY || icalaction == ICAL_ACTION_AUDIO) {
        action = json_pack("{s:s}", "type", "display");
        /* mediaLinks */
        json_t *mediaLinks = links_from_ical(ctx, alarm, "alertMediaLink");
        if (JNOTNULL(mediaLinks)) {
            json_object_set_new(action, "mediaLinks", mediaLinks);
        }
    }
    if (!action) {
        goto done;
    }

    /* acknowledged */
    if ((prop = icalcomponent_get_acknowledged_property(alarm))) {
        icaltimetype t = icalproperty_get_acknowledged(prop);
        if (icaltime_is_valid_time(t)) {
            char *val = utcdate_from_icaltime_r(t);
            json_object_set_new(action, "acknowledged", json_string(val));
            free(val);
        }
    }

    /* snoozed */
    icalcomponent *snooze;
    if ((uid = icalcomponent_get_uid(alarm)) &&
        (snooze = hash_lookup(uid, snoozes)) &&
        (prop = icalcomponent_get_first_property(snooze,
                                                 ICAL_TRIGGER_PROPERTY))) {

        icaltimetype t = icalproperty_get_trigger(prop).time;
        if (!icaltime_is_null_time(t) && icaltime_is_valid_time(t)) {
            char *val = utcdate_from_icaltime_r(t);
            json_object_set_new(action, "snoozed", json_string(val));
            free(val);
        }
    }

done:
    return action;
}

/* Convert the VALARMS in the VEVENT comp to CalendarEvent alerts.
 * Adds any ATTACH properties found in VALARM components to the
 * event 'links' property. */
static json_t*
alerts_from_ical(context_t *ctx, icalcomponent *comp)
{
    json_t* alerts = json_pack("{}");
    icalcomponent* alarm;
    hash_table snoozes;
    ptrarray_t alarms = PTRARRAY_INITIALIZER;

    construct_hash_table(&snoozes, 32, 0);

    /* Split VALARMS into regular alerst and their snoozing VALARMS */
    for (alarm = icalcomponent_get_first_component(comp, ICAL_VALARM_COMPONENT);
         alarm;
         alarm = icalcomponent_get_next_component(comp, ICAL_VALARM_COMPONENT)) {

        icalproperty* prop = NULL;
        icalparameter *param = NULL;
        const char *uid = NULL;

        /* Check for RELATED-TO property... */
        prop = icalcomponent_get_first_property(alarm, ICAL_RELATEDTO_PROPERTY);
        if (!prop) {
            ptrarray_push(&alarms, alarm);
            continue;
        }
        /* .. that has a UID value... */
        uid = icalproperty_get_value_as_string(prop);
        if (!uid || !strlen(uid)) {
            ptrarray_push(&alarms, alarm);
            continue;
        }
        /* ... and it's RELTYPE is set to SNOOZE */
        param = icalproperty_get_first_parameter(prop, ICAL_RELTYPE_PARAMETER);
        if (!param || strcasecmp(icalparameter_get_xvalue(param), "SNOOZE")) {
            ptrarray_push(&alarms, alarm);
            continue;
        }

        /* Must be a SNOOZE alarm */
        hash_insert(uid, alarm, &snoozes);
    }

    while ((alarm = (icalcomponent*) ptrarray_pop(&alarms))) {
        icalproperty* prop;
        icalparameter *param;
        struct icaltriggertype trigger;
        icalparameter_related related = ICAL_RELATED_START;

        json_t *action, *alert;
        const char *relativeTo;
        char *id, *offset;
        struct icaldurationtype duration;

        relativeTo = offset = id = NULL;

        /* alert id */
        id = (char *) icalcomponent_get_uid(alarm);
        if (!id) {
            id = hexkey(icalcomponent_as_ical_string(alarm));
        } else {
            id = xstrdup(id);
        }
        beginprop_key(ctx, "alerts", id);

        /* Determine TRIGGER */
        prop = icalcomponent_get_first_property(alarm, ICAL_TRIGGER_PROPERTY);
        if (!prop) {
            goto done;
        }
        trigger = icalproperty_get_trigger(prop);

        /* Determine RELATED parameter */
        param = icalproperty_get_first_parameter(prop, ICAL_RELATED_PARAMETER);
        if (param) {
            related = icalparameter_get_related(param);
            if (related != ICAL_RELATED_START && related != ICAL_RELATED_END) {
                goto done;
            }
        }

        /* Determine duration between alarm and start/end */
        if (!icaldurationtype_is_null_duration(trigger.duration) ||
             icaltime_is_null_time(trigger.time)) {
            duration = trigger.duration;
        } else {
            icaltimetype ttrg, tref;
            icaltimezone *utc = icaltimezone_get_utc_timezone();

            ttrg = icaltime_convert_to_zone(trigger.time, utc);
            if (related == ICAL_RELATED_START) {
                tref = icaltime_convert_to_zone(dtstart_from_ical(comp), utc);
            } else {
                tref = icaltime_convert_to_zone(dtend_from_ical(comp), utc);
            }
            duration = icaltime_subtract(ttrg, tref);
        }

        /* action */
        beginprop(ctx, "action");
        action = alertaction_from_ical(ctx, &snoozes, alarm);
        endprop(ctx);
        if (!action) {
            goto done;
        }

        /* relativeTo */
        if (duration.is_neg) {
            relativeTo = related == ICAL_RELATED_START ?
                "before-start" : "before-end";
        } else {
            relativeTo = related == ICAL_RELATED_START ?
                "after-start" : "after-end";
        }

        /* offset*/
        duration.is_neg = 0;
        offset = icaldurationtype_as_ical_string_r(duration);
        alert = json_pack("{s:s s:s s:o}",
                "relativeTo", relativeTo,
                "offset", offset,
                "action", action);
        json_object_set_new(alerts, id, alert);
        free(offset);
done:
        endprop(ctx);
        free(id);
    }

    if (!json_object_size(alerts)) {
        json_decref(alerts);
        alerts = json_null();
    }

    ptrarray_fini(&alarms);
    free_hash_table(&snoozes, NULL);
    return alerts;
}



/* Convert a VEVENT ical component to CalendarEvent keywords */
static json_t*
keywords_from_ical(context_t *ctx __attribute__((unused)), icalcomponent *comp)
{
    icalproperty* prop;
    json_t *ret = json_array();

    for (prop = icalcomponent_get_first_property(comp, ICAL_CATEGORIES_PROPERTY);
         prop;
         prop = icalcomponent_get_next_property(comp, ICAL_CATEGORIES_PROPERTY)) {
        json_array_append_new(ret, json_string(icalproperty_get_categories(prop)));
    }
    if (!json_array_size(ret)) {
        json_decref(ret);
        ret = json_null();
    }

    return ret;
}

/* Convert a VEVENT ical component to CalendarEvent relatedTo */
static json_t*
relatedto_from_ical(context_t *ctx __attribute__((unused)), icalcomponent *comp)
{
    icalproperty* prop;
    json_t *ret = json_pack("{}");

    for (prop = icalcomponent_get_first_property(comp, ICAL_RELATEDTO_PROPERTY);
         prop;
         prop = icalcomponent_get_next_property(comp, ICAL_RELATEDTO_PROPERTY)) {

        const char *uid, *reltype;
        char *s = NULL;
        icalparameter *param;

        param = icalproperty_get_first_parameter(prop, ICAL_RELTYPE_PARAMETER);
        if (!param) continue;

        reltype = icalparameter_get_xvalue(param);
        if (!reltype || !strlen(reltype)) continue;

        uid = icalproperty_get_value_as_string(prop);
        if (!uid || !strlen(uid)) continue;

        s = lcase(xstrdup(reltype));
        json_object_set_new(ret, s, json_string(uid));
        free(s);
    }

    if (!json_object_size(ret)) {
        json_decref(ret);
        ret = json_null();
    }

    return ret;
}

static json_t *location_features_from_ical(icalparameter *param)
{
    const char *val = icalparameter_get_xvalue(param);
    if (!val) return json_array();

    struct buf buf = BUF_INITIALIZER;
    json_t *features = json_array();
    if (strchr(val, ',')) {
        /* libical doesn't split a comma-separatured list of features,
         * so it's treated as x-value. Split by our own */
        strarray_t *icalfeatures = strarray_split(val, ",", STRARRAY_TRIM);
        int i;
        for (i = 0; i < icalfeatures->count; i++) {
            buf_setcstr(&buf, strarray_nth(icalfeatures, i));
            json_array_append_new(features, json_string(buf_lcase(&buf)));
        }
        strarray_free(icalfeatures);
    } else {
        buf_setcstr(&buf, val);
        json_array_append_new(features, json_string(buf_lcase(&buf)));
    }
    buf_free(&buf);
    return features;
}

static json_t* location_from_ical(context_t *ctx __attribute__((unused)), icalproperty *prop)
{
    icalparameter *param;
    json_t *loc = json_object();

    /* name, uri and rel */
    const char *name = NULL;
    const char *uri = NULL;
    const char *rel = get_icalxparam_value(prop, JMAPICAL_XPARAM_REL);

    if (icalproperty_isa(prop) == ICAL_CONFERENCE_PROPERTY) {
        uri = icalproperty_get_value_as_string(prop);
        param = icalproperty_get_first_parameter(prop, ICAL_LABEL_PARAMETER);
        if (param) name = icalparameter_get_label(param);
        if (!rel) rel = "virtual";
    } else {
        name = icalvalue_get_text(icalproperty_get_value(prop));
        param = icalproperty_get_first_parameter(prop, ICAL_ALTREP_PARAMETER);
        if (param) uri = icalparameter_get_altrep(param);
        if (!rel) rel = "unknown";
    }
    if (!rel) rel = "unknown";

    json_object_set_new(loc, "name", json_string(name ? name : ""));
    json_object_set_new(loc, "uri", uri ? json_string(uri) : json_null());
    json_object_set_new(loc, "rel", json_string(rel));

    /* features */
    json_t *features = json_array();
    if (icalproperty_isa(prop) == ICAL_CONFERENCE_PROPERTY) {
        /* Read from FEATUREs parameter */
        for (param = icalproperty_get_first_parameter(prop, ICAL_FEATURE_PARAMETER);
             param;
             param = icalproperty_get_next_parameter(prop, ICAL_FEATURE_PARAMETER)) {
            const char *val = NULL;
            switch (icalparameter_get_feature(param)) {
                case ICAL_FEATURE_AUDIO:
                    val = "audio";
                    break;
                case ICAL_FEATURE_CHAT:
                    val = "chat";
                    break;
                case ICAL_FEATURE_FEED:
                    val = "feed";
                    break;
                case ICAL_FEATURE_MODERATOR:
                    val = "moderator";
                    break;
                case ICAL_FEATURE_PHONE:
                    val = "phone";
                    break;
                case ICAL_FEATURE_SCREEN:
                    val = "screen";
                    break;
                case ICAL_FEATURE_VIDEO:
                    val = "video";
                    break;
                case ICAL_FEATURE_X:
                case ICAL_FEATURE_NONE:
                default:
                    val = NULL;
            }
            if (val) {
                json_array_append_new(features, json_string(val));
            } else {
                json_t *l = location_features_from_ical(param);
                json_array_extend(features, l);
                json_decref(l);
            }
        }
    } else {
        /* Read features from X-JMAP-FEATURE parameters */
        for (param = icalproperty_get_first_parameter(prop, ICAL_X_PARAMETER);
             param;
             param = icalproperty_get_next_parameter(prop, ICAL_X_PARAMETER)) {

            if (strcmp(icalparameter_get_xname(param), JMAPICAL_XPARAM_FEATURE))
                continue;
            json_t *l = location_features_from_ical(param);
            json_array_extend(features, l);
            json_decref(l);
        }
    }
    if (!json_array_size(features)) {
        json_decref(features);
        features = json_null();
    }
    json_object_set_new(loc, "features", features);

    /* description */
    const char *desc = get_icalxparam_value(prop, JMAPICAL_XPARAM_DESCRIPTION);
    json_object_set_new(loc, "description", desc ? json_string(desc) : json_null());

    /* linkIds */
    json_t *linkids = json_array();
    for (param = icalproperty_get_first_parameter(prop, ICAL_X_PARAMETER);
         param;
         param = icalproperty_get_next_parameter(prop, ICAL_X_PARAMETER)) {

        if (strcasecmp(icalparameter_get_xname(param), JMAPICAL_XPARAM_LINKID)) {
            continue;
        }
        const char *s = icalparameter_get_xvalue(param);
        if (!s) continue;
        json_array_append_new(linkids, json_string(s));
    }
    if (!json_array_size(linkids)) {
        json_decref(linkids);
        linkids = json_null();
    }
    json_object_set_new(loc, "linkIds", linkids);

    /* timeZone */
    const char *tzid = get_icalxparam_value(prop, JMAPICAL_XPARAM_TZID);
    json_object_set_new(loc, "timeZone", tzid ? json_string(tzid) : json_null());

    /* coordinates */
    const char *coord = get_icalxparam_value(prop, JMAPICAL_XPARAM_GEO);
    json_object_set_new(loc, "coordinates", coord ? json_string(coord) : json_null());

    return loc;
}

static json_t *coordinates_from_ical(icalproperty *prop)
{
    /* Use verbatim coordinate string, rather than the parsed ical value */
    const char *p, *val = icalproperty_get_value_as_string(prop);
    struct buf buf = BUF_INITIALIZER;
    json_t *c;

    p = strchr(val, ';');
    if (!p) return NULL;

    buf_setcstr(&buf, "geo:");
    buf_appendmap(&buf, val, p-val);
    buf_appendcstr(&buf, ",");
    val = p + 1;
    buf_appendcstr(&buf, val);

    c = json_string(buf_cstring(&buf));
    buf_free(&buf);
    return c;
}

static json_t*
locations_from_ical(context_t *ctx, icalcomponent *comp)
{
    icalproperty* prop;
    json_t *loc, *locations = json_pack("{}");
    char *id;

    /* Handle end locations */
    const char *tzidstart = tzid_from_ical(comp, ICAL_DTSTART_PROPERTY);
    const char *tzidend = tzid_from_ical(comp, ICAL_DTEND_PROPERTY);

    if (tzidstart && tzidend && strcmp(tzidstart, tzidend)) {
        prop = icalcomponent_get_first_property(comp, ICAL_DTEND_PROPERTY);
        id = xjmapid_from_ical(prop);
        loc = json_pack("{s:s s:s}", "timeZone", tzidend, "rel", "end");
        json_object_set_new(locations, id, loc);
        free(id);
    }

    /* LOCATION */
    if ((prop = icalcomponent_get_first_property(comp, ICAL_LOCATION_PROPERTY))) {
        id = xjmapid_from_ical(prop);
        beginprop_key(ctx, "locations", id);
        if ((loc = location_from_ical(ctx, prop))) {
            json_object_set_new(locations, id, loc);
        }
        endprop(ctx);
        free(id);
    }

    /* GEO */
    if ((prop = icalcomponent_get_first_property(comp, ICAL_GEO_PROPERTY))) {
        json_t *coord = coordinates_from_ical(prop);
        if (coord) {
            loc = json_pack("{s:o}", "coordinates", coord);
            id = xjmapid_from_ical(prop);
            json_object_set_new(locations, id, loc);
            free(id);
        }
    }

    /* CONFERENCE */
    for (prop = icalcomponent_get_first_property(comp, ICAL_CONFERENCE_PROPERTY);
         prop;
         prop = icalcomponent_get_next_property(comp, ICAL_CONFERENCE_PROPERTY)) {

        id = xjmapid_from_ical(prop);
        beginprop_key(ctx, "locations", id);
        if ((loc = location_from_ical(ctx, prop))) {
            json_object_set_new(locations, id, loc);
        }
        endprop(ctx);
        free(id);
    }

    /* Lookup X-property locations */
    for (prop = icalcomponent_get_first_property(comp, ICAL_X_PROPERTY);
         prop;
         prop = icalcomponent_get_next_property(comp, ICAL_X_PROPERTY)) {

        const char *name = icalproperty_get_property_name(prop);

        /* X-APPLE-STRUCTURED-LOCATION */
        /* FIXME Most probably,
         * a X-APPLE-STRUCTURED-LOCATION may occur only once and
         * always comes with a LOCATION. But who knows for sure? */
        if (!strcmp(name, "X-APPLE-STRUCTURED-LOCATION")) {
            const char *title, *uri;
            icalvalue *val;

            val = icalproperty_get_value(prop);
            if (icalvalue_isa(val) != ICAL_URI_VALUE) continue;

            uri = icalvalue_as_ical_string(val);
            if (strncmp(uri, "geo:", 4)) continue;

            loc = json_pack("{s:s}", "coordinates", uri);
            if ((title = get_icalxparam_value(prop, JMAPICAL_XPARAM_TITLE))) {
                json_object_set_new(loc, "name", json_string(title));
            }

            id = xjmapid_from_ical(prop);
            json_object_set_new(locations, id, loc);
            free(id);
            continue;
        }

        if (strcmp(name, JMAPICAL_XPROP_LOCATION)) {
            continue;
        }

        /* X-JMAP-LOCATION */
        id = xjmapid_from_ical(prop);
        beginprop_key(ctx, "locations", id);
        loc = location_from_ical(ctx, prop);
        if (loc) json_object_set_new(locations, id, loc);
        free(id);
        endprop(ctx);
    }

    if (!json_object_size(locations)) {
        json_decref(locations);
        locations = json_null();
    }

    return locations;
}

static json_t* duration_from_ical(icalcomponent *comp)
{
    struct icaltimetype dtstart, dtend;
    char *val = NULL;

    dtstart = dtstart_from_ical(comp);
    dtend = dtend_from_ical(comp);

    if (!icaltime_is_null_time(dtend)) {
        time_t tstart, tend;
        struct icaldurationtype dur;

        tstart = icaltime_as_timet_with_zone(dtstart, dtstart.zone);
        tend = icaltime_as_timet_with_zone(dtend, dtend.zone);
        dur = icaldurationtype_from_int((int)(tend - tstart));

        if (!icaldurationtype_is_bad_duration(dur) && !dur.is_neg) {
            val = icaldurationtype_as_ical_string_r(dur);
        }
    }

    json_t *ret = json_string(val && strcmp(val, "PT0S") ? val : "P0D");
    if (val) free(val);
    return ret;
}

static json_t*
locale_from_ical(context_t *ctx __attribute__((unused)), icalcomponent *comp)
{
    icalproperty *sum, *dsc;
    icalparameter *param = NULL;
    const char *lang = NULL;

    sum = icalcomponent_get_first_property(comp, ICAL_SUMMARY_PROPERTY);
    dsc = icalcomponent_get_first_property(comp, ICAL_DESCRIPTION_PROPERTY);

    if (sum) {
        param = icalproperty_get_first_parameter(sum, ICAL_LANGUAGE_PARAMETER);
    }
    if (!param && dsc) {
        param = icalproperty_get_first_parameter(dsc, ICAL_LANGUAGE_PARAMETER);
    }
    if (param) {
        lang = icalparameter_get_language(param);
    }

    return lang ? json_string(lang) : json_null();
}

/* Convert the libical VEVENT comp to a CalendarEvent 
 *
 * parent: if not NULL, treat comp as a VEVENT exception
 * props:  if not NULL, only convert properties named as keys
 */
static json_t*
calendarevent_from_ical(context_t *ctx, icalcomponent *comp)
{
    icalproperty* prop;
    json_t *event, *wantprops;
    int is_exc = ctx->master != NULL;

    wantprops = NULL;
    if (ctx->wantprops && wantprop(ctx, "recurrenceOverrides") && !is_exc) {
        /* Fetch all properties if recurrenceOverrides are requested,
         * otherwise we might return incomplete override patches */
        wantprops = ctx->wantprops;
        ctx->wantprops = NULL;
    }

    event = json_pack("{s:s}", "@type", "jsevent");

    /* Always determine the event's start timezone. */
    ctx->tzid_start = tzid_from_ical(comp, ICAL_DTSTART_PROPERTY);

    /* Always determine isAllDay to set start, end and timezone fields. */
    ctx->is_allday = icaltime_is_date(icalcomponent_get_dtstart(comp));
    if (ctx->is_allday && ctx->tzid_start) {
        /* bogus iCalendar data */
        ctx->tzid_start = NULL;
    }

    /* isAllDay */
    if (wantprop(ctx, "isAllDay") && !is_exc) {
        json_object_set_new(event, "isAllDay", json_boolean(ctx->is_allday));
    }

    /* uid */
    const char *uid = icalcomponent_get_uid(comp);
    if (uid && !is_exc) {
        json_object_set_new(event, "uid", json_string(uid));
    }

    /* relatedTo */
    if (wantprop(ctx, "relatedTo") && !is_exc) {
        json_object_set_new(event, "relatedTo", relatedto_from_ical(ctx, comp));
    }

    /* prodId */
    if (wantprop(ctx, "prodId") && !is_exc) {
        icalcomponent *ical = icalcomponent_get_parent(comp);
        const char *prodid = NULL;
        prop = icalcomponent_get_first_property(ical, ICAL_PRODID_PROPERTY);
        if (prop) prodid = icalproperty_get_prodid(prop);
        json_object_set_new(event, "prodId",
                prodid ? json_string(prodid) : json_null());
    }

    /* created */
    if (wantprop(ctx, "created")) {
        json_t *val = json_null();
        prop = icalcomponent_get_first_property(comp, ICAL_CREATED_PROPERTY);
        if (prop) {
            char *t = utcdate_from_icaltime_r(icalproperty_get_created(prop));
            if (t) {
                val = json_string(t);
                free(t);
            }
        }
        json_object_set_new(event, "created", val);
    }

    /* updated */
    if (wantprop(ctx, "updated")) {
        json_t *val = json_null();
        prop = icalcomponent_get_first_property(comp, ICAL_DTSTAMP_PROPERTY);
        if (prop) {
            char *t = utcdate_from_icaltime_r(icalproperty_get_dtstamp(prop));
            if (t) {
                val = json_string(t);
                free(t);
            }
        }
        json_object_set_new(event, "updated", val);
    }

    /* sequence */
    if (wantprop(ctx, "sequence")) {
        json_object_set_new(event, "sequence",
                json_integer(icalcomponent_get_sequence(comp)));
    }

    /* priority */
    if (wantprop(ctx, "priority")) {
        prop = icalcomponent_get_first_property(comp, ICAL_PRIORITY_PROPERTY);
        if (prop) {
            json_object_set_new(event, "priority",
                    json_integer(icalproperty_get_priority(prop)));
        }
    }

    /* title */
    if (wantprop(ctx, "title")) {
        prop = icalcomponent_get_first_property(comp, ICAL_SUMMARY_PROPERTY);
        if (prop) {
            json_object_set_new(event, "title",
                                json_string(icalproperty_get_summary(prop)));
        } else {
            json_object_set_new(event, "title", json_string(""));
        }
        if (!wantprop(ctx, "title")) {
            json_object_del(event, "title");
        }
    }

    /* description */
    if (wantprop(ctx, "description")) {
        const char *desc = "";
        prop = icalcomponent_get_first_property(comp, ICAL_DESCRIPTION_PROPERTY);
        if (prop) {
            desc = icalproperty_get_description(prop);
            if (!desc) desc = "";
        }
        json_object_set_new(event, "description", json_string(desc));
        if (!wantprop(ctx, "description")) {
            json_object_del(event, "description");
        }
    }

    /* htmlDescription */
    if (wantprop(ctx, "htmlDescription")) {
        json_t *desc = htmldescription_from_ical(ctx, comp);
        json_object_set_new(event, "htmlDescription", desc);
        if (!wantprop(ctx, "htmlDescription"))
            json_object_del(event, "htmlDescription");
    }

    /* color */
    if (wantprop(ctx, "color")) {
        prop = icalcomponent_get_first_property(comp, ICAL_COLOR_PROPERTY);
        if (prop) {
            json_object_set_new(event, "color",
                    json_string(icalproperty_get_color(prop)));
        }
    }

    /* keywords */
    if (wantprop(ctx, "keywords")) {
        json_object_set_new(event, "keywords", keywords_from_ical(ctx, comp));
    }

    /* links */
    if (wantprop(ctx, "links")) {
        json_object_set_new(event, "links", links_from_ical(ctx, comp, "link"));
        if (!wantprop(ctx, "links")) {
            json_object_del(event, "links");
        }
    }

    /* locale */
    if (wantprop(ctx, "locale")) {
        json_object_set_new(event, "locale", locale_from_ical(ctx, comp));
    }

    /* locations */
    if (wantprop(ctx, "locations")) {
        json_object_set_new(event, "locations", locations_from_ical(ctx, comp));
        if (!wantprop(ctx, "locations")) {
            json_object_del(event, "locations");
        }
    }

    /* start */
    if (wantprop(ctx, "start")) {
        struct icaltimetype dt = icalcomponent_get_dtstart(comp);
        char *s = localdate_from_icaltime_r(dt);
        json_object_set_new(event, "start", json_string(s));
        free(s);
    }

    /* timeZone */
    if (wantprop(ctx, "timeZone")) {
        json_object_set_new(event, "timeZone",
                ctx->tzid_start && !ctx->is_allday ?
                json_string(ctx->tzid_start) : json_null());
    }

    /* duration */
    if (wantprop(ctx, "duration")) {
        json_object_set_new(event, "duration", duration_from_ical(comp));
    }

    /* recurrenceRule */
    if (wantprop(ctx, "recurrenceRule") && !is_exc) {
        json_object_set_new(event, "recurrenceRule",
                            recurrence_from_ical(ctx, comp));
    }

    /* status */
    if (wantprop(ctx, "status")) {
        const char *status;
        switch (icalcomponent_get_status(comp)) {
            case ICAL_STATUS_TENTATIVE:
                status = "tentative";
                break;
            case ICAL_STATUS_CONFIRMED:
                status = "confirmed";
                break;
            case ICAL_STATUS_CANCELLED:
                status = "cancelled";
                break;
            default:
                status = NULL;
        }
        if (status) json_object_set_new(event, "status", json_string(status));
    }

    /* freeBusyStatus */
    if (wantprop(ctx, "freeBusyStatus")) {
        const char *fbs = "busy";
        if ((prop = icalcomponent_get_first_property(comp,
                                                     ICAL_TRANSP_PROPERTY))) {
            if (icalproperty_get_transp(prop) == ICAL_TRANSP_TRANSPARENT) {
                fbs = "free";
            }
        }
        json_object_set_new(event, "freeBusyStatus", json_string(fbs));
    }

    /* privacy */
    if (wantprop(ctx, "privacy")) {
        const char *prv = "public";
        if ((prop = icalcomponent_get_first_property(comp,
                                                     ICAL_CLASS_PROPERTY))) {
            switch (icalproperty_get_class(prop)) {
                case ICAL_CLASS_CONFIDENTIAL:
                    prv = "secret";
                    break;
                case ICAL_CLASS_PRIVATE:
                    prv = "private";
                    break;
                default:
                    prv = "public";
            }
        }
        json_object_set_new(event, "privacy", json_string(prv));
    }

    /* replyTo */
    if (wantprop(ctx, "replyTo") && !is_exc) {
        json_object_set_new(event, "replyTo", replyto_from_ical(ctx, comp));
    }

    /* participants */
    if (wantprop(ctx, "participants")) {
        json_object_set_new(event, "participants",
                            participants_from_ical(ctx, comp));
    }

    /* useDefaultAlerts */
    if (wantprop(ctx, "useDefaultAlerts")) {
        const char *v = get_icalxprop_value(comp, JMAPICAL_XPROP_USEDEFALERTS);
        if (v && !strcasecmp(v, "true")) {
            json_object_set_new(event, "useDefaultAlerts", json_true());
        }
    }

    /* alerts */
    if (wantprop(ctx, "alerts")) {
        json_object_set_new(event, "alerts", alerts_from_ical(ctx, comp));
        if (!wantprop(ctx, "alerts")) {
            json_object_del(event, "alerts");
        }
    }

    /* recurrenceOverrides - must be last to generate patches */
    if (wantprop(ctx, "recurrenceOverrides") && !is_exc) {
        json_object_set_new(event, "recurrenceOverrides",
                            overrides_from_ical(ctx, comp, event));
    }

    if (wantprops) {
        /* Remove all properties that weren't requested by the caller. */
        json_t *tmp = json_pack("{}");
        void *iter = json_object_iter(wantprops);
        while (iter)
        {
            const char *key = json_object_iter_key(iter);
            json_object_set(tmp, key, json_object_get(event, key));
            iter = json_object_iter_next(wantprops, iter);
        }
        json_decref(event);
        event = tmp;
    }
    ctx->wantprops = wantprops;

    return event;
}

json_t*
jmapical_tojmap(icalcomponent *ical, json_t *props, jmapical_err_t *err)
{
    icalcomponent* comp;
    json_t *obj = NULL;
    context_t *ctx = context_new(props, err, JMAPICAL_READ_MODE);

    /* Locate the main VEVENT. */
    icalcomponent *firstcomp =
        icalcomponent_get_first_component(ical, ICAL_VEVENT_COMPONENT);
    for (comp = firstcomp;
         comp;
         comp = icalcomponent_get_next_component(ical, ICAL_VEVENT_COMPONENT)) {
        if (!icalcomponent_get_first_property(comp,
                                              ICAL_RECURRENCEID_PROPERTY)) {
            break;
        }
    }
    /* magic promote to toplevel for the first item */
    if (!comp) comp = firstcomp;
    if (!comp) {
        goto done;
    }

    /* Convert main VEVENT to JMAP. */
    obj = calendarevent_from_ical(ctx, comp);
    if (!obj) goto done;

done:
    context_free(ctx);
    return obj;
}

/*
 * Convert to iCalendar from JMAP
 */

/* defined in http_tzdist */
extern void icalcomponent_add_required_timezones(icalcomponent *ical);

/* Remove and deallocate any properties of kind in comp. */
static void remove_icalprop(icalcomponent *comp, icalproperty_kind kind)
{
    icalproperty *prop, *next;

    for (prop = icalcomponent_get_first_property(comp, kind);
         prop;
         prop = next) {

        next = icalcomponent_get_next_property(comp, kind);
        icalcomponent_remove_property(comp, prop);
        icalproperty_free(prop);
    }
}

/* Convert the JMAP local datetime in buf to tm time.
   Return non-zero on success. */
static int localdate_to_tm(const char *buf, struct tm *tm) {
    /* Initialize tm. We don't know about daylight savings time here. */
    memset(tm, 0, sizeof(struct tm));
    tm->tm_isdst = -1;

    /* Parse LocalDate. */
    const char *p = strptime(buf, "%Y-%m-%dT%H:%M:%S", tm);
    if (!p || *p) {
        return 0;
    }
    return 1;
}

/* Convert the JMAP local datetime formatted buf into ical datetime dt
 * using timezone tz. Return non-zero on success. */
static int localdate_to_icaltime(const char *buf,
                                 icaltimetype *dt,
                                 icaltimezone *tz,
                                 int is_allday) {
    struct tm tm;
    int r;
    char *s = NULL;
    icaltimetype tmp;
    int is_utc;
    size_t n;

    r = localdate_to_tm(buf, &tm);
    if (!r) return 0;

    if (is_allday && (tm.tm_sec || tm.tm_min || tm.tm_hour)) {
        return 0;
    }

    is_utc = tz == icaltimezone_get_utc_timezone();

    /* Can't use icaltime_from_timet_with_zone since it tries to convert
     * t from UTC into tz. Let's feed ical a DATETIME string, instead. */
    s = xcalloc(19, sizeof(char));
    n = strftime(s, 18, "%Y%m%dT%H%M%S", &tm);
    if (is_utc) {
        s[n]='Z';
    }
    tmp = icaltime_from_string(s);
    free(s);
    if (icaltime_is_null_time(tmp)) {
        return 0;
    }
    tmp.zone = tz;
    tmp.is_date = is_allday && tz == NULL;
    *dt = tmp;
    return 1;
}

static int utcdate_to_icaltime(const char *src,
                               icaltimetype *dt)
{
    struct buf buf = BUF_INITIALIZER;
    size_t len = strlen(src);
    int r;
    icaltimezone *utc = icaltimezone_get_utc_timezone();

    if (!len || src[len-1] != 'Z') {
        return 0;
    }

    buf_setmap(&buf, src, len-1);
    r = localdate_to_icaltime(buf_cstring(&buf), dt, utc, 0);
    buf_free(&buf);
    return r;
}

/* Add or overwrite the datetime property kind in comp. If tz is not NULL, set
 * the TZID parameter on the property. Also take care to purge conflicting
 * datetime properties such as DTEND and DURATION. */
static icalproperty *dtprop_to_ical(icalcomponent *comp,
                                    icaltimetype dt,
                                    icaltimezone *tz,
                                    int purge,
                                    enum icalproperty_kind kind) {
    icalproperty *prop;

    /* Purge existing property. */
    if (purge) {
        remove_icalprop(comp, kind);
    }

    /* Resolve DTEND/DURATION conflicts. */
    if (kind == ICAL_DTEND_PROPERTY) {
        remove_icalprop(comp, ICAL_DURATION_PROPERTY);
    } else if (kind == ICAL_DURATION_PROPERTY) {
        remove_icalprop(comp, ICAL_DTEND_PROPERTY);
    }

    /* backwards compatible way to set date or datetime */
    icalvalue *val =
        dt.is_date ? icalvalue_new_date(dt) : icalvalue_new_datetime(dt);
    assert(val);  // no way to return errors from here

    /* Set the new property. */
    prop = icalproperty_new(kind);
    icalproperty_set_value(prop, val);
    if (tz && !icaltime_is_utc(dt)) {
        icalparameter *param =
            icalproperty_get_first_parameter(prop, ICAL_TZID_PARAMETER);
        const char *tzid = icaltimezone_get_location(tz);
        if (param) {
            icalparameter_set_tzid(param, tzid);
        } else {
            icalproperty_add_parameter(prop,icalparameter_new_tzid(tzid));
        }
    }
    icalcomponent_add_property(comp, prop);
    return prop;
}

static int location_is_endtimezone(json_t *loc)
{
    const char *rel = json_string_value(json_object_get(loc, "rel"));
    if (!rel) return 0;
    return json_object_get(loc, "timeZone") && !strcmp(rel, "end");
}

/* Update the start and end properties of VEVENT comp, as defined by
 * the JMAP calendarevent event. */
static void
startend_to_ical(context_t *ctx, icalcomponent *comp, json_t *event)
{
    const char *tzid;
    int pe;
    const char *dur_old, *dur, *val, *endzoneid;
    struct icaltimetype dtstart_old, dtstart;
    int is_create = !(ctx->mode & JMAPICAL_UPDATE_MODE);
    json_t *locations;
    json_t *duration;

    /* Determine current timezone */
    tzid = tzid_from_ical(comp, ICAL_DTSTART_PROPERTY);
    if (tzid) {
        ctx->tzstart_old = tz_from_tzid(tzid);
    } else {
        ctx->tzstart_old = NULL;
    }

    /* Read new timezone */
    if (!json_is_null(json_object_get(event, "timeZone"))) {
        pe = readprop(ctx, event, "timeZone",
                      is_create && !ctx->is_allday, "s", &val);
        if (pe > 0) {
            /* Lookup the new timezone. */
            ctx->tzstart = tz_from_tzid(val);
            if (!ctx->tzstart) {
                invalidprop(ctx, "timeZone");
            }
        } else if (!pe) {
            ctx->tzstart = ctx->tzstart_old;
        }
    } else {
        ctx->tzstart = NULL;
    }
    if (is_create) {
        ctx->tzstart_old = ctx->tzstart;
    }

    /* Determine current end timezone */
    tzid = tzid_from_ical(comp, ICAL_DTEND_PROPERTY);
    if (tzid) {
        ctx->tzend_old = tz_from_tzid(tzid);
    } else {
        ctx->tzend_old = ctx->tzstart_old;
    }

    /* Read new end timezone */
    endzoneid = NULL;
    locations = json_object_get(event, "locations");
    if (locations && !json_is_null(locations)) {
        json_t *loc;
        const char *id;

        /* Pick the first location with timeZone and rel=end */
        json_object_foreach(json_object_get(event, "locations"), id, loc) {
            json_t *timeZone;

            if (!location_is_endtimezone(loc)) {
                continue;
            }
            endzoneid = id;

            /* Prepare prefix for error reporting */
            beginprop_key(ctx, "locations", id);

            timeZone = json_object_get(loc, "timeZone");
            if (!json_is_null(timeZone)) {
                tzid = json_string_value(json_object_get(loc, "timeZone"));
                if (tzid) {
                    ctx->tzend = tz_from_tzid(tzid);
                } else {
                    invalidprop(ctx, "timeZone");
                }
            } else {
                /* The end timeZone is set to floating time */
                ctx->tzend = NULL;
            }

            /* Make sure that both timezones are either floating time or not */
            if ((ctx->tzstart == NULL) != (ctx->tzend == NULL)) {
                invalidprop(ctx, "timeZone");
            }
            /* allDay requires floating time */
            if (ctx->is_allday && ctx->tzend) {
                invalidprop(ctx, "timeZone");
            }

            endprop(ctx);
            break;
        }
    } else if (json_is_null(locations)) {
        ctx->tzend = NULL;
    } else {
        ctx->tzend = ctx->tzend_old;
    }
    if (is_create) {
        ctx->tzend_old = endzoneid ? ctx->tzend : ctx->tzstart;
    }
    if (!endzoneid) {
        ctx->tzend = ctx->tzend_old;
    }

    /* Determine current duration */
    if (!is_create) {
        duration = duration_from_ical(comp);
        dur_old = json_string_value(duration);
    } else {
        duration = NULL;
        dur_old = "P0D";
    }

    /* Read new duration */
    pe = readprop(ctx, event, "duration", 0, "s", &dur);
    if (pe > 0) {
        struct icaldurationtype d = icaldurationtype_from_string(dur);
        if (!icaldurationtype_is_bad_duration(d)) {
            /* Make sure that pointer equality works */
            if (!strcmp(dur_old, dur)) {
                dur = dur_old;
            }
        } else {
            invalidprop(ctx, "duration");
        }
    } else {
        dur = dur_old;
    }
    if (ctx->is_allday && strchr(dur, 'T')) {
        invalidprop(ctx, "duration");
    }

    /* Determine current start */
    dtstart_old = dtstart_from_ical(comp);

    /* Read new start */
    dtstart = dtstart_old;
    pe = readprop(ctx, event, "start", is_create, "s", &val);
    if (pe > 0) {
        if (!localdate_to_icaltime(val, &dtstart,
                                   ctx->tzstart, ctx->is_allday)) {
            invalidprop(ctx, "start");
        }
    } else {
        dtstart = dtstart_old;
    }

    /* Bail out for property errors */
    if (have_invalid_props(ctx))
        return;

    /* Either all timezones float or none */
    assert((ctx->tzstart != NULL) == (ctx->tzend != NULL));

    /* Purge and rebuild start and end */
    remove_icalprop(comp, ICAL_DTSTART_PROPERTY);
    remove_icalprop(comp, ICAL_DTEND_PROPERTY);
    remove_icalprop(comp, ICAL_DURATION_PROPERTY);

    dtprop_to_ical(comp, dtstart, ctx->tzstart, 1, ICAL_DTSTART_PROPERTY);
    if (ctx->tzstart != ctx->tzend) {
        /* Add DTEND */
        icaltimetype dtend;
        icalproperty *prop;

        dtend = icaltime_add(dtstart, icaldurationtype_from_string(dur));
        dtend = icaltime_convert_to_zone(dtend, ctx->tzend);
        prop = dtprop_to_ical(comp, dtend, ctx->tzend, 1, ICAL_DTEND_PROPERTY);
        xjmapid_to_ical(prop, endzoneid);
    } else {
        /* Add DURATION */
        icalcomponent_set_duration(comp, icaldurationtype_from_string(dur));
    }

    json_decref(duration);
}

static void
participant_to_ical(context_t *ctx, icalproperty *prop, json_t *p)
{
    icalparameter *param;

    /* name */
    json_t *name = json_object_get(p, "name");
    if (json_is_string(name)) {
        param = icalparameter_new_cn(json_string_value(name));
        icalproperty_add_parameter(prop, param);
    }
    else if (JNOTNULL(name)) {
        invalidprop(ctx, "name");
    }

    /* kind */
    json_t *kind = json_object_get(p, "kind");
    if (json_is_string(kind)) {
        char *tmp = ucase(xstrdup(json_string_value(kind)));
        icalparameter_cutype cu;
        if (!strcmp(tmp, "LOCATION"))
            cu = ICAL_CUTYPE_ROOM;
        else
            cu = icalparameter_string_to_enum(tmp);
        switch (cu) {
            case ICAL_CUTYPE_INDIVIDUAL:
            case ICAL_CUTYPE_GROUP:
            case ICAL_CUTYPE_RESOURCE:
            case ICAL_CUTYPE_ROOM:
                param = icalparameter_new_cutype(cu);
                icalproperty_add_parameter(prop, param);
                break;
            default:
                /* ignore */ ;
        }
        free(tmp);
    }
    else if (JNOTNULL(kind)) {
        invalidprop(ctx, "kind");
    }

    /* participation */
    icalparameter_role ical_role = ICAL_ROLE_REQPARTICIPANT;
    json_t *participation = json_object_get(p, "participation");
    if (json_is_string(participation)) {
        const char *s = json_string_value(participation);
        if (!strcasecmp(s, "required")) {
            ical_role = ICAL_ROLE_REQPARTICIPANT;
        }
        else if (!strcasecmp(s, "optional")) {
            ical_role = ICAL_ROLE_OPTPARTICIPANT;
        }
        else if (!strcasecmp(s, "non-participant")) {
            ical_role = ICAL_ROLE_NONPARTICIPANT;
        }
        if (ical_role != ICAL_ROLE_REQPARTICIPANT) {
            icalproperty_add_parameter(prop, icalparameter_new_role(ical_role));
        }
    }
    else if (JNOTNULL(participation)) {
        invalidprop(ctx, "participation");
    }

    /* roles */
    json_t *roles = json_object_get(p, "roles");
    if (json_array_size(roles)) {
        size_t i;
        json_t *jval;
        json_array_foreach(roles, i, jval) {
            const char *s = json_string_value(jval);
            if (!s) {
                beginprop_idx(ctx, "roles", i);
                invalidprop(ctx, NULL);
                endprop(ctx);
                continue;
            }
            char *role = NULL;
            if (!strcasecmp(s, "attendee"))
                role = "ATTENDEE";
            else if (!strcasecmp(s, "chair"))
                role = "CHAIR";
            else if (!strcasecmp(s, "owner"))
                role = "OWNER";
            if (!role) {
                beginprop_idx(ctx, "roles", i);
                invalidprop(ctx, NULL);
                endprop(ctx);
                continue;
            }
            /* Try to use standard CHAIR role */
            if (!strcmp(role, "CHAIR") && ical_role == ICAL_ROLE_REQPARTICIPANT) {
                param = icalparameter_new_role(ICAL_ROLE_CHAIR);
                icalproperty_add_parameter(prop, param);
            } else {
                set_icalxparam(prop, JMAPICAL_XPARAM_ROLE, role, 0);
            }
        }
    }
    else if (roles) {
        invalidprop(ctx, "roles");
    }

    /* locationId */
    json_t *locationId = json_object_get(p, "locationId");
    if (json_is_string(locationId)) {
        const char *s = json_string_value(locationId);
        set_icalxparam(prop, JMAPICAL_XPARAM_LOCATIONID, s, 1);
    }
    else if (JNOTNULL(locationId)) {
        invalidprop(ctx, "locationId");
    }

    /* rsvpResponse */
    icalparameter_partstat ps = ICAL_PARTSTAT_NEEDSACTION;
    json_t *rsvpResponse = json_object_get(p, "rsvpResponse");
    if (json_is_string(rsvpResponse)) {
        char *tmp = ucase(xstrdup(json_string_value(rsvpResponse)));
        ps = icalparameter_string_to_enum(tmp);
        switch (ps) {
            case ICAL_PARTSTAT_NEEDSACTION:
            case ICAL_PARTSTAT_ACCEPTED:
            case ICAL_PARTSTAT_DECLINED:
            case ICAL_PARTSTAT_TENTATIVE:
                break;
            default:
                invalidprop(ctx, "rsvpResponse");
                ps = ICAL_PARTSTAT_NONE;
        }
        free(tmp);
    }
    else if (JNOTNULL(rsvpResponse)) {
        invalidprop(ctx, "rsvpResponse");
    }
    if (ps != ICAL_PARTSTAT_NONE) {
        param = icalparameter_new_partstat(ps);
        icalproperty_add_parameter(prop, param);
    }

    /* rsvpWanted */
    json_t *rsvpWanted = json_object_get(p, "rsvpWanted");
    if (json_is_boolean(rsvpWanted)) {
        if (rsvpWanted == json_true())
            param = icalparameter_new_rsvp(ICAL_RSVP_TRUE);
        else
            param = icalparameter_new_rsvp(ICAL_RSVP_FALSE);
        icalproperty_add_parameter(prop, param);
    }
    else if (JNOTNULL(rsvpWanted)) {
        invalidprop(ctx, "rsvpWanted");
    }

    /* delegatedTo */
    json_t *delegatedTo = json_object_get(p, "delegatedTo");
    if (json_array_size(delegatedTo)) {
        size_t i;
        json_t *jval;
        json_array_foreach(delegatedTo, i, jval) {
            const char *s = json_string_value(jval);
            if (!s) {
                beginprop_idx(ctx, "delegatedTo", i);
                invalidprop(ctx, NULL);
                endprop(ctx);
                continue;
            }
            char *tmp = mailaddr_to_uri(json_string_value(jval));
            param = icalparameter_new_delegatedto(tmp);
            icalproperty_add_parameter(prop, param);
            free(tmp);
        }
    }
    else if (JNOTNULL(delegatedTo)) {
        invalidprop(ctx, "delegatedTo");
    }

    /* delegatedFrom */
    json_t *delegatedFrom = json_object_get(p, "delegatedFrom");
    if (json_array_size(delegatedFrom)) {
        size_t i;
        json_t *jval;
        json_array_foreach(delegatedFrom, i, jval) {
            const char *s = json_string_value(jval);
            if (!s) {
                beginprop_idx(ctx, "delegatedFrom", i);
                invalidprop(ctx, NULL);
                endprop(ctx);
                continue;
            }
            char *tmp = mailaddr_to_uri(json_string_value(jval));
            param = icalparameter_new_delegatedfrom(tmp);
            icalproperty_add_parameter(prop, param);
            free(tmp);
        }
    }
    else if (JNOTNULL(delegatedFrom)) {
        invalidprop(ctx, "delegatedFrom");
    }

    /* memberOf */
    json_t *memberOf = json_object_get(p, "memberOf");
    if (json_array_size(memberOf)) {
        size_t i;
        json_t *jval;
        struct buf buf = BUF_INITIALIZER;
        /* libical already sets surrounding quotes on an x-value,
         * so make sure not to set a start quote for the first
         * mailto: URI and chomp of the QUOTE char of the last one */
        json_array_foreach(memberOf, i, jval) {
            const char *s = json_string_value(jval);
            if (!s) {
                beginprop_idx(ctx, "memberOf", i);
                invalidprop(ctx, NULL);
                endprop(ctx);
                continue;
            }
            char *tmp = mailaddr_to_uri(s);
            if (i) buf_appendcstr(&buf, ",\"");
            buf_appendcstr(&buf, tmp);
            buf_appendcstr(&buf, "\"");
            free(tmp);
        }
        buf_truncate(&buf, -1);
        set_icalxparam(prop, "MEMBER", buf_cstring(&buf), 1);
        buf_free(&buf);
    }
    else if (JNOTNULL(memberOf)) {
        invalidprop(ctx, "memberOf");
    }

    /* linkIds */
    json_t *linkIds = json_object_get(p, "linkIds");
    if (json_array_size(linkIds)) {
        size_t i;
        json_t *jval;
        json_array_foreach(linkIds, i, jval) {
            const char *s = json_string_value(jval);
            if (!s) {
                beginprop_idx(ctx, "linkIds", i);
                invalidprop(ctx, NULL);
                endprop(ctx);
                continue;
            }
            set_icalxparam(prop, JMAPICAL_XPARAM_LINKID, s, 0);
        }
    }
    else if (JNOTNULL(linkIds)) {
        invalidprop(ctx, "linkIds");
    }

    /* scheduleSequence */
    json_t *scheduleSequence = json_object_get(p, "scheduleSequence");
    if (json_is_integer(scheduleSequence) && json_integer_value(scheduleSequence) >= 0) {
        struct buf buf = BUF_INITIALIZER;
        buf_printf(&buf, "%lld", json_integer_value(scheduleSequence));
        set_icalxparam(prop, JMAPICAL_XPARAM_SEQUENCE, buf_cstring(&buf), 0);
        buf_free(&buf);
    }
    else if (JNOTNULL(scheduleSequence)) {
        invalidprop(ctx, "scheduleSequence");
    }

    /* scheduleUpdated */
    json_t *scheduleUpdated = json_object_get(p, "scheduleUpdated");
    if (json_is_string(scheduleUpdated)) {
        const char *s = json_string_value(scheduleUpdated);
        icaltimetype dtstamp;
        if (utcdate_to_icaltime(s, &dtstamp)) {
            char *tmp = icaltime_as_ical_string_r(dtstamp);
            set_icalxparam(prop, JMAPICAL_XPARAM_DTSTAMP, tmp, 0);
            free(tmp);
        }
        else {
            invalidprop(ctx, "scheduleSequence");
        }
    }
    else if (JNOTNULL(scheduleUpdated)) {
        invalidprop(ctx, "scheduleSequence");
    }
}

/* Create or update the ATTENDEEs in the VEVENT component comp as
 * defined by the participants property. */
static void
participants_to_ical(context_t *ctx, icalcomponent *comp, json_t *participants)
{
    const char *id;
    json_t *p;

    /* Purge existing ATTENDEEs */
    remove_icalprop(comp, ICAL_ATTENDEE_PROPERTY);

    if (!JNOTNULL(participants)) {
        return;
    }

    json_object_foreach(participants, id, p) {
        if (!strlen(id))
            continue;

        beginprop_key(ctx, "participants", id);

        const char *email = json_string_value(json_object_get(p, "email"));
        if (!email) {
            invalidprop(ctx, "email");
            endprop(ctx);
            continue;
        }
        char *tmp = mailaddr_to_uri(email);
        icalproperty *prop = icalproperty_new_attendee(tmp);
        participant_to_ical(ctx, prop, p);
        if (strcmp(id, email)) {
            set_icalxparam(prop, JMAPICAL_XPARAM_ID, id, 1);
        }
        icalcomponent_add_property(comp, prop);
        free(tmp);

        endprop(ctx);
    }
}

static void
links_to_ical(context_t *ctx, icalcomponent *comp, json_t *links,
              const char *propname, icalproperty_kind icalkind)
{
    icalproperty *prop;
    struct buf buf = BUF_INITIALIZER;

    /* Purge existing attachments */
    remove_icalprop(comp, icalkind);

    const char *id;
    json_t *link;
    json_object_foreach(links, id, link) {
        int pe;
        const char *href = NULL;
        const char *type = NULL;
        const char *title = NULL;
        const char *rel = NULL;
        const char *cid = NULL;
        json_int_t size = -1;
        json_t *properties = NULL;

        beginprop_key(ctx, propname, id);

        pe = readprop(ctx, link, "href", 1, "s", &href);
        if (pe > 0) {
            if (!strlen(href)) {
                invalidprop(ctx, "href");
                href = NULL;
            }
        }
        if (JNOTNULL(json_object_get(link, "type"))) {
            readprop(ctx, link, "type", 0, "s", &type);
        }
        if (JNOTNULL(json_object_get(link, "title"))) {
            readprop(ctx, link, "title", 0, "s", &title);
        }
        if (JNOTNULL(json_object_get(link, "cid"))) {
            readprop(ctx, link, "cid", 0, "s", &cid);
        }
        if (JNOTNULL(json_object_get(link, "size"))) {
            pe = readprop(ctx, link, "size", 0, "I", &size);
            if (pe > 0 && size < 0) {
                invalidprop(ctx, "size");
            }
        }
        if (JNOTNULL(json_object_get(link, "properties"))) {
            pe = readprop(ctx, link, "properties", 0, "o", &properties);
            if (pe > 0 && !json_object_size(properties)) {
                invalidprop(ctx, "properties");
            }
        }
        readprop(ctx, link, "rel", 0, "s", &rel);

        if (href && !have_invalid_props(ctx)) {

            /* Build iCalendar property */
            if (icalkind == ICAL_ATTACH_PROPERTY) {
                icalattach *icalatt = icalattach_new_from_url(href);
                prop = icalproperty_new_attach(icalatt);
                icalattach_unref(icalatt);
            }
            else {
                prop = icalproperty_new(ICAL_X_PROPERTY);
                icalproperty_set_x_name(prop, JMAPICAL_XPROP_ATTACH);
                icalproperty_set_value(prop, icalvalue_new_uri(href));
            }

            /* type */
            if (type) {
                icalproperty_add_parameter(prop,
                        icalparameter_new_fmttype(type));
            }

            /* title */
            if (title) {
                set_icalxparam(prop, JMAPICAL_XPARAM_TITLE, title, 1);
            }

            /* cid */
            if (cid) set_icalxparam(prop, JMAPICAL_XPARAM_CID, cid, 1);

            /* size */
            if (size >= 0) {
                buf_printf(&buf, "%"JSON_INTEGER_FORMAT, size);
                icalproperty_add_parameter(prop,
                        icalparameter_new_size(buf_cstring(&buf)));
                buf_reset(&buf);
            }

            /* rel */
            if (rel && strcmp(rel, "rel")) {
                set_icalxparam(prop, JMAPICAL_XPARAM_REL, rel, 1);
            }

            /* properties */
            if (properties) {
                char *tmp = encode_base64_json(properties);
                set_icalxparam(prop, JMAPICAL_XPARAM_PROPERTIES, tmp, 1);
                free(tmp);
            }

            /* Set custom id */
            set_icalxparam(prop, JMAPICAL_XPARAM_ID, id, 1);

            /* Add ATTACH property. */
            icalcomponent_add_property(comp, prop);
        }
        endprop(ctx);
        buf_free(&buf);
    }
}

static void
htmldescription_to_ical(context_t *ctx __attribute__((unused)),
                        icalcomponent *comp, json_t *htmldesc)
{
    icalproperty *prop = icalcomponent_get_first_property(comp, ICAL_DESCRIPTION_PROPERTY);

    /* Purge existing ALTREP, no matter what */
    if (prop) icalproperty_remove_parameter_by_kind(prop, ICAL_ALTREP_PARAMETER);

    if (htmldesc == json_null())
        return;

    if (!prop) {
        prop = icalproperty_new_description("");
        icalcomponent_add_property(comp, prop);
    }

    /* Set HTML description in ALTREP parameter */
    const char *html = json_string_value(htmldesc);
    struct buf buf = BUF_INITIALIZER;
    buf_setcstr(&buf, "data:text/html,");
    buf_appendcstr(&buf, html);
    icalparameter *altrep = icalparameter_new_altrep(buf_cstring(&buf));
    icalproperty_add_parameter(prop, altrep);
    buf_free(&buf);

    /* Convert HTML to plain */
    /* libical returns NULL for empty string */
    const char *s = icalproperty_get_description(prop);
    if (!s || *s == '\0') {
        char *plain = charset_extract_plain(html);
        if (html) icalproperty_set_description(prop, plain);
        free(plain);
    }
}

static void
alertaction_to_ical(context_t *ctx, icalcomponent *comp, icalcomponent *alarm,
                    json_t *action, int *is_unknown)
{
    const char *s;
    int pe;
    icalproperty *prop;
    icalparameter *param;

    /* type */
    icalproperty_action type = ICAL_ACTION_NONE;
    pe = readprop(ctx, action, "type", 1, "s", &s);
    if (pe > 0) {
        if (!strcmp(s, "email")) {
            type = ICAL_ACTION_EMAIL;
        } else if (!strcmp(s, "display")) {
            type = ICAL_ACTION_DISPLAY;
        }
    }
    *is_unknown = type == ICAL_ACTION_NONE;
    if (have_invalid_props(ctx) || *is_unknown) {
        return;
    }

    /* action */
    prop = icalproperty_new_action(type);
    icalcomponent_add_property(alarm, prop);

    /* alert contents */
    if (type == ICAL_ACTION_EMAIL) {
        json_t *to, *t;
        size_t i;

        pe = readprop(ctx, action, "to", 1, "o", &to);
        if (pe > 0 && json_array_size(to)) {
            json_array_foreach(to, i, t) {

                beginprop_idx(ctx, "to", i);

                /* email */
                pe = readprop(ctx, t, "email", 1, "s", &s);
                if (pe > 0) {
                    char *addr = mailaddr_to_uri(s);
                    prop = icalproperty_new_attendee(addr);
                    free(addr);
                }
                pe = readprop(ctx, t, "name", 0, "s", &s);
                if (pe > 0) {
                    param = icalparameter_new_cn(s);
                    icalproperty_add_parameter(prop, param);
                }
                if (!have_invalid_props(ctx)) {
                    icalcomponent_add_property(alarm, prop);
                }
                endprop(ctx);
            }
        } else if (!pe || (pe > 0 && json_typeof(to) != JSON_ARRAY)) {
            invalidprop(ctx, "to");
        }

        /* summary */
        s = NULL;
        readprop(ctx, action, "subject", 0, "s", &s);
        prop = icalproperty_new_summary(s ? s : "");
        icalcomponent_add_property(alarm, prop);

        /* textBody */
        s = NULL;
        readprop(ctx, action, "textBody", 0, "s", &s);
        prop = icalproperty_new_description(s ? s : "");
        icalcomponent_add_property(alarm, prop);

        /* htmlBody - must come after setting textBody */
        json_t *htmlBody = json_object_get(action, "htmlBody");
        if (json_is_null(htmlBody) || json_is_string(htmlBody)) {
            htmldescription_to_ical(ctx, alarm, htmlBody);
        }
        else if (JNOTNULL(htmlBody)) {
            invalidprop(ctx, "htmlBody");
        }

        /* attachments */
        json_t *attachments = json_object_get(action, "attachments");
        if (json_is_null(attachments) || json_is_object(attachments)) {
            links_to_ical(ctx, alarm, attachments, "attachments", ICAL_ATTACH_PROPERTY);
        }
        else if (JNOTNULL(attachments)) {
            invalidprop(ctx, "attachments");
        }
    } else {
        /* A DISPLAY alert */
        prop = icalproperty_new_description("");
        icalcomponent_add_property(alarm, prop);

        json_t *mediaLinks = json_object_get(action, "mediaLinks");
        if (json_is_null(mediaLinks) || json_is_object(mediaLinks)) {
            links_to_ical(ctx, alarm, mediaLinks, "mediaLinks", ICAL_X_PROPERTY);
        }
        else if (JNOTNULL(mediaLinks)) {
            invalidprop(ctx, "mediaLinks");
        }
    }

    /* snoozed */
    pe = readprop(ctx, action, "snoozed", 0, "s", &s);
    if (pe > 0) {
        icaltimetype t;
        if (utcdate_to_icaltime(s, &t)) {
            const char *uid = icalcomponent_get_uid(alarm);
            icalcomponent *snooze = icalcomponent_new_clone(alarm);
            struct icaltriggertype trigger;

            /* Add RELATED-TO */
            remove_icalprop(snooze, ICAL_UID_PROPERTY);
            prop = icalproperty_new_relatedto(uid);
            param = icalparameter_new(ICAL_RELTYPE_PARAMETER);
            icalparameter_set_xvalue(param, "SNOOZE");
            icalproperty_add_parameter(prop, param);
            icalcomponent_add_property(snooze, prop);

            /* Add TRIGGER */
            trigger.duration = icaldurationtype_null_duration();
            trigger.time = t;
            prop = icalproperty_new_trigger(trigger);
            icalcomponent_add_property(snooze, prop);
            icalcomponent_add_component(comp, snooze);
        } else {
            invalidprop(ctx, "snoozed");
        }
    }

    /* acknowledged */
    pe = readprop(ctx, action, "acknowledged", 0, "s", &s);
    if (pe > 0) {
        icaltimetype t;
        if (utcdate_to_icaltime(s, &t)) {
            prop = icalproperty_new_acknowledged(t);
            icalcomponent_add_property(alarm, prop);
        } else {
            invalidprop(ctx, "acknowledged");
        }
    }
}

/* Create or update the VALARMs in the VEVENT component comp as defined by the
 * JMAP alerts. */
static void
alerts_to_ical(context_t *ctx, icalcomponent *comp, json_t *alerts)
{
    const char *id;
    json_t *alert;
    icalcomponent *alarm, *next;
    int pe;

    /* Purge all VALARMs. */
    for (alarm = icalcomponent_get_first_component(comp, ICAL_VALARM_COMPONENT);
         alarm;
         alarm = next) {
        next = icalcomponent_get_next_component(comp, ICAL_VALARM_COMPONENT);
        icalcomponent_remove_component(comp, alarm);
        icalcomponent_free(alarm);
    }

    if (!JNOTNULL(alerts)) {
        return;
    }

    json_object_foreach(alerts, id, alert) {
        const char *s;
        struct icaltriggertype trigger;
        icalparameter_related rel;
        icalproperty *prop;
        icalparameter *param;
        json_t *action;

        alarm = icalcomponent_new_valarm();
        icalcomponent_set_uid(alarm, id);

        beginprop_key(ctx, "alerts", id);

        /* offset */
        trigger.time = icaltime_null_time();
        pe = readprop(ctx, alert, "offset", 1, "s", &s);
        if (pe > 0) {
            trigger.duration = icaldurationtype_from_string(s);
            if (icaldurationtype_is_bad_duration(trigger.duration)) {
                invalidprop(ctx, "offset");
            }
        }

        /* relativeTo */
        rel = ICAL_RELATED_NONE;
        pe = readprop(ctx, alert, "relativeTo", 1, "s", &s);
        if (pe > 0) {
            if (!strcmp(s, "before-start")) {
                rel = ICAL_RELATED_START;
                trigger.duration.is_neg = 1;
            } else if (!strcmp(s, "after-start")) {
                rel = ICAL_RELATED_START;
            } else if (!strcmp(s, "before-end")) {
                rel = ICAL_RELATED_END;
                trigger.duration.is_neg = 1;
            } else if (!strcmp(s, "after-end")) {
                rel = ICAL_RELATED_END;
            } else {
                invalidprop(ctx, "relativeTo");
            }
        }

        /* action */
        int is_unknown_action = 0;
        if (readprop(ctx, alert, "action", 1, "o", &action) > 0) {
            beginprop(ctx, "action");
            alertaction_to_ical(ctx, comp, alarm, action, &is_unknown_action);
            endprop(ctx);
        }

        if (is_unknown_action || have_invalid_props(ctx)) {
            icalcomponent_free(alarm);
            endprop(ctx);
            continue;
        }

        /* Add TRIGGER */
        prop = icalproperty_new_trigger(trigger);
        param = icalparameter_new_related(rel);
        icalproperty_add_parameter(prop, param);
        icalcomponent_add_property(alarm, prop);

        icalcomponent_add_component(comp, alarm);
        endprop(ctx);
    }
}

static void int_to_ical(struct buf *buf, int val) {
    buf_printf(buf, "%d", val);
}

/* Convert and print the JMAP byX recurrence value to ical into buf, otherwise
 * report the erroneous fieldName as invalid. If lower or upper is not NULL,
 * make sure that every byX value is within these bounds. */
static void recurrence_byX_to_ical(context_t *ctx,
                                   json_t *byX,
                                   struct buf *buf,
                                   const char *tag,
                                   int *lower,
                                   int *upper,
                                   int allowZero,
                                   const char *fieldName,
                                   void(*conv)(struct buf*, int)) {

    /* Make sure there is at least one entry. */
    if (!json_array_size(byX)) {
        invalidprop(ctx, fieldName);
        return;
    }

    /* Convert the array. */
    buf_printf(buf, ";%s=", tag);
    size_t i;
    for (i = 0; i < json_array_size(byX); i++) {
        int val;
        int err = json_unpack(json_array_get(byX, i), "i", &val);
        if (!err && !allowZero && !val) {
            err = 1;
        }
        if (!err && ((lower && val < *lower) || (upper && val > *upper))) {
            err = 2;
        }
        if (err) {
            beginprop_idx(ctx, fieldName, i);
            invalidprop(ctx, NULL);
            endprop(ctx);
            continue;
        }
        /* Prepend leading comma, if not first parameter value. */
        if (i) {
            buf_printf(buf, "%c", ',');
        }
        /* Convert the byX value to ical. */
        conv(buf, val);
    }
}

/* Create or overwrite the RRULE in the VEVENT component comp as defined by the
 * JMAP recurrence. */
static void
recurrence_to_ical(context_t *ctx, icalcomponent *comp, json_t *recur)
{
    struct buf buf = BUF_INITIALIZER;
    int pe, lower, upper;
    icalproperty *prop, *next;

    /* Purge existing RRULE. */
    for (prop = icalcomponent_get_first_property(comp, ICAL_RRULE_PROPERTY);
         prop;
         prop = next) {
        next = icalcomponent_get_next_property(comp, ICAL_RRULE_PROPERTY);
        icalcomponent_remove_property(comp, prop);
        icalproperty_free(prop);
    }

    if (!JNOTNULL(recur)) {
        return;
    }

    beginprop(ctx, "recurrenceRule");

    /* frequency */
    char *freq;
    pe = readprop(ctx, recur, "frequency", 1, "s", &freq);
    if (pe > 0) {
        char *s = xstrdup(freq);
        s = lcase(s);
        buf_printf(&buf, "FREQ=%s", s);
        free(s);
    }

    /* interval */
    int interval = 1;
    pe = readprop(ctx, recur, "interval", 0, "i", &interval);
    if (pe > 0) {
        if (interval > 1) {
            buf_printf(&buf, ";INTERVAL=%d", interval);
        } else if (interval < 1) {
            invalidprop(ctx, "interval");
        }
    }

    /* skip */
    char *skip = NULL;
    pe = readprop(ctx, recur, "skip", 0, "s", &skip);
    if (pe > 0 && strlen(skip)) {
        skip = xstrdup(skip);
        skip = ucase(skip);
        buf_printf(&buf, ";SKIP=%s", skip);
        free(skip);
    } else if (pe > 0) {
        invalidprop(ctx, "skip");
    }

    /* rscale */
    char *rscale = NULL;
    pe = readprop(ctx, recur, "rscale", skip != NULL, "s", &rscale);
    if (pe > 0 && strlen(rscale)) {
        rscale = xstrdup(rscale);
        rscale = ucase(rscale);
        buf_printf(&buf, ";RSCALE=%s", rscale);
        free(rscale);
    } else if (pe > 0) {
        invalidprop(ctx, "rscale");
    }

    /* firstDayOfWeek */
    const char *firstday = NULL;
    pe = readprop(ctx, recur, "firstDayOfWeek", 0, "s", &firstday);
    if (pe > 0) {
        char *tmp = xstrdup(firstday);
        tmp = ucase(tmp);
        if (icalrecur_string_to_weekday(tmp) != ICAL_NO_WEEKDAY) {
            buf_printf(&buf, ";WKST=%s", tmp);
        } else {
            invalidprop(ctx, "firstDayOfWeek");
        }
        free(tmp);
    }

    /* byDay */
    json_t *byday = json_object_get(recur, "byDay");
    if (json_array_size(byday) > 0) {
        size_t i;
        json_t *bd;

        buf_appendcstr(&buf, ";BYDAY=");

        json_array_foreach(byday, i, bd) {
            const char *s;
            char *day = NULL;
            json_int_t nth;

            beginprop_idx(ctx, "byDay", i);

            /* day */
            pe = readprop(ctx, bd, "day", 1, "s", &s);
            if (pe > 0) {
                day = xstrdup(s);
                day = ucase(day);
                if (icalrecur_string_to_weekday(day) == ICAL_NO_WEEKDAY) {
                    invalidprop(ctx, "day");
                }
            }

            /* nthOfPeriod */
            nth = 0;
            pe = readprop(ctx, bd, "nthOfPeriod", 0, "I", &nth);
            if (pe > 0 && !nth) {
                invalidprop(ctx, "nthOfPeriod");
            }

            /* Bail out for property errors */
            if (have_invalid_props(ctx)) {
                if (day) free(day);
                endprop(ctx);
                continue;
            }

            /* Append day */
            if (i > 0) {
                buf_appendcstr(&buf, ",");
            }
            if (nth) {
                buf_printf(&buf, "%+"JSON_INTEGER_FORMAT, nth);
            }
            buf_appendcstr(&buf, day);
            free(day);

            endprop(ctx);
        }
    } else if (byday) {
        invalidprop(ctx, "byDay");
    }

    /* byDate */
    json_t *bydate = NULL;
    lower = -31;
    upper = 31;
    pe = readprop(ctx, recur, "byDate", 0, "o", &bydate);
    if (pe > 0) {
        recurrence_byX_to_ical(ctx, bydate, &buf, "BYDATE",
                &lower, &upper, 0 /* allowZero */,
                "byDate", int_to_ical);
    }

    /* byMonth */
    json_t *bymonth = NULL;
    pe = readprop(ctx, recur, "byMonth", 0, "o", &bymonth);
    if (pe > 0) {
        if (json_array_size(bymonth) > 0) {
            size_t i;
            json_t *jval;
            buf_printf(&buf, ";BYMONTH=");
            json_array_foreach(bymonth, i, jval) {
                const char *s = json_string_value(jval);
                if (!s) {
                    beginprop_idx(ctx,"byMonth", i);
                    invalidprop(ctx, NULL);
                    endprop(ctx);
                    continue;
                }
                int val;
                char leap = 0, dummy = 0;
                int matched = sscanf(s, "%2d%c%c", &val, &leap, &dummy);
                if (matched < 1 || matched > 2 || (leap && leap != 'L') || val < 1) {
                    beginprop_idx(ctx,"byMonth", i);
                    invalidprop(ctx, NULL);
                    endprop(ctx);
                    continue;
                }
                if (i) buf_putc(&buf, ',');
                buf_printf(&buf, "%d", val);
                if (leap) buf_putc(&buf, 'L');
            }
        }
    }

    /* byYearDay */
    json_t *byyearday = NULL;
    lower = -366;
    upper = 366;
    pe = readprop(ctx, recur, "byYearDay", 0, "o", &byyearday);
    if (pe > 0) {
        recurrence_byX_to_ical(ctx, byyearday, &buf, "BYYEARDAY",
                &lower, &upper, 0 /* allowZero */,
                "byYearDay", int_to_ical);
    }


    /* byWeekNo */
    json_t *byweekno = NULL;
    lower = -53;
    upper = 53;
    pe = readprop(ctx, recur, "byWeekNo", 0, "o", &byweekno);
    if (pe > 0) {
        recurrence_byX_to_ical(ctx, byweekno, &buf, "BYWEEKNO",
                &lower, &upper, 0 /* allowZero */,
                "byWeekNo", int_to_ical);
    }

    /* byHour */
    json_t *byhour = NULL;
    lower = 0;
    upper = 23;
    pe = readprop(ctx, recur, "byHour", 0, "o", &byhour);
    if (pe > 0) {
        recurrence_byX_to_ical(ctx, byhour, &buf, "BYHOUR",
                &lower, &upper, 1 /* allowZero */,
                "byHour", int_to_ical);
    }

    /* byMinute */
    json_t *byminute = NULL;
    lower = 0;
    upper = 59;
    pe = readprop(ctx, recur, "byMinute", 0, "o", &byminute);
    if (pe > 0) {
        recurrence_byX_to_ical(ctx, byminute, &buf, "BYMINUTE",
                &lower, &upper, 1 /* allowZero */,
                "byMinute", int_to_ical);
    }

    /* bySecond */
    json_t *bysecond = NULL;
    lower = 0;
    upper = 59;
    pe = readprop(ctx, recur, "bySecond", 0, "o", &bysecond);
    if (pe > 0) {
        recurrence_byX_to_ical(ctx, bysecond, &buf, "BYSECOND",
                &lower, &upper, 1 /* allowZero */,
                "bySecond", int_to_ical);
    }

    /* bySetPosition */
    json_t *bysetpos = NULL;
    lower = 0;
    upper = 59;
    pe = readprop(ctx, recur, "bySetPosition", 0, "o", &bysetpos);
    if (pe > 0) {
        recurrence_byX_to_ical(ctx, bysetpos, &buf, "BYSETPOS",
                &lower, &upper, 1 /* allowZero */,
                "bySetPos", int_to_ical);
    }

    if (json_object_get(recur, "count") && json_object_get(recur, "until")) {
        invalidprop(ctx, "count");
        invalidprop(ctx, "until");
    }

    /* count */
    int count;
    pe = readprop(ctx, recur, "count", 0, "i", &count);
    if (pe > 0) {
        if (count > 0 && !json_object_get(recur, "until")) {
            buf_printf(&buf, ";COUNT=%d", count);
        } else {
            invalidprop(ctx, "count");
        }
    }

    /* until */
    const char *until;
    pe = readprop(ctx, recur, "until", 0, "s", &until);
    if (pe > 0) {
        icaltimetype dtloc;

        if (localdate_to_icaltime(until, &dtloc, ctx->tzstart, ctx->is_allday)) {
            icaltimezone *utc = icaltimezone_get_utc_timezone();
            icaltimetype dt = icaltime_convert_to_zone(dtloc, utc);
            buf_printf(&buf, ";UNTIL=%s", icaltime_as_ical_string(dt));
        } else {
            invalidprop(ctx, "until");
        }
    }

    if (!have_invalid_props(ctx)) {
        /* Add RRULE to component */
        struct icalrecurrencetype rt =
            icalrecurrencetype_from_string(buf_cstring(&buf));
        if (rt.freq != ICAL_NO_RECURRENCE) {
            icalcomponent_add_property(comp, icalproperty_new_rrule(rt));
        } else {
            /* Messed up the RRULE value. That's an error. */
            ctx->err->code = JMAPICAL_ERROR_UNKNOWN;
            invalidprop(ctx, NULL);
        }
    }

    endprop(ctx);
    buf_free(&buf);
}

/* Create or overwrite JMAP keywords in comp */
static void
keywords_to_ical(context_t *ctx, icalcomponent *comp, json_t *keywords)
{
    icalproperty *prop, *next;

    /* Purge existing keywords from component */
    for (prop = icalcomponent_get_first_property(comp, ICAL_CATEGORIES_PROPERTY);
         prop;
         prop = next) {

        next = icalcomponent_get_next_property(comp, ICAL_CATEGORIES_PROPERTY);
        icalcomponent_remove_property(comp, prop);
        icalproperty_free(prop);
    }

    /* Add keywords */
    struct buf buf = BUF_INITIALIZER;
    json_t *jval;
    size_t i;
    json_array_foreach(keywords, i, jval) {
        const char *keyword = json_string_value(jval);
        if (!keyword) {
            beginprop_idx(ctx, "keywords", i);
            invalidprop(ctx, NULL);
            endprop(ctx);
            continue;
        }
        // FIXME known bug: libical doesn't properly
        // handle multi-values separated by comma,
        // if a single entry contains a comma.
        prop = icalproperty_new_categories(keyword);
        icalcomponent_add_property(comp, prop);
    }
    if (buf_len(&buf)) {
    }
    buf_free(&buf);
}

/* Create or overwrite JMAP relatedTo in comp */
static void
relatedto_to_ical(context_t *ctx, icalcomponent *comp, json_t *related)
{
    icalproperty *prop, *next;
    icalparameter *param;
    json_t *to;
    const char *reltype;

    /* Purge existing relatedTo properties from component */
    for (prop = icalcomponent_get_first_property(comp, ICAL_RELATEDTO_PROPERTY);
         prop;
         prop = next) {

        next = icalcomponent_get_next_property(comp, ICAL_RELATEDTO_PROPERTY);
        icalcomponent_remove_property(comp, prop);
        icalproperty_free(prop);
    }

    /* Add relatedTo */
    json_object_foreach(related, reltype, to) {
        const char *uid = json_string_value(to);

        beginprop_key(ctx, "relatedTo", reltype);

        /* Validate uid and reltype */
        if (uid && strlen(uid) && strlen(reltype)) {
            prop = icalproperty_new_relatedto(uid);
            param = icalparameter_new(ICAL_RELTYPE_PARAMETER);

            char *s = ucase(xstrdup(reltype));
            icalparameter_set_xvalue(param, s);
            icalproperty_add_parameter(prop, param);
            icalcomponent_add_property(comp, prop);
            free(s);
        } else {
            invalidprop(ctx, NULL);
        }
        endprop(ctx);
    }
}

static int
validate_location(context_t *ctx, json_t *loc)
{
    size_t invalid_cnt = invalid_prop_count(ctx);
    json_t *jval;
    size_t i;

    /* At least one property MUST be set */
    if (json_object_size(loc) == 0) {
        invalidprop(ctx, NULL);
        return 0;
    }

    jval = json_object_get(loc, "name");
    if (JNOTNULL(jval) && !json_is_string(jval))
        invalidprop(ctx, "name");

    jval = json_object_get(loc, "description");
    if (JNOTNULL(jval) && !json_is_string(jval))
        invalidprop(ctx, "description");

    jval = json_object_get(loc, "rel");
    if (JNOTNULL(jval) && !json_is_string(jval))
        invalidprop(ctx, "rel");

    jval = json_object_get(loc, "coordinates");
    if (JNOTNULL(jval) && !json_is_string(jval))
        invalidprop(ctx, "coordinates");

    jval = json_object_get(loc, "uri");
    if (JNOTNULL(jval) && !json_is_string(jval))
        invalidprop(ctx, "uri");

    jval = json_object_get(loc, "timeZone");
    if (json_is_string(jval)) {
        if (!tz_from_tzid(json_string_value(jval)))
            invalidprop(ctx, "timeZone");
    }
    else if (JNOTNULL(jval)) {
        invalidprop(ctx, "timeZone");
    }

    /* linkIds */
    json_t *linkids = json_object_get(loc, "linkIds");
    if (JNOTNULL(linkids) && json_is_array(linkids)) {
        json_array_foreach(linkids, i, jval) {
            if (!json_is_string(jval)) {
                beginprop_idx(ctx, "linkIds", i);
                invalidprop(ctx, NULL);
                endprop(ctx);
            }
        }
    }
    else if (JNOTNULL(linkids)) {
        invalidprop(ctx, "linkIds");
    }

    /* features */
    json_t *features = json_object_get(loc, "features");
    if (JNOTNULL(features) && json_is_array(features)) {
        json_array_foreach(features, i, jval) {
            if (!json_is_string(jval)) {
                beginprop_idx(ctx, "features", i);
                invalidprop(ctx, NULL);
                endprop(ctx);
            }
        }
    }
    else if (JNOTNULL(features)) {
        invalidprop(ctx, "features");
    }

    /* Location is invalid, if any invalid property has been added */
    return invalid_prop_count(ctx) == invalid_cnt;
}

static void
location_to_ical(context_t *ctx __attribute__((unused)), icalcomponent *comp, const char *id, json_t *loc)
{
    const char *name = json_string_value(json_object_get(loc, "name"));
    const char *uri = json_string_value(json_object_get(loc, "uri"));
    const char *rel = json_string_value(json_object_get(loc, "rel"));

    /* Gracefully handle bogus values */
    if (rel && !strcmp(rel, "unknown")) rel = NULL;

    /* Determine which property kind to use for this location.
     * Always try to create at least one LOCATION, even if CONFERENCE
     * would be more appropriate, to gracefully handle legacy clients. */
    icalproperty *prop;
    if (!icalcomponent_get_first_property(comp, ICAL_LOCATION_PROPERTY)) {
        prop = icalproperty_new(ICAL_LOCATION_PROPERTY);
    }
    else if (uri && (rel && !strcmp(rel, "virtual"))) {
        prop = icalproperty_new(ICAL_CONFERENCE_PROPERTY);
    } else {
        prop = icalproperty_new(ICAL_X_PROPERTY);
        icalproperty_set_x_name(prop, JMAPICAL_XPROP_LOCATION);
    }

    /* Keep user-supplied location id */
    xjmapid_to_ical(prop, id);

    /* name, uri, rel */
    if (icalproperty_isa(prop) == ICAL_CONFERENCE_PROPERTY) {
        icalvalue *val = icalvalue_new_from_string(ICAL_URI_VALUE, uri);
        icalproperty_set_value(prop, val);
        icalproperty_add_parameter(prop, icalparameter_new_label(name));
    } else {
        icalvalue *val = icalvalue_new_from_string(ICAL_TEXT_VALUE, name);
        icalproperty_set_value(prop, val);
        if (uri) icalproperty_add_parameter(prop, icalparameter_new_altrep(uri));
        if (rel) set_icalxparam(prop, JMAPICAL_XPARAM_REL, rel, 0);
    }

    /* description, timeZone, coordinates */
    const char *s = json_string_value(json_object_get(loc, "description"));
    if (s) set_icalxparam(prop, JMAPICAL_XPARAM_DESCRIPTION, s, 0);
    s = json_string_value(json_object_get(loc, "timeZone"));
    if (s) set_icalxparam(prop, JMAPICAL_XPARAM_TZID, s, 0);
    s = json_string_value(json_object_get(loc, "coordinates"));
    if (s) set_icalxparam(prop, JMAPICAL_XPARAM_GEO, s, 0);

    json_t *jval;
    size_t i;

    /* linkIds */
    json_array_foreach(json_object_get(loc, "linkIds"), i, jval) {
        const char *linkid = json_string_value(jval);
        set_icalxparam(prop, JMAPICAL_XPARAM_LINKID, linkid, 0);
    }

    /* feature */
    struct buf buf = BUF_INITIALIZER;
    json_array_foreach(json_object_get(loc, "features"), i, jval) {
        if (i) buf_appendcstr(&buf, ",");
        buf_appendcstr(&buf, json_string_value(jval));
    }
    if (buf_len(&buf)) {
        const char *pname =
            (icalproperty_isa(prop) == ICAL_CONFERENCE_PROPERTY) ?
            "FEATURE" : JMAPICAL_XPARAM_FEATURE;
        // FIXME libical quotes X-values with commas
        set_icalxparam(prop, pname, buf_ucase(&buf), 0);
    }
    buf_free(&buf);

    icalcomponent_add_property(comp, prop);
}

/* Create or overwrite the JMAP locations in comp */
static void
locations_to_ical(context_t *ctx, icalcomponent *comp, json_t *locations)
{
    json_t *loc;
    const char *id;

    /* Purge existing locations */
    remove_icalprop(comp, ICAL_LOCATION_PROPERTY);
    remove_icalprop(comp, ICAL_GEO_PROPERTY);
    remove_icalxprop(comp, JMAPICAL_XPROP_LOCATION);
    remove_icalxprop(comp, "X-APPLE-STRUCTURED-LOCATION");

    /* Bail out if no location needs to be set */
    if (!JNOTNULL(locations)) {
        return;
    }

    /* Add locations */
    json_object_foreach(locations, id, loc) {
        beginprop_key(ctx, "locations", id);

        /* Validate the location id */
        if (!strlen(id)) {
            invalidprop(ctx, NULL);
            endprop(ctx);
            continue;
        }

        /* Ignore end timeZone locations */
        if (location_is_endtimezone(loc)) {
            endprop(ctx);
            continue;
        }
        /* Validate location */
        if (!validate_location(ctx, loc)) {
            endprop(ctx);
            continue;
        }

        /* Add location */
        location_to_ical(ctx, comp, id, loc);
        endprop(ctx);
    }
}

static void set_language_icalprop(icalcomponent *comp, icalproperty_kind kind,
                                  const char *lang)
{
    icalproperty *prop;
    icalparameter *param;

    prop = icalcomponent_get_first_property(comp, kind);
    if (!prop) return;

    icalproperty_remove_parameter_by_kind(prop, ICAL_LANGUAGE_PARAMETER);
    if (!lang) return;

    param = icalparameter_new(ICAL_LANGUAGE_PARAMETER);
    icalparameter_set_language(param, lang);
    icalproperty_add_parameter(prop, param);
}

static void
replyto_to_ical(context_t *ctx, icalcomponent *comp, json_t *replyto)
{
    icalproperty *prop;
    json_t *imip, *web;

    remove_icalprop(comp, ICAL_ORGANIZER_PROPERTY);

    /* XXX(rsto): We want ORGANIZER always to have a mailto: URI
     * for now, and without ORGANIZER we can't store the 'web'
     * replyTo property. */
    if ((imip = json_object_get(replyto, "imip"))) {
        const char *addr = NULL;

        beginprop_key(ctx, "replyTo", "imip");

        addr = json_string_value(imip);
        if (!addr) {
            invalidprop(ctx, NULL);
            endprop(ctx);
            return;
        }

        prop = icalproperty_new_organizer(addr);
        icalcomponent_add_property(comp, prop);
        endprop(ctx);

        if ((web = json_object_get(replyto, "web"))) {
            const char *uri = NULL;

            beginprop_key(ctx, "replyTo", "web");

            uri = json_string_value(web);
            if (!uri || (strncmp(uri, "http:", 5) && strncmp(uri, "https:", 6))) {
                invalidprop(ctx, NULL);
                endprop(ctx);
                return;
            }
            set_icalxparam(prop, JMAPICAL_XPARAM_RSVP_URI, uri, 1);

            endprop(ctx);
        }
    }
}

static void
overrides_to_ical(context_t *ctx, icalcomponent *comp, json_t *overrides)
{
    json_t *override, *master;
    const char *id;
    icalcomponent *excomp, *next, *ical;
    context_t *fromctx;
    hash_table recurs = HASH_TABLE_INITIALIZER;
    int n;

    /* Purge EXDATE, RDATE */
    remove_icalprop(comp, ICAL_RDATE_PROPERTY);
    remove_icalprop(comp, ICAL_EXDATE_PROPERTY);

    /* Move VEVENT exceptions to a cache */
    ical = icalcomponent_get_parent(comp);
    n = icalcomponent_count_components(ical, ICAL_VEVENT_COMPONENT);
    construct_hash_table(&recurs, n + 1, 0);
    for (excomp = icalcomponent_get_first_component(ical, ICAL_VEVENT_COMPONENT);
         excomp;
         excomp = next) {

        icaltimetype recurid;
        char *t;

        next = icalcomponent_get_next_component(ical, ICAL_VEVENT_COMPONENT);
        if (excomp == comp) continue;

        /* Index VEVENT by its LocalDate recurrence id */
        icalcomponent_remove_component(ical, excomp);
        recurid = icalcomponent_get_recurrenceid(excomp);
        t = localdate_from_icaltime_r(recurid);
        hash_insert(t, excomp, &recurs);
        free(t);
    }

    if (json_is_null(overrides)) {
        free_hash_table(&recurs, (void (*)(void *))icalcomponent_free);
        return;
    }

    /* Convert current master event to JMAP */
    fromctx = context_new(NULL, NULL, JMAPICAL_READ_MODE);
    master = calendarevent_from_ical(fromctx, comp);
    if (!master) {
        if (ctx->err) {
            ctx->err->code = JMAPICAL_ERROR_UNKNOWN;
            context_free(fromctx);
            return;
        }
    }
    json_object_del(master, "recurrenceRule");
    json_object_del(master, "recurrenceOverrides");

    json_object_foreach(overrides, id, override) {
        icaltimetype start;

        beginprop_key(ctx, "recurrenceOverrides", id);

        if (!localdate_to_icaltime(id, &start, ctx->tzstart, ctx->is_allday)) {
            invalidprop(ctx, NULL);
            endprop(ctx);
            continue;
        }

        json_t *excluded = json_object_get(override, "excluded");
        if (excluded) {
            if (json_object_size(override) == 1 && excluded == json_true()) {
                /* Add EXDATE */
                dtprop_to_ical(comp, start, ctx->tzstart, 0, ICAL_EXDATE_PROPERTY);
            }
            else {
                invalidprop(ctx, id);
                endprop(ctx);
                continue;
            }
        } else if (!json_object_size(override)) {
            /* Add RDATE */
            dtprop_to_ical(comp, start, ctx->tzstart, 0, ICAL_RDATE_PROPERTY);
        } else {
            /* Add VEVENT exception */
            context_t *toctx;
            json_t *ex, *val;
            const char *key;
            int ignore = 0;

            /* JMAP spec: "A pointer MUST NOT start with one of the following
             * prefixes; any patch with a such a key MUST be ignored" */
            json_object_foreach(override, key, val) {
                if (!strcmp(key, "uid") ||
                    !strcmp(key, "relatedTo") ||
                    !strcmp(key, "prodId") ||
                    !strcmp(key, "isAllDay") ||
                    !strcmp(key, "recurrenceRule") ||
                    !strcmp(key, "recurrenceOverrides") ||
                    !strcmp(key, "replyTo") ||
                    !strcmp(key, "participantId")) {

                    ignore = 1;
                }
            }
            if (ignore)
                continue;

            /* If the override doesn't have a custom start date, use
             * the LocalDate in the recurrenceOverrides object key */
            if (!json_object_get(override, "start")) {
                json_object_set_new(override, "start", json_string(id));
            }

            /* Create overridden event from patch and master event */
            if (!(ex = jmap_patchobject_apply(master, override))) {
                invalidprop(ctx, NULL);
                endprop(ctx);
                continue;
            }

            /* Lookup or create the VEVENT for this override */
            if ((excomp = hash_del(id, &recurs)) == NULL) {
                excomp = icalcomponent_new_clone(comp);
                remove_icalprop(excomp, ICAL_RDATE_PROPERTY);
                remove_icalprop(excomp, ICAL_EXDATE_PROPERTY);
                remove_icalprop(excomp, ICAL_RRULE_PROPERTY);
            }
            dtprop_to_ical(excomp, start,
                           ctx->tzstart, 1, ICAL_RECURRENCEID_PROPERTY);

            /* Convert the override event to iCalendar */
            toctx = context_new(NULL, ctx->err, ctx->mode | JMAPICAL_EXC_MODE);
            calendarevent_to_ical(toctx, excomp, ex);
            if (have_invalid_props(toctx)) {
                json_t *invalid = get_invalid_props(toctx);
                invalidprop_append(ctx, invalid);
                json_decref(invalid);
            }
            context_free(toctx);

            /* Add the exception */
            icalcomponent_add_component(ical, excomp);
            json_decref(ex);
        }

        endprop(ctx);
    }

    free_hash_table(&recurs, (void (*)(void *))icalcomponent_free);
    context_free(fromctx);
    json_decref(master);
}

/* Create or overwrite the iCalendar properties in VEVENT comp based on the
 * properties the JMAP calendar event.
 *
 * Collect all required timezone ids in ctx. 
 */
static void
calendarevent_to_ical(context_t *ctx, icalcomponent *comp, json_t *event)
{
    int pe; /* parse error */
    const char *val = NULL;
    icalproperty *prop = NULL;
    int is_create = !(ctx->mode & JMAPICAL_UPDATE_MODE);
    int is_exc = ctx->mode & JMAPICAL_EXC_MODE;
    json_t *overrides = NULL;

    assert(is_create || comp);

    if (!is_create && !is_exc) {
        /* Read and write back the event, updated by the current changes */
        context_t *fromctx = context_new(NULL, NULL, JMAPICAL_READ_MODE);
        json_t *cur = calendarevent_from_ical(fromctx, comp);
        json_object_update(cur, event);
        event = cur;
        context_free(fromctx);
    } else {
        /* Do not preserve any current contents */
        json_incref(event);
    }

    icaltimezone *utc = icaltimezone_get_utc_timezone();
    icaltimetype now = icaltime_current_time_with_zone(utc);

    json_t *excluded = json_object_get(event, "excluded");
    if (excluded && excluded != json_false()) {
        invalidprop(ctx, "excluded");
    }

    /* uid */
    icalcomponent_set_uid(comp, ctx->uid);

    json_t *jtype = json_object_get(event, "@type");
    if (JNOTNULL(jtype) && json_is_string(jtype)) {
        if (strcmp(json_string_value(jtype), "jsevent")) {
            invalidprop(ctx, "@type");
        }
    }
    else if (JNOTNULL(jtype)) {
        invalidprop(ctx, "@type");
    }

    /* isAllDay */
    readprop(ctx, event, "isAllDay", is_create, "b", &ctx->is_allday);

    /* start, duration, timeZone */
    startend_to_ical(ctx, comp, event);

    /* relatedTo */
    json_t *relatedTo = NULL;
    pe = readprop(ctx, event, "relatedTo", 0, "o", &relatedTo);
    if (pe > 0) {
        if (json_is_null(relatedTo) || json_object_size(relatedTo)) {
            relatedto_to_ical(ctx, comp, relatedTo);
        } else {
            invalidprop(ctx, "relatedTo");
        }
    }

    /* prodId */
    if (!is_exc) {
        val = NULL;
        if (!json_is_null(json_object_get(event, "prodId"))) {
            pe = readprop(ctx, event, "prodId", 0, "s", &val);
            if (pe > 0 || is_create) {
                struct buf buf = BUF_INITIALIZER;
                if (!val) {
                    /* Use same product id like jcal.c */
                    buf_setcstr(&buf, "-//CyrusJMAP.org/Cyrus ");
                    buf_appendcstr(&buf, CYRUS_VERSION);
                    buf_appendcstr(&buf, "//EN");
                    val = buf_cstring(&buf);
                }
                /* Purge any PRODID from the component. It should
                 * go into the enclosing VCALENDAR instead. */
                remove_icalprop(comp, ICAL_PRODID_PROPERTY);

                /* Set PRODID in the VCALENDAR */
                icalcomponent *ical = icalcomponent_get_parent(comp);
                remove_icalprop(ical, ICAL_PRODID_PROPERTY);
                prop = icalproperty_new_prodid(val);
                icalcomponent_add_property(ical, prop);
                buf_free(&buf);
            }
        }
    }

    /* created */
    if (is_create) {
        dtprop_to_ical(comp, now, utc, 1, ICAL_CREATED_PROPERTY);
    }

    /* updated */
    dtprop_to_ical(comp, now, utc, 1, ICAL_DTSTAMP_PROPERTY);

    /* sequence */
    if (is_create) {
        icalcomponent_set_sequence(comp, 0);
    }

    json_t *jprio = json_object_get(event, "priority");
    if (json_integer_value(jprio) >= 0 || json_integer_value(jprio) <= 9) {
        prop = icalproperty_new_priority(json_integer_value(jprio));
        icalcomponent_add_property(comp, prop);
    } else if (JNOTNULL(jprio)) {
        invalidprop(ctx, "priority");
    }

    /* title */
    pe = readprop(ctx, event, "title", is_create, "s", &val);
    if (pe > 0) {
        icalcomponent_set_summary(comp, val);
    }

    /* description */
    pe = readprop(ctx, event, "description", 0, "s", &val);
    if (pe > 0 && strlen(val)) {
        icalcomponent_set_description(comp, val);
    }

    /* htmlDescription - must come after description property */
    json_t *htmldesc = json_object_get(event, "htmlDescription");
    if (htmldesc == json_null() || json_is_string(htmldesc)) {
        htmldescription_to_ical(ctx, comp, htmldesc);
    } else if (htmldesc) {
        invalidprop(ctx, "htmlDescription");
    }

    /* color */
    pe = readprop(ctx, event, "color", 0, "s", &val);
    if (pe > 0 && strlen(val)) {
        prop = icalproperty_new_color(val);
        icalcomponent_add_property(comp, prop);
    }

    /* keywords */
    json_t *keywords = NULL;
    pe = readprop(ctx, event, "keywords", 0, "o", &keywords);
    if (pe > 0) {
        if (json_is_null(keywords) || json_array_size(keywords)) {
            keywords_to_ical(ctx, comp, keywords);
        } else {
            invalidprop(ctx, "keywords");
        }
    }

    /* links */
    json_t *links = NULL;
    pe = readprop(ctx, event, "links", 0, "o", &links);
    if (pe > 0) {
        if (json_is_null(links) || json_object_size(links)) {
            links_to_ical(ctx, comp, links, "links", ICAL_ATTACH_PROPERTY);
        } else {
            invalidprop(ctx, "links");
        }
    }

    /* locale */
    if (!json_is_null(json_object_get(event, "locale"))) {
        pe = readprop(ctx, event, "locale", 0, "s", &val);
        if (pe > 0) {
            set_language_icalprop(comp, ICAL_SUMMARY_PROPERTY, NULL);
            set_language_icalprop(comp, ICAL_DESCRIPTION_PROPERTY, NULL);
            if (strlen(val)) {
                set_language_icalprop(comp, ICAL_SUMMARY_PROPERTY, val);
            }
        }
    } else {
        set_language_icalprop(comp, ICAL_SUMMARY_PROPERTY, NULL);
        set_language_icalprop(comp, ICAL_DESCRIPTION_PROPERTY, NULL);
    }

    /* locations */
    json_t *locations = NULL;
    pe = readprop(ctx, event, "locations", 0, "o", &locations);
    if (pe > 0) {
        if (json_is_null(locations) || json_object_size(locations)) {
            locations_to_ical(ctx, comp, locations);
        } else {
            invalidprop(ctx, "locations");
        }
    }

    /* recurrenceRule */
    json_t *recurrence = NULL;
    pe = readprop(ctx, event, "recurrenceRule", 0, "o", &recurrence);
    if (pe > 0 && !is_exc) {
        recurrence_to_ical(ctx, comp, recurrence);
    }

    /* status */
    enum icalproperty_status status = ICAL_STATUS_NONE;
    pe = readprop(ctx, event, "status", 0, "s", &val);
    if (pe > 0) {
        if (!strcmp(val, "confirmed")) {
            status = ICAL_STATUS_CONFIRMED;
        } else if (!strcmp(val, "cancelled")) {
            status = ICAL_STATUS_CANCELLED;
        } else if (!strcmp(val, "tentative")) {
            status = ICAL_STATUS_TENTATIVE;
        } else {
            invalidprop(ctx, "status");
        }
    } else if (!pe && is_create) {
        status = ICAL_STATUS_CONFIRMED;
    }
    if (status != ICAL_STATUS_NONE) {
        remove_icalprop(comp, ICAL_STATUS_PROPERTY);
        icalcomponent_set_status(comp, status);
    }

    /* freeBusyStatus */
    pe = readprop(ctx, event, "freeBusyStatus", 0, "s", &val);
    if (pe > 0) {
        enum icalproperty_transp v = ICAL_TRANSP_NONE;
        if (!strcmp(val, "free")) {
            v = ICAL_TRANSP_TRANSPARENT;
        } else if (!strcmp(val, "busy")) {
            v = ICAL_TRANSP_OPAQUE;
        } else {
            invalidprop(ctx, "freeBusyStatus");
        }
        if (v != ICAL_TRANSP_NONE) {
            prop = icalcomponent_get_first_property(comp, ICAL_TRANSP_PROPERTY);
            if (prop) {
                icalproperty_set_transp(prop, v);
            } else {
                icalcomponent_add_property(comp, icalproperty_new_transp(v));
            }
        }
    }

    /* privacy */
    pe = readprop(ctx, event, "privacy", 0, "s", &val);
    if (pe > 0) {
        enum icalproperty_class v = ICAL_CLASS_NONE;
        if (!strcmp(val, "public")) {
            v = ICAL_CLASS_PUBLIC;
        } else if (!strcmp(val, "private")) {
            v = ICAL_CLASS_PRIVATE;
        } else if (!strcmp(val, "secret")) {
            v = ICAL_CLASS_CONFIDENTIAL;
        } else {
            invalidprop(ctx, "privacy");
        }
        if (v != ICAL_CLASS_NONE) {
            prop = icalcomponent_get_first_property(comp, ICAL_CLASS_PROPERTY);
            if (prop) {
                icalproperty_set_class(prop, v);
            } else {
                icalcomponent_add_property(comp, icalproperty_new_class(v));
            }
        }
    }

    /* replyTo */
    json_t *replyto;
    if (!json_is_null(json_object_get(event, "replyTo"))) {
        pe = readprop(ctx, event, "replyTo", 0, "o", &replyto);
        if (pe > 0) {
            replyto_to_ical(ctx, comp, replyto);
        }
    } else {
        remove_icalprop(comp, ICAL_ORGANIZER_PROPERTY);
    }

    /* participants */
    json_t *participants = NULL;
    pe = readprop(ctx, event, "participants", 0, "o", &participants);
    if (pe > 0) {
        if (json_is_null(participants) || json_object_size(participants)) {
            participants_to_ical(ctx, comp, participants);
        } else {
            invalidprop(ctx, "participants");
        }
    }

    /* participantId: readonly */

    /* useDefaultAlerts */
    int default_alerts;
    pe = readprop(ctx, event, "useDefaultAlerts", 0, "b", &default_alerts);
    if (pe > 0) {
        remove_icalxprop(comp, JMAPICAL_XPROP_USEDEFALERTS);
        if (default_alerts) {
            icalvalue *val = icalvalue_new_boolean(1);
            prop = icalproperty_new(ICAL_X_PROPERTY);
            icalproperty_set_x_name(prop, JMAPICAL_XPROP_USEDEFALERTS);
            icalproperty_set_value(prop, val);
            icalcomponent_add_property(comp, prop);
        }
    }

    /* alerts */
    json_t *alerts = NULL;
    pe = readprop(ctx, event, "alerts", 0, "o", &alerts);
    if (pe > 0) {
        if (json_is_null(alerts) || json_object_size(alerts)) {
            alerts_to_ical(ctx, comp, alerts);
        } else {
            invalidprop(ctx, "alerts");
        }
    } else if (!pe && !is_create && ctx->tzstart_old != ctx->tzstart) {
        /* The start timezone has changed but none of the alerts. */
        /* This is where we would like to update the timezones of any VALARMs
         * that have a TRIGGER value type of DATETIME (instead of the usual
         * DURATION type). Unfortunately, these DATETIMEs are stored in UTC.
         * Hence we can't tell if the event owner really wants to wake up
         * at e.g. 1am UTC or if it just was close to a local datetime during
         * creation of the iCalendar file. For now, do nothing about that. */
    }

    /* recurrenceOverrides - must be last to apply patches */
    pe = readprop(ctx, event, "recurrenceOverrides", 0, "o", &overrides);
    if (pe > 0 && !is_exc) {
        overrides_to_ical(ctx, comp, overrides);
    }

    /* Bail out for property errors */
    if (have_invalid_props(ctx)) {
        json_decref(event);
        return;
    }

    /* Check JMAP specification conditions on the generated iCalendar file, so 
     * this also doubles as a sanity check. Note that we *could* report a
     * property here as invalid, which had only been set by the client in a
     * previous request. */

    /* Either both organizer and attendees are null, or neither are. */
    if ((icalcomponent_get_first_property(comp, ICAL_ORGANIZER_PROPERTY) == NULL) !=
        (icalcomponent_get_first_property(comp, ICAL_ATTENDEE_PROPERTY) == NULL)) {
        invalidprop(ctx, "replyTo");
        invalidprop(ctx, "participants");
    }
    json_decref(event);
}

icalcomponent*
jmapical_toical(json_t *obj, icalcomponent *src, jmapical_err_t *err)
{
    icalcomponent *ical = NULL;
    icalcomponent *comp = NULL;
    context_t *ctx = NULL;

    if (src) {
        ical = icalcomponent_new_clone(src);
        /* Locate the main VEVENT. */
        for (comp = icalcomponent_get_first_component(ical,
                                                      ICAL_VEVENT_COMPONENT);
             comp;
             comp = icalcomponent_get_next_component(ical,
                                                     ICAL_VEVENT_COMPONENT)) {
            if (!icalcomponent_get_first_property(comp,
                                                  ICAL_RECURRENCEID_PROPERTY)) {
                break;
            }
        }
        if (!comp) {
            if (err) err->code = JMAPICAL_ERROR_ICAL;
            goto done;
        }
    } else {
        /* Create a new VCALENDAR. */
        ical = icalcomponent_new_vcalendar();
        icalcomponent_add_property(ical, icalproperty_new_version("2.0"));
        icalcomponent_add_property(ical, icalproperty_new_calscale("GREGORIAN"));

        /* Create a new VEVENT. */
        icaltimezone *utc = icaltimezone_get_utc_timezone();
        struct icaltimetype now =
            icaltime_from_timet_with_zone(time(NULL), 0, utc);
        comp = icalcomponent_new_vevent();
        icalcomponent_set_sequence(comp, 0);
        icalcomponent_set_dtstamp(comp, now);
        icalcomponent_add_property(comp, icalproperty_new_created(now));
        icalcomponent_add_component(ical, comp);
    }

    /* Convert the JMAP calendar event to ical. */
    ctx = context_new(NULL, err, JMAPICAL_WRITE_MODE);
    if (src) {
        ctx->mode |= JMAPICAL_UPDATE_MODE;
    }
    ctx->uid = json_string_value(json_object_get(obj, "uid"));
    if (!ctx->uid && src) {
        ctx->uid = icalcomponent_get_uid(comp);
    }
    if (!ctx->uid) {
        if (err) err->code = JMAPICAL_ERROR_UID;
        if (ical) icalcomponent_free(ical);
        ical = NULL;
        goto done;
    }
    calendarevent_to_ical(ctx, comp, obj);
    icalcomponent_add_required_timezones(ical);

    /* Bubble up any property errors. */
    if (have_invalid_props(ctx) && err) {
        err->code = JMAPICAL_ERROR_PROPS;
        err->props = get_invalid_props(ctx);
        if (ical) icalcomponent_free(ical);
        ical = NULL;
    }

    /* Free erroneous ical data */
    if (ctx->err && ctx->err->code) {
        if (ical) icalcomponent_free(ical);
        ical = NULL;
    }

done:
    context_free(ctx);
    return ical;
}

const char *
jmapical_strerror(int err)
{
    switch (err) {
        case 0:
            return "jmapical: success";
        case JMAPICAL_ERROR_CALLBACK:
            return "jmapical: callback error";
        case JMAPICAL_ERROR_MEMORY:
            return "jmapical: no memory";
        case JMAPICAL_ERROR_ICAL:
            return "jmapical: iCalendar error";
        case JMAPICAL_ERROR_PROPS:
            return "jmapical: property error";
        case JMAPICAL_ERROR_UID:
            return "jmapical: iCalendar uid error";
        default:
            return "jmapical: unknown error";
    }
}

/*
 * Construct a jevent string for an iCalendar component.
 */
EXPORTED struct buf *icalcomponent_as_jevent_string(icalcomponent *ical)
{
    struct buf *ret;
    json_t *jcal;
    size_t flags = JSON_PRESERVE_ORDER;
    char *buf;

    if (!ical) return NULL;

    jcal = jmapical_tojmap(ical, NULL, NULL);

    flags |= (config_httpprettytelemetry ? JSON_INDENT(2) : JSON_COMPACT);
    buf = json_dumps(jcal, flags);

    json_decref(jcal);

    ret = buf_new();
    buf_initm(ret, buf, strlen(buf));

    return ret;
}

EXPORTED icalcomponent *jevent_string_as_icalcomponent(const struct buf *buf)
{
    json_t *obj;
    json_error_t jerr;
    icalcomponent *ical;
    const char *str = buf_cstring(buf);

    if (!str) return NULL;

    obj = json_loads(str, 0, &jerr);
    if (!obj) {
        syslog(LOG_WARNING, "json parse error: '%s'", jerr.text);
        return NULL;
    }

    ical = jmapical_toical(obj, NULL, NULL);

    json_decref(obj);

    return ical;
}

