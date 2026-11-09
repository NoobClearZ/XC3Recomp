// Virtual RomFS image over a folder (runtime/services/romfs_virtual.c).  [XCDE]
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#define O_BINARY_FLAG 0x8000  // _O_BINARY
#else
#define O_BINARY_FLAG 0
#endif

bool rvfs_init(const char *dir, char *why, size_t why_size);
uint64_t rvfs_size(void);
int64_t rvfs_read(uint8_t *dst, uint64_t size, uint64_t off);
const char *rvfs_file_at(uint64_t off);

// [XC3] Another folder served as its own image (the add-on content: services/fs.c). NULL (why says it) on failure.
typedef struct Rvfs Rvfs;
Rvfs *rvfs_open(const char *dir, char *why, size_t why_size);
uint64_t rvfs_isize(const Rvfs *v);
int64_t rvfs_iread(Rvfs *v, uint8_t *dst, uint64_t size, uint64_t off);
const char *rvfs_ifile_at(const Rvfs *v, uint64_t off);
