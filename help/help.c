/**
 * @file
 * Help system
 *
 * @authors
 * Copyright (C) 2018-2019 Richard Russon <rich@flatcap.org>
 * Copyright (C) 2018 Floyd Anderson <f.a@31c0.net>
 * Copyright (C) 2019 Tran Manh Tu <xxlaguna93@gmail.com>
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

#include "config.h"
#include <stddef.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "mutt/mutt.h"
#include "address/lib.h"
#include "config/lib.h"
#include "email/lib.h"
#include "core/lib.h"
#include "help.h"
#include "context.h"
#include "globals.h"
#include "mutt_header.h"
#include "mutt_options.h"
#include "mutt_thread.h"
#include "muttlib.h"
#include "mx.h"
#include "protos.h"
#include "sendlib.h"

#define HELP_CACHE_DOCLIST                                                     \
  1 ///< whether to cache the DocList between help_mbox_open calls
#define HELP_FHDR_MAXLINES                                                     \
  -1 ///< max help file header lines to read (N < 0 means all)
#define HELP_LINK_CHAPTERS                                                     \
  0 ///< whether to link all help chapter upwards to root box

static bool __Backup_HTS; ///< used to restore $hide_thread_subject on help_mbox_close()
static char DocDirID[33]; ///< MD5 checksum of current $help_doc_dir DT_PATH option
static struct HelpList *DocList; ///< all valid help documents within $help_doc_dir folder

/**
 * help_list_free - Free a list of Help documents
 * @param list      List to free
 * @param item_free Function to free the list contents
 */
static void help_list_free(struct HelpList **list, void (*item_free)(void **))
{
  if (!list || !*list)
    return;

  for (size_t i = 0; i < (*list)->size; i++)
  {
    item_free(&((*list)->data[i]));
  }
  FREE(&(*list)->data);
  FREE(list);
}

/**
 * help_list_shrink - Resize a List of Help documents to save space
 * @param list List to resize
 */
static void help_list_shrink(struct HelpList *list)
{
  if (!list)
    return;

  mutt_mem_realloc(list->data, list->size * list->item_size);
  list->capa = list->size;
}

/**
 * help_list_new - Create a new list of Help documents
 * @param item_size Size of items in list
 * @retval ptr New Help list
 */
static struct HelpList *help_list_new(size_t item_size)
{
  struct HelpList *list = NULL;
  if (item_size == 0)
    return NULL;

  list = mutt_mem_malloc(sizeof(struct HelpList));
  list->item_size = item_size;
  list->size = 0;
  list->capa = HELPLIST_INIT_CAPACITY;
  list->data = mutt_mem_calloc(list->capa, sizeof(void *) * list->item_size);

  return list;
}

/**
 * help_list_append - Add an item to the Help document list
 * @param list List to add to
 * @param item Item to add
 */
static void help_list_append(struct HelpList *list, void *item)
{
  if (!list || !item)
    return;

  if (list->size >= list->capa)
  {
    list->capa = (list->capa == 0) ? HELPLIST_INIT_CAPACITY : (list->capa * 2);
    mutt_mem_realloc(list->data, list->capa * list->item_size);
  }

  list->data[list->size] = mutt_mem_calloc(1, list->item_size);
  list->data[list->size] = item;
  list->size++;
}

/**
 * help_list_new_append - Append a new item to a Help document list
 * @param list      List to append to
 * @param item_size Size of item to add
 * @param item      Item to add to list
 */
static void help_list_new_append(struct HelpList **list, size_t item_size, void *item)
{
  if ((item_size == 0) || !item)
    return;

  if (!list || !*list)
    *list = help_list_new(item_size);

  help_list_append(*list, item);
}

/**
 * help_list_get - Get an item from a Help document list
 * @param list  List to use
 * @param index Index in list
 * @param copy  Function to copy item (may be NULL)
 * @retval ptr  Item selected
 * @retval NULL Invalid index
 */
static void *help_list_get(struct HelpList *list, size_t index, void *(*copy)(const void *) )
{
  if (!list || (index >= list->size))
    return NULL;

  return ((copy) ? copy(list->data[index]) : list->data[index]);
}

/**
 * help_list_clone - Copy a list of Help documents
 * @param list   List to copy
 * @param shrink true if the list should be minimised
 * @param copy   Function to copy a list item
 * @retval ptr Duplicated list of Help documents
 */
static struct HelpList *help_list_clone(struct HelpList *list, bool shrink,
                                        void *(*copy)(const void *) )
{
  if (!list)
    return NULL;

  struct HelpList *clone = help_list_new(list->item_size);
  for (size_t i = 0; i < list->size; i++)
    help_list_append(clone, help_list_get(list, i, copy));

  if (shrink)
    help_list_shrink(clone);

  return clone;
}

/**
 * help_list_sort - Sort a list of Help documents
 * @param list    List to sort
 * @param compare Function to compare two items
 */
