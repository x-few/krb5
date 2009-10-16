/*
 * plugins/kdb/hdb/kdb_hdb.c
 *
 * Copyright 2009 by the Massachusetts Institute of Technology.
 * All Rights Reserved.
 *
 * Export of this software from the United States of America may
 *   require a specific license from the United States Government.
 *   It is the responsibility of any person or organization contemplating
 *   export to obtain such a license before exporting.
 *
 * WITHIN THAT CONSTRAINT, permission to use, copy, modify, and
 * distribute this software and its documentation for any purpose and
 * without fee is hereby granted, provided that the above copyright
 * notice appear in all copies and that both that copyright notice and
 * this permission notice appear in supporting documentation, and that
 * the name of M.I.T. not be used in advertising or publicity pertaining
 * to distribution of the software without specific, written prior
 * permission.  Furthermore if you modify this software you must label
 * your software as modified software and not distribute it in such a
 * fashion that it might be confused with the original M.I.T. software.
 * M.I.T. makes no representations about the suitability of
 * this software for any purpose.  It is provided "as is" without express
 * or implied warranty.
 *
 */

#include "k5-int.h"

#if HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <db.h>
#include <stdio.h>
#include <errno.h>
#include <utime.h>
#include "kdb5.h"
#include "kdb_hdb.h"

static krb5_error_code
kh_init(void)
{
    return 0;
}

static krb5_error_code
kh_fini(void)
{
    return 0;
}

krb5_error_code
kh_map_error(krb5_error_code code)
{
    switch (code) {
    case HDB_ERR_UK_SERROR:
        code = KRB5_KDB_UK_SERROR;
        break;
    case HDB_ERR_UK_RERROR:
        code = KRB5_KDB_UK_RERROR;
        break;
    case HDB_ERR_NOENTRY:
        code = KRB5_KDB_NOENTRY;
        break;
    case HDB_ERR_DB_INUSE:
        code = KRB5_KDB_DB_INUSE;
        break;
    case HDB_ERR_DB_CHANGED:
        code = KRB5_KDB_DB_CHANGED;
        break;
    case HDB_ERR_RECURSIVELOCK:
        code = KRB5_KDB_RECURSIVELOCK;
        break;
    case HDB_ERR_NOTLOCKED:
        code = KRB5_KDB_NOTLOCKED;
        break;
    case HDB_ERR_BADLOCKMODE:
        code = KRB5_KDB_BADLOCKMODE;
        break;
    case HDB_ERR_CANT_LOCK_DB:
        code = KRB5_KDB_CANTLOCK_DB;
        break;
    case HDB_ERR_EXISTS:
        code = EEXIST;
        break;
    case HDB_ERR_BADVERSION:
        code = KRB5_KDB_BAD_VERSION;
        break;
    case HDB_ERR_NO_MKEY:
        code = KRB5_KDB_NOMASTERKEY;
        break;
    case HDB_ERR_MANDATORY_OPTION:
        code = KRB5_KDB_DBTYPE_NOSUP;
        break;
    default:
        break;
    }

    return code;
}

static void
kh_db_context_free(krb5_context context, kh_db_context *kh)
{
    if (kh != NULL) {
        if (kh->hdb != NULL)
            (*kh->hdb->hdb_destroy)(kh->hcontext, kh->hdb);
        if (kh->hcontext != NULL)
            (*kh->heim_free_context)(kh->hcontext);
        if (kh->libkrb5 != NULL)
            krb5int_close_plugin(kh->libkrb5);
        if (kh->libhdb != NULL)
            krb5int_close_plugin(kh->libhdb);
        if (kh->windc != NULL)
            (*kh->windc->fini)(kh->windc_ctx);
        krb5int_close_plugin_dirs(&kh->windc_plugins);
        krb5int_mutex_free(kh->lock);
        free(kh);
    }
}

