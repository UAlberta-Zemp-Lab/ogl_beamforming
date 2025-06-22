/* See LICENSE for license details. */
/* TODO(rnp):
 * [ ]: refactor: compute shader timers should be generated based on the pipeline stage limit
 * [ ]: reinvestigate ring buffer raw_data_ssbo
 *      - to minimize latency the main thread should manage the subbuffer upload so that the
 *        compute thread can just keep computing. This way we can keep the copmute thread busy
 *        with work while we image.
 *      - In particular we will potentially need multiple GPUComputeContexts so that we
 *        can overwrite one while the other is in use.
 *      - make use of glFenceSync to guard buffer uploads
 * [ ]: BeamformWorkQueue -> BeamformerWorkQueue
 * [ ]: bug: re-beamform on shader reload
 * [ ]: need to keep track of gpu memory in some way
 *      - want to be able to store more than 16 2D frames but limit 3D frames
 *      - maybe keep track of how much gpu memory is committed for beamformed images
 *        and use that to determine when to loop back over existing textures
 *      - to do this maybe use a circular linked list instead of a flat array
 *      - then have a way of querying how many frames are available for a specific point count
 * [ ]: bug: reinit cuda on hot-reload
 */

#include "beamformer.h"
#include "beamformer_work_queue.c"

global f32 dt_for_frame;
global u32 cycle_t;

#ifndef _DEBUG
#define start_renderdoc_capture(...)
#define end_renderdoc_capture(...)
#else
global renderdoc_start_frame_capture_fn *start_frame_capture;
global renderdoc_end_frame_capture_fn   *end_frame_capture;
#define start_renderdoc_capture(gl) if (start_frame_capture) start_frame_capture(gl, 0)
#define end_renderdoc_capture(gl)   if (end_frame_capture)   end_frame_capture(gl, 0)
#endif

typedef struct {
	BeamformComputeFrame *frames;
	u32 capacity;
	u32 offset;
	u32 cursor;
	u32 needed_frames;
} ComputeFrameIterator;

function uv3
make_valid_test_dim(u32 in[3])
{
	uv3 result;
	result.E[0] = MAX(in[0], 1);
	result.E[1] = MAX(in[1], 1);
	result.E[2] = MAX(in[2], 1);
	return result;
}

function ComputeFrameIterator
compute_frame_iterator(BeamformerCtx *ctx, u32 start_index, u32 needed_frames)
{
	start_index = start_index % ARRAY_COUNT(ctx->beamform_frames);

	ComputeFrameIterator result;
	result.frames        = ctx->beamform_frames;
	result.offset        = start_index;
	result.capacity      = ARRAY_COUNT(ctx->beamform_frames);
	result.cursor        = 0;
	result.needed_frames = needed_frames;
	return result;
}

function BeamformComputeFrame *
frame_next(ComputeFrameIterator *bfi)
{
	BeamformComputeFrame *result = 0;
	if (bfi->cursor != bfi->needed_frames) {
		u32 index = (bfi->offset + bfi->cursor++) % bfi->capacity;
		result    = bfi->frames + index;
	}
	return result;
}

function void
alloc_beamform_frame(GLParams *gp, BeamformFrame *out, uv3 out_dim, s8 name, Arena arena)
{
	out->dim.x = MAX(1, out_dim.x);
	out->dim.y = MAX(1, out_dim.y);
	out->dim.z = MAX(1, out_dim.z);

	if (gp) {
		out->dim.x = MIN(out->dim.x, gp->max_3d_texture_dim);
		out->dim.y = MIN(out->dim.y, gp->max_3d_texture_dim);
		out->dim.z = MIN(out->dim.z, gp->max_3d_texture_dim);
	}

	/* NOTE: allocate storage for beamformed output data;
	 * this is shared between compute and fragment shaders */
	u32 max_dim = MAX(out->dim.x, MAX(out->dim.y, out->dim.z));
	out->mips   = ctz_u32(round_up_power_of_2(max_dim)) + 1;

	Stream label = arena_stream(arena);
	stream_append_s8(&label, name);
	stream_append_byte(&label, '[');
	stream_append_hex_u64(&label, out->id);
	stream_append_byte(&label, ']');

	glDeleteTextures(1, &out->texture);
	glCreateTextures(GL_TEXTURE_3D, 1, &out->texture);
	glTextureStorage3D(out->texture, out->mips, GL_RG32F, out->dim.x, out->dim.y, out->dim.z);
	LABEL_GL_OBJECT(GL_TEXTURE, out->texture, stream_to_s8(&label));
}

