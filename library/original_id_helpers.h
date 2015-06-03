#ifndef __ORIGINAL_ID_HELPERS_H
#define __ORIGINAL_ID_HELPERS_H
#include "blob.h"

// reorder ids in key
static inline void original_id_reorder(struct eblob_key *key_reord, const struct eblob_key *key)
{
	memcpy(key_reord->id, key->id + sizeof(int64_t), EBLOB_ID_SIZE - sizeof(int64_t));
	memcpy(key_reord->id + EBLOB_ID_SIZE - sizeof(int64_t), key->id, sizeof(int64_t));
}

#endif /* __ORIGINAL_ID_HELPERS_H */