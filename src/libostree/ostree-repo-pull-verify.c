/*
 * Copyright (C) 2020 Red Hat, Inc.
 * Copyright © 2017 Endless Mobile, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.0+
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include "libglnx.h"
#include "ostree.h"
#include "otutil.h"
#include "ostree-repo-pull-private.h"
#include "ostree-repo-private.h"

#include "ostree-core-private.h"
#include "ostree-repo-static-delta-private.h"
#include "ostree-metalink.h"
#include "ostree-fetcher-util.h"
#include "ostree-remote-private.h"
#include "ot-fs-utils.h"

#include <gio/gunixinputstream.h>
#include <sys/statvfs.h>
#ifdef HAVE_LIBSYSTEMD
#include <systemd/sd-journal.h>
#endif

#include "ostree-sign.h"

static gboolean
get_signapi_remote_option (OstreeRepo *repo,
                           OstreeSign *sign,
                           const char *remote_name,
                           const char *keysuffix,
                           char      **out_value,
                           GError    **error)
{
  g_autofree char *key = g_strdup_printf ("verification-%s-%s", ostree_sign_get_name (sign), keysuffix);
  return ostree_repo_get_remote_option (repo, remote_name, key, NULL, out_value, error);
}

/* _signapi_load_public_keys:
 *
 * Load public keys according remote's configuration:
 * inlined key passed via config option `verification-<signapi>-key` or
 * file name with public keys via `verification-<signapi>-file` option.
 *
 * If both options are set then load all all public keys
 * both from file and inlined in config.
 *
 * Returns: %FALSE if any source is configured but nothing has been loaded.
 * Returns: %TRUE if no configuration or any key loaded.
 * */
static gboolean
_signapi_load_public_keys (OstreeSign *sign,
                           OstreeRepo *repo,
                           const gchar *remote_name,
                           gboolean required,
                           GError **error)
{
  g_autofree gchar *pk_ascii = NULL;
  g_autofree gchar *pk_file = NULL;
  gboolean loaded_from_file = TRUE;
  gboolean loaded_inlined = TRUE;

  if (!get_signapi_remote_option (repo, sign, remote_name, "file", &pk_file, error))
    return FALSE;
  if (!get_signapi_remote_option (repo, sign, remote_name, "key", &pk_ascii, error))
    return FALSE;

  /* return TRUE if there is no configuration for remote */
  if ((pk_file == NULL) &&(pk_ascii == NULL))
    {
      /* It is expected what remote may have verification file as
       * a part of configuration. Hence there is not a lot of sense
       * for automatic resolve of per-remote keystore file as it
       * used in find_keyring () for GPG.
       * If it is needed to add the similar mechanism, it is preferable
       * to pass the path to ostree_sign_load_pk () via GVariant options
       * and call it here for loading with method and file structure
       * specific for signature type.
       */
      if (required)
        return glnx_throw (error, "No keys found for required signapi type %s", ostree_sign_get_name (sign));
      return TRUE;
    }

  if (pk_file != NULL)
    {
      g_autoptr (GError) local_error = NULL;
      g_autoptr (GVariantBuilder) builder = NULL;
      g_autoptr (GVariant) options = NULL;

      builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
      g_variant_builder_add (builder, "{sv}", "filename", g_variant_new_string (pk_file));
      options = g_variant_builder_end (builder);

      if (ostree_sign_load_pk (sign, options, &local_error))
        loaded_from_file = TRUE;
      else
        {
          return glnx_throw (error, "Failed loading '%s' keys from '%s",
                             ostree_sign_get_name (sign), pk_file);
        }
    }

  if (pk_ascii != NULL)
    {
      g_autoptr (GError) local_error = NULL;
      g_autoptr (GVariant) pk = g_variant_new_string(pk_ascii);

      /* Add inlined public key */
      if (loaded_from_file)
        loaded_inlined = ostree_sign_add_pk (sign, pk, &local_error);
      else
        loaded_inlined = ostree_sign_set_pk (sign, pk, &local_error);

      if (!loaded_inlined)
        {
          return glnx_throw (error, "Failed loading '%s' keys from inline `verification-key`",
                             ostree_sign_get_name (sign));
        }
    }

  /* Return true if able to load from any source */
  if (!(loaded_from_file || loaded_inlined))
    return glnx_throw (error, "No keys found");

  return TRUE;
}

