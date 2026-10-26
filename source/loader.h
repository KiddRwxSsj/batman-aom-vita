#ifndef LOADER_H
#define LOADER_H

#include <so_util/so_util.h>

#define OBB_NAME "main.3030.com.wb.goog.batman.brawler2013.obb"
#define SO_PATH  DATA_PATH "libUnrealEngine3.so"
#define OBB_PATH DATA_PATH OBB_NAME

extern so_module ue3_mod;

void loader_init(void);
void patch_game(void);
uintptr_t ue3_sym(const char *name);
void *gnu_unwind_find_exidx(uintptr_t pc, int *count);

#endif