static krb5_error_code
kh_db_context_init(krb5_context context,
                   const char *libdir,
                   const char *filename,
                   kh_db_context **pkh)
{
    kh_db_context *kh;
    krb5_error_code code;
    char *libhdb = NULL;
    char *libkrb5 = NULL;
    struct errinfo errinfo;
    int *hdb_interface_version;

    if (libdir == NULL)
        return KRB5_KDB_DBTYPE_INIT; /* XXX */

    kh = k5alloc(sizeof(*kh), &code);
    if (code != 0)
        goto cleanup;

    code = krb5int_mutex_alloc(&kh->lock);
    if (code != 0)
        goto cleanup;

    if (asprintf(&libkrb5, "%s/libkrb5%s", libdir, SHLIBEXT) < 0) {
        code = ENOMEM;
        goto cleanup;
    }

    code = krb5int_open_plugin(libkrb5, &kh->libkrb5, &errinfo);
    if (code != 0)
        goto cleanup;

#define GET_PLUGIN_FUNC(_lib, _sym, _member)     do { \
    code = krb5int_get_plugin_func(kh->_lib, _sym, \
                                   (void (**)())&kh->_member, &errinfo); \
    if (code != 0) \
        goto cleanup; \
    } while (0)

    GET_PLUGIN_FUNC(libkrb5, "krb5_init_context",     heim_init_context);
    GET_PLUGIN_FUNC(libkrb5, "krb5_free_context",     heim_free_context);
    GET_PLUGIN_FUNC(libkrb5, "krb5_free_principal",   heim_free_principal);
    GET_PLUGIN_FUNC(libkrb5, "krb5_free_addresses",   heim_free_addresses);
    GET_PLUGIN_FUNC(libkrb5, "krb5_pac_free",         heim_pac_free);
    GET_PLUGIN_FUNC(libkrb5, "krb5_pac_parse",        heim_pac_parse);
    GET_PLUGIN_FUNC(libkrb5, "krb5_pac_verify",       heim_pac_verify);
    GET_PLUGIN_FUNC(libkrb5, "_krb5_pac_sign",        heim_pac_sign);
    
    if (asprintf(&libhdb, "%s/libhdb%s", libdir, SHLIBEXT) < 0)
        goto cleanup;

    code = krb5int_open_plugin(libhdb, &kh->libhdb, &errinfo);
    if (code != 0)
        goto cleanup;

    /*
     * New versions of Heimdal export this symbol to mark the
     * HDB ABI version.
     */
    if (krb5int_get_plugin_data(kh->libhdb, "hdb_interface_version",
                                (void **)&hdb_interface_version,
                                &errinfo) == 0 &&
        *hdb_interface_version != HDB_INTERFACE_VERSION) {
        code = KRB5_KDB_DBTYPE_NOSUP;
        goto cleanup;
    }

    GET_PLUGIN_FUNC(libhdb,  "hdb_create",            hdb_create);
    GET_PLUGIN_FUNC(libhdb,  "hdb_free_entry",        hdb_free_entry);

    code = kh_map_error((*kh->heim_init_context)(&kh->hcontext));
    if (code != 0)
        goto cleanup;

    code = kh_map_error((*kh->hdb_create)(kh->hcontext, &kh->hdb, filename));
    if (code != 0)
        goto cleanup;

    kh_hdb_windc_init(context, libdir, kh);

cleanup:
    if (code != 0) {
        kh_db_context_free(context, kh);
        kh = NULL;
    }

    *pkh = kh;

    return code;
}

static krb5_error_code
kh_init_module(krb5_context context,
               char *conf_section,
               char **db_args,
               int mode)
{
    kdb5_dal_handle *dal_handle = context->dal_handle;
    krb5_error_code code;
    kh_db_context *kh;
    char *libdir = NULL;
    char *filename = NULL;

    if (dal_handle->db_context != NULL) {
        kh_db_context_free(context, dal_handle->db_context);
        dal_handle->db_context = NULL;
    }

    code = profile_get_string(context->profile,
                              KDB_MODULE_SECTION,
                              conf_section,
                              "heimdal_libdir",
                              NULL,
                              &libdir);
    if (code != 0)
        goto cleanup;

    code = profile_get_string(context->profile,
                              KDB_MODULE_SECTION,
                              conf_section,
                              "heimdal_dbname",
                              NULL,
                              &filename);
    if (code != 0)
        goto cleanup;

    code = kh_db_context_init(context, libdir, filename, &kh);
    if (code != 0)
        goto cleanup;

    dal_handle->db_context = kh;

cleanup:
    if (libdir != NULL)
        free(libdir);
    if (filename != NULL)
        free(filename);

    return 0;
}

static krb5_error_code
kh_fini_module(krb5_context context)
{
    kdb5_dal_handle *dal_handle = context->dal_handle;

    kh_db_context_free(context, dal_handle->db_context);
    dal_handle->db_context = NULL;

    return 0;
}

static krb5_error_code
kh_db_create(krb5_context context,
             char *conf_section,
             char **db_args)
{
    return KRB5_KDB_DBTYPE_NOSUP;
}

static krb5_error_code
kh_db_destroy(krb5_context context,
              char *conf_section,
              char **db_args)
{
    return KRB5_KDB_DBTYPE_NOSUP;
}

static krb5_error_code
kh_db_get_age(krb5_context context,
              char *db_name,
              time_t *age)
{
    return KRB5_KDB_DBTYPE_NOSUP;
}

static krb5_error_code
kh_db_set_option(krb5_context context,
                  int option,
                  void *value)
{
    return KRB5_KDB_DBTYPE_NOSUP;
}

static krb5_error_code
kh_db_lock(krb5_context context, int mode)
{
    return 0;
}

static krb5_error_code
kh_db_unlock(krb5_context context)
{
    return 0;
}

