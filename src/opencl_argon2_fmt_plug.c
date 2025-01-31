//-------------------------------------------------------------------------------------
// JtR OpenCL format to crack hashes from argon2.
//
// This software is copyright (c) 2023, Alain Espinosa <alainesp at gmail.com> and it
// is hereby released to the general public under the following terms:
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted.
//
// Based on https://gitlab.com/omos/argon2-gpu with some ideas from the CPU format.
//-------------------------------------------------------------------------------------
#ifdef HAVE_OPENCL

#if FMT_EXTERNS_H
extern struct fmt_main fmt_opencl_argon2;
#elif FMT_REGISTERS_H
john_register_one(&fmt_opencl_argon2);
#else

#include <string.h>
#include <assert.h>

#include "arch.h"
#include "params.h"
#include "common.h"
#include "formats.h"
#include "options.h"
#include "argon2.h"
#include "argon2_core.h"
#include "argon2_encoding.h"
#include "opencl_common.h"

#define FORMAT_LABEL            "argon2-opencl"
#define FORMAT_NAME             ""
#define ALGORITHM_NAME          "Blake2 OpenCL"
#define FORMAT_TAG_d            "$argon2d$"
#define FORMAT_TAG_i            "$argon2i$"
#define FORMAT_TAG_id           "$argon2id$"

#define BENCHMARK_COMMENT       ""
#define BENCHMARK_LENGTH        0x107
#define PLAINTEXT_LENGTH        100
#define BINARY_SIZE             256
#define BINARY_ALIGN            sizeof(uint32_t)
#define SALT_SIZE               64
#define SALT_ALIGN              sizeof(uint32_t)

struct fmt_main fmt_opencl_argon2;
#define MIN_KEYS_PER_CRYPT      1
#define MAX_KEYS_PER_CRYPT      (fmt_opencl_argon2.params.max_keys_per_crypt)

static struct fmt_tests tests[] = {
	{"$argon2d$v=19$m=4096,t=3,p=1$ZGFtYWdlX2RvbmU$w9w3s5/zV8+PcAZlJhnTCOE+vBkZssmZf6jOq3dKv50", "password"},
	{"$argon2i$v=19$m=4096,t=3,p=1$ZGFtYWdlX2RvbmU$N59QwnpxDQZRj1/cO6bqm408dD6Z2Z9LKYpwFJSPVKA", "password"},
	{"$argon2d$v=19$m=4096,t=3,p=1$c2hvcnRfc2FsdA$zMrTcOAOUje6UqObRVh84Pe1K6gumcDqqGzRM0ILzYmj", "sacrificed"},
	{"$argon2i$v=19$m=4096,t=3,p=1$c2hvcnRfc2FsdA$1l4kAwUdAApoCbFH7ghBEf7bsdrOQzE4axIJ3PV0Ncrd", "sacrificed"},
	{"$argon2d$v=19$m=16384,t=3,p=1$c2hvcnRfc2FsdA$TLSTPihIo+5F67Y1vJdfWdB9", "blessed_dead"},
	{"$argon2i$v=19$m=16384,t=3,p=1$c2hvcnRfc2FsdA$vvjDVog22A5x9eljmB+2yC8y", "blessed_dead"},
	{"$argon2d$v=19$m=16384,t=4,p=3$YW5vdGhlcl9zYWx0$yw93eMxC8REPAwbQ0e/q43jR9+RI9HI/DHP75uzm7tQfjU734oaI3dzcMWjYjHzVQD+J4+MG+7oyD8dN/PtnmPCZs+UZ67E+rkXJ/wTvY4WgXgAdGtJRrAGxhy4rD7d5G+dCpqhrog", "death_dying"},
	{"$argon2i$v=19$m=16384,t=4,p=3$YW5vdGhlcl9zYWx0$K7unxwO5aeuZCpnIJ06FMCRKod3eRg8oIRzQrK3E6mGbyqlTvvl47jeDWq/5drF1COJkEF9Ty7FWXJZHa+vqlf2YZGp/4qSlAvKmdtJ/6JZU32iQItzMRwcfujHE+PBjbL5uz4966A", "death_dying"},
	{"$argon2id$v=19$m=4096,t=3,p=1$c2hvcmF0X3NhbHQ$K6/V3qNPJwVmLb/ELiD8gKGskLaFv5OweJYwSKUW1hE", "password"},
	{"$argon2id$v=19$m=16384,t=4,p=3$c2hvcmF0X3NhbHQ$hG83oaWEcftTjbiWJxoQs6gKCModwYAC+9EK8j/DUsk", "sacrificed"},
	{NULL}
};
// TODO: Backport many of the improvements to the CPU format
// TODO: Optimize OpenCL code

