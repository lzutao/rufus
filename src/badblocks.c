/*
 * badblocks.c		- Bad blocks checker
 *
 * Copyright (C) 1992, 1993, 1994  Remy Card <card@masi.ibp.fr>
 *                                 Laboratoire MASI, Institut Blaise Pascal
 *                                 Universite Pierre et Marie Curie (Paris VI)
 *
 * Copyright 1995, 1996, 1997, 1998, 1999 by Theodore Ts'o
 * Copyright 1999 by David Beattie
 * Copyright 2011 by Pete Batard
 *
 * This file is based on the minix file system programs fsck and mkfs
 * written and copyrighted by Linus Torvalds <Linus.Torvalds@cs.helsinki.fi>
 *
 * %Begin-Header%
 * This file may be redistributed under the terms of the GNU Public
 * License.
 * %End-Header%
 */

/*
 * History:
 * 93/05/26	- Creation from e2fsck
 * 94/02/27	- Made a separate bad blocks checker
 * 99/06/30...99/07/26 - Added non-destructive write-testing,
 *                       configurable blocks-at-once parameter,
 * 			 loading of badblocks list to avoid testing
 * 			 blocks known to be bad, multiple passes to
 * 			 make sure that no new blocks are added to the
 * 			 list.  (Work done by David Beattie)
 * 11/12/04	- Windows/Rufus integration (Pete Batard)
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <time.h>
#include <setjmp.h>
#include <windows.h>

#include "rufus.h"
#include "badblocks.h"
#include "file.h"

/*
 *From e2fsprogs/lib/ext2fs/badblocks.c
 */
static errcode_t make_u32_list(int size, int num, __u32 *list, ext2_u32_list *ret)
{
	ext2_u32_list	bb;

	bb = calloc(1, sizeof(struct ext2_struct_u32_list));
	if (bb == NULL)
		return EXT2_ET_NO_MEMORY;
	bb->magic = EXT2_ET_MAGIC_BADBLOCKS_LIST;
	bb->size = size ? size : 10;
	bb->num = num;
	bb->list = malloc(sizeof(blk_t) * bb->size);
	if (bb->list == NULL) {
		free(bb);
		bb = NULL;
		return EXT2_ET_NO_MEMORY;
	}
	if (list)
		memcpy(bb->list, list, bb->size * sizeof(blk_t));
	else
		memset(bb->list, 0, bb->size * sizeof(blk_t));
	*ret = bb;
	return 0;
}

/*
 * This procedure creates an empty badblocks list.
 */
static errcode_t ext2fs_badblocks_list_create(ext2_badblocks_list *ret, int size)
{
	return make_u32_list(size, 0, 0, (ext2_badblocks_list *) ret);
}

/*
 * This procedure adds a block to a badblocks list.
 */
static errcode_t ext2fs_u32_list_add(ext2_u32_list bb, __u32 blk)
{
	int		i, j;
	__u32* old_bb_list = bb->list;

	EXT2_CHECK_MAGIC(bb, EXT2_ET_MAGIC_BADBLOCKS_LIST);

	if (bb->num >= bb->size) {
		bb->size += 100;
		bb->list = realloc(bb->list, bb->size * sizeof(__u32));
		if (bb->list == NULL) {
			bb->list = old_bb_list;
			bb->size -= 100;
			return EXT2_ET_NO_MEMORY;
		}
	}

	/*
	 * Add special case code for appending to the end of the list
	 */
	i = bb->num-1;
	if ((bb->num != 0) && (bb->list[i] == blk))
		return 0;
	if ((bb->num == 0) || (bb->list[i] < blk)) {
		bb->list[bb->num++] = blk;
		return 0;
	}

	j = bb->num;
	for (i=0; i < bb->num; i++) {
		if (bb->list[i] == blk)
			return 0;
		if (bb->list[i] > blk) {
			j = i;
			break;
		}
	}
	for (i=bb->num; i > j; i--)
		bb->list[i] = bb->list[i-1];
	bb->list[j] = blk;
	bb->num++;
	return 0;
}