function void
alloc_shader_storage(BeamformerCtx *ctx, u32 rf_raw_size, Arena a)
{
	ComputeShaderCtx     *cs = &ctx->csctx;
	BeamformerParameters *bp = &((BeamformerSharedMemory *)ctx->shared_memory.region)->parameters;

	cs->dec_data_dim = uv4_from_u32_array(bp->dec_data_dim);
	cs->rf_raw_size  = rf_raw_size;

	glDeleteBuffers(ARRAY_COUNT(cs->rf_data_ssbos), cs->rf_data_ssbos);
	glCreateBuffers(ARRAY_COUNT(cs->rf_data_ssbos), cs->rf_data_ssbos);

	i32 storage_flags = GL_DYNAMIC_STORAGE_BIT;
	glDeleteBuffers(1, &cs->raw_data_ssbo);
	glCreateBuffers(1, &cs->raw_data_ssbo);
	glNamedBufferStorage(cs->raw_data_ssbo, rf_raw_size, 0, storage_flags);
	LABEL_GL_OBJECT(GL_BUFFER, cs->raw_data_ssbo, s8("Raw_RF_SSBO"));

	iz rf_decoded_size = 2 * sizeof(f32) * cs->dec_data_dim.x * cs->dec_data_dim.y * cs->dec_data_dim.z;
	Stream label = arena_stream(a);
	stream_append_s8(&label, s8("Decoded_RF_SSBO_"));
	u32 s_widx = label.widx;
	for (u32 i = 0; i < ARRAY_COUNT(cs->rf_data_ssbos); i++) {
		glNamedBufferStorage(cs->rf_data_ssbos[i], rf_decoded_size, 0, 0);
		stream_append_u64(&label, i);
		LABEL_GL_OBJECT(GL_BUFFER, cs->rf_data_ssbos[i], stream_to_s8(&label));
		stream_reset(&label, s_widx);
	}

	/* NOTE(rnp): these are stubs when CUDA isn't supported */
	ctx->cuda_lib.register_buffers(cs->rf_data_ssbos, countof(cs->rf_data_ssbos), cs->raw_data_ssbo);
	ctx->cuda_lib.init(bp->rf_raw_dim, bp->dec_data_dim);

	u32  order    = cs->dec_data_dim.z;
	i32 *hadamard = make_hadamard_transpose(&a, order);
	if (hadamard) {
		glDeleteTextures(1, &cs->hadamard_texture);
		glCreateTextures(GL_TEXTURE_2D, 1, &cs->hadamard_texture);
		glTextureStorage2D(cs->hadamard_texture, 1, GL_R8I, order, order);
		glTextureSubImage2D(cs->hadamard_texture, 0, 0, 0,  order, order, GL_RED_INTEGER,
		                    GL_INT, hadamard);
		LABEL_GL_OBJECT(GL_TEXTURE, cs->hadamard_texture, s8("Hadamard_Matrix"));
	}
}

function void
push_compute_timing_info(ComputeTimingTable *t, ComputeTimingInfo info)
{
	u32 index = atomic_add_u32(&t->write_index, 1) % countof(t->buffer);
	t->buffer[index] = info;
}

function b32
fill_frame_compute_work(BeamformerCtx *ctx, BeamformWork *work, ImagePlaneTag plane)
{
	b32 result = 0;
	if (work) {
		result = 1;
		u32 frame_id    = atomic_add_u32(&ctx->next_render_frame_index, 1);
		u32 frame_index = frame_id % countof(ctx->beamform_frames);
		work->type      = BW_COMPUTE;
		work->lock      = BeamformerSharedMemoryLockKind_DispatchCompute;
		work->frame     = ctx->beamform_frames + frame_index;
		work->frame->ready_to_present = 0;
		work->frame->frame.id = frame_id;
		work->frame->image_plane_tag = plane;
	}
	return result;
}

function void
export_frame(BeamformerCtx *ctx, iptr handle, BeamformFrame *frame)
{
	uv3 dim            = frame->dim;
	iz  out_size       = dim.x * dim.y * dim.z * 2 * sizeof(f32);
	ctx->export_buffer = ctx->os.alloc_arena(ctx->export_buffer, out_size);
	glGetTextureImage(frame->texture, 0, GL_RG, GL_FLOAT, out_size, ctx->export_buffer.beg);
	s8 raw = {.len = out_size, .data = ctx->export_buffer.beg};
	if (!ctx->os.write_file(handle, raw))
		ctx->os.write_file(ctx->os.error_handle, s8("failed to export frame\n"));
	ctx->os.close(handle);
}

function void
do_sum_shader(ComputeShaderCtx *cs, u32 *in_textures, u32 in_texture_count, f32 in_scale,
              u32 out_texture, uv3 out_data_dim)
{
	/* NOTE: zero output before summing */
	glClearTexImage(out_texture, 0, GL_RED, GL_FLOAT, 0);
	glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);

	glBindImageTexture(0, out_texture, 0, GL_TRUE, 0, GL_READ_WRITE, GL_RG32F);
	glProgramUniform1f(cs->programs[ShaderKind_Sum], CS_SUM_PRESCALE_UNIFORM_LOC, in_scale);
	for (u32 i = 0; i < in_texture_count; i++) {
		glBindImageTexture(1, in_textures[i], 0, GL_TRUE, 0, GL_READ_ONLY, GL_RG32F);
		glDispatchCompute(ORONE(out_data_dim.x / 32),
		                  ORONE(out_data_dim.y),
		                  ORONE(out_data_dim.z / 32));
		glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
	}
}

struct compute_cursor {
	iv3 cursor;
	iv3 dispatch;
	iv3 target;
	u32 points_per_dispatch;
	u32 completed_points;
	u32 total_points;
};