static krb5_error_code
kh_hdb_open(krb5_context context,
            kh_db_context *kh,
            int oflag,
            mode_t mode)
{
    krb5_error_code hcode;

    hcode = (*kh->hdb->hdb_open)(kh->hcontext, kh->hdb, oflag, mode);

    return kh_map_error(hcode);
}

static krb5_error_code
kh_hdb_close(krb5_context context,kh_db_context *kh)
{
    krb5_error_code hcode;

    hcode = (*kh->hdb->hdb_close)(kh->hcontext, kh->hdb);

    return kh_map_error(hcode);
}

static krb5_error_code
kh_hdb_fetch(krb5_context context,
             kh_db_context *kh,
             const Principal *princ,
             unsigned int flags,
             hdb_entry_ex *entry)
{
    krb5_error_code hcode;

    hcode = (*kh->hdb->hdb_fetch)(kh->hcontext, kh->hdb, princ, flags, entry);

    return kh_map_error(hcode);
}

static krb5_error_code
kh_hdb_firstkey(krb5_context context,
                kh_db_context *kh,
                unsigned int flags,
                hdb_entry_ex *entry)
{
    krb5_error_code hcode;

    hcode = (*kh->hdb->hdb_firstkey)(kh->hcontext, kh->hdb, flags, entry);

    return kh_map_error(hcode);
}

static krb5_error_code
kh_hdb_nextkey(krb5_context context,
               kh_db_context *kh,
               unsigned int flags,
               hdb_entry_ex *entry)
{
    krb5_error_code hcode;

    hcode = (*kh->hdb->hdb_nextkey)(kh->hcontext, kh->hdb, flags, entry);

    return kh_map_error(hcode);
}

static HDB_extension *
kh_hdb_find_extension(const hdb_entry *entry, unsigned int type)
{
    unsigned int i;
    HDB_extension *ret = NULL;

    if (entry->extensions != NULL) {
        for (i = 0; i < entry->extensions->len; i++) {
            if (entry->extensions->val[i].data.element == type) {
                ret = &entry->extensions->val[i];
                break;
            }
        }
    }

    return ret;
}

static void
kh_hdb_free_entry(krb5_context context,
                  kh_db_context *kh,
                  hdb_entry_ex *entry)
{
    (*kh->hdb_free_entry)(kh->hcontext, entry);
}

static void
kh_kdb_free_entry(krb5_context context,
                  kh_db_context *kh,
                  krb5_db_entry *entry)
{
    krb5_tl_data *tl_data_next = NULL;
    krb5_tl_data *tl_data = NULL;
    int i, j;

    if (entry->e_data != NULL) {
        assert(entry->e_length == sizeof(hdb_entry_ex));
        kh_hdb_free_entry(context, kh, KH_DB_ENTRY(entry));
        free(entry->e_data);
    }

    krb5_free_principal(context, entry->princ);

    for (tl_data = entry->tl_data; tl_data; tl_data = tl_data_next) {
        tl_data_next = tl_data->tl_data_next;
        if (tl_data->tl_data_contents != NULL)
            free(tl_data->tl_data_contents);
        free(tl_data);
    }

    if (entry->key_data != NULL) {
        for (i = 0; i < entry->n_key_data; i++) {
            for (j = 0; j < entry->key_data[i].key_data_ver; j++) {
                if (entry->key_data[i].key_data_length[j] != 0) {
                    if (entry->key_data[i].key_data_contents[j] != NULL) {
                        memset(entry->key_data[i].key_data_contents[j],
                               0,
                               entry->key_data[i].key_data_length[j]);
                        free(entry->key_data[i].key_data_contents[j]);
                    }
                }
                entry->key_data[i].key_data_contents[j] = NULL;
                entry->key_data[i].key_data_length[j] = 0;
                entry->key_data[i].key_data_type[j] = 0;
            }
        }
        free(entry->key_data);
    }

    memset(entry, 0, sizeof(*entry));
}

void
kh_free_Principal(krb5_context context,
                  Principal *principal)
{
    kh_db_context *kh = KH_DB_CONTEXT(context);

    if (principal != NULL)
        (*kh->heim_free_principal)(kh->hcontext, principal);
}

void
kh_free_HostAddresses(krb5_context context,
                      HostAddresses *addrs)
{
    kh_db_context *kh = KH_DB_CONTEXT(context);

    if (addrs != NULL)
        (*kh->heim_free_addresses)(kh->hcontext, addrs);
}

static krb5_error_code
kh_marshal_octet_string(krb5_context context,
                         const krb5_data *in_data,
                         heim_octet_string *out_data)
{
    out_data->data = malloc(in_data->length);
    if (out_data->data == NULL)
        return ENOMEM;

    memcpy(out_data->data, in_data->data, in_data->length);

    out_data->length = in_data->length;

    return 0;
}

