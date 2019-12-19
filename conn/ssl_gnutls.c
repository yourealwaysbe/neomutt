/**
 * @file
 * Handling of GnuTLS encryption
 *
 * @authors
 * Copyright (C) 2001 Marco d'Itri <md@linux.it>
 * Copyright (C) 2001-2004 Andrew McDonald <andrew@mcdonald.org.uk>
 *
 * @copyright
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @page conn_ssl_gnutls Handling of GnuTLS encryption
 *
 * Handling of GnuTLS encryption
 */

#include "config.h"
#include <gnutls/gnutls.h>
#include <gnutls/x509.h>
#include <regex.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "mutt/mutt.h"
#include "config/lib.h"
#include "conn_globals.h"
#include "connaccount.h"
#include "connection.h"
#include "globals.h"
#include "keymap.h"
#include "mutt_account.h"
#include "mutt_menu.h"
#include "mutt_window.h"
#include "muttlib.h"
#include "opcodes.h"
#include "options.h"
#include "protos.h"
#include "socket.h"
#include "ssl.h" // IWYU pragma: keep

/* certificate error bitmap values */
#define CERTERR_VALID 0
#define CERTERR_EXPIRED 1
#define CERTERR_NOTYETVALID 2
#define CERTERR_REVOKED 4
#define CERTERR_NOTTRUSTED 8
#define CERTERR_HOSTNAME 16
#define CERTERR_SIGNERNOTCA 32
#define CERTERR_INSECUREALG 64
#define CERTERR_OTHER 128

const int dialog_row_len = 128;

#define CERT_SEP "-----BEGIN"

static int tls_socket_close(struct Connection *conn);

#ifndef HAVE_GNUTLS_PRIORITY_SET_DIRECT
/* This array needs to be large enough to hold all the possible values support
 * by NeoMutt.  The initialized values are just placeholders--the array gets
 * overwrriten in tls_negotiate() depending on the $ssl_use_* options.
 *
 * Note: gnutls_protocol_set_priority() was removed in GnuTLS version
 * 3.4 (2015-04).  TLS 1.3 support wasn't added until version 3.6.5.
 * Therefore, no attempt is made to support $ssl_use_tlsv1_3 in this code.
 */
static int protocol_priority[] = { GNUTLS_TLS1_2, GNUTLS_TLS1_1, GNUTLS_TLS1,
                                   GNUTLS_SSL3, 0 };
#endif

/**
 * struct TlsSockData - TLS socket data
 */
struct TlsSockData
{
  gnutls_session_t state;
  gnutls_certificate_credentials_t xcred;
};

/**
 * tls_init - Set up Gnu TLS
 * @retval  0 Success
 * @retval -1 Error
 */
static int tls_init(void)
{
  static bool init_complete = false;
  int err;

  if (init_complete)
    return 0;

  err = gnutls_global_init();
  if (err < 0)
  {
    mutt_error("gnutls_global_init: %s", gnutls_strerror(err));
    return -1;
  }

  init_complete = true;
  return 0;
}

/**
 * tls_starttls_close - Close a TLS connection - Implements Connection::conn_close()
 */
static int tls_starttls_close(struct Connection *conn)
{
  int rc;

  rc = tls_socket_close(conn);
  conn->conn_read = raw_socket_read;
  conn->conn_write = raw_socket_write;
  conn->conn_close = raw_socket_close;
  conn->conn_poll = raw_socket_poll;

  return rc;
}

/**
 * tls_verify_peers - wrapper for gnutls_certificate_verify_peers
 * @param tlsstate TLS state
 * @retval  0 Success
 * @retval >0 Error, e.g. GNUTLS_CERT_INVALID
 *
 * wrapper with sanity-checking
 */
static gnutls_certificate_status_t tls_verify_peers(gnutls_session_t tlsstate)
{
  unsigned int status = 0;
  int verify_ret = gnutls_certificate_verify_peers2(tlsstate, &status);
  if (verify_ret == 0)
    return status;

  if (status == GNUTLS_E_NO_CERTIFICATE_FOUND)
  {
    mutt_error(_("Unable to get certificate from peer"));
    return 0;
  }
  if (verify_ret < 0)
  {
    mutt_error(_("Certificate verification error (%s)"), gnutls_strerror(status));
    return 0;
  }

  /* We only support X.509 certificates (not OpenPGP) at the moment */
  if (gnutls_certificate_type_get(tlsstate) != GNUTLS_CRT_X509)
  {
    mutt_error(_("Certificate is not X.509"));
    return 0;
  }

  return status;
}

/**
 * tls_fingerprint - Create a fingerprint of a TLS Certificate
 * @param algo   Fingerprint algorithm, e.g. GNUTLS_MAC_SHA256
 * @param buf    Buffer for the fingerprint
 * @param buflen Length of the buffer
 * @param data Certificate
 */
static void tls_fingerprint(gnutls_digest_algorithm_t algo, char *buf,
                            size_t buflen, const gnutls_datum_t *data)
{
  unsigned char md[64];
  size_t n;

  n = 64;

  if (gnutls_fingerprint(algo, data, (char *) md, &n) < 0)
  {
    snprintf(buf, buflen, _("[unable to calculate]"));
  }
  else
  {
    for (int i = 0; i < (int) n; i++)
    {
      char ch[8];
      snprintf(ch, 8, "%02X%s", md[i], ((i % 2) ? " " : ""));
      mutt_str_strcat(buf, buflen, ch);
    }
    buf[2 * n + n / 2 - 1] = '\0'; /* don't want trailing space */
  }
}

