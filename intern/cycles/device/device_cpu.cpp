/*
 * Copyright 2011-2013 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>
#include <string.h>

/* So ImathMath is included before our kernel_cpu_compat. */
#ifdef WITH_OSL
/* So no context pollution happens from indirectly included windows.h */
#  include "util_windows.h"
#  include <OSL/oslexec.h>
#endif

#include "device.h"
#include "device_intern.h"

#include "kernel.h"
#include "kernel_compat_cpu.h"
#include "kernel_types.h"
#include "kernel_globals.h"

#include "osl_shader.h"
#include "osl_globals.h"

#include "buffers.h"

#include "util_debug.h"
#include "util_foreach.h"
#include "util_function.h"
#include "util_logging.h"
#include "util_opengl.h"
#include "util_progress.h"
#include "util_system.h"
#include "util_thread.h"

CCL_NAMESPACE_BEGIN

/* Has to be outside of the class to be shared across template instantiations. */
static bool logged_architecture = false;

template<typename F>
class KernelFunctions {
public:
	KernelFunctions(F kernel_default,
	                F kernel_sse2,
	                F kernel_sse3,
	                F kernel_sse41,
	                F kernel_avx,
	                F kernel_avx2)
	{
		string architecture_name = "default";
		kernel = kernel_default;

		/* Silence potential warnings about unused variables
		 * when compiling without some architectures. */
		(void)kernel_sse2;
		(void)kernel_sse3;
		(void)kernel_sse41;
		(void)kernel_avx;
		(void)kernel_avx2;
#ifdef WITH_CYCLES_OPTIMIZED_KERNEL_AVX2
		if(system_cpu_support_avx2()) {
			architecture_name = "AVX2";
			kernel = kernel_avx2;
		}
		else
#endif
#ifdef WITH_CYCLES_OPTIMIZED_KERNEL_AVX
		if(system_cpu_support_avx()) {
			architecture_name = "AVX";
			kernel = kernel_avx;
		}
		else
#endif
#ifdef WITH_CYCLES_OPTIMIZED_KERNEL_SSE41
		if(system_cpu_support_sse41()) {
			architecture_name = "SSE4.1";
			kernel = kernel_sse41;
		}
		else
#endif
#ifdef WITH_CYCLES_OPTIMIZED_KERNEL_SSE3
		if(system_cpu_support_sse3()) {
			architecture_name = "SSE3";
			kernel = kernel_sse3;
		}
		else
#endif
#ifdef WITH_CYCLES_OPTIMIZED_KERNEL_SSE2
		if(system_cpu_support_sse2()) {
			architecture_name = "SSE2";
			kernel = kernel_sse2;
		}
#endif

		if(!logged_architecture) {
			VLOG(1) << "Will be using " << architecture_name << " kernels.";
			logged_architecture = true;
		}
	}

	inline F operator()() const {
		return kernel;
	}
protected:
	F kernel;
};

class CPUDevice : public Device
{
public:
	TaskPool task_pool;
	KernelGlobals kernel_globals;

#ifdef WITH_OSL
	OSLGlobals osl_globals;
#endif

	KernelFunctions<void(*)(KernelGlobals *, float *, unsigned int *, int, int, int, int, int)>   path_trace_kernel;
	KernelFunctions<void(*)(KernelGlobals *, uchar4 *, float *, float, int, int, int, int)>       convert_to_half_float_kernel;
	KernelFunctions<void(*)(KernelGlobals *, uchar4 *, float *, float, int, int, int, int)>       convert_to_byte_kernel;
	KernelFunctions<void(*)(KernelGlobals *, uint4 *, float4 *, float*, int, int, int, int, int)> shader_kernel;
	KernelFunctions<void(*)(KernelGlobals*, int, float**, int, int, int*, int*, int*, int*, float*, float*, float*, float*, int*)> filter_divide_shadow_kernel;
	KernelFunctions<void(*)(KernelGlobals*, int, float**, int, int, int, int, int*, int*, int*, int*, float*, float*, int*)>       filter_get_feature_kernel;
	KernelFunctions<void(*)(int, int, float*, float*, float*, float*, int*, int)>                                                  filter_combine_halves_kernel;
	KernelFunctions<void(*)(KernelGlobals*, int, float*, int, int, void*, int*)>                                      filter_construct_transform_kernel;
	KernelFunctions<void(*)(KernelGlobals*, int, float*, int, int, int, int, float*, void*, float*, int*, int*)>      filter_reconstruct_kernel;
	KernelFunctions<void(*)(KernelGlobals*, int, int, int, float*, int, int)>                                         filter_divide_combined_kernel;

