#ifndef INCLUDES_TARANTOOL_BOX_VY_LOG_H
#define INCLUDES_TARANTOOL_BOX_VY_LOG_H
/*
 * Copyright 2010-2017, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * AUTHORS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdbool.h>
#include <stdint.h>

/*
 * Data stored in vinyl is organized in ranges and runs.
 * Runs correspond to data files written to disk, while
 * ranges are used to group runs together. Sometimes, we
 * need to manipulate several ranges or runs atomically,
 * e.g. on compaction several runs are replaced with a
 * single one. To reflect events like this on disk and be
 * able to recover to a consistent state after restart, we
 * need to log all metadata changes. This module implements
 * the infrastructure necessary for such logging as well
 * as recovery.
 */

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

struct latch;
struct xlog;

struct mh_i64ptr_t;

/** Type of a vinyl metadata log record. */
enum vy_log_type {
	/**
	 * Create a new index.
	 * Requires vy_log_record::index_id.
	 */
	VY_LOG_CREATE_INDEX		= 0,
	/**
	 * Drop an index.
	 * Requires vy_log_record::index_id.
	 */
	VY_LOG_DROP_INDEX		= 1,
	/**
	 * Insert a new range into an index.
	 * Requires vy_log_record::index_id, range_id,
	 * range_begin, range_end.
	 */
	VY_LOG_INSERT_RANGE		= 2,
	/**
	 * Delete a range and all its runs.
	 * Requires vy_log_record::range_id.
	 */
	VY_LOG_DELETE_RANGE		= 3,
	/**
	 * Insert a new run into a range.
	 * Requires vy_log_record::range_id, run_id.
	 */
	VY_LOG_INSERT_RUN		= 4,
	/**
	 * Delete a run.
	 * Requires vy_log_record::run_id.
	 */
	VY_LOG_DELETE_RUN		= 5,

	vy_log_MAX
};

/** Record in vinyl metadata log. */
struct vy_log_record {
	/** Type of the record. */
	enum vy_log_type type;
	/**
	 * Unique ID of the index.
	 *
	 * The ID must be unique for different incarnations of
	 * the same index, so we use LSN from the time of index
	 * creation for it.
	 */
	int64_t index_id;
	/** Unique ID of the range. */
	int64_t range_id;
	/** Unique ID of the run. */
	int64_t run_id;
	/** Msgpack key for start of a range. */
	const char *range_begin;
	/** Msgpack key for end of a range. */
	const char *range_end;
};

/**
 * Max number of records in the log buffer.
 * This limits the size of a transaction.
 */
enum { VY_LOG_TX_BUF_SIZE = 64 };

/** Vinyl metadata log object. */
struct vy_log {
	/** Xlog object used for writing the log. */
	struct xlog *xlog;
	/** Vector clock sum from the time of the log creation. */
	int64_t signature;
	/**
	 * Latch protecting the xlog.
	 *
	 * We need to lock the log before writing to xlog, because
	 * xlog object doesn't support concurrent accesses.
	 */
	struct latch *latch;
	/** Next ID to use for a range. Used by vy_log_next_range_id(). */
	int64_t next_range_id;
	/** Next ID to use for a run. Used by vy_log_next_run_id(). */
	int64_t next_run_id;
	/**
	 * Index of the first record of the current
	 * transaction in tx_buf.
	 */
	int tx_begin;
	/**
	 * Index of the record following the last one
	 * of the current transaction in tx_buf.
	 */
	int tx_end;
	/** Records awaiting to be written to disk. */
	struct vy_log_record tx_buf[VY_LOG_TX_BUF_SIZE];
};

/** Recovery context. */
struct vy_recovery {
	/** ID -> vy_index_recovery_info. */
	struct mh_i64ptr_t *index_hash;
	/** ID -> vy_range_recovery_info. */
	struct mh_i64ptr_t *range_hash;
	/** ID -> vy_run_recovery_info. */
	struct mh_i64ptr_t *run_hash;
	/**
	 * Maximal range ID, according to the metadata log,
	 * or -1 in case no ranges were recovered.
	 */
	int64_t range_id_max;
	/**
	 * Maximal run ID, according to the metadata log,
	 * or -1 in case no runs were recovered.
	 */
	int64_t run_id_max;
	/** Vector clock sum from the time of the log creation. */
	int64_t signature;
};

/**
 * Allocate and initialize a vy_log structure.
 *
 * Returns NULL on memory allocation failure.
 */
struct vy_log *
vy_log_new(void);

/*
 * Open the vinyl metadata log file stored in directory @dir
 * having signature @signature for appending, or create a new
 * one if it doesn't exist.
 * On success, it flushes all pending records written with
 * vy_log_write() before the log was opened.
 *
 * Returns 0 on success, -1 on failure.
 */
int
vy_log_open(struct vy_log *log, const char *dir, int64_t signature);

/**
 * Close a metadata log and free associated structures.
 */
void
vy_log_delete(struct vy_log *log);

/**
 * Rotate vinyl metadata log @log. This function creates a
 * new xlog file in directory @dir having signature @signature
 * and writes records required to recover active indexes.
 * The goal of log rotation is to compact the log file by
 * discarding records cancelling each other and records left
 * from dropped indexes.
 *
 * Returns 0 on success, -1 on failure.
 */