static void help_list_sort(struct HelpList *list, int (*compare)(const void *, const void *))
{
  if (!list)
    return;

  qsort(list->data, list->size, sizeof(void *), compare);
}

/**
 * help_doc_type_cmp - Compare two help documents by their name - Implements ::sort_t
 */
static int help_doc_type_cmp(const void *a, const void *b)
{
  const struct Email *e1 = *(const struct Email **) a;
  const struct Email *e2 = *(const struct Email **) b;
  if (mutt_str_strcasecmp(e1->path, "index.md") == 0)
    return -1;
  else if (mutt_str_strcasecmp(e2->path, "index.md") == 0)
    return 1;

  return mutt_str_strcmp(e1->path, e2->path);
}

/**
 * help_file_hdr_free - Free a file header
 * @param item File header
 */
static void help_file_hdr_free(void **item)
{
  struct HelpFileHeader *fhdr = ((struct HelpFileHeader *) *item);

  FREE(&fhdr->key);
  FREE(&fhdr->val);
  FREE(&fhdr);
}

/**
 * help_doc_meta_free - Free help doc metadata
 * @param data Metadata
 *
 * @note Called by mutt_header_free() to free custom metadata in Context::data
 */
static void help_doc_meta_free(void **data)
{
  if (!data || !*data)
    return;

  struct HelpDocMeta *meta = *data;

  FREE(&meta->name);
  help_list_free(&meta->fhdr, help_file_hdr_free);
  *data = NULL;
}

/**
 * help_doc_free - Free a DocList element
 * @param item DocList element
 */
static void help_doc_free(void **item)
{
  struct Email *hdoc = ((struct Email *) *item);

  email_free(&hdoc);
  FREE(hdoc);
}

/**
 * help_doclist_free - Free the global DocList
 */
void help_doclist_free(void)
{
  help_list_free(&DocList, help_doc_free);
  mutt_str_strfcpy(DocDirID, "", sizeof(DocDirID));
}

/**
 * help_checksum_md5 - Calculate a string MD5 checksum and store it in a buffer
 * @param string String to hash
 * @param digest Buffer for storing the calculated hash
 *
 * @note The digest buffer _must_ be at least 33 bytes long
 */
static void help_checksum_md5(const char *string, char *digest)
{
  unsigned char md5[16];

  mutt_md5(NONULL(string), md5);
  mutt_md5_toascii(md5, digest);
}

/**
 * help_docdir_id - Get current DocDirID
 * @param docdir Path to set the DocDirID from (optional)
 * @retval ptr Current DocDirID MD5 checksum string
 */
static char *help_docdir_id(const char *docdir)
{
  if (docdir && DocList) /* only set ID if DocList != NULL */
    help_checksum_md5(docdir, DocDirID);

  return DocDirID;
}

/**
 * help_docdir_changed - Determine if $help_doc_dir differs from previous run
 * @retval true  `$help_doc_dir` path differs (DocList rebuilt recommended)
 * @retval false same $help_doc_dir path where DocList was built from
 */
static bool help_docdir_changed(void)
{
  char digest[33];
  help_checksum_md5(C_HelpDocDir, digest);

  return (mutt_str_strcmp(DocDirID, digest) != 0);
}

/**
 * help_file_type - Determine the type of a help file (relative to #C_HelpDocDir)
 * @param  file as full qualified file path of the document to test
 * @retval num Type of the file/document, see #HelpDocFlags
 *
 * @note The type of a file is determined only from its path string, so it does
 *       not need to exist. That means also, a file can have a proper type, but
 *       the document itself may be invalid (and discarded later by a filter).
 */
static HelpDocFlags help_file_type(const char *file)
{
  HelpDocFlags type = HELP_DOC_UNKNOWN;
  const size_t l = mutt_str_strlen(file);
  const size_t m = mutt_str_strlen(C_HelpDocDir);

  if ((l < 5) || (m == 0) || (l <= m))
    return type; /* relative subpath requirements doesn't match the minimum */

  const char *p = file + m;
  const char *q = file + l - 3;

  if ((mutt_str_strncasecmp(q, ".md", 3) != 0) ||
      (mutt_str_strncmp(file, C_HelpDocDir, m) != 0))
  {
    return type; /* path below C_HelpDocDir and ".md" extension are mandatory */
  }

  if (mutt_str_strcasecmp(q = strrchr(p, '/'), "/index.md") == 0)
    type = HELP_DOC_INDEX; /* help document is a special named ("index.md") file */
  else
    type = 0;

  if (p == q)
    type |= HELP_DOC_ROOTDOC; /* help document lives directly in C_HelpDocDir root */
  else if ((p = strchr(p + 1, '/')) == q)
    type |= HELP_DOC_CHAPTER;
  else /* handle all remaining (deeper nested) help documents as a section */
    type |= HELP_DOC_SECTION;

  mutt_debug(1, "File '%s' has type %d\n", file, type);
  return type;
}