	KernelFunctions<void(*)(int, int, float*, float*, float*, int*, int, int, float, float)> filter_nlm_calc_difference_kernel;
	KernelFunctions<void(*)(float*, float*, int*, int, int)>                                 filter_nlm_blur_kernel;
	KernelFunctions<void(*)(float*, float*, int*, int, int)>                                 filter_nlm_calc_weight_kernel;
	KernelFunctions<void(*)(int, int, float*, float*, float*, float*, int*, int, int)>       filter_nlm_update_output_kernel;
	KernelFunctions<void(*)(float*, float*, int*, int)>                                      filter_nlm_normalize_kernel;

	KernelFunctions<void(*)(int, int, float*, float*, int, void*, float*, float3*, int*, int*, int, int, int)>  filter_nlm_construct_gramian_kernel;
	KernelFunctions<void(*)(int, int, int, int, int, float*, void*, float*, float3*, int*, int)>                filter_finalize_kernel;

#define KERNEL_FUNCTIONS(name) \
	      KERNEL_NAME_EVAL(cpu, name), \
	      KERNEL_NAME_EVAL(cpu_sse2, name), \
	      KERNEL_NAME_EVAL(cpu_sse3, name), \
	      KERNEL_NAME_EVAL(cpu_sse41, name), \
	      KERNEL_NAME_EVAL(cpu_avx, name), \
	      KERNEL_NAME_EVAL(cpu_avx2, name)

	CPUDevice(DeviceInfo& info, Stats &stats, bool background)
	: Device(info, stats, background),
	  path_trace_kernel(KERNEL_FUNCTIONS(path_trace)),
	  convert_to_half_float_kernel(KERNEL_FUNCTIONS(convert_to_half_float)),
	  convert_to_byte_kernel(KERNEL_FUNCTIONS(convert_to_byte)),
	  shader_kernel(KERNEL_FUNCTIONS(shader)),
	  filter_divide_shadow_kernel(KERNEL_FUNCTIONS(filter_divide_shadow)),
	  filter_get_feature_kernel(KERNEL_FUNCTIONS(filter_get_feature)),
	  filter_combine_halves_kernel(KERNEL_FUNCTIONS(filter_combine_halves)),
	  filter_construct_transform_kernel(KERNEL_FUNCTIONS(filter_construct_transform)),
	  filter_reconstruct_kernel(KERNEL_FUNCTIONS(filter_reconstruct)),
	  filter_divide_combined_kernel(KERNEL_FUNCTIONS(filter_divide_combined)),
	  filter_nlm_calc_difference_kernel(KERNEL_FUNCTIONS(filter_nlm_calc_difference)),
	  filter_nlm_blur_kernel(KERNEL_FUNCTIONS(filter_nlm_blur)),
	  filter_nlm_calc_weight_kernel(KERNEL_FUNCTIONS(filter_nlm_calc_weight)),
	  filter_nlm_update_output_kernel(KERNEL_FUNCTIONS(filter_nlm_update_output)),
	  filter_nlm_normalize_kernel(KERNEL_FUNCTIONS(filter_nlm_normalize)),
	  filter_nlm_construct_gramian_kernel(KERNEL_FUNCTIONS(filter_nlm_construct_gramian)),
	  filter_finalize_kernel(KERNEL_FUNCTIONS(filter_finalize))
	{
#ifdef WITH_OSL
		kernel_globals.osl = &osl_globals;
#endif
		system_enable_ftz();
	}

	~CPUDevice()
	{
		task_pool.stop();
	}

	virtual bool show_samples() const
	{
		return (TaskScheduler::num_threads() == 1);
	}

	void mem_alloc(device_memory& mem, MemoryType /*type*/)
	{
		mem.device_pointer = mem.data_pointer;
		mem.device_size = mem.memory_size();
		stats.mem_alloc(mem.device_size);
	}

	void mem_copy_to(device_memory& /*mem*/)
	{
		/* no-op */
	}