static krb5_error_code
kh_unmarshal_octet_string_contents(krb5_context context,
                                   const heim_octet_string *in_data,
                                   krb5_data *out_data)
{
    out_data->magic = KV5M_DATA;
    out_data->data = malloc(in_data->length);
    if (out_data->data == NULL)
        return ENOMEM;

    memcpy(out_data->data, in_data->data, in_data->length);

    out_data->length = in_data->length;

    return 0;
}

static krb5_error_code
kh_unmarshal_octet_string(krb5_context context,
                          heim_octet_string *in_data,
                          krb5_data **out_data)
{
    krb5_error_code code;

    *out_data = malloc(sizeof(krb5_data));
    if (*out_data == NULL)
        return ENOMEM;

    code = kh_unmarshal_octet_string_contents(context, in_data, *out_data);
    if (code != 0) {
        free(*out_data);
        *out_data = NULL;
        return code;
    }

    return 0;
}

static krb5_error_code
kh_marshal_general_string(krb5_context context,
                           const krb5_data *in_data,
                           heim_general_string *out_str)
{
    *out_str = malloc(in_data->length + 1);
    if (*out_str == NULL)
        return ENOMEM;

    memcpy(*out_str, in_data->data, in_data->length);
    (*out_str)[in_data->length] = '\0';

    return 0;
}

static krb5_error_code
kh_unmarshal_general_string_contents(krb5_context context,
                                     const heim_general_string in_str,
                                     krb5_data *out_data)
{
    out_data->magic = KV5M_DATA;
    out_data->length = strlen(in_str);
    out_data->data = malloc(out_data->length);
    if (out_data->data == NULL)
        return ENOMEM;

    memcpy(out_data->data, in_str, out_data->length);
    return 0;
}

static krb5_error_code
kh_unmarshal_general_string(krb5_context context,
                            const heim_general_string in_str,
                            krb5_data **out_data)
{
    krb5_error_code code;

    *out_data = malloc(sizeof(krb5_data));
    if (*out_data == NULL)
        return ENOMEM;

    code = kh_unmarshal_general_string_contents(context, in_str, *out_data);
    if (code != 0) {
        free(*out_data);
        *out_data = NULL;
        return code;
    }

    return 0;
}

krb5_error_code
kh_marshal_Principal(krb5_context context,
                     krb5_const_principal kprinc,
                     Principal **out_hprinc)
{
    krb5_error_code code;
    Principal *hprinc;
    int i;

    hprinc = k5alloc(sizeof(*hprinc), &code);
    if (code != 0)
        return code;

    hprinc->name.name_type = kprinc->type;
    hprinc->name.name_string.val = k5alloc(kprinc->length *
                                           sizeof(heim_general_string),
                                           &code);
    if (code != 0) {
        kh_free_Principal(context, hprinc);
        return code;
    }
    for (i = 0; i < kprinc->length; i++) {
        code = kh_marshal_general_string(context, &kprinc->data[i],
                                          &hprinc->name.name_string.val[i]);
        if (code != 0) {
            kh_free_Principal(context, hprinc);
            return code;
        }
        hprinc->name.name_string.len++;
    }
    code = kh_marshal_general_string(context, &kprinc->realm, &hprinc->realm);
    if (code != 0) {
        kh_free_Principal(context, hprinc);
        return code;
    }

    *out_hprinc = hprinc;

    return 0;
}

static krb5_error_code
kh_unmarshal_Principal(krb5_context context,
                       const Principal *hprinc,
                       krb5_principal *out_kprinc)
{
    krb5_error_code code;
    krb5_principal kprinc;
    unsigned int i;

    kprinc = k5alloc(sizeof(*kprinc), &code);
    if (code != 0)
        return code;

    kprinc->magic = KV5M_PRINCIPAL;
    kprinc->type = hprinc->name.name_type;
    kprinc->data = k5alloc(hprinc->name.name_string.len * sizeof(krb5_data),
                           &code);
    if (code != 0) {
        krb5_free_principal(context, kprinc);
        return code;
    }
    for (i = 0; i < hprinc->name.name_string.len; i++) {
        code = kh_unmarshal_general_string_contents(context,
                                                    hprinc->name.name_string.val[i],
                                                    &kprinc->data[i]);
        if (code != 0) {
            krb5_free_principal(context, kprinc);
            return code;
        }
        kprinc->length++;
    }
    code = kh_unmarshal_general_string_contents(context,
                                                hprinc->realm,
                                                &kprinc->realm);
    if (code != 0) {
        krb5_free_principal(context, kprinc);
        return code;
    }

    *out_kprinc = kprinc;

    return 0;
}

static krb5_error_code
kh_unmarshal_Event(krb5_context context,
                   const Event *event,
                   krb5_db_entry *kentry)
{
    krb5_error_code code;
    krb5_principal princ = NULL;

    if (event->principal != NULL) {
        code = kh_unmarshal_Principal(context, event->principal, &princ);
        if (code != 0)
            return code;
    }

    code = krb5_dbe_update_mod_princ_data(context, kentry,
                                          event->time, princ);

    krb5_free_principal(context, princ);

    return code;
}