static errcode_t ext2fs_badblocks_list_add(ext2_badblocks_list bb, blk_t blk)
{
	return ext2fs_u32_list_add((ext2_u32_list) bb, (__u32) blk);
}

/*
 * This procedure finds a particular block is on a badblocks
 * list.
 */
static int ext2fs_u32_list_find(ext2_u32_list bb, __u32 blk)
{
	int	low, high, mid;

	if (bb->magic != EXT2_ET_MAGIC_BADBLOCKS_LIST)
		return -1;

	if (bb->num == 0)
		return -1;

	low = 0;
	high = bb->num-1;
	if (blk == bb->list[low])
		return low;
	if (blk == bb->list[high])
		return high;

	while (low < high) {
		mid = ((unsigned)low + (unsigned)high)/2;
		if (mid == low || mid == high)
			break;
		if (blk == bb->list[mid])
			return mid;
		if (blk < bb->list[mid])
			high = mid;
		else
			low = mid;
	}
	return -1;
}

/*
 * This procedure tests to see if a particular block is on a badblocks
 * list.
 */
static int ext2fs_u32_list_test(ext2_u32_list bb, __u32 blk)
{
	if (ext2fs_u32_list_find(bb, blk) < 0)
		return 0;
	else
		return 1;
}

static int ext2fs_badblocks_list_test(ext2_badblocks_list bb, blk_t blk)
{
	return ext2fs_u32_list_test((ext2_u32_list) bb, (__u32) blk);
}

static errcode_t ext2fs_u32_list_iterate_begin(ext2_u32_list bb,
					ext2_u32_iterate *ret)
{
	ext2_u32_iterate iter;

	EXT2_CHECK_MAGIC(bb, EXT2_ET_MAGIC_BADBLOCKS_LIST);

	iter = malloc(sizeof(struct ext2_struct_u32_iterate));
	if (iter == NULL)
		return EXT2_ET_NO_MEMORY;

	iter->magic = EXT2_ET_MAGIC_BADBLOCKS_ITERATE;
	iter->bb = bb;
	iter->ptr = 0;
	*ret = iter;
	return 0;
}

static errcode_t ext2fs_badblocks_list_iterate_begin(ext2_badblocks_list bb,
					      ext2_badblocks_iterate *ret)
{
	return ext2fs_u32_list_iterate_begin((ext2_u32_list) bb,
					      (ext2_u32_iterate *) ret);
}


static int ext2fs_u32_list_iterate(ext2_u32_iterate iter, __u32 *blk)
{
	ext2_u32_list	bb;

	if (iter->magic != EXT2_ET_MAGIC_BADBLOCKS_ITERATE)
		return 0;

	bb = iter->bb;

	if (bb->magic != EXT2_ET_MAGIC_BADBLOCKS_LIST)
		return 0;

	if (iter->ptr < bb->num) {
		*blk = bb->list[iter->ptr++];
		return 1;
	}
	*blk = 0;
	return 0;
}

static int ext2fs_badblocks_list_iterate(ext2_badblocks_iterate iter, blk_t *blk)
{
	return ext2fs_u32_list_iterate((ext2_u32_iterate) iter,
				       (__u32 *) blk);
}


static void ext2fs_u32_list_iterate_end(ext2_u32_iterate iter)
{
	if (!iter || (iter->magic != EXT2_ET_MAGIC_BADBLOCKS_ITERATE))
		return;

	iter->bb = 0;
	free(iter);
}

static void ext2fs_badblocks_list_iterate_end(ext2_badblocks_iterate iter)
{
	ext2fs_u32_list_iterate_end((ext2_u32_iterate) iter);
}


/*
 * from e2fsprogs/misc/badblocks.c
 */
