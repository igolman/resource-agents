/******************************************************************************
*******************************************************************************
**
**  Copyright (C) Sistina Software, Inc.  1997-2003  All rights reserved.
**  Copyright (C) 2004-2005 Red Hat, Inc.  All rights reserved.
**
**  This copyrighted material is made available to anyone wishing to use,
**  modify, copy, or redistribute it subject to the terms and conditions
**  of the GNU General Public License v.2.
**
*******************************************************************************
******************************************************************************/

#include <stdio.h>
#include "fsck_incore.h"
#include "fsck.h"
#include "ondisk.h"
#include "fs_bits.h"
#include "bio.h"

#ifdef DEBUG
int rgrp_countbits(unsigned char *buffer, unsigned int buflen,
		   uint32_t *bit_array)
{
	unsigned char *byte, *end;
	unsigned int bit;
	unsigned char state;

	byte = buffer;
	bit = 0;
	end = buffer + buflen;

	while (byte < end){
		state = ((*byte >> bit) & GFS_BIT_MASK);
		switch (state) {
		case GFS_BLKST_FREE:
			bit_array[0]++;
			break;
		case GFS_BLKST_USED:
			bit_array[1]++;
			break;
		case GFS_BLKST_FREEMETA:
			bit_array[2]++;
			break;
		case GFS_BLKST_USEDMETA:
			bit_array[3]++;
			break;
		default:
			log_err("Invalid state %d found at byte %u, bit %u\n",
				state, byte, bit);
			return -1;
			break;
		}

		bit += GFS_BIT_SIZE;
		if (bit >= 8){
			bit = 0;
			byte++;
		}
	}
	return 0;
}

int fsck_countbits(struct fsck_sb *sbp, uint64_t start_blk, uint64_t count,
		   uint32_t *bit_array)
{
	uint64_t i;
	struct block_query q;
	for(i = start_blk; i < start_blk+count; i++) {
		block_check(sbp->bl, i, &q);
		switch(q.block_type) {
		case block_free:
			bit_array[0]++;
			break;
		case block_used:
			bit_array[1]++;
			break;
		case meta_free:
		case meta_inval:
			bit_array[2]++;
			break;
		case indir_blk:
		case inode_dir:
		case inode_file:
		case leaf_blk:
		case journal_blk:
		case meta_other:
		case meta_eattr:
			bit_array[3]++;
			break;
		default:
			log_err("Invalid state %d found at block%"PRIu64"\n",
				mark, i);
			return -1;
			break;
		}
	}
	return 0;
}


int count_bmaps(struct fsck_rgrp *rgp)
{
	uint32_t i;
	uint32_t bit_array_rgrp[4] = {0};
	uint32_t bit_array_fsck[4] = {0};
	fs_bitmap_t *bits;

	for(i = 0; i < rgp->rd_ri.ri_length; i++) {
		bits = &rgp->rd_bits[i];
		rgrp_countbits(BH_DATA(rgp->rd_bh[i]) + bits->bi_offset,
			       bits->bi_len, bit_array_rgrp);
	}
	log_err("rgrp: free %u used %u meta_free %u meta_used %u\n",
		bit_array_rgrp[0], bit_array_rgrp[1],
		bit_array_rgrp[2], bit_array_rgrp[3]);
	fsck_countbits(rgp->rd_sbd, rgp->rd_ri.ri_data1,
		       rgp->rd_ri.ri_data, bit_array_fsck);
	log_err("fsck: free %u used %u meta_free %u meta_used %u\n",
		bit_array_fsck[0], bit_array_fsck[1],
		bit_array_fsck[2], bit_array_fsck[3]);

	for(i = 0; i < 4; i++) {
		if(bit_array_rgrp[i] != bit_array_fsck[i]) {
			log_err("Bitmap count in index %d differ: "
				"ondisk %d, fsck %d\n", i,
				bit_array_rgrp[i], bit_array_fsck[i]);
		}
	}
	return 0;
}
#endif /* DEBUG */

int convert_mark(enum mark_block mark, uint32_t *count)
{
	switch(mark) {
	case block_free:
		count[0]++;
		return GFS_BLKST_FREE;

	case block_used:
		return GFS_BLKST_USED;

	case meta_free:
		count[4]++;
		return GFS_BLKST_FREEMETA;

	case meta_inval:
		/* Convert invalid metadata to free blocks */
		count[0]++;
		return GFS_BLKST_FREE;

	case inode_dir:
	case inode_file:
	case inode_lnk:
	case inode_blk:
	case inode_chr:
	case inode_fifo:
	case inode_sock:
		count[1]++;
		return GFS_BLKST_USEDMETA;

	case indir_blk:
	case leaf_blk:
	case journal_blk:
	case meta_other:
	case meta_eattr:
		count[3]++;
		return GFS_BLKST_USEDMETA;

	default:
		log_err("Invalid state %d found\n", mark);
		return -1;

	}
	return -1;
}