struct argon2_salt {
	uint32_t t_cost, m_cost, lanes;
	uint32_t hash_size;
	uint32_t salt_length;
	char salt[SALT_SIZE];
	argon2_type type;
	argon2_version version;
};
static struct argon2_salt saved_salt;

static char (*saved_key)[PLAINTEXT_LENGTH + 1] = NULL;
static int *saved_len = NULL;
static uint8_t (*crypted)[BINARY_SIZE] = NULL;

// GPU functions and memory
#define ARGON2_NUM_TYPES 3
static cl_int cl_error = CL_SUCCESS;
static cl_kernel kernels[ARGON2_NUM_TYPES] = {NULL, NULL, NULL};
static cl_mem memory_buffer = NULL;

// CPU buffers to move data from and to the GPU
static uint8_t* blocks_in = NULL;
static uint8_t* blocks_out = NULL;

// Autotune params
struct kernel_run_params
{
	uint32_t lanes_per_block;
	uint32_t jobs_per_block;
};
static struct kernel_run_params* best_kernel_params = NULL;
static uint32_t max_salt_lanes = 0;
static uint32_t max_segment_blocks = 0;

#define THREADS_PER_LANE 32
#define ARGON2_REFS_PER_BLOCK 	(ARGON2_BLOCK_SIZE / (2 * sizeof(cl_uint)))

static int run_kernel_on_gpu(uint32_t lanes_per_block, size_t jobs_per_block)
{
        uint32_t pass, slice;
        uint32_t lanes = saved_salt.lanes;
        uint32_t passes = saved_salt.t_cost;

	assert(lanes > 0 && passes > 0 && saved_salt.m_cost > 0);

	if (lanes_per_block > lanes || lanes % lanes_per_block != 0)
		error_msg("Invalid lanes_per_block!");

	if (jobs_per_block > MAX_KEYS_PER_CRYPT || MAX_KEYS_PER_CRYPT % jobs_per_block != 0)
    	error_msg("Invalid jobs_per_block!");

	size_t global_range[2] = {THREADS_PER_LANE * lanes, MAX_KEYS_PER_CRYPT};
	size_t local_range[2] = {THREADS_PER_LANE * lanes_per_block, jobs_per_block};

	// Calculate memory size
	uint32_t segment_blocks = MAX(saved_salt.m_cost / (saved_salt.lanes * ARGON2_SYNC_POINTS), 2);
        // Copy data to GPU
	size_t jobSize = segment_blocks * ARGON2_SYNC_POINTS * saved_salt.lanes * ARGON2_BLOCK_SIZE;
	size_t copySize = saved_salt.lanes * 2 * ARGON2_BLOCK_SIZE;
	size_t zero3[3] = {0, 0, 0};
	size_t region3_in[3] = {copySize, MAX_KEYS_PER_CRYPT, 1};
	assert(gpu_id >= 0 && gpu_id < MAX_GPU_DEVICES && queue[gpu_id]);
	assert(blocks_in && blocks_out);
	HANDLE_CLERROR(clEnqueueWriteBufferRect(queue[gpu_id], memory_buffer, CL_FALSE, 
						zero3, zero3,
						region3_in,
						jobSize, 0, copySize, 0, blocks_in, 0, NULL, NULL), "Copy data to gpu");

	// Set parameters and execute kernel
	assert(saved_salt.type >= 0 && saved_salt.type < ARGON2_NUM_TYPES && kernels[saved_salt.type]);
        size_t shmemSize = THREADS_PER_LANE * lanes_per_block * jobs_per_block * sizeof(cl_uint) * 2;
	HANDLE_CLERROR(clSetKernelArg(kernels[saved_salt.type], 0, shmemSize, NULL), "Error setting kernel argument");
	HANDLE_CLERROR(clSetKernelArg(kernels[saved_salt.type], 2, sizeof(passes), &passes), "Error setting kernel argument");
	HANDLE_CLERROR(clSetKernelArg(kernels[saved_salt.type], 3, sizeof(lanes), &lanes), "Error setting kernel argument");
	HANDLE_CLERROR(clSetKernelArg(kernels[saved_salt.type], 4, sizeof(segment_blocks), &segment_blocks), "Error setting kernel argument");
	for (pass = 0; pass < passes; pass++)
		for (slice = 0; slice < ARGON2_SYNC_POINTS; slice++)
		{
			HANDLE_CLERROR(clSetKernelArg(kernels[saved_salt.type], 5, sizeof(pass), &pass), "Error setting kernel argument");
			HANDLE_CLERROR(clSetKernelArg(kernels[saved_salt.type], 6, sizeof(slice), &slice), "Error setting kernel argument");
			BENCH_CLERROR(clEnqueueNDRangeKernel(queue[gpu_id], kernels[saved_salt.type], 2, NULL, global_range, local_range, 0, NULL, NULL), "Run loop kernel");
			HANDLE_CLERROR(clFinish(queue[gpu_id]), "clFinish");
		}

        // Copy data from GPU
	copySize = saved_salt.lanes * ARGON2_BLOCK_SIZE;
	size_t buffer_origin3[3] = {jobSize - copySize, 0, 0};
	size_t region3_out[3] = {copySize, MAX_KEYS_PER_CRYPT, 1};
	HANDLE_CLERROR(clEnqueueReadBufferRect(queue[gpu_id], memory_buffer, CL_TRUE, 
						buffer_origin3, zero3,
						region3_out,
						jobSize, 0, copySize, 0, blocks_out, 0, NULL, NULL), "Copy data from gpu");

	return 0;
}


