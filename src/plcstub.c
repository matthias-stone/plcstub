/* plcstub.c
 *
 * Routines for the top-level interface to the PLC.
 * author: ntaylor
 */

#include <err.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug.h"
#include "plcstub.h"
#include "libplctag.h"
#include "lock_utils.h"
#include "tagtree.h"

/************************ Public API ************************/


int
plc_tag_check_lib_version(int req_major, int req_minor, int req_patch) {
    (void)(req_major);
    (void)(req_minor);
    (void)(req_patch);
    return true;
}

int
plc_tag_get_debug_level()
{
    return debug_get_level();
}

int
plc_tag_create(const char* attrib, int timeout)
{
    int ret = PLCTAG_STATUS_OK;
    char *kv_ctx, *kv;
    char* kv_sep = "&";
    struct tag* tag;

    /* There are three attributes that we are interested in at the moment:
     * 1) name: the name of the tag
     * 2) elem_size: the width of each element in the tag
     * 3) elem_count: how many elements. (TODO: how does this work with multi-dim arrays?)
     */
    char* name = NULL;
    size_t elem_size = 0;
    size_t elem_count = 0;

    plcstub_init();

    char* str = strdup(attrib);

    for (kv = strtok_r(str, kv_sep, &kv_ctx);
         kv != NULL;
         kv = strtok_r(NULL, kv_sep, &kv_ctx)) {
        char *key, *val;
        pdebug(PLCTAG_DEBUG_SPEW, "Current kv-pair: %s", kv);

        key = kv;
        val = strchr(key, '=');
        if (val == NULL && (strcmp("protocol", kv) != 0)) {
            /* At the moment, the only attribute we've seen that isn't a key-value pair
             * is "protocol".  If we encounter others, we can either check for them or
             * just ignore them altogether, depending on our confidence of things. */
            pdebug(PLCTAG_DEBUG_WARN, "Missing '=' in non-'protocol' attribute %s", kv);
            ret = PLCTAG_ERR_BAD_PARAM;
            goto done;
        }
        *val = '\0';
        val++;
        pdebug(PLCTAG_DEBUG_SPEW, "key=%s,val=%s", key, val);

        /* We have a key and value parsed out at ths point. */

        if (strcmp("name", key) == 0) {
            if (name != NULL) {
                pdebug(PLCTAG_DEBUG_WARN, "Overwriting attribute %s", "name");
            }
            name = val; /* We will strdup name when we insert a new node into the rb tree. */
        } else if (strcmp("elem_size", key) == 0) {
            if (elem_size > 0) {
                pdebug(PLCTAG_DEBUG_WARN, "Overwriting attribute %s", "elem_size");
            }
            elem_size = atoi(val);
        } else if (strcmp("elem_count", key) == 0) {
            if (elem_count > 0) {
                pdebug(PLCTAG_DEBUG_WARN, "Overwriting attribute %s", "elem_count");
            }
            elem_count = atoi(val);
        }
    }

    if (name == NULL) {
        pdebug(PLCTAG_DEBUG_WARN, "Missing attribute %s", "name");
        ret = PLCTAG_ERR_BAD_PARAM;
        goto done;
    }
    if (elem_size == 0) {
        pdebug(PLCTAG_DEBUG_WARN, "Missing attribute %s", "elem_size");
        ret = PLCTAG_ERR_BAD_PARAM;
        goto done;
    }
    if (elem_count == 0) {
        pdebug(PLCTAG_DEBUG_WARN, "Missing attribute %s", "elem_size");
        ret = PLCTAG_ERR_BAD_PARAM;
        goto done;
    }

    if (pthread_rwlock_wrlock(&plcstub_mtx)) {
        err(1, "pthread_rwlock_wrlock");
    }

    /* TODO: special case for the empty tree?. */
    tag = RB_MAX(tag_tree_t, &tag_tree);
    if (tag == NULL) {
        ret = 1;
    } else {
        ret = tag->tag_id + 1;
    }

    tag = malloc(sizeof(struct tag));
    if (tag == NULL) {
        err(1, "malloc");
    }
    asprintf(&tag->name, "DUMMY_AQUA_DATA_%s", name);
    if (tag->name == NULL) {
        err(1, "asnprintf");
    }
    tag->tag_id = ret;
    tag->elem_count = elem_count;
    tag->elem_size = elem_size;

    tag->data = calloc(tag->elem_count, tag->elem_size);
    if (tag->data == NULL) {
        err(1, "calloc");
    }

    if (pthread_rwlock_unlock(&plcstub_mtx)) {
        err(1, "pthread_rwlock_unlock");
    }

done:
    free(str);
    return ret;
}

/* Stubs out the tag read path.  Only checks that the arguments
 * are valid.  It might be interesting to stub out "in-flight"
 * reads and writes for a heavily-concurrent integration test
 * but I suspect that isn't worth our effort.
 */