	void mem_copy_from(device_memory& /*mem*/,
	                   int /*y*/, int /*w*/, int /*h*/,
	                   int /*elem*/)
	{
		/* no-op */
	}

	void mem_zero(device_memory& mem)
	{
		memset((void*)mem.device_pointer, 0, mem.memory_size());
	}

	void mem_free(device_memory& mem)
	{
		if(mem.device_pointer) {
			mem.device_pointer = 0;
			stats.mem_free(mem.device_size);
			mem.device_size = 0;
		}
	}

	void const_copy_to(const char *name, void *host, size_t size)
	{
		kernel_const_copy(&kernel_globals, name, host, size);
	}

	void tex_alloc(const char *name,
	               device_memory& mem,
	               InterpolationType interpolation,
	               ExtensionType extension)
	{
		VLOG(1) << "Texture allocate: " << name << ", "
		        << string_human_readable_number(mem.memory_size()) << " bytes. ("
		        << string_human_readable_size(mem.memory_size()) << ")";
		kernel_tex_copy(&kernel_globals,
		                name,
		                mem.data_pointer,
		                mem.data_width,
		                mem.data_height,
		                mem.data_depth,
		                interpolation,
		                extension);
		mem.device_pointer = mem.data_pointer;
		mem.device_size = mem.memory_size();
		stats.mem_alloc(mem.device_size);
	}

	void tex_free(device_memory& mem)
	{
		if(mem.device_pointer) {
			mem.device_pointer = 0;
			stats.mem_free(mem.device_size);
			mem.device_size = 0;
		}
	}

	void *osl_memory()
	{
#ifdef WITH_OSL
		return &osl_globals;
#else
		return NULL;
#endif
	}

	void thread_run(DeviceTask *task)
	{
		if(task->type == DeviceTask::RENDER)
			thread_render(*task);
		else if(task->type == DeviceTask::FILM_CONVERT)
			thread_film_convert(*task);
		else if(task->type == DeviceTask::SHADER)
			thread_shader(*task);
	}

	class CPUDeviceTask : public DeviceTask {
	public:
		CPUDeviceTask(CPUDevice *device, DeviceTask& task)
		: DeviceTask(task)
		{
			run = function_bind(&CPUDevice::thread_run, device, this);
		}
	};

	void non_local_means(int4 rect, float *image, float *weight, float *out, float *variance, float *difference, float *blurDifference, float *weightAccum, int r, int f, float a, float k_2)
	{
		int w = align_up(rect.z-rect.x, 4);
		int h = rect.w-rect.y;

		memset(weightAccum, 0, sizeof(float)*w*h);
		memset(out, 0, sizeof(float)*w*h);

		for(int i = 0; i < (2*r+1)*(2*r+1); i++) {
			int dy = i / (2*r+1) - r;
			int dx = i % (2*r+1) - r;

			int local_rect[4] = {max(0, -dx), max(0, -dy), rect.z-rect.x - max(0, dx), rect.w-rect.y - max(0, dy)};
			filter_nlm_calc_difference_kernel()(dx, dy, weight, variance, difference, local_rect, w, 0, a, k_2);
			filter_nlm_blur_kernel()(difference, blurDifference, local_rect, w, f);
			filter_nlm_calc_weight_kernel()(blurDifference, difference, local_rect, w, f);
			filter_nlm_blur_kernel()(difference, blurDifference, local_rect, w, f);
			filter_nlm_update_output_kernel()(dx, dy, blurDifference, image, out, weightAccum, local_rect, w, f);
		}

		int local_rect[4] = {0, 0, rect.z-rect.x, rect.w-rect.y};
		filter_nlm_normalize_kernel()(out, weightAccum, local_rect, w);
	}

