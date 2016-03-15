/*
 * Copyright (c) 2005-2008, Message Systems, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following
 *      disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *    * Neither the name Message Systems, Inc. nor the names
 *      of its contributors may be used to endorse or promote products
 *      derived from this software without specific prior written
 *      permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*****************************************************************

  Journaled logging... append only.

      (1) find current file, or allocate a file, extendible and mark
          it current.
 
      (2) Write records to it, records include their size, so
          a simple inspection can detect and incomplete trailing
          record.
    
      (3) Write append until the file reaches a certain size.

      (4) Allocate a file, extensible.

      (5) RESYNC INDEX on 'finished' file (see reading:3) and postpend
          an offset '0' to the index.
    
      (2) goto (1)
    
  Reading journals...

      (1) find oldest checkpoint of all subscribers, remove all older files.

      (2) (file, last_read) = find_checkpoint for this subscriber

      (3) RESYNC INDEX:
          open record index for file, seek to the end -  off_t.
          this is the offset of the last noticed record in this file.
          open file, seek to this point, roll forward writing the index file
          _do not_ write an offset for the last record unless it is found
          complete.

      (4) read entries from last_read+1 -> index of record index

*/

#include <stdio.h>

#include "jlog_config.h"
#include "jlog_private.h"
#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#if HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#if HAVE_DIRENT_H
#include <dirent.h>
#endif
#if HAVE_FCNTL_H
#include <fcntl.h>
#endif
#if HAVE_ERRNO_H
#include <errno.h>
#endif
#if HAVE_TIME_H
#include <time.h>
#endif
#if HAVE_SYS_MMAN_H
#include <sys/mman.h>
#endif

#include "fassert.h"

#define BUFFERED_INDICES 1024

static jlog_file *__jlog_open_writer(jlog_ctx *ctx);
static int __jlog_close_writer(jlog_ctx *ctx);
static jlog_file *__jlog_open_reader(jlog_ctx *ctx, u_int32_t log);
static int __jlog_close_reader(jlog_ctx *ctx);
static int __jlog_close_checkpoint(jlog_ctx *ctx);
static jlog_file *__jlog_open_indexer(jlog_ctx *ctx, u_int32_t log);
static int __jlog_close_indexer(jlog_ctx *ctx);
static int __jlog_resync_index(jlog_ctx *ctx, u_int32_t log, jlog_id *last, int *c);
static jlog_file *__jlog_open_named_checkpoint(jlog_ctx *ctx, const char *cpname, int flags);
static int __jlog_mmap_reader(jlog_ctx *ctx, u_int32_t log);
static int __jlog_munmap_reader(jlog_ctx *ctx);

int jlog_snprint_logid(char *b, int n, const jlog_id *id) {
  return snprintf(b, n, "%08x:%08x", id->log, id->marker);
}

int jlog_repair_datafile(jlog_ctx *ctx, u_int32_t log)
{
  jlog_message_header hdr;
  char *this, *next, *afternext = NULL, *mmap_end;
  int i, invalid_count = 0;
  struct {
    off_t start, end;
  } *invalid = NULL;
  off_t orig_len, src, dst, len;

#define TAG_INVALID(s, e) do { \
  if (invalid_count) \
    invalid = realloc(invalid, (invalid_count + 1) * sizeof(*invalid)); \
  else \
    invalid = malloc(sizeof(*invalid)); \
  invalid[invalid_count].start = s - (char *)ctx->mmap_base; \
  invalid[invalid_count].end = e - (char *)ctx->mmap_base; \
  invalid_count++; \
} while (0)

  ctx->last_error = JLOG_ERR_SUCCESS;

  /* we want the reader's open logic because this runs in the read path
   * the underlying fds are always RDWR anyway */
  __jlog_open_reader(ctx, log);
  if (!ctx->data) {
    ctx->last_error = JLOG_ERR_FILE_OPEN;
    ctx->last_errno = errno;
    return -1;
  }
  if (!jlog_file_lock(ctx->data)) {
    ctx->last_error = JLOG_ERR_LOCK;
    ctx->last_errno = errno;
    return -1;
  }
  if (__jlog_mmap_reader(ctx, log) != 0)
    SYS_FAIL(JLOG_ERR_FILE_READ);

  orig_len = ctx->mmap_len;
  mmap_end = (char*)ctx->mmap_base + ctx->mmap_len;
  /* these values will cause us to fall right into the error clause and
   * start searching for a valid header from offset 0 */
  this = (char*)ctx->mmap_base - sizeof(hdr);
  hdr.reserved = ctx->meta->hdr_magic;
  hdr.mlen = 0;

  while (this + sizeof(hdr) <= mmap_end) {
    next = this + sizeof(hdr) + hdr.mlen;
    if (next <= (char *)ctx->mmap_base) goto error;
    if (next == mmap_end) {
      this = next;
      break;
    }
    if (next + sizeof(hdr) > mmap_end) goto error;
    memcpy(&hdr, next, sizeof(hdr));
    if (hdr.reserved != ctx->meta->hdr_magic) goto error;
    this = next;
    continue;
  error:
    for (next = this + sizeof(hdr); next + sizeof(hdr) <= mmap_end; next++) {
      memcpy(&hdr, next, sizeof(hdr));
      if (hdr.reserved == ctx->meta->hdr_magic) {
        afternext = next + sizeof(hdr) + hdr.mlen;
        if (afternext <= (char *)ctx->mmap_base) continue;
        if (afternext == mmap_end) break;
        if (afternext + sizeof(hdr) > mmap_end) continue;
        memcpy(&hdr, afternext, sizeof(hdr));
        if (hdr.reserved == ctx->meta->hdr_magic) break;
      }
    }
    /* correct for while loop entry condition */
    if (this < (char *)ctx->mmap_base) this = ctx->mmap_base;
    if (next + sizeof(hdr) > mmap_end) break;
    if (next > this) TAG_INVALID(this, next);
    this = afternext;
  }
  if (this != mmap_end) TAG_INVALID(this, mmap_end);

#undef TAG_INVALID

#define MOVE_SEGMENT do { \
  char cpbuff[4096]; \
  off_t chunk; \
  while(len > 0) { \
    chunk = len; \
    if (chunk > sizeof(cpbuff)) chunk = sizeof(cpbuff); \
    if (!jlog_file_pread(ctx->data, &cpbuff, chunk, src)) \
      SYS_FAIL(JLOG_ERR_FILE_READ); \
    if (!jlog_file_pwrite(ctx->data, &cpbuff, chunk, dst)) \
      SYS_FAIL(JLOG_ERR_FILE_WRITE); \
    src += chunk; \
    dst += chunk; \
    len -= chunk; \
  } \
} while (0)

  if (invalid_count > 0) {
    __jlog_munmap_reader(ctx);
    dst = invalid[0].start;
    for (i = 0; i < invalid_count - 1; ) {
      src = invalid[i].end;
      len = invalid[++i].start - src;
      MOVE_SEGMENT;
    }
    src = invalid[invalid_count - 1].end;
    len = orig_len - src;
    if (len > 0) MOVE_SEGMENT;
    if (!jlog_file_truncate(ctx->data, dst))
      SYS_FAIL(JLOG_ERR_FILE_WRITE);
  }

#undef MOVE_SEGMENT

finish:
  jlog_file_unlock(ctx->data);
  if (invalid) free(invalid);
  if (ctx->last_error != JLOG_ERR_SUCCESS) return -1;
  return invalid_count;
}

int jlog_inspect_datafile(jlog_ctx *ctx, u_int32_t log, int verbose)
{
  jlog_message_header hdr;
  char *this, *next, *mmap_end;
  int i;
  time_t timet;
  struct tm tm;
  char tbuff[128];

  ctx->last_error = JLOG_ERR_SUCCESS;

  __jlog_open_reader(ctx, log);
  if (!ctx->data)
    SYS_FAIL(JLOG_ERR_FILE_OPEN);
  if (__jlog_mmap_reader(ctx, log) != 0)
    SYS_FAIL(JLOG_ERR_FILE_READ);

  mmap_end = (char*)ctx->mmap_base + ctx->mmap_len;
  this = ctx->mmap_base;
  i = 0;
  while (this + sizeof(hdr) <= mmap_end) {
    int initial = 1;
    memcpy(&hdr, this, sizeof(hdr));
    i++;
    if (hdr.reserved != ctx->meta->hdr_magic) {
      fprintf(stderr, "Message %d at [%ld] has invalid reserved value %u\n",
              i, (long int)(this - (char *)ctx->mmap_base), hdr.reserved);
      return 1;
    }

#define PRINTMSGHDR do { if(initial) { \
  fprintf(stderr, "Message %d at [%ld] of (%lu+%u)", i, \
          (long int)(this - (char *)ctx->mmap_base), \
          (long unsigned int)sizeof(hdr), hdr.mlen); \
  initial = 0; \
} } while(0)

    if(verbose) {
      PRINTMSGHDR;
    }

    next = this + sizeof(hdr) + hdr.mlen;
    if (next <= (char *)ctx->mmap_base) {
      PRINTMSGHDR;
      fprintf(stderr, " WRAPPED TO NEGATIVE OFFSET!\n");
      return 1;
    }
    if (next > mmap_end) {
      PRINTMSGHDR;
      fprintf(stderr, " OFF THE END!\n");
      return 1;
    }

    timet = hdr.tv_sec;
    localtime_r(&timet, &tm);
    strftime(tbuff, sizeof(tbuff), "%c", &tm);
    if(verbose) fprintf(stderr, "\n\ttime: %s\n\tmlen: %u\n", tbuff, hdr.mlen);
    this = next;
  }
  if (this < mmap_end) {
    fprintf(stderr, "%ld bytes of junk at the end\n",
            (long int)(mmap_end - this));
    return 1;
  }

  return 0;
finish:
  return -1;
}

int jlog_idx_details(jlog_ctx *ctx, u_int32_t log,
                     u_int32_t *marker, int *closed)
{
  off_t index_len;
  u_int64_t index;

  __jlog_open_indexer(ctx, log);
  if (!ctx->index)
    SYS_FAIL(JLOG_ERR_IDX_OPEN);
  if ((index_len = jlog_file_size(ctx->index)) == -1)
    SYS_FAIL(JLOG_ERR_IDX_SEEK);
  if (index_len % sizeof(u_int64_t))
    SYS_FAIL(JLOG_ERR_IDX_CORRUPT);
  if (index_len > sizeof(u_int64_t)) {
    if (!jlog_file_pread(ctx->index, &index, sizeof(u_int64_t),
                         index_len - sizeof(u_int64_t)))
    {
      SYS_FAIL(JLOG_ERR_IDX_READ);
    }
    if (index) {
      *marker = index_len / sizeof(u_int64_t);
      *closed = 0;
    } else {
      *marker = (index_len / sizeof(u_int64_t)) - 1;
      *closed = 1;
    }
  } else {
    *marker = index_len / sizeof(u_int64_t);
    *closed = 0;
  }

  return 0;
finish:
  return -1;
}