/**
 * tls_check_stored_hostname - Does the hostname match a stored certificate?
 * @param cert     Certificate
 * @param hostname Hostname
 * @retval 1 Hostname match found
 * @retval 0 Error, or no match
 */
static int tls_check_stored_hostname(const gnutls_datum_t *cert, const char *hostname)
{
  char *linestr = NULL;
  size_t linestrsize = 0;
  int linenum = 0;
  regex_t preg;
  regmatch_t pmatch[3];

  /* try checking against names stored in stored certs file */
  FILE *fp = fopen(C_CertificateFile, "r");
  if (fp)
  {
    if (REG_COMP(&preg, "^#H ([a-zA-Z0-9_\\.-]+) ([0-9A-F]{4}( [0-9A-F]{4}){7})[ \t]*$",
                 REG_ICASE) != 0)
    {
      mutt_file_fclose(&fp);
      return 0;
    }

    char buf[80];
    buf[0] = '\0';
    tls_fingerprint(GNUTLS_DIG_MD5, buf, sizeof(buf), cert);
    while ((linestr = mutt_file_read_line(linestr, &linestrsize, fp, &linenum, 0)))
    {
      if ((linestr[0] == '#') && (linestr[1] == 'H'))
      {
        if (regexec(&preg, linestr, 3, pmatch, 0) == 0)
        {
          linestr[pmatch[1].rm_eo] = '\0';
          linestr[pmatch[2].rm_eo] = '\0';
          if ((strcmp(linestr + pmatch[1].rm_so, hostname) == 0) &&
              (strcmp(linestr + pmatch[2].rm_so, buf) == 0))
          {
            regfree(&preg);
            FREE(&linestr);
            mutt_file_fclose(&fp);
            return 1;
          }
        }
      }
    }

    regfree(&preg);
    mutt_file_fclose(&fp);
  }

  /* not found a matching name */
  return 0;
}

/**
 * tls_compare_certificates - Compare certificates against #C_CertificateFile
 * @param peercert Certificate
 * @retval 1 Certificate matches file
 * @retval 0 Error, or no match
 */
static int tls_compare_certificates(const gnutls_datum_t *peercert)
{
  gnutls_datum_t cert;
  unsigned char *ptr = NULL;
  gnutls_datum_t b64_data;
  unsigned char *b64_data_data = NULL;
  struct stat filestat;

  if (stat(C_CertificateFile, &filestat) == -1)
    return 0;

  b64_data.size = filestat.st_size;
  b64_data_data = mutt_mem_calloc(1, b64_data.size + 1);
  b64_data.data = b64_data_data;

  FILE *fp = fopen(C_CertificateFile, "r");
  if (!fp)
    return 0;

  b64_data.size = fread(b64_data.data, 1, b64_data.size, fp);
  mutt_file_fclose(&fp);

  do
  {
    const int ret = gnutls_pem_base64_decode_alloc(NULL, &b64_data, &cert);
    if (ret != 0)
    {
      FREE(&b64_data_data);
      return 0;
    }

    /* find start of cert, skipping junk */
    ptr = (unsigned char *) strstr((char *) b64_data.data, CERT_SEP);
    if (!ptr)
    {
      gnutls_free(cert.data);
      FREE(&b64_data_data);
      return 0;
    }
    /* find start of next cert */
    ptr = (unsigned char *) strstr((char *) ptr + 1, CERT_SEP);

    b64_data.size = b64_data.size - (ptr - b64_data.data);
    b64_data.data = ptr;

    if (cert.size == peercert->size)
    {
      if (memcmp(cert.data, peercert->data, cert.size) == 0)
      {
        /* match found */
        gnutls_free(cert.data);
        FREE(&b64_data_data);
        return 1;
      }
    }

    gnutls_free(cert.data);
  } while (ptr);

  /* no match found */
  FREE(&b64_data_data);
  return 0;
}

/**
 * tls_check_preauth - Prepare a certificate for authentication
 * @param[in]  certdata  List of GnuTLS certificates
 * @param[in]  certstat  GnuTLS certificate status
 * @param[in]  hostname  Hostname
 * @param[in]  chainidx  Index in the certificate chain
 * @param[out] certerr   Result, e.g. #CERTERR_VALID
 * @param[out] savedcert 1 if certificate has been saved
 * @retval  0 Success
 * @retval -1 Error
 */
