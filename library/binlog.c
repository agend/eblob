/*
 * 2012+ Copyright (c) Alexey Ivanov <rbtz@ph34r.me>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*
 * This is implementation of very simple statement-based binary log conformig
 * to following blueprint:
 * - http://doc.ioremap.net/blueprints:eblob:binlog
 *
 * For now it's used only for data sorting.
 *
 * Useful information:
 * - Write-Ahead Logging
 *     http://www.sqlite.org/wal.html
 * - Algorithms for Recovery and Isolation Exploiting Semantics (ARIES)
 *     http://www.cs.berkeley.edu/~brewer/cs262/Aries.pdf
 * - Repeating History Beyond ARIES
 *     http://www.vldb.org/conf/1999/P1.pdf
 */

#include "features.h"

#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "blob.h"
#include "binlog.h"


/* Extend fd by @bcfg->prealloc_step if PREALLOC is enabled */
static inline int binlog_extend(struct eblob_binlog_cfg *bcfg, int fd) {
	int err;

	if (bcfg->flags & EBLOB_BINLOG_FLAGS_CFG_PREALLOC) {
		bcfg->prealloc_size += bcfg->prealloc_step;

		err = _binlog_allocate(fd, bcfg->prealloc_size);
		if (err) {
			EBLOB_WARNC(bcfg->log, EBLOB_LOG_ERROR, -err, "_binlog_allocate: %s: %lld",
					bcfg->binlog_path, (long long)bcfg->prealloc_size);
			return err;
		}
	}
	return 0;
}

/*
 * Preform simple checks on binlog header, later in can also signal on disk
 * data format changes or perform checksum verifications.
 */
static inline int binlog_verify_hdr(struct eblob_binlog_disk_hdr *dhdr) {
	if (strcmp(dhdr->magic, EBLOB_BINLOG_MAGIC))
		return -EINVAL;

	/* Here we can request format convertion. */
	if (dhdr->version != EBLOB_BINLOG_VERSION)
		return -ENOTSUP;

	if (dhdr->flags & (~EBLOB_BINLOG_FLAGS_CFG_ALL))
		return -EINVAL;

	return 0;
}

/*
 * Performs some basic checks on record header
 */
static inline int binlog_verify_record_hdr(struct eblob_binlog_disk_record_hdr *rhdr) {
	assert(rhdr != NULL);

	if (rhdr->type <= EBLOB_BINLOG_TYPE_FIRST || rhdr->type >= EBLOB_BINLOG_TYPE_LAST)
		return -EINVAL;

	/* For now we don't have any flags */
	if (rhdr->flags)
		return -EINVAL;

	return 0;
}

static int binlog_hdr_write(int fd, struct eblob_binlog_disk_hdr *dhdr) {
	ssize_t err;

	if(dhdr == NULL || fd < 0)
		return -EINVAL;

	/* Written header MUST be verifiable by us */
	assert(binlog_verify_hdr(dhdr) == 0);

	err = pwrite(fd, dhdr, sizeof(*dhdr), 0);
	if (err != sizeof(*dhdr))
		return (err == -1) ? -errno : -EINTR; /* TODO: handle signal case gracefully */

	err = binlog_datasync(fd);
	if (err)
		return err;
	return 0;
}

static int binlog_hdr_read(int fd, struct eblob_binlog_disk_hdr **dhdrp) {
	ssize_t err;
	struct eblob_binlog_disk_hdr *dhdr;

	assert(dhdrp != NULL);
	assert(fd >= 0);

	dhdr = malloc(sizeof(*dhdr));
	if (dhdr == NULL)
		return -ENOMEM;

	err = pread(fd, dhdr, sizeof(*dhdr), 0);
	if (err != sizeof(*dhdr)) {
		err = (err == -1) ? -errno : -EINTR; /* TODO: handle signal case gracefully */
		goto err_free_dhdr;
	}

	err = binlog_verify_hdr(dhdr);
	if (err)
		goto err_free_dhdr;

	*dhdrp = dhdr;
	return 0;

err_free_dhdr:
	free(dhdr);
	return err;
}