static void init(struct fmt_main *self)
{
	assert(gpu_id < MAX_GPU_DEVICES);
	opencl_prepare_dev(gpu_id);
}

static void done(void)
{
	MEM_FREE(saved_key);
	MEM_FREE(saved_len);
	MEM_FREE(crypted);

	MEM_FREE(blocks_in);
	MEM_FREE(blocks_out);

	// Release OpenCL resources
	if (program[gpu_id])
	{
		assert(memory_buffer && gpu_id >= 0 && gpu_id < MAX_GPU_DEVICES && program[gpu_id]);
		// Release memory
		HANDLE_CLERROR(clReleaseMemObject(memory_buffer), "Release GPU memory");
		memory_buffer = NULL;
		// Release kernels
		int i;
		for (i = 0; i < ARGON2_NUM_TYPES; i++)
		{
			assert(kernels[i]);
			HANDLE_CLERROR(clReleaseKernel(kernels[i]), "Release kernel");
			kernels[i] = NULL;
		}
		// Release program
		clReleaseProgram(program[gpu_id]);
		program[gpu_id] = NULL;

		MEM_FREE(best_kernel_params);
	}
}

// Autotune
static int is_power_of_two(uint32_t x)
{
    return (x & (x - 1)) == 0;
}
static uint32_t index_best_kernel_params(argon2_type type, uint32_t lanes, uint32_t segment_blocks)
{
	assert(best_kernel_params && type >= 0 && type < ARGON2_NUM_TYPES && 
		lanes > 0 && lanes <= max_salt_lanes &&
		segment_blocks > 0 && segment_blocks <= max_segment_blocks);

	return type * max_salt_lanes * max_segment_blocks + lanes * max_segment_blocks + segment_blocks;
}
static void autotune(argon2_type type, uint32_t lanes, uint32_t segment_blocks, cl_command_queue profiling_queue, cl_event* profiling_event)
{
	uint32_t index = index_best_kernel_params(type, lanes, segment_blocks);
	// If not initialize
	if (!best_kernel_params[index].lanes_per_block)
	{
		cl_ulong start_time, end_time, best_time = 0;
		uint32_t best_lanes_per_block = 1, best_jobs_per_block = 1, lpb, jpb;
		size_t global_range[2] = {THREADS_PER_LANE * lanes, MAX_KEYS_PER_CRYPT};
		HANDLE_CLERROR(clSetKernelArg(kernels[type], 3, sizeof(lanes), &lanes), "Error setting kernel argument");
		HANDLE_CLERROR(clSetKernelArg(kernels[type], 4, sizeof(segment_blocks), &segment_blocks), "Error setting kernel argument");

		assert(profiling_queue && profiling_event);
		// Get basic kernel execution time
		{
			// TODO: Check that 'local_range' is multiple of get_kernel_preferred_multiple(), particularly for CPUs
			size_t local_range[2] = {THREADS_PER_LANE * best_lanes_per_block, best_jobs_per_block};
			size_t shmemSize = THREADS_PER_LANE * best_lanes_per_block * best_jobs_per_block * sizeof(cl_uint) * 2;
			if (shmemSize > get_local_memory_size(gpu_id))
				printf("-- Overflowing %u KB / %u KB local GPU memory --\n", (uint32_t)(shmemSize / 1024), (uint32_t)(get_local_memory_size(gpu_id) / 1024));

			HANDLE_CLERROR(clSetKernelArg(kernels[type], 0, shmemSize, NULL), "Error setting local memory size");
			// Warm-up
			HANDLE_CLERROR(clEnqueueNDRangeKernel(profiling_queue, kernels[type], 2, NULL, global_range, local_range, 0, NULL, NULL), "Error on kernel");
			// Profile
			HANDLE_CLERROR(clEnqueueNDRangeKernel(profiling_queue, kernels[type], 2, NULL, global_range, local_range, 0, NULL, profiling_event), "Error on kernel");
			HANDLE_CLERROR(clFinish(profiling_queue), "Error profiling clFinish");

			HANDLE_CLERROR(clGetEventProfilingInfo(*profiling_event, CL_PROFILING_COMMAND_START, sizeof(cl_ulong), &start_time, NULL), "clGetEventProfilingInfo start");
			HANDLE_CLERROR(clGetEventProfilingInfo(*profiling_event, CL_PROFILING_COMMAND_END, sizeof(cl_ulong), &end_time, NULL), "clGetEventProfilingInfo end");
			best_time = end_time - start_time;
		}

		// Optimize 'lanes_per_block'
		if (lanes > 1 && is_power_of_two(lanes))
			for (lpb = 1; lpb <= lanes; lpb *= 2)
			{
				size_t local_range[2] = {THREADS_PER_LANE * lpb, best_jobs_per_block};
				size_t shmemSize = THREADS_PER_LANE * lpb * best_jobs_per_block * sizeof(cl_uint) * 2;

				if(CL_SUCCESS != clSetKernelArg(kernels[type], 0, shmemSize, NULL)) break;
				// Warm-up
				if(CL_SUCCESS != clEnqueueNDRangeKernel(profiling_queue, kernels[type], 2, NULL, global_range, local_range, 0, NULL, NULL)) break;
				// Profile
				if(CL_SUCCESS != clEnqueueNDRangeKernel(profiling_queue, kernels[type], 2, NULL, global_range, local_range, 0, NULL, profiling_event)) break;
				if(CL_SUCCESS != clFinish(profiling_queue)) break;

				HANDLE_CLERROR(clGetEventProfilingInfo(*profiling_event, CL_PROFILING_COMMAND_START, sizeof(cl_ulong), &start_time, NULL), "clGetEventProfilingInfo start");
				HANDLE_CLERROR(clGetEventProfilingInfo(*profiling_event, CL_PROFILING_COMMAND_END, sizeof(cl_ulong), &end_time, NULL), "clGetEventProfilingInfo end");

				// Select best params
				cl_ulong time = end_time - start_time;
				if (best_time > time)
				{
					best_time = time;
					best_lanes_per_block = lpb;
				}
			}

		// Optimize 'jobs_per_block'
		// Only tune jobs per block if we hit maximum lanes per block 
		if (best_lanes_per_block == lanes && MAX_KEYS_PER_CRYPT > 1 && is_power_of_two(MAX_KEYS_PER_CRYPT))// TODO: try 'best_lanes_per_block=lanes' when not power of 2
		{
			for (jpb = 1; jpb <= MAX_KEYS_PER_CRYPT; jpb *= 2)
			{
				size_t local_range[2] = {THREADS_PER_LANE * best_lanes_per_block, jpb};
				size_t shmemSize = THREADS_PER_LANE * best_lanes_per_block * jpb * sizeof(cl_uint) * 2;

				if(CL_SUCCESS != clSetKernelArg(kernels[type], 0, shmemSize, NULL)) break;
				// Warm-up
				if(CL_SUCCESS != clEnqueueNDRangeKernel(profiling_queue, kernels[type], 2, NULL, global_range, local_range, 0, NULL, NULL)) break;
				// Profile
				if(CL_SUCCESS != clEnqueueNDRangeKernel(profiling_queue, kernels[type], 2, NULL, global_range, local_range, 0, NULL, profiling_event)) break;
				if(CL_SUCCESS != clFinish(profiling_queue)) break;

				HANDLE_CLERROR(clGetEventProfilingInfo(*profiling_event, CL_PROFILING_COMMAND_START, sizeof(cl_ulong), &start_time, NULL), "clGetEventProfilingInfo start");
				HANDLE_CLERROR(clGetEventProfilingInfo(*profiling_event, CL_PROFILING_COMMAND_END, sizeof(cl_ulong), &end_time, NULL), "clGetEventProfilingInfo end");

				// Select best params
				cl_ulong time = end_time - start_time;
				if (best_time > time)
				{
					best_time = time;
					best_jobs_per_block = jpb;
				}
			}
		}

		// Save results
		best_kernel_params[index].lanes_per_block = best_lanes_per_block;
		best_kernel_params[index].jobs_per_block = best_jobs_per_block;

		if (best_time > 200000000ull)// 200ms
		{
			// Reduce MAX_KEYS_PER_CRYPT. This may not be needed
			//MAX_KEYS_PER_CRYPT = MAX(1, MAX_KEYS_PER_CRYPT * 200000000ull / best_time);
			//printf("Slow kernel %u ms MAX_KEYS_PER_CRYPT = %u\n", (uint32_t)(best_time / 1000000), MAX_KEYS_PER_CRYPT);
			// TODO: Need to change 'best_jobs_per_block' to a multiple of MAX_KEYS_PER_CRYPT for all
			//best_kernel_params[index].best_jobs_per_block = best_jobs_per_block = 1;
		}

		fprintf(stderr, "Autotune [type: %u, lanes: %u, segments: %u => (%u, %u) => %02u ms]\n", 
                        type, lanes, segment_blocks, 
		        best_lanes_per_block, best_jobs_per_block,
			(uint32_t)(best_time / 1000000));
	}
}
static void reset(struct db_main *db)
{
	assert(gpu_id >= 0 && gpu_id < MAX_GPU_DEVICES && db);
	int i;

        if (!program[gpu_id])
	{
		// Find max_lanes and max_memory_size
		MEM_FREE(best_kernel_params);
		max_salt_lanes = 0;
		max_segment_blocks = 0;
		size_t max_memory_size = 0;

		// Iterate on all salts
		struct db_salt* curr_salt = db->salts;
		for (i = 0; i < db->salt_count; i++)
		{
			assert(curr_salt && curr_salt->salt);
			struct argon2_salt* salt = (struct argon2_salt*)curr_salt->salt;

			uint32_t segment_blocks = MAX(salt->m_cost / (salt->lanes * ARGON2_SYNC_POINTS), 2);
			if (max_segment_blocks < segment_blocks)
				max_segment_blocks = segment_blocks;
			size_t memory_size = ((size_t)segment_blocks) * ARGON2_SYNC_POINTS * salt->lanes * ARGON2_BLOCK_SIZE;
			if (max_salt_lanes < salt->lanes)
				max_salt_lanes = salt->lanes;
			if (max_memory_size < memory_size)
				max_memory_size = memory_size;

			curr_salt = curr_salt->next;
		}
		assert(max_salt_lanes > 0 && max_memory_size > 0);
		// Autotune saved params
		best_kernel_params = mem_calloc(ARGON2_NUM_TYPES * max_salt_lanes * max_segment_blocks, sizeof(struct kernel_run_params));

		//----------------------------------------------------------------------------------------------------------------------------
		// Create OpenCL objects
		//----------------------------------------------------------------------------------------------------------------------------
		// Load GWS from config/command line
		opencl_get_user_preferences(FORMAT_NAME);
		if (global_work_size)
		{
			MAX_KEYS_PER_CRYPT = MAX(1, global_work_size / (THREADS_PER_LANE * max_salt_lanes));
			printf("\nCustom GWS result on MAX_KEYS_PER_CRYPT = %u", MAX_KEYS_PER_CRYPT);
		}
		assert(!saved_key && !saved_len && !crypted);
		saved_key = mem_calloc(MAX_KEYS_PER_CRYPT, sizeof(*saved_key));
		saved_len = mem_calloc(MAX_KEYS_PER_CRYPT, sizeof(int));
		crypted = mem_calloc(MAX_KEYS_PER_CRYPT, BINARY_SIZE);
		max_memory_size *= MAX_KEYS_PER_CRYPT;

		assert(!blocks_in && !blocks_out && !memory_buffer);
		// Manage GPU memory
		do {
			// CPU memory to transfer to and from the GPU
			blocks_in = mem_calloc_align(MAX_KEYS_PER_CRYPT * max_salt_lanes * 2 * ARGON2_BLOCK_SIZE, sizeof(uint8_t), MEM_ALIGN_PAGE);
			blocks_out = mem_calloc_align(MAX_KEYS_PER_CRYPT * max_salt_lanes * ARGON2_BLOCK_SIZE, sizeof(uint8_t), MEM_ALIGN_PAGE);

			// Create main GPU memory
			memory_buffer = clCreateBuffer(context[gpu_id], CL_MEM_READ_WRITE, max_memory_size, NULL, &cl_error);
			printf("\nTrying to use %zu MB / %u MB GPU memory. Max Allocation: %u MB\n", max_memory_size / 1048576, 
					(uint32_t)(get_global_memory_size(gpu_id) / 1048576),
					(uint32_t)(get_max_mem_alloc_size(gpu_id) / 1048576));

			// Something like this reduce too much performance on Nvidia: get_max_mem_alloc_size(gpu_id)
			//
			// The best option is to try and try again
			if(cl_error != CL_SUCCESS)
			{
				max_memory_size /= 2;
				MAX_KEYS_PER_CRYPT /= 2;
				MEM_FREE(blocks_in);
				MEM_FREE(blocks_out);
			}
		} while(cl_error != CL_SUCCESS);

		assert(MAX_KEYS_PER_CRYPT >= 1);
		
		// Create and build opencl kernels
		opencl_init("$JOHN/opencl/argon2_kernels_include.cl", gpu_id, NULL);

		// Select opencl kernel
		char kernel_name[32];
		for (i = 0; i < ARGON2_NUM_TYPES; i++)
		{
			snprintf(kernel_name, sizeof(kernel_name), "argon2_kernel_segment_%i", i);	
			assert(!kernels[i]);
			kernels[i] = clCreateKernel(program[gpu_id], kernel_name, &cl_error);
			HANDLE_CLERROR(cl_error, "Error creating kernel");
			// Set opencl kernel parameters
			HANDLE_CLERROR(clSetKernelArg(kernels[i], 1, sizeof(memory_buffer), &memory_buffer), "Error setting kernel argument");
		}
		//--------------------------------------------------------------------------------------------------------------------------
	}
	assert(program[gpu_id] && blocks_in && blocks_out && memory_buffer && kernels[0] && kernels[1] && kernels[2]);

	//-----------------------------------------------------------------------------------------------------------
	// Autotune
	//-----------------------------------------------------------------------------------------------------------
	// Set common params
	uint32_t ZERO = 0;
	uint32_t PASSES = 1;
	for (i = 0; i < ARGON2_NUM_TYPES; i++)
	{
		HANDLE_CLERROR(clSetKernelArg(kernels[i], 2, sizeof(PASSES), &PASSES), "Error setting kernel argument");
		HANDLE_CLERROR(clSetKernelArg(kernels[i], 5, sizeof(ZERO), &ZERO), "Error setting kernel argument");
		HANDLE_CLERROR(clSetKernelArg(kernels[i], 6, sizeof(ZERO), &ZERO), "Error setting kernel argument");
	}
	// Create OpenCL profiling objects
	cl_command_queue profiling_queue = clCreateCommandQueue(context[gpu_id], devices[gpu_id], CL_QUEUE_PROFILING_ENABLE, &cl_error);
	HANDLE_CLERROR(cl_error, "clCreateCommandQueue profiling");
	cl_event profiling_event = clCreateUserEvent(context[gpu_id], &cl_error);
	HANDLE_CLERROR(cl_error, "clCreateUserEvent profiling");

	// Iterate on all salts and autotuned for each one
	struct db_salt* curr_salt = db->salts;
	for (i = 0; i < db->salt_count; i++)
	{
		struct argon2_salt* salt = (struct argon2_salt*)curr_salt->salt;
		autotune(salt->type, salt->lanes, MAX(salt->m_cost / (salt->lanes * ARGON2_SYNC_POINTS), 2), profiling_queue, &profiling_event);
		curr_salt = curr_salt->next;
	}
	// Release profiling objects
	HANDLE_CLERROR(clReleaseCommandQueue(profiling_queue), "Releasing Profiling CommandQueue");
	clReleaseEvent(profiling_event);
	//-----------------------------------------------------------------------------------------------------------
}