static int __jlog_unlink_datafile(jlog_ctx *ctx, u_int32_t log) {
  char file[MAXPATHLEN];
  int len;

  memset(file, 0, sizeof(file));
  if(ctx->current_log == log) {
    __jlog_close_reader(ctx);
    __jlog_close_indexer(ctx);
  }

  STRSETDATAFILE(ctx, file, log);
#ifdef DEBUG
  fprintf(stderr, "unlinking %s\n", file);
#endif
  unlink(file);

  len = strlen(file);
  if((len + sizeof(INDEX_EXT)) > sizeof(file)) return -1;
  memcpy(file + len, INDEX_EXT, sizeof(INDEX_EXT));
#ifdef DEBUG
  fprintf(stderr, "unlinking %s\n", file);
#endif
  unlink(file);
  return 0;
}

static int __jlog_open_metastore(jlog_ctx *ctx)
{
  char file[MAXPATHLEN];
  int len;

  memset(file, 0, sizeof(file));
#ifdef DEBUG
  fprintf(stderr, "__jlog_open_metastore\n");
#endif
  len = strlen(ctx->path);
  if((len + 1 /* IFS_CH */ + 9 /* "metastore" */ + 1) > MAXPATHLEN) {
#ifdef ENAMETOOLONG
    ctx->last_errno = ENAMETOOLONG;
#endif
    FASSERT(0, "__jlog_open_metastore: filename too long");
    ctx->last_error = JLOG_ERR_CREATE_META;
    return -1;
  }
  memcpy(file, ctx->path, len);
  file[len++] = IFS_CH;
  memcpy(&file[len], "metastore", 10); /* "metastore" + '\0' */

  ctx->metastore = jlog_file_open(file, O_CREAT, ctx->file_mode);

  if (!ctx->metastore) {
    ctx->last_errno = errno;
    FASSERT(0, "__jlog_open_metastore: file create failed");
    ctx->last_error = JLOG_ERR_CREATE_META;
    return -1;
  }

  return 0;
}

/* exported */
int __jlog_pending_readers(jlog_ctx *ctx, u_int32_t log) {
  return jlog_pending_readers(ctx, log, NULL);
}
int jlog_pending_readers(jlog_ctx *ctx, u_int32_t log,
                         u_int32_t *earliest_out) {
  int readers;
  DIR *dir;
  struct dirent *ent;
  char file[MAXPATHLEN];
  int len, seen = 0;
  u_int32_t earliest = 0;
  jlog_id id;

  memset(file, 0, sizeof(file));
  readers = 0;

  dir = opendir(ctx->path);
  if (!dir) return -1;

  len = strlen(ctx->path);
  if(len + 2 > sizeof(file)) {
    closedir(dir);
    return -1;
  }
  memcpy(file, ctx->path, len);
  file[len++] = IFS_CH;
  file[len] = '\0';

  while ((ent = readdir(dir))) {
    if (ent->d_name[0] == 'c' && ent->d_name[1] == 'p' && ent->d_name[2] == '.') {
      jlog_file *cp;
      int dlen;

      dlen = strlen(ent->d_name);
      if((len + dlen + 1) > sizeof(file)) continue;
      memcpy(file + len, ent->d_name, dlen + 1); /* include \0 */
#ifdef DEBUG
      fprintf(stderr, "Checking if %s needs %s...\n", ent->d_name, ctx->path);
#endif
      if ((cp = jlog_file_open(file, 0, ctx->file_mode))) {
        if (jlog_file_lock(cp)) {
          (void) jlog_file_pread(cp, &id, sizeof(id), 0);
#ifdef DEBUG
          fprintf(stderr, "\t%u <= %u (pending reader)\n", id.log, log);
#endif
          if (!seen) {
            earliest = id.log;
            seen = 1;
          }
          else {
            if(id.log < earliest) {
              earliest = id.log;
            }
          }
          if (id.log <= log) {
            readers++;
          }
          jlog_file_unlock(cp);
        }
        jlog_file_close(cp);
      }
    }
  }
  closedir(dir);
  if(earliest_out) *earliest_out = earliest;
  return readers;
}
struct _jlog_subs {
  char **subs;
  int used;
  int allocd;
};

int jlog_ctx_list_subscribers_dispose(jlog_ctx *ctx, char **subs) {
  char *s;
  int i = 0;
  if(subs) {
    while((s = subs[i++]) != NULL) free(s);
    free(subs);
  }
  return 0;
}

int jlog_ctx_list_subscribers(jlog_ctx *ctx, char ***subs) {
  struct _jlog_subs js = { NULL, 0, 0 };
  DIR *dir;
  struct dirent *ent;
  unsigned char file[MAXPATHLEN];
  char *p;
  int len;

  memset(file, 0, sizeof(file));
  js.subs = calloc(16, sizeof(char *));
  js.allocd = 16;

  dir = opendir(ctx->path);
  if (!dir) return -1;
  while ((ent = readdir(dir))) {
    if (ent->d_name[0] == 'c' && ent->d_name[1] == 'p' && ent->d_name[2] == '.') {

      for (len = 0, p = ent->d_name + 3; *p;) {
        unsigned char c;
        int i;

        for (c = 0, i = 0; i < 16; i++) {
          if (__jlog_hexchars[i] == *p) {
            c = i << 4;
            break;
          }
        }
        p++;
        for (i = 0; i < 16; i++) {
          if (__jlog_hexchars[i] == *p) {
            c |= i;
            break;
          }
        }
        p++;
        file[len++] = c;
      }
      file[len] = '\0';

      js.subs[js.used++] = strdup((char *)file);
      if(js.used == js.allocd) {
        js.allocd *= 2;
        js.subs = realloc(js.subs, js.allocd*sizeof(char *));
      }
      js.subs[js.used] = NULL;
    }
  }
  closedir(dir);
  *subs = js.subs;
  return js.used;
}

static int __jlog_save_metastore(jlog_ctx *ctx, int ilocked)
{
#ifdef DEBUG
  fprintf(stderr, "__jlog_save_metastore\n");
#endif

  if (!ilocked && !jlog_file_lock(ctx->metastore)) {
    FASSERT(0, "__jlog_save_metastore: cannot get lock");
    ctx->last_error = JLOG_ERR_LOCK;
    return -1;
  }

  if(ctx->meta_is_mapped) {
    int rv, flags = MS_INVALIDATE;
    if(ctx->meta->safety == JLOG_SAFE) flags |= MS_SYNC;
    rv = msync((void *)(ctx->meta), sizeof(*ctx->meta), flags);
    FASSERT(rv >= 0, "jlog_save_metastore");
    if (!ilocked) jlog_file_unlock(ctx->metastore);
    if ( rv < 0 )
      ctx->last_error = JLOG_ERR_FILE_WRITE;
    return rv;
  }
  else {
    if (!jlog_file_pwrite(ctx->metastore, ctx->meta, sizeof(*ctx->meta), 0)) {
      if (!ilocked) jlog_file_unlock(ctx->metastore);
      FASSERT(0, "jlog_file_pwrite failed");
      ctx->last_error = JLOG_ERR_FILE_WRITE;
      return -1;
    }
    if (ctx->meta->safety == JLOG_SAFE) {
      jlog_file_sync(ctx->metastore);
    }
  }

  if (!ilocked) jlog_file_unlock(ctx->metastore);
  return 0;
}

static int __jlog_restore_metastore(jlog_ctx *ctx, int ilocked)
{
  void *base = NULL;
  size_t len = 0;
  if(ctx->meta_is_mapped) return 0;
#ifdef DEBUG
  fprintf(stderr, "__jlog_restore_metastore\n");
#endif

  if (!ilocked && !jlog_file_lock(ctx->metastore)) {
    FASSERT(0, "__jlog_restore_metastore: cannot get lock");
    ctx->last_error = JLOG_ERR_LOCK;
    return -1;
  }

  if(ctx->meta_is_mapped == 0) {
    int rv;
    rv = jlog_file_map_rdwr(ctx->metastore, &base, &len);
    FASSERT(rv == 1, "jlog_file_map_rdwr");
    if(rv != 1) {
      if (!ilocked) jlog_file_unlock(ctx->metastore);
      ctx->last_error = JLOG_ERR_OPEN;
      return -1;
    }
    if(len == 12) {
      /* old metastore format doesn't have the new magic hdr in it
       * we need to extend it by four bytes, but we know the hdr was
       * previously 0, so we write out zero.
       */
       u_int32_t dummy = 0;
       jlog_file_pwrite(ctx->metastore, &dummy, sizeof(dummy), 12);
       rv = jlog_file_map_rdwr(ctx->metastore, &base, &len);
    }
    FASSERT(rv == 1, "jlog_file_map_rdwr");
    if(rv != 1 || len != sizeof(*ctx->meta)) {
      if (!ilocked) jlog_file_unlock(ctx->metastore);
      ctx->last_error = JLOG_ERR_OPEN;
      return -1;
    }
    ctx->meta = base;
    ctx->meta_is_mapped = 1;
  }

  if (!ilocked) jlog_file_unlock(ctx->metastore);

  if(ctx->meta != &ctx->pre_init)
    ctx->pre_init.hdr_magic = ctx->meta->hdr_magic;
  return 0;
}

int jlog_get_checkpoint(jlog_ctx *ctx, const char *s, jlog_id *id) {
  jlog_file *f;
  int rv = -1;

  if(ctx->subscriber_name && !strcmp(ctx->subscriber_name, s)) {
    if(!ctx->checkpoint) {
      ctx->checkpoint = __jlog_open_named_checkpoint(ctx, s, 0);
    }
    f = ctx->checkpoint;
  } else
    f = __jlog_open_named_checkpoint(ctx, s, 0);

  if (f) {
    if (jlog_file_lock(f)) {
      if (jlog_file_pread(f, id, sizeof(*id), 0)) rv = 0;
      jlog_file_unlock(f);
    }
  }
  if (f && f != ctx->checkpoint) jlog_file_close(f);
  return rv;
}

static int __jlog_set_checkpoint(jlog_ctx *ctx, const char *s, const jlog_id *id)
{
  jlog_file *f;
  int rv = -1;
  jlog_id old_id;
  u_int32_t log;

  if(ctx->subscriber_name && !strcmp(ctx->subscriber_name, s)) {
    if(!ctx->checkpoint) {
      ctx->checkpoint = __jlog_open_named_checkpoint(ctx, s, 0);
    }
    f = ctx->checkpoint;
  } else
    f = __jlog_open_named_checkpoint(ctx, s, 0);

  if(!f) return -1;
  if (!jlog_file_lock(f))
    goto failset;

  if (jlog_file_size(f) == 0) {
    /* we're setting it for the first time, no segments were pending on it */
    old_id.log = id->log;
  } else {
    if (!jlog_file_pread(f, &old_id, sizeof(old_id), 0))
      goto failset;
  }
  if (!jlog_file_pwrite(f, id, sizeof(*id), 0)) {
    FASSERT(0, "jlog_file_pwrite failed in jlog_set_checkpoint");
    ctx->last_error = JLOG_ERR_FILE_WRITE;
    goto failset;
  }
  if (ctx->meta->safety == JLOG_SAFE) {
    jlog_file_sync(f);
  }
  jlog_file_unlock(f);
  rv = 0;

  for (log = old_id.log; log < id->log; log++) {
    if (__jlog_pending_readers(ctx, log) == 0) {
      __jlog_unlink_datafile(ctx, log);
    }
  }

 failset:
  if (f && f != ctx->checkpoint) jlog_file_close(f);
  return rv;
}

