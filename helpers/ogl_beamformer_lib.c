/* See LICENSE for license details. */
#include "../util.h"
#include "../beamformer_parameters.h"
#include "../beamformer_work_queue.c"

#define PIPE_RETRY_PERIOD_MS (100ULL)

static BeamformerSharedMemory *g_bp;

#if defined(__linux__)
#include <fcntl.h>
#include <linux/futex.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

i64 syscall(i64, ...);

#define OS_EXPORT_PIPE_NAME "/tmp/beamformer_output_pipe"

#elif defined(_WIN32)

#define OS_EXPORT_PIPE_NAME "\\\\.\\pipe\\beamformer_output_fifo"

#define OPEN_EXISTING        3
#define GENERIC_WRITE        0x40000000
#define FILE_MAP_ALL_ACCESS  0x000F001F

#define PIPE_TYPE_BYTE      0x00
#define PIPE_ACCESS_INBOUND 0x01

#define PIPE_WAIT   0x00
#define PIPE_NOWAIT 0x01

#define ERROR_NO_DATA            232L
#define ERROR_PIPE_NOT_CONNECTED 233L
#define ERROR_PIPE_LISTENING     536L

#define W32(r) __declspec(dllimport) r __stdcall
W32(b32)  CloseHandle(iptr);
W32(iptr) CreateFileA(c8 *, u32, u32, void *, u32, u32, void *);
W32(iptr) CreateNamedPipeA(c8 *, u32, u32, u32, u32, u32, u32, void *);
W32(b32)  DisconnectNamedPipe(iptr);
W32(i32)  GetLastError(void);
W32(iptr) MapViewOfFile(iptr, u32, u32, u32, u64);
W32(iptr) OpenFileMappingA(u32, b32, c8 *);
W32(b32)  ReadFile(iptr, u8 *, i32, i32 *, void *);
W32(i32)  RtlWaitOnAddress(void *, void *, uz, void *);
W32(void) Sleep(u32);
W32(void) UnmapViewOfFile(iptr);
W32(b32)  WriteFile(iptr, u8 *, i32, i32 *, void *);

#else
#error Unsupported Platform
#endif

#if defined(MATLAB_CONSOLE)
#define mexErrMsgIdAndTxt  mexErrMsgIdAndTxt_800
#define mexWarnMsgIdAndTxt mexWarnMsgIdAndTxt_800
void mexErrMsgIdAndTxt(const c8*, c8*, ...);
void mexWarnMsgIdAndTxt(const c8*, c8*, ...);
#define error_tag "ogl_beamformer_lib:error"
#define error_msg(...)   mexErrMsgIdAndTxt(error_tag, __VA_ARGS__)
#define warning_msg(...) mexWarnMsgIdAndTxt(error_tag, __VA_ARGS__)
#else
#define error_msg(...)
#define warning_msg(...)
#endif

#if defined(__linux__)
static void
os_wait_on_value(i32 *value, i32 current, u32 timeout_ms)
{
	struct timespec *timeout = 0, timeout_value;
	if (timeout_ms != U32_MAX) {
		timeout_value.tv_sec  = timeout_ms / 1000;
		timeout_value.tv_nsec = (timeout_ms % 1000) * 1000000;
		timeout = &timeout_value;
	}
	syscall(SYS_futex, value, FUTEX_WAIT, current, timeout, 0, 0);
}

static Pipe
os_open_read_pipe(char *name)
{
	mkfifo(name, 0660);
	return (Pipe){.file = open(name, O_RDONLY|O_NONBLOCK), .name = name};
}

static void
os_disconnect_pipe(Pipe p)
{
}

static void
os_close_pipe(iptr *file, char *name)
{
	if (file) close(*file);
	if (name) unlink(name);
	*file = INVALID_FILE;
}

static b32
os_wait_read_pipe(Pipe p, void *buf, iz read_size, u32 timeout_ms)
{
	struct pollfd pfd = {.fd = p.file, .events = POLLIN};
	iz total_read = 0;
	if (poll(&pfd, 1, timeout_ms) > 0) {
		iz r;
		do {
			 r = read(p.file, (u8 *)buf + total_read, read_size - total_read);
			 if (r > 0) total_read += r;
		} while (r != 0);
	}
	return total_read == read_size;
}