/*
 * Creates binlog and preallocates space for it.
 */
static int binlog_create(struct eblob_binlog_cfg *bcfg) {
	int fd, err;
	struct eblob_binlog_disk_hdr dhdr;

	assert(bcfg != NULL);
	assert(bcfg->binlog_path != NULL);
	assert(strlen(bcfg->binlog_path) != 0);

	/* Create */
	fd = open(bcfg->binlog_path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (fd == -1) {
		err = -errno;
		EBLOB_WARNC(bcfg->log, EBLOB_LOG_ERROR, -err, "open: %s", bcfg->binlog_path);
		goto err;
	}

	/* Allocate */
	err = binlog_extend(bcfg, fd);
	if (err)
		goto err_close;

	/* Construct header */
	memset(&dhdr, 0, sizeof(dhdr));
	memcpy(dhdr.magic, EBLOB_BINLOG_MAGIC, sizeof(dhdr.magic));
	dhdr.version = EBLOB_BINLOG_VERSION;
	dhdr.flags = bcfg->flags;

	/* Save header */
	err = binlog_hdr_write(fd, &dhdr);
	if (err) {
		EBLOB_WARNC(bcfg->log, EBLOB_LOG_ERROR, -err, "binlog_hdr_write: %s", bcfg->binlog_path);
		goto err_close;
	}

err_close:
	close(fd);
err:
	return err;
}

/*
 * Reads one binlog record header starting at @offset
 * and returns pointer to it.
 */
static int binlog_read_record_hdr(struct eblob_binlog_cfg *bcfg,
		struct eblob_binlog_disk_record_hdr *rhdr, off_t offset) {
	ssize_t err;

	assert(bcfg != NULL);
	assert(rhdr != NULL);
	assert(bcfg->fd >= 0);
	assert(offset > 0);

	err = pread(bcfg->fd, rhdr, sizeof(*rhdr), offset);
	if (err != sizeof(*rhdr)) {
		err = (err == -1) ? -errno : -EINTR; /* TODO: handle signal case gracefully */
		EBLOB_WARNC(bcfg->log, EBLOB_LOG_ERROR, -err, "pread: %s, offset: %lld", bcfg->binlog_path, (long long)offset);
		goto err;
	}

	EBLOB_WARNX(bcfg->log, EBLOB_LOG_DEBUG, "pread: %s, type: %lld, size: %lld, flags: %lld, key: %s, "
			"offset: %lld", bcfg->binlog_path, rhdr->type, rhdr->size,
			rhdr->flags, eblob_dump_id(rhdr->key.id), offset);

	err = binlog_verify_record_hdr(rhdr);
	if (err) {
		EBLOB_WARNC(bcfg->log, EBLOB_LOG_ERROR, -err, "binlog_verify_record_hdr: %s", bcfg->binlog_path);
		goto err;
	}

err:
	return err;
}

/*
 * Reads data from file into memory
 *
 * TODO: This process involves lots of data copying - this can be solved by
 * using mmap(2) in case @size is bigger than some specified threshold.
 *
 * TODO: unify style acording to binlog_read_record_hdr()
 */
static char *binlog_read_record_data(struct eblob_binlog_cfg *bcfg, off_t offset, ssize_t size) {
	ssize_t err;
	char *buf;

	assert(bcfg != NULL);
	assert(bcfg->fd >= 0);
	assert(offset > 0);
	assert(size > 0);

	buf = malloc(size);
	if (buf == NULL) {
		EBLOB_WARNC(bcfg->log, EBLOB_LOG_ERROR, errno, "malloc: %zd", size);
		goto err;
	}

	err = pread(bcfg->fd, buf, size, offset);
	if (err != size) {
		EBLOB_WARNC(bcfg->log, EBLOB_LOG_ERROR, ((err == -1) ? errno : EINTR), "pread: %s, offset: %lld", bcfg->binlog_path, (long long)offset);
		goto err_free;
	}
	return buf;

err_free:
	free(buf);
err:
	return NULL;
}

/*
 * Iterate over binlog, starting right after the header and find next LSN.
 *
 * FIXME: Last record of binlog can be truncated / corupted so we
 * really need checksumming
 */
static off_t binlog_get_next_lsn(struct eblob_binlog_cfg *bcfg) {
	off_t lsn;
	struct eblob_binlog_disk_record_hdr rhdr;

	lsn = sizeof(*bcfg->disk_hdr);
	while(binlog_read_record_hdr(bcfg, &rhdr, lsn) == 0) {
		lsn += rhdr.size + sizeof(rhdr);
	}

	return lsn;
}

/*
 * Returns pointer to cooked @eblob_binlog_cfg structure.
 * @path is desired name of binlog file.
 * @log is logger control structure.
 */
struct eblob_binlog_cfg *binlog_init(char *path, struct eblob_log *log) {
	int len;
	char *binlog_path;
	struct eblob_binlog_cfg *bcfg;

	if (log == NULL)
		goto err;

	if (path == NULL) {
		EBLOB_WARNX(log, EBLOB_LOG_ERROR, "path is NULL");
		goto err;
	}

	len = strlen(path);
	if (len == 0 || len > PATH_MAX) {
		EBLOB_WARNX(log, EBLOB_LOG_ERROR, "path length is out of bounds");
		goto err;
	}

	bcfg = calloc(1, sizeof(*bcfg));
	if (bcfg == NULL) {
		EBLOB_WARNX(log, EBLOB_LOG_ERROR, "malloc");
		goto err;
	}

	/* Copy path to bcfg */
	binlog_path = strndup(path, len);
	if (binlog_path == NULL) {
		EBLOB_WARNX(log, EBLOB_LOG_ERROR, "strndup");
		goto err_free_bcfg;
	}

	bcfg->flags = EBLOB_BINLOG_DEFAULTS_FLAGS;
	bcfg->prealloc_step = EBLOB_BINLOG_DEFAULTS_PREALLOC_STEP;
	bcfg->binlog_path = binlog_path;
	bcfg->fd = -1;
	bcfg->log = log;

	return bcfg;

err_free_bcfg:
	free(bcfg);
err:
	return NULL;
}

/*
 * Opens binlog for given blob.
 *
 * @bcfg->binlog_path: full path to binlog file.
 * @bcfg->prealloc_step: number of bytes to preallocate on disk for
 * binlog.
 */
int binlog_open(struct eblob_binlog_cfg *bcfg) {
	int fd, oflag, err;
	struct stat binlog_stat;

	if (bcfg == NULL)
		return -EINVAL;

	assert(bcfg->fd == -1);

	/* Creating binlog if it does not exist and use fd provided by binlog_create */
	err = binlog_create(bcfg);
	if (err && err != -EEXIST) {
		EBLOB_WARNC(bcfg->log, EBLOB_LOG_ERROR, -err, "binlog_create: %s", bcfg->binlog_path);
		goto err;
	}

	oflag = O_RDWR | O_CLOEXEC;
	if (bcfg->flags & EBLOB_BINLOG_FLAGS_CFG_SYNC)
		oflag |= O_SYNC;

	/* Open created/already existent binlog */
	fd = open(bcfg->binlog_path, oflag);
	if (fd == -1) {
		err = -errno;
		EBLOB_WARNC(bcfg->log, EBLOB_LOG_ERROR, -err, "open: %s", bcfg->binlog_path);
		goto err;
	}

	/* Lock binlog */
	err = flock(fd, LOCK_EX | LOCK_NB);
	if (err == -1) {
		err = -errno;
		EBLOB_WARNC(bcfg->log, EBLOB_LOG_ERROR, -err, "flock: %s", bcfg->binlog_path);
		goto err_close;
	}
	/* Truncate binlog if requested */
	if (bcfg->flags & EBLOB_BINLOG_FLAGS_CFG_TRUNCATE) {
		err = ftruncate(fd, sizeof(struct eblob_binlog_disk_hdr));
		if (err == -1) {
			err = -errno;
			EBLOB_WARNC(bcfg->log, EBLOB_LOG_ERROR, -err, "ftruncate: %s", bcfg->binlog_path);
			goto err_unlock;
		}
	}

	bcfg->fd = fd;

	/* It's not critical if hint fails, but we should log it anyway */
	err = eblob_pagecache_hint(bcfg->fd, EBLOB_FLAGS_HINT_WILLNEED);
	if (err)
		EBLOB_WARNC(bcfg->log, EBLOB_LOG_INFO, -err, "binlog_pgecache_hint: %s", bcfg->binlog_path);

	/* Stat binlog */
	err = fstat(bcfg->fd, &binlog_stat);
	if (err == -1) {
		err = -errno;
		EBLOB_WARNC(bcfg->log, EBLOB_LOG_ERROR, -err, "fstat: %s", bcfg->binlog_path);
		goto err_unlock;
	}
	bcfg->prealloc_size = binlog_stat.st_size;

	/* Read header */
	err = binlog_hdr_read(bcfg->fd, &bcfg->disk_hdr);
	if (err) {
		EBLOB_WARNC(bcfg->log, EBLOB_LOG_ERROR, -err, "binlog_hdr_read: %s", bcfg->binlog_path);
		goto err_unlock;
	}

	/* Find last LSN */
	bcfg->binlog_position = binlog_get_next_lsn(bcfg);
	EBLOB_WARNX(bcfg->log, EBLOB_LOG_INFO, "next LSN: %s(%d): %lld", bcfg->binlog_path,
			bcfg->fd, (long long)bcfg->binlog_position);

	return 0;

err_unlock:
	flock(fd, LOCK_UN);
err_close:
	close(fd);
err:
	return err;
}

/*
 * Append record to the end of binlog.
 */
int binlog_append(struct eblob_binlog_ctl *bctl) {
	ssize_t err, record_len;
	off_t offset;
	struct eblob_binlog_cfg *bcfg;
	struct eblob_binlog_disk_record_hdr rhdr;

	if (bctl == NULL || bctl->cfg == NULL)
		return -EINVAL;
	bcfg = bctl->cfg;

	assert(bcfg->fd >= 0);
	assert(bcfg->binlog_position > 0);

	/* Check if binlog needs to be extended */
	record_len = sizeof(rhdr) + bctl->meta_size + bctl->size;
	if (bcfg->binlog_position + record_len >= bcfg->prealloc_size) {
		err = binlog_extend(bcfg, bcfg->fd);
		if (err)
			goto err;
	}

	/* Construct record header */
	rhdr.type = bctl->type;
	rhdr.size = bctl->meta_size + bctl->size;
	rhdr.flags = bctl->flags;
	memcpy(&rhdr.key.id, bctl->key->id, sizeof(rhdr.key.id));

	/* Written header MUST be verifiable by us */
	assert(binlog_verify_record_hdr(&rhdr) == 0);

	EBLOB_WARNX(bcfg->log, EBLOB_LOG_DEBUG, "pwrite: %s, type: %lld, size: %lld, flags: %lld, key: %s, "
			"position: %lld", bcfg->binlog_path, rhdr.type, rhdr.size, rhdr.flags,
			eblob_dump_id(rhdr.key.id), bcfg->binlog_position);

	/* Write header */
	offset = bcfg->binlog_position;
	err = pwrite(bcfg->fd, &rhdr, sizeof(rhdr), offset);
	if (err != sizeof(rhdr)) {
		err = (err == -1) ? -errno : -EINTR; /* TODO: handle signal case gracefully */
		EBLOB_WARNC(bcfg->log, EBLOB_LOG_ERROR, -err, "pwrite header: %s, offset: %lld", bcfg->binlog_path, (long long)offset);
		goto err;
	}

	/* Write metadata */
	if (bctl->meta_size > 0) {
		offset += sizeof(rhdr);
		err = pwrite(bcfg->fd, bctl->meta, bctl->meta_size, offset);
		if (err != bctl->meta_size) {
			err = (err == -1) ? -errno : -EINTR; /* TODO: handle signal case gracefully */
			EBLOB_WARNC(bcfg->log, EBLOB_LOG_ERROR, -err, "pwrite metadata: %s, offset: %lld", bcfg->binlog_path, (long long)offset);
			goto err;
		}
	}

	/* Write data */
	if (bctl->size > 0) {
		offset += bctl->meta_size;
		err = pwrite(bcfg->fd, bctl->data, bctl->size, offset);
		if (err != bctl->size) {
			err = (err == -1) ? -errno : -EINTR; /* TODO: handle signal case gracefully */
			EBLOB_WARNC(bcfg->log, EBLOB_LOG_ERROR, -err, "pwrite data: %s, offset: %lld", bcfg->binlog_path, (long long)offset);
			goto err;
		}
	}

	/* Sync if not already opened with O_SYNC */
	if (!(bcfg->flags & EBLOB_BINLOG_FLAGS_CFG_SYNC)) {
		err = binlog_datasync(bcfg->fd);
		if (err) {
			EBLOB_WARNC(bcfg->log, EBLOB_LOG_ERROR, -err, "binlog_datasync: %s", bcfg->binlog_path);
			goto err;
		}
	}

	/* Finally is everything is ok - bump length */
	bcfg->binlog_position += record_len;

	return 0;

err:
	return err;
}

/*
 * Reads binlog data for from an offset
 *
 * Data is placed to @bctl->data
 */
int binlog_read(struct eblob_binlog_ctl *bctl, off_t offset) {
	int err;
	char *data = NULL;
	struct eblob_binlog_disk_record_hdr rhdr;
	struct eblob_binlog_cfg *bcfg;

	if (bctl == NULL || bctl->cfg == NULL)
		return -EINVAL;
	bcfg = bctl->cfg;

	assert(offset >= (off_t)sizeof(struct eblob_binlog_disk_hdr));

	/* Read record's header with corresponding LSN */
	err = binlog_read_record_hdr(bcfg, &rhdr, offset);
	if (err) {
		EBLOB_WARNC(bcfg->log, EBLOB_LOG_ERROR, -err, "binlog_read_record_hdr: %lld", (long long)offset);
		goto err;
	}

	/* Read data */
	if (rhdr.size) {
		data = binlog_read_record_data(bcfg, offset + sizeof(rhdr), rhdr.size);
		if (data == NULL) {
			err = -EIO;
			EBLOB_WARNX(bcfg->log, EBLOB_LOG_ERROR, "binlog_read_record_data: %lld", (long long)(offset + sizeof(rhdr)));
			goto err;
		}
	}

	bctl->type = rhdr.type;
	bctl->flags = rhdr.flags;
	memcpy(bctl->key->id, rhdr.key.id, sizeof(bctl->key->id));

	/* Record starts with metadata */
	bctl->meta_size = rhdr.meta_size;
	if (bctl->meta_size > 0)
		bctl->meta = data;
	/* Then goes data itself */
	bctl->size = rhdr.size - rhdr.meta_size;
	if (bctl->size > 0)
		bctl->data = data + bctl->meta_size;

err:
	return err;
}

/*
 * Sequentially applies given binlog to backing file.
 * From binlog header to current binlog position:
 *  - Read log record
 *  - Run function that applies given record to blob
 *
 * NB! Caller should prevent binlog from beeing modified.
 */
int binlog_apply(struct eblob_binlog_cfg *bcfg, int (*func)(struct eblob_binlog_ctl *bctl)) {
	off_t offset = sizeof(struct eblob_binlog_disk_hdr);
	struct eblob_binlog_ctl bctl;
	uint64_t count = 0;
	int err = 0;

	if (bcfg == NULL || func == NULL)
		return -EINVAL;

	assert(bcfg->binlog_position <= offset);

	EBLOB_WARNX(bcfg->log, EBLOB_LOG_INFO, "binlog_apply: %s: started", bcfg->binlog_path);
	while (offset < bcfg->binlog_position) {
		memset(&bctl, 0, sizeof(bctl));
		bctl.cfg = bcfg;

		err = binlog_read(&bctl, offset);
		if (err) {
			EBLOB_WARNC(bcfg->log, EBLOB_LOG_ERROR, -err,
					"binlog_read: %s, offset: %lld", bcfg->binlog_path, offset);
			goto err;
		}
		err = func(&bctl);
		if (err) {
			EBLOB_WARNC(bcfg->log, EBLOB_LOG_ERROR, -err,
					"(*func): %s, offset: %lld", bcfg->binlog_path, offset);
			goto err;
		}
		offset += bctl.size + sizeof(struct eblob_binlog_disk_record_hdr);
		count++;
	}
	EBLOB_WARNX(bcfg->log, EBLOB_LOG_INFO, "binlog_apply: %s: finished, offset: %lld, applied: %lld",
			bcfg->binlog_path, offset, count);

err:
	return err;
}

/*
 * Closes binlog and tries to flush it from OS memory.
 */
int binlog_close(struct eblob_binlog_cfg *bcfg) {
	int err;

	if (bcfg == NULL || bcfg->fd < 0)
		return -EINVAL;

	EBLOB_WARNX(bcfg->log, EBLOB_LOG_INFO, "closing: %s(%d)", bcfg->binlog_path,
			bcfg->fd);

	/* Write */
	err = binlog_hdr_write(bcfg->fd, bcfg->disk_hdr);
	if (err) {
		EBLOB_WARNC(bcfg->log, EBLOB_LOG_ERROR, -err, "binlog_hdr_write: %s", bcfg->binlog_path);
		goto err;
	}

	/* Sync */
	err = binlog_sync(bcfg->fd);
	if (err) {
		EBLOB_WARNC(bcfg->log, EBLOB_LOG_ERROR, -err, "binlog_sync: %s", bcfg->binlog_path);
		goto err;
	}

	/* Unlock */
	err = flock(bcfg->fd, LOCK_UN);
	if (err == -1) {
		err = -errno;
		EBLOB_WARNC(bcfg->log, EBLOB_LOG_ERROR, -err, "flock: %s", bcfg->binlog_path);
		goto err;
	}

	/* It's not critical if hint fails, but we should log it anyway */
	err = eblob_pagecache_hint(bcfg->fd, EBLOB_FLAGS_HINT_DONTNEED);
	if (err)
		EBLOB_WARNC(bcfg->log, EBLOB_LOG_INFO, -err, "binlog_pgecache_hint: %s", bcfg->binlog_path);

	/* Close */
	err = close(bcfg->fd);
	if (err == -1) {
		err = -errno;
		EBLOB_WARNC(bcfg->log, EBLOB_LOG_ERROR, -err, "close: %s", bcfg->binlog_path);
		goto err;
	}
err:
	return err;
}

/* Recursively destroys binlog object */
int binlog_destroy(struct eblob_binlog_cfg *bcfg) {
	/*
	 * It's safe to free NULL but it still strange to pass it to
	 * binlog_destroy, so return an error.
	 */
	if (bcfg == NULL)
		return -EINVAL;

	free(bcfg->disk_hdr);
	free(bcfg->binlog_path);
	free(bcfg);

	return 0;
}