	float* denoise_fill_buffer(KernelGlobals *kg, int sample, int4 rect, float** buffers, int* tile_x, int* tile_y, int *offsets, int *strides, int frames, int *frame_strides)
	{
		bool cross_denoise = kg->__data.film.denoise_cross;
		int w = align_up(rect.z - rect.x, 4), h = (rect.w - rect.y);
		int pass_stride = w*h*frames;
		int passes = cross_denoise? 28:22;
		float *filter_buffers = new float[passes*pass_stride];
		memset(filter_buffers, 0, sizeof(float)*passes*pass_stride);


		for(int frame = 0; frame < frames; frame++) {
			float *filter_buffer = filter_buffers + w*h*frame;
			float *buffer[9];
			for(int i = 0; i < 9; i++) {
				buffer[i] = buffers[i] + frame_strides[i]*frame;
			}
#ifdef WITH_CYCLES_DEBUG_FILTER
			DenoiseDebug debug((rect.z - rect.x), h, 34);
#endif


#define PASSPTR(i) (filter_buffer + (i)*pass_stride)

			/* ==== Step 1: Prefilter shadow feature. ==== */
			{
				/* Reuse some passes of the filter_buffer for temporary storage. */
				float *sampleV = PASSPTR(0), *sampleVV = PASSPTR(1), *bufferV = PASSPTR(2), *cleanV = PASSPTR(3);
				float *unfilteredA = PASSPTR(4), *unfilteredB = PASSPTR(5);
				float *nlm_temp1 = PASSPTR(10), *nlm_temp2 = PASSPTR(11), *nlm_temp3 = PASSPTR(12);

				/* Get the A/B unfiltered passes, the combined sample variance, the estimated variance of the sample variance and the buffer variance. */
				for(int y = rect.y; y < rect.w; y++) {
					for(int x = rect.x; x < rect.z; x++) {
						filter_divide_shadow_kernel()(kg, sample, buffer, x, y, tile_x, tile_y, offsets, strides, unfilteredA, sampleV, sampleVV, bufferV, &rect.x);
					}
				}
#ifdef WITH_CYCLES_DEBUG_FILTER
#define WRITE_DEBUG(name, var) debug.add_pass(string_printf("shadow_%s", name), var, 1, w);
				WRITE_DEBUG("unfilteredA", unfilteredA);
				WRITE_DEBUG("unfilteredB", unfilteredB);
				WRITE_DEBUG("bufferV", bufferV);
				WRITE_DEBUG("sampleV", sampleV);
				WRITE_DEBUG("sampleVV", sampleVV);
#endif

				/* Smooth the (generally pretty noisy) buffer variance using the spatial information from the sample variance. */
				non_local_means(rect, bufferV, sampleV, cleanV, sampleVV, nlm_temp1, nlm_temp2, nlm_temp3, 6, 3, 4.0f, 1.0f);
#ifdef WITH_CYCLES_DEBUG_FILTER
				WRITE_DEBUG("cleanV", cleanV);
#endif

				/* Use the smoothed variance to filter the two shadow half images using each other for weight calculation. */
				non_local_means(rect, unfilteredA, unfilteredB, sampleV, cleanV, nlm_temp1, nlm_temp2, nlm_temp3, 5, 3, 1.0f, 0.25f);
				non_local_means(rect, unfilteredB, unfilteredA, bufferV, cleanV, nlm_temp1, nlm_temp2, nlm_temp3, 5, 3, 1.0f, 0.25f);
#ifdef WITH_CYCLES_DEBUG_FILTER
				WRITE_DEBUG("filteredA", sampleV);
				WRITE_DEBUG("filteredB", bufferV);
#endif

				/* Estimate the residual variance between the two filtered halves. */
				for(int y = rect.y; y < rect.w; y++) {
					for(int x = rect.x; x < rect.z; x++) {
						filter_combine_halves_kernel()(x, y, NULL, sampleVV, sampleV, bufferV, &rect.x, 2);
					}
				}
#ifdef WITH_CYCLES_DEBUG_FILTER
				WRITE_DEBUG("residualV", sampleVV);
#endif

				/* Use the residual variance for a second filter pass. */
				non_local_means(rect, sampleV, bufferV, unfilteredA, sampleVV, nlm_temp1, nlm_temp2, nlm_temp3, 4, 2, 1.0f, 0.5f);
				non_local_means(rect, bufferV, sampleV, unfilteredB, sampleVV, nlm_temp1, nlm_temp2, nlm_temp3, 4, 2, 1.0f, 0.5f);
#ifdef WITH_CYCLES_DEBUG_FILTER
				WRITE_DEBUG("finalA", unfilteredA);
				WRITE_DEBUG("finalB", unfilteredB);
#endif

				/* Combine the two double-filtered halves to a final shadow feature image and associated variance. */
				for(int y = rect.y; y < rect.w; y++) {
					for(int x = rect.x; x < rect.z; x++) {
						filter_combine_halves_kernel()(x, y, PASSPTR(8), PASSPTR(9), unfilteredA, unfilteredB, &rect.x, 0);
					}
				}
#ifdef WITH_CYCLES_DEBUG_FILTER
				WRITE_DEBUG("final", PASSPTR(8));
				WRITE_DEBUG("finalV", PASSPTR(9));
				debug.write(string_printf("debugf_%dx%d.exr", tile_x[1], tile_y[1]));
#undef WRITE_DEBUG
#endif
			}

			/* ==== Step 2: Prefilter general features. ==== */
			{

				float *unfiltered = PASSPTR(16);
				float *nlm_temp1 = PASSPTR(17), *nlm_temp2 = PASSPTR(18), *nlm_temp3 = PASSPTR(19);
				/* Order in render buffers:
				 *   Normal[X, Y, Z] NormalVar[X, Y, Z] Albedo[R, G, B] AlbedoVar[R, G, B ] Depth DepthVar
				 *          0  1  2            3  4  5         6  7  8            9  10 11  12    13
				 *
				 * Order in denoise buffer:
				 *   Normal[X, XVar, Y, YVar, Z, ZVar] Depth DepthVar Shadow ShadowVar Albedo[R, RVar, G, GVar, B, BVar] Color[R, RVar, G, GVar, B, BVar]
				 *          0  1     2  3     4  5     6     7        8      9                10 11    12 13    14 15          16 17    18 19    20 21
				 *
				 * Order of processing: |NormalXYZ|Depth|AlbedoXYZ |
				 *                      |         |     |          | */
				int mean_from[]      = { 0, 1, 2,   6,    7,  8, 12 };
				int variance_from[]  = { 3, 4, 5,   9,   10, 11, 13 };
				int offset_to[]      = { 0, 2, 4,  10,   12, 14,  6 };
				for(int i = 0; i < 7; i++) {
					for(int y = rect.y; y < rect.w; y++) {
						for(int x = rect.x; x < rect.z; x++) {
							filter_get_feature_kernel()(kg, sample, buffer, mean_from[i], variance_from[i], x, y, tile_x, tile_y, offsets, strides, unfiltered, PASSPTR(offset_to[i]+1), &rect.x);
						}
					}
					non_local_means(rect, unfiltered, unfiltered, PASSPTR(offset_to[i]), PASSPTR(offset_to[i]+1), nlm_temp1, nlm_temp2, nlm_temp3, 2, 2, 1, 0.25f);
#ifdef WITH_CYCLES_DEBUG_FILTER
#define WRITE_DEBUG(name, var) debug.add_pass(string_printf("f%d_%s", i, name), var, 1, w);
					WRITE_DEBUG("unfiltered", unfiltered);
					WRITE_DEBUG("sampleV", PASSPTR(offset_to[i]+1));
					WRITE_DEBUG("filtered", PASSPTR(offset_to[i]));
#undef WRITE_DEBUG
#endif
				}
			}



			/* ==== Step 3: Copy combined color pass. ==== */
			{
				if(cross_denoise) {
					int mean_from[]      = {20, 21, 22, 26, 27, 28};
					int variance_from[]  = {23, 24, 25, 29, 30, 31};
					int offset_to[]      = {16, 18, 20, 22, 24, 26};
					for(int i = 0; i < 6; i++) {
						for(int y = rect.y; y < rect.w; y++) {
							for(int x = rect.x; x < rect.z; x++) {
								filter_get_feature_kernel()(kg, sample, buffer, mean_from[i], variance_from[i], x, y, tile_x, tile_y, offsets, strides, PASSPTR(offset_to[i]), PASSPTR(offset_to[i]+1), &rect.x);
							}
						}
					}
				}
				else {
					int mean_from[]      = {20, 21, 22};
					int variance_from[]  = {23, 24, 25};
					int offset_to[]      = {16, 18, 20};
					for(int i = 0; i < 3; i++) {
						for(int y = rect.y; y < rect.w; y++) {
							for(int x = rect.x; x < rect.z; x++) {
								filter_get_feature_kernel()(kg, sample, buffer, mean_from[i], variance_from[i], x, y, tile_x, tile_y, offsets, strides, PASSPTR(offset_to[i]), PASSPTR(offset_to[i]+1), &rect.x);
							}
						}
					}
				}
			}
		}

		return filter_buffers;
	}