/**
 * help_file_header - Process and extract a YAML header of a potential help file
 * @param fhdr list where to store the final header information
 * @param file path to the (potential help document) file to parse
 * @param max  how many header lines to read (N < 0 means all)
 * @retval (N>=0) success, N valid header lines read from file
 * @retval -1     file isn't a helpdoc: extension doesn't match ".md"
 * @retval -2     file header not read: file cannot be open for read
 * @retval -3     found invalid header: no triple-dashed start mark
 * @retval -4     found invalid header: no triple-dashed end mark
 */
static int help_file_header(struct HelpList **fhdr, const char *file, int max)
{
  const char *bfn = mutt_path_basename(NONULL(file));
  const char *ext = strrchr(bfn, '.');
  if (!bfn || (ext == bfn) || (mutt_str_strncasecmp(ext, ".md", 3) != 0))
    return -1;

  FILE *fp = mutt_file_fopen(file, "r");
  if (!fp)
    return -2;

  int lineno = 0;
  size_t linelen;
  const char *mark = "---";

  char *p = mutt_file_read_line(NULL, &linelen, fp, &lineno, 0);
  if (mutt_str_strcmp(p, mark) != 0)
  {
    mutt_file_fclose(&fp);
    return -3;
  }

  struct HelpList *list = NULL;
  char *q = NULL;
  bool endmark = false;
  int count = 0;
  int limit = (max < 0) ? -1 : max;

  while ((mutt_file_read_line(p, &linelen, fp, &lineno, 0) != NULL) &&
         !(endmark = (mutt_str_strcmp(p, mark) == 0)) && ((q = strpbrk(p, ": \t")) != NULL))
  {
    if (limit == 0)
      continue; /* to find the end mark that qualify the header as valid */
    else if ((p == q) || (*q != ':'))
      continue; /* to skip wrong keyworded lines, XXX: or should we abort? */

    mutt_str_remove_trailing_ws(p);
    struct HelpFileHeader *item = mutt_mem_calloc(1, sizeof(struct HelpFileHeader));
    item->key = mutt_str_substr_dup(p, q);
    item->val = mutt_str_strdup(mutt_str_skip_whitespace(NONULL(++q)));
    help_list_new_append(&list, sizeof(struct HelpFileHeader), item);

    count++;
    limit--;
  }
  mutt_file_fclose(&fp);
  FREE(&p);

  if (!endmark)
  {
    help_list_free(&list, help_file_hdr_free);
    count = -4;
  }
  else
  {
    help_list_shrink(list);
    *fhdr = list;
  }

  return count;
}

/**
 * help_file_hdr_find - Find a help document header line by its key(word)
 * @param key  string to search for in fhdr list (case-sensitive)
 * @param fhdr list of struct HelpFileHeader elements to search for key
 * @retval ptr  Success, struct containing the found key
 * @retval NULL Failure, or when key could not be found
 */
static struct HelpFileHeader *help_file_hdr_find(const char *key, const struct HelpList *fhdr)
{
  if (!fhdr || !key || !*key)
    return NULL;

  struct HelpFileHeader *hdr = NULL;
  for (size_t i = 0; i < fhdr->size; i++)
  {
    if (mutt_str_strcmp(((struct HelpFileHeader *) fhdr->data[i])->key, key) != 0)
      continue;

    hdr = fhdr->data[i];
    break;
  }

  return hdr;
}

/**
 * help_doc_msg_id - Return a simple message ID
 * @param tm Timestamp used for date part in the message ID
 * @retval ptr Generated message ID string
 */
static char *help_doc_msg_id(const struct tm *tm)
{
  char buf[128];
  unsigned char rndid[MUTT_RANDTAG_LEN + 1];
  mutt_rand_base32(rndid, sizeof(rndid) - 1);
  rndid[MUTT_RANDTAG_LEN] = 0;

  snprintf(buf, sizeof(buf), "<%d%02d%02d%02d%02d%02d.%s>", tm->tm_year + 1900,
           tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, rndid);

  return mutt_str_strdup(buf);
}

/**
 * help_doc_subject - Build a message subject, based on a string format and
 *                    from the keyword value(s) of help file header line(s)
 * @param fhdr    list of struct HelpFileHeader elements to search for key
 * @param defsubj this given default subject will be used on failures
 * @param strfmt  the string format to use for the subject line (only
 *                string conversion placeholder ("%s") are supported)
 * @param key     a header line keyword whose value will be used at the
 *                related strfmt placeholder position
 * @param ...     additional keywords to find (not limited in count)
 * @retval ptr Success, holds the built subject or defsubj otherwise
 *
 * @note  Whether length, flag nor precision specifier currently supported, only
 *        poor "%s" placeholder will be recognised.
 * XXX: - Should the length of the subject line be limited by an additional
 *        parameter? Currently an internal limit of STRING (256) is used.
 *      - This function use variable argument list (va_list), that may provide
 *        more than the real specified arguments and there seems to be no way to
 *        stop processing after the last real parameter. Therefore, the last
 *        parameter MUST be NULL currently or weird things happens when count of
 *        placeholder in strfmt and specified keys differs.
 */
