/*
 * Copyright (C) 2019 Kevin J. McCarthy <kevin@8t8.us>
 *
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program; if not, write to the Free Software
 *     Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#if HAVE_CONFIG_H
# include "config.h"
#endif

#include "mutt.h"
#include "crypt-gpgme.h"
#include "mutt_idna.h"
#include "autocrypt.h"
#include "autocrypt_private.h"

#include <gpgme.h>

static int create_gpgme_context (gpgme_ctx_t *ctx)
{
  gpgme_error_t err;

  err = gpgme_new (ctx);
  if (!err)
    err = gpgme_ctx_set_engine_info (*ctx, GPGME_PROTOCOL_OpenPGP, NULL,
                                     AutocryptDir);
  if (err)
  {
    mutt_error (_("error creating gpgme context: %s\n"), gpgme_strerror (err));
    sleep (2);
    return -1;
  }

  return 0;
}

int mutt_autocrypt_gpgme_init (void)
{
  pgp_gpgme_init ();
  return 0;
}

static int export_keydata (gpgme_ctx_t ctx, gpgme_key_t key, BUFFER *keydata)
{
  int rv = -1;
  gpgme_data_t dh = NULL;
  gpgme_key_t export_keys[2];
  unsigned char *export_data = NULL;
  size_t export_data_len;

  if (gpgme_data_new (&dh))
    goto cleanup;

  /* This doesn't seem to work */
#if 0
  if (gpgme_data_set_encoding (dh, GPGME_DATA_ENCODING_BASE64))
    goto cleanup;
#endif

  export_keys[0] = key;
  export_keys[1] = NULL;
  if (gpgme_op_export_keys (ctx, export_keys,
                            GPGME_EXPORT_MODE_MINIMAL,
                            dh))
    goto cleanup;

  export_data = (unsigned char *)gpgme_data_release_and_get_mem (dh, &export_data_len);
  dh = NULL;

  mutt_buffer_to_base64 (keydata, export_data, export_data_len);
  gpgme_free (export_data);
  export_data = NULL;

  rv = 0;

cleanup:
  gpgme_data_release (dh);
  return rv;
}

/* TODO: not sure if this function will be useful in the future. */
int mutt_autocrypt_gpgme_export_key (const char *keyid, BUFFER *keydata)
{
  int rv = -1;
  gpgme_ctx_t ctx = NULL;
  gpgme_key_t key = NULL;

  if (create_gpgme_context (&ctx))
    goto cleanup;

  if (gpgme_get_key (ctx, keyid, &key, 0))
    goto cleanup;

  if (export_keydata (ctx, key, keydata))
    goto cleanup;

  rv = 0;
cleanup:
  gpgme_key_unref (key);
  gpgme_release (ctx);
  return rv;
}

int mutt_autocrypt_gpgme_create_key (ADDRESS *addr, BUFFER *keyid, BUFFER *keydata)
{
  int rv = -1;
  gpgme_ctx_t ctx = NULL;
  gpgme_error_t err;
  gpgme_genkey_result_t keyresult;
  gpgme_key_t primary_key = NULL;
  char buf[LONG_STRING] = {0};

  /* gpgme says addresses should not be in idna form */
  mutt_addrlist_to_local (addr);
  rfc822_write_address (buf, sizeof(buf), addr, 0);

  if (create_gpgme_context (&ctx))
    goto cleanup;

  mutt_message _("Generating autocrypt key...");

  /* Primary key */
  err = gpgme_op_createkey (ctx, buf, "ed25519",
                            0, 0, NULL,
                            GPGME_CREATE_NOPASSWD | GPGME_CREATE_FORCE | GPGME_CREATE_NOEXPIRE);
  if (err)
  {
    mutt_error (_("Error creating autocrypt key: %s\n"), gpgme_strerror (err));
    sleep (2);
    goto cleanup;
  }
  keyresult = gpgme_op_genkey_result (ctx);
  if (!keyresult->fpr)
    goto cleanup;
  mutt_buffer_strcpy (keyid, keyresult->fpr);
  dprint (1, (debugfile, "Generated key with id %s\n", mutt_b2s (keyid)));

  /* Get gpgme_key_t to create the secondary key and export keydata */
  err = gpgme_get_key (ctx, mutt_b2s (keyid), &primary_key, 0);
  if (err)
    goto cleanup;

  /* Secondary key */
  err = gpgme_op_createsubkey (ctx, primary_key, "cv25519",
                               0, 0,
                               GPGME_CREATE_NOPASSWD | GPGME_CREATE_NOEXPIRE);
  if (err)
  {
    mutt_error (_("Error creating autocrypt key: %s\n"), gpgme_strerror (err));
    sleep (2);
    goto cleanup;
  }

  /* get keydata */
  if (export_keydata (ctx, primary_key, keydata))
    goto cleanup;
  dprint (1, (debugfile, "key has keydata *%s*\n", mutt_b2s (keydata)));

  rv = 0;

cleanup:
  mutt_addrlist_to_intl (addr, NULL);
  gpgme_key_unref (primary_key);
  gpgme_release (ctx);
  return rv;
}

int mutt_autocrypt_gpgme_import_key (const char *keydata, BUFFER *keyid)
{
  int rv = -1;
  gpgme_ctx_t ctx = NULL;
  BUFFER *raw_keydata = NULL;
  gpgme_data_t dh = NULL;
  gpgme_import_result_t result;

  if (create_gpgme_context (&ctx))
    goto cleanup;

  raw_keydata = mutt_buffer_pool_get ();
  if (!mutt_buffer_from_base64 (raw_keydata, keydata))
    goto cleanup;

  if (gpgme_data_new_from_mem (&dh, mutt_b2s (raw_keydata),
                               mutt_buffer_len (raw_keydata), 0))
    goto cleanup;

  if (gpgme_op_import (ctx, dh))
    goto cleanup;

  result = gpgme_op_import_result (ctx);
  if (!result->imports || !result->imports->fpr)
    goto cleanup;
  mutt_buffer_strcpy (keyid, result->imports->fpr);

  rv = 0;

cleanup:
  gpgme_data_release (dh);
  gpgme_release (ctx);
  mutt_buffer_pool_release (&raw_keydata);
  return rv;
}

int mutt_autocrypt_gpgme_is_valid_key (const char *keyid)
{
  int rv = 0;
  gpgme_ctx_t ctx = NULL;
  gpgme_key_t key = NULL;

  if (!keyid)
    return 0;

  if (create_gpgme_context (&ctx))
    goto cleanup;

  if (gpgme_get_key (ctx, keyid, &key, 0))
    goto cleanup;

  rv = 1;
  if (key->revoked || key->expired || key->disabled || key->invalid ||
      !key->can_encrypt)
    rv = 0;

cleanup:
  gpgme_key_unref (key);
  gpgme_release (ctx);
  return rv;
}