	void denoise_run(KernelGlobals *kg, int sample, float *filter_buffer, int4 filter_area, int4 rect, int offset, int stride, float *buffers)
	{
		bool use_gradients = kg->__data.integrator.use_gradients;

		int hw = kg->__data.integrator.half_window;
		int storage_num = filter_area.z*filter_area.w;
		FilterStorage *storage = new FilterStorage[storage_num];

		int w = align_up(rect.z - rect.x, 4), h = (rect.w - rect.y);
		int pass_stride = w*h;

		float *XtWX = new float[(DENOISE_FEATURES+1)*(DENOISE_FEATURES+1)*storage_num];
		float3 *XtWY = new float3[(DENOISE_FEATURES+1)*storage_num];
		memset(XtWX, 0, sizeof(float)*(DENOISE_FEATURES+1)*(DENOISE_FEATURES+1)*storage_num);
		memset(XtWY, 0, sizeof(float3)*(DENOISE_FEATURES+1)*storage_num);

		for(int y = 0; y < filter_area.w; y++) {
			for(int x = 0; x < filter_area.z; x++) {
				filter_construct_transform_kernel()(kg, sample, filter_buffer, x + filter_area.x, y + filter_area.y, storage + y*filter_area.z + x, &rect.x);
			}
		}

		{
			int f = 4;
			float a = 1.0f;
			float k_2 = kg->__data.integrator.weighting_adjust;
			float *weight = filter_buffer + 16*pass_stride;
			float *variance = filter_buffer + 17*pass_stride;
			float *difference = new float[pass_stride];
			float *blurDifference = new float[pass_stride];
			int local_filter_rect[4] = {filter_area.x-rect.x, filter_area.y-rect.y, filter_area.z, filter_area.w};
			for(int i = 0; i < (2*hw+1)*(2*hw+1); i++) {
				int dy = i / (2*hw+1) - hw;
				int dx = i % (2*hw+1) - hw;

				int local_rect[4] = {max(0, -dx), max(0, -dy), rect.z-rect.x - max(0, dx), rect.w-rect.y - max(0, dy)};
				filter_nlm_calc_difference_kernel()(dx, dy, weight, variance, difference, local_rect, w, 2*pass_stride, a, k_2);
				filter_nlm_blur_kernel()(difference, blurDifference, local_rect, w, f);
				filter_nlm_calc_weight_kernel()(blurDifference, difference, local_rect, w, f);
				filter_nlm_blur_kernel()(difference, blurDifference, local_rect, w, f);
				filter_nlm_construct_gramian_kernel()(dx, dy, blurDifference, filter_buffer, 0*pass_stride, storage, XtWX, XtWY, local_rect, local_filter_rect, w, h, 4);
			}
			delete[] difference;
			delete[] blurDifference;
			int buffer_params[4] = {offset, stride, kg->__data.film.pass_stride, kg->__data.film.pass_no_denoising};
			for(int y = 0; y < filter_area.w; y++) {
				for(int x = 0; x < filter_area.z; x++) {
					filter_finalize_kernel()(x + filter_area.x, y + filter_area.y, y*filter_area.z + x, w, h, buffers, storage, XtWX, XtWY, buffer_params, sample);
				}
			}
		}

		delete[] storage;
		delete[] XtWX;
		delete[] XtWY;
	}