static krb5_error_code
kh_unmarshal_HDBFlags(krb5_context context,
                      HDBFlags hflags,
                      krb5_flags *kflags)
{
    *kflags = 0;

    if (hflags.initial)
        *kflags |= KRB5_KDB_DISALLOW_TGT_BASED;
    if (!hflags.forwardable)
        *kflags |= KRB5_KDB_DISALLOW_FORWARDABLE;
    if (!hflags.proxiable)
        *kflags |= KRB5_KDB_DISALLOW_PROXIABLE;
    if (!hflags.renewable)
        *kflags |= KRB5_KDB_DISALLOW_RENEWABLE;
    if (!hflags.postdate)
        *kflags |= KRB5_KDB_DISALLOW_POSTDATED;
    if (!hflags.server)
        *kflags |= KRB5_KDB_DISALLOW_SVR;
    if (hflags.client)
        ;
    if (hflags.invalid)
        *kflags |= KRB5_KDB_DISALLOW_ALL_TIX;
    if (hflags.require_preauth)
        *kflags |= KRB5_KDB_REQUIRES_PRE_AUTH;
    if (hflags.change_pw)
        *kflags |= KRB5_KDB_PWCHANGE_SERVICE;
    if (hflags.require_hwauth)
        *kflags |= KRB5_KDB_REQUIRES_HW_AUTH;
    if (hflags.ok_as_delegate)
        *kflags |= KRB5_KDB_OK_AS_DELEGATE;
    if (hflags.user_to_user)
        ;
    if (hflags.immutable)
        ;
    if (hflags.trusted_for_delegation)
        *kflags |= KRB5_KDB_OK_TO_AUTH_AS_DELEGATE;
    if (hflags.allow_kerberos4)
        ;
    if (hflags.allow_digest)
        ;
    return 0;
}

static krb5_error_code
kh_unmarshal_Key(krb5_context context,
                 const hdb_entry *hentry,
                 const Key *hkey,
                 krb5_key_data *kkey)
{
    memset(kkey, 0, sizeof(*kkey));

    kkey->key_data_ver = KRB5_KDB_V1_KEY_DATA_ARRAY;
    kkey->key_data_kvno = hentry->kvno;

    kkey->key_data_type[0] = hkey->key.keytype;
    kkey->key_data_contents[0] = malloc(hkey->key.keyvalue.length);
    if (kkey->key_data_contents[0] == NULL)
        return ENOMEM;

    memcpy(kkey->key_data_contents[0], hkey->key.keyvalue.data,
           hkey->key.keyvalue.length);
    kkey->key_data_length[0] = hkey->key.keyvalue.length;

    if (hkey->salt != NULL) {
        switch (hkey->salt->type) {
        case hdb_pw_salt:
            kkey->key_data_type[1] = KRB5_KDB_SALTTYPE_NORMAL;
            break;
        case hdb_afs3_salt:
            kkey->key_data_type[1] = KRB5_KDB_SALTTYPE_AFS3;
            break;
        default:
            kkey->key_data_type[1] = KRB5_KDB_SALTTYPE_SPECIAL;
            break;
        }

        kkey->key_data_contents[1] = malloc(hkey->salt->salt.length);
        if (kkey->key_data_contents[1] == NULL) {
            memset(kkey->key_data_contents[0], 0, kkey->key_data_length[0]);
            free(kkey->key_data_contents[0]);
            return ENOMEM;
        }
        memcpy(kkey->key_data_contents[1], hkey->salt->salt.data,
               hkey->salt->salt.length);
        kkey->key_data_length[1] = hkey->salt->salt.length;
    }

    return 0;
}

/*
 * Extension marshalers
 */

static krb5_error_code
kh_unmarshal_HDB_extension_data_last_pw_change(krb5_context context,
                                               HDB_extension *hext,
                                               krb5_db_entry *kentry)
{
    return krb5_dbe_update_last_pwd_change(context, kentry,
                                           hext->data.u.last_pw_change);
}

typedef krb5_error_code (*kh_hdb_marshal_extension_fn)(krb5_context,
                                                        HDB_extension *,
                                                        krb5_db_entry *);

static kh_hdb_marshal_extension_fn kh_hdb_extension_vtable[] = {

    NULL,           /* choice_HDB_extension_data_asn1_ellipsis */
    NULL,           /* choice_HDB_extension_data_pkinit_acl */
    NULL,           /* choice_HDB_extension_data_pkinit_cert_hash */
    NULL,           /* choice_HDB_extension_data_allowed_to_delegate_to */
    NULL,           /* choice_HDB_extension_data_lm_owf */
    NULL,           /* choice_HDB_extension_data_password */
    NULL,           /* choice_HDB_extension_data_aliases */
    kh_unmarshal_HDB_extension_data_last_pw_change
};