// Ciphertext managment
static void ctx_init(argon2_context *ctx)
{
	static uint8_t out[BINARY_SIZE];
	static uint8_t salt[SALT_SIZE];

	ctx->adlen = 0;
	ctx->saltlen = SALT_SIZE;
	ctx->outlen = BINARY_SIZE;

	ctx->out = out;
	ctx->salt = salt;
}

static int valid(char *ciphertext, struct fmt_main *self)
{
	argon2_context ctx;
	int res;

	ctx_init(&ctx);

	if (!strncmp(ciphertext, FORMAT_TAG_d, sizeof(FORMAT_TAG_d)-1))
		res = argon2_decode_string(&ctx, ciphertext, Argon2_d);
	else if (!strncmp(ciphertext, FORMAT_TAG_id, sizeof(FORMAT_TAG_id)-1))
		res = argon2_decode_string(&ctx, ciphertext, Argon2_id);
	else if (!strncmp(ciphertext, FORMAT_TAG_i, sizeof(FORMAT_TAG_i)-1))
		res = argon2_decode_string(&ctx, ciphertext, Argon2_i);
	else
		return 0;

	if (res != ARGON2_OK || ctx.outlen < 8)
	  return 0;

	// TODO: Support ARGON2_VERSION_10
	if (ctx.version == ARGON2_VERSION_10)
	{
		printf("Format doesn't support ARGON2_VERSION_10\n");
		return 0;
	}

	return 1;
}