static BeamformerSharedMemory *
os_open_shared_memory_area(char *name)
{
	BeamformerSharedMemory *result = 0;
	i32 fd = shm_open(name, O_RDWR, S_IRUSR|S_IWUSR);
	if (fd > 0) {
		void *new = mmap(0, BEAMFORMER_SHARED_MEMORY_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
		if (new != MAP_FAILED)
			result = new;
		close(fd);
	}
	return result;
}

#elif defined(_WIN32)

static void
os_wait_on_value(i32 *value, i32 current, u32 timeout_ms)
{
	i64 *timeout = 0, timeout_value;
	if (timeout_ms != U32_MAX) {
		/* TODO(rnp): not sure about this one, but this is how wine converts the ms */
		timeout_value = -(i64)timeout_ms * 10000;
		timeout       = &timeout_value;
	}
	RtlWaitOnAddress(value, &current, sizeof(*value), timeout);
}

static Pipe
os_open_read_pipe(char *name)
{
	iptr file = CreateNamedPipeA(name, PIPE_ACCESS_INBOUND, PIPE_TYPE_BYTE|PIPE_NOWAIT, 1,
	                             0, 1024UL * 1024UL, 0, 0);
	return (Pipe){.file = file, .name = name};
}

static void
os_disconnect_pipe(Pipe p)
{
	DisconnectNamedPipe(p.file);
}

static void
os_close_pipe(iptr *file, char *name)
{
	if (file) CloseHandle(*file);
	*file = INVALID_FILE;
}

static b32
os_wait_read_pipe(Pipe p, void *buf, iz read_size, u32 timeout_ms)
{
	iz elapsed_ms = 0, total_read = 0;
	while (elapsed_ms <= timeout_ms && read_size != total_read) {
		u8 data;
		i32 read;
		b32 result = ReadFile(p.file, &data, 0, &read, 0);
		if (!result) {
			i32 error = GetLastError();
			if (error != ERROR_NO_DATA &&
			    error != ERROR_PIPE_LISTENING &&
			    error != ERROR_PIPE_NOT_CONNECTED)
			{
				/* NOTE: pipe is in a bad state; we will never read anything */
				break;
			}
			Sleep(PIPE_RETRY_PERIOD_MS);
			elapsed_ms += PIPE_RETRY_PERIOD_MS;
		} else {
			ReadFile(p.file, (u8 *)buf + total_read, read_size - total_read, &read, 0);
			total_read += read;
		}
	}
	return total_read == read_size;
}

static BeamformerSharedMemory *
os_open_shared_memory_area(char *name)
{
	BeamformerSharedMemory *result = 0;
	iptr h = OpenFileMappingA(FILE_MAP_ALL_ACCESS, 0, name);
	if (h != INVALID_FILE) {
		iptr view = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, BEAMFORMER_SHARED_MEMORY_SIZE);
		result = (BeamformerSharedMemory *)view;
		CloseHandle(h);
	}

	return result;
}

#endif

static b32
try_wait_sync(i32 *sync, i32 timeout_ms)
{
	b32 result = 0;
	for (;;) {
		i32 current = atomic_load(sync);
		if (current) {
			atomic_inc(sync, -current);
			result = 1;
			break;
		} else if (!timeout_ms) {
			break;
		}
		os_wait_on_value(sync, 0, timeout_ms);
	}
	return result;
}

static b32
check_shared_memory(char *name)
{
	if (!g_bp) {
		g_bp = os_open_shared_memory_area(name);
		if (!g_bp) {
			error_msg("failed to open shared memory area");
			return 0;
		}
	}
	return 1;
}

b32
set_beamformer_pipeline(char *shm_name, i32 *stages, i32 stages_count)
{
	if (stages_count > ARRAY_COUNT(g_bp->compute_stages)) {
		error_msg("maximum stage count is %lu", ARRAY_COUNT(g_bp->compute_stages));
		return 0;
	}

	if (!check_shared_memory(shm_name))
		return 0;

	for (i32 i = 0; i < stages_count; i++) {
		b32 valid = 0;
		#define X(en, number, sfn, nh, pn) if (number == stages[i]) valid = 1;
		COMPUTE_SHADERS
		#undef X

		if (!valid) {
			error_msg("invalid shader stage: %d", stages[i]);
			return 0;
		}

		g_bp->compute_stages[i] = stages[i];
	}
	g_bp->compute_stages_count = stages_count;

	return 1;
}

b32
beamformer_push_channel_mapping(char *shm_name, i16 *mapping, u32 count, i32 timeout_ms)
{
	b32 result = check_shared_memory(shm_name) && count <= ARRAY_COUNT(g_bp->channel_mapping);
	if (result) {
		BeamformWork *work = beamform_work_queue_push(&g_bp->external_work_queue);
		result = work && try_wait_sync(&g_bp->channel_mapping_sync, timeout_ms);
		if (result) {
			work->type = BW_UPLOAD_CHANNEL_MAPPING;
			work->completion_barrier = offsetof(BeamformerSharedMemory, channel_mapping_sync);
			mem_copy(g_bp->channel_mapping, mapping, count * sizeof(*mapping));
			beamform_work_queue_push_commit(&g_bp->external_work_queue);
		}
	}
	return result;
}

