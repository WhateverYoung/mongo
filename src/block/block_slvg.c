/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_block_salvage_start --
 *	Start a file salvage.
 */
int
__wt_block_salvage_start(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	off_t len;
	uint32_t allocsize;

	/*
	 * Truncate the file to an initial sector plus N allocation size
	 * units (bytes trailing the last multiple of an allocation size
	 * unit must be garbage, by definition).
	 */
	if (block->fh->file_size > WT_BLOCK_DESC_SECTOR) {
		allocsize = block->allocsize;
		len = block->fh->file_size - WT_BLOCK_DESC_SECTOR;
		len = (len / allocsize) * allocsize;
		len += WT_BLOCK_DESC_SECTOR;
		if (len != block->fh->file_size)
			WT_RET(__wt_ftruncate(session, block->fh, len));
	}

	/* Reset the description sector. */
	WT_RET(__wt_desc_init(session, block->fh));

	/* The first sector of the file is the description record, skip it. */
	block->slvg_off = WT_BLOCK_DESC_SECTOR;

	/*
	 * We don't currently need to do anything about the freelist because
	 * we don't read it for salvage operations.
	 */

	return (0);
}

/*
 * __wt_block_salvage_end --
 *	End a file salvage.
 */
int
__wt_block_salvage_end(WT_SESSION_IMPL *session, WT_BLOCK *block, int success)
{
	/*
	 * If not successful, discard the free-list, it's not useful, and
	 * don't write back an updated description block.
	 */
	if (!success) {
		F_CLR(block, WT_BLOCK_OK);
		__wt_block_discard(session, block);
	}
	return (0);
}

/*
 * __wt_block_salvage_next --
 *	Return the next block from the file.
 */
int
__wt_block_salvage_next(WT_SESSION_IMPL *session, WT_BLOCK *block,
    WT_BUF *buf, uint8_t *addr, uint32_t *addr_sizep, int *eofp)
{
	WT_PAGE_DISK *dsk;
	WT_FH *fh;
	off_t max, offset;
	uint32_t allocsize, cksum, size;
	uint8_t *endp;

	*eofp = 0;

	offset = block->slvg_off;
	fh = block->fh;
	allocsize = block->allocsize;
	WT_RET(__wt_buf_initsize(session, buf, allocsize));

	/* Read through the file, looking for pages with valid checksums. */
	for (max = fh->file_size;;) {
		if (offset >= max) {			/* Check eof. */
			*eofp = 1;
			return (0);
		}

		/*
		 * Read the start of a possible page (an allocation-size block),
		 * and get a page length from it.
		 */
		WT_RET(__wt_read(session, fh, offset, allocsize, buf->mem));
		dsk = buf->mem;

		/*
		 * The page can't be more than the min/max page size, or past
		 * the end of the file.
		 */
		size = dsk->size;
		cksum = dsk->cksum;
		if (size == 0 ||
		    size % allocsize != 0 ||
		    size > WT_BTREE_PAGE_SIZE_MAX ||
		    offset + (off_t)size > max)
			goto skip;

		/*
		 * After reading the file, we write pages in order to resolve
		 * key range overlaps.   We give our newly written pages LSNs
		 * larger than any LSN found in the file in case the salvage
		 * run fails and is restarted later.  (Regardless of our LSNs,
		 * it's possible our newly written pages will have to be merged
		 * in a subsequent salvage run, at least if it's a row-store,
		 * as the key ranges are not exact.  However, having larger LSNs
		 * should make our newly written pages more likely to win over
		 * previous pages, minimizing the work done in subsequent
		 * salvage runs.)  Reset the tree's current LSN to the largest
		 * LSN we read.
		 */
		if (block->lsn < dsk->lsn)
			block->lsn = dsk->lsn;

		/*
		 * The page size isn't insane, read the entire page: reading the
		 * page validates the checksum and then decompresses the page as
		 * needed.  If reading the page fails, it's probably corruption,
		 * we ignore this block.
		 */
		if (__wt_block_read(session, block, buf, offset, size, cksum)) {
skip:			WT_VERBOSE(session, salvage,
			    "skipping %" PRIu32 "B at file offset %" PRIuMAX,
			    allocsize, (uintmax_t)offset);

			/*
			 * Free the block and make sure we don't return it more
			 * than once.
			 */
			WT_RET(
			    __wt_block_free(session, block, offset, allocsize));
			block->slvg_off = offset += allocsize;
			continue;
		}

		/* Valid block, return to our caller. */
		break;
	}

	/* Re-create the address cookie that should reference this block. */
	endp = addr;
	WT_RET(__wt_block_addr_to_buffer(block, &endp, offset, size, cksum));
	*addr_sizep = WT_PTRDIFF32(endp, addr);

	/* We're successfully returning the page, move past it. */
	block->slvg_off = offset + size;

	return (0);
}
