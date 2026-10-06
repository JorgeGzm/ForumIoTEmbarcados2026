/*
 * Copyright (c) 2026 GZM Embarcados
 */

#ifndef GZM_ACCESS_LEVEL_H
#define GZM_ACCESS_LEVEL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/** Access levels, from the lowest to the highest privilege. */
enum access_level {
	ACC_LEVEL_NONE = (uint8_t)0x00,
	ACC_LEVEL_USER,
	ACC_LEVEL_SUBMANAGER,
	ACC_LEVEL_MANAGER,
	ACC_LEVEL_ADMIN,
	ACC_LEVEL_ENGINEER,
	ACC_LEVEL_FACTORY,
	ACC_LEVEL_MAX,
};

#ifdef __cplusplus
}
#endif

#endif /* GZM_ACCESS_LEVEL_H */