function struct compute_cursor
start_compute_cursor(uv3 dim, u32 max_points)
{
	struct compute_cursor result = {0};
	u32 invocations_per_dispatch = DAS_LOCAL_SIZE_X * DAS_LOCAL_SIZE_Y * DAS_LOCAL_SIZE_Z;

	result.dispatch.y = MIN(max_points / invocations_per_dispatch, ceil_f32((f32)dim.y / DAS_LOCAL_SIZE_Y));

	u32 remaining     = max_points / result.dispatch.y;
	result.dispatch.x = MIN(remaining / invocations_per_dispatch, ceil_f32((f32)dim.x / DAS_LOCAL_SIZE_X));
	result.dispatch.z = MIN(remaining / (invocations_per_dispatch * result.dispatch.x),
	                        ceil_f32((f32)dim.z / DAS_LOCAL_SIZE_Z));

	result.target.x = MAX(dim.x / result.dispatch.x / DAS_LOCAL_SIZE_X, 1);
	result.target.y = MAX(dim.y / result.dispatch.y / DAS_LOCAL_SIZE_Y, 1);
	result.target.z = MAX(dim.z / result.dispatch.z / DAS_LOCAL_SIZE_Z, 1);

	result.points_per_dispatch = 1;
	result.points_per_dispatch *= result.dispatch.x * DAS_LOCAL_SIZE_X;
	result.points_per_dispatch *= result.dispatch.y * DAS_LOCAL_SIZE_Y;
	result.points_per_dispatch *= result.dispatch.z * DAS_LOCAL_SIZE_Z;

	result.total_points = dim.x * dim.y * dim.z;

	return result;
}

function iv3
step_compute_cursor(struct compute_cursor *cursor)
{
	cursor->cursor.x += 1;
	if (cursor->cursor.x >= cursor->target.x) {
		cursor->cursor.x  = 0;
		cursor->cursor.y += 1;
		if (cursor->cursor.y >= cursor->target.y) {
			cursor->cursor.y  = 0;
			cursor->cursor.z += 1;
		}
	}

	cursor->completed_points += cursor->points_per_dispatch;

	iv3 result = cursor->cursor;
	result.x *= cursor->dispatch.x * DAS_LOCAL_SIZE_X;
	result.y *= cursor->dispatch.y * DAS_LOCAL_SIZE_Y;
	result.z *= cursor->dispatch.z * DAS_LOCAL_SIZE_Z;

	return result;
}

function b32
compute_cursor_finished(struct compute_cursor *cursor)
{
	b32 result = cursor->completed_points >= cursor->total_points;
	return result;
}