b32
beamformer_push_sparse_elements(char *shm_name, i16 *elements, u32 count, i32 timeout_ms)
{
	b32 result = check_shared_memory(shm_name) && count <= ARRAY_COUNT(g_bp->sparse_elements);
	if (result) {
		BeamformWork *work = beamform_work_queue_push(&g_bp->external_work_queue);
		result = work && try_wait_sync(&g_bp->sparse_elements_sync, timeout_ms);
		if (result) {
			work->type = BW_UPLOAD_SPARSE_ELEMENTS;
			work->completion_barrier = offsetof(BeamformerSharedMemory, sparse_elements_sync);
			mem_copy(g_bp->sparse_elements, elements, count * sizeof(*elements));
			beamform_work_queue_push_commit(&g_bp->external_work_queue);
		}
	}
	return result;
}

b32
set_beamformer_parameters(char *shm_name, BeamformerParametersV0 *new_bp)
{
	b32 result = 0;
	result |= beamformer_push_channel_mapping(shm_name, (i16 *)new_bp->channel_mapping,
	                                          ARRAY_COUNT(new_bp->channel_mapping), 0);
	result |= beamformer_push_sparse_elements(shm_name, (i16 *)new_bp->uforces_channels,
	                                          ARRAY_COUNT(new_bp->uforces_channels), 0);
	if (result) {
		mem_copy(&g_bp->raw, &new_bp->focal_depths, sizeof(g_bp->raw));
		g_bp->upload = 1;
	}

	return result;
}

b32
send_data(char *pipe_name, char *shm_name, void *data, u32 data_size)
{
	b32 result = check_shared_memory(shm_name) && data_size <= BEAMFORMER_MAX_RF_DATA_SIZE;
	if (result) {
		BeamformWork *work = beamform_work_queue_push(&g_bp->external_work_queue);
		if (work) {
			i32 current = atomic_load(&g_bp->raw_data_sync);
			if (current) {
				atomic_inc(&g_bp->raw_data_sync, -current);
				work->type = BW_UPLOAD_RF_DATA;
				work->completion_barrier = offsetof(BeamformerSharedMemory, raw_data_sync);
				mem_copy((u8 *)g_bp + BEAMFORMER_RF_DATA_OFF, data, data_size);
				g_bp->raw_data_size = data_size;

				beamform_work_queue_push_commit(&g_bp->external_work_queue);

				g_bp->start_compute = 1;
				/* TODO(rnp): set timeout on acquiring the lock instead of this */
				os_wait_on_value(&g_bp->raw_data_sync, 0, -1);
			} else {
				warning_msg("failed to acquire raw data lock");
				result = 0;
			}
		}
	}
	return result;
}

b32
beamform_data_synchronized(char *pipe_name, char *shm_name, void *data, u32 data_size,
                           uv4 output_points, f32 *out_data, i32 timeout_ms)
{
	if (!check_shared_memory(shm_name))
		return 0;

	if (output_points.x == 0) output_points.x = 1;
	if (output_points.y == 0) output_points.y = 1;
	if (output_points.z == 0) output_points.z = 1;
	output_points.w = 1;

	g_bp->raw.output_points.x = output_points.x;
	g_bp->raw.output_points.y = output_points.y;
	g_bp->raw.output_points.z = output_points.z;
	g_bp->export_next_frame   = 1;

	s8 export_name = s8(OS_EXPORT_PIPE_NAME);
	if (export_name.len > ARRAY_COUNT(g_bp->export_pipe_name)) {
		error_msg("export pipe name too long");
		return 0;
	}

	Pipe export_pipe = os_open_read_pipe(OS_EXPORT_PIPE_NAME);
	if (export_pipe.file == INVALID_FILE) {
		error_msg("failed to open export pipe");
		return 0;
	}

	for (u32 i = 0; i < export_name.len; i++)
		g_bp->export_pipe_name[i] = export_name.data[i];

	b32 result = send_data(pipe_name, shm_name, data, data_size);
	if (result) {
		iz output_size = output_points.x * output_points.y * output_points.z * sizeof(f32) * 2;
		result = os_wait_read_pipe(export_pipe, out_data, output_size, timeout_ms);
		if (!result)
			warning_msg("failed to read full export data from pipe");
	}

	os_disconnect_pipe(export_pipe);
	os_close_pipe(&export_pipe.file, export_pipe.name);

	return result;
}
