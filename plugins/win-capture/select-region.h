#pragma once

typedef void (*select_region_cb)(void *data, POINT pos, SIZE size);

extern bool select_region_begin(select_region_cb cb, RECT start_region,
		void *data);
extern void select_region_free(void);