function void
do_compute_shader(BeamformerCtx *ctx, Arena arena, BeamformComputeFrame *frame, ShaderKind shader)
{
	ComputeShaderCtx *csctx    = &ctx->csctx;
	BeamformerSharedMemory *sm = ctx->shared_memory.region;

	glUseProgram(csctx->programs[shader]);

	u32 output_ssbo_idx = !csctx->last_output_ssbo_index;
	u32 input_ssbo_idx  = csctx->last_output_ssbo_index;

	switch (shader) {
	case ShaderKind_Decode:
	case ShaderKind_DecodeFloat:
	case ShaderKind_DecodeFloatComplex:{
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, csctx->raw_data_ssbo);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, csctx->rf_data_ssbos[output_ssbo_idx]);
		glBindImageTexture(0, csctx->hadamard_texture, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8I);
		glBindImageTexture(1, csctx->channel_mapping_texture, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16I);
		glDispatchCompute(ceil_f32((f32)csctx->dec_data_dim.x / DECODE_LOCAL_SIZE_X),
		                  ceil_f32((f32)csctx->dec_data_dim.y / DECODE_LOCAL_SIZE_Y),
		                  ceil_f32((f32)csctx->dec_data_dim.z / DECODE_LOCAL_SIZE_Z));
		csctx->last_output_ssbo_index = !csctx->last_output_ssbo_index;
	}break;
	case ShaderKind_CudaDecode:{
		ctx->cuda_lib.decode(0, output_ssbo_idx, 0);
		csctx->last_output_ssbo_index = !csctx->last_output_ssbo_index;
	}break;
	case ShaderKind_CudaHilbert:
		ctx->cuda_lib.hilbert(input_ssbo_idx, output_ssbo_idx);
		csctx->last_output_ssbo_index = !csctx->last_output_ssbo_index;
		break;
	case ShaderKind_Demodulate:{
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, csctx->rf_data_ssbos[input_ssbo_idx]);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, csctx->rf_data_ssbos[output_ssbo_idx]);
		glDispatchCompute(ORONE(csctx->dec_data_dim.x / 32),
		                  ORONE(csctx->dec_data_dim.y / 32),
		                  ORONE(csctx->dec_data_dim.z));
		csctx->last_output_ssbo_index = !csctx->last_output_ssbo_index;
	}break;
	case ShaderKind_MinMax:{
		u32 texture = frame->frame.texture;
		for (u32 i = 1; i < frame->frame.mips; i++) {
			glBindImageTexture(0, texture, i - 1, GL_TRUE, 0, GL_READ_ONLY,  GL_RG32F);
			glBindImageTexture(1, texture, i - 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RG32F);
			glProgramUniform1i(csctx->programs[shader], CS_MIN_MAX_MIPS_LEVEL_UNIFORM_LOC, i);

			u32 width  = frame->frame.dim.x >> i;
			u32 height = frame->frame.dim.y >> i;
			u32 depth  = frame->frame.dim.z >> i;
			glDispatchCompute(ORONE(width / 32), ORONE(height), ORONE(depth / 32));
			glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
		}
	}break;
	case ShaderKind_DASCompute:{
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, csctx->rf_data_ssbos[input_ssbo_idx]);
		glBindImageTexture(0, frame->frame.texture, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RG32F);
		glBindImageTexture(1, csctx->sparse_elements_texture, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16I);
		glBindImageTexture(2, csctx->focal_vectors_texture, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG32F);

		glProgramUniform1ui(csctx->programs[shader], DAS_CYCLE_T_UNIFORM_LOC, cycle_t++);

		#if 1
		/* TODO(rnp): compute max_points_per_dispatch based on something like a
		 * transmit_count * channel_count product */
		u32 max_points_per_dispatch = KB(64);
		struct compute_cursor cursor = start_compute_cursor(frame->frame.dim, max_points_per_dispatch);
		f32 percent_per_step = (f32)cursor.points_per_dispatch / (f32)cursor.total_points;
		csctx->processing_progress = -percent_per_step;
		for (iv3 offset = {0};
		     !compute_cursor_finished(&cursor);
		     offset = step_compute_cursor(&cursor))
		{
			csctx->processing_progress += percent_per_step;
			/* IMPORTANT(rnp): prevents OS from coalescing and killing our shader */
			glFinish();
			glProgramUniform3iv(csctx->programs[shader], DAS_VOXEL_OFFSET_UNIFORM_LOC,
			                    1, offset.E);
			glDispatchCompute(cursor.dispatch.x, cursor.dispatch.y, cursor.dispatch.z);
		}
		#else
		/* NOTE(rnp): use this for testing tiling code. The performance of the above path
		 * should be the same as this path if everything is working correctly */
		iv3 compute_dim_offset = {0};
		glUniform3iv(csctx->voxel_offset_id, 1, compute_dim_offset.E);
		glDispatchCompute(ORONE(frame->frame.dim.x / 32),
		                  ORONE(frame->frame.dim.y),
		                  ORONE(frame->frame.dim.z / 32));
		#endif
		glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT|GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
	}break;
	case ShaderKind_Sum:{
		u32 aframe_index = ctx->averaged_frame_index % ARRAY_COUNT(ctx->averaged_frames);
		BeamformComputeFrame *aframe = ctx->averaged_frames + aframe_index;
		aframe->ready_to_present     = 0;
		aframe->frame.id             = ctx->averaged_frame_index;
		/* TODO(rnp): hack we need a better way of specifying which frames to sum;
		 * this is fine for rolling averaging but what if we want to do something else */
		assert(frame >= ctx->beamform_frames);
		assert(frame < ctx->beamform_frames + countof(ctx->beamform_frames));
		u32 base_index   = (u32)(frame - ctx->beamform_frames);
		u32 to_average   = sm->parameters.output_points[3];
		u32 frame_count  = 0;
		u32 *in_textures = push_array(&arena, u32, MAX_BEAMFORMED_SAVED_FRAMES);
		ComputeFrameIterator cfi = compute_frame_iterator(ctx, 1 + base_index - to_average,
		                                                  to_average);
		for (BeamformComputeFrame *it = frame_next(&cfi); it; it = frame_next(&cfi))
			in_textures[frame_count++] = it->frame.texture;

		ASSERT(to_average == frame_count);

		do_sum_shader(csctx, in_textures, frame_count, 1 / (f32)frame_count,
		              aframe->frame.texture, aframe->frame.dim);
		aframe->frame.min_coordinate  = frame->frame.min_coordinate;
		aframe->frame.max_coordinate  = frame->frame.max_coordinate;
		aframe->frame.compound_count  = frame->frame.compound_count;
		aframe->frame.das_shader_kind = frame->frame.das_shader_kind;
	}break;
	InvalidDefaultCase;
	}
}