	void thread_render(DeviceTask& task)
	{
		if(task_pool.canceled()) {
			if(task.need_finish_queue == false)
				return;
		}

		KernelGlobals kg = thread_kernel_globals_init();
		RenderTile tile;

		while(task.acquire_tile(this, tile)) {
			float *render_buffer = (float*)tile.buffer;

			if(tile.task == RenderTile::PATH_TRACE) {
				uint *rng_state = (uint*)tile.rng_state;
				int start_sample = tile.start_sample;
				int end_sample = tile.start_sample + tile.num_samples;

				for(int sample = start_sample; sample < end_sample; sample++) {
#ifdef WITH_CYCLES_DEBUG_FPE
					scoped_fpe fpe(FPE_ENABLED);
#endif
					if(task.get_cancel() || task_pool.canceled()) {
						if(task.need_finish_queue == false)
							break;
					}

					for(int y = tile.y; y < tile.y + tile.h; y++) {
						for(int x = tile.x; x < tile.x + tile.w; x++) {
							path_trace_kernel()(&kg, render_buffer, rng_state,
							                    sample, x, y, tile.offset, tile.stride);
						}
					}

					tile.sample = sample + 1;

#ifdef WITH_CYCLES_DEBUG_FPE
					fpe.restore();
#endif
					task.update_progress(&tile, tile.w*tile.h);
				}

				if(tile.buffers->params.overscan && !task.get_cancel()) {
					int tile_x[4] = {tile.x, tile.x, tile.x+tile.w, tile.x+tile.w};
					int tile_y[4] = {tile.y, tile.y, tile.y+tile.h, tile.y+tile.h};
					int offsets[9] = {0, 0, 0, 0, tile.offset, 0, 0, 0, 0};
					int strides[9] = {0, 0, 0, 0, tile.stride, 0, 0, 0, 0};
					float *buffers[9] = {NULL, NULL, NULL, NULL, (float*) tile.buffer, NULL, NULL, NULL, NULL};
					BufferParams &params = tile.buffers->params;
					int frame_stride[9] = {0, 0, 0, 0, params.width * params.height * params.get_passes_size(), 0, 0, 0, 0};

					int overscan = tile.buffers->params.overscan;
					int4 filter_area = make_int4(tile.x + overscan, tile.y + overscan, tile.w - 2*overscan, tile.h - 2*overscan);
					int4 rect = make_int4(tile.x, tile.y, tile.x + tile.w, tile.y + tile.h);

					float* filter_buffer = denoise_fill_buffer(&kg, end_sample, rect, buffers, tile_x, tile_y, offsets, strides, tile.buffers->params.frames, frame_stride);
					denoise_run(&kg, end_sample, filter_buffer, filter_area, rect, tile.offset, tile.stride, (float*) tile.buffer);
					delete[] filter_buffer;
				}
			}
			else if(tile.task == RenderTile::DENOISE) {
				int sample = tile.start_sample + tile.num_samples;

				RenderTile rtiles[9];
				rtiles[4] = tile;
				task.get_neighbor_tiles(rtiles);
				float *buffers[9];
				int offsets[9], strides[9];
				int frame_stride[9];
				for(int i = 0; i < 9; i++) {
					buffers[i] = (float*) rtiles[i].buffer;
					offsets[i] = rtiles[i].offset;
					strides[i] = rtiles[i].stride;
					if(rtiles[i].buffers) {
						BufferParams &params = rtiles[i].buffers->params;
						frame_stride[i] = params.width * params.height * params.get_passes_size();
					}
					else {
						frame_stride[i] = 0;
					}
				}
				int tile_x[4] = {rtiles[3].x, rtiles[4].x, rtiles[5].x, rtiles[5].x+rtiles[5].w};
				int tile_y[4] = {rtiles[1].y, rtiles[4].y, rtiles[7].y, rtiles[7].y+rtiles[7].h};

				int hw = kg.__data.integrator.half_window;
				int4 filter_area = make_int4(tile.x, tile.y, tile.w, tile.h);
				int4 rect = make_int4(max(tile.x - hw, tile_x[0]), max(tile.y - hw, tile_y[0]), min(tile.x + tile.w + hw+1, tile_x[3]), min(tile.y + tile.h + hw+1, tile_y[3]));

				float* filter_buffer = denoise_fill_buffer(&kg, sample, rect, buffers, tile_x, tile_y, offsets, strides, tile.buffers->params.frames, frame_stride);
				denoise_run(&kg, sample, filter_buffer, filter_area, rect, tile.offset, tile.stride, (float*) tile.buffer);
				delete[] filter_buffer;

				tile.sample = sample;

				task.update_progress(&tile, tile.w*tile.h);
			}

			task.release_tile(tile);

			if(task_pool.canceled()) {
				if(task.need_finish_queue == false)
					break;
			}
		}

		thread_kernel_globals_free(&kg);
	}