static char *help_doc_subject(struct HelpList *fhdr, const char *defsubj,
                              const char *strfmt, const char *key, ...)
{
  va_list ap;
  char subject[256]; /* XXX: Should trailing white space (from strfmt) be removed? */

  struct HelpFileHeader *hdr = NULL;
  const char *f = NULL;
  const char *p = strfmt;
  const char *q = NULL;
  size_t l = 0;
  int size = 0;

  va_start(ap, key);
  while ((q = strstr(p, "%s")) && key)
  {
    hdr = help_file_hdr_find(key, fhdr);
    if (!hdr)
    {
      mutt_str_strfcpy(subject, defsubj, sizeof(subject));
      break;
    }

    f = mutt_str_substr_dup(p, strstr(q + 2, "%s"));
    size = snprintf(subject + l, sizeof(subject) - l, f, hdr->val);

    if (size < 0)
    {
      mutt_str_strfcpy(subject, defsubj, sizeof(subject));
      break;
    }

    p += mutt_str_strlen(f);
    l += size;
    key = va_arg(ap, const char *);
  }
  va_end(ap);

  return mutt_str_strdup(subject);
}

/**
 * help_path_transpose - Convert (vice versa) a scheme prefixed into a file path
 * @param path     to transpose (starts with "help[://]" or $help_doc_dir)
 * @param validate specifies the strictness, if true, file path must exist
 * @retval ptr  Success, contains the transposed contrary path
 * @retval NULL Failure, path may not exist or was invalid
 *
 * @note The resulting path is sanitised, which means any trailing slash(es) of
 *       the input path are stripped.
 */
static char *help_path_transpose(const char *path, bool validate)
{
  if (!path || !*path)
    return NULL;

  char fqp[PATH_MAX];
  char url[PATH_MAX];

  const char *docdir = C_HelpDocDir;
  const char *scheme = "help"; /* TODO: don't hardcode, use sth. like: UrlMap[U_HELP]->name */
  const char *result = NULL;
  size_t i = 0;
  size_t j = 0;

  if (mutt_str_strncasecmp(path, scheme, (j = mutt_str_strlen(scheme))) == 0)
  { /* unlike url_check_scheme(), allow scheme alone (without a separator) */
    if ((path[j] != ':') && (path[j] != '\0'))
      return NULL;
    if (path[j] == ':')
      j += 1;

    result = fqp;
    i = mutt_str_strlen(docdir);
  }
  else if (mutt_str_strncmp(path, docdir, (j = mutt_str_strlen(docdir))) == 0)
  {
    if ((path[j] != '/') && (path[j] != '\0'))
      return NULL;

    result = url;
    i = mutt_str_strlen(scheme) + 3;
  }
  else
  {
    return NULL;
  }

  j += strspn(path + j, "/");
  snprintf(fqp, sizeof(fqp), "%s/%s", docdir, path + j);
  snprintf(url, sizeof(url), "%s://%s", scheme, path + j);
  j = mutt_str_strlen(result);

  while ((i < j) && (result[j - 1] == '/'))
    j--;

  return (validate && !realpath(fqp, NULL)) ? NULL : strndup(result, j);
}

/**
 * help_file_hdr_clone - Callback to clone a file header object (struct HelpFileHeader)
 * @param item list element pointer to the object to copy
 * @retval ptr  Success, the duplicated object
 * @retval NULL Failure, otherwise
 */
static void *help_file_hdr_clone(const void *item)
{
  if (!item)
    return NULL;

  struct HelpFileHeader *src = (struct HelpFileHeader *) item;
  struct HelpFileHeader *dup = mutt_mem_calloc(1, sizeof(struct HelpFileHeader));

  dup->key = mutt_str_strdup(src->key);
  dup->val = mutt_str_strdup(src->val);

  return dup;
}

/**
 * help_doc_meta_clone - Callback to clone a help metadata object (struct HelpDocMeta)
 * @param item list element pointer to the object to copy
 * @retval ptr  Success, the duplicated object
 * @retval NULL Failure, otherwise
 */
static void *help_doc_meta_clone(const void *item)
{
  if (!item)
    return NULL;

  struct HelpDocMeta *src = (struct HelpDocMeta *) item;
  struct HelpDocMeta *dup = mutt_mem_calloc(1, sizeof(struct HelpDocMeta));

  dup->fhdr = help_list_clone(src->fhdr, true, help_file_hdr_clone);
  dup->name = mutt_str_strdup(src->name);
  dup->type = src->type;

  return dup;
}