int check_block_status(struct fsck_sb *sbp, char *buffer, unsigned int buflen,
		       uint64_t *rg_block, uint64_t rg_data, uint32_t *count)
{
	unsigned char *byte, *end;
	unsigned int bit;
	unsigned char rg_status, block_status;
	struct block_query q;
	uint64_t block;

	byte = buffer;
	bit = 0;
	end = buffer + buflen;

	while(byte < end) {
		rg_status = ((*byte >> bit) & GFS_BIT_MASK);
		block = rg_data + *rg_block;
		block_check(sbp->bl, block, &q);

		block_status = convert_mark(q.block_type, count);

		if(rg_status != block_status) {
			log_err("ondisk and fsck bitmaps differ at block %"
				PRIu64"\n", block);
			log_debug("Ondisk is %u - FSCK thinks it is %u (%u)\n",
				  rg_status, block_status, q.block_type);
			if(query(sbp, "Fix bitmap for block %"PRIu64"? (y/n) ",
				 block)) {
				if(fs_set_bitmap(sbp, block, block_status)) {
					log_err("Failed.\n");
				}
				else {
					log_err("Suceeded.\n");
				}
			} else {
				log_err("Bitmap at block %"PRIu64
					" left inconsistent\n", block);
			}
		}
		(*rg_block)++;
		bit += GFS_BIT_SIZE;
		if (bit >= 8){
			bit = 0;
			byte++;
		}
	}

	return 0;
}


int update_rgrp(struct fsck_rgrp *rgp, uint32_t *count)
{
	int update = 0;
	uint32_t i;
	fs_bitmap_t *bits;
	uint64_t rg_block = 0;

	for(i = 0; i < rgp->rd_ri.ri_length; i++) {
		bits = &rgp->rd_bits[i];

		/* update the bitmaps */
		check_block_status(rgp->rd_sbd,
				   BH_DATA(rgp->rd_bh[i]) + bits->bi_offset,
				   bits->bi_len, &rg_block,
				   rgp->rd_ri.ri_data1, count);
	}

	/* Compare the rgrps counters with what we found */
	/* actually adjust counters and write out to disk */
	if(rgp->rd_rg.rg_free != count[0]) {
		log_err("free count inconsistent: is %u should be %u\n",
			rgp->rd_rg.rg_free, count[0] );
		rgp->rd_rg.rg_free = count[0];
		update = 1;
	}
	if(rgp->rd_rg.rg_useddi != count[1]) {
		log_err("used inode count inconsistent: is %u should be %u\n",
			rgp->rd_rg.rg_useddi, count[1]);
		rgp->rd_rg.rg_useddi = count[1];
		update = 1;
	}
	if(rgp->rd_rg.rg_freedi != count[2]) {
		log_err("free inode count inconsistent: is %u should be %u\n",
			rgp->rd_rg.rg_freedi, count[2]);
		rgp->rd_rg.rg_freedi = count[2];
		update = 1;
	}
	if(rgp->rd_rg.rg_usedmeta != count[3]) {
		log_err("used meta count inconsistent: is %u should be %u\n",
			rgp->rd_rg.rg_usedmeta, count[3]);
		rgp->rd_rg.rg_usedmeta = count[3];
		update = 1;
	}
	if(rgp->rd_rg.rg_freemeta != count[4]) {
		log_err("free meta count inconsistent: is %u should be %u\n",
			rgp->rd_rg.rg_freemeta, count[4]);
		rgp->rd_rg.rg_freemeta = count[4];
		update = 1;
	}

	if(update) {
		if(query(rgp->rd_sbd,
			 "Update resource group counts? (y/n) ")) {
		/* write out the rgrp */
			gfs_rgrp_out(&rgp->rd_rg, BH_DATA(rgp->rd_bh[0]));
			write_buf(rgp->rd_sbd, rgp->rd_bh[0], 0);
		} else {
			log_err("Resource group counts left inconsistent\n");
		}
	}

	return 0;
}

/**
 * pass5 - check resource groups
 *
 * fix free block maps
 * fix used inode maps
 */
int pass5(struct fsck_sb *sbp, struct options *opts)
{
	osi_list_t *tmp;
	struct fsck_rgrp *rgp = NULL;
	uint32_t count[5];
	uint64_t rg_count = 0;

	/* Reconcile RG bitmaps with fsck bitmap */
	for(tmp = sbp->rglist.next; tmp != &sbp->rglist; tmp = tmp->next){
		log_info("Updating Resource Group %"PRIu64"\n", rg_count);
		memset(count, 0, sizeof(*count) * 5);
		rgp = osi_list_entry(tmp, struct fsck_rgrp, rd_list);

		if(fs_rgrp_read(rgp)){
			stack;
			return -1;
		}
		/* Compare the bitmaps and report the differences */
		update_rgrp(rgp, count);
		rg_count++;
		fs_rgrp_relse(rgp);
	}
	/* Fix up superblock info based on this - don't think there's
	 * anything to do here... */


	return 0;
}
