/* See LICENSE for license details. */
#define BEAMFORMER_SHARED_MEMORY_VERSION (14UL)

typedef struct BeamformerFrame     BeamformerFrame;
typedef struct ShaderReloadContext ShaderReloadContext;

typedef enum {
	BeamformerWorkKind_Compute,
	BeamformerWorkKind_ComputeIndirect,
	BeamformerWorkKind_CreateFilter,
	BeamformerWorkKind_ReloadShader,
	BeamformerWorkKind_ExportBuffer,
	BeamformerWorkKind_UploadBuffer,
} BeamformerWorkKind;

/* TODO(rnp): this is massively bloating the queue; think of some other
 * way to communicate these to the beamformer */
typedef struct {
	union {
		#define X(kind, ...) struct {__VA_ARGS__ ;} kind;
		BEAMFORMER_FILTER_KIND_LIST(f32, ;)
		#undef X
	};
	f32 sampling_frequency;
	b16 complex;
} BeamformerFilterParameters;

typedef struct {
	BeamformerFilterKind       kind;
	BeamformerFilterParameters parameters;
	u8 filter_slot;
	u8 parameter_block;
	static_assert(BeamformerFilterSlots            <= 255, "CreateFilterContext only supports 255 filter slots");
	static_assert(BeamformerMaxParameterBlockSlots <= 255, "CreateFilterContext only supports 255 parameter blocks");
} BeamformerCreateFilterContext;

typedef enum {
	BeamformerExportKind_BeamformedData,
	BeamformerExportKind_Stats,
} BeamformerExportKind;

typedef struct {
	BeamformerExportKind kind;
	u32 size;
} BeamformerExportContext;

#define BEAMFORMER_SHARED_MEMORY_LOCKS \
	X(ScratchSpace)    \
	X(UploadRF)        \
	X(ExportSync)      \
	X(DispatchCompute)

#define X(name) BeamformerSharedMemoryLockKind_##name,
typedef enum {BEAMFORMER_SHARED_MEMORY_LOCKS BeamformerSharedMemoryLockKind_Count} BeamformerSharedMemoryLockKind;
#undef X

typedef struct {
	BeamformerFrame *frame;
	u32              parameter_block;
} BeamformerComputeWorkContext;

typedef struct {
	BeamformerViewPlaneTag view_plane;
	u32                    parameter_block;
} BeamformerComputeIndirectWorkContext;

/* NOTE: discriminated union based on type */
typedef struct {
	BeamformerWorkKind kind;
	BeamformerSharedMemoryLockKind lock;
	union {
		void                                 *generic;
		BeamformerComputeWorkContext          compute_context;
		BeamformerComputeIndirectWorkContext  compute_indirect_context;
		BeamformerCreateFilterContext         create_filter_context;
		BeamformerExportContext               export_context;
		ShaderReloadContext                  *shader_reload_context;
	};
} BeamformWork;

typedef struct {
	union {
		u64 queue;
		struct {u32 widx, ridx;};
	};
	BeamformWork work_items[1 << 6];
} BeamformWorkQueue;

#define BEAMFORMER_SHARED_MEMORY_SIZE             (GB(2))
#define BEAMFORMER_SHARED_MEMORY_MAX_SCRATCH_SIZE (BEAMFORMER_SHARED_MEMORY_SIZE - \
                                                   sizeof(BeamformerSharedMemory) - \
                                                   sizeof(BeamformerParameterBlock))

#define X(name, id) BeamformerLiveImagingDirtyFlags_##name = (1 << id),
typedef enum {BEAMFORMER_LIVE_IMAGING_DIRTY_FLAG_LIST} BeamformerLiveImagingDirtyFlags;
#undef X

#define BEAMFORMER_PARAMETER_BLOCK_REGION_LIST \
	X(ComputePipeline, pipeline)        \
	X(ChannelMapping,  channel_mapping) \
	X(FocalVectors,    focal_vectors)   \
	X(Parameters,      parameters)      \
	X(SparseElements,  sparse_elements)

typedef enum {
	#define X(k, ...) BeamformerParameterBlockRegion_##k,
	BEAMFORMER_PARAMETER_BLOCK_REGION_LIST
	#undef X
	BeamformerParameterBlockRegion_Count
} BeamformerParameterBlockRegions;

typedef union {
	u8 filter_slot;
} BeamformerShaderParameters;

typedef struct {
	BeamformerShaderKind       shaders[BeamformerMaxComputeShaderStages];
	BeamformerShaderParameters parameters[BeamformerMaxComputeShaderStages];
	u32                        program_indices[BeamformerMaxComputeShaderStages];
	u32                        shader_count;
	BeamformerDataKind         data_kind;
} BeamformerComputePipeline;

typedef struct {
	alignas(16) union {
		BeamformerParameters parameters;
		struct {
			BeamformerParametersHead parameters_head;
			BeamformerUIParameters   parameters_ui;
		};
	};

	/* NOTE(rnp): signals to the beamformer that a subregion of a block has been updated */
	u32 dirty_regions;
	static_assert(BeamformerParameterBlockRegion_Count <= 32, "only 32 parameter block regions supported");

	BeamformerComputePipeline pipeline;

	alignas(16) i16 channel_mapping[BeamformerMaxChannelCount];
	alignas(16) i16 sparse_elements[BeamformerMaxChannelCount];
	/* NOTE(rnp): interleaved transmit angle, focal depth pairs */
	alignas(16) v2  focal_vectors[BeamformerMaxChannelCount];
} BeamformerParameterBlock;
static_assert(sizeof(BeamformerParameterBlock) % alignof(BeamformerParameterBlock) == 0,
              "sizeof(BeamformerParametersBlock) must be a multiple of its alignment");