	void thread_film_convert(DeviceTask& task)
	{
		float sample_scale = 1.0f/(task.sample + 1);

		if(task.rgba_half) {
			for(int y = task.y; y < task.y + task.h; y++)
				for(int x = task.x; x < task.x + task.w; x++)
					convert_to_half_float_kernel()(&kernel_globals, (uchar4*)task.rgba_half, (float*)task.buffer,
					                               sample_scale, x, y, task.offset, task.stride);
		}
		else {
			for(int y = task.y; y < task.y + task.h; y++)
				for(int x = task.x; x < task.x + task.w; x++)
					convert_to_byte_kernel()(&kernel_globals, (uchar4*)task.rgba_byte, (float*)task.buffer,
					                         sample_scale, x, y, task.offset, task.stride);

		}
	}

	void thread_shader(DeviceTask& task)
	{
		KernelGlobals kg = kernel_globals;

#ifdef WITH_OSL
		OSLShader::thread_init(&kg, &kernel_globals, &osl_globals);
#endif
		for(int sample = 0; sample < task.num_samples; sample++) {
			for(int x = task.shader_x; x < task.shader_x + task.shader_w; x++)
				shader_kernel()(&kg,
				                (uint4*)task.shader_input,
				                (float4*)task.shader_output,
				                (float*)task.shader_output_luma,
				                task.shader_eval_type,
				                task.shader_filter,
				                x,
				                task.offset,
				                sample);

			if(task.get_cancel() || task_pool.canceled())
				break;

			task.update_progress(NULL);

		}

#ifdef WITH_OSL
		OSLShader::thread_free(&kg);
#endif
	}