static int __jlog_close_metastore(jlog_ctx *ctx) {
  if (ctx->metastore) {
    jlog_file_close(ctx->metastore);
    ctx->metastore = NULL;
  }
  if (ctx->meta_is_mapped) {
    munmap((void *)ctx->meta, sizeof(*ctx->meta));
    ctx->meta = &ctx->pre_init;
    ctx->meta_is_mapped = 0;
  }
  return 0;
}

/* path is assumed to be MAXPATHLEN */
static char *compute_checkpoint_filename(jlog_ctx *ctx, const char *subscriber, char *name)
{
  const char *sub;
  int len;

  /* build checkpoint filename */
  len = strlen(ctx->path);
  memcpy(name, ctx->path, len);
  name[len++] = IFS_CH;
  name[len++] = 'c';
  name[len++] = 'p';
  name[len++] = '.';
  for (sub = subscriber; *sub; ) {
    name[len++] = __jlog_hexchars[((*sub & 0xf0) >> 4)];
    name[len++] = __jlog_hexchars[(*sub & 0x0f)];
    sub++;
  }
  name[len] = '\0';

#ifdef DEBUG
  fprintf(stderr, "checkpoint %s filename is %s\n", subscriber, name);
#endif
  return name;
}

static jlog_file *__jlog_open_named_checkpoint(jlog_ctx *ctx, const char *cpname, int flags)
{
  char name[MAXPATHLEN];
  compute_checkpoint_filename(ctx, cpname, name);
  return jlog_file_open(name, flags, ctx->file_mode);
}

static jlog_file *__jlog_open_reader(jlog_ctx *ctx, u_int32_t log) {
  char file[MAXPATHLEN];

  memset(file, 0, sizeof(file));
  if(ctx->current_log != log) {
    __jlog_close_reader(ctx);
    __jlog_close_indexer(ctx);
  }
  if(ctx->data) {
    return ctx->data;
  }
  STRSETDATAFILE(ctx, file, log);
#ifdef DEBUG
  fprintf(stderr, "opening log file[ro]: '%s'\n", file);
#endif
  ctx->data = jlog_file_open(file, 0, ctx->file_mode);
  ctx->current_log = log;
  return ctx->data;
}

static int __jlog_munmap_reader(jlog_ctx *ctx) {
  if(ctx->mmap_base) {
    munmap(ctx->mmap_base, ctx->mmap_len);
    ctx->mmap_base = NULL;
    ctx->mmap_len = 0;
  }
  return 0;
}

static int __jlog_mmap_reader(jlog_ctx *ctx, u_int32_t log) {
  if(ctx->current_log == log && ctx->mmap_base) return 0;
  __jlog_open_reader(ctx, log);
  if(!ctx->data)
    return -1;
  if (!jlog_file_map_read(ctx->data, &ctx->mmap_base, &ctx->mmap_len)) {
    ctx->mmap_base = NULL;
    ctx->last_error = JLOG_ERR_FILE_READ;
    ctx->last_errno = errno;
    return -1;
  }
  return 0;
}

static jlog_file *__jlog_open_writer(jlog_ctx *ctx) {
  char file[MAXPATHLEN];

  if(ctx->data) {
    /* Still open */
    return ctx->data;
  }

  memset(file, 0, sizeof(file));
  FASSERT(ctx != NULL, "__jlog_open_writer");
  if(!jlog_file_lock(ctx->metastore))
    SYS_FAIL(JLOG_ERR_LOCK);
  int x;
  x = __jlog_restore_metastore(ctx, 1);
  if(x) {
    FASSERT(x == 0, "__jlog_open_writer calls jlog_restore_metastore");
    SYS_FAIL(JLOG_ERR_META_OPEN);
  }
  ctx->current_log =  ctx->meta->storage_log;
  STRSETDATAFILE(ctx, file, ctx->current_log);
#ifdef DEBUG
  fprintf(stderr, "opening log file[rw]: '%s'\n", file);
#endif
  ctx->data = jlog_file_open(file, O_CREAT, ctx->file_mode);
  FASSERT(ctx->data != NULL, "__jlog_open_writer calls jlog_file_open");
  if ( ctx->data == NULL )
    ctx->last_error = JLOG_ERR_FILE_OPEN;
  else
    ctx->last_error = JLOG_ERR_SUCCESS;
 finish:
  jlog_file_unlock(ctx->metastore);
  return ctx->data;
}

static int __jlog_close_writer(jlog_ctx *ctx) {
  if (ctx->data) {
    jlog_file_close(ctx->data);
    ctx->data = NULL;
  }
  return 0;
}

static int __jlog_close_reader(jlog_ctx *ctx) {
  __jlog_munmap_reader(ctx);
  if (ctx->data) {
    jlog_file_close(ctx->data);
    ctx->data = NULL;
  }
  return 0;
}

static int __jlog_close_checkpoint(jlog_ctx *ctx) {
  if (ctx->checkpoint) {
    jlog_file_close(ctx->checkpoint);
    ctx->checkpoint = NULL;
  }
  return 0;
}

static jlog_file *__jlog_open_indexer(jlog_ctx *ctx, u_int32_t log) {
  char file[MAXPATHLEN];
  int len;

  memset(file, 0, sizeof(file));
  if(ctx->current_log != log) {
    __jlog_close_reader(ctx);
    __jlog_close_indexer(ctx);
  }
  if(ctx->index) {
    return ctx->index;
  }
  STRSETDATAFILE(ctx, file, log);

  len = strlen(file);
  if((len + sizeof(INDEX_EXT)) > sizeof(file)) return NULL;
  memcpy(file + len, INDEX_EXT, sizeof(INDEX_EXT));
#ifdef DEBUG
  fprintf(stderr, "opening index file: '%s'\n", file);
#endif
  ctx->index = jlog_file_open(file, O_CREAT, ctx->file_mode);
  ctx->current_log = log;
  return ctx->index;
}

static int __jlog_close_indexer(jlog_ctx *ctx) {
  if (ctx->index) {
    jlog_file_close(ctx->index);
    ctx->index = NULL;
  }
  return 0;
}

static int
___jlog_resync_index(jlog_ctx *ctx, u_int32_t log, jlog_id *last,
                     int *closed) {
  jlog_message_header logmhdr;
  int i, second_try = 0;
  off_t index_off, data_off, data_len;
  u_int64_t index;
  u_int64_t indices[BUFFERED_INDICES];

  ctx->last_error = JLOG_ERR_SUCCESS;
  if(closed) *closed = 0;

  __jlog_open_reader(ctx, log);
  if (!ctx->data) {
    ctx->last_error = JLOG_ERR_FILE_OPEN;
    ctx->last_errno = errno;
    return -1;
  }

#define RESTART do { \
  if (second_try == 0) { \
    jlog_file_truncate(ctx->index, index_off); \
    jlog_file_unlock(ctx->index); \
    second_try = 1; \
    ctx->last_error = JLOG_ERR_SUCCESS; \
    goto restart; \
  } \
  SYS_FAIL(JLOG_ERR_IDX_CORRUPT); \
} while (0)

restart:
  __jlog_open_indexer(ctx, log);
  if (!ctx->index) {
    ctx->last_error = JLOG_ERR_IDX_OPEN;
    ctx->last_errno = errno;
    return -1;
  }
  if (!jlog_file_lock(ctx->index)) {
    ctx->last_error = JLOG_ERR_LOCK;
    ctx->last_errno = errno;
    return -1;
  }

  data_off = 0;
  if ((data_len = jlog_file_size(ctx->data)) == -1)
    SYS_FAIL(JLOG_ERR_FILE_SEEK);
  if ((index_off = jlog_file_size(ctx->index)) == -1)
    SYS_FAIL(JLOG_ERR_IDX_SEEK);

  if (index_off % sizeof(u_int64_t)) {
#ifdef DEBUG
    fprintf(stderr, "corrupt index [%llu]\n", index_off);
#endif
    RESTART;
  }

  if (index_off > sizeof(u_int64_t)) {
    if (!jlog_file_pread(ctx->index, &index, sizeof(index),
                         index_off - sizeof(u_int64_t)))
    {
      SYS_FAIL(JLOG_ERR_IDX_READ);
    }
    if (index == 0) {
      /* This log file has been "closed" */
#ifdef DEBUG
      fprintf(stderr, "index closed\n");
#endif
      if(last) {
        last->log = log;
        last->marker = (index_off / sizeof(u_int64_t)) - 1;
      }
      if(closed) *closed = 1;
      goto finish;
    } else {
      if (index > data_len) {
#ifdef DEBUG
        fprintf(stderr, "index told me to seek somehwere I can't\n");
#endif
        RESTART;
      }
      data_off = index;
    }
  }

  if (index_off > 0) {
    /* We are adding onto a partial index so we must advance a record */
    if (!jlog_file_pread(ctx->data, &logmhdr, sizeof(logmhdr), data_off))
      SYS_FAIL(JLOG_ERR_FILE_READ);
    if ((data_off += sizeof(logmhdr) + logmhdr.mlen) > data_len)
      RESTART;
  }

  i = 0;
  while (data_off + sizeof(logmhdr) <= data_len) {
    off_t next_off = data_off;

    if (!jlog_file_pread(ctx->data, &logmhdr, sizeof(logmhdr), data_off))
      SYS_FAIL(JLOG_ERR_FILE_READ);
    if (logmhdr.reserved != ctx->meta->hdr_magic) {
#ifdef DEBUG
      fprintf(stderr, "logmhdr.reserved == %d\n", logmhdr.reserved);
#endif
      SYS_FAIL(JLOG_ERR_FILE_CORRUPT);
    }
    if ((next_off += sizeof(logmhdr) + logmhdr.mlen) > data_len)
      break;

    /* Write our new index offset */
    indices[i++] = data_off;
    if(i >= BUFFERED_INDICES) {
#ifdef DEBUG
      fprintf(stderr, "writing %i offsets\n", i);
#endif
      if (!jlog_file_pwrite(ctx->index, indices, i * sizeof(u_int64_t), index_off))
        RESTART;
      index_off += i * sizeof(u_int64_t);
      i = 0;
    }
    data_off = next_off;
  }
  if(i > 0) {
#ifdef DEBUG
    fprintf(stderr, "writing %i offsets\n", i);
#endif
    if (!jlog_file_pwrite(ctx->index, indices, i * sizeof(u_int64_t), index_off))
      RESTART;
    index_off += i * sizeof(u_int64_t);
  }
  if(last) {
    last->log = log;
    last->marker = index_off / sizeof(u_int64_t);
  }
  if(log < ctx->meta->storage_log) {
    if (data_off != data_len) {
#ifdef DEBUG
      fprintf(stderr, "closing index, but %llu != %llu\n", data_off, data_len);
#endif
      SYS_FAIL(JLOG_ERR_FILE_CORRUPT);
    }
    /* Special case: if we are closing, we next write a '0'
     * we can't write the closing marker if the data segment had no records
     * in it, since it will be confused with an index to offset 0 by the
     * next reader; this only happens when segments are repaired */
    if (index_off) {
      index = 0;
      if (!jlog_file_pwrite(ctx->index, &index, sizeof(u_int64_t), index_off))
        RESTART;
      index_off += sizeof(u_int64_t);
    }
    if(closed) *closed = 1;
  }
