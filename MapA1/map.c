#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "map.h"

// note: must be 2^n, default is 8
#define MIN_EMPTY_SLOTS (1 << 3)

#define MAGIC 0x1234567890123456


//
//	This map works so that it allocates an array of entities and whenever a key is writen it calculates a hash above
//	the key and uses this as the start index in the array. Then it checks for the first entity that has an empty hash
//	and places the provided entity values there.
//
//	If a key is removed, it will only delete the key, but leave the hash untouched. Therefore the slot stays reserved
//	for all keys with the same hash.
//
//	As soon as the allocation reaches the size it will optimize the map, so it will resize the map and re-index all
//	entities (without re-calculating the hashes).
//
//	This is very space efficient and on modern CPUs it is very effective because memory is only accessed linar and
//	there you can expect no L1 cache miss. However, the hash-map gets slow if it grows too big and it is sub-optimal
//	if being full.
//



//
//	This function calculates a modified FNV1 hash value. Modified in the way that it guarantees that the high bit is
//	always set, therefore the hash is always negative. This means as well it may never be zero, what is what we need
//	later on.
//
//	@param text
//		the zero terminated US-ASCII encoded string to hash.
//	@return
//		the 64-bit hash code above the provided string.
//
inline int64_t fnv1_hash( const char* text ) {
	int64_t hash = 0xCBF29CE484222325L;
	int i = 0;
	char c = *(text + i++);
	while (c != 0) {
		hash ^= c & 0xFF;
		hash *= 1099511628211L;
		c = *(text + i++);
	};
	return hash | 0x8000000000000000L;
}


//
//	This function is internally used to place a key-value pair into the map. It returns OK if that succeeded,
//	KEY_EXISTS is the parameter override is false (0) and this key is already contained in the map or
//	REQUIRES_OPTIMIZATION if there is no more space to add the key.
//
//	@param self
//		pointer to the map base structure.
//	@param key
//		pointer to the zero terminated key string.
//	@param value
//		pointer to the zero terminated value string.
//	@param hash
//		the modified FNV1 hash above the key.
//	@param override
//		if zero, then an existing key is not replaced, otherwise the value of an existing key is replaced.
//	@return
//		OK, KEY_EXISTS or REQUIRES_OPTIMIZATION.
//
inline int map_set(map_t* self, const char* key, const char* val, const int64_t hash, const int override ) {
	// the length of the items array and a bit-mask to mask the length
	const unsigned int length = self->capacity;
	const unsigned int mask = length - 1;

	// the items array
	map_entry_t* entries = self->entries;

	// the initial index where to start to search for an empty spot or for an already existing slot
	unsigned int i = hash & mask;
	unsigned int l = length;
	while (l-- > 0) {
		map_entry_t* entry = entries + i;

		// if the spot is free
		if (entry->hash==0) {
			// add the key, value and hash here
			entry->key = key;
			entry->value = val;
			entry->hash = hash;
			self->allocated++;
			self->size++;
			return OK;
		}

		// if the hash of this spot is the same as the provided one
		if (entry->hash == hash) {
			// if the entry was deleted and is therefore free
			if (entry->key==NULL) {
				// reset it
				entry->key = key;
				entry->value = val;
				// allocation stays the same, but the size increases
				self->size++;
				return OK;
			}

			// if the item is valid and has the same key
			if (entry->key==key || strcmp(entry->key, key)==0) {
				// if we should not override it
				if (override==0) return KEY_EXISTS;

				// replace the value (size and allocation stay unchanged)
				entry->value = val;
				return OK;
			}

			// otherwise this is a collision, we solve this collision by simple using the next available spot
		}

		i = (i+1) & mask;
	}
	return REQUIRES_OPTIMIZATION;
}


//
//	This function is internally used to optimize the map. An optimization will ensure that there is at least enough
//	space for MIN_EMPTY_SLOTS further new key-value pairs. This means it may increase or decrease the size of the map,
//	dependend at the requirements.
//
//	@param self
//		the pointer to the map struct.
//	@return
//		OK or SYS_ERROR.
//
int map_optimize(map_t* self) {
	// grab the old entries
	const unsigned int oldLength = self->capacity;
	const unsigned int oldSize = self->size;
	map_entry_t* oldEntries = self->entries;

	// the optimal size must be 2^n, ensuring that there are at least MIN_EMPTY_SLOTS free slots
	const unsigned int minNewSize = oldSize + MIN_EMPTY_SLOTS;
	unsigned int newLength = MIN_EMPTY_SLOTS;
	while (newLength < minNewSize) newLength <<= 1;
	const unsigned int bytes = sizeof(map_entry_t) * newLength;
	self->capacity = newLength;
	self->allocated = 0;
	self->size = 0;
	self->entries = memset(malloc(bytes),0,bytes);

	// re-add all items using the internal map_set method for performance reasons
	unsigned int i=0;
	map_entry_t* oldEntry = oldEntries;
	while (i++ < oldLength) {
		// if this key is not deleted
		if (oldEntry->key != NULL) {
			// add it again into the new resized map
			if (map_set(self, oldEntry->key, oldEntry->value, oldEntry->hash, 0) != 0) {
				// this must not happen
				return SYS_ERROR;
			}
		}

		oldEntry++;
	}

	// release the old memory
	free (oldEntries);
	return OK;
}