function s8
shader_text_with_header(ShaderReloadContext *ctx, OS *os, Arena *arena)
{
	Stream sb = arena_stream(*arena);
	stream_append_s8s(&sb, s8("#version 460 core\n\n"), ctx->header);

	switch (ctx->kind) {
	case ShaderKind_DASCompute:{
		#define X(type, id, pretty, fixed_tx) "#define DAS_ID_" #type " " #id "\n"
		stream_append_s8(&sb, s8(""
		"layout(local_size_x = " str(DAS_LOCAL_SIZE_X) ", "
		       "local_size_y = " str(DAS_LOCAL_SIZE_Y) ", "
		       "local_size_z = " str(DAS_LOCAL_SIZE_Z) ") in;\n\n"
		"layout(location = " str(DAS_VOXEL_OFFSET_UNIFORM_LOC) ") uniform ivec3 u_voxel_offset;\n"
		"layout(location = " str(DAS_CYCLE_T_UNIFORM_LOC)      ") uniform uint  u_cycle_t;\n\n"
		DAS_TYPES
		));
		#undef X
	}break;
	case ShaderKind_DecodeFloat:
	case ShaderKind_DecodeFloatComplex:{
		if (ctx->kind == ShaderKind_DecodeFloat)
			stream_append_s8(&sb, s8("#define INPUT_DATA_TYPE_FLOAT\n\n"));
		else
			stream_append_s8(&sb, s8("#define INPUT_DATA_TYPE_FLOAT_COMPLEX\n\n"));
	} /* FALLTHROUGH */
	case ShaderKind_Decode:{
		#define X(type, id, pretty) "#define DECODE_MODE_" #type " " #id "\n"
		stream_append_s8(&sb, s8(""
		"layout(local_size_x = " str(DECODE_LOCAL_SIZE_X) ", "
		       "local_size_y = " str(DECODE_LOCAL_SIZE_Y) ", "
		       "local_size_z = " str(DECODE_LOCAL_SIZE_Z) ") in;\n\n"
		DECODE_TYPES
		));
		#undef X
	}break;
	case ShaderKind_MinMax:{
		stream_append_s8(&sb, s8("layout(location = " str(CS_MIN_MAX_MIPS_LEVEL_UNIFORM_LOC)
		                         ") uniform int u_mip_map;\n\n"));
	}break;
	case ShaderKind_Sum:{
		stream_append_s8(&sb, s8("layout(location = " str(CS_SUM_PRESCALE_UNIFORM_LOC)
		                         ") uniform float u_sum_prescale = 1.0;\n\n"));
	}break;
	default:{}break;
	}
	stream_append_s8(&sb, s8("\n#line 1\n"));

	s8 result = arena_stream_commit(arena, &sb);
	if (ctx->path.len) {
		s8 file = os->read_whole_file(arena, (c8 *)ctx->path.data);
		assert(file.data == result.data + result.len);
		result.len += file.len;
	}

	return result;
}

DEBUG_EXPORT BEAMFORMER_RELOAD_SHADER_FN(beamformer_reload_shader)
{
	i32 shader_count = 1;
	ShaderReloadContext *link = src->link;
	while (link != src) { shader_count++; link = link->link; }

	s8  *shader_texts = push_array(&arena, s8,  shader_count);
	u32 *shader_types = push_array(&arena, u32, shader_count);

	i32 index = 0;
	do {
		shader_texts[index] = shader_text_with_header(link, &ctx->os, &arena);
		shader_types[index] = link->gl_type;
		index++;
		link = link->link;
	} while (link != src);

	u32 new_program = load_shader(&ctx->os, arena, shader_texts, shader_types, shader_count, shader_name);
	if (new_program) {
		glDeleteProgram(*src->shader);
		*src->shader = new_program;
		if (src->kind == ShaderKind_Render2D) ctx->frame_view_render_context.updated = 1;
	}
	return new_program != 0;
}

function b32
reload_compute_shader(BeamformerCtx *ctx, ShaderReloadContext *src, s8 name_extra, Arena arena)
{
	Stream sb  = arena_stream(arena);
	stream_append_s8s(&sb, src->name, name_extra);
	s8  name   = arena_stream_commit(&arena, &sb);
	b32 result = beamformer_reload_shader(ctx, src, arena, name);
	if (result) {
		glUseProgram(*src->shader);
		glBindBufferBase(GL_UNIFORM_BUFFER, 0, ctx->csctx.shared_ubo);
	}
	return result;
}