static int tls_check_preauth(const gnutls_datum_t *certdata,
                             gnutls_certificate_status_t certstat, const char *hostname,
                             int chainidx, int *certerr, int *savedcert)
{
  gnutls_x509_crt_t cert;

  *certerr = CERTERR_VALID;
  *savedcert = 0;

  if (gnutls_x509_crt_init(&cert) < 0)
  {
    mutt_error(_("Error initialising gnutls certificate data"));
    return -1;
  }

  if (gnutls_x509_crt_import(cert, certdata, GNUTLS_X509_FMT_DER) < 0)
  {
    mutt_error(_("Error processing certificate data"));
    gnutls_x509_crt_deinit(cert);
    return -1;
  }

  /* Note: tls_negotiate() contains a call to
   * gnutls_certificate_set_verify_flags() with a flag disabling
   * GnuTLS checking of the dates.  So certstat shouldn't have the
   * GNUTLS_CERT_EXPIRED and GNUTLS_CERT_NOT_ACTIVATED bits set. */
  if (C_SslVerifyDates != MUTT_NO)
  {
    if (gnutls_x509_crt_get_expiration_time(cert) < mutt_date_epoch())
      *certerr |= CERTERR_EXPIRED;
    if (gnutls_x509_crt_get_activation_time(cert) > mutt_date_epoch())
      *certerr |= CERTERR_NOTYETVALID;
  }

  if ((chainidx == 0) && (C_SslVerifyHost != MUTT_NO) &&
      !gnutls_x509_crt_check_hostname(cert, hostname) &&
      !tls_check_stored_hostname(certdata, hostname))
  {
    *certerr |= CERTERR_HOSTNAME;
  }

  if (certstat & GNUTLS_CERT_REVOKED)
  {
    *certerr |= CERTERR_REVOKED;
    certstat ^= GNUTLS_CERT_REVOKED;
  }

  /* see whether certificate is in our cache (certificates file) */
  if (tls_compare_certificates(certdata))
  {
    *savedcert = 1;

    /* We check above for certs with bad dates or that are revoked.
     * These must be accepted manually each time.  Otherwise, we
     * accept saved certificates as valid. */
    if (*certerr == CERTERR_VALID)
    {
      gnutls_x509_crt_deinit(cert);
      return 0;
    }
  }

  if (certstat & GNUTLS_CERT_INVALID)
  {
    *certerr |= CERTERR_NOTTRUSTED;
    certstat ^= GNUTLS_CERT_INVALID;
  }

  if (certstat & GNUTLS_CERT_SIGNER_NOT_FOUND)
  {
    /* NB: already cleared if cert in cache */
    *certerr |= CERTERR_NOTTRUSTED;
    certstat ^= GNUTLS_CERT_SIGNER_NOT_FOUND;
  }

  if (certstat & GNUTLS_CERT_SIGNER_NOT_CA)
  {
    /* NB: already cleared if cert in cache */
    *certerr |= CERTERR_SIGNERNOTCA;
    certstat ^= GNUTLS_CERT_SIGNER_NOT_CA;
  }

  if (certstat & GNUTLS_CERT_INSECURE_ALGORITHM)
  {
    /* NB: already cleared if cert in cache */
    *certerr |= CERTERR_INSECUREALG;
    certstat ^= GNUTLS_CERT_INSECURE_ALGORITHM;
  }

  /* we've been zeroing the interesting bits in certstat -
   * don't return OK if there are any unhandled bits we don't
   * understand */
  if (certstat != 0)
    *certerr |= CERTERR_OTHER;

  gnutls_x509_crt_deinit(cert);

  if (*certerr == CERTERR_VALID)
    return 0;

  return -1;
}

/**
 * tls_check_one_certificate - Check a GnuTLS certificate
 * @param certdata List of GnuTLS certificates
 * @param certstat GnuTLS certificate status
 * @param hostname Hostname
 * @param idx      Index into certificate list
 * @param len      Length of certificate list
 * @retval 0  Failure
 * @retval >0 Success
 */