#define X(k, field) [BeamformerParameterBlockRegion_##k] = offsetof(BeamformerParameterBlock, field),
read_only global u16 BeamformerParameterBlockRegionOffsets[BeamformerParameterBlockRegion_Count] = {
	BEAMFORMER_PARAMETER_BLOCK_REGION_LIST
};
#undef X

typedef struct {
	u32 version;

	/* NOTE(rnp): causes future library calls to fail.
	 * see note in beamformer_invalidate_shared_memory() */
	b32 invalid;

	/* NOTE(rnp): not used for locking on w32 but we can use these to peek at the status of
	 * the lock without leaving userspace. */
	i32 locks[BeamformerSharedMemoryLockKind_Count + BeamformerMaxParameterBlockSlots];

	/* NOTE(rnp): total number of parameter block regions the client has requested.
	 * used to calculate offset to scratch space and to track number of allocated
	 * semaphores on w32. Defaults to 1 but can be changed at runtime */
	u32 reserved_parameter_blocks;

	/* TODO(rnp): this is really sucky. we need a better way to communicate this */
	u32 scratch_rf_size;

	BeamformerLiveImagingParameters live_imaging_parameters;
	BeamformerLiveImagingDirtyFlags live_imaging_dirty_flags;

	BeamformWorkQueue external_work_queue;
} BeamformerSharedMemory;

function BeamformWork *
beamform_work_queue_pop(BeamformWorkQueue *q)
{
	BeamformWork *result = 0;

	static_assert(ISPOWEROF2(countof(q->work_items)), "queue capacity must be a power of 2");
	u64 val  = atomic_load_u64(&q->queue);
	u64 mask = countof(q->work_items) - 1;
	u64 widx = val       & mask;
	u64 ridx = val >> 32 & mask;

	if (ridx != widx)
		result = q->work_items + ridx;

	return result;
}

function void
beamform_work_queue_pop_commit(BeamformWorkQueue *q)
{
	atomic_add_u64(&q->queue, 0x100000000ULL);
}

function BeamformWork *
beamform_work_queue_push(BeamformWorkQueue *q)
{
	BeamformWork *result = 0;

	static_assert(ISPOWEROF2(countof(q->work_items)), "queue capacity must be a power of 2");
	u64 val  = atomic_load_u64(&q->queue);
	u64 mask = countof(q->work_items) - 1;
	u64 widx = val        & mask;
	u64 ridx = val >> 32  & mask;
	u64 next = (widx + 1) & mask;

	if (val & 0x80000000)
		atomic_and_u64(&q->queue, ~0x80000000);

	if (next != ridx) {
		result = q->work_items + widx;
		zero_struct(result);
	}

	return result;
}

function void
beamform_work_queue_push_commit(BeamformWorkQueue *q)
{
	atomic_add_u64(&q->queue, 1);
}

function BeamformerParameterBlock *
beamformer_parameter_block(BeamformerSharedMemory *sm, u32 block)
{
	assert(sm->reserved_parameter_blocks >= block);
	BeamformerParameterBlock *result = (typeof(result))((u8 *)(sm + 1) + block * sizeof(*result));
	return result;
}

function b32
beamformer_parameter_block_dirty(BeamformerSharedMemory *sm, u32 block)
{
	b32 result = beamformer_parameter_block(sm, block)->dirty_regions != 0;
	return result;
}

function BeamformerParameterBlock *
beamformer_parameter_block_lock(SharedMemoryRegion *sm, u32 block, i32 timeout_ms)
{
	assert(block < BeamformerMaxParameterBlockSlots);
	BeamformerSharedMemory   *b      = sm->region;
	BeamformerParameterBlock *result = 0;
	if (os_shared_memory_region_lock(sm, b->locks, BeamformerSharedMemoryLockKind_Count + (i32)block, (u32)timeout_ms))
		result = beamformer_parameter_block(sm->region, block);
	return result;
}

function void
beamformer_parameter_block_unlock(SharedMemoryRegion *sm, u32 block)
{
	assert(block < BeamformerMaxParameterBlockSlots);
	BeamformerSharedMemory *b = sm->region;
	os_shared_memory_region_unlock(sm, b->locks, BeamformerSharedMemoryLockKind_Count + (i32)block);
}

function Arena
beamformer_shared_memory_scratch_arena(BeamformerSharedMemory *sm)
{
	assert(sm->reserved_parameter_blocks > 0);
	BeamformerParameterBlock *last = beamformer_parameter_block(sm, sm->reserved_parameter_blocks);
	Arena result = {.beg = (u8 *)(last + 1), .end = (u8 *)sm + BEAMFORMER_SHARED_MEMORY_SIZE};
	result.beg = arena_aligned_start(result, KB(4));
	return result;
}

function void
mark_parameter_block_region_dirty(BeamformerSharedMemory *sm, u32 block, BeamformerParameterBlockRegions region)
{
	BeamformerParameterBlock *pb = beamformer_parameter_block(sm, block);
	atomic_or_u32(&pb->dirty_regions, 1 << region);
}

function void
mark_parameter_block_region_clean(BeamformerSharedMemory *sm, u32 block, BeamformerParameterBlockRegions region)
{
	BeamformerParameterBlock *pb = beamformer_parameter_block(sm, block);
	atomic_and_u32(&pb->dirty_regions, ~(1 << region));
}

function void
post_sync_barrier(SharedMemoryRegion *sm, BeamformerSharedMemoryLockKind lock, i32 *locks)
{
	/* NOTE(rnp): debug: here it is not a bug to release the lock if it
	 * isn't held but elswhere it is */
	DEBUG_DECL(if (locks[lock])) {
		os_shared_memory_region_unlock(sm, locks, (i32)lock);
	}
}
