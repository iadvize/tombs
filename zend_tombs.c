/*
  +----------------------------------------------------------------------+
  | tombs                                                                |
  +----------------------------------------------------------------------+
  | Copyright (c) Joe Watkins 2019                                       |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: krakjoe                                                      |
  +----------------------------------------------------------------------+
 */

#ifndef ZEND_TOMBS
# define ZEND_TOMBS

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "zend.h"
#include "zend_API.h"
#include "zend_closures.h"
#include "zend_extensions.h"
#include "zend_ini.h"
#include "zend_virtual_cwd.h"

#include "zend_tombs.h"
#include "zend_tombs_strings.h"
#include "zend_tombs_graveyard.h"
#include "zend_tombs_ini.h"
#include "zend_tombs_network.h"

typedef struct {
    zend_long limit;
    zend_long end;
    zend_bool *reserved;

    zend_tombs_graveyard_t *graveyard;
} zend_tombs_shared_t;

static zend_long            zend_tombs_shared_size;
static zend_tombs_shared_t *zend_tombs_shared;
static zend_bool            zend_tombs_started = 0;
       int                  zend_tombs_resource = -1;

#define ZTSG(v) zend_tombs_shared->v

static void (*zend_execute_function)(zend_execute_data *) = NULL;

static void zend_tombs_execute(zend_execute_data *execute_data) {
    zend_op_array *ops = (zend_op_array*) EX(func);
    zend_bool *reserved = NULL, 
              _unmarked = 0, 
              _marked   = 1;

    if (UNEXPECTED(NULL == ops->function_name)) {
        goto _zend_tombs_execute_real;
    }

    if (UNEXPECTED(NULL == (reserved = ops->reserved[zend_tombs_resource]))) {
        goto _zend_tombs_execute_real;
    }

    if (__atomic_compare_exchange(
        reserved,
        &_unmarked,
        &_marked,
        0,
        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST
    )) {
        zend_tombs_graveyard_delete(ZTSG(graveyard), reserved - ZTSG(reserved));
    }

_zend_tombs_execute_real:
    zend_execute_function(execute_data);
}

zend_extension_version_info extension_version_info = {
    ZEND_EXTENSION_API_NO,
    ZEND_EXTENSION_BUILD_ID
};

static int zend_tombs_startup(zend_extension *ze) {
    zend_tombs_ini_load();
    zend_tombs_strings_startup(zend_tombs_ini_strings);

    zend_tombs_shared_size = 
        sizeof(zend_tombs_shared) + 
        (sizeof(zend_bool*) * zend_tombs_ini_max);
    zend_tombs_shared = 
        (zend_tombs_shared_t*)
            zend_tombs_map(zend_tombs_shared_size);

    if (!zend_tombs_shared) {
#ifdef ZEND_DEBUG
        fprintf(stderr, "Failed to allocate global shared memory\n");
#endif
        zend_tombs_ini_unload();

        return FAILURE;
    }

    ZTSG(reserved) = (zend_bool*) (((char*) zend_tombs_shared) + sizeof(zend_tombs_shared_t));
    ZTSG(limit)    = zend_tombs_ini_max;
    ZTSG(end)      = 0;

    if (!(ZTSG(graveyard) = zend_tombs_graveyard_create(zend_tombs_ini_max))) {
#ifdef ZEND_DEBUG
        fprintf(stderr, "Failed to allocate graveyard\n");
#endif
        zend_tombs_unmap(zend_tombs_shared, zend_tombs_shared_size);
        zend_tombs_strings_shutdown();
        zend_tombs_ini_unload();

        return FAILURE;
    }

    if (!zend_tombs_network_startup(zend_tombs_ini_runtime, ZTSG(graveyard))) {
#ifdef ZEND_DEBUG
        fprintf(stderr, "Failed to activate network, this may be normal\n");
#endif
        zend_tombs_graveyard_destroy(ZTSG(graveyard));
        zend_tombs_unmap(zend_tombs_shared, zend_tombs_shared_size);
        zend_tombs_strings_shutdown();
        zend_tombs_ini_unload();

        return SUCCESS;
    }

    zend_tombs_resource = zend_get_resource_handle(ze);
    zend_tombs_started  = 1;

    ze->handle = 0;

    if (zend_execute_function == NULL) {
        zend_execute_function = zend_execute_ex;
        zend_execute_ex       = zend_tombs_execute;
    }

    return SUCCESS;
}

static void zend_tombs_activate(void) {
#if defined(ZTS) && defined(COMPILE_DL_TOMBS)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif

    if (INI_INT("opcache.optimization_level")) {
        zend_string *optimizer = zend_string_init(
	        ZEND_STRL("opcache.optimization_level"), 1);
        zend_long level = INI_INT("opcache.optimization_level");
        zend_string *value;

        /* disable pass 1 (pre-evaluate constant function calls) */
        level &= ~(1<<0);

        /* disable pass 4 (optimize function calls) */
        level &= ~(1<<3);

        value = zend_strpprintf(0, "0x%08X", (unsigned int) level);

        zend_alter_ini_entry(optimizer, value,
	        ZEND_INI_SYSTEM, ZEND_INI_STAGE_ACTIVATE);

        zend_string_release(optimizer);
        zend_string_release(value);
    }
}


static zend_always_inline void* zend_tombs_reserve(zend_op_array *ops) {
    zend_long end = 
        __atomic_fetch_add(
            &ZTSG(end), 1, __ATOMIC_SEQ_CST);

    if (UNEXPECTED(end >= ZTSG(limit))) {
        return NULL;
    }

    return ZTSG(reserved) + end;
}

static void zend_tombs_setup(zend_op_array *ops)
{
    zend_bool **reserved;

    if (UNEXPECTED(!zend_tombs_started)) {
        return;
    }

    if (UNEXPECTED(NULL == ops->function_name)) {
        return;
    }

    reserved =
        (zend_bool**)
            &ops->reserved[zend_tombs_resource];

    if (UNEXPECTED(NULL == (*reserved = zend_tombs_reserve(ops)))) {
        return;
    }

    zend_tombs_graveyard_insert(ZTSG(graveyard), *reserved - ZTSG(reserved), ops);
}

static void zend_tombs_shutdown(zend_extension *ze) {
    if (!zend_tombs_started) {
        return;
    }

    zend_tombs_network_shutdown();

    if (zend_tombs_ini_dump > 0) {
        zend_tombs_graveyard_dump(ZTSG(graveyard), zend_tombs_ini_dump);
    }

    zend_tombs_graveyard_destroy(ZTSG(graveyard));
    zend_tombs_unmap(zend_tombs_shared, zend_tombs_shared_size);
    zend_tombs_strings_shutdown();
    zend_tombs_ini_unload();

    if (zend_execute_function == zend_tombs_execute) {
        zend_execute_ex = zend_execute_function;
    }
}

zend_extension zend_extension_entry = {
    ZEND_TOMBS_EXTNAME,
    ZEND_TOMBS_VERSION,
    ZEND_TOMBS_AUTHOR,
    ZEND_TOMBS_URL,
    ZEND_TOMBS_COPYRIGHT,
    zend_tombs_startup,
    zend_tombs_shutdown,
    zend_tombs_activate,
    NULL,
    NULL,
    zend_tombs_setup,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    STANDARD_ZEND_EXTENSION_PROPERTIES
};

#if defined(ZTS) && defined(COMPILE_DL_TOMBS)
    ZEND_TSRMLS_CACHE_DEFINE();
#endif

#endif /* ZEND_TOMBS */