static int tls_check_one_certificate(const gnutls_datum_t *certdata,
                                     gnutls_certificate_status_t certstat,
                                     const char *hostname, int idx, size_t len)
{
  int certerr, savedcert;
  gnutls_x509_crt_t cert;
  char buf[128];
  char fpbuf[128];
  size_t buflen;
  char dn_common_name[128];
  char dn_email[128];
  char dn_organization[128];
  char dn_organizational_unit[128];
  char dn_locality[128];
  char dn_province[128];
  char dn_country[128];
  time_t t;
  char datestr[30];
  struct Menu *menu = NULL;
  char helpstr[1024];
  char title[256];
  FILE *fp = NULL;
  gnutls_datum_t pemdata;
  int done, ret;
  bool reset_ignoremacro = false;

  if (tls_check_preauth(certdata, certstat, hostname, idx, &certerr, &savedcert) == 0)
    return 1;

  /* interactive check from user */
  if (gnutls_x509_crt_init(&cert) < 0)
  {
    mutt_error(_("Error initialising gnutls certificate data"));
    return 0;
  }

  if (gnutls_x509_crt_import(cert, certdata, GNUTLS_X509_FMT_DER) < 0)
  {
    mutt_error(_("Error processing certificate data"));
    gnutls_x509_crt_deinit(cert);
    return 0;
  }

  struct Buffer *drow = mutt_buffer_pool_get();

  struct MuttWindow *dlg =
      mutt_window_new(MUTT_WIN_ORIENT_VERTICAL, MUTT_WIN_SIZE_MAXIMISE,
                      MUTT_WIN_SIZE_UNLIMITED, MUTT_WIN_SIZE_UNLIMITED);
  dlg->type = WT_DIALOG;
  struct MuttWindow *index =
      mutt_window_new(MUTT_WIN_ORIENT_VERTICAL, MUTT_WIN_SIZE_MAXIMISE,
                      MUTT_WIN_SIZE_UNLIMITED, MUTT_WIN_SIZE_UNLIMITED);
  index->type = WT_INDEX;
  struct MuttWindow *ibar = mutt_window_new(
      MUTT_WIN_ORIENT_VERTICAL, MUTT_WIN_SIZE_FIXED, 1, MUTT_WIN_SIZE_UNLIMITED);
  ibar->type = WT_INDEX_BAR;

  if (C_StatusOnTop)
  {
    mutt_window_add_child(dlg, ibar);
    mutt_window_add_child(dlg, index);
  }
  else
  {
    mutt_window_add_child(dlg, index);
    mutt_window_add_child(dlg, ibar);
  }

  dialog_push(dlg);

  menu = mutt_menu_new(MENU_GENERIC);
  menu->pagelen = index->state.rows;
  menu->win_index = index;
  menu->win_ibar = ibar;

  mutt_menu_push_current(menu);

  buflen = sizeof(dn_common_name);
  if (gnutls_x509_crt_get_dn_by_oid(cert, GNUTLS_OID_X520_COMMON_NAME, 0, 0,
                                    dn_common_name, &buflen) != 0)
  {
    dn_common_name[0] = '\0';
  }
  buflen = sizeof(dn_email);
  if (gnutls_x509_crt_get_dn_by_oid(cert, GNUTLS_OID_PKCS9_EMAIL, 0, 0, dn_email, &buflen) != 0)
    dn_email[0] = '\0';
  buflen = sizeof(dn_organization);
  if (gnutls_x509_crt_get_dn_by_oid(cert, GNUTLS_OID_X520_ORGANIZATION_NAME, 0,
                                    0, dn_organization, &buflen) != 0)
  {
    dn_organization[0] = '\0';
  }
  buflen = sizeof(dn_organizational_unit);
  if (gnutls_x509_crt_get_dn_by_oid(cert, GNUTLS_OID_X520_ORGANIZATIONAL_UNIT_NAME,
                                    0, 0, dn_organizational_unit, &buflen) != 0)
  {
    dn_organizational_unit[0] = '\0';
  }
  buflen = sizeof(dn_locality);
  if (gnutls_x509_crt_get_dn_by_oid(cert, GNUTLS_OID_X520_LOCALITY_NAME, 0, 0,
                                    dn_locality, &buflen) != 0)
  {
    dn_locality[0] = '\0';
  }
  buflen = sizeof(dn_province);
  if (gnutls_x509_crt_get_dn_by_oid(cert, GNUTLS_OID_X520_STATE_OR_PROVINCE_NAME,
                                    0, 0, dn_province, &buflen) != 0)
  {
    dn_province[0] = '\0';
  }
  buflen = sizeof(dn_country);
  if (gnutls_x509_crt_get_dn_by_oid(cert, GNUTLS_OID_X520_COUNTRY_NAME, 0, 0,
                                    dn_country, &buflen) != 0)
  {
    dn_country[0] = '\0';
  }

  mutt_menu_add_dialog_row(menu, _("This certificate belongs to:"));
  mutt_buffer_printf(drow, "   %s  %s", dn_common_name, dn_email);
  mutt_menu_add_dialog_row(menu, mutt_b2s(drow));
  mutt_buffer_printf(drow, "   %s", dn_organization);
  mutt_menu_add_dialog_row(menu, mutt_b2s(drow));
  mutt_buffer_printf(drow, "   %s", dn_organizational_unit);
  mutt_menu_add_dialog_row(menu, mutt_b2s(drow));
  mutt_buffer_printf(drow, "   %s  %s  %s", dn_locality, dn_province, dn_country);
  mutt_menu_add_dialog_row(menu, mutt_b2s(drow));

  buflen = sizeof(dn_common_name);
  if (gnutls_x509_crt_get_issuer_dn_by_oid(cert, GNUTLS_OID_X520_COMMON_NAME, 0,
                                           0, dn_common_name, &buflen) != 0)
  {
    dn_common_name[0] = '\0';
  }
  buflen = sizeof(dn_email);
  if (gnutls_x509_crt_get_issuer_dn_by_oid(cert, GNUTLS_OID_PKCS9_EMAIL, 0, 0,
                                           dn_email, &buflen) != 0)
  {
    dn_email[0] = '\0';
  }
  buflen = sizeof(dn_organization);
  if (gnutls_x509_crt_get_issuer_dn_by_oid(cert, GNUTLS_OID_X520_ORGANIZATION_NAME,
                                           0, 0, dn_organization, &buflen) != 0)
  {
    dn_organization[0] = '\0';
  }
  buflen = sizeof(dn_organizational_unit);
  if (gnutls_x509_crt_get_issuer_dn_by_oid(cert, GNUTLS_OID_X520_ORGANIZATIONAL_UNIT_NAME,
                                           0, 0, dn_organizational_unit, &buflen) != 0)
  {
    dn_organizational_unit[0] = '\0';
  }
  buflen = sizeof(dn_locality);
  if (gnutls_x509_crt_get_issuer_dn_by_oid(cert, GNUTLS_OID_X520_LOCALITY_NAME,
                                           0, 0, dn_locality, &buflen) != 0)
  {
    dn_locality[0] = '\0';
  }
  buflen = sizeof(dn_province);
  if (gnutls_x509_crt_get_issuer_dn_by_oid(cert, GNUTLS_OID_X520_STATE_OR_PROVINCE_NAME,
                                           0, 0, dn_province, &buflen) != 0)
  {
    dn_province[0] = '\0';
  }
  buflen = sizeof(dn_country);
  if (gnutls_x509_crt_get_issuer_dn_by_oid(cert, GNUTLS_OID_X520_COUNTRY_NAME,
                                           0, 0, dn_country, &buflen) != 0)
  {
    dn_country[0] = '\0';
  }

  mutt_menu_add_dialog_row(menu, "");
  mutt_menu_add_dialog_row(menu, _("This certificate was issued by:"));
  mutt_buffer_printf(drow, "   %s  %s", dn_common_name, dn_email);
  mutt_menu_add_dialog_row(menu, mutt_b2s(drow));
  mutt_buffer_printf(drow, "   %s", dn_organization);
  mutt_menu_add_dialog_row(menu, mutt_b2s(drow));
  mutt_buffer_printf(drow, "   %s", dn_organizational_unit);
  mutt_menu_add_dialog_row(menu, mutt_b2s(drow));
  mutt_buffer_printf(drow, "   %s  %s  %s", dn_locality, dn_province, dn_country);
  mutt_menu_add_dialog_row(menu, mutt_b2s(drow));

  mutt_menu_add_dialog_row(menu, "");
  mutt_menu_add_dialog_row(menu, _("This certificate is valid"));

  t = gnutls_x509_crt_get_activation_time(cert);
  mutt_date_make_tls(datestr, sizeof(datestr), t);
  mutt_buffer_printf(drow, _("   from %s"), datestr);

  t = gnutls_x509_crt_get_expiration_time(cert);
  mutt_date_make_tls(datestr, sizeof(datestr), t);
  mutt_buffer_printf(drow, _("     to %s"), datestr);
  mutt_menu_add_dialog_row(menu, mutt_b2s(drow));

  fpbuf[0] = '\0';
  tls_fingerprint(GNUTLS_DIG_SHA, fpbuf, sizeof(fpbuf), certdata);
  mutt_buffer_printf(drow, _("SHA1 Fingerprint: %s"), fpbuf);
  mutt_menu_add_dialog_row(menu, mutt_b2s(drow));
  fpbuf[0] = '\0';
  fpbuf[40] = '\0'; /* Ensure the second printed line is null terminated */
  tls_fingerprint(GNUTLS_DIG_SHA256, fpbuf, sizeof(fpbuf), certdata);
  fpbuf[39] = '\0'; /* Divide into two lines of output */
  mutt_buffer_printf(drow, "%s%s", _("SHA256 Fingerprint: "), fpbuf);
  mutt_menu_add_dialog_row(menu, mutt_b2s(drow));
  mutt_buffer_printf(drow, "%*s%s", (int) mutt_str_strlen(_("SHA256 Fingerprint: ")),
                     "", fpbuf + 40);
  mutt_menu_add_dialog_row(menu, mutt_b2s(drow));

  if (certerr)
    mutt_menu_add_dialog_row(menu, "");

  if (certerr & CERTERR_NOTYETVALID)
  {
    mutt_menu_add_dialog_row(menu,
                             _("WARNING: Server certificate is not yet valid"));
  }
  if (certerr & CERTERR_EXPIRED)
  {
    mutt_menu_add_dialog_row(menu,
                             _("WARNING: Server certificate has expired"));
  }
  if (certerr & CERTERR_REVOKED)
  {
    mutt_menu_add_dialog_row(menu,
                             _("WARNING: Server certificate has been revoked"));
  }
  if (certerr & CERTERR_HOSTNAME)
  {
    mutt_menu_add_dialog_row(
        menu, _("WARNING: Server hostname does not match certificate"));
  }
  if (certerr & CERTERR_SIGNERNOTCA)
  {
    mutt_menu_add_dialog_row(
        menu, _("WARNING: Signer of server certificate is not a CA"));
  }
  if (certerr & CERTERR_INSECUREALG)
  {
    mutt_menu_add_dialog_row(menu, _("Warning: Server certificate was signed "
                                     "using an insecure algorithm"));
  }

  snprintf(title, sizeof(title),
           _("SSL Certificate check (certificate %zu of %zu in chain)"), len - idx, len);
  menu->title = title;
  /* certificates with bad dates, or that are revoked, must be
   * accepted manually each and every time */
  if (C_CertificateFile && !savedcert &&
      !(certerr & (CERTERR_EXPIRED | CERTERR_NOTYETVALID | CERTERR_REVOKED)))
  {
    menu->prompt = _("(r)eject, accept (o)nce, (a)ccept always");
    /* L10N: These three letters correspond to the choices in the string:
       (r)eject, accept (o)nce, (a)ccept always.
       This is an interactive certificate confirmation prompt for
       a GNUTLS connection. */
    menu->keys = _("roa");
  }
  else
  {
    menu->prompt = _("(r)eject, accept (o)nce");
    /* L10N: These two letters correspond to the choices in the string:
       (r)eject, accept (o)nce.
       These is an interactive certificate confirmation prompt for
       a GNUTLS connection. */
    menu->keys = _("ro");
  }

  helpstr[0] = '\0';
  mutt_make_help(buf, sizeof(buf), _("Exit  "), MENU_GENERIC, OP_EXIT);
  mutt_str_strcat(helpstr, sizeof(helpstr), buf);
  mutt_make_help(buf, sizeof(buf), _("Help"), MENU_GENERIC, OP_HELP);
  mutt_str_strcat(helpstr, sizeof(helpstr), buf);
  menu->help = helpstr;

  if (!OptIgnoreMacroEvents)
  {
    OptIgnoreMacroEvents = true;
    reset_ignoremacro = true;
  }

  done = 0;
  while (done == 0)
  {
    switch (mutt_menu_loop(menu))
    {
      case -1:         /* abort */
      case OP_MAX + 1: /* reject */
      case OP_EXIT:
        done = 1;
        break;
      case OP_MAX + 3: /* accept always */
        done = 0;
        fp = mutt_file_fopen(C_CertificateFile, "a");
        if (fp)
        {
          /* save hostname if necessary */
          if (certerr & CERTERR_HOSTNAME)
          {
            fpbuf[0] = '\0';
            tls_fingerprint(GNUTLS_DIG_MD5, fpbuf, sizeof(fpbuf), certdata);
            fprintf(fp, "#H %s %s\n", hostname, fpbuf);
            done = 1;
          }
          /* Save the cert for all other errors */
          if (certerr ^ CERTERR_HOSTNAME)
          {
            done = 0;
            ret = gnutls_pem_base64_encode_alloc("CERTIFICATE", certdata, &pemdata);
            if (ret == 0)
            {
              if (fwrite(pemdata.data, pemdata.size, 1, fp) == 1)
              {
                done = 1;
              }
              gnutls_free(pemdata.data);
            }
          }
          mutt_file_fclose(&fp);
        }
        if (done == 0)
        {
          mutt_error(_("Warning: Couldn't save certificate"));
        }
        else
        {
          mutt_message(_("Certificate saved"));
          mutt_sleep(0);
        }
      /* fallthrough */
      case OP_MAX + 2: /* accept once */
        done = 2;
        break;
    }
  }
  if (reset_ignoremacro)
    OptIgnoreMacroEvents = false;

  mutt_buffer_pool_release(&drow);
  mutt_menu_pop_current(menu);
  mutt_menu_free(&menu);
  dialog_pop();
  mutt_window_free(&dlg);
  gnutls_x509_crt_deinit(cert);

  return done == 2;
}