static int v_flag = 2;			/* verbose */
static int s_flag = 1;			/* show progress of test */
static int t_flag = 0;			/* number of test patterns */
static unsigned int *t_patts = NULL;	/* test patterns */
// TODO: set this from parameter
static unsigned int max_bb = 32;		/* Abort test if more than this number of bad blocks has been encountered */
static unsigned int d_flag = 0;		/* delay factor between reads */
static DWORD time_start;

#define T_INC 32

unsigned int sys_page_size = 4096;

static blk_t currently_testing = 0;
static blk_t num_blocks = 0;
static blk_t num_read_errors = 0;
static blk_t num_write_errors = 0;
static blk_t num_corruption_errors = 0;
static ext2_badblocks_list bb_list = NULL;
static FILE *out;
static blk_t next_bad = 0;
static ext2_badblocks_iterate bb_iter = NULL;

enum error_types { READ_ERROR, WRITE_ERROR, CORRUPTION_ERROR };

static __inline void *allocate_buffer(size_t size) {
#ifdef __MINGW32__
	return __mingw_aligned_malloc(size, sys_page_size);
#else 
	return _aligned_malloc(size, sys_page_size);
#endif
}

static __inline void free_buffer(void* p) {
#ifdef __MINGW32__
	__mingw_aligned_free(p);
#else
	_aligned_free(p);
#endif
}

/*
 * This routine reports a new bad block.  If the bad block has already
 * been seen before, then it returns 0; otherwise it returns 1.
 */
static int bb_output (blk_t bad, enum error_types error_type)
{
	errcode_t error_code;

	if (ext2fs_badblocks_list_test(bb_list, bad))
		return 0;

	uprintf("%lu\n", (unsigned long) bad);

	error_code = ext2fs_badblocks_list_add(bb_list, bad);
	if (error_code) {
		uprintf("Error %d adding to in-memory bad block list", error_code);
		exit (1);
	}

	/* kludge:
	   increment the iteration through the bb_list if
	   an element was just added before the current iteration
	   position.  This should not cause next_bad to change. */
	if (bb_iter && bad < next_bad)
		ext2fs_badblocks_list_iterate (bb_iter, &next_bad);

	if (error_type == READ_ERROR) {
	  num_read_errors++;
	} else if (error_type == WRITE_ERROR) {
	  num_write_errors++;
	} else if (error_type == CORRUPTION_ERROR) {
	  num_corruption_errors++;
	}
	return 1;
}

static float calc_percent(unsigned long current, unsigned long total) {
	float percent = 0.0;
	if (total <= 0)
		return percent;
	if (current >= total) {
		percent = 100.0f;
	} else {
		percent=(100.0f*(float)current/(float)total);
	}
	return percent;
}

static void print_status(void)
{
	static DWORD msecs = 0;
	DWORD time_end;

	// TODO: use GetTickCount64 on Vista and later
	time_end = GetTickCount();
	/* update status every second */
	if (time_end - time_start >= msecs) {
		uprintf("%6.2f%% done, %0.2f elapsed. "
					 "(%d/%d/%d errors)",
				   calc_percent((unsigned long) currently_testing,
						(unsigned long) num_blocks), 
				   (time_end - time_start)/1000.0,
				   num_read_errors,
				   num_write_errors,
				   num_corruption_errors);
		msecs += 1000;
	}
}

static void pattern_fill(unsigned char *buffer, unsigned int pattern,
			 size_t n)
{
	unsigned int	i, nb;
	unsigned char	bpattern[sizeof(pattern)], *ptr;

	if (pattern == (unsigned int) ~0) {
		for (ptr = buffer; ptr < buffer + n; ptr++) {
			(*ptr) = rand() % (1 << (8 * sizeof(char)));
		}
		PrintStatus("Testing with random pattern: ");
	} else {
		bpattern[0] = 0;
		for (i = 0; i < sizeof(bpattern); i++) {
			if (pattern == 0)
				break;
			bpattern[i] = pattern & 0xFF;
			pattern = pattern >> 8;
		}
		nb = i ? (i-1) : 0;
		for (ptr = buffer, i = nb; ptr < buffer + n; ptr++) {
			*ptr = bpattern[i];
			if (i == 0)
				i = nb;
			else
				i--;
		}
		PrintStatus("Testing with pattern 0x%02X", bpattern[i]);
	}
}