static void set_key(char *key, int index)
{
	assert(key);
	assert(index >= 0 && index < MAX_KEYS_PER_CRYPT);
	saved_len[index] = strnzcpyn(saved_key[index], key, sizeof(*saved_key));
}

static char *get_key(int index)
{
	assert(index >= 0 && index < MAX_KEYS_PER_CRYPT);
	return saved_key[index];
}


static void *get_binary(char *ciphertext)
{
	assert(ciphertext);

	static char out[BINARY_SIZE];
	argon2_context ctx;

	ctx_init(&ctx);

	if (!strncmp(ciphertext, FORMAT_TAG_d, sizeof(FORMAT_TAG_d)-1))
		argon2_decode_string(&ctx, ciphertext, Argon2_d);
	else if (!strncmp(ciphertext, FORMAT_TAG_id, sizeof(FORMAT_TAG_id)-1))
		argon2_decode_string(&ctx, ciphertext, Argon2_id);
	else
		argon2_decode_string(&ctx, ciphertext, Argon2_i);

	assert(ctx.outlen <= BINARY_SIZE);

	memset(out, 0, BINARY_SIZE);
	memcpy(out, ctx.out, ctx.outlen);

	return out;
}

static void *get_salt(char *ciphertext)
{
	assert(ciphertext);

	static struct argon2_salt salt;
	argon2_context ctx;

	memset(&salt, 0, sizeof(salt));

	ctx_init(&ctx);

	if (!strncmp(ciphertext, FORMAT_TAG_d, sizeof(FORMAT_TAG_d)-1))
	{
		argon2_decode_string(&ctx, ciphertext, Argon2_d);
		salt.type = Argon2_d;
	}
	else if (!strncmp(ciphertext, FORMAT_TAG_id, sizeof(FORMAT_TAG_id)-1))
	{
		argon2_decode_string(&ctx, ciphertext, Argon2_id);
		salt.type = Argon2_id;
	}
	else
	{
		argon2_decode_string(&ctx, ciphertext, Argon2_i);
		salt.type = Argon2_i;
	}

	assert(ctx.outlen <= BINARY_SIZE);
	assert(ctx.saltlen <= SALT_SIZE);
	assert(ctx.m_cost > 0 && ctx.t_cost > 0 && ctx.lanes > 0);

	salt.version = ctx.version;
	salt.salt_length = ctx.saltlen;
	salt.m_cost = ctx.m_cost;
	salt.t_cost = ctx.t_cost;
	salt.lanes = ctx.lanes;
	salt.hash_size = ctx.outlen;
	memcpy(salt.salt, ctx.salt, ctx.saltlen);

	return (void *)&salt;
}