#undef RESTART

finish:
  jlog_file_unlock(ctx->index);
#ifdef DEBUG
  fprintf(stderr, "index is %s\n", closed?(*closed?"closed":"open"):"unknown");
#endif
  if(ctx->last_error == JLOG_ERR_SUCCESS) return 0;
  return -1;
}

static int __jlog_resync_index(jlog_ctx *ctx, u_int32_t log, jlog_id *last, int *closed) {
  int attempts, rv = -1;
  for(attempts=0; attempts<4; attempts++) {
    rv = ___jlog_resync_index(ctx, log, last, closed);
    if(ctx->last_error == JLOG_ERR_SUCCESS) break;
    if(ctx->last_error == JLOG_ERR_FILE_OPEN ||
       ctx->last_error == JLOG_ERR_IDX_OPEN) break;

    /* We can't fix the file if someone may write to it again */
    if(log >= ctx->meta->storage_log) break;

    jlog_file_lock(ctx->index);
    /* it doesn't really matter what jlog_repair_datafile returns
     * we'll keep retrying anyway */
    jlog_repair_datafile(ctx, log);
    jlog_file_truncate(ctx->index, 0);
    jlog_file_unlock(ctx->index);
  }
  return rv;
}

jlog_ctx *jlog_new(const char *path) {
  jlog_ctx *ctx;
  ctx = calloc(1, sizeof(*ctx));
  ctx->meta = &ctx->pre_init;
  ctx->pre_init.unit_limit = DEFAULT_UNIT_LIMIT;
  ctx->pre_init.safety = DEFAULT_SAFETY;
  ctx->pre_init.hdr_magic = DEFAULT_HDR_MAGIC;
  ctx->file_mode = DEFAULT_FILE_MODE;
  ctx->context_mode = JLOG_NEW;
  ctx->path = strdup(path);
  //  fassertxsetpath(path);
  return ctx;
}

void jlog_set_error_func(jlog_ctx *ctx, jlog_error_func Func, void *ptr) {
  ctx->error_func = Func;
  ctx->error_ctx = ptr;
}

size_t jlog_raw_size(jlog_ctx *ctx) {
  DIR *d;
  struct dirent *de;
  size_t totalsize = 0;
  int ferr, len;
  char filename[MAXPATHLEN];

  d = opendir(ctx->path);
  if(!d) return 0;
  len = strlen(ctx->path);
  memcpy(filename, ctx->path, len);
  filename[len++] = IFS_CH;
  while((de = readdir(d)) != NULL) {
    struct stat sb;
    int dlen;

    dlen = strlen(de->d_name);
    if((len + dlen + 1) > sizeof(filename)) continue;
    memcpy(filename+len, de->d_name, dlen + 1); /* include \0 */
    while((ferr = stat(filename, &sb)) == -1 && errno == EINTR);
    if(ferr == 0 && S_ISREG(sb.st_mode)) totalsize += sb.st_size;
  }
  closedir(d);
  return totalsize;
}

const char *jlog_ctx_err_string(jlog_ctx *ctx) {
  switch (ctx->last_error) {
#define MSG_O_MATIC(x)  case x: return #x;
    MSG_O_MATIC( JLOG_ERR_SUCCESS);
    MSG_O_MATIC( JLOG_ERR_ILLEGAL_INIT);
    MSG_O_MATIC( JLOG_ERR_ILLEGAL_OPEN);
    MSG_O_MATIC( JLOG_ERR_OPEN);
    MSG_O_MATIC( JLOG_ERR_NOTDIR);
    MSG_O_MATIC( JLOG_ERR_CREATE_PATHLEN);
    MSG_O_MATIC( JLOG_ERR_CREATE_EXISTS);
    MSG_O_MATIC( JLOG_ERR_CREATE_MKDIR);
    MSG_O_MATIC( JLOG_ERR_CREATE_META);
    MSG_O_MATIC( JLOG_ERR_LOCK);
    MSG_O_MATIC( JLOG_ERR_IDX_OPEN);
    MSG_O_MATIC( JLOG_ERR_IDX_SEEK);
    MSG_O_MATIC( JLOG_ERR_IDX_CORRUPT);
    MSG_O_MATIC( JLOG_ERR_IDX_WRITE);
    MSG_O_MATIC( JLOG_ERR_IDX_READ);
    MSG_O_MATIC( JLOG_ERR_FILE_OPEN);
    MSG_O_MATIC( JLOG_ERR_FILE_SEEK);
    MSG_O_MATIC( JLOG_ERR_FILE_CORRUPT);
    MSG_O_MATIC( JLOG_ERR_FILE_READ);
    MSG_O_MATIC( JLOG_ERR_FILE_WRITE);
    MSG_O_MATIC( JLOG_ERR_META_OPEN);
    MSG_O_MATIC( JLOG_ERR_ILLEGAL_WRITE);
    MSG_O_MATIC( JLOG_ERR_ILLEGAL_CHECKPOINT);
    MSG_O_MATIC( JLOG_ERR_INVALID_SUBSCRIBER);
    MSG_O_MATIC( JLOG_ERR_ILLEGAL_LOGID);
    MSG_O_MATIC( JLOG_ERR_SUBSCRIBER_EXISTS);
    MSG_O_MATIC( JLOG_ERR_CHECKPOINT);
    MSG_O_MATIC( JLOG_ERR_NOT_SUPPORTED);
    MSG_O_MATIC( JLOG_ERR_CLOSE_LOGID);
    default: return "Unknown";
  }
}

int jlog_ctx_err(jlog_ctx *ctx) {
  return ctx->last_error;
}

int jlog_ctx_errno(jlog_ctx *ctx) {
  return ctx->last_errno;
}

int jlog_ctx_alter_safety(jlog_ctx *ctx, jlog_safety safety) {
  if(ctx->meta->safety == safety) return 0;
  if(ctx->context_mode == JLOG_APPEND ||
     ctx->context_mode == JLOG_NEW) {
    ctx->meta->safety = safety;
    if(ctx->context_mode == JLOG_APPEND) {
      if(__jlog_save_metastore(ctx, 0) != 0) {
        FASSERT(0, "jlog_ctx_alter_safety calls jlog_save_metastore");
        SYS_FAIL(JLOG_ERR_CREATE_META);
      }
    }
    return 0;
  }
 finish:
  return -1;
}
int jlog_ctx_alter_journal_size(jlog_ctx *ctx, size_t size) {
  if(ctx->meta->unit_limit == size) return 0;
  if(ctx->context_mode == JLOG_APPEND ||
     ctx->context_mode == JLOG_NEW) {
    ctx->meta->unit_limit = size;
    if(ctx->context_mode == JLOG_APPEND) {
      if(__jlog_save_metastore(ctx, 0) != 0) {
        FASSERT(0, "jlog_ctx_alter_journal_size calls jlog_save_metastore");
        SYS_FAIL(JLOG_ERR_CREATE_META);
      }
    }
    return 0;
  }
 finish:
  return -1;
}
int jlog_ctx_alter_mode(jlog_ctx *ctx, int mode) {
  ctx->file_mode = mode;
  return 0;
}
int jlog_ctx_open_writer(jlog_ctx *ctx) {
  int rv;
  struct stat sb;

  ctx->last_error = JLOG_ERR_SUCCESS;
  if(ctx->context_mode != JLOG_NEW) {
    ctx->last_error = JLOG_ERR_ILLEGAL_OPEN;
    return -1;
  }
  ctx->context_mode = JLOG_APPEND;
  while((rv = stat(ctx->path, &sb)) == -1 && errno == EINTR);
  if(rv == -1) SYS_FAIL(JLOG_ERR_OPEN);
  if(!S_ISDIR(sb.st_mode)) SYS_FAIL(JLOG_ERR_NOTDIR);
  FASSERT(ctx != NULL, "jlog_ctx_open_writer");
  if(__jlog_open_metastore(ctx) != 0) {
    FASSERT(0, "jlog_ctx_open_writer calls jlog_open_metastore");
    SYS_FAIL(JLOG_ERR_META_OPEN);
  }
  if(__jlog_restore_metastore(ctx, 0)) {
    FASSERT(0, "jlog_ctx_open_writer calls jlog_restore_metastore");
    SYS_FAIL(JLOG_ERR_META_OPEN);
  }
 finish:
  if(ctx->last_error == JLOG_ERR_SUCCESS) return 0;
  ctx->context_mode = JLOG_INVALID;
  return -1;
}
int jlog_ctx_open_reader(jlog_ctx *ctx, const char *subscriber) {
  int rv;
  struct stat sb;
  jlog_id dummy;

  ctx->last_error = JLOG_ERR_SUCCESS;
  if(ctx->context_mode != JLOG_NEW) {
    ctx->last_error = JLOG_ERR_ILLEGAL_OPEN;
    return -1;
  }
  ctx->context_mode = JLOG_READ;
  ctx->subscriber_name = strdup(subscriber);
  while((rv = stat(ctx->path, &sb)) == -1 && errno == EINTR);
  if(rv == -1) SYS_FAIL(JLOG_ERR_OPEN);
  if(!S_ISDIR(sb.st_mode)) SYS_FAIL(JLOG_ERR_NOTDIR);
  FASSERT(ctx != NULL, "__jlog_ctx_open_reader");
  if(__jlog_open_metastore(ctx) != 0) {
    FASSERT(0, "jlog_ctx_open_reader calls jlog_open_metastore");
    SYS_FAIL(JLOG_ERR_META_OPEN);
  }
  if(jlog_get_checkpoint(ctx, ctx->subscriber_name, &dummy))
    SYS_FAIL(JLOG_ERR_INVALID_SUBSCRIBER);
  if(__jlog_restore_metastore(ctx, 0)) {
    FASSERT(0, "jlog_ctx_open_reader calls jlog_restore_metastore");
    SYS_FAIL(JLOG_ERR_META_OPEN);
  }
 finish:
  if(ctx->last_error == JLOG_ERR_SUCCESS) return 0;
  ctx->context_mode = JLOG_INVALID;
  return -1;
}

