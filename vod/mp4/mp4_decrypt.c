#include "mp4_decrypt.h"

#if (VOD_HAVE_OPENSSL_EVP)

#include "mp4_aes_ctr.h"
#include "mp4_parser.h"
#include "../read_stream.h"

// constants
#define BUFFER_SIZE (65536)
#define MIN_BUFFER_SIZE (16)

// tyepdefs
typedef struct {
	// input params
	request_context_t* request_context;
	frames_source_t* frames_source;
	void* frames_source_context;
	bool_t reuse_buffers;
	bool_t use_subsamples;
	u_char key[MP4_AES_CTR_KEY_SIZE];

	// decryption state
	mp4_aes_ctr_state_t cipher;
	u_char* auxiliary_info_pos;
	u_char* auxiliary_info_end;
	uint16_t subsample_count;
	uint16_t clear_bytes;
	uint32_t encrypted_bytes;

	// input buffer
	u_char* input_pos;
	uint32_t input_size;
	bool_t frame_done;

	// output buffer
	u_char* output_start;
	u_char* output_end;
	u_char* output_pos;
} mp4_decrypt_state_t;

vod_status_t
mp4_decrypt_init(
	request_context_t* request_context,
	frames_source_t* frames_source,
	void* frames_source_context,
	u_char* key,
	media_encryption_t* encryption, 
	void** result)
{
	mp4_decrypt_state_t* state;
	vod_status_t rc;

	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_decrypt_init: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	vod_memzero(state, sizeof(*state));

	rc = mp4_aes_ctr_init(&state->cipher, request_context, key);
	if (rc != VOD_OK)
	{
		return rc;
	}

	vod_memcpy(state->key, key, sizeof(state->key));
	state->request_context = request_context;
	state->frames_source = frames_source;
	state->frames_source_context = frames_source_context;
	state->reuse_buffers = TRUE;

	state->auxiliary_info_pos = encryption->auxiliary_info;
	state->auxiliary_info_end = encryption->auxiliary_info_end;
	state->use_subsamples = encryption->use_subsamples;

	*result = state;

	return VOD_OK;
}

static void
mp4_decrypt_set_cache_slot_id(void* ctx, int cache_slot_id)
{
	mp4_decrypt_state_t* state = ctx;

	state->frames_source->set_cache_slot_id(state->frames_source_context, cache_slot_id);
}

static vod_status_t
mp4_decrypt_start_frame(void* ctx, input_frame_t* frame, uint64_t frame_offset)
{
	mp4_decrypt_state_t* state = ctx;
	vod_status_t rc;

	rc = state->frames_source->start_frame(state->frames_source_context, frame, frame_offset);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// get the iv
	if (state->auxiliary_info_pos + MP4_AES_CTR_IV_SIZE > state->auxiliary_info_end)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"mp4_decrypt_start_frame: failed to get iv from auxiliary info");
		return VOD_BAD_DATA;
	}

	mp4_aes_ctr_set_iv(&state->cipher, state->auxiliary_info_pos);
	state->auxiliary_info_pos += MP4_AES_CTR_IV_SIZE;

	if (!state->use_subsamples)
	{
		state->encrypted_bytes = UINT_MAX;
		return VOD_OK;
	}

	// get the subsample info
	if (state->auxiliary_info_pos + sizeof(uint16_t) + sizeof(cenc_sample_auxiliary_data_subsample_t) > state->auxiliary_info_end)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"mp4_decrypt_start_frame: failed to get subsample info from auxiliary info");
		return VOD_BAD_DATA;
	}

	read_be16(state->auxiliary_info_pos, state->subsample_count);
	if (state->subsample_count <= 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"mp4_decrypt_start_frame: invalid subsample count");
		return VOD_BAD_DATA;
	}

	read_be16(state->auxiliary_info_pos, state->clear_bytes);
	read_be32(state->auxiliary_info_pos, state->encrypted_bytes);

	state->subsample_count--;

	return VOD_OK;
}