static void set_salt(void *salt)
{
	assert(salt);
	memcpy(&saved_salt, salt, sizeof(struct argon2_salt));
}

// Compare result hashes with db hashes
static int cmp_all(void *binary, int count)
{
	return 1;
}

static int cmp_one(void *binary, int index)
{
	assert(binary && index >=0 && index < MAX_KEYS_PER_CRYPT);
	return !memcmp(binary, crypted[index],  saved_salt.hash_size);
}

static int cmp_exact(char *source, int index)
{
	return 1;
}

static int crypt_all(int *pcount, struct db_salt *salt)
{
	int i;
	const int count = *pcount;
	assert(count > 0 && count <= MAX_KEYS_PER_CRYPT);
	struct argon2_salt* assert_salt = salt->salt;
	assert(salt && !memcmp(assert_salt, &saved_salt, sizeof(struct argon2_salt)));

	// Same context everywhere
	argon2_context context;
	context.out = (uint8_t *)NULL;
	context.secret = NULL;
        context.secretlen = 0;
        context.ad = NULL;
        context.adlen = 0;
	context.flags = ARGON2_DEFAULT_FLAGS;
	context.pseudo_rands = NULL;

	// Pre-processing on the CPU
	//ProcessingUnit::setPassword(size_t index, const void *pw, size_t pwSize)
	for (i = 0; i < count; i++)
	{
                // argon2_context initialization
                context.outlen = (uint32_t)saved_salt.hash_size;
                context.pwd = (uint8_t *)saved_key[i];
                context.pwdlen = (uint32_t)saved_len[i];
                context.salt = (uint8_t *)saved_salt.salt;
                context.saltlen = (uint32_t)saved_salt.salt_length;
                context.t_cost = saved_salt.t_cost;
                context.m_cost = saved_salt.m_cost;
                context.lanes = saved_salt.lanes;
                context.threads = saved_salt.lanes;
                context.version = saved_salt.version;
                context.memory = blocks_in + i * saved_salt.lanes * 2 * ARGON2_BLOCK_SIZE;

                /* 3. Initialization: Hashing inputs, allocating memory, filling first blocks */
                opencl_argon2_initialize(&context, saved_salt.type);
	}

	// Run on the GPU
	uint32_t index = index_best_kernel_params(saved_salt.type, saved_salt.lanes, MAX(saved_salt.m_cost / (saved_salt.lanes * ARGON2_SYNC_POINTS), 2));
	assert(best_kernel_params[index].lanes_per_block && best_kernel_params[index].jobs_per_block);
	run_kernel_on_gpu(best_kernel_params[index].lanes_per_block, best_kernel_params[index].jobs_per_block);

	// Post-processing on CPU
	// ProcessingUnit::getHash()
	// TODO: nicify this (or move it into the kernel (I mean, we currently have all lanes in one work-group...)
	for (i = 0; i < count; i++)
	{
		uint32_t l;
		size_t j;
			
		const block* cursor = (const block*)(blocks_out + i * saved_salt.lanes * ARGON2_BLOCK_SIZE);
		block xored = *cursor;
		for (l = 1; l < saved_salt.lanes; l++) {
			++cursor;
			for (j = 0; j < ARGON2_BLOCK_SIZE / 8; j++) 
				xored.v[j] ^= cursor->v[j];
		}

		// TODO: Check if we need to save data as little-endian before this call
		blake2b_long(crypted[i], saved_salt.hash_size, &xored, ARGON2_BLOCK_SIZE);
	}

	return count;
}