//
//	Searches for the provided key in this map and returns the index in the entries array if it finds the key or
//	-1 if the is not yet in the map.
//
//	@param self
//		the map to search in.
//	@param key
//		the key to search for.
//	@param hash
//		the modified FNV1 hash above the key.
//	@return
//		the index of the key or -1 if this key is not in the map.
//
int map_indexOf(map_t* self, const char* key, const int64_t hash) {
	// the length of the items array and a bit-mask to mask the length
	const unsigned int length = self->capacity;
	const unsigned int mask = length - 1;

	// the items array
	map_entry_t* entries = self->entries;

	// the hash of the key
	unsigned int i = hash & mask;
	unsigned int l = length;

	// search the key
	while (l-- > 0) {
		map_entry_t* entry = entries + i;

		// as soon as we hit an empty hash we can be sure that this key is not in the map
		if (entry->hash==0) return -1;

		// if the hash is the same as the one we're looking for
		if (entry->hash==hash) {
			// if the key is not deleted and the same as the one we're looking for
			if (entry->key!=NULL && (entry->key==key || strcmp(entry->key,key)==0)) {
				// we found it
				return i;
			}

			// otherwise this was a collision, continue to search
		}

		i = (i+1) & mask;
	}
	return -1;
}

//
//	Initializes the given map and allocates memory to the map.
//
//	@param self
//		the map to be initialized.
//
void map_init(map_t* self) {
	if (self==NULL) return;
	const unsigned int bytes = sizeof(map_entry_t) * MIN_EMPTY_SLOTS;
	self->magic = MAGIC;
	self->capacity = MIN_EMPTY_SLOTS;
	self->size = 0;
	self->allocated = 0;
	self->entries = memset(malloc(bytes),0,bytes);
}

//
//	Assigns the provided value to the provided key and returns OK if this was successfull or KEY_EXISTS if the key
//	exists already.
//
//	@param self
//		the map in which to put the key-value pair.
//	@param key
//		the key.
//	@param value
//		the value.
//	@return
//		OK if the key-value pair was inserted or KEY_EXISTS is the key is already set.
//
int map_put(map_t* self, const char* key, const char* val) {
	if (self==NULL || key==NULL) return NULL_POINTER;
	if (self->magic != MAGIC) return NOT_INITIALIZED;

	const int64_t hash = fnv1_hash(key);
	const int i = map_indexOf(self,key,hash);
	if (i >= 0) return KEY_EXISTS;

	// if there is not enough space to add another key-value pair, make space
	if (self->allocated >= self->capacity) map_optimize(self);

	// add the key
	return map_set(self,key,val,hash,0);
}

//
//	Looks up for the provided key and returns its value.
//
//	@param self
//		the map into which to look for the key.
//	@param key
//		the key to search.
//	@return
//		the value (which might be null either!) of the key or null is no such key exists in the map.
//
const char* map_get(map_t* self, const char* key) {
	if (self==NULL || key==NULL || self->magic != MAGIC) return NULL;

	const int64_t hash = fnv1_hash(key);
	const int i = map_indexOf(self,key,hash);
	return i < 0 ? NULL : self->entries[i].value;
}

//
//	Returns the amount of key-value pairs stored in the provided map.
//
//	@param self
//		the map for which to return the size.
//	@return
//		the amount of key-value pairs stored in the provided map.
//
int map_size(map_t* self) {
	if (self==NULL || self->magic != MAGIC) return 0;
    return self->size;
}

//
//	Removes the key-value pair with the given key from the map.
//
//	@param self
//		the map from which to remove the key-value pair.
//	@param key
//		the key of the entity to be removed.
//	@return
//		OK if the key-value pair was removed successfully or NO_KEY_EXISTS if the provided map doesn't contain such
//		a key.
//
int map_remove(map_t* self, const char* key) {
	if (self==NULL || key==NULL) return NULL_POINTER;
	if (self->magic != MAGIC) return NOT_INITIALIZED;

	const int64_t hash = fnv1_hash(key);
	const int i = map_indexOf(self,key,hash);
	if (i >= 0) {
		self->entries[i].key = NULL;
		self->size--;
		printf("\n a node was deleted\n");
		return OK;
	}
	return NO_KEY_EXISTS;
}

//
//
//
int map_serialize(map_t* self, FILE* stream) {
	return ERR_NOT_IMPLEMENTED;
}

//
//
//
int map_deserialize(map_t* self, FILE* stream) {
	return ERR_NOT_IMPLEMENTED;
}

//
//	Frees the memory allocated for the map.
//
//	@param self
//		the map to destroy and for which to release memory.
//
void map_destroy(map_t* self) {
	if (self==NULL) return;
	if (self->magic != MAGIC) return;

	free(self->entries);
	self->entries = NULL;
	self->magic = 0;
	printf("\n A destroy has been done.\n");
}

//
//	Test.
//
int main() {
	map_t* m = malloc(sizeof(map_t));
	map_init(m);
	map_put(m, "a", "1");
	map_put(m, "b", "2");
	map_put(m, "c", "3");
	map_put(m, "d", "4");
	map_put(m, "e", "5");
	map_put(m, "f", "This is testing the map_put functions");
	map_put(m,"tester","tested");

	map_remove(m, "b");
	printf("Size: %d\n", map_size(m));
	printf("Read key 'a': %s \n", map_get(m,"a"));
	printf("Read key 'tester': %s \n", map_get(m,"tester"));
	map_remove(m, "tester");
	printf("Read key 'f': %s \n", map_get(m,"f"));
	return OK;
}