int jlog_ctx_init(jlog_ctx *ctx) {
  int rv;
  struct stat sb;
  int dirmode;

  ctx->last_error = JLOG_ERR_SUCCESS;
  if(strlen(ctx->path) > MAXLOGPATHLEN-1) {
    ctx->last_error = JLOG_ERR_CREATE_PATHLEN;
    return -1;
  }
  if(ctx->context_mode != JLOG_NEW) {
    ctx->last_error = JLOG_ERR_ILLEGAL_INIT;
    return -1;
  }
  ctx->context_mode = JLOG_INIT;
  while((rv = stat(ctx->path, &sb)) == -1 && errno == EINTR);
  if(rv == 0 || errno != ENOENT) {
    SYS_FAIL_EX(JLOG_ERR_CREATE_EXISTS, 0);
  }
  dirmode = ctx->file_mode;
  if(dirmode & 0400) dirmode |= 0100;
  if(dirmode & 040) dirmode |= 010;
  if(dirmode & 04) dirmode |= 01;
  if(mkdir(ctx->path, dirmode) == -1)
    SYS_FAIL(JLOG_ERR_CREATE_MKDIR);
  chmod(ctx->path, dirmode);
  // fassertxsetpath(ctx->path);
  /* Setup our initial state and store our instance metadata */
  if(__jlog_open_metastore(ctx) != 0) {
    FASSERT(0, "jlog_ctx_init calls jlog_open_metastore");
    SYS_FAIL(JLOG_ERR_CREATE_META);
  }
  if(__jlog_save_metastore(ctx, 0) != 0) {
    FASSERT(0, "jlog_ctx_init calls jlog_save_metastore");
    SYS_FAIL(JLOG_ERR_CREATE_META);
  }
  //  FASSERT(0, "Start of fassert log");
 finish:
  FASSERT(ctx->last_error == JLOG_ERR_SUCCESS, "jlog_ctx_init failed");
  if(ctx->last_error == JLOG_ERR_SUCCESS) return 0;
  return -1;
}

int jlog_ctx_close(jlog_ctx *ctx) {
  __jlog_close_writer(ctx);
  __jlog_close_indexer(ctx);
  __jlog_close_reader(ctx);
  __jlog_close_metastore(ctx);
  __jlog_close_checkpoint(ctx);
  if(ctx->subscriber_name) free(ctx->subscriber_name);
  if(ctx->path) free(ctx->path);
  free(ctx);
  return 0;
}

static int __jlog_metastore_atomic_increment(jlog_ctx *ctx) {
  char file[MAXPATHLEN];

#ifdef DEBUG
  fprintf(stderr, "atomic increment on %u\n", ctx->current_log);
#endif
  memset(file, 0, sizeof(file));
  FASSERT(ctx != NULL, "__jlog_metastore_atomic_increment");
  if(ctx->data) SYS_FAIL(JLOG_ERR_NOT_SUPPORTED);
  if (!jlog_file_lock(ctx->metastore))
    SYS_FAIL(JLOG_ERR_LOCK);
  if(__jlog_restore_metastore(ctx, 1)) {
    FASSERT(0,
            "jlog_metastore_atomic_increment calls jlog_restore_metastore");
    SYS_FAIL(JLOG_ERR_META_OPEN);
  }
  if(ctx->meta->storage_log == ctx->current_log) {
    /* We're the first ones to it, so we get to increment it */
    ctx->current_log++;
    STRSETDATAFILE(ctx, file, ctx->current_log);
    ctx->data = jlog_file_open(file, O_CREAT, ctx->file_mode);
    ctx->meta->storage_log = ctx->current_log;
    if(__jlog_save_metastore(ctx, 1)) {
      FASSERT(0,
              "jlog_metastore_atomic_increment calls jlog_save_metastore");
      SYS_FAIL(JLOG_ERR_META_OPEN);
    }
  }
 finish:
  jlog_file_unlock(ctx->metastore);
  /* Now we update our curent_log to the current storage_log,
   * it may have advanced farther than we know.
   */
  ctx->current_log = ctx->meta->storage_log;
  if(ctx->last_error == JLOG_ERR_SUCCESS) return 0;
  return -1;
}
int jlog_ctx_write_message(jlog_ctx *ctx, jlog_message *mess, struct timeval *when) {
  struct timeval now;
  jlog_message_header hdr;
  off_t current_offset;

  ctx->last_error = JLOG_ERR_SUCCESS;
  if(ctx->context_mode != JLOG_APPEND) {
    ctx->last_error = JLOG_ERR_ILLEGAL_WRITE;
    ctx->last_errno = EPERM;
    return -1;
  }
 begin:
  __jlog_open_writer(ctx);
  if(!ctx->data) {
    ctx->last_error = JLOG_ERR_FILE_OPEN;
    ctx->last_errno = errno;
    return -1;
  }
  if (!jlog_file_lock(ctx->data)) {
    ctx->last_error = JLOG_ERR_LOCK;
    ctx->last_errno = errno;
    return -1;
  }

  if ((current_offset = jlog_file_size(ctx->data)) == -1)
    SYS_FAIL(JLOG_ERR_FILE_SEEK);
  if(ctx->meta->unit_limit <= current_offset) {
    jlog_file_unlock(ctx->data);
    __jlog_close_writer(ctx);
    __jlog_metastore_atomic_increment(ctx);
    goto begin;
  }

  hdr.reserved = ctx->meta->hdr_magic;
  if (when) {
    hdr.tv_sec = when->tv_sec;
    hdr.tv_usec = when->tv_usec;
  } else {
    gettimeofday(&now, NULL);
    hdr.tv_sec = now.tv_sec;
    hdr.tv_usec = now.tv_usec;
  }
  hdr.mlen = mess->mess_len;
  if (!jlog_file_pwrite(ctx->data, &hdr, sizeof(hdr), current_offset)) {
    FASSERT(0, "jlog_file_pwrite failed in jlog_ctx_write_message");
    SYS_FAIL(JLOG_ERR_FILE_WRITE);
  }
  current_offset += sizeof(hdr);
  if (!jlog_file_pwrite(ctx->data, mess->mess, mess->mess_len, current_offset)) {
    FASSERT(0, "jlog_file_pwrite failed in jlog_ctx_write_message");
    SYS_FAIL(JLOG_ERR_FILE_WRITE);
  }
  current_offset += mess->mess_len;

  if(ctx->meta->unit_limit <= current_offset) {
    jlog_file_unlock(ctx->data);
    __jlog_close_writer(ctx);
    __jlog_metastore_atomic_increment(ctx);
    return 0;
  }
 finish:
  jlog_file_unlock(ctx->data);
  if(ctx->last_error == JLOG_ERR_SUCCESS) return 0;
  return -1;
}
int jlog_ctx_read_checkpoint(jlog_ctx *ctx, const jlog_id *chkpt) {
  ctx->last_error = JLOG_ERR_SUCCESS;
  
  if(ctx->context_mode != JLOG_READ) {
    ctx->last_error = JLOG_ERR_ILLEGAL_CHECKPOINT;
    ctx->last_errno = EPERM;
    return -1;
  }
  if(__jlog_set_checkpoint(ctx, ctx->subscriber_name, chkpt) != 0) {
    ctx->last_error = JLOG_ERR_CHECKPOINT;
    ctx->last_errno = 0;
    return -1;
  }
  return 0;
}

int jlog_ctx_remove_subscriber(jlog_ctx *ctx, const char *s) {
  char name[MAXPATHLEN];
  int rv;

  compute_checkpoint_filename(ctx, s, name);
  rv = unlink(name);

  if (rv == 0) {
    ctx->last_error = JLOG_ERR_SUCCESS;
    return 1;
  }
  if (errno == ENOENT) {
    ctx->last_error = JLOG_ERR_INVALID_SUBSCRIBER;
    return 0;
  }
  return -1;
}

int jlog_ctx_add_subscriber(jlog_ctx *ctx, const char *s, jlog_position whence) {
  jlog_id chkpt;
  jlog_ctx *tmpctx = NULL;
  jlog_file *jchkpt;
  ctx->last_error = JLOG_ERR_SUCCESS;

  jchkpt = __jlog_open_named_checkpoint(ctx, s, O_CREAT|O_EXCL);
  if(!jchkpt) {
    ctx->last_errno = errno;
    if(errno == EEXIST)
      ctx->last_error = JLOG_ERR_SUBSCRIBER_EXISTS;
    else
      ctx->last_error = JLOG_ERR_OPEN;
    return -1;
  }
  jlog_file_close(jchkpt);
  
  if(whence == JLOG_BEGIN) {
    memset(&chkpt, 0, sizeof(chkpt));
    jlog_ctx_first_log_id(ctx, &chkpt);
    if(__jlog_set_checkpoint(ctx, s, &chkpt) != 0) {
      ctx->last_error = JLOG_ERR_CHECKPOINT;
      ctx->last_errno = 0;
      return -1;
    }
    return 0;
  }
  if(whence == JLOG_END) {
    jlog_id start, finish;
    memset(&chkpt, 0, sizeof(chkpt));
    FASSERT(ctx != NULL, "__jlog_ctx_add_subscriber");
    if(__jlog_open_metastore(ctx) != 0) {
      FASSERT(0, "jlog_ctx_add_subscriber calls jlog_open_metastore");
      SYS_FAIL(JLOG_ERR_META_OPEN);
    }
    if(__jlog_restore_metastore(ctx, 0)) {
      FASSERT(0, "jlog_ctx_add_subscriber calls jlog_restore_metastore");
      SYS_FAIL(JLOG_ERR_META_OPEN);
    }
    chkpt.log = ctx->meta->storage_log;
    if(__jlog_set_checkpoint(ctx, s, &chkpt) != 0)
      SYS_FAIL(JLOG_ERR_CHECKPOINT);
    tmpctx = jlog_new(ctx->path);
    if(jlog_ctx_open_reader(tmpctx, s) < 0) goto finish;
    if(jlog_ctx_read_interval(tmpctx, &start, &finish) < 0) goto finish;
    jlog_ctx_close(tmpctx);
    tmpctx = NULL;
    if(__jlog_set_checkpoint(ctx, s, &finish) != 0)
      SYS_FAIL(JLOG_ERR_CHECKPOINT);
    return 0;
  }
  ctx->last_error = JLOG_ERR_NOT_SUPPORTED;
 finish:
  if(tmpctx) jlog_ctx_close(tmpctx);
  return -1;
}

int jlog_ctx_add_subscriber_copy_checkpoint(jlog_ctx *old_ctx, const char *new,
                                const char *old) {
  jlog_id chkpt;
  jlog_ctx *new_ctx = NULL;;

  /* If there's no old checkpoint available, just return */
  if (jlog_get_checkpoint(old_ctx, old, &chkpt)) {
    return -1;
  }

  /* If we can't open the jlog_ctx, just return */
  new_ctx = jlog_new(old_ctx->path);
  if (!new_ctx) {
    return -1;
  }
  if (jlog_ctx_add_subscriber(new_ctx, new, JLOG_BEGIN)) {
    /* If it already exists, we want to overwrite it */
    if (errno != EEXIST) {
      jlog_ctx_close(new_ctx);
      return -1;
    }
  }

  /* Open a reader for the new subscriber */
  if(jlog_ctx_open_reader(new_ctx, new) < 0) {
    jlog_ctx_close(new_ctx);
    return -1;
  }

  /* Set the checkpoint of the new subscriber to 
   * the old subscriber's checkpoint */
  if (jlog_ctx_read_checkpoint(new_ctx, &chkpt)) {
    return -1;
  }

  jlog_ctx_close(new_ctx);
  return 0;
}