static gboolean
string_is_gkeyfile_truthy (const char *value,
                           gboolean   *out_truth)
{
  /* See https://gitlab.gnome.org/GNOME/glib/-/blob/20fb5bf868added5aec53c013ae85ec78ba2eedc/glib/gkeyfile.c#L4528 */
  if (g_str_equal (value, "true") || g_str_equal (value, "1"))
    {
      *out_truth = TRUE;
      return TRUE;
    }
  else if (g_str_equal (value, "false") || g_str_equal (value, "0"))
    {
      *out_truth = FALSE;
      return TRUE;
    }
  return FALSE;
}

static gboolean
verifiers_from_config (OstreeRepo *repo,
                       const char *remote_name,
                       const char *key,
                       GPtrArray **out_verifiers,
                       GError    **error)
{
  g_autoptr(GPtrArray) verifiers = NULL;

  g_autofree char *raw_value = NULL;
  if (!ostree_repo_get_remote_option (repo, remote_name,
                                      key, NULL,
                                      &raw_value, error))
    return FALSE;
  if (raw_value == NULL || g_str_equal (raw_value, ""))
    {
      *out_verifiers = NULL;
      return TRUE;
    }
  gboolean sign_verify_bool = FALSE;
  /* Is the value "truthy" according to GKeyFile's rules?  If so,
   * then we take this to be "accept signatures from any compiled
   * type that happens to have keys configured".
   */
  if (string_is_gkeyfile_truthy (raw_value, &sign_verify_bool))
    {
      if (sign_verify_bool)
        {
          verifiers = ostree_sign_get_all ();
          for (guint i = 0; i < verifiers->len; i++)
            {
              OstreeSign *sign = verifiers->pdata[i];
              /* Try to load public key(s) according remote's configuration;
               * this one is optional.
               */
              if (!_signapi_load_public_keys (sign, repo, remote_name, FALSE, error))
                return FALSE;
            }
        }
    }
  else
    {
      /* If the value isn't "truthy", then it must be an explicit list */
      g_auto(GStrv) sign_types = NULL;
      if (!ostree_repo_get_remote_list_option (repo, remote_name,
                                               key, &sign_types,
                                               error))
        return FALSE;
      verifiers = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
      for (char **iter = sign_types; iter && *iter; iter++)
        {
          const char *sign_type = *iter;
          OstreeSign *verifier = ostree_sign_get_by_name (sign_type, error);
          if (!verifier)
            return FALSE;
          if (!_signapi_load_public_keys (verifier, repo, remote_name, TRUE, error))
            return FALSE;
          g_ptr_array_add (verifiers, verifier);
        }
      g_assert_cmpuint (verifiers->len, >=, 1);
    }

  *out_verifiers = g_steal_pointer (&verifiers);
  return TRUE;
}

/* Create a new array of OstreeSign objects and load the public
 * keys as described by the remote configuration.  If the
 * remote does not have signing verification enabled, then
 * the resulting verifier list will be NULL.
 */
gboolean
_signapi_init_for_remote (OstreeRepo *repo,
                          const char *remote_name,
                          GPtrArray **out_commit_verifiers,
                          GPtrArray **out_summary_verifiers,
                          GError    **error)
{
  g_autoptr(GPtrArray) commit_verifiers = NULL;
  g_autoptr(GPtrArray) summary_verifiers = NULL;

  if (!verifiers_from_config (repo, remote_name, "sign-verify", &commit_verifiers, error))
    return FALSE;
  if (!verifiers_from_config (repo, remote_name, "sign-verify-summary", &summary_verifiers, error))
    return FALSE;

  ot_transfer_out_value (out_commit_verifiers, &commit_verifiers);
  ot_transfer_out_value (out_summary_verifiers, &summary_verifiers);
  return TRUE;
}

/* Iterate over the configured verifiers, and require the commit is signed
 * by at least one.
 */