/**
 * help_doc_clone - Callback to clone a help document object (Email)
 * @param item list element pointer to the object to copy
 * @retval ptr  Success, the duplicated object
 * @retval NULL Failure, otherwise
 *
 * @note This function should only duplicate statically defined attributes from
 *       an Email object that help_doc_from() build and return.
 */
static void *help_doc_clone(const void *item)
{
  if (!item)
    return NULL;

  struct Email *src = (struct Email *) item;
  struct Email *dup = email_new();
  /* struct Email */
  dup->date_sent = src->date_sent;
  dup->display_subject = src->display_subject;
  dup->index = src->index;
  dup->path = mutt_str_strdup(src->path);
  dup->read = src->read;
  dup->received = src->received;
  /* struct Email::data (custom metadata) */
  dup->edata = help_doc_meta_clone(src->edata);
  dup->free_edata = help_doc_meta_free;
  /* struct Body */
  dup->content = mutt_body_new();
  dup->content->disposition = src->content->disposition;
  dup->content->encoding = src->content->encoding;
  dup->content->length = src->content->length;
  dup->content->subtype = mutt_str_strdup(src->content->subtype);
  dup->content->type = src->content->type;
  /* struct Envelope */
  dup->env = mutt_env_new();
  mutt_addrlist_copy(&dup->env->from, &src->env->from, false);
  dup->env->message_id = mutt_str_strdup(src->env->message_id);
  dup->env->organization = mutt_str_strdup(src->env->organization);
  dup->env->subject = mutt_str_strdup(src->env->subject);
  /* struct Envelope::references */
  struct ListNode *src_np = NULL, *dup_np = NULL;
  STAILQ_FOREACH(src_np, &src->env->references, entries)
  {
    dup_np = mutt_mem_calloc(1, sizeof(struct ListNode));
    dup_np->data = mutt_str_strdup(src_np->data);
    STAILQ_INSERT_TAIL(&dup->env->references, dup_np, entries);
  }

  return dup;
}

/**
 * help_doc_from - Provides a validated/newly created help document (Email) from
 *                 a full qualified file path
 * @param file that is related to be a help document
 * @retval ptr  Success, an Email
 * @retval NULL Failure, otherwise
 *
 * @note This function only statically set specific member of an Email structure
 *       and some attributes, like Email::index, should be reset/updated.
 *       It also use Email::data to store additional help document information.
 */
static struct Email *help_doc_from(const char *file)
{
  mutt_debug(1, "entering help_doc_from: '%s'\n", file);
  HelpDocFlags type = HELP_DOC_UNKNOWN;

  type = help_file_type(file);
  if (type == HELP_DOC_UNKNOWN)
    return NULL; /* file is not a valid help doc */

  struct HelpList *fhdr = NULL;
  int len = help_file_header(&fhdr, file, HELP_FHDR_MAXLINES);
  if (!fhdr || (len < 1))
    return NULL; /* invalid or empty file header */

  /* from here, it should be safe to treat file as a valid help document */
  const char *bfn = mutt_path_basename(file);
  const char *pdn = mutt_path_basename(mutt_path_dirname(file));
  const char *rfp = (file + mutt_str_strlen(C_HelpDocDir) + 1);
  /* default timestamp, based on PACKAGE_VERSION */
  struct tm *tm = mutt_mem_calloc(1, sizeof(struct tm));
  strptime(PACKAGE_VERSION, "%Y%m%d", tm);
  time_t epoch = mutt_date_make_time(tm, 0);
  /* default subject, final may come from file header, e.g. "[title]: description" */
  char sbj[256];
  snprintf(sbj, sizeof(sbj), "[%s]: %s", pdn, bfn);
  /* bundle metadata */
  struct HelpDocMeta *meta = mutt_mem_calloc(1, sizeof(struct HelpDocMeta));
  meta->fhdr = fhdr;
  meta->name = mutt_str_strdup(bfn);
  meta->type = type;

  struct Email *hdoc = email_new();
  /* struct Email */
  hdoc->date_sent = epoch;
  hdoc->display_subject = true;
  hdoc->index = 0;
  hdoc->path = mutt_str_strdup(rfp);
  hdoc->read = true;
  hdoc->received = epoch;
  /* struct Email::data (custom metadata) */
  hdoc->edata = meta;
  hdoc->free_edata = help_doc_meta_free;
  /* struct Body */
  hdoc->content = mutt_body_new();
  hdoc->content->disposition = DISP_INLINE;
  hdoc->content->encoding = ENC_8BIT;
  hdoc->content->length = -1;
  hdoc->content->subtype = mutt_str_strdup("plain");
  hdoc->content->type = TYPE_TEXT;
  /* struct Envelope */
  hdoc->env = mutt_env_new();
  mutt_addrlist_parse(&hdoc->env->from, "Richard Russon <rich@flatcap.org>");
  hdoc->env->message_id = help_doc_msg_id(tm);
  FREE(tm);
  hdoc->env->organization = mutt_str_strdup("NeoMutt");
  hdoc->env->subject =
      help_doc_subject(fhdr, sbj, "[%s]: %s", "title", "description", NULL);