/*
 * Perform a read of a sequence of blocks; return the number of blocks
 *    successfully sequentially read.
 */
static int do_read (HANDLE hDrive, unsigned char * buffer, int tryout, int block_size,
		    blk_t current_block)
{
	long got;
//	DWORD tv1, tv2;
#define NANOSEC (1000000000L)
#define MILISEC (1000L)

#if 0
	printf("do_read: block %d, try %d\n", current_block, tryout);
#endif

	if (v_flag > 1)
		print_status();

	/* Try the read */
//	if (d_flag)
//		tv1 = GetTickCount();
	got = read_sectors(hDrive, block_size, current_block, tryout, buffer);
		// read (dev, buffer, tryout * block_size);
//	if (d_flag)
//		tv2 = GetTickCount();
	if (got < 0)
		got = 0;
	if (got & 511)
		uprintf("Weird value (%ld) in do_read\n", got);
	got /= block_size;
	if (d_flag && got == tryout) {
// TODO: either remove or update for Windows
#ifdef HAVE_NANOSLEEP
		struct timespec ts;
		ts.tv_sec = tv2.tv_sec - tv1.tv_sec;
		ts.tv_nsec = (tv2.tv_usec - tv1.tv_usec) * MILISEC;
		if (ts.tv_nsec < 0) {
			ts.tv_nsec += NANOSEC;
			ts.tv_sec -= 1;
		}
		/* increase/decrease the sleep time based on d_flag value */
		ts.tv_sec = ts.tv_sec * d_flag / 100;
		ts.tv_nsec = ts.tv_nsec * d_flag / 100;
		if (ts.tv_nsec > NANOSEC) {
			ts.tv_sec += ts.tv_nsec / NANOSEC;
			ts.tv_nsec %= NANOSEC;
		}
		if (ts.tv_sec || ts.tv_nsec)
			nanosleep(&ts, NULL);
#else
#ifdef HAVE_USLEEP
		struct timeval tv;
		tv.tv_sec = tv2.tv_sec - tv1.tv_sec;
		tv.tv_usec = tv2.tv_usec - tv1.tv_usec;
		tv.tv_sec = tv.tv_sec * d_flag / 100;
		tv.tv_usec = tv.tv_usec * d_flag / 100;
		if (tv.tv_usec > 1000000) {
			tv.tv_sec += tv.tv_usec / 1000000;
			tv.tv_usec %= 1000000;
		}
		if (tv.tv_sec)
			sleep(tv.tv_sec);
		if (tv.tv_usec)
			usleep(tv.tv_usec);
#endif
#endif
	}
	return got;
}

/*
 * Perform a write of a sequence of blocks; return the number of blocks
 *    successfully sequentially written.
 */
static int do_write(HANDLE hDrive, unsigned char * buffer, int tryout, int block_size,
		    unsigned long current_block)
{
	long got;

#if 0
	printf("do_write: block %lu, try %d\n", current_block, tryout);
#endif
	if (v_flag > 1)
		print_status();

	/* Try the write */
	got = write_sectors(hDrive, block_size, current_block, tryout, buffer);
	if (got < 0)
		got = 0;
	if (got & 511)
		uprintf("Weird value (%ld) in do_write\n", got);
	got /= block_size;
	return got;
}