gboolean
_sign_verify_for_remote (GPtrArray *verifiers,
                         GBytes *signed_data,
                         GVariant *metadata,
                         GError **error)
{
  guint n_invalid_signatures = 0;
  g_autoptr (GError) last_sig_error = NULL;
  gboolean found_sig = FALSE;

  g_assert_cmpuint (verifiers->len, >=, 1);
  for (guint i = 0; i < verifiers->len; i++)
    {
      OstreeSign *sign = verifiers->pdata[i];
      const gchar *signature_key = ostree_sign_metadata_key (sign);
      GVariantType *signature_format = (GVariantType *) ostree_sign_metadata_format (sign);
      g_autoptr (GVariant) signatures =
        g_variant_lookup_value (metadata, signature_key, signature_format);

      /* If not found signatures for requested signature subsystem */
      if (!signatures)
        continue;

      found_sig = TRUE;

        /* Return true if any signature fit to pre-loaded public keys.
          * If no keys configured -- then system configuration will be used */
      if (!ostree_sign_data_verify (sign,
                                    signed_data,
                                    signatures,
                                    last_sig_error ? NULL : &last_sig_error))
        {
          n_invalid_signatures++;
          continue;
        }
      /* Accept the first valid signature */
      return TRUE;
    }

  if (!found_sig)
    return glnx_throw (error, "No signatures found");

  g_assert (last_sig_error);
  g_propagate_error (error, g_steal_pointer (&last_sig_error));
  if (n_invalid_signatures > 1)
    glnx_prefix_error (error, "(%d other invalid signatures)", n_invalid_signatures-1);
  return FALSE;
}


#ifndef OSTREE_DISABLE_GPGME
gboolean
_process_gpg_verify_result (OtPullData            *pull_data,
                            const char            *checksum,
                            OstreeGpgVerifyResult *result,
                            GError               **error)
{
  const char *error_prefix = glnx_strjoina ("Commit ", checksum);
  GLNX_AUTO_PREFIX_ERROR(error_prefix, error);
  if (result == NULL)
    return FALSE;

  /* Allow callers to output the results immediately. */
  g_signal_emit_by_name (pull_data->repo,
                         "gpg-verify-result",
                         checksum, result);

  if (!ostree_gpg_verify_result_require_valid_signature (result, error))
    return FALSE;


  /* We now check both *before* writing the commit, and after. Because the
   * behavior used to be only verifiying after writing, we need to handle
   * the case of "written but not verified". But we also don't want to check
   * twice, as that'd result in duplicate signals.
   */
  g_hash_table_add (pull_data->verified_commits, g_strdup (checksum));

  return TRUE;
}
#endif /* OSTREE_DISABLE_GPGME */

gboolean
_verify_unwritten_commit (OtPullData                 *pull_data,
                          const char                 *checksum,
                          GVariant                   *commit,
                          GVariant                   *detached_metadata,
                          const OstreeCollectionRef  *ref,
                          GCancellable               *cancellable,
                          GError                    **error)
{

  if (pull_data->gpg_verify || pull_data->signapi_commit_verifiers)
    /* Shouldn't happen, but see comment in process_gpg_verify_result() */
    if (g_hash_table_contains (pull_data->verified_commits, checksum))
      return TRUE;

  g_autoptr(GBytes) signed_data = g_variant_get_data_as_bytes (commit);

#ifndef OSTREE_DISABLE_GPGME
  if (pull_data->gpg_verify)
    {
      const char *keyring_remote = NULL;

      if (ref != NULL)
        keyring_remote = g_hash_table_lookup (pull_data->ref_keyring_map, ref);
      if (keyring_remote == NULL)
        keyring_remote = pull_data->remote_name;

      g_autoptr(OstreeGpgVerifyResult) result =
        _ostree_repo_gpg_verify_with_metadata (pull_data->repo, signed_data,
                                               detached_metadata,
                                               keyring_remote,
                                               NULL, NULL, cancellable, error);
      if (!_process_gpg_verify_result (pull_data, checksum, result, error))
        return FALSE;
    }
#endif /* OSTREE_DISABLE_GPGME */

  if (pull_data->signapi_commit_verifiers)
    {
      /* Nothing to check if detached metadata is absent */
      if (detached_metadata == NULL)
        return glnx_throw (error, "Can't verify commit without detached metadata");

      if (!_sign_verify_for_remote (pull_data->signapi_commit_verifiers, signed_data, detached_metadata, error))
        return glnx_prefix_error (error, "Can't verify commit");

      /* Mark the commit as verified to avoid double verification
       * see process_verify_result () for rationale */
      g_hash_table_add (pull_data->verified_commits, g_strdup (checksum));
    }

  return TRUE;
}