function void
complete_queue(BeamformerCtx *ctx, BeamformWorkQueue *q, Arena arena, iptr gl_context)
{
	ComputeShaderCtx       *cs = &ctx->csctx;
	BeamformerSharedMemory *sm = ctx->shared_memory.region;
	BeamformerParameters   *bp = &sm->parameters;

	BeamformWork *work = beamform_work_queue_pop(q);
	while (work) {
		b32 can_commit = 1;
		switch (work->type) {
		case BW_RELOAD_SHADER: {
			ShaderReloadContext *src = work->shader_reload_context;
			b32 success = reload_compute_shader(ctx, src, s8(""), arena);
			if (src->kind == ShaderKind_Decode) {
				/* TODO(rnp): think of a better way of doing this */
				src->kind   = ShaderKind_DecodeFloatComplex;
				src->shader = cs->programs + ShaderKind_DecodeFloatComplex;
				success &= reload_compute_shader(ctx, src, s8(" (F32C)"), arena);
				src->kind   = ShaderKind_DecodeFloat;
				src->shader = cs->programs + ShaderKind_DecodeFloat;
				success &= reload_compute_shader(ctx, src, s8(" (F32)"),  arena);
				src->kind   = ShaderKind_Decode;
				src->shader = cs->programs + ShaderKind_Decode;
			}

			if (success) {
				/* TODO(rnp): this check seems off */
				if (ctx->csctx.raw_data_ssbo) {
					can_commit = 0;
					ImagePlaneTag plane = ctx->beamform_frames[ctx->display_frame_index].image_plane_tag;
					fill_frame_compute_work(ctx, work, plane);
				}
			}
		} break;
		case BW_UPLOAD_BUFFER: {
			ctx->os.shared_memory_region_lock(&ctx->shared_memory, sm->locks, (i32)work->lock, -1);
			BeamformerUploadContext *uc = &work->upload_context;
			u32 tex_type, tex_format, tex_element_count, tex_1d = 0, buffer = 0;
			switch (uc->kind) {
			case BU_KIND_CHANNEL_MAPPING: {
				tex_1d            = cs->channel_mapping_texture;
				tex_type          = GL_SHORT;
				tex_format        = GL_RED_INTEGER;
				tex_element_count = ARRAY_COUNT(sm->channel_mapping);
				ctx->cuda_lib.set_channel_mapping(sm->channel_mapping);
			} break;
			case BU_KIND_FOCAL_VECTORS: {
				tex_1d            = cs->focal_vectors_texture;
				tex_type          = GL_FLOAT;
				tex_format        = GL_RG;
				tex_element_count = ARRAY_COUNT(sm->focal_vectors);
			} break;
			case BU_KIND_SPARSE_ELEMENTS: {
				tex_1d            = cs->sparse_elements_texture;
				tex_type          = GL_SHORT;
				tex_format        = GL_RED_INTEGER;
				tex_element_count = ARRAY_COUNT(sm->sparse_elements);
			} break;
			case BU_KIND_PARAMETERS: {
				ctx->ui_read_params = ctx->beamform_work_queue != q;
				buffer = cs->shared_ubo;
			} break;
			case BU_KIND_RF_DATA: {
				if (cs->rf_raw_size != uc->size ||
				    !uv4_equal(cs->dec_data_dim, uv4_from_u32_array(bp->dec_data_dim)))
				{
					alloc_shader_storage(ctx, uc->size, arena);
				}
				buffer = cs->raw_data_ssbo;

				ComputeTimingInfo info = {0};
				info.kind = ComputeTimingInfoKind_RF_Data;
				/* TODO(rnp): this could stall. what should we do about it? */
				glGetQueryObjectui64v(cs->rf_data_timestamp_query, GL_QUERY_RESULT, &info.timer_count);
				glQueryCounter(cs->rf_data_timestamp_query, GL_TIMESTAMP);
				push_compute_timing_info(ctx->compute_timing_table, info);
			}break;
			InvalidDefaultCase;
			}

			if (tex_1d) {
				glTextureSubImage1D(tex_1d, 0, 0, tex_element_count, tex_format,
				                    tex_type, (u8 *)sm + uc->shared_memory_offset);
			}

			if (buffer) {
				glNamedBufferSubData(buffer, 0, uc->size,
				                     (u8 *)sm + uc->shared_memory_offset);
			}

			atomic_and_u32(&sm->dirty_regions, ~(sm->dirty_regions & 1 << (work->lock - 1)));
			ctx->os.shared_memory_region_unlock(&ctx->shared_memory, sm->locks, (i32)work->lock);
		} break;
		case BW_COMPUTE_INDIRECT:{
			fill_frame_compute_work(ctx, work, work->compute_indirect_plane);
			DEBUG_DECL(work->type = BW_COMPUTE_INDIRECT;)
		} /* FALLTHROUGH */
		case BW_COMPUTE:{
			/* NOTE(rnp): debug: here it is not a bug to release the lock if it
			 * isn't held but elswhere it is */
			DEBUG_DECL(if (sm->locks[work->lock])) {
				ctx->os.shared_memory_region_unlock(&ctx->shared_memory,
				                                    sm->locks, work->lock);
			}

			push_compute_timing_info(ctx->compute_timing_table,
			                         (ComputeTimingInfo){.kind = ComputeTimingInfoKind_ComputeFrameBegin});

			i32 mask = 1 << (BeamformerSharedMemoryLockKind_Parameters - 1);
			if (sm->dirty_regions & mask) {
				glNamedBufferSubData(cs->shared_ubo, 0, sizeof(sm->parameters), &sm->parameters);
				atomic_and_u32(&sm->dirty_regions, ~mask);
			}

			atomic_store_u32(&cs->processing_compute, 1);
			start_renderdoc_capture(gl_context);

			BeamformComputeFrame *frame = work->frame;
			uv3 try_dim = make_valid_test_dim(bp->output_points);
			if (!uv3_equal(try_dim, frame->frame.dim))
				alloc_beamform_frame(&ctx->gl, &frame->frame, try_dim, s8("Beamformed_Data"), arena);

			if (bp->output_points[3] > 1) {
				if (!uv3_equal(try_dim, ctx->averaged_frames[0].frame.dim)) {
					alloc_beamform_frame(&ctx->gl, &ctx->averaged_frames[0].frame,
					                     try_dim, s8("Averaged Frame"), arena);
					alloc_beamform_frame(&ctx->gl, &ctx->averaged_frames[1].frame,
					                     try_dim, s8("Averaged Frame"), arena);
				}
			}

			frame->in_flight = 1;
			frame->frame.min_coordinate  = v4_from_f32_array(bp->output_min_coordinate);
			frame->frame.max_coordinate  = v4_from_f32_array(bp->output_max_coordinate);
			frame->frame.das_shader_kind = bp->das_shader_id;
			frame->frame.compound_count  = bp->dec_data_dim[2];

			b32 did_sum_shader = 0;
			u32 stage_count    = sm->compute_stages_count;
			ComputeShaderKind *stages = sm->compute_stages;
			for (u32 i = 0; i < stage_count; i++) {
				did_sum_shader |= stages[i] == ComputeShaderKind_Sum;
				glBeginQuery(GL_TIME_ELAPSED, cs->shader_timer_ids[i]);
				do_compute_shader(ctx, arena, frame, (ShaderKind)stages[i]);
				glEndQuery(GL_TIME_ELAPSED);
			}
			/* NOTE(rnp): block until work completes so that we can record timings */
			glFinish();
			cs->processing_progress = 1;

			for (u32 i = 0; i < stage_count; i++) {
				ComputeTimingInfo info = {0};
				info.kind   = ComputeTimingInfoKind_Shader;
				info.shader = (ShaderKind)stages[i];
				glGetQueryObjectui64v(cs->shader_timer_ids[i], GL_QUERY_RESULT, &info.timer_count);
				push_compute_timing_info(ctx->compute_timing_table, info);
			}

			if (did_sum_shader) {
				u32 aframe_index = (ctx->averaged_frame_index % countof(ctx->averaged_frames));
				ctx->averaged_frames[aframe_index].image_plane_tag  = frame->image_plane_tag;
				ctx->averaged_frames[aframe_index].ready_to_present = 1;
				atomic_add_u32(&ctx->averaged_frame_index, 1);
			}
			frame->ready_to_present = 1;
			cs->processing_compute  = 0;

			push_compute_timing_info(ctx->compute_timing_table,
			                         (ComputeTimingInfo){.kind = ComputeTimingInfoKind_ComputeFrameEnd});

			end_renderdoc_capture(gl_context);
		} break;
		case BW_SAVE_FRAME: {
			BeamformComputeFrame *frame = work->output_frame_ctx.frame;
			if (frame->ready_to_present) {
				export_frame(ctx, work->output_frame_ctx.file_handle, &frame->frame);
			} else {
				/* TODO(rnp): should we handle this? */
				INVALID_CODE_PATH;
			}
		} break;
		InvalidDefaultCase;
		}

		if (can_commit) {
			beamform_work_queue_pop_commit(q);
			work = beamform_work_queue_pop(q);
		}
	}
}