int
vy_log_rotate(struct vy_log *log, const char *dir, int64_t signature);

/** Allocate a unique ID for a run. */
static inline int64_t
vy_log_next_run_id(struct vy_log *log)
{
	return log->next_run_id++;
}

/** Allocate a unique ID for a range. */
static inline int64_t
vy_log_next_range_id(struct vy_log *log)
{
	return log->next_range_id++;
}

/**
 * Begin a transaction in a metadata log.
 *
 * To commit the transaction, call vy_log_tx_commit() or
 * vy_log_tx_try_commit().
 */
void
vy_log_tx_begin(struct vy_log *log);

/**
 * Commit a transaction started with vy_log_tx_begin().
 *
 * This function flushes all buffered records to disk. If it fails,
 * all records of the current transaction are discarded.
 *
 * See also vy_log_tx_try_commit().
 *
 * Returns 0 on success, -1 on failure.
 */
int
vy_log_tx_commit(struct vy_log *log);

/**
 * Try to commit a transaction started with vy_log_tx_begin().
 *
 * Similarly to vy_log_tx_commit(), this function tries to write all
 * buffered records to disk, but in case of failure pending records
 * are not expunged from the buffer, so that the next transaction
 * will retry to flush them.
 *
 * Returns 0 on success, -1 on failure.
 */
int
vy_log_tx_try_commit(struct vy_log *log);

/**
 * Write a record to a metadata log.
 *
 * This function simply appends the record to the internal buffer.
 * It must be called inside a vy_log_tx_begin/commit block, and it
 * is up to vy_log_tx_commit() to actually write the record to disk.
 *
 * Returns 0 on success, -1 on failure.
 */
void
vy_log_write(struct vy_log *log, const struct vy_log_record *record);

/**
 * Load the vinyl metadata log stored in directory @dir and
 * having signature @signature and return the recovery context
 * that can be further used for recovering vinyl indexes with
 * vy_recovery_load_index().
 *
 * Returns NULL on failure.
 */
struct vy_recovery *
vy_recovery_new(const char *dir, int64_t signature);

/**
 * Free the recovery context created by a call to vy_recovery_new().
 */
void
vy_recovery_delete(struct vy_recovery *recovery);

typedef int
(*vy_recovery_cb)(const struct vy_log_record *record, void *arg);

/**
 * Given a context and index ID, recover the corresponding vinyl index.
 *
 * For each range and run of the index, this function calls @cb passing
 * a log record and an optional @cb_arg to it. A log record type is
 * either VY_LOG_CREATE_INDEX, VY_LOG_INSERT_RANGE, or VY_LOG_INSERT_RUN.
 * The callback is supposed to rebuild the index structure and open run
 * files. If the callback returns a non-zero value, the function stops
 * iteration over ranges and runs and returns error.
 * To ease the work done by the callback, records corresponding to
 * runs of a range always go right after the range, in the
 * chronological order.
 *
 * Returns 0 on success, -1 on failure.
 */
int
vy_recovery_load_index(struct vy_recovery *recovery, int64_t index_id,
		       vy_recovery_cb cb, void *cb_arg);

/**
 * Return true if the index with id @index_id was dropped or never
 * created in the scope of recovery context @recovery.
 */
bool
vy_recovery_index_is_dropped(struct vy_recovery *recovery, int64_t index_id);

/** Helper to log an index creation. */
static inline void
vy_log_create_index(struct vy_log *log, int64_t index_id)
{
	struct vy_log_record record = {
		.type = VY_LOG_CREATE_INDEX,
		.index_id = index_id,
	};
	vy_log_write(log, &record);
}

/** Helper to log an index drop. */
static inline void
vy_log_drop_index(struct vy_log *log, int64_t index_id)
{
	struct vy_log_record record = {
		.type = VY_LOG_DROP_INDEX,
		.index_id = index_id,
	};
	vy_log_write(log, &record);
}

/** Helper to log a range insertion. */
static inline void
vy_log_insert_range(struct vy_log *log, int64_t index_id, int64_t range_id,
		    const char *range_begin, const char *range_end)
{
	struct vy_log_record record = {
		.type = VY_LOG_INSERT_RANGE,
		.index_id = index_id,
		.range_id = range_id,
		.range_begin = range_begin,
		.range_end = range_end,
	};
	vy_log_write(log, &record);
}

/** Helper to log a range deletion. */
static inline void
vy_log_delete_range(struct vy_log *log, int64_t range_id)
{
	struct vy_log_record record = {
		.type = VY_LOG_DELETE_RANGE,
		.range_id = range_id,
	};
	vy_log_write(log, &record);
}

/** Helper to log a run insertion. */
static inline void
vy_log_insert_run(struct vy_log *log, int64_t range_id, int64_t run_id)
{
	struct vy_log_record record = {
		.type = VY_LOG_INSERT_RUN,
		.range_id = range_id,
		.run_id = run_id,
	};
	vy_log_write(log, &record);
}

/** Helper to log a run deletion. */
static inline void
vy_log_delete_run(struct vy_log *log, int64_t run_id)
{
	struct vy_log_record record = {
		.type = VY_LOG_DELETE_RUN,
		.run_id = run_id,
	};
	vy_log_write(log, &record);
}

#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */

#endif /* INCLUDES_TARANTOOL_BOX_VY_LOG_H */
