/* See LICENSE for license details. */
#ifndef BEAMFORMER_INTERNAL_H
#define BEAMFORMER_INTERNAL_H

// TODO(rnp):
// [ ]: move structs requiring beamformer_internal.h
//      - BeamformerCtx
//      - BeamformerFrame
//      - basically anything besides what is needed for beamformer_frame_step,
//        beamformer_load

typedef enum {
	GPUBufferCreateFlags_HostWritable = 1 << 0,
	GPUBufferCreateFlags_MemoryOnly   = 1 << 1,
} GPUBufferCreateFlags;

typedef struct { u64 value[1]; } VulkanHandle;

typedef struct {
	u64          gpu_pointer;
	i64          size;
	VulkanHandle buffer;
} GPUBuffer;

typedef struct {
	u64 gpu_heap_size;
	u64 gpu_heap_used;

	f32 timestamp_period_ns;
} GPUInfo;

///////////////////////////
// NOTE: vulkan layer API
DEBUG_EXPORT void vk_load(Arena *memory, Stream *error);

DEBUG_EXPORT GPUInfo *vk_gpu_info(void);

DEBUG_EXPORT void vk_buffer_allocate(GPUBuffer *, iz size, GPUBufferCreateFlags flags, Handle *export, s8 label);
DEBUG_EXPORT void vk_buffer_release(GPUBuffer *);
DEBUG_EXPORT void vk_buffer_range_upload(GPUBuffer *, void *data, u64 offset, u64 size, b32 non_temporal);
DEBUG_EXPORT u64  vk_round_up_to_sync_size(u64, u64 min);

// NOTE: temporary API
DEBUG_EXPORT b32 vk_buffer_needs_sync(GPUBuffer *);

DEBUG_EXPORT VulkanHandle vk_semaphore_create(Handle *export);

#endif /* BEAMFORMER_INTERNAL_H */