static krb5_error_code
kh_unmarshal_HDB_extension(krb5_context context,
                           HDB_extension *hext,
                           krb5_db_entry *kentry)
{
    static const size_t nexts =
        sizeof(kh_hdb_extension_vtable) / sizeof(kh_hdb_extension_vtable[0]);
    kh_hdb_marshal_extension_fn marshal = NULL;

    if (hext->data.element < nexts)
        marshal = kh_hdb_extension_vtable[hext->data.element];

    if (marshal == NULL)
        return hext->mandatory ? KRB5_KDB_DBTYPE_NOSUP : 0;

    return (*marshal)(context, hext, kentry);
}


static krb5_error_code
kh_unmarshal_HDB_extensions(krb5_context context,
                            HDB_extensions *hexts,
                            krb5_db_entry *kentry)
{
    unsigned int i;
    krb5_error_code code = 0;

    for (i = 0; i < hexts->len; i++) {
        code = kh_unmarshal_HDB_extension(context, &hexts->val[i], kentry);
        if (code != 0)
            break;
    }

    return code;
}

static krb5_error_code
kh_unmarshal_hdb_entry(krb5_context context,
                       const hdb_entry *hentry,
                       krb5_db_entry *kentry)
{
    kh_db_context *kh = KH_DB_CONTEXT(context);
    krb5_error_code code;
    unsigned int i;

    memset(kentry, 0, sizeof(*kentry));

    code = kh_unmarshal_Principal(context, hentry->principal, &kentry->princ);
    if (code != 0)
        goto cleanup;

    code = kh_unmarshal_HDBFlags(context, hentry->flags, &kentry->attributes);
    if (code != 0)
        goto cleanup;

    if (hentry->max_life != NULL)
        kentry->max_life = *(hentry->max_life);
    if (hentry->max_renew != NULL)
        kentry->max_renewable_life = *(hentry->max_renew);
    if (hentry->valid_end != NULL)
        kentry->expiration = *(hentry->valid_end);
    if (hentry->pw_end != NULL)
        kentry->pw_expiration = *(hentry->pw_end);

    /* last_success */
    /* last_failed */
    /* fail_auth_count */
    /* n_tl_data */
    /* e_length */
    /* e_data */

    code = kh_unmarshal_Event(context,
                              hentry->modified_by ? hentry->modified_by :
                                                    &hentry->created_by,
                              kentry);
    if (code != 0)
        goto cleanup;

    code = kh_unmarshal_HDB_extensions(context, hentry->extensions, kentry);
    if (code != 0)
        goto cleanup;

    kentry->key_data = k5alloc(hentry->keys.len * sizeof(krb5_key_data), &code);
    if (code != 0)
        goto cleanup;

    for (i = 0; i < hentry->keys.len; i++) {
        code = kh_unmarshal_Key(context, hentry,
                                &hentry->keys.val[i],
                                &kentry->key_data[i]);
        if (code != 0)
            goto cleanup;

        kentry->n_key_data++;
    }

cleanup:
    if (code != 0)
        kh_kdb_free_entry(context, kh, kentry);

    return code;
}

static krb5_error_code
kh_is_tgs_principal(krb5_context context,
                    krb5_const_principal princ)
{
    return krb5_princ_size(context, princ) == 2 &&
        data_eq_string(princ->data[0], KRB5_TGS_NAME);
}

static krb5_error_code
kh_db_get_principal(krb5_context context,
                    krb5_const_principal princ,
                    unsigned int kflags,
                    krb5_db_entry *kentry,
                    int *nentries,
                    krb5_boolean *more)
{
    krb5_error_code code;
    kh_db_context *kh = KH_DB_CONTEXT(context);
    Principal *hprinc = NULL;
    hdb_entry_ex *hentry = NULL;
    unsigned int hflags = 0;

    *nentries = 0;
    *more = FALSE;

    if (kh == NULL)
        return KRB5_KDB_DBNOTINITED;

    code = k5_mutex_lock(kh->lock);
    if (code != 0)
        return code;

    code = kh_marshal_Principal(context, princ, &hprinc);
    if (code != 0)
        goto cleanup;

    code = kh_hdb_open(context, kh, O_RDONLY, 0);
    if (code != 0)
        goto cleanup;

    hflags = HDB_F_DECRYPT;
    if (kflags & KRB5_KDB_FLAG_CANONICALIZE)
        hflags |= HDB_F_CANON;
    if (kflags & KRB5_KDB_FLAG_CLIENT_REFERRALS_ONLY)
        hflags |= HDB_F_GET_CLIENT;
    else if (kflags & KRB5_KDB_FLAG_INCLUDE_PAC)
        hflags |= HDB_F_GET_SERVER;
    else if (kh_is_tgs_principal(context, princ))
        hflags |= HDB_F_GET_KRBTGT;
    else
        hflags |= HDB_F_GET_ANY;

    hentry = k5alloc(sizeof(*hentry), &code);
    if (code != 0)
        goto cleanup;

    code = kh_hdb_fetch(context, kh, hprinc, hflags, hentry);
    if (code != 0) {
        kh_hdb_close(context, kh);
        goto cleanup;
    }

    if (kh_unmarshal_hdb_entry(context, &hentry->entry, kentry) == 0) {
        *nentries = 1;
        kentry->e_length = sizeof(*hentry);
        kentry->e_data = (krb5_octet *)hentry;
        hentry = NULL;
    }

    kh_hdb_close(context, kh);

cleanup:
    if (hentry != NULL) {
        kh_hdb_free_entry(context, kh, hentry);
        free(hentry);
    }
    kh_free_Principal(context, hprinc);
    k5_mutex_unlock(kh->lock);

    return 0;
}