int
plc_tag_read(int32_t tag_id, int timeout)
{
    (void)(timeout);

    int ret = PLCTAG_STATUS_OK;
    struct tag* t;

    plcstub_init();

    if (timeout < 0) {
        pdebug(PLCTAG_DEBUG_WARN, "Timeout must not be negative");
        return PLCTAG_ERR_BAD_PARAM;
    }

    if (pthread_rwlock_rdlock(&plcstub_mtx)) {
        err(1, "pthread_rwlock_dock");
    }

    t = plcstub_tag_lookup(tag_id);
    if (!t) {
        pdebug(PLCTAG_DEBUG_WARN, "Unknown tag %d", tag_id);
        ret = PLCTAG_ERR_NOT_FOUND;
        goto done;
    }

    if (t->cb) {
        pdebug(PLCTAG_DEBUG_SPEW,
            "Calling cb for %d with PLCTAG_READ_EVENT_STARTED", tag_id);
        t->cb(tag_id, PLCTAG_EVENT_READ_STARTED, PLCTAG_STATUS_OK);
        pdebug(PLCTAG_DEBUG_SPEW,
            "Calling cb for %d with PLCTAG_READ_EVENT_COMPLETED", tag_id);
        t->cb(tag_id, PLCTAG_EVENT_READ_COMPLETED, PLCTAG_STATUS_OK);
    }

done:
    if (pthread_rwlock_unlock(&plcstub_mtx)) {
        err(1, "pthread_rwlock_unlock");
    }

    return ret;
}

int
plc_tag_register_callback(int32_t tag_id, tag_callback_func cb)
{
    int ret = PLCTAG_STATUS_OK;
    struct tag* t;

    plcstub_init();

    if (pthread_rwlock_wrlock(&plcstub_mtx)) {
        err(1, "pthread_rwlock_wrlock");
    }

    t = plcstub_tag_lookup(tag_id);
    if (!t) {
        pdebug(PLCTAG_DEBUG_WARN, "Unknown tag %d", tag_id);
        ret = PLCTAG_ERR_NOT_FOUND;
        goto done;
    }
    t->cb = cb;

done:
    if (pthread_rwlock_unlock(&plcstub_mtx)) {
        err(1, "pthread_rwlock_unlock");
    }

    return ret;
}

void
plc_tag_set_debug_level(int level)
{
    plcstub_init();
    set_debug_level(level);
}

int
plc_tag_set_int32(int32_t tag, int offset, int value)
{
    int ret;

    struct tag* t;

    /* TODO: how is offset meant to be used within a tag? */

    plcstub_init();

    if (pthread_rwlock_wrlock(&plcstub_mtx)) {
        err(1, "pthread_rwlock_wrlock");
    }

    t = plcstub_tag_lookup(tag);
    if (!t) {
        pdebug(PLCTAG_DEBUG_WARN, "Unknown tag %d", tag);
        ret = PLCTAG_ERR_NOT_FOUND;
        goto done;
    }

    if (t->cb) {
        pdebug(PLCTAG_DEBUG_SPEW,
            "Calling cb for %d with PLCTAG_WRITE_EVENT_STARTED", tag);
        t->cb(tag, PLCTAG_EVENT_WRITE_STARTED, PLCTAG_STATUS_OK);
    }

    // TODO
    //t->val = value;

    if (t->cb) {
        pdebug(PLCTAG_DEBUG_SPEW,
            "Calling cb for %d with PLCTAG_WRITE_EVENT_COMPLETED", tag);
        t->cb(tag, PLCTAG_EVENT_WRITE_COMPLETED, PLCTAG_STATUS_OK);
    }

done:
    if (pthread_rwlock_unlock(&plcstub_mtx)) {
        err(1, "pthread_rwlock_unlock");
    }
    return ret;
}

int
plc_tag_status(int32_t tag)
{
    int ret = PLCTAG_STATUS_OK;
    struct tag* t;

    if (pthread_rwlock_rdlock(&plcstub_mtx)) {
        err(1, "pthread_rwlock_rdlock");
    }

    t = plcstub_tag_lookup(tag);
    if (!t) {
        pdebug(PLCTAG_DEBUG_WARN, "Unknown tag %d", tag);
        ret = PLCTAG_ERR_NOT_FOUND;
        goto done;
    }

    /* For the stub, let's always just treat the tag status as 
     * okay.  If we stub out in-flight reads and writes later on,
     * this would change.
     */

done:
    if (pthread_rwlock_unlock(&plcstub_mtx)) {
        err(1, "pthread_rwlock_unlock");
    }
    return ret;
}

int
plc_tag_unregister_callback(int32_t tag_id)
{
    return plc_tag_register_callback(tag_id, NULL);
}
