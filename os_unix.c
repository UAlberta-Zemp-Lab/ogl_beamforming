/* See LICENSE for license details. */

/* NOTE(rnp): provides the platform layer for the beamformer. This code must
 * be provided by any platform the beamformer is ported to. */

#include "util.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef _DEBUG
static void *
os_get_module(char *name, Stream *e)
{
	void *result = dlopen(name, RTLD_NOW|RTLD_LOCAL|RTLD_NOLOAD);
	if (!result && e) {
		s8 errs[] = {s8("os_get_module(\""), c_str_to_s8(name), s8("\"): "),
		             c_str_to_s8(dlerror()), s8("\n")};
		stream_append_s8_array(e, errs, ARRAY_COUNT(errs));
	}
	return result;
}
#endif

static PLATFORM_WRITE_FILE_FN(os_write_file)
{
	while (raw.len) {
		size r = write(file, raw.data, raw.len);
		if (r < 0) return 0;
		raw = s8_cut_head(raw, r);
	}
	return 1;
}

static void __attribute__((noreturn))
os_fatal(s8 msg)
{
	os_write_file(STDERR_FILENO, msg);
	_exit(1);
	unreachable();
}

static PLATFORM_ALLOC_ARENA_FN(os_alloc_arena)
{
	Arena result;
	size pagesize = sysconf(_SC_PAGESIZE);
	if (capacity % pagesize != 0)
		capacity += pagesize - capacity % pagesize;

	size oldsize = old.end - old.beg;
	if (oldsize > capacity)
		return old;

	if (old.beg)
		munmap(old.beg, oldsize);

	result.beg = mmap(0, capacity, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
	if (result.beg == MAP_FAILED)
		os_fatal(s8("os_alloc_arena: couldn't allocate memory\n"));
	result.end = result.beg + capacity;
	return result;
}

static PLATFORM_CLOSE_FN(os_close)
{
	close(file);
}

static PLATFORM_OPEN_FOR_WRITE_FN(os_open_for_write)
{
	iptr result = open(fname, O_WRONLY|O_TRUNC);
	if (result == -1)
		result = INVALID_FILE;
	return result;
}

static PLATFORM_READ_WHOLE_FILE_FN(os_read_whole_file)
{
	s8 result = {0};

	struct stat sb;
	i32 fd = open(file, O_RDONLY);
	if (fd >= 0 && fstat(fd, &sb) >= 0) {
		result = s8_alloc(arena, sb.st_size);
		size rlen = read(fd, result.data, result.len);
		if (rlen != result.len)
			result = (s8){0};
	}
	if (fd >= 0) close(fd);

	return result;
}

static PLATFORM_WRITE_NEW_FILE_FN(os_write_new_file)
{
	iptr fd = open(fname, O_WRONLY|O_TRUNC|O_CREAT, 0600);
	if (fd == INVALID_FILE)
		return 0;
	b32 ret = os_write_file(fd, raw);
	close(fd);
	return ret;
}

static b32
os_file_exists(char *path)
{
	struct stat st;
	b32 result = stat(path, &st) == 0;
	return result;
}

static Pipe
os_open_named_pipe(char *name)
{
	mkfifo(name, 0660);
	return (Pipe){.file = open(name, O_RDONLY|O_NONBLOCK), .name = name};
}

static PLATFORM_READ_FILE_FN(os_read_file)
{
	size r = 0, total_read = 0;
	do {
		if (r != -1)
			total_read += r;
		r = read(file, buf + total_read, len - total_read);
	} while (r);
	return total_read;
}

static void *
os_open_shared_memory_area(char *name, size cap)
{
	i32 fd = shm_open(name, O_CREAT|O_RDWR, S_IRUSR|S_IWUSR);
	if (fd == -1)
		return NULL;

	if (ftruncate(fd, cap) == -1) {
		close(fd);
		return NULL;
	}

	void *new = mmap(NULL, cap, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);

	if (new == MAP_FAILED)
		return NULL;

	return new;
}

/* NOTE: complete garbage because there is no standarized copyfile() in POSix */
static b32
os_copy_file(char *name, char *new)
{
	b32 result = 0;
	struct stat sb;
	if (stat(name, &sb) < 0)
		return 0;

	i32 fd_old = open(name, O_RDONLY);
	i32 fd_new = open(new, O_WRONLY|O_TRUNC, sb.st_mode);

	if (fd_old < 0 || fd_new < 0)
		goto ret;
	u8 buf[4096];
	size copied = 0;
	while (copied != sb.st_size) {
		size r = read(fd_old, buf, ARRAY_COUNT(buf));
		if (r < 0) goto ret;
		size w = write(fd_new, buf, r);
		if (w < 0) goto ret;
		copied += w;
	}
	result = 1;
ret:
	if (fd_old != -1) close(fd_old);
	if (fd_new != -1) close(fd_new);
	return result;
}

static void *
os_load_library(char *name, char *temp_name, Stream *e)
{
	if (temp_name) {
		if (os_copy_file(name, temp_name))
			name = temp_name;
	}

	void *result = dlopen(name, RTLD_NOW|RTLD_LOCAL);
	if (!result && e) {
		s8 errs[] = {s8("os_load_library(\""), c_str_to_s8(name), s8("\"): "),
		             c_str_to_s8(dlerror()), s8("\n")};
		stream_append_s8_array(e, errs, ARRAY_COUNT(errs));
	}

	if (temp_name)
		unlink(temp_name);

	return result;
}

static void *
os_lookup_dynamic_symbol(void *h, char *name, Stream *e)
{
	void *result = 0;
	if (h) {
		result = dlsym(h, name);
		if (!result && e) {
			s8 errs[] = {s8("os_lookup_dynamic_symbol(\""), c_str_to_s8(name),
			             s8("\"): "), c_str_to_s8(dlerror()), s8("\n")};
			stream_append_s8_array(e, errs, ARRAY_COUNT(errs));
		}
	}
	return result;
}

static void
os_unload_library(void *h)
{
	/* NOTE: glibc is buggy gnuware so we need to check this */
	if (h)
		dlclose(h);
}

static PLATFORM_ADD_FILE_WATCH_FN(os_add_file_watch)
{
	s8 directory  = path;
	directory.len = s8_scan_backwards(path, '/');
	ASSERT(directory.len > 0);

	u64 hash = s8_hash(directory);
	FileWatchContext *fwctx = &platform->file_watch_context;
	FileWatchDirectory *dir = lookup_file_watch_directory(fwctx, hash);
	if (!dir) {
		ASSERT(path.data[directory.len] == '/');

		dir         = fwctx->directory_watches + fwctx->directory_watch_count++;
		dir->hash   = hash;
		dir->name   = push_s8_zero(a, directory);
		i32 mask    = IN_MOVED_TO|IN_CLOSE_WRITE;
		dir->handle = inotify_add_watch(fwctx->handle, (c8 *)dir->name.data, mask);
	}

	insert_file_watch(dir, s8_cut_head(path, dir->name.len + 1), user_data, callback);
}

i32 pthread_setname_np(pthread_t, char *);
static iptr
os_create_thread(Arena arena, iptr user_context, s8 name, platform_thread_entry_point_fn *fn)
{
	pthread_t result;
	pthread_create(&result, 0, (void *(*)(void *))fn, (void *)user_context);
	pthread_setname_np(result, (char *)name.data);
	return (iptr)result;
}

static iptr
os_create_sync_object(Arena *arena)
{
	sem_t *result = push_struct(arena, sem_t);
	sem_init(result, 0, 0);
	return (iptr)result;
}

static void
os_sleep_thread(iptr sync_handle)
{
	sem_wait((sem_t *)sync_handle);
}

static PLATFORM_WAKE_THREAD_FN(os_wake_thread)
{
	sem_post((sem_t *)sync_handle);
}

/* TODO(rnp): what do if not X11? */
iptr glfwGetGLXContext(iptr);

static iptr
os_get_native_gl_context(iptr window)
{
	return glfwGetGLXContext(window);
}
