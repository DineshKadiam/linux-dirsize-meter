#ifndef DS_HL_H
#define DS_HL_H
#include <sys/stat.h>

bool hl_already_seen(void*, dev_t st_dev, ino_t st_ino);

#endif