// Hash hash
#define COMMON_GET_HASH_VAR crypted
#include "common-get-hash.h"

static int salt_hash(void *_salt)
{
	uint32_t i;
	struct argon2_salt *salt = (struct argon2_salt*)_salt;
	unsigned int hash = 0;
	char *p = salt->salt;

	for (i = 0; i < salt->salt_length; i++)
	{
		hash <<= 1;
		hash += (unsigned char)*p++;
		if (hash >> SALT_HASH_LOG)
		{
			hash ^= hash >> SALT_HASH_LOG;
			hash &= (SALT_HASH_SIZE - 1);
		}
	}

	hash ^= hash >> SALT_HASH_LOG;
	hash &= (SALT_HASH_SIZE - 1);

	return hash;
}

// Tunable costs
#if FMT_MAIN_VERSION > 11
static unsigned int tunable_cost_t(void *_salt)
{
	assert(_salt);
	struct argon2_salt *salt = (struct argon2_salt *)_salt;
	return salt->t_cost;
}

static unsigned int tunable_cost_m(void *_salt)
{
	assert(_salt);
	struct argon2_salt *salt = (struct argon2_salt *)_salt;
	return salt->m_cost;
}

static unsigned int tunable_cost_p(void *_salt)
{
	assert(_salt);
	struct argon2_salt *salt = (struct argon2_salt *)_salt;
	return salt->lanes;
}