  return hdoc;
}

/**
 * help_doc_gather - Handler callback function for help_dir_scan()
 *                   Builds a list of help document objects
 * @param list generic list, for successfully processed item paths
 * @param path absolute path of a dir entry that pass preselection
 * @retval      0  Success,
 * @retval  (N!=0) Failure, (not used currently, failures are externally
 *                 checked and silently suppressed herein)
 */
static int help_doc_gather(struct HelpList **list, const char *path)
{
  mutt_debug(1, "entering help_doc_gather: '%s'\n", path);
  help_list_new_append(list, sizeof(struct Email *), help_doc_from(path));

  return 0;
}

/**
 * help_doc_uplink - Set a reference (threading) of one help document to an other
 * @param target Document to refer to (via Email::message_id)
 * @param source Document to link
 */
static void help_doc_uplink(const struct Email *target, const struct Email *source)
{
  if (!target || !source)
    return;

  char *tgt_msgid = target->env->message_id;
  if (!tgt_msgid || !*tgt_msgid)
    return;

  mutt_list_insert_tail(&source->env->references, mutt_str_strdup(tgt_msgid));
}

/**
 * help_add_to_list - Callback for nftw whenever a file is read
 * @param fpath  Filename
 * @param sb     Timestamp for the file
 * @param tflag  File type
 * @param ftwbuf Private nftw data
 *
 * @sa https://linux.die.net/man/3/nftw
 *
 * @note Only act on file
 */
static int help_add_to_list(const char *fpath, const struct stat *sb, int tflag,
                            struct FTW *ftwbuf)
{
  mutt_debug(1, "entering add_to_list: '%s'\n", fpath);
  if (tflag == FTW_F)
    help_doc_gather(&DocList, fpath);

  return 0; /* To tell nftw() to continue */
}

/**
 * help_read_dir - Read a directory and process its entries recursively using nftw to
 *                 find and link all help documents
 * @param path absolute path of a directory
 *
 * @note All sections are linked to their parent chapter regardless how deeply
 *       they're nested on the filesystem. Empty directories are ignored.
 */
static int help_read_dir(const char *path)
{
  mutt_debug(1, "entering help_read_dir: '%s'\n", path);

  // Max of 20 open file handles, 0 flags
  if (nftw(path, help_add_to_list, 20, 0) == -1)
  {
    perror("nftw");
    return 1;
  }
  /* Sort 'index.md' in list to the top */
  help_list_sort(DocList, help_doc_type_cmp);

  struct Email *help_msg_cur = NULL;
  // All email at level 1 (directly under root will use uplinks[0] => index.md, at level n will use uplinks[n-1])
  int list_size = 16;
  int *uplinks = mutt_mem_calloc(list_size, sizeof(size_t));
  struct Email *help_msg_index = NULL;

  if (DocList->size > 0)
    help_msg_index = help_list_get(DocList, 0, NULL);

  /* link all docs except the index.md (top element) */
  for (size_t i = 1; i < DocList->size; i++)
  {
    help_msg_cur = help_list_get(DocList, i, NULL);

    int level = 1;
    char *msg_path = help_msg_cur->path;
    int c = 0;
    while (msg_path[c] != '\0')
    {
      if (msg_path[c] == '/')
        level++;
      c++;
    }

    size_t uplink_index = uplinks[level - 1];
    if (level >= list_size)
    {
      list_size *= 2;
      mutt_mem_realloc(uplinks, list_size * sizeof(size_t));
    }

    struct Email *help_msg_uplink = help_list_get(DocList, uplink_index, NULL);
    mutt_debug(5, "Uplinking '%s' to '%s'\n", path, help_msg_uplink->path);
    help_doc_uplink(help_msg_uplink, help_msg_cur);
    help_msg_cur->index = i;
    uplinks[level] = i;

    // Flatten the top chapters in index
    if ((level == 2) && (uplink_index == (i - 1)))
      if (mutt_list_match(help_msg_index->env->message_id, &help_msg_uplink->env->references))
        mutt_list_free(&help_msg_uplink->env->references);
  }

  return 0;
}

/**
 * help_doclist_init - Initialise the DocList at $help_doc_dir
 * @retval  0 Success,
 * @retval -1 Failure, when help_dir_scan() of $help_doc_dir fails
 *
 * @note Initialisation depends on several things, like $help_doc_dir changed,
 *       DocList isn't (and should not) be cached, DocList is empty.
 */