function void
coalesce_timing_table(ComputeTimingTable *t, ComputeShaderStats *stats)
{
	/* TODO(rnp): we do not currently do anything to handle the potential for a half written
	 * info item. this could result in garbage entries but they shouldn't really matter */

	u32 target = atomic_load_u32(&t->write_index);
	u32 stats_index = (stats->latest_frame_index + 1) % countof(stats->times);

	static_assert(ShaderKind_Count + 1 <= 32, "timing coalescence bitfield test");
	u32 seen_info_test = 0;

	while (t->read_index != target) {
		ComputeTimingInfo info = t->buffer[(t->read_index++) % countof(t->buffer)];
		switch (info.kind) {
		case ComputeTimingInfoKind_ComputeFrameBegin:{
			assert(t->compute_frame_active == 0);
			t->compute_frame_active = 1;
			/* NOTE(rnp): allow multiple instances of same shader to accumulate */
			mem_clear(stats->times[stats_index], 0, sizeof(stats->times[stats_index]));
		}break;
		case ComputeTimingInfoKind_ComputeFrameEnd:{
			assert(t->compute_frame_active == 1);
			t->compute_frame_active = 0;
			stats->latest_frame_index = stats_index;
			stats_index = (stats_index + 1) % countof(stats->times);
		}break;
		case ComputeTimingInfoKind_Shader:{
			stats->times[stats_index][info.shader] += (f32)info.timer_count / 1.0e9;
			seen_info_test |= (1 << info.shader);
		}break;
		case ComputeTimingInfoKind_RF_Data:{
			stats->latest_rf_index = (stats->latest_rf_index + 1) % countof(stats->rf_time_deltas);
			f32 delta = (f32)(info.timer_count - stats->last_rf_timer_count) / 1.0e9;
			stats->rf_time_deltas[stats->latest_rf_index] = delta;
			stats->last_rf_timer_count = info.timer_count;
			seen_info_test |= (1 << ShaderKind_Count);
		}break;
		}
	}

	if (seen_info_test) {
		for EachEnumValue(ShaderKind, shader) {
			if (seen_info_test & (1 << shader)) {
				f32 sum = 0;
				for EachElement(stats->times, i)
					sum += stats->times[i][shader];
				stats->average_times[shader] = sum / countof(stats->times);
			}
		}

		if (seen_info_test & (1 << ShaderKind_Count)) {
			f32 sum = 0;
			for EachElement(stats->rf_time_deltas, i)
				sum += stats->rf_time_deltas[i];
			stats->rf_time_delta_average = sum / countof(stats->rf_time_deltas);
		}
	}
}

