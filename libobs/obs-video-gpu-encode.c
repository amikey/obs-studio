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

static void *gpu_encode_thread(void *unused)
{
	struct obs_core_video *video = &obs->video;
	uint64_t interval = video_output_get_frame_time(obs->video.video);
	DARRAY(obs_encoder_t *) encoders;

	UNUSED_PARAMETER(unused);
	da_init(encoders);

	os_set_thread_name("obs gpu encode thread");

	while (os_sem_wait(video->gpu_encode_semaphore) == 0) {
		struct obs_tex_frame tf;
		uint64_t timestamp;

		if (os_atomic_load_bool(&video->gpu_encode_stop))
			break;

		/* -------------- */

		pthread_mutex_lock(&video->gpu_encoder_mutex);

		circlebuf_pop_front(&video->gpu_encoder_queue, &tf, sizeof(tf));
		timestamp = tf.timestamp;

		if (--tf.count) {
			tf.timestamp += interval;
			circlebuf_push_front(&video->gpu_encoder_queue,
					&tf, sizeof(tf));
		}

		for (size_t i = 0; i < video->gpu_encoders.num; i++) {
			obs_encoder_t *encoder = video->gpu_encoders.array[i];
			da_push_back(encoders, &encoder);
			obs_encoder_addref(encoder);
		}

		pthread_mutex_unlock(&video->gpu_encoder_mutex);

		/* -------------- */

		for (size_t i = 0; i < encoders.num; i++) {
			struct encoder_packet pkt = {0};
			bool received = false;
			bool success;

			obs_encoder_t *encoder = encoders.array[i];
			struct obs_encoder *pair = encoder->paired_encoder;

			pkt.timebase_num = encoder->timebase_num;
			pkt.timebase_den = encoder->timebase_den;
			pkt.encoder = encoder;

			if (!encoder->first_received && pair) {
				if (!pair->first_received ||
				    pair->first_raw_ts > timestamp) {
					continue;
				}
			}

			if (!encoder->start_ts)
				encoder->start_ts = timestamp;

			success = encoder->info.encode_texture(
					encoder->context.data, tf.handle,
					encoder->cur_pts, &pkt, &received);
			send_off_encoder_packet(encoder, success, received,
					&pkt);
			obs_encoder_release(encoder);

			encoder->cur_pts += encoder->timebase_num;

			if (success)
				tf.refs++;
		}

		da_resize(encoders, 0);

		/* -------------- */

		pthread_mutex_lock(&video->gpu_encoder_mutex);
		if (!tf.count) {
			circlebuf_push_back(
					&video->gpu_encoder_avail_queue,
					&tf, sizeof(tf));
		}
		pthread_mutex_unlock(&video->gpu_encoder_mutex);
	}

	da_free(encoders);
	return NULL;
}

bool init_gpu_encoding(struct obs_core_video *video)
{
#ifdef _WIN32
	struct obs_video_info *ovi = &video->ovi;

	video->gpu_encode_stop = false;

	circlebuf_reserve(&video->gpu_encoder_avail_queue, NUM_ENCODE_TEXTURES);
	for (size_t i = 0; i < NUM_ENCODE_TEXTURES; i++) {
		gs_texture_t *tex;
		gs_texture_t *tex_uv;

		gs_texture_create_nv12(
				&tex, &tex_uv,
				ovi->output_width, ovi->output_height,
				GS_RENDER_TARGET | GS_SHARED_TEX);
		if (!tex) {
			return false;
		}

		uint32_t handle = gs_texture_get_shared_handle(tex);

		struct obs_tex_frame frame = {
			.tex = tex,
			.tex_uv = tex_uv,
			.handle = handle
		};

		circlebuf_push_back(&video->gpu_encoder_avail_queue, &frame,
				sizeof(frame));
	}

	if (os_sem_init(&video->gpu_encode_semaphore, 0) != 0)
		return false;
	if (pthread_create(&video->gpu_encode_thread, NULL,
				gpu_encode_thread, NULL) != 0)
		return false;

	video->gpu_encode_thread_initialized = true;
	return true;
#else
	UNUSED_PARAMETER(video);
	return false;
#endif
}

void free_gpu_encoding(struct obs_core_video *video)
{
	if (video->gpu_encode_thread_initialized) {
		os_atomic_set_bool(&video->gpu_encode_stop, true);
		os_sem_post(video->gpu_encode_semaphore);
		pthread_join(video->gpu_encode_thread, NULL);
		video->gpu_encode_thread_initialized = false;
	}
	if (video->gpu_encode_semaphore) {
		os_sem_destroy(video->gpu_encode_semaphore);
		video->gpu_encode_semaphore = NULL;
	}

#define free_circlebuf(x) \
	do { \
		while (x.size) { \
			struct obs_tex_frame frame; \
			circlebuf_pop_front(&x, &frame, sizeof(frame)); \
			gs_texture_destroy(frame.tex); \
			gs_texture_destroy(frame.tex_uv); \
		} \
		circlebuf_free(&x); \
	} while (false)

	free_circlebuf(video->gpu_encoder_queue);
	free_circlebuf(video->gpu_encoder_avail_queue);
#undef free_circlebuf

	for (size_t i = 0; i < video->gpu_encoder_active_queue.num; i++) {
		struct obs_tex_frame *tf;
		tf = &video->gpu_encoder_active_queue.array[i];

		gs_texture_destroy(tf->tex);
		gs_texture_destroy(tf->tex_uv);
	}
	da_free(video->gpu_encoder_active_queue);
}

void obs_unqueue_encode_texture(uint32_t handle)
{
	struct obs_core_video *video = &obs->video;
	size_t start = 0;

	pthread_mutex_lock(&video->gpu_encoder_mutex);
	for (size_t i = 0; i < video->gpu_encoder_active_queue.num; i++) {
		struct obs_tex_frame *tf;
		tf = &video->gpu_encoder_active_queue.array[i];

		if (tf->handle == handle) {
			if (!--tf->refs) {
				circlebuf_push_back(
						&video->gpu_encoder_avail_queue,
						tf, sizeof(*tf));
				da_erase(video->gpu_encoder_active_queue, i);
			}
			break;
		}
	}
	pthread_mutex_unlock(&video->gpu_encoder_mutex);
}