int help_doclist_init(void)
{
  if ((HELP_CACHE_DOCLIST != 0) && DocList && !help_docdir_changed())
    return 0;

  help_doclist_free();
  DocList = help_list_new(sizeof(struct Email));
  help_read_dir(C_HelpDocDir);
  help_docdir_id(C_HelpDocDir);
  return 0;
}

/**
 * help_doclist_parse - Evaluate and copy the DocList items to Context struct
 * @param m Mailbox
 * @retval  0 Success,
 * @retval -1 Failure, e.g. DocList initialisation failed
 *
 * @note XXX This function also sets the status of a help document to unread,
 *       when its path match the user input, so the index line will mark it.
 *       This is just a test, has room for improvements and is less-than-ideal,
 *       because the user needs some knowledge about helpbox folder structure.
 */
static int help_doclist_parse(struct Mailbox *m)
{
  if ((help_doclist_init() != 0) || (DocList->size == 0))
    return -1;

  m->emails = (struct Email **) (help_list_clone(DocList, true, help_doc_clone))->data;
  m->msg_count = m->email_max = DocList->size;
  mutt_mem_realloc(&m->v2r, sizeof(int) * m->email_max);

  mutt_make_label_hash(m);

  m->readonly = true;
  /* all document paths are relative to C_HelpDocDir, so no transpose of ctx->path */
  mutt_str_replace(&m->realpath, C_HelpDocDir);

  /* check (none strict) what the user wants to see */
  const char *request = help_path_transpose(mutt_b2s(&m->pathbuf), false);
  m->emails[0]->read = false;
  if (request)
  {
    mutt_buffer_strcpy(&m->pathbuf, help_path_transpose(request, false)); /* just sanitise */
    request += mutt_str_strlen(C_HelpDocDir) + 1;
    for (size_t i = 0; i < m->msg_count; i++)
    { /* TODO: prioritise folder (chapter/section) over root file names */
      if (mutt_str_strncmp(m->emails[i]->path, request, mutt_str_strlen(request)) == 0)
      {
        m->emails[0]->read = true;
        m->emails[i]->read = false;
        break;
      }
    }
  }

  return 0;
}

/**
 * help_ac_find - Find an Account that matches a Mailbox path -- Implements MxOps::ac_find
 */
struct Account *help_ac_find(struct Account *a, const char *path)
{
  if (!a || !path)
    return NULL;

  return a;
}

/**
 * help_ac_add - Add a Mailbox to an Account -- Implements MxOps::ac_add
 */
int help_ac_add(struct Account *a, struct Mailbox *m)
{
  if (!a || !m)
    return -1;

  if (m->magic != MUTT_HELP)
    return -1;

  m->account = a;

  struct MailboxNode *np = mutt_mem_calloc(1, sizeof(*np));
  np->mailbox = m;
  STAILQ_INSERT_TAIL(&a->mailboxes, np, entries);
  return 0;
}

/**
 * help_mbox_open - Open a Mailbox -- Implements MxOps::mbox_open
 */
static int help_mbox_open(struct Mailbox *m)
{
  mutt_debug(1, "entering help_mbox_open\n");

  if (m->magic != MUTT_HELP)
    return -1;

  /* TODO: ensure either mutt_option_set()/mutt_expand_path() sanitise a DT_PATH
   * option or let help_docdir_changed() treat "/path" and "/path///" as equally
   * to avoid a useless re-caching of the same directory */
  if (help_docdir_changed())
  {
    if (access(C_HelpDocDir, F_OK) == 0)
    { /* ensure a proper path, especially without any trailing slashes */
      mutt_str_replace(&C_HelpDocDir, NONULL(realpath(C_HelpDocDir, NULL)));
    }
    else
    {
      mutt_debug(1, "unable to access help mailbox '%s': %s (errno %d).\n",
                 C_HelpDocDir, strerror(errno), errno);
      return -1;
    }
  }

  __Backup_HTS = C_HideThreadSubject; /* backup the current global setting */
  C_HideThreadSubject = false; /* temporarily ensure subject is shown in thread view */

  return help_doclist_parse(m);
}

/**
 * help_mbox_open_append - Open a Mailbox for appending -- Implements MxOps::mbox_open_append
 */
static int help_mbox_open_append(struct Mailbox *m, OpenMailboxFlags flags)
{
  mutt_debug(1, "entering help_mbox_open_append\n");
  return -1;
}

/**
 * help_mbox_check - Check for new mail -- Implements MxOps::mbox_check
 */
static int help_mbox_check(struct Mailbox *m, int *index_hint)
{
  mutt_debug(1, "entering help_mbox_check\n");
  return 0;
}

/**
 * help_mbox_sync - Save changes to the Mailbox -- Implements MxOps::mbox_sync
 */
static int help_mbox_sync(struct Mailbox *m, int *index_hint)
{
  mutt_debug(1, "entering help_mbox_sync\n");
  return 0;
}