static unsigned int test_ro (HANDLE hDrive, blk_t last_block,
			     int block_size, blk_t first_block,
			     unsigned int blocks_at_once)
{
	unsigned char * blkbuf;
	int tryout;
	int got;
	unsigned int bb_count = 0;
	errcode_t error_code;
	blk_t recover_block = ~0;

	error_code = ext2fs_badblocks_list_iterate_begin(bb_list,&bb_iter);
	if (error_code) {
		// TODO: set FormatStatus
		uprintf("errcode %d while beginning bad block list iteration\n", error_code);
		return 0;
	}
	do {
		ext2fs_badblocks_list_iterate (bb_iter, &next_bad);
	} while (next_bad && next_bad < first_block);

	if (t_flag) {
		blkbuf = allocate_buffer((blocks_at_once + 1) * block_size);
	} else {
		blkbuf = allocate_buffer(blocks_at_once * block_size);
	}
	if (!blkbuf)
	{
		// TODO: err
		uprintf("could not allocate buffers\n");
		return 0;
	}
	if (t_flag) {
		uprintf("Checking for bad blocks in read-only mode\n");
		pattern_fill(blkbuf + blocks_at_once * block_size,
			     t_patts[0], block_size);
	}
	tryout = blocks_at_once;
	currently_testing = first_block;
	num_blocks = last_block - 1;
	if (!t_flag && (s_flag || v_flag)) {
		// Printstatus
		uprintf("Checking for bad blocks (read-only test): \n");
	}
	while (currently_testing < last_block)
	{
		if (max_bb && bb_count >= max_bb) {
			if (s_flag || v_flag) {
				uprintf("Too many bad blocks, aborting test\n");
			}
			break;
		}
		if (next_bad) {
			if (currently_testing == next_bad) {
				/* fprintf (out, "%lu\n", nextbad); */
				ext2fs_badblocks_list_iterate (bb_iter, &next_bad);
				currently_testing++;
				continue;
			}
			else if (currently_testing + tryout > next_bad)
				tryout = next_bad - currently_testing;
		}
		if (currently_testing + tryout > last_block)
			tryout = last_block - currently_testing;
		got = do_read(hDrive, blkbuf, tryout, block_size, currently_testing);
		if (t_flag) {
			/* test the comparison between all the
			   blocks successfully read  */
			int i;
			for (i = 0; i < got; ++i)
				if (memcmp (blkbuf+i*block_size,
					    blkbuf+blocks_at_once*block_size,
					    block_size))
					bb_count += bb_output(currently_testing + i, CORRUPTION_ERROR);
		}
		if (got == 0 && tryout == 1)
			bb_count += bb_output(currently_testing++, READ_ERROR);
		currently_testing += got;
		if (got != tryout) {
			tryout = 1;
			if (recover_block == ~0)
				recover_block = currently_testing - got +
					blocks_at_once;
			continue;
		} else if (currently_testing == recover_block) {
			tryout = blocks_at_once;
			recover_block = ~0;
		}
	}
	num_blocks = 0;
//	alarm(0);
//	if (s_flag || v_flag)
//		fputs(_(done_string), stderr);

	fflush (stderr);
	free_buffer(blkbuf);

	ext2fs_badblocks_list_iterate_end(bb_iter);

	return bb_count;
}