	int get_split_task_count(DeviceTask& task)
	{
		if(task.type == DeviceTask::SHADER)
			return task.get_subtask_count(TaskScheduler::num_threads(), 256);
		else
			return task.get_subtask_count(TaskScheduler::num_threads());
	}

	void task_add(DeviceTask& task)
	{
		/* split task into smaller ones */
		list<DeviceTask> tasks;

		if(task.type == DeviceTask::SHADER)
			task.split(tasks, TaskScheduler::num_threads(), 256);
		else
			task.split(tasks, TaskScheduler::num_threads());

		foreach(DeviceTask& task, tasks)
			task_pool.push(new CPUDeviceTask(this, task));
	}

	void task_wait()
	{
		task_pool.wait_work();
	}

	void task_cancel()
	{
		task_pool.cancel();
	}

protected:
	inline KernelGlobals thread_kernel_globals_init()
	{
		KernelGlobals kg = kernel_globals;
		kg.transparent_shadow_intersections = NULL;
		const int decoupled_count = sizeof(kg.decoupled_volume_steps) /
		                            sizeof(*kg.decoupled_volume_steps);
		for(int i = 0; i < decoupled_count; ++i) {
			kg.decoupled_volume_steps[i] = NULL;
		}
		kg.decoupled_volume_steps_index = 0;
#ifdef WITH_OSL
		OSLShader::thread_init(&kg, &kernel_globals, &osl_globals);
#endif
		return kg;
	}

	inline void thread_kernel_globals_free(KernelGlobals *kg)
	{
		if(kg->transparent_shadow_intersections != NULL) {
			free(kg->transparent_shadow_intersections);
		}
		const int decoupled_count = sizeof(kg->decoupled_volume_steps) /
		                            sizeof(*kg->decoupled_volume_steps);
		for(int i = 0; i < decoupled_count; ++i) {
			if(kg->decoupled_volume_steps[i] != NULL) {
				free(kg->decoupled_volume_steps[i]);
			}
		}
#ifdef WITH_OSL
		OSLShader::thread_free(kg);
#endif
	}
};

Device *device_cpu_create(DeviceInfo& info, Stats &stats, bool background)
{
	return new CPUDevice(info, stats, background);
}

void device_cpu_info(vector<DeviceInfo>& devices)
{
	DeviceInfo info;

	info.type = DEVICE_CPU;
	info.description = system_cpu_brand_string();
	info.id = "CPU";
	info.num = 0;
	info.advanced_shading = true;
	info.pack_images = false;

	devices.insert(devices.begin(), info);
}

string device_cpu_capabilities(void)
{
	string capabilities = "";
	capabilities += system_cpu_support_sse2() ? "SSE2 " : "";
	capabilities += system_cpu_support_sse3() ? "SSE3 " : "";
	capabilities += system_cpu_support_sse41() ? "SSE41 " : "";
	capabilities += system_cpu_support_avx() ? "AVX " : "";
	capabilities += system_cpu_support_avx2() ? "AVX2" : "";
	if(capabilities[capabilities.size() - 1] == ' ')
		capabilities.resize(capabilities.size() - 1);
	return capabilities;
}

CCL_NAMESPACE_END