int jlog_ctx_write(jlog_ctx *ctx, const void *data, size_t len) {
  jlog_message m;
  m.mess = (void *)data;
  m.mess_len = len;
  return jlog_ctx_write_message(ctx, &m, NULL);
}

static int __jlog_find_first_log_after(jlog_ctx *ctx, jlog_id *chkpt,
                                jlog_id *start, jlog_id *finish) {
  jlog_id last;
  int closed;

  memcpy(start, chkpt, sizeof(*chkpt));
 attempt:
  if(__jlog_resync_index(ctx, start->log, &last, &closed) != 0) {
    if(ctx->last_error == JLOG_ERR_FILE_OPEN &&
        ctx->last_errno == ENOENT) {
      char file[MAXPATHLEN];
      int ferr, len;
      struct stat sb = {0};

      memset(file, 0, sizeof(file));
      STRSETDATAFILE(ctx, file, start->log + 1);
      while((ferr = stat(file, &sb)) == -1 && errno == EINTR);
      /* That file doesn't exist... bad, but we can fake a recovery by
         advancing the next file that does exist */
      ctx->last_error = JLOG_ERR_SUCCESS;
      if(start->log >= ctx->meta->storage_log || (ferr != 0 && errno != ENOENT)) {
        /* We don't advance past where people are writing */
        memcpy(finish, start, sizeof(*start));
        return 0;
      }
      if(__jlog_resync_index(ctx, start->log + 1, &last, &closed) != 0) {
        /* We don't advance past where people are writing */
        memcpy(finish, start, sizeof(*start));
        return 0;
      }
      len = strlen(file);
      if((len + sizeof(INDEX_EXT)) > sizeof(file)) return -1;
      memcpy(file + len, INDEX_EXT, sizeof(INDEX_EXT));
      while((ferr = stat(file, &sb)) == -1 && errno == EINTR);
      if(ferr != 0 || sb.st_size == 0) {
        /* We don't advance past where people are writing */
        memcpy(finish, start, sizeof(*start));
        return 0;
      }
      start->marker = 0;
      start->log++;  /* BE SMARTER! */
      goto attempt;
    }
    return -1; /* Just persist resync's error state */
  }

  /* If someone checkpoints off the end, be nice */
  if(last.log == start->log && last.marker < start->marker)
    memcpy(start, &last, sizeof(*start));

  if(!memcmp(start, &last, sizeof(last)) && closed) {
    char file[MAXPATHLEN];
    int ferr, len;
    struct stat sb = {0};

    memset(file, 0, sizeof(file));
    STRSETDATAFILE(ctx, file, start->log + 1);
    while((ferr = stat(file, &sb)) == -1 && errno == EINTR);
    if(ferr) {
      fprintf(stderr, "stat(%s) error: %s\n", file, strerror(errno));
      if(start->log < ctx->meta->storage_log - 1) {
        start->marker = 0;
        start->log += 2;
        memcpy(finish, start, sizeof(*start));
        return 0;
      }
    }
    if(start->log >= ctx->meta->storage_log || ferr != 0 || sb.st_size == 0) {
      /* We don't advance past where people are writing */
      memcpy(finish, start, sizeof(*start));
      return 0;
    }
    if(__jlog_resync_index(ctx, start->log + 1, &last, &closed) != 0) {
      /* We don't advance past where people are writing */
      memcpy(finish, start, sizeof(*start));
      return 0;
    }
    len = strlen(file);
    if((len + sizeof(INDEX_EXT)) > sizeof(file)) return -1;
    memcpy(file + len, INDEX_EXT, sizeof(INDEX_EXT));
    while((ferr = stat(file, &sb)) == -1 && errno == EINTR);
    if(ferr != 0 || sb.st_size == 0) {
      /* We don't advance past where people are writing */
      memcpy(finish, start, sizeof(*start));
      return 0;
    }
    start->marker = 0;
    start->log++;
    goto attempt;
  }
  memcpy(finish, &last, sizeof(last));
  return 0;
}
int jlog_ctx_read_message(jlog_ctx *ctx, const jlog_id *id, jlog_message *m) {
  off_t index_len;
  u_int64_t data_off;
  int with_lock = 0;

 once_more_with_lock:

  ctx->last_error = JLOG_ERR_SUCCESS;
  if (ctx->context_mode != JLOG_READ)
    SYS_FAIL(JLOG_ERR_ILLEGAL_WRITE);
  if (id->marker < 1) {
    SYS_FAIL(JLOG_ERR_ILLEGAL_LOGID);
  }

  __jlog_open_reader(ctx, id->log);
  if(!ctx->data)
    SYS_FAIL(JLOG_ERR_FILE_OPEN);
  __jlog_open_indexer(ctx, id->log);
  if(!ctx->index)
    SYS_FAIL(JLOG_ERR_IDX_OPEN);

  if(with_lock) {
    if (!jlog_file_lock(ctx->index)) {
      with_lock = 0;
      SYS_FAIL(JLOG_ERR_LOCK);
    }
  }

  if ((index_len = jlog_file_size(ctx->index)) == -1)
    SYS_FAIL(JLOG_ERR_IDX_SEEK);
  if (index_len % sizeof(u_int64_t))
    SYS_FAIL(JLOG_ERR_IDX_CORRUPT);
  if (id->marker * sizeof(u_int64_t) > index_len) {
    SYS_FAIL(JLOG_ERR_ILLEGAL_LOGID);
  }

  if (!jlog_file_pread(ctx->index, &data_off, sizeof(u_int64_t),
                       (id->marker - 1) * sizeof(u_int64_t)))
  {
    SYS_FAIL(JLOG_ERR_IDX_READ);
  }
  if (data_off == 0 && id->marker != 1) {
    if (id->marker * sizeof(u_int64_t) == index_len) {
      /* close tag; not a real offset */
      ctx->last_error = JLOG_ERR_CLOSE_LOGID;
      ctx->last_errno = 0;
      if(with_lock) jlog_file_unlock(ctx->index);
      return -1;
    } else {
      /* an offset of 0 in the middle of an index means curruption */
      SYS_FAIL(JLOG_ERR_IDX_CORRUPT);
    }
  }

  if(__jlog_mmap_reader(ctx, id->log) != 0)
    SYS_FAIL(JLOG_ERR_FILE_READ);

  if(data_off > ctx->mmap_len - sizeof(jlog_message_header)) {
#ifdef DEBUG
    fprintf(stderr, "read idx off end: %llu\n", data_off);
#endif
    SYS_FAIL(JLOG_ERR_IDX_CORRUPT);
  }

  memcpy(&m->aligned_header, ((u_int8_t *)ctx->mmap_base) + data_off,
         sizeof(jlog_message_header));

  if(data_off + sizeof(jlog_message_header) + m->aligned_header.mlen > ctx->mmap_len) {
#ifdef DEBUG
    fprintf(stderr, "read idx off end: %llu %llu\n", data_off, ctx->mmap_len);
#endif
    SYS_FAIL(JLOG_ERR_IDX_CORRUPT);
  }

  m->header = &m->aligned_header;
  m->mess_len = m->header->mlen;
  m->mess = (((u_int8_t *)ctx->mmap_base) + data_off + sizeof(jlog_message_header));

 finish:
  if(with_lock) jlog_file_unlock(ctx->index);
  if(ctx->last_error == JLOG_ERR_SUCCESS) return 0;
  if(!with_lock) {
    if (ctx->last_error == JLOG_ERR_IDX_CORRUPT) {
      if (jlog_file_lock(ctx->index)) {
        jlog_file_truncate(ctx->index, 0);
        jlog_file_unlock(ctx->index);
      }
    }
    ___jlog_resync_index(ctx, id->log, NULL, NULL);
    with_lock = 1;
#ifdef DEBUG
    fprintf(stderr, "read retrying with lock\n");
#endif
    goto once_more_with_lock;
  }
  return -1;
}
int jlog_ctx_read_interval(jlog_ctx *ctx, jlog_id *start, jlog_id *finish) {
  jlog_id chkpt;
  int count = 0;

  ctx->last_error = JLOG_ERR_SUCCESS;
  if(ctx->context_mode != JLOG_READ) {
    ctx->last_error = JLOG_ERR_ILLEGAL_WRITE;
    ctx->last_errno = EPERM;
    return -1;
  }

  __jlog_restore_metastore(ctx, 0);
  if(jlog_get_checkpoint(ctx, ctx->subscriber_name, &chkpt))
    SYS_FAIL(JLOG_ERR_INVALID_SUBSCRIBER);
  if(__jlog_find_first_log_after(ctx, &chkpt, start, finish) != 0)
    goto finish; /* Leave whatever error was set in find_first_log_after */
  if(start->log != chkpt.log) start->marker = 0;
  else start->marker = chkpt.marker;
  if(start->log != chkpt.log) {
    /* We've advanced our checkpoint, let's not do this work again */
    if(__jlog_set_checkpoint(ctx, ctx->subscriber_name, start) != 0)
      SYS_FAIL(JLOG_ERR_CHECKPOINT);
  }
  /* Here 'start' is actually the checkpoint, so we must advance it one.
     However, that may not be possible, if there are no messages, so first
     make sure finish is bigger */
  count = finish->marker - start->marker;
  if(finish->marker > start->marker) start->marker++;

  /* If the count is less than zero, the checkpoint is off the end
   * of the file. When this happens, we need to set it to the end of
   * the file */
  if (count < 0) {
    fprintf(stderr, "need to repair checkpoint for %s - start (%08x:%08x) > finish (%08x:%08x)\n", ctx->path, 
        start->log, start->marker, finish->log, finish->marker);
    if(__jlog_set_checkpoint(ctx, ctx->subscriber_name, finish) != 0) {
      fprintf(stderr, "failed repairing checkpoint for %s\n", ctx->path);
      SYS_FAIL(JLOG_ERR_CHECKPOINT);
    }
    if(jlog_get_checkpoint(ctx, ctx->subscriber_name, &chkpt)) {
      /* Should never happen */
      SYS_FAIL(JLOG_ERR_INVALID_SUBSCRIBER);
    }
    fprintf(stderr, "repaired checkpoint for %s: %08x:%08x\n", ctx->path, chkpt.log, chkpt.marker);
    ctx->last_error = JLOG_ERR_SUCCESS;
    count = 0;
  }

  /* We need to munmap it, so that we can remap it with more data if needed */
  __jlog_munmap_reader(ctx);
 finish:
  if(ctx->last_error == JLOG_ERR_SUCCESS) return count;
  return -1;
}

int jlog_ctx_first_log_id(jlog_ctx *ctx, jlog_id *id) {
  DIR *d;
  struct dirent *de;
  ctx->last_error = JLOG_ERR_SUCCESS;
  u_int32_t log;
  int found = 0;

  id->log = 0xffffffff;
  id->marker = 0;
  d = opendir(ctx->path);
  if (!d) return -1;

  while ((de = readdir(d))) {
    int i;
    char *cp = de->d_name;
    if(strlen(cp) != 8) continue;
    log = 0;
    for(i=0;i<8;i++) {
      log <<= 4;
      if(cp[i] >= '0' && cp[i] <= '9') log |= (cp[i] - '0');
      else if(cp[i] >= 'a' && cp[i] <= 'f') log |= (cp[i] - 'a' + 0xa);
      else if(cp[i] >= 'A' && cp[i] <= 'F') log |= (cp[i] - 'A' + 0xa);
      else break;
    }
    if(i != 8) continue;
    found = 1;
    if(log < id->log) id->log = log;
  }
  if(!found) id->log = 0;
  closedir(d);
  return 0;
}