static unsigned int test_rw(HANDLE hDrive, blk_t last_block, int block_size, blk_t first_block, unsigned int blocks_at_once)
{
	unsigned char *buffer, *read_buffer;
	const unsigned int patterns[] = {0xaa}; // {0xaa, 0x55, 0xff, 0x00};
	const unsigned int *pattern;
	int i, tryout, got, nr_pattern, pat_idx;
	unsigned int bb_count = 0;
	blk_t recover_block = ~0;

	buffer = allocate_buffer(2 * blocks_at_once * block_size);
	read_buffer = buffer + blocks_at_once * block_size;

	if (!buffer) {
		uprintf("Error while allocating buffers");
		return 0;
	}

	uprintf("Checking for bad blocks in read-write mode\n");
	uprintf("From block %lu to %lu\n", (unsigned long) first_block, (unsigned long) last_block - 1);
	if (t_flag) {
		pattern = t_patts;
		nr_pattern = t_flag;
	} else {
		pattern = patterns;
		nr_pattern = ARRAYSIZE(patterns);
	}
	// TODO: allow cancellation
	for (pat_idx = 0; pat_idx < nr_pattern; pat_idx++) {
		pattern_fill(buffer, pattern[pat_idx], blocks_at_once * block_size);
		num_blocks = last_block - 1;
		currently_testing = first_block;
//		if (s_flag && v_flag <= 1)
//			alarm_intr(SIGALRM);
		if (s_flag | v_flag)
			uprintf("Writing\n");
		tryout = blocks_at_once;
		while (currently_testing < last_block) {
			if (max_bb && bb_count >= max_bb) {
				if (s_flag || v_flag) {
					// TODO: this abort blows
					uprintf("Too many bad blocks, aborting test\n");
				}
				break;
			}
			if (currently_testing + tryout > last_block)
				tryout = last_block - currently_testing;
			got = do_write(hDrive, buffer, tryout, block_size, currently_testing);
			if (v_flag > 1)
				print_status();

			if (got == 0 && tryout == 1)
				bb_count += bb_output(currently_testing++, WRITE_ERROR);
			currently_testing += got;
			if (got != tryout) {
				tryout = 1;
				if (recover_block == ~0)
					recover_block = currently_testing -
						got + blocks_at_once;
				continue;
			} else if (currently_testing == recover_block) {
				tryout = blocks_at_once;
				recover_block = ~0;
			}
		}

		num_blocks = 0;
//		alarm (0);
//		if (s_flag | v_flag)
//			fputs(_(done_string), stderr);
		if (s_flag | v_flag)
			// TODO: status
			uprintf("Reading and comparing\n");
		num_blocks = last_block;
		currently_testing = first_block;
//		if (s_flag && v_flag <= 1)
//			alarm_intr(SIGALRM);

		tryout = blocks_at_once;
		while (currently_testing < last_block) {
			if (max_bb && bb_count >= max_bb) {
				if (s_flag || v_flag) {
					uprintf("Too many bad blocks, aborting test\n");
				}
				break;
			}
			if (currently_testing + tryout > last_block)
				tryout = last_block - currently_testing;
			got = do_read(hDrive, read_buffer, tryout, block_size,
				       currently_testing);
			if (got == 0 && tryout == 1)
				bb_count += bb_output(currently_testing++, READ_ERROR);
			currently_testing += got;
			if (got != tryout) {
				tryout = 1;
				if (recover_block == ~0)
					recover_block = currently_testing -
						got + blocks_at_once;
				continue;
			} else if (currently_testing == recover_block) {
				tryout = blocks_at_once;
				recover_block = ~0;
			}
			for (i=0; i < got; i++) {
				if (memcmp(read_buffer + i * block_size,
					   buffer + i * block_size,
					   block_size))
					bb_count += bb_output(currently_testing+i, CORRUPTION_ERROR);
			}
			if (v_flag > 1)
				print_status();
		}

		num_blocks = 0;
//		alarm (0);
//		if (s_flag | v_flag)
//			fputs(_(done_string), stderr);
	}
	free_buffer(buffer);
	return bb_count;
}

struct saved_blk_record {
	blk_t	block;
	int	num;
};