static unsigned int tunable_cost_type(void *_salt)
{
	assert(_salt);
	struct argon2_salt *salt = (struct argon2_salt *)_salt;
	return (int)salt->type;
}
#endif

struct fmt_main fmt_opencl_argon2 = {
	{
		FORMAT_LABEL,
		FORMAT_NAME,
		ALGORITHM_NAME,
		BENCHMARK_COMMENT,
		BENCHMARK_LENGTH,
		0,
		PLAINTEXT_LENGTH,
		BINARY_SIZE,
		BINARY_ALIGN,
		sizeof(struct argon2_salt),
		SALT_ALIGN,
		MIN_KEYS_PER_CRYPT,
		256,// MAX_KEYS_PER_CRYPT,
		FMT_CASE | FMT_8_BIT,
		{
			"t",
			"m",
			"p",
			"type [0:Argon2d 1:Argon2i 2:Argon2id]"
		},
		{0},
		tests
	}, {
		init,
		done,
		reset,
		fmt_default_prepare,
		valid,
		fmt_default_split,
		get_binary,
		get_salt,
		{
			tunable_cost_t,
			tunable_cost_m,
			tunable_cost_p,
			tunable_cost_type,
		},
		fmt_default_source,
		{
			fmt_default_binary_hash_0,
			fmt_default_binary_hash_1,
			fmt_default_binary_hash_2,
			fmt_default_binary_hash_3,
			fmt_default_binary_hash_4,
			fmt_default_binary_hash_5,
			fmt_default_binary_hash_6
		},
		salt_hash,
		NULL,
		set_salt,
		set_key,
		get_key,
		fmt_default_clear_keys,
		crypt_all,
		{
#define COMMON_GET_HASH_LINK
#include "common-get-hash.h"
		},
		cmp_all,
		cmp_one,
		cmp_exact
	}
};

#endif
#endif