/**
 * tls_check_certificate - Check a connection's certificate
 * @param conn Connection to a server
 * @retval >0 Certificate is valid
 * @retval 0  Error, or certificate is invalid
 */
static int tls_check_certificate(struct Connection *conn)
{
  struct TlsSockData *data = conn->sockdata;
  gnutls_session_t state = data->state;
  const gnutls_datum_t *cert_list = NULL;
  unsigned int cert_list_size = 0;
  gnutls_certificate_status_t certstat;
  int certerr, savedcert, rc = 0;
  int rcpeer = -1; /* the result of tls_check_preauth() on the peer's EE cert */

  if (gnutls_auth_get_type(state) != GNUTLS_CRD_CERTIFICATE)
  {
    mutt_error(_("Unable to get certificate from peer"));
    return 0;
  }

  certstat = tls_verify_peers(state);

  cert_list = gnutls_certificate_get_peers(state, &cert_list_size);
  if (!cert_list)
  {
    mutt_error(_("Unable to get certificate from peer"));
    return 0;
  }

  /* tls_verify_peers doesn't check hostname or expiration, so walk
   * from most specific to least checking these. If we see a saved certificate,
   * its status short-circuits the remaining checks. */
  int preauthrc = 0;
  for (int i = 0; i < cert_list_size; i++)
  {
    rc = tls_check_preauth(&cert_list[i], certstat, conn->account.host, i,
                           &certerr, &savedcert);
    preauthrc += rc;
    if (i == 0)
    {
      /* This is the peer's end-entity X.509 certificate.  Stash the result
       * to check later in this function.  */
      rcpeer = rc;
    }

    if (savedcert)
    {
      if (preauthrc == 0)
        return 1;
      break;
    }
  }

  /* then check interactively, starting from chain root */
  for (int i = cert_list_size - 1; i >= 0; i--)
  {
    rc = tls_check_one_certificate(&cert_list[i], certstat, conn->account.host,
                                   i, cert_list_size);

    /* add signers to trust set, then reverify */
    if (i && rc)
    {
      rc = gnutls_certificate_set_x509_trust_mem(data->xcred, &cert_list[i], GNUTLS_X509_FMT_DER);
      if (rc != 1)
        mutt_debug(LL_DEBUG1, "error trusting certificate %d: %d\n", i, rc);

      certstat = tls_verify_peers(state);
      /* If the cert chain now verifies, and the peer's cert was otherwise
       * valid (rcpeer==0), we are done.  */
      if (!certstat && !rcpeer)
        return 1;
    }
  }

  return rc;
}