static unsigned int test_nd(HANDLE hDrive, blk_t last_block,
			     int block_size, blk_t first_block,
			     unsigned int blocks_at_once)
{
	unsigned char *blkbuf, *save_ptr, *test_ptr, *read_ptr;
	unsigned char *test_base, *save_base, *read_base;
	int tryout, i;
	const unsigned int patterns[] = { ~0 };
	const unsigned int *pattern;
	int nr_pattern, pat_idx;
	int got, used2, written;
	blk_t save_currently_testing;
	struct saved_blk_record *test_record;
	/* This is static to prevent being clobbered by the longjmp */
	static int num_saved;
	jmp_buf terminate_env;
	errcode_t error_code;
	unsigned long buf_used;
	static unsigned int bb_count;
	int granularity = blocks_at_once;
	blk_t recover_block = ~0;

	bb_count = 0;
	error_code = ext2fs_badblocks_list_iterate_begin(bb_list,&bb_iter);
	if (error_code) {
		uprintf("Error %d while beginning bad block list iteration", error_code);
		exit (1);
	}
	do {
		ext2fs_badblocks_list_iterate (bb_iter, &next_bad);
	} while (next_bad && next_bad < first_block);

	blkbuf = allocate_buffer(3 * blocks_at_once * block_size);
	test_record = malloc (blocks_at_once*sizeof(struct saved_blk_record));
	if (!blkbuf || !test_record) {
		uprintf("Error while allocating buffers");
		exit (1);
	}

	save_base = blkbuf;
	test_base = blkbuf + (blocks_at_once * block_size);
	read_base = blkbuf + (2 * blocks_at_once * block_size);

	num_saved = 0;

	if (v_flag) {
	    uprintf("Checking for bad blocks in non-destructive read-write mode\n");
	    uprintf("From block %lu to %lu\n",
		     (unsigned long) first_block,
		     (unsigned long) last_block - 1);
	}
	if (s_flag || v_flag > 1) {
		uprintf("Checking for bad blocks (non-destructive read-write test)\n");
	}
	if (setjmp(terminate_env)) {
		/*
		 * Abnormal termination by a signal is handled here.
		 */
//		signal (SIGALRM, SIG_IGN);
		uprintf("Interrupt caught, cleaning up\n");

		save_ptr = save_base;
		for (i=0; i < num_saved; i++) {
			do_write(hDrive, save_ptr, test_record[i].num,
				 block_size, test_record[i].block);
			save_ptr += test_record[i].num * block_size;
		}
		fflush (out);
		exit(1);
	}

	if (t_flag) {
		pattern = t_patts;
		nr_pattern = t_flag;
	} else {
		pattern = patterns;
		nr_pattern = sizeof(patterns) / sizeof(patterns[0]);
	}
	for (pat_idx = 0; pat_idx < nr_pattern; pat_idx++) {
		pattern_fill(test_base, pattern[pat_idx],
			     blocks_at_once * block_size);

		buf_used = 0;
		bb_count = 0;
		save_ptr = save_base;
		test_ptr = test_base;
		currently_testing = first_block;
		num_blocks = last_block - 1;
//		if (s_flag && v_flag <= 1)
//			alarm_intr(SIGALRM);

		while (currently_testing < last_block) {
			if (max_bb && bb_count >= max_bb) {
				if (s_flag || v_flag) {
					uprintf("Too many bad blocks, aborting test\n");
				}
				break;
			}
			tryout = granularity - buf_used;
			if (next_bad) {
				if (currently_testing == next_bad) {
					/* fprintf (out, "%lu\n", nextbad); */
					ext2fs_badblocks_list_iterate (bb_iter, &next_bad);
					currently_testing++;
					goto check_for_more;
				}
				else if (currently_testing + tryout > next_bad)
					tryout = next_bad - currently_testing;
			}
			if (currently_testing + tryout > last_block)
				tryout = last_block - currently_testing;
			got = do_read(hDrive, save_ptr, tryout, block_size,
				       currently_testing);
			if (got == 0) {
				if (recover_block == ~0)
					recover_block = currently_testing +
						blocks_at_once;
				if (granularity != 1) {
					granularity = 1;
					continue;
				}
				/* First block must have been bad. */
				bb_count += bb_output(currently_testing++, READ_ERROR);
				goto check_for_more;
			}

			/*
			 * Note the fact that we've saved this much data
			 * *before* we overwrite it with test data
			 */
			test_record[num_saved].block = currently_testing;
			test_record[num_saved].num = got;
			num_saved++;

			/* Write the test data */
			written = do_write(hDrive, test_ptr, got, block_size,
					    currently_testing);
			if (written != got)
				uprintf("Error %d during test data write, block %lu", errno,
					 (unsigned long) currently_testing +
					 written);

			buf_used += got;
			save_ptr += got * block_size;
			test_ptr += got * block_size;
			currently_testing += got;
			if (got != tryout) {
				if (recover_block == ~0)
					recover_block = currently_testing -
						got + blocks_at_once;
				continue;
			}

		check_for_more:
			/*
			 * If there's room for more blocks to be tested this
			 * around, and we're not done yet testing the disk, go
			 * back and get some more blocks.
			 */
			if ((buf_used != granularity) &&
			    (currently_testing < last_block))
				continue;

			if (currently_testing >= recover_block) {
				granularity = blocks_at_once;
				recover_block = ~0;
			}

			save_currently_testing = currently_testing;

			/*
			 * for each contiguous block that we read into the
			 * buffer (and wrote test data into afterwards), read
			 * it back (looping if necessary, to get past newly
			 * discovered unreadable blocks, of which there should
			 * be none, but with a hard drive which is unreliable,
			 * it has happened), and compare with the test data
			 * that was written; output to the bad block list if
			 * it doesn't match.
			 */
			used2 = 0;
			save_ptr = save_base;
			test_ptr = test_base;
			read_ptr = read_base;
			tryout = 0;

			while (1) {
				if (tryout == 0) {
					if (used2 >= num_saved)
						break;
					currently_testing = test_record[used2].block;
					tryout = test_record[used2].num;
					used2++;
				}

				got = do_read(hDrive, read_ptr, tryout,
					       block_size, currently_testing);

				/* test the comparison between all the
				   blocks successfully read  */
				for (i = 0; i < got; ++i)
					if (memcmp (test_ptr+i*block_size,
						    read_ptr+i*block_size, block_size))
						bb_count += bb_output(currently_testing + i, CORRUPTION_ERROR);
				if (got < tryout) {
					bb_count += bb_output(currently_testing + got, READ_ERROR);
					got++;
				}

				/* write back original data */
				do_write(hDrive, save_ptr, got,
					  block_size, currently_testing);
				save_ptr += got * block_size;

				currently_testing += got;
				test_ptr += got * block_size;
				read_ptr += got * block_size;
				tryout -= got;
			}

			/* empty the buffer so it can be reused */
			num_saved = 0;
			buf_used = 0;
			save_ptr = save_base;
			test_ptr = test_base;
			currently_testing = save_currently_testing;
		}
		num_blocks = 0;
//		alarm(0);
//		if (s_flag || v_flag > 1)
//			fputs(_(done_string), stderr);

	}
	free_buffer(blkbuf);
	free(test_record);

	ext2fs_badblocks_list_iterate_end(bb_iter);

	return bb_count;
}