static vod_status_t
mp4_decrypt_process(
	mp4_decrypt_state_t* state, 
	size_t size)
{
	u_char* dest = state->output_pos;
	u_char* src = state->input_pos;
	vod_status_t rc;
	size_t cur_size;

	while (size > 0)
	{
		if (state->clear_bytes <= 0 && state->encrypted_bytes <= 0)
		{
			// finished a subsample, read the next one
			if (state->subsample_count <= 0)
			{
				vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
					"mp4_decrypt_process: exhausted subsample bytes");
				return VOD_BAD_DATA;
			}

			if (state->auxiliary_info_pos + sizeof(cenc_sample_auxiliary_data_subsample_t) > state->auxiliary_info_end)
			{
				vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
					"mp4_decrypt_process: failed to get subsample info from auxiliary info");
				return VOD_BAD_DATA;
			}

			read_be16(state->auxiliary_info_pos, state->clear_bytes);
			read_be32(state->auxiliary_info_pos, state->encrypted_bytes);

			state->subsample_count--;
		}

		if (state->clear_bytes > 0)
		{
			// copy clear bytes
			cur_size = vod_min(state->clear_bytes, size);
			dest = vod_copy(dest, src, cur_size);
			src += cur_size;
			size -= cur_size;
			state->clear_bytes -= cur_size;
		}

		// decrypt encrypted bytes
		cur_size = vod_min(state->encrypted_bytes, size);
		rc = mp4_aes_ctr_process(&state->cipher, dest, src, cur_size);
		if (rc != VOD_OK)
		{
			return rc;
		}

		dest += cur_size;
		src += cur_size;
		size -= cur_size;
		state->encrypted_bytes -= cur_size;
	}

	state->output_pos = dest;
	state->input_pos = src;

	return VOD_OK;
}

static vod_status_t
mp4_decrypt_read(void* ctx, u_char** buffer, uint32_t* size, bool_t* frame_done)
{
	mp4_decrypt_state_t* state = ctx;
	vod_status_t rc;
	uint32_t cur_size;

	// make sure there is some output space
	if (state->output_pos + MIN_BUFFER_SIZE >= state->output_end)
	{
		if (!state->reuse_buffers || state->output_start == NULL)
		{
			state->output_start = vod_alloc(state->request_context->pool, BUFFER_SIZE);
			if (state->output_start == NULL)
			{
				vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
					"mp4_decrypt_read: vod_alloc failed");
				return VOD_ALLOC_FAILED;
			}
			state->output_end = state->output_start + BUFFER_SIZE;
		}

		state->output_pos = state->output_start;
	}

	// make sure there is some input buffer
	if (state->input_size <= 0)
	{
		rc = state->frames_source->read(
			state->frames_source_context, 
			&state->input_pos, 
			&state->input_size, 
			&state->frame_done);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	// process the min of input size and output size
	cur_size = state->output_end - state->output_pos;
	cur_size = vod_min(cur_size, state->input_size);
	state->input_size -= cur_size;

	*buffer = state->output_pos;
	*size = cur_size;
	*frame_done = state->input_size <= 0 ? state->frame_done : FALSE;

	rc = mp4_decrypt_process(state, cur_size);
	if (rc != VOD_OK)
	{
		return rc;
	}

	return VOD_OK;
}

static void
mp4_decrypt_disable_buffer_reuse(void* ctx)
{
	mp4_decrypt_state_t* state = ctx;

	state->reuse_buffers = FALSE;
}

u_char* 
mp4_decrypt_get_key(void* ctx)
{
	mp4_decrypt_state_t* state = ctx;

	return state->key;
}

void 
mp4_decrypt_get_original_source(
	void* ctx,
	frames_source_t** frames_source,
	void** frames_source_context)
{
	mp4_decrypt_state_t* state = ctx;

	*frames_source = state->frames_source;
	*frames_source_context = state->frames_source_context;
}

// globals
frames_source_t mp4_decrypt_frames_source = {
	mp4_decrypt_set_cache_slot_id,
	mp4_decrypt_start_frame,
	mp4_decrypt_read,
	mp4_decrypt_disable_buffer_reuse,
};

#else

// empty stubs
frames_source_t mp4_decrypt_frames_source = {
	NULL,
	NULL,
	NULL,
	NULL,
};

vod_status_t 
mp4_decrypt_init(
	request_context_t* request_context,
	frames_source_t* frames_source,
	void* frames_source_context,
	u_char* key,
	media_encryption_t* encryption,
	void** result)
{
	return VOD_UNEXPECTED;
}

#endif //(VOD_HAVE_OPENSSL_EVP)