/**
 * tls_get_client_cert - Get the client certificate for a TLS connection
 * @param conn Connection to a server
 */
static void tls_get_client_cert(struct Connection *conn)
{
  struct TlsSockData *data = conn->sockdata;
  gnutls_x509_crt_t clientcrt;
  char *dn = NULL;
  char *cn = NULL;
  char *cnend = NULL;
  size_t dnlen;

  /* get our cert CN if we have one */
  const gnutls_datum_t *crtdata = gnutls_certificate_get_ours(data->state);
  if (!crtdata)
    return;

  if (gnutls_x509_crt_init(&clientcrt) < 0)
  {
    mutt_debug(LL_DEBUG1, "Failed to init gnutls crt\n");
    return;
  }
  if (gnutls_x509_crt_import(clientcrt, crtdata, GNUTLS_X509_FMT_DER) < 0)
  {
    mutt_debug(LL_DEBUG1, "Failed to import gnutls client crt\n");
    goto err_crt;
  }
  /* get length of DN */
  dnlen = 0;
  gnutls_x509_crt_get_dn(clientcrt, NULL, &dnlen);
  dn = mutt_mem_calloc(1, dnlen);

  gnutls_x509_crt_get_dn(clientcrt, dn, &dnlen);
  mutt_debug(LL_DEBUG2, "client certificate DN: %s\n", dn);

  /* extract CN to use as external user name */
  cn = strstr(dn, "CN=");
  if (!cn)
  {
    mutt_debug(LL_DEBUG1, "no CN found in DN\n");
    goto err_dn;
  }

  cnend = strstr(dn, ",EMAIL=");
  if (cnend)
    *cnend = '\0';

  /* if we are using a client cert, SASL may expect an external auth name */
  if (mutt_account_getuser(&conn->account) < 0)
    mutt_debug(LL_DEBUG1, "Couldn't get user info\n");

err_dn:
  FREE(&dn);
err_crt:
  gnutls_x509_crt_deinit(clientcrt);
}

#ifdef HAVE_GNUTLS_PRIORITY_SET_DIRECT
/**
 * tls_set_priority - Set TLS algorithm priorities
 * @param data TLS socket data
 * @retval  0 Success
 * @retval -1 Error
 */
static int tls_set_priority(struct TlsSockData *data)
{
  size_t nproto = 5;
  int rv = -1;

  struct Buffer *priority = mutt_buffer_pool_get();

  if (C_SslCiphers)
    mutt_buffer_strcpy(priority, C_SslCiphers);
  else
    mutt_buffer_strcpy(priority, "NORMAL");

  if (!C_SslUseTlsv13)
  {
    nproto--;
    mutt_buffer_addstr(priority, ":-VERS-TLS1.3");
  }
  if (!C_SslUseTlsv12)
  {
    nproto--;
    mutt_buffer_addstr(priority, ":-VERS-TLS1.2");
  }
  if (!C_SslUseTlsv11)
  {
    nproto--;
    mutt_buffer_addstr(priority, ":-VERS-TLS1.1");
  }
  if (!C_SslUseTlsv1)
  {
    nproto--;
    mutt_buffer_addstr(priority, ":-VERS-TLS1.0");
  }
  if (!C_SslUseSslv3)
  {
    nproto--;
    mutt_buffer_addstr(priority, ":-VERS-SSL3.0");
  }

  if (nproto == 0)
  {
    mutt_error(_("All available protocols for TLS/SSL connection disabled"));
    goto cleanup;
  }

  int err = gnutls_priority_set_direct(data->state, mutt_b2s(priority), NULL);
  if (err < 0)
  {
    mutt_error("gnutls_priority_set_direct(%s): %s", mutt_b2s(priority),
               gnutls_strerror(err));
    goto cleanup;
  }

  rv = 0;

cleanup:
  mutt_buffer_pool_release(&priority);
  return rv;
}

#else
/**
 * tls_set_priority - Set the priority of various protocols
 * @param data TLS socket data
 * @retval  0 Success
 * @retval -1 Error
 */
static int tls_set_priority(struct TlsSockData *data)
{
  size_t nproto = 0; /* number of tls/ssl protocols */

  if (C_SslUseTlsv12)
    protocol_priority[nproto++] = GNUTLS_TLS1_2;
  if (C_SslUseTlsv11)
    protocol_priority[nproto++] = GNUTLS_TLS1_1;
  if (C_SslUseTlsv1)
    protocol_priority[nproto++] = GNUTLS_TLS1;
  if (C_SslUseSslv3)
    protocol_priority[nproto++] = GNUTLS_SSL3;
  protocol_priority[nproto] = 0;

  if (nproto == 0)
  {
    mutt_error(_("All available protocols for TLS/SSL connection disabled"));
    return -1;
  }

  if (C_SslCiphers)
  {
    mutt_error(
        _("Explicit ciphersuite selection via $ssl_ciphers not supported"));
  }
  if (certerr & CERTERR_INSECUREALG)
  {
    row++;
    strfcpy(menu->dialog[row], _("Warning: Server certificate was signed using an insecure algorithm"),
            dialog_row_len);
  }

  /* We use default priorities (see gnutls documentation),
   * except for protocol version */
  gnutls_set_default_priority(data->state);
  gnutls_protocol_set_priority(data->state, protocol_priority);
  return 0;
}
#endif

/**
 * tls_negotiate - Negotiate TLS connection
 * @param conn Connection to a server
 * @retval  0 Success
 * @retval -1 Error
 *
 * After TLS state has been initialized, attempt to negotiate TLS over the
 * wire, including certificate checks.
 */
static int tls_negotiate(struct Connection *conn)
{
  struct TlsSockData *data = mutt_mem_calloc(1, sizeof(struct TlsSockData));
  conn->sockdata = data;
  int err = gnutls_certificate_allocate_credentials(&data->xcred);
  if (err < 0)
  {
    FREE(&conn->sockdata);
    mutt_error("gnutls_certificate_allocate_credentials: %s", gnutls_strerror(err));
    return -1;
  }

  gnutls_certificate_set_x509_trust_file(data->xcred, C_CertificateFile, GNUTLS_X509_FMT_PEM);
  /* ignore errors, maybe file doesn't exist yet */

  if (C_SslCaCertificatesFile)
  {
    gnutls_certificate_set_x509_trust_file(data->xcred, C_SslCaCertificatesFile,
                                           GNUTLS_X509_FMT_PEM);
  }

  if (C_SslClientCert)
  {
    mutt_debug(LL_DEBUG2, "Using client certificate %s\n", C_SslClientCert);
    gnutls_certificate_set_x509_key_file(data->xcred, C_SslClientCert,
                                         C_SslClientCert, GNUTLS_X509_FMT_PEM);
  }

#ifdef HAVE_DECL_GNUTLS_VERIFY_DISABLE_TIME_CHECKS
  /* disable checking certificate activation/expiration times
   * in gnutls, we do the checks ourselves */
  gnutls_certificate_set_verify_flags(data->xcred, GNUTLS_VERIFY_DISABLE_TIME_CHECKS);
#endif

  err = gnutls_init(&data->state, GNUTLS_CLIENT);
  if (err)
  {
    mutt_error("gnutls_handshake: %s", gnutls_strerror(err));
    goto fail;
  }

  /* set socket */
  gnutls_transport_set_ptr(data->state, (gnutls_transport_ptr_t)(long) conn->fd);

  if (gnutls_server_name_set(data->state, GNUTLS_NAME_DNS, conn->account.host,
                             mutt_str_strlen(conn->account.host)))
  {
    mutt_error(_("Warning: unable to set TLS SNI host name"));
  }

  if (tls_set_priority(data) < 0)
  {
    goto fail;
  }

  if (C_SslMinDhPrimeBits > 0)
  {
    gnutls_dh_set_prime_bits(data->state, C_SslMinDhPrimeBits);
  }

  /* gnutls_set_cred (data->state, GNUTLS_ANON, NULL); */

  gnutls_credentials_set(data->state, GNUTLS_CRD_CERTIFICATE, data->xcred);

  err = gnutls_handshake(data->state);

  while (err == GNUTLS_E_AGAIN)
  {
    err = gnutls_handshake(data->state);
  }
  if (err < 0)
  {
    if (err == GNUTLS_E_FATAL_ALERT_RECEIVED)
    {
      mutt_error("gnutls_handshake: %s(%s)", gnutls_strerror(err),
                 gnutls_alert_get_name(gnutls_alert_get(data->state)));
    }
    else
    {
      mutt_error("gnutls_handshake: %s", gnutls_strerror(err));
    }
    goto fail;
  }

  if (tls_check_certificate(conn) == 0)
    goto fail;

  /* set Security Strength Factor (SSF) for SASL */
  /* NB: gnutls_cipher_get_key_size() returns key length in bytes */
  conn->ssf = gnutls_cipher_get_key_size(gnutls_cipher_get(data->state)) * 8;

  tls_get_client_cert(conn);

  if (!OptNoCurses)
  {
    mutt_message(_("SSL/TLS connection using %s (%s/%s/%s)"),
                 gnutls_protocol_get_name(gnutls_protocol_get_version(data->state)),
                 gnutls_kx_get_name(gnutls_kx_get(data->state)),
                 gnutls_cipher_get_name(gnutls_cipher_get(data->state)),
                 gnutls_mac_get_name(gnutls_mac_get(data->state)));
    mutt_sleep(0);
  }

  return 0;

fail:
  gnutls_certificate_free_credentials(data->xcred);
  gnutls_deinit(data->state);
  FREE(&conn->sockdata);
  return -1;
}