static krb5_error_code
kh_db_free_principal(krb5_context context,
                     krb5_db_entry *entry,
                     int count)
{
    kh_db_context *kh = KH_DB_CONTEXT(context);
    krb5_error_code code;
    int i;

    code = k5_mutex_lock(kh->lock);
    if (code != 0)
        return code;

    for (i = 0; i < count; i++)
        kh_kdb_free_entry(context, kh, &entry[i]);

    k5_mutex_unlock(kh->lock);

    return 0;
}

static krb5_error_code
kh_db_put_principal(krb5_context context,
                    krb5_db_entry *entries,
                    int *nentries,
                    char **db_args)
{
    return KRB5_KDB_DBTYPE_NOSUP;
}

static krb5_error_code
kh_db_iterate(krb5_context context,
              char *match_entry,
              int (*func)(krb5_pointer, krb5_db_entry *),
              krb5_pointer func_arg)
{
    krb5_error_code code;
    kh_db_context *kh = KH_DB_CONTEXT(context);
    hdb_entry_ex hentry;
    unsigned int hflags = HDB_F_DECRYPT | HDB_F_GET_ANY;

    if (kh == NULL)
        return KRB5_KDB_DBNOTINITED;

    code = k5_mutex_lock(kh->lock);
    if (code != 0)
        return code;

    memset(&hentry, 0, sizeof(hentry));

    code = kh_hdb_open(context, kh, O_RDONLY, 0);
    if (code != 0)
        goto cleanup;

    code = kh_hdb_firstkey(context, kh, hflags, &hentry);
    while (code == 0) {
        krb5_db_entry kentry;

        if (kh_unmarshal_hdb_entry(context, &hentry.entry, &kentry) == 0) {
            code = (*func)(func_arg, &kentry);
            kh_kdb_free_entry(context, kh, &kentry);
        }

        kh_hdb_free_entry(context, kh, &hentry);

        if (code != 0)
            break;

        code = kh_hdb_nextkey(context, kh, hflags, &hentry);
    }

    if (code == KRB5_KDB_NOENTRY)
        code = 0;

    kh_hdb_close(context, kh);

cleanup:
    k5_mutex_unlock(kh->lock);

    return 0;
}

static krb5_error_code
kh_fetch_master_key(krb5_context context,
                    krb5_principal name,
                    krb5_keyblock *key,
                    krb5_kvno *kvno,
                    char *db_args)
{
    return 0;
}

static krb5_error_code
kh_fetch_master_key_list(krb5_context context,
                         krb5_principal mname,
                         const krb5_keyblock *key,
                         krb5_kvno kvno,
                         krb5_keylist_node **mkeys_list)
{
    /* just create a dummy one so that the KDC doesn't balk */
    krb5_keylist_node *mkey;
    krb5_error_code code;

    mkey = k5alloc(sizeof(*mkey), &code);
    if (mkey == NULL)
        return code;

    mkey->keyblock.magic = KV5M_KEYBLOCK;
    mkey->keyblock.enctype = ENCTYPE_NULL;
    mkey->kvno = 1;

    *mkeys_list = mkey;

    return 0;
}

static void *
kh_db_alloc(krb5_context context, void *ptr, size_t size)
{
    return realloc(ptr, size);
}

static void
kh_db_free(krb5_context context, void *ptr)
{
    free(ptr);
}

static krb5_error_code
kh_set_master_key(krb5_context context,
                  char *pwd,
                  krb5_keyblock *key)
{
    return 0;
}

static krb5_error_code
kh_get_master_key(krb5_context context,
                  krb5_keyblock **key)
{
    krb5_error_code code;

    *key = k5alloc(sizeof(krb5_keyblock), &code);

    return code;
}