int jlog_ctx_last_log_id(jlog_ctx *ctx, jlog_id *id) {
  ctx->last_error = JLOG_ERR_SUCCESS;
  if(ctx->context_mode != JLOG_READ) {
    ctx->last_error = JLOG_ERR_ILLEGAL_WRITE;
    ctx->last_errno = EPERM;
    return -1;
  }
  if (__jlog_restore_metastore(ctx, 0) != 0) return -1;
  ___jlog_resync_index(ctx, ctx->meta->storage_log, id, NULL);
  if(ctx->last_error == JLOG_ERR_SUCCESS) return 0;
  return -1;
}

int jlog_ctx_advance_id(jlog_ctx *ctx, jlog_id *cur, 
                        jlog_id *start, jlog_id *finish)
{
  int rv;
  if(memcmp(cur, finish, sizeof(jlog_id))) {
    start->marker++;
  } else {
    if((rv = __jlog_find_first_log_after(ctx, cur, start, finish)) != 0) {
      return rv;
    }
    if(cur->log != start->log) {
      start->marker = 1;
    }
    else start->marker = cur->marker;
  }
  return 0;
}

static int is_datafile(const char *f, u_int32_t *logid) {
  int i;
  u_int32_t l = 0;
  for(i=0; i<8; i++) {
    if((f[i] >= '0' && f[i] <= '9') ||
       (f[i] >= 'a' && f[i] <= 'f')) {
      l <<= 4;
      l |= (f[i] < 'a') ? (f[i] - '0') : (f[i] - 'a' + 10);
    }
    else
      return 0;
  }
  if(f[i] != '\0') return 0;
  if(logid) *logid = l;
  return 1;
}

int jlog_clean(const char *file) {
  int rv = -1;
  u_int32_t earliest = 0;
  jlog_ctx *log;
  DIR *dir;
  struct dirent *de;

  log = jlog_new(file);
  jlog_ctx_open_writer(log);
  dir = opendir(file);
  if(!dir) goto out;

  earliest = 0;
  if(jlog_pending_readers(log, 0, &earliest) < 0) goto out;

  rv = 0;
  while((de = readdir(dir)) != NULL) {
    u_int32_t logid;
    if(is_datafile(de->d_name, &logid) && logid < earliest) {
      char fullfile[MAXPATHLEN];
      char fullidx[MAXPATHLEN];

      memset(fullfile, 0, sizeof(fullfile));
      memset(fullidx, 0, sizeof(fullidx));
      snprintf(fullfile, sizeof(fullfile), "%s/%s", file, de->d_name);
      snprintf(fullidx, sizeof(fullidx), "%s/%s" INDEX_EXT, file, de->d_name);
      (void)unlink(fullfile);
      (void)unlink(fullidx); /* this may not exist; don't care */
      rv++;
    }
  }
  closedir(dir);
 out:
  jlog_ctx_close(log);
  return rv;
}

/* ------------------ jlog_ctx_repair() and friends ----------- */

/*
  This code attempts to repair problems with the metastore file and
  also a checkpoint file, within a jlog directory. The top level
  function takes an integer parameter and returns an integer result.
  If the argument is zero, then non-aggressive repairs
  are attempted. If the argument is non-zero, and if the
  non-aggressive repairs were not successful, then an aggressive
  repair approach is attempted. This consists of; (a) deleting
  all files in the log directory; (b) deleting the log directory
  itself.

  The reader will note that some of this functionality is addressed
  by other code within this file. An early decision was made not
  to reuse any of this code, but rather to attempt a solution from
  first principles. This is not due to a bad case of NIH, instead
  it is due to a desire to implement all and only the behaviors
  stated, without any (apparent) possibility of side effects.

  The reader will also notice that this code uses memory allocation
  for filenames and directory paths, rather than static variables of
  size MAXPATHLEN. This is also intentional. Having large local
  variables (like 4k in this case) can lead to unfortunate behavior
  on some systems. The compiler should do the right thing, but that
  does not mean that it will do the right thing.
*/

// find the earliest and latest hex files in the directory

static int findel(DIR *dir, unsigned int *earp, unsigned int *latp) {
  unsigned int maxx = 0;
  unsigned int minn = 0;
  unsigned int hexx = 0;
  struct dirent *ent;
  int havemaxx = 0;
  int haveminn = 0;
  int nent = 0;

  if ( dir == NULL )
    return 0;
  (void)rewinddir(dir);
  while ( (ent = readdir(dir)) != NULL ) {
    if ( ent->d_name[0] != '\0' ) {
      nent++;
      if ( strlen(ent->d_name) == 8 &&
           sscanf(ent->d_name, "%x", &hexx) == 1 ) {
        if ( havemaxx == 0 ) {
          havemaxx = 1;
          maxx = hexx;
        } else {
          if ( hexx > maxx )
            maxx = hexx;
        }
        if ( haveminn == 0 ) {
          haveminn = 1;
          minn = hexx;
        } else {
          if ( hexx < minn )
            minn = hexx;
        }
      }
    }
  }
  if ( (havemaxx == 1) && (latp != NULL) )
    *latp = maxx;
  if ( (haveminn == 1) && (earp != NULL) )
    *earp = minn;
  // a valid directory has at least . and .. entries
  return (nent >= 2);
}

/*
  The metastore repair command is:
   perl -e 'print pack("IIII", 0xLATEST_FILE_HERE, 4*1024*1024, 1, 0x663A7318);
      > metastore
   The final hex number is known as DEFAULT_HDR_MAGIC
*/

static int metastore_ok_p(char *ag, unsigned int lat) {
  int fd = open(ag, O_RDONLY);
  FASSERT(fd >= 0, "cannot open metastore file");
  if ( fd < 0 )
    return 0;
  // now we use a very slightly tricky way to get the filesize on
  // systems that don't necessarily have <sys/stat.h>
  off_t oof = lseek(fd, 0, SEEK_END);
  (void)lseek(fd, 0, SEEK_SET);
  size_t fourI = 4*sizeof(unsigned int);
  FASSERT(oof == (off_t)fourI, "metastore size invalid");
  if ( oof != (off_t)fourI ) {
    (void)close(fd);
    return 0;
  }
  unsigned int goal[4];
  goal[0] = lat;
  goal[1] = 4*1024*1024;
  goal[2] = 1;
  goal[3] = DEFAULT_HDR_MAGIC;
  unsigned int have[4];
  int rd = read(fd, &have[0], fourI);
  (void)close(fd);
  fd = -1;
  FASSERT(rd == fourI, "read error on metastore file");
  if ( rd != fourI )
    return 0;
  int gotem = 0;
  int i;
  for(i=0;i<4;i++) {
    if ( goal[i] == have[i] )
      gotem++;
  }
  FASSERT(gotem == 4, "metastore contents incorrect");
  return (gotem == 4);
}

static int repair_metastore(const char *pth, unsigned int lat) {
  if ( pth == NULL || pth[0] == '\0' ) {
    FASSERT(0, "invalid metastore path");
    return 0;
  }
  size_t leen = strlen(pth);
  if ( (leen == 0) || (leen > (MAXPATHLEN-12)) ) {
    FASSERT(0, "invalid metastore path length");
    return 0;
  }
  size_t leen2 = leen + strlen("metastore") + 4; 
  char *ag = (char *)calloc(leen2, sizeof(char));
  if ( ag == NULL )             /* out of memory, so bail */
    return 0;
  (void)snprintf(ag, leen2-1, "%s%cmetastore", pth, IFS_CH);
  int b = metastore_ok_p(ag, lat);
  FASSERT(b, "metastore integrity check failed");
  unsigned int goal[4];
  goal[0] = lat;
  goal[1] = 4*1024*1024;
  goal[2] = 1;
  goal[3] = DEFAULT_HDR_MAGIC;
  (void)unlink(ag);             /* start from scratch */
  int fd = creat(ag, DEFAULT_FILE_MODE);
  free((void *)ag);
  ag = NULL;
  FASSERT(fd >= 0, "cannot create new metastore file");
  if ( fd < 0 )
    return 0;
  int wr = write(fd, &goal[0], sizeof(goal));
  (void)close(fd);
  FASSERT(wr == sizeof(goal), "cannot write new metastore file");
  return (wr == sizeof(goal));
}

static int new_checkpoint(char *ag, int fd, unsigned int ear) {
  int newfd = 0;
  int sta = 0;
  if ( ag == NULL || ag[0] == '\0' ) {
    FASSERT(0, "invalid checkpoint path");
    return 0;
  }
  if ( fd < 0 ) {
    (void)unlink(ag);
    fd = creat(ag, DEFAULT_FILE_MODE);
    FASSERT(fd >= 0, "cannot create checkpoint file");
    if ( fd < 0 )
      return 0;
    else
      newfd = 1;
  }
  int x = ftruncate(fd, 0);
  FASSERT(x >= 0, "ftruncate failed to zero out checkpoint file");
  if ( x >= 0 ) {
    off_t xcvR = lseek(fd, 0, SEEK_SET);
    FASSERT(xcvR == 0, "cannot seek to beginning of checkpoint file");
    if ( xcvR == 0 ) {
      unsigned int goal[2];
      goal[0] = ear;
      goal[1] = 0;
      int wr = write(fd, goal, sizeof(goal));
      FASSERT(wr == sizeof(goal), "cannot write checkpoint file");
      sta = (wr == sizeof(goal));
    }
  }
  if ( newfd == 1 )
    (void)close(fd);
  return sta;
}

static const int five = 5;

