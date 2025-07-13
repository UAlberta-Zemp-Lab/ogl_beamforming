/* See LICENSE for license details. */
#include "compiler.h"

#if !OS_WINDOWS
#error This file is only meant to be compiled for Win32
#endif

#include "beamformer.h"

#include "os_win32.c"

#define OS_DEBUG_LIB_NAME      ".\\beamformer.dll"
#define OS_DEBUG_LIB_TEMP_NAME ".\\beamformer_temp.dll"

#define OS_CUDA_LIB_NAME       "external\\cuda_toolkit.dll"
#define OS_CUDA_LIB_TEMP_NAME  "external\\cuda_toolkit_temp.dll"

#define OS_RENDERDOC_SONAME    "renderdoc.dll"

iptr glfwGetWGLContext(iptr);
function iptr
os_get_native_gl_context(iptr window)
{
	return glfwGetWGLContext(window);
}

function iptr
os_gl_proc_address(char *name)
{
	return wglGetProcAddress(name);
}

#include "static.c"

function void
dispatch_file_watch(OS *os, FileWatchDirectory *fw_dir, u8 *buf, Arena arena)
{
	i64 offset = 0;
	TempArena save_point = {0};
	w32_file_notify_info *fni = (w32_file_notify_info *)buf;
	do {
		end_temp_arena(save_point);
		save_point = begin_temp_arena(&arena);

		Stream path = {.data = arena_commit(&arena, KB(1)), .cap = KB(1)};

		if (fni->action != FILE_ACTION_MODIFIED) {
			stream_append_s8(&path, s8("unknown file watch event: "));
			stream_append_u64(&path, fni->action);
			stream_append_byte(&path, '\n');
			os->write_file(os->error_handle, stream_to_s8(&path));
			stream_reset(&path, 0);
		}

		stream_append_s8(&path, fw_dir->name);
		stream_append_byte(&path, '\\');

		s8 file_name = s16_to_s8(&arena, (s16){.data = fni->filename,
		                                       .len  = fni->filename_size / 2});
		stream_append_s8(&path, file_name);
		stream_append_byte(&path, 0);
		stream_commit(&path, -1);

		u64 hash = s8_hash(file_name);
		for (u32 i = 0; i < fw_dir->count; i++) {
			FileWatch *fw = fw_dir->data + i;
			if (fw->hash == hash) {
				fw->callback(os, stream_to_s8(&path), fw->user_data, arena);
				break;
			}
		}

		offset = fni->next_entry_offset;
		fni    = (w32_file_notify_info *)((u8 *)fni + offset);
	} while (offset);
}

function void
clear_io_queue(OS *os, BeamformerInput *input, Arena arena)
{
	w32_context *ctx = (w32_context *)os->context;

	iptr handle = ctx->io_completion_handle;
	w32_overlapped *overlapped;
	u32  bytes_read;
	uptr user_data;
	while (GetQueuedCompletionStatus(handle, &bytes_read, &user_data, &overlapped, 0)) {
		w32_io_completion_event *event = (w32_io_completion_event *)user_data;
		switch (event->tag) {
		case W32_IO_FILE_WATCH: {
			FileWatchDirectory *dir = (FileWatchDirectory *)event->context;
			dispatch_file_watch(os, dir, dir->buffer.beg, arena);
			zero_struct(overlapped);
			ReadDirectoryChangesW(dir->handle, dir->buffer.beg, 4096, 0,
			                      FILE_NOTIFY_CHANGE_LAST_WRITE, 0, overlapped, 0);
		} break;
		case W32_IO_PIPE: break;
		}
	}
}

extern i32
main(void)
{
	Arena program_memory = os_alloc_arena(MB(16));

	BeamformerCtx   *ctx   = 0;
	BeamformerInput *input = 0;

	setup_beamformer(&program_memory, &ctx, &input);
	os_wake_waiters(&ctx->os.compute_worker.sync_variable);

	w32_context *w32_ctx = (w32_context *)ctx->os.context;
	u64 last_time = os_get_timer_counter();
	while (!ctx->should_exit) {
		clear_io_queue(&ctx->os, input, program_memory);

		u64 now = os_get_timer_counter();
		input->last_mouse = input->mouse;
		input->mouse.rl   = GetMousePosition();
		input->dt         = (f32)((f64)(now - last_time) / (f64)w32_ctx->timer_frequency);
		last_time         = now;

		beamformer_frame_step(ctx, input);

		input->executable_reloaded = 0;
	}

	beamformer_invalidate_shared_memory(ctx);
	beamformer_debug_ui_deinit(ctx);
}
