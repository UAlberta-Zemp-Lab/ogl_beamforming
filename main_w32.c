/* See LICENSE for license details. */
#ifndef _WIN32
#error This file is only meant to be compiled for Win32
#endif

#include "beamformer.h"

typedef struct {
	iptr io_completion_handle;
} w32_context;

enum w32_io_events {
	W32_IO_FILE_WATCH,
	W32_IO_PIPE,
};

typedef struct {
	u64  tag;
	iptr context;
} w32_io_completion_event;

#include "os_win32.c"

#define OS_DEBUG_LIB_NAME      ".\\beamformer.dll"
#define OS_DEBUG_LIB_TEMP_NAME ".\\beamformer_temp.dll"

#define OS_CUDA_LIB_NAME       "external\\cuda_toolkit.dll"
#define OS_CUDA_LIB_TEMP_NAME  "external\\cuda_toolkit_temp.dll"

#define OS_RENDERDOC_SONAME    "renderdoc.dll"

#define OS_PIPE_NAME           "\\\\.\\pipe\\beamformer_data_fifo"
#define OS_SMEM_NAME           "Local\\ogl_beamformer_parameters"

#define OS_PATH_SEPERATOR      "\\"

#include "static.c"

static void
dispatch_file_watch(Platform *platform, FileWatchDirectory *fw_dir, u8 *buf, Arena arena)
{
	i64 offset = 0;
	TempArena save_point = {0};
	w32_file_notify_info *fni = (w32_file_notify_info *)buf;
	do {
		end_temp_arena(save_point);
		save_point = begin_temp_arena(&arena);

		Stream path = {.data = arena_commit(&arena, KB(1)), .cap = KB(1)};

		if (fni->action != FILE_ACTION_MODIFIED) {
			path.widx = 0;
			stream_append_s8(&path, s8("unknown file watch event: "));
			stream_append_u64(&path, fni->action);
			stream_append_byte(&path, '\n');
			os_write_err_msg(stream_to_s8(&path));
		}

		stream_append_s8(&path, fw_dir->name);
		stream_append_byte(&path, '\\');

		s8 file_name = s16_to_s8(&arena, (s16){.data = fni->filename,
		                                       .len  = fni->filename_size / 2});
		stream_append_s8(&path, file_name);
		stream_append_byte(&path, 0);
		path.widx--;

		u64 hash = s8_hash(file_name);
		for (u32 i = 0; i < fw_dir->file_watch_count; i++) {
			FileWatch *fw = fw_dir->file_watches + i;
			if (fw->hash == hash) {
				fw->callback(platform, stream_to_s8(&path), fw->user_data, arena);
				break;
			}
		}

		offset = fni->next_entry_offset;
		fni    = (w32_file_notify_info *)((u8 *)fni + offset);
	} while (offset);
}

static void
clear_io_queue(Platform *platform, BeamformerInput *input, Arena arena)
{
	w32_context *ctx = (w32_context *)platform->os_context;

	iptr handle = ctx->io_completion_handle;
	w32_overlapped *overlapped;
	u32  bytes_read;
	uptr user_data;
	while (GetQueuedCompletionStatus(handle, &bytes_read, &user_data, &overlapped, 0)) {
		w32_io_completion_event *event = (w32_io_completion_event *)user_data;
		switch (event->tag) {
		case W32_IO_FILE_WATCH: {
			FileWatchDirectory *dir = (FileWatchDirectory *)event->context;
			dispatch_file_watch(platform, dir, dir->buffer.beg, arena);
			zero_struct(overlapped);
			ReadDirectoryChangesW(dir->handle, dir->buffer.beg, 4096, 0,
			                      FILE_NOTIFY_CHANGE_LAST_WRITE, 0, overlapped, 0);
		} break;
		case W32_IO_PIPE: break;
		}
	}
}

static b32
poll_pipe(Pipe *p, Stream *e)
{
	u8  data;
	i32 total_read = 0;
	b32 result = ReadFile(p->file, &data, 0, &total_read, 0);
	if (!result) {
		i32 error = GetLastError();
		/* NOTE: These errors mean nothing's been sent yet, otherwise pipe is busted
		 * and needs to be recreated. */
		if (error != ERROR_NO_DATA &&
		    error != ERROR_PIPE_LISTENING &&
		    error != ERROR_PIPE_NOT_CONNECTED)
		{
			DisconnectNamedPipe(p->file);
			CloseHandle(p->file);
			*p = os_open_named_pipe(p->name);

			if (p->file == INVALID_FILE) {
				stream_append_s8(e, s8("poll_pipe: failed to reopen pipe: error: "));
				stream_append_i64(e, GetLastError());
				stream_append_byte(e, '\n');
				os_write_err_msg(stream_to_s8(e));
				e->widx = 0;
			}
		}
	}
	return result;
}

int
main(void)
{
	BeamformerCtx   ctx   = {0};
	BeamformerInput input = {.executable_reloaded = 1};
	Arena temp_memory = os_alloc_arena((Arena){0}, MB(16));
	ctx.error_stream  = stream_alloc(&temp_memory, MB(1));

	ctx.ui_backing_store              = sub_arena(&temp_memory, MB(2), KB(4));
	ctx.platform.compute_worker.arena = sub_arena(&temp_memory, MB(2), KB(4));

	Pipe data_pipe    = os_open_named_pipe(OS_PIPE_NAME);
	input.pipe_handle = data_pipe.file;
	ASSERT(data_pipe.file != INVALID_FILE);

	#define X(name) ctx.platform.name = os_ ## name;
	PLATFORM_FNS
	#undef X

	w32_context w32_ctx = {0};
	w32_ctx.io_completion_handle = CreateIoCompletionPort(INVALID_FILE, 0, 0, 0);

	ctx.platform.os_context            = (iptr)&w32_ctx;
	ctx.platform.compute_worker.asleep = 1;
	ctx.platform.error_file_handle     = GetStdHandle(STD_ERROR_HANDLE);

	debug_init(&ctx.platform, (iptr)&input, &temp_memory);
	setup_beamformer(&ctx, &temp_memory);
	os_wake_thread(ctx.platform.compute_worker.sync_handle);

	while (!ctx.should_exit) {
		clear_io_queue(&ctx.platform, &input, temp_memory);

		input.last_mouse = input.mouse;
		input.mouse.rl   = GetMousePosition();

		input.pipe_data_available = poll_pipe(&data_pipe, &ctx.error_stream);

		beamformer_frame_step(&ctx, &temp_memory, &input);

		input.executable_reloaded = 0;
	}
}