/**
 * help_mbox_close - Close a Mailbox -- Implements MxOps::mbox_close
 */
static int help_mbox_close(struct Mailbox *m)
{
  mutt_debug(1, "entering help_mbox_close\n");

  C_HideThreadSubject = __Backup_HTS; /* restore the previous global setting */

  return 0;
}

/**
 * help_msg_open - Open an email message in a Mailbox -- Implements MxOps::msg_open
 */
static int help_msg_open(struct Mailbox *m, struct Message *msg, int msgno)
{
  mutt_debug(1, "entering help_msg_open: %d, %s\n", msgno, m->emails[msgno]->env->subject);

  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/%s", m->realpath, m->emails[msgno]->path);

  m->emails[msgno]->read = true; /* reset a probably previously set unread status */

  msg->fp = fopen(path, "r");
  if (!msg->fp)
  {
    mutt_perror(path);
    mutt_debug(1, "fopen: %s: %s (errno %d).\n", path, strerror(errno), errno);
    return -1;
  }

  return 0;
}

/**
 * help_msg_open_new - Open a new message in a Mailbox -- Implements MxOps::msg_open_new
 */
static int help_msg_open_new(struct Mailbox *m, struct Message *msg, struct Email *e)
{
  mutt_debug(1, "entering help_msg_open_new\n");
  return -1;
}

/**
 * help_msg_commit - Save changes to an email -- Implements MxOps::msg_commit
 */
static int help_msg_commit(struct Mailbox *m, struct Message *msg)
{
  mutt_debug(1, "entering help_msg_commit\n");
  return -1;
}

/**
 * help_msg_close - Close an email -- Implements MxOps::msg_close
 */
static int help_msg_close(struct Mailbox *m, struct Message *msg)
{
  mutt_debug(1, "entering help_msg_close\n");
  mutt_file_fclose(&msg->fp);
  return 0;
}

/**
 * help_msg_padding_size - Bytes of padding between messages -- Implements MxOps::msg_padding_size
 */
static int help_msg_padding_size(struct Mailbox *m)
{
  mutt_debug(1, "entering help_msg_padding_size\n");
  return -1;
}

/**
 * help_tags_edit - Prompt and validate new messages tags -- Implements MxOps::tags_edit
 */
static int help_tags_edit(struct Mailbox *m, const char *tags, char *buf, size_t buflen)
{
  mutt_debug(1, "entering help_tags_edit\n");
  return -1;
}

/**
 * help_tags_commit - Save the tags to a message -- Implements MxOps::tags_commit
 */
static int help_tags_commit(struct Mailbox *m, struct Email *e, char *buf)
{
  mutt_debug(1, "entering help_tags_commit\n");
  return -1;
}

/**
 * help_path_probe - Is this a Help Mailbox? -- Implements MxOps::path_probe
 */
static enum MailboxType help_path_probe(const char *path, const struct stat *st)
{
  if (!path)
    return MUTT_UNKNOWN;

  if (mutt_str_strncasecmp(path, "help://", 7) == 0)
    return MUTT_HELP;

  return MUTT_UNKNOWN;
}

/**
 * help_path_canon - Canonicalise a Mailbox path -- Implements MxOps::path_canon
 */
static int help_path_canon(char *buf, size_t buflen)
{
  mutt_debug(1, "entering help_path_canon\n");
  return 0;
}

/**
 * help_path_pretty - Abbreviate a Mailbox path -- Implements MxOps::path_pretty
 */
static int help_path_pretty(char *buf, size_t buflen, const char *folder)
{
  mutt_debug(1, "entering help_path_pretty\n");
  return -1;
}

/**
 * help_path_parent - Find the parent of a Mailbox path -- Implements MxOps::path_parent
 */
static int help_path_parent(char *buf, size_t buflen)
{
  mutt_debug(1, "entering help_path_parent\n");
  return -1;
}

// clang-format off
/**
 * MxHelpOps - Help Mailbox - Implements ::MxOps
 */
struct MxOps MxHelpOps = {
  .magic            = MUTT_HELP,
  .name             = "help",
  .ac_find          = help_ac_find,
  .ac_add           = help_ac_add,
  .mbox_open        = help_mbox_open,
  .mbox_open_append = help_mbox_open_append,
  .mbox_check       = help_mbox_check,
  .mbox_sync        = help_mbox_sync,
  .mbox_close       = help_mbox_close,
  .msg_open         = help_msg_open,
  .msg_open_new     = help_msg_open_new,
  .msg_commit       = help_msg_commit,
  .msg_close        = help_msg_close,
  .msg_padding_size = help_msg_padding_size,
  .tags_edit        = help_tags_edit,
  .tags_commit      = help_tags_commit,
  .path_probe       = help_path_probe,
  .path_canon       = help_path_canon,
  .path_pretty      = help_path_pretty,
  .path_parent      = help_path_parent,
};
// clang-format on