static krb5_error_code
kh_dbekd_decrypt_key_data(krb5_context context,
                          const krb5_keyblock *mkey,
                          const krb5_key_data *key_data,
                          krb5_keyblock *dbkey,
                          krb5_keysalt *keysalt)
{
    dbkey->magic = KV5M_KEYBLOCK;

    dbkey->enctype = key_data->key_data_type[0];
    dbkey->contents = malloc(key_data->key_data_length[0]);
    if (dbkey->contents == NULL) {
        return ENOMEM;
    }
    memcpy(dbkey->contents, key_data->key_data_contents[0],
           key_data->key_data_length[0]);
    dbkey->length = key_data->key_data_length[0];

    if (keysalt != NULL) {
        keysalt->type = key_data->key_data_type[1];
        keysalt->data.data = malloc(key_data->key_data_length[1]);
        if (keysalt->data.data == NULL) {
            memset(dbkey->contents, 0, dbkey->length);
            free(dbkey->contents);
            return ENOMEM;
        }
        memcpy(keysalt->data.data, key_data->key_data_contents[1],
               key_data->key_data_length[1]);
        keysalt->data.length = key_data->key_data_length[1];
    }

    return 0;
}

static krb5_error_code
kh_dbekd_encrypt_key_data(krb5_context context,
                          const krb5_keyblock *mkey,
                          const krb5_keyblock *dbkey,
                          const krb5_keysalt *keysalt,
                          int keyver,
                          krb5_key_data *key_data)
{
    return 0;
}

/*
 * Invoke methods
 */

static krb5_error_code
kh_db_check_allowed_to_delegate(krb5_context context,
                                unsigned int method,
                                const krb5_data *req_data,
                                krb5_data *rep_data)
{
    kdb_check_allowed_to_delegate_req *req;
    krb5_error_code code;
    hdb_entry_ex *hentry;
    HDB_extension *ext;
    HDB_Ext_Constrained_delegation_acl *acl;
    unsigned int i;

    req = (kdb_check_allowed_to_delegate_req *)req_data->data;
    hentry = KH_DB_ENTRY(req->server);
    ext = kh_hdb_find_extension(&hentry->entry,
                                choice_HDB_extension_data_allowed_to_delegate_to);

    code = KRB5KDC_ERR_POLICY;

    if (ext != NULL) {
        acl = &ext->data.u.allowed_to_delegate_to;

        for (i = 0; i < acl->len; i++) {
            krb5_principal princ;

            if (kh_unmarshal_Principal(context, &acl->val[i], &princ) == 0) {
                if (krb5_principal_compare(context, req->proxy, princ)) {
                    code = 0;
                    krb5_free_principal(context, princ);
                    break;
                }
                krb5_free_principal(context, princ);
            }
        }
    }

    return code;
}

static struct _kh_invoke_fn {
    unsigned int method;
    krb5_error_code (*function)(krb5_context, unsigned int,
                                const krb5_data *, krb5_data *);
} kh_invoke_vtable[] = {
    { KRB5_KDB_METHOD_CHECK_POLICY_AS,           kh_db_check_policy_as },
    { KRB5_KDB_METHOD_SIGN_AUTH_DATA,            kh_db_sign_auth_data },
    { KRB5_KDB_METHOD_CHECK_ALLOWED_TO_DELEGATE, kh_db_check_allowed_to_delegate },
};

static krb5_error_code
kh_db_invoke(krb5_context context,
             unsigned int method,
             const krb5_data *req,
             krb5_data *rep)
{
    kh_db_context *kh = KH_DB_CONTEXT(context);
    size_t i;
    krb5_error_code code;

    code = k5_mutex_lock(kh->lock);
    if (code != 0)
        return code;

    code = KRB5_KDB_DBTYPE_NOSUP;

    for (i = 0;
         i < sizeof(kh_invoke_vtable) / sizeof(kh_invoke_vtable[0]);
         i++) {
        struct _kh_invoke_fn *fn = &kh_invoke_vtable[i];

        if (fn->method == method) {
            code = (*fn->function)(context, method, req, rep);
            break;
        }
    }

    k5_mutex_unlock(kh->lock);

    return code;
}

kdb_vftabl kdb_function_table = {
    1,
    0,
    kh_init,
    kh_fini,
    kh_init_module,
    kh_fini_module,
    kh_db_create,
    kh_db_destroy,
    kh_db_get_age,
    kh_db_set_option,
    kh_db_lock,
    kh_db_unlock,
    kh_db_get_principal,
    kh_db_free_principal,
    kh_db_put_principal,
    NULL,
    kh_db_iterate,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    kh_db_alloc,
    kh_db_free,
    kh_set_master_key,
    kh_get_master_key,
    NULL,
    NULL,
    NULL,
    NULL,
    kh_fetch_master_key,
    NULL,
    kh_fetch_master_key_list,
    NULL,
    NULL,
    NULL,
    NULL,
    kh_dbekd_decrypt_key_data,
    kh_dbekd_encrypt_key_data,
    kh_db_invoke,
};