DEBUG_EXPORT BEAMFORMER_COMPUTE_SETUP_FN(beamformer_compute_setup)
{
	BeamformerCtx          *ctx = (BeamformerCtx *)user_context;
	BeamformerSharedMemory *sm  = ctx->shared_memory.region;
	ComputeShaderCtx       *cs  = &ctx->csctx;

	glCreateBuffers(1, &cs->shared_ubo);
	glNamedBufferStorage(cs->shared_ubo, sizeof(sm->parameters), 0, GL_DYNAMIC_STORAGE_BIT);

	glCreateTextures(GL_TEXTURE_1D, 1, &cs->channel_mapping_texture);
	glCreateTextures(GL_TEXTURE_1D, 1, &cs->sparse_elements_texture);
	glCreateTextures(GL_TEXTURE_1D, 1, &cs->focal_vectors_texture);
	glTextureStorage1D(cs->channel_mapping_texture, 1, GL_R16I,  ARRAY_COUNT(sm->channel_mapping));
	glTextureStorage1D(cs->sparse_elements_texture, 1, GL_R16I,  ARRAY_COUNT(sm->sparse_elements));
	glTextureStorage1D(cs->focal_vectors_texture,   1, GL_RG32F, ARRAY_COUNT(sm->focal_vectors));

	LABEL_GL_OBJECT(GL_TEXTURE, cs->channel_mapping_texture, s8("Channel_Mapping"));
	LABEL_GL_OBJECT(GL_TEXTURE, cs->focal_vectors_texture,   s8("Focal_Vectors"));
	LABEL_GL_OBJECT(GL_TEXTURE, cs->sparse_elements_texture, s8("Sparse_Elements"));
	LABEL_GL_OBJECT(GL_BUFFER,  cs->shared_ubo,              s8("Beamformer_Parameters"));

	glCreateQueries(GL_TIME_ELAPSED, countof(cs->shader_timer_ids), cs->shader_timer_ids);
	glCreateQueries(GL_TIMESTAMP, 1, &cs->rf_data_timestamp_query);

	/* NOTE(rnp): start this here so we don't have to worry about it being started or not */
	glQueryCounter(cs->rf_data_timestamp_query, GL_TIMESTAMP);
}

DEBUG_EXPORT BEAMFORMER_COMPLETE_COMPUTE_FN(beamformer_complete_compute)
{
	BeamformerCtx *ctx         = (BeamformerCtx *)user_context;
	BeamformerSharedMemory *sm = ctx->shared_memory.region;
	complete_queue(ctx, &sm->external_work_queue, arena, gl_context);
	complete_queue(ctx, ctx->beamform_work_queue, arena, gl_context);
}

#include "ui.c"

DEBUG_EXPORT BEAMFORMER_FRAME_STEP_FN(beamformer_frame_step)
{
	dt_for_frame = input->dt;

	if (IsWindowResized()) {
		ctx->window_size.h = GetScreenHeight();
		ctx->window_size.w = GetScreenWidth();
	}

	coalesce_timing_table(ctx->compute_timing_table, ctx->compute_shader_stats);

	if (input->executable_reloaded) {
		ui_init(ctx, ctx->ui_backing_store);
		DEBUG_DECL(start_frame_capture = ctx->os.start_frame_capture);
		DEBUG_DECL(end_frame_capture   = ctx->os.end_frame_capture);
	}

	BeamformerSharedMemory *sm = ctx->shared_memory.region;
	BeamformerParameters   *bp = &sm->parameters;
	if (sm->locks[BeamformerSharedMemoryLockKind_DispatchCompute] && ctx->os.compute_worker.asleep) {
		if (sm->start_compute_from_main) {
			BeamformWork *work = beamform_work_queue_push(ctx->beamform_work_queue);
			ImagePlaneTag tag  = ctx->beamform_frames[ctx->display_frame_index].image_plane_tag;
			if (fill_frame_compute_work(ctx, work, tag)) {
				beamform_work_queue_push_commit(ctx->beamform_work_queue);
				if (sm->export_next_frame) {
					BeamformWork *export = beamform_work_queue_push(ctx->beamform_work_queue);
					if (export) {
						/* TODO: we don't really want the beamformer opening/closing files */
						iptr f = ctx->os.open_for_write(ctx->os.export_pipe_name);
						export->type = BW_SAVE_FRAME;
						export->output_frame_ctx.file_handle = f;
						if (bp->output_points[3] > 1) {
							static_assert(countof(ctx->averaged_frames) == 2,
							              "fix this, we assume average frame ping pong buffer");
							u32 a_index = !(ctx->averaged_frame_index %
							                countof(ctx->averaged_frames));
							BeamformComputeFrame *aframe = ctx->averaged_frames + a_index;
							export->output_frame_ctx.frame = aframe;
						} else {
							export->output_frame_ctx.frame = work->frame;
						}
						beamform_work_queue_push_commit(ctx->beamform_work_queue);
					}
					sm->export_next_frame = 0;
				}
			}
			atomic_store_u32(&sm->start_compute_from_main, 0);
		}
		ctx->os.wake_waiters(&ctx->os.compute_worker.sync_variable);
	}

	ComputeFrameIterator cfi = compute_frame_iterator(ctx, ctx->display_frame_index,
	                                                  ctx->next_render_frame_index - ctx->display_frame_index);
	for (BeamformComputeFrame *frame = frame_next(&cfi); frame; frame = frame_next(&cfi)) {
		if (frame->in_flight && frame->ready_to_present) {
			frame->in_flight         = 0;
			ctx->display_frame_index = frame - cfi.frames;
		}
	}

	BeamformComputeFrame *frame_to_draw;
	if (bp->output_points[3] > 1) {
		u32 a_index = !(ctx->averaged_frame_index % countof(ctx->averaged_frames));
		frame_to_draw = ctx->averaged_frames + a_index;
	} else {
		frame_to_draw = ctx->beamform_frames + ctx->display_frame_index;
	}

	draw_ui(ctx, input, frame_to_draw->ready_to_present? &frame_to_draw->frame : 0,
	        frame_to_draw->image_plane_tag);

	ctx->frame_view_render_context.updated = 0;

	if (WindowShouldClose())
		ctx->should_exit = 1;
}
