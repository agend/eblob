#ifndef __ORIGINAL_ID_HELPERS_H
#define __ORIGINAL_ID_HELPERS_H
#include "blob.h"

// reorder ids in key
static inline void original_id_reorder(struct eblob_key *key_reord, const struct eblob_key *key)
{
	memcpy(key_reord->id, key->id + sizeof(int64_t), EBLOB_ID_SIZE - sizeof(int64_t));
	memcpy(key_reord->id + EBLOB_ID_SIZE - sizeof(int64_t), key->id, sizeof(int64_t));
}

static int eblob_key_range_compare_original_id_mod(const void *k, const void *r) {
	const struct eblob_key *key = k;
	const struct eblob_index_block *range = r;

	// #### BEGIN: passthrought_original_id modifications ####
	// reorder ids in key
	struct eblob_key key_reord;
	original_id_reorder(&key_reord, key);
	key = &key_reord;
	// #### END: passthrought_original_id modifications ####

	if (eblob_id_cmp(key->id, range->start_key.id) < 0)
		return -1;
	if (eblob_id_cmp(key->id, range->end_key.id) > 0)
		return 1;
	return 0;
}

#endif /* __ORIGINAL_ID_HELPERS_H */