static int repair_checkpointfile(DIR *dir, const char *pth, unsigned int ear) {
  FASSERT(dir != NULL, "invalid directory");
  if ( dir == NULL )
    return 0;
  struct dirent *ent = NULL;
  char *ag = NULL;
  int   fd = -1;

  (void)rewinddir(dir);
  size_t twoI = 2*sizeof(unsigned int);
  int sta = 0;
  while ( (ent = readdir(dir)) != NULL ) {
    if ( ent->d_name[0] != '\0' ) {
      if ( strncmp(ent->d_name, "cp.", 3) == 0 ) {
        char n[3];
        n[0] = ent->d_name[3];
        if ( n[0] != 0 )
          n[1] = ent->d_name[4];
        else
          n[1] = 0;
        n[2] = 0;
        int tilde = (int)'~';
        int mtilde = 0;
        if ( (sscanf(n, "%d", &mtilde) != 1) || (mtilde != tilde ) ) {
          sta = 1;
          break;
        }
      }
    }
  }
  FASSERT(sta, "could not find a checkpoint file");
  // we cannot simply create a checkpoint file if we don't have the
  // filename, so there is nothing to do here
  if ( sta == 0 )
    return 1;
  size_t leen = strlen(pth) + strlen(ent->d_name) + five;
  FASSERT(leen < MAXPATHLEN, "invalid checkpoint path length");
  if ( leen >= MAXPATHLEN )
    return 0;
  ag = (char *)calloc(leen+1, sizeof(char));
  if ( ag == NULL )     /* out of memory, so bail */
    return 0;
  (void)snprintf(ag, leen-1, "%s%c%s", pth, IFS_CH, ent->d_name);
  unsigned int goal[2];
  goal[0] = ear;
  goal[1] = 0;
  fd = open(ag, O_RDWR);
  sta = 0;
  FASSERT(fd >= 0, "cannot open checkpoint file");
  if ( fd >= 0 ) {
    off_t oof = lseek(fd, 0, SEEK_END);
    (void)lseek(fd, 0, SEEK_SET);
    FASSERT(oof != (off_t)twoI, "checkpoint file size incorrect");
    if ( oof == (off_t)twoI ) {
      unsigned int have[2];
      int rd = read(fd, have, sizeof(have));
      FASSERT(rd == sizeof(have), "cannot read checkpoint file");
      if ( rd == sizeof(have) ) {
        if ( (goal[0] != have[0]) || (goal[1] != have[1]) ) {
          FASSERT(0, "invalid checkpoint data");
        } else
          sta = 1;
      }
    }
  }
  if ( sta == 0 ) {
    sta = new_checkpoint(ag, fd, ear);
    FASSERT(sta, "cannot create new checkpoint file");
  }
  if ( fd >= 0 )
    (void)close(fd);
  if ( ag != NULL )
    (void)free((void *)ag);
  return sta;
}

#if 0

// we want a directory of the form DIRsepDIRsep, with a separator
// already at the end

static const char *findparentdirectory(const char *pth, int *off2fnp) {
  size_t strt = 0;
  size_t leen = strlen(pth);
  // special case: the top level directory
  if ( leen == 1 && pth[0] == IFS_CH ) {
    *off2fnp = 1;
    return strdup(pth);
  }
  // is the last char already a sep?
  if ( pth[leen-1] == IFS_CH )
    strt = leen - 2;
  else
    strt = leen - 1;
  char *sep = strrchr(&pth[strt], IFS_CH);
  *off2fnp = (int)(sep - &pth[0]);
  char *ag = strdup(pth);
  if ( ag == NULL )
    return NULL;
  ag[*off2fnp] = '\0';
  return ag;
}

static char *makeparentname(const char *pth, size_t fnlen, int *off2fnp) {
  const char *pdir = findparentdirectory(pth, off2fnp);
  if ( pdir == NULL || pdir[0] == '\0' )
    return NULL;
  char *ag = (char *)calloc(fnlen, sizeof(char));
  if ( ag == NULL )
    return(NULL);
  (void)memcpy((void *)ag, pdir, strlen(pdir));
  free((void *)pdir);
  return ag;
}

#endif

typedef struct _strlist {
  char *entry;
  struct _strlist *next;
} strlist;

static strlist *strhead = NULL;

/*
  When doing a directory traveral using readdir(), it is not safe to
  perform a rename() or unlink() during the traversal. So we have to
  save these filenames for processing after the traversal is done.
*/

static void schedule_one_file(char *fn) {
  if ( fn == NULL || fn[0] == '\0' )
    return;
  strlist *snew = (strlist *)calloc(1, sizeof(strlist));
  if ( snew == NULL )           /* no memory, bail */
    return;
  snew->entry = strdup(fn);
  if ( snew->entry == NULL )
    return;                     /* dangerous to free memory, if out of mem */
  snew->next = strhead;
  strhead = snew;
}

static void destroy_all_schedule_memory(void)
{
  strlist *runn = strhead;
  while ( runn != NULL ) {
    strlist *nxt = runn->next;
    if ( runn->entry != NULL ) {
      free((void *)(runn->entry));
      runn->entry = NULL;
    }
    free((void *)runn);
    runn = nxt;
  }
  strhead = NULL;
}

#if 0

static void move_one_file(const char *pth, char *parent, int off2fn,
                          char *nam) {
  size_t leen = strlen(pth) + strlen(nam) + five;
  if ( leen >= MAXPATHLEN )
    return;
  char *ag = (char *)calloc(leen, sizeof(char));
  if ( ag == NULL )
    return;
  (void)snprintf(ag, leen-1, "%s%c%s", pth, IFS_CH, nam);
  (void)memcpy(&parent[off2fn], nam, 1 + strlen(nam)); /* copy the NUL */
  (void)rename(ag, parent);
  free((void *)ag);
}

static void move_the_files(const char *pth, char *parent, int off2fn) {
#ifdef DUSTY_SPRINGFIELD
  (void)printf("I'd like the Dusty Springfield special, with extra dust\n");
#endif
  strlist *runn = strhead;
  while ( runn != NULL ) {
    if ( runn->entry != NULL && runn->entry[0] != '\0' )
      move_one_file(pth, parent, off2fn, runn->entry);
    runn = runn->next;
  }
  destroy_all_schedule_memory();
}

#endif

static void delete_one_file(const char *pth, char *nam) {
  size_t leen = strlen(pth) + strlen(nam) + five;
  if ( leen >= MAXPATHLEN )
    return;
  char *ag = (char *)calloc(leen, sizeof(char));
  if ( ag == NULL )
    return;
  (void)snprintf(ag, leen-1, "%s%c%s", pth, IFS_CH, nam);
  (void)unlink(ag);
  free((void *)ag);
}

static void delete_the_files(const char *pth) {
  strlist *runn = strhead;
  while ( runn != NULL ) {
    if ( runn->entry != NULL && runn->entry[0] != '\0' )
      delete_one_file(pth, runn->entry);
    runn = runn->next;
  }
  destroy_all_schedule_memory();
}

#if 0

/*
  if there are fassert files in the jlog directory, try to move them
  to the parent directory. It is ok, for the moment, if this fails.
  Also, not to be circular, never call FASSERT() in this function,
  or any of its callees. [unused]
*/

static void try_to_save_fasserts(const char *pth, DIR *dir) {
  if ( pth == NULL || pth[0] == '\0' || dir == NULL )
  return;
  size_t leen = strlen(pth);
  // an fassert file has the form fassertT, where T is the time as a long
  size_t flen = strlen("fassert");
  size_t fnlen = flen + 10 + 3;
  if ( (leen + fnlen) >= MAXPATHLEN )
    return;
  int off2fn = 0;
  char *parent = makeparentname(pth, fnlen, &off2fn);
  if ( parent == NULL )
    return;
  struct dirent *ent;
  (void)rewinddir(dir);
  int ntomove = 0;
  while ( (ent = readdir(dir)) != NULL ) {
    if ( ent->d_name[0] != '\0' ) {
      if ( strncmp("fassert", ent->d_name, flen) == 0 ) {
        // if we attempt to do a rename() during a directory traversal
        // using readdir(), the results will be undesirable
        schedule_one_file(ent->d_name);
        ntomove++;
      }
    }
  }
  if ( ntomove > 0 )
    move_the_files(pth, parent, off2fn);
  free((void *)parent);
}

#endif

/*
  Try as hard as we can to remove all files. Ignore failures of intermediate
  steps, because the user can always manually remove the directory and its
  contents if all else fails.

  We cannot use FASSERT in this function because it might create a new file
  in the directory we are trying to remove.
*/

static int rmcontents_and_dir(const char *pth, DIR *dir) {
  int sta = 0;
  if ( pth == NULL || pth[0] == '\0' )
    return 0;
  int ntodelete = 0;
  if ( dir != NULL ) {
    struct dirent *ent = NULL;
    (void)rewinddir(dir);
    while ( (ent = readdir(dir)) != NULL ) {
      if ( ent->d_name[0] != '\0' ) {
        if ( (strcmp(ent->d_name, ".") != 0) &&
             (strcmp(ent->d_name, "..") != 0) ) {
          schedule_one_file(ent->d_name);
          ntodelete++;
        }
      }
    }
    (void)closedir(dir);
  }
  if ( ntodelete > 0 )
    delete_the_files(pth);
  sta = rmdir(pth);
  return (sta >= 0);
}

/* exported */
int jlog_ctx_repair(jlog_ctx *ctx, int aggressive) {
  // step 1: extract the directory path
  const char *pth;
  DIR *dir = NULL;

  if ( ctx != NULL )
    pth = ctx->path;
  else
    pth = NULL; // fassertxgetpath();
  if ( pth == NULL || pth[0] == '\0' ) {
    FASSERT(0, "repair command cannot find jlog path");
    ctx->last_error = JLOG_ERR_NOTDIR;
    return 0;               /* hopeless without a dir name */
  }
  // step 2: find the earliest and the latest files with hex names
  dir = opendir(pth);
  FASSERT(dir != NULL, "cannot open jlog directory");
  if ( dir == NULL ) {
    int bx = 0;
    if ( aggressive == 1 )
      bx = rmcontents_and_dir(pth, NULL);
    if ( bx == 0 )
      ctx->last_error = JLOG_ERR_NOTDIR;
    else
      ctx->last_error = JLOG_ERR_SUCCESS;
    return bx;
  }
  unsigned int ear = 0;
  unsigned int lat = 0;
  int b0 = findel(dir, &ear, &lat);
  FASSERT(b0, "cannot find hex files in jlog directory");
  if ( b0 == 1 ) {
    // step 3: attempt to repair the metastore. It might not need any
    // repair, in which case nothing will happen
    int b1 = repair_metastore(pth, lat);
    FASSERT(b1, "cannot repair metastore");
    // step 4: attempt to repair the checkpoint file. It might not need
    // any repair, in which case nothing will happen
    int b2 = repair_checkpointfile(dir, pth, ear);
    FASSERT(b2, "cannot repair checkpoint file");
    // if non-aggressive repair succeeded, then declare success
    if ( (b1 == 1) && (b2 == 1) ) {
      (void)closedir(dir);
      ctx->last_error = JLOG_ERR_SUCCESS;
      return 1;
    }
  }
  // if aggressive repair is not authorized, fail
  FASSERT(aggressive, "non-aggressive repair failed");
  if ( aggressive == 0 ) {
    (void)closedir(dir);
    ctx->last_error = JLOG_ERR_CREATE_META;
    return 0;
  }
  // step 5: if there are any fassert files, try to save them by
  // moving them to the parent directory of "pth". Also make sure
  // to close the current fassert file. [unused]
  // fassertxend();
  // try_to_save_fasserts(pth, dir);
  // step 6: destroy the directory with extreme prejudice
  int b3 = rmcontents_and_dir(pth, dir);
  FASSERT(b3, "Aggressive repair of jlog directory failed");
  //  (void)closedir(dir);
  if ( b3 == 0 )
    ctx->last_error = JLOG_ERR_NOTDIR;
  else
    ctx->last_error = JLOG_ERR_SUCCESS;
  return b3;
}

/* -------------end of jlog_ctx_repair() and friends ----------- */

/* vim:se ts=2 sw=2 et: */
