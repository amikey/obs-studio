/******************************************************************************
    Copyright (C) 2018 by Hugh Bailey <obs.jim@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "obs-internal.h"

static void *gpu_encode_thread(void *param)
{
	obs_encoder_t *encoder = param;

	os_set_thread_name("obs-encoder: gpu encode thread");

	while (os_sem_wait(encoder->gpu_encode_semaphore) == 0) {
		if (os_atomic_load_bool(&encoder->gpu_encode_stop))
			break;

		do_encode(encoder, NULL);
	}

	return NULL;
}

bool init_gpu_encode(struct obs_encoder *encoder)
{
	encoder->gpu_encode_stop = false;

	if (os_sem_init(&encoder->gpu_encode_semaphore, 0) != 0)
		return false;
	if (pthread_create(&encoder->gpu_encode_thread, NULL,
				gpu_encode_thread, encoder) != 0)
		return false;

	encoder->gpu_encode_thread_initialized = true;
	return true;
}

void free_gpu_encode(struct obs_encoder *encoder)
{
	if (encoder->gpu_encode_thread_initialized) {
		os_atomic_set_bool(&encoder->gpu_encode_stop, true);
		os_sem_post(encoder->gpu_encode_semaphore);
		pthread_join(encoder->gpu_encode_thread, NULL);
		encoder->gpu_encode_thread_initialized = false;
	}
	if (encoder->gpu_encode_semaphore) {
		os_sem_destroy(encoder->gpu_encode_semaphore);
		encoder->gpu_encode_semaphore = NULL;
	}
}