int BadBlocks(HANDLE hPhysicalDrive, ULONGLONG disk_size, int block_size, int test_type)
{
	errcode_t error_code;
	unsigned int blocks_at_once = 64;
	unsigned int (*test_func)(HANDLE, blk_t,
				  int, blk_t,
				  unsigned int);
	int num_passes = 0;
	int passes_clean = 0;
	int bb_count;
	blk_t first_block = 0, last_block = (blk_t)disk_size/block_size;

	error_code = ext2fs_badblocks_list_create(&bb_list, 0);
	if (error_code) {
		uprintf("Error %d while creating in-memory bad blocks list", error_code);
		return -1;
	}

	switch(test_type) {
	case BADBLOCKS_RW:
		test_func = test_rw;
		break;
	case BADBLOCKS_ND:
		test_func = test_nd;
		break;
	default:
		test_func = test_ro;
		break;
	}
	time_start = GetTickCount();
	do {
		bb_count = test_func(hPhysicalDrive, last_block, block_size, first_block, blocks_at_once);
		if (bb_count)
			passes_clean = 0;
		else
			++passes_clean;

		if (v_flag)
			uprintf("Pass completed, %u bad blocks found. (%d/%d/%d errors)\n",
				bb_count, num_read_errors, num_write_errors, num_corruption_errors);

	} while (passes_clean < num_passes);
	free(t_patts);
	free(bb_list->list);
	free(bb_list);

	return bb_count;
}