/**
 * tls_socket_poll - Check whether a socket read would block - Implements Connection::conn_poll()
 */
static int tls_socket_poll(struct Connection *conn, time_t wait_secs)
{
  struct TlsSockData *data = conn->sockdata;

  if (gnutls_record_check_pending(data->state))
    return 1;

  return raw_socket_poll(conn, wait_secs);
}

/**
 * tls_socket_open - Open a TLS socket - Implements Connection::conn_open()
 */
static int tls_socket_open(struct Connection *conn)
{
  if (raw_socket_open(conn) < 0)
    return -1;

  if (tls_negotiate(conn) < 0)
  {
    tls_socket_close(conn);
    return -1;
  }

  return 0;
}

/**
 * tls_socket_read - Read data from a TLS socket - Implements Connection::conn_read()
 */
static int tls_socket_read(struct Connection *conn, char *buf, size_t count)
{
  struct TlsSockData *data = conn->sockdata;
  if (!data)
  {
    mutt_error(_("Error: no TLS socket open"));
    return -1;
  }

  int rc;
  do
  {
    rc = gnutls_record_recv(data->state, buf, count);
  } while ((rc == GNUTLS_E_AGAIN) || (rc == GNUTLS_E_INTERRUPTED));

  if (rc < 0)
  {
    mutt_error("tls_socket_read (%s)", gnutls_strerror(rc));
    return -1;
  }

  return rc;
}

/**
 * tls_socket_write - Write data to a TLS socket - Implements Connection::conn_write()
 */
static int tls_socket_write(struct Connection *conn, const char *buf, size_t count)
{
  struct TlsSockData *data = conn->sockdata;
  size_t sent = 0;

  if (!data)
  {
    mutt_error(_("Error: no TLS socket open"));
    return -1;
  }

  do
  {
    int ret;
    do
    {
      ret = gnutls_record_send(data->state, buf + sent, count - sent);
    } while ((ret == GNUTLS_E_AGAIN) || (ret == GNUTLS_E_INTERRUPTED));

    if (ret < 0)
    {
      mutt_error("tls_socket_write (%s)", gnutls_strerror(ret));
      return -1;
    }

    sent += ret;
  } while (sent < count);

  return sent;
}

/**
 * tls_socket_close - Close a TLS socket - Implements Connection::conn_close()
 */
static int tls_socket_close(struct Connection *conn)
{
  struct TlsSockData *data = conn->sockdata;
  if (data)
  {
    /* shut down only the write half to avoid hanging waiting for the remote to respond.
     *
     * RFC5246 7.2.1. "Closure Alerts"
     *
     * It is not required for the initiator of the close to wait for the
     * responding close_notify alert before closing the read side of the
     * connection.  */
    gnutls_bye(data->state, GNUTLS_SHUT_WR);

    gnutls_certificate_free_credentials(data->xcred);
    gnutls_deinit(data->state);
    FREE(&conn->sockdata);
  }

  return raw_socket_close(conn);
}

/**
 * mutt_ssl_socket_setup - Set up SSL socket mulitplexor
 * @param conn Connection to a server
 * @retval  0 Success
 * @retval -1 Error
 */
int mutt_ssl_socket_setup(struct Connection *conn)
{
  if (tls_init() < 0)
    return -1;

  conn->conn_open = tls_socket_open;
  conn->conn_read = tls_socket_read;
  conn->conn_write = tls_socket_write;
  conn->conn_close = tls_socket_close;
  conn->conn_poll = tls_socket_poll;

  return 0;
}

/**
 * mutt_ssl_starttls - Negotiate TLS over an already opened connection
 * @param conn Connection to a server
 * @retval  0 Success
 * @retval -1 Error
 */
int mutt_ssl_starttls(struct Connection *conn)
{
  if (tls_init() < 0)
    return -1;

  if (tls_negotiate(conn) < 0)
    return -1;

  conn->conn_read = tls_socket_read;
  conn->conn_write = tls_socket_write;
  conn->conn_close = tls_starttls_close;
  conn->conn_poll = tls_socket_poll;

  return 0;
}
