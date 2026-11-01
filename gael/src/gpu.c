/* gpu.c - running gpu-each kernels (kernel.c) on a GPU through wgpu-native, which is loaded at run time with dlopen,
 * so gael builds and runs without it: then every kernel runs as its loop on the VM.
 *
 * GAEL_GPU=0 keeps everything on the VM, GAEL_GPU=1 makes a missing GPU an error, and without it a GPU is used when
 * there is one. GAEL_WGPU=FILE names the library; otherwise it is lib/libwgpu_native.so next to the gael binary, in
 * gael's own folder, then wherever the system's loader finds libwgpu_native.so.
 *
 * A run: the vec's columns that the body uses go to the GPU as they are, each as 32-bit numbers in a buffer of its
 * own (written while the buffer is created, so no memory of the program's is needed for it), the count and the values
 * from around go in a uniform, then the kernel runs once per element, 64 at a time, and the columns the body sets come
 * back into the vec. Buffers are made for each run for now; keeping them between runs comes next. */

#include <dlfcn.h>
#include <limits.h>
#include "wgpu89.h"
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int exe_dir(char *path);
static int gael_home(char *path);
static void gpu_close(void); /* gfx.c: everything given back to the GPU at the end */

#define WGFN(ret, name, args) ret (*name) args;
#define WGPU_FNS(X) \
	X(WgHandle, wgpuCreateInstance, (const void *)) \
	X(WgFuture, wgpuInstanceRequestAdapter, (WgHandle, const WgRequestAdapterOptions *, WgRequestCallbackInfo)) \
	X(WgFuture, wgpuAdapterRequestDevice, (WgHandle, const WgDeviceDescriptor *, WgRequestCallbackInfo)) \
	X(void, wgpuInstanceProcessEvents, (WgHandle)) \
	X(WgHandle, wgpuDeviceGetQueue, (WgHandle)) \
	X(WgHandle, wgpuDeviceCreateBuffer, (WgHandle, const WgBufferDescriptor *)) \
	X(void *, wgpuBufferGetMappedRange, (WgHandle, size_t, size_t)) \
	X(const void *, wgpuBufferGetConstMappedRange, (WgHandle, size_t, size_t)) \
	X(void, wgpuBufferUnmap, (WgHandle)) \
	X(WgFuture, wgpuBufferMapAsync, (WgHandle, u64, size_t, size_t, WgBufferMapCallbackInfo)) \
	X(void, wgpuBufferRelease, (WgHandle)) \
	X(void, wgpuQueueWriteBuffer, (WgHandle, WgHandle, u64, const void *, size_t)) \
	X(WgHandle, wgpuDeviceCreateShaderModule, (WgHandle, const WgShaderModuleDescriptor *)) \
	X(void, wgpuShaderModuleRelease, (WgHandle)) \
	X(WgHandle, wgpuDeviceCreateComputePipeline, (WgHandle, const WgComputePipelineDescriptor *)) \
	X(void, wgpuComputePipelineRelease, (WgHandle)) \
	X(WgHandle, wgpuComputePipelineGetBindGroupLayout, (WgHandle, u32)) \
	X(void, wgpuBindGroupLayoutRelease, (WgHandle)) \
	X(WgHandle, wgpuDeviceCreateBindGroup, (WgHandle, const WgBindGroupDescriptor *)) \
	X(void, wgpuBindGroupRelease, (WgHandle)) \
	X(WgHandle, wgpuDeviceCreateCommandEncoder, (WgHandle, const void *)) \
	X(void, wgpuCommandEncoderRelease, (WgHandle)) \
	X(WgHandle, wgpuCommandEncoderBeginComputePass, (WgHandle, const void *)) \
	X(void, wgpuComputePassEncoderSetPipeline, (WgHandle, WgHandle)) \
	X(void, wgpuComputePassEncoderSetBindGroup, (WgHandle, u32, WgHandle, size_t, const u32 *)) \
	X(void, wgpuComputePassEncoderDispatchWorkgroups, (WgHandle, u32, u32, u32)) \
	X(void, wgpuComputePassEncoderEnd, (WgHandle)) \
	X(void, wgpuComputePassEncoderRelease, (WgHandle)) \
	X(void, wgpuCommandEncoderCopyBufferToBuffer, (WgHandle, WgHandle, u64, WgHandle, u64, u64)) \
	X(WgHandle, wgpuCommandEncoderFinish, (WgHandle, const void *)) \
	X(void, wgpuCommandBufferRelease, (WgHandle)) \
	X(void, wgpuQueueSubmit, (WgHandle, size_t, const WgHandle *)) \
	X(u32, wgpuDevicePoll, (WgHandle, u32, const u64 *)) \
	X(WgHandle, wgpuDeviceCreateTexture, (WgHandle, const WgTextureDescriptor *)) \
	X(WgHandle, wgpuTextureCreateView, (WgHandle, const void *)) \
	X(WgHandle, wgpuDeviceCreateRenderPipeline, (WgHandle, const WgRenderPipelineDescriptor *)) \
	X(WgHandle, wgpuRenderPipelineGetBindGroupLayout, (WgHandle, u32)) \
	X(WgHandle, wgpuCommandEncoderBeginRenderPass, (WgHandle, const WgRenderPassDescriptor *)) \
	X(void, wgpuRenderPassEncoderSetPipeline, (WgHandle, WgHandle)) \
	X(void, wgpuRenderPassEncoderSetBindGroup, (WgHandle, u32, WgHandle, size_t, const u32 *)) \
	X(void, wgpuRenderPassEncoderSetVertexBuffer, (WgHandle, u32, WgHandle, u64, u64)) \
	X(void, wgpuRenderPassEncoderDraw, (WgHandle, u32, u32, u32, u32)) \
	X(void, wgpuRenderPassEncoderEnd, (WgHandle)) \
	X(void, wgpuRenderPassEncoderRelease, (WgHandle)) \
	X(void, wgpuCommandEncoderCopyTextureToBuffer, (WgHandle, const WgTexelCopyTextureInfo *, const WgTexelCopyBufferInfo *, const WgExtent3D *)) \
	X(void, wgpuTextureRelease, (WgHandle)) \
	X(void, wgpuTextureViewRelease, (WgHandle)) \
	X(void, wgpuRenderPipelineRelease, (WgHandle)) \
	X(void, wgpuQueueRelease, (WgHandle)) \
	X(void, wgpuDeviceRelease, (WgHandle)) \
	X(void, wgpuAdapterRelease, (WgHandle)) \
	X(void, wgpuInstanceRelease, (WgHandle)) \
	X(WgHandle, wgpuInstanceCreateSurface, (WgHandle, const WgSurfaceDescriptor *)) \
	X(u32, wgpuSurfaceGetCapabilities, (WgHandle, WgHandle, WgSurfaceCapabilities *)) \
	X(void, wgpuSurfaceCapabilitiesFreeMembers, (WgSurfaceCapabilities)) \
	X(void, wgpuSurfaceConfigure, (WgHandle, const WgSurfaceConfiguration *)) \
	X(void, wgpuSurfaceGetCurrentTexture, (WgHandle, WgSurfaceTexture *)) \
	X(u32, wgpuSurfacePresent, (WgHandle)) \
	X(void, wgpuSurfaceRelease, (WgHandle))

static struct {
	int tried, ok, need;     /* tried to start; a device is there; GAEL_GPU=1 */
	void *lib;
	WgHandle inst, adapter, device, queue;
	int errors;              /* the device's uncaptured errors so far */
	char why[400];           /* why there is no GPU, or the last error it gave */
	WGPU_FNS(WGFN)
} g_gpu;

static struct {
	WgHandle module, pipeline, layout;
	int failed;              /* the GPU did not take it: it runs on the VM */
} g_kgpu[MAX_KERNELS];

static o gpu_forget(i32 k)
{
	if (g_kgpu[k].layout) g_gpu.wgpuBindGroupLayoutRelease(g_kgpu[k].layout);
	if (g_kgpu[k].pipeline) g_gpu.wgpuComputePipelineRelease(g_kgpu[k].pipeline);
	if (g_kgpu[k].module) g_gpu.wgpuShaderModuleRelease(g_kgpu[k].module);
	memset(&g_kgpu[k], 0, sizeof(g_kgpu[k]));
}

static WgStringView wg_str(const char *s)
{
	WgStringView v;
	v.data = s;
	v.length = strlen(s);
	return v;
}

static o on_request(u32 status, WgHandle got, WgStringView message, void *where, void *unused)
{
	(void)unused;
	if (status == WG_STATUS_SUCCESS) *(WgHandle *)where = got;
	else sprintf(g_gpu.why, "%.*s", (int)(message.length < 300 ? message.length : 300), message.data ? message.data : "");
}

static o on_error(WgHandle const *device, u32 type, WgStringView message, void *u1, void *u2)
{
	(void)device;
	(void)u1;
	(void)u2;
	g_gpu.errors++;
	sprintf(g_gpu.why, "error %lu: %.*s", (unsigned long)type, (int)(message.length < 300 ? message.length : 300), message.data ? message.data : "");
}

static int g_mapped; /* 0 waiting, 1 mapped, -1 failed */
static o on_map(u32 status, WgStringView message, void *u1, void *u2)
{
	(void)u1;
	(void)u2;
	g_mapped = status == WG_STATUS_SUCCESS ? 1 : -1;
	if (g_mapped < 0) sprintf(g_gpu.why, "map: %.*s", (int)(message.length < 300 ? message.length : 300), message.data ? message.data : "");
}

/* gpu_open: the library, a GPU and a device on it; 0 with the reason in g_gpu.why when there is none */
static int gpu_open(void)
{
	char path[PATH_MAX + 64], home[1024];
	const char *env = getenv("GAEL_WGPU");
	int i;
	if (env && *env) g_gpu.lib = dlopen(env, RTLD_NOW | RTLD_LOCAL);
	if (!g_gpu.lib && (!env || !*env) && exe_dir(path)) {
		strcat(path, "/lib/libwgpu_native.so");
		g_gpu.lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	}
	if (!g_gpu.lib && (!env || !*env) && gael_home(home) && strlen(home) < 1000) {
		sprintf(path, "%s/lib/libwgpu_native.so", home);
		g_gpu.lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	}
	if (!g_gpu.lib && (!env || !*env)) g_gpu.lib = dlopen("libwgpu_native.so", RTLD_NOW | RTLD_LOCAL);
	if (!g_gpu.lib) {
		sprintf(g_gpu.why, "no wgpu-native: %.300s", dlerror());
		return 0;
	}
#define WGLOAD(ret, name, args) \
	if (!(*(void **)&g_gpu.name = dlsym(g_gpu.lib, #name))) { sprintf(g_gpu.why, "the wgpu-native there has no " #name); return 0; }
	WGPU_FNS(WGLOAD)
#undef WGLOAD
	if (!(g_gpu.inst = g_gpu.wgpuCreateInstance(0))) { strcpy(g_gpu.why, "wgpu-native made no instance"); return 0; }
	{
		WgRequestAdapterOptions o;
		WgRequestCallbackInfo ci;
		memset(&o, 0, sizeof(o));
		memset(&ci, 0, sizeof(ci));
		o.featureLevel = WG_FEATURE_LEVEL_CORE;
		ci.mode = WG_CALLBACK_ALLOW_PROCESS_EVENTS;
		ci.callback = on_request;
		ci.userdata1 = &g_gpu.adapter;
		strcpy(g_gpu.why, "no GPU adapter");
		g_gpu.wgpuInstanceRequestAdapter(g_gpu.inst, &o, ci);
		for (i = 0; i < 1000 && !g_gpu.adapter; i++) g_gpu.wgpuInstanceProcessEvents(g_gpu.inst);
		if (!g_gpu.adapter) return 0;
	}
	{
		WgDeviceDescriptor d;
		WgRequestCallbackInfo ci;
		memset(&d, 0, sizeof(d));
		memset(&ci, 0, sizeof(ci));
		d.uncapturedErrorCallbackInfo.callback = on_error;
		ci.mode = WG_CALLBACK_ALLOW_PROCESS_EVENTS;
		ci.callback = on_request;
		ci.userdata1 = &g_gpu.device;
		strcpy(g_gpu.why, "no GPU device");
		g_gpu.wgpuAdapterRequestDevice(g_gpu.adapter, &d, ci);
		for (i = 0; i < 1000 && !g_gpu.device; i++) g_gpu.wgpuInstanceProcessEvents(g_gpu.inst);
		if (!g_gpu.device) return 0;
	}
	g_gpu.queue = g_gpu.wgpuDeviceGetQueue(g_gpu.device);
	g_gpu.why[0] = 0;
	atexit(gpu_close);
	return 1;
}

/* gpu_kernel K: kernel K's pipeline, made on its first run; 0 when the GPU would not take it */
static int gpu_kernel(i32 k)
{
	WgShaderSourceWGSL src;
	WgShaderModuleDescriptor md;
	WgComputePipelineDescriptor pd;
	int errors = g_gpu.errors;
	if (g_kgpu[k].pipeline) return 1;
	memset(&src, 0, sizeof(src));
	memset(&md, 0, sizeof(md));
	memset(&pd, 0, sizeof(pd));
	src.chain.sType = WG_STYPE_SHADER_SOURCE_WGSL;
	src.code = wg_str(g_kernels[k].wgsl);
	md.nextInChain = &src.chain;
	g_kgpu[k].module = g_gpu.wgpuDeviceCreateShaderModule(g_gpu.device, &md);
	pd.compute.module = g_kgpu[k].module;
	pd.compute.entryPoint = wg_str("main");
	if (g_kgpu[k].module) g_kgpu[k].pipeline = g_gpu.wgpuDeviceCreateComputePipeline(g_gpu.device, &pd);
	if (g_kgpu[k].pipeline) g_kgpu[k].layout = g_gpu.wgpuComputePipelineGetBindGroupLayout(g_kgpu[k].pipeline, 0);
	if (!g_kgpu[k].layout || g_gpu.errors != errors) { /* gael wrote WGSL that the GPU does not take: gael's mistake */
		fprintf(stderr, "gael: the GPU did not take the kernel of the gpu-each at %s:%ld, so it runs on the VM: %s\n",
		        g_kernels[k].file, (long)g_kernels[k].line, g_gpu.why);
		g_kgpu[k].failed = 1;
		return 0;
	}
	return 1;
}

static u32 gpu_bits(Cell c, u8 kt) /* a cell as the 32 bits the GPU reads */
{
	union {
		f32 f;
		u32 u;
	} b;
	if (kt == KT_F32) {
		b.f = (f32)c.f;
		return b.u;
	}
	return (u32)c.i;
}

static Cell gpu_cell(u32 v, u8 kt) /* and back */
{
	union {
		f32 f;
		u32 u;
	} b;
	Cell c;
	if (kt == KT_F32) {
		b.u = v;
		c.f = (f64)b.f;
	} else if (kt == KT_I32) {
		c.i = (i64)(i32)v;
	} else {
		c.i = (i64)v;
	}
	return c;
}

/* gpu_ready: the GPU, opened on the first call; 0 when there is none or GAEL_GPU=0 (the reason in g_gpu.why) */
static int gpu_ready(void)
{
	if (!g_gpu.tried) {
		const char *env = getenv("GAEL_GPU");
		g_gpu.tried = 1;
		g_gpu.need = env && !strcmp(env, "1");
		if (env && !strcmp(env, "0")) strcpy(g_gpu.why, "GAEL_GPU=0");
		else g_gpu.ok = gpu_open();
	}
	return g_gpu.ok;
}

/* gpu_run K FRAME: run kernel K over its vec; 1 when it ran on the GPU, 0 when the VM should run the loop, -1 with
 * the error in g_gpu_error */
static int gpu_run(i32 k, const Cell *frame)
{
	const Kernel *kn = &g_kernels[k];
	WgHandle cols[MAX_KCOLS], ubuf = 0, rb = 0, bg = 0, fb = 0, enc, pass, cmd;
	WgBindGroupEntry ents[MAX_KCOLS + 2];
	WgBufferDescriptor bd;
	WgBindGroupDescriptor bgd;
	WgBufferMapCallbackInfo mi;
	u32 uni[4 + MAX_KCAPS];
	const Cell *hd;
	i64 n, cap, data, off;
	u64 rbsize = 0, at;
	u32 groups, gx, gy;
	i32 c, e;
	int ok = 1, errors;
	if (!gpu_ready()) {
		if (!g_gpu.need) return 0;
		sprintf(g_gpu_error, "gpu-each: GAEL_GPU=1, and there is no GPU to run it on (%.400s)", g_gpu.why);
		return -1;
	}
	if (!kn->wgsl || g_kgpu[k].failed || !gpu_kernel(k)) return 0;
	hd = g_heap + (kn->vwhere == AT_LOCAL ? frame[kn->vat].i : g_global_cells[kn->vat].i);
	n = hd[VH_LEN].i;
	cap = hd[VH_CAP].i;
	data = hd[VH_DATA].i;
	if (n == 0) return 1;
	if (n > 0xffffffffL / 64) return 0; /* more than the GPU's index counts: the VM */
	errors = g_gpu.errors; /* wgpu stops the program at an error in a submit, so each step is checked before it */
	memset(cols, 0, sizeof(cols));
	memset(&bd, 0, sizeof(bd));
	for (c = 0; c < kn->ncols && ok; c++) { /* each column, written into its buffer as it is made */
		u32 *to;
		const Cell *from;
		bd.size = (u64)n * 4;
		bd.usage = WG_BUFFER_STORAGE | WG_BUFFER_COPY_SRC | WG_BUFFER_COPY_DST;
		bd.mappedAtCreation = 1;
		if (!(cols[c] = g_gpu.wgpuDeviceCreateBuffer(g_gpu.device, &bd)) || !(to = (u32 *)g_gpu.wgpuBufferGetMappedRange(cols[c], 0, (size_t)bd.size))) {
			ok = 0;
			break;
		}
		off = data + col_off(kn->elem, kn->col_cell[c], cap);
		from = g_heap + off;
		for (e = 0; e < n; e++) to[e] = gpu_bits(from[e], kn->col_ty[c]);
		g_gpu.wgpuBufferUnmap(cols[c]);
		if (kn->col_set[c]) rbsize += bd.size;
	}
	if (ok) { /* the uniform: the count, then each value from around */
		memset(uni, 0, sizeof(uni));
		uni[0] = (u32)n;
		for (c = 0; c < kn->ncaps; c++)
			uni[1 + c] = gpu_bits(kn->cap_where[c] == AT_LOCAL ? frame[kn->cap_at[c]] : g_global_cells[kn->cap_at[c]], kn->cap_ty[c]);
		bd.size = (u64)((1 + kn->ncaps + 3) / 4 * 16);
		bd.usage = WG_BUFFER_UNIFORM | WG_BUFFER_COPY_DST;
		bd.mappedAtCreation = 0;
		if (!(ubuf = g_gpu.wgpuDeviceCreateBuffer(g_gpu.device, &bd))) ok = 0;
		else g_gpu.wgpuQueueWriteBuffer(g_gpu.queue, ubuf, 0, uni, (size_t)bd.size);
	}
	if (ok && kn->fault) { /* one u32 the kernel sets when a run fails as the VM's would */
		u32 *zero;
		bd.size = 4;
		bd.usage = WG_BUFFER_STORAGE | WG_BUFFER_COPY_SRC;
		bd.mappedAtCreation = 1;
		if (!(fb = g_gpu.wgpuDeviceCreateBuffer(g_gpu.device, &bd)) || !(zero = (u32 *)g_gpu.wgpuBufferGetMappedRange(fb, 0, 4))) ok = 0;
		else {
			*zero = 0;
			g_gpu.wgpuBufferUnmap(fb);
			rbsize += 4;
		}
	}
	if (ok && rbsize) {
		bd.size = rbsize;
		bd.mappedAtCreation = 0;
		bd.usage = WG_BUFFER_MAP_READ | WG_BUFFER_COPY_DST;
		if (!(rb = g_gpu.wgpuDeviceCreateBuffer(g_gpu.device, &bd))) ok = 0;
	}
	if (ok) {
		memset(ents, 0, sizeof(ents));
		ents[0].binding = 0;
		ents[0].buffer = ubuf;
		ents[0].size = (u64)((1 + kn->ncaps + 3) / 4 * 16);
		for (c = 0; c < kn->ncols; c++) {
			ents[c + 1].binding = (u32)c + 1;
			ents[c + 1].buffer = cols[c];
			ents[c + 1].size = (u64)n * 4;
		}
		if (kn->fault) {
			ents[kn->ncols + 1].binding = (u32)kn->ncols + 1;
			ents[kn->ncols + 1].buffer = fb;
			ents[kn->ncols + 1].size = 4;
		}
		memset(&bgd, 0, sizeof(bgd));
		bgd.layout = g_kgpu[k].layout;
		bgd.entryCount = (size_t)kn->ncols + 1 + kn->fault;
		bgd.entries = ents;
		if (!(bg = g_gpu.wgpuDeviceCreateBindGroup(g_gpu.device, &bgd))) ok = 0;
	}
	if (g_gpu.errors != errors) ok = 0;
	if (ok) {
		groups = (u32)((n + 63) / 64);
		gx = groups < 65535 ? groups : 65535;
		gy = (groups + gx - 1) / gx;
		enc = g_gpu.wgpuDeviceCreateCommandEncoder(g_gpu.device, 0);
		pass = g_gpu.wgpuCommandEncoderBeginComputePass(enc, 0);
		g_gpu.wgpuComputePassEncoderSetPipeline(pass, g_kgpu[k].pipeline);
		g_gpu.wgpuComputePassEncoderSetBindGroup(pass, 0, bg, 0, 0);
		g_gpu.wgpuComputePassEncoderDispatchWorkgroups(pass, gx, gy, 1);
		g_gpu.wgpuComputePassEncoderEnd(pass);
		g_gpu.wgpuComputePassEncoderRelease(pass);
		for (c = 0, at = 0; c < kn->ncols; c++) { /* the columns the body set, one after another, to be read back */
			if (!kn->col_set[c]) continue;
			g_gpu.wgpuCommandEncoderCopyBufferToBuffer(enc, cols[c], 0, rb, at, (u64)n * 4);
			at += (u64)n * 4;
		}
		if (fb) g_gpu.wgpuCommandEncoderCopyBufferToBuffer(enc, fb, 0, rb, at, 4);
		cmd = g_gpu.wgpuCommandEncoderFinish(enc, 0);
		g_gpu.wgpuQueueSubmit(g_gpu.queue, 1, &cmd);
		g_gpu.wgpuCommandBufferRelease(cmd);
		g_gpu.wgpuCommandEncoderRelease(enc);
	}
	if (ok && rb) {
		const u32 *got;
		memset(&mi, 0, sizeof(mi));
		mi.mode = WG_CALLBACK_ALLOW_PROCESS_EVENTS;
		mi.callback = on_map;
		g_mapped = 0;
		g_gpu.wgpuBufferMapAsync(rb, WG_MAP_READ, 0, (size_t)rbsize, mi);
		for (e = 0; !g_mapped && e < 1000000; e++) { /* the poll waits for the GPU; the map's callback comes after */
			g_gpu.wgpuDevicePoll(g_gpu.device, 1, 0);
			g_gpu.wgpuInstanceProcessEvents(g_gpu.inst);
		}
		if (g_mapped < 0 || !(got = (const u32 *)g_gpu.wgpuBufferGetConstMappedRange(rb, 0, (size_t)rbsize))) {
			ok = 0;
		} else if (fb && got[rbsize / 4 - 1]) { /* a run failed: nothing comes back, and the VM runs the loop to say where */
			g_gpu.wgpuBufferUnmap(rb);
			rbsize = 0;
		} else {
			for (c = 0; c < kn->ncols; c++) {
				Cell *to;
				if (!kn->col_set[c]) continue;
				to = g_heap + data + col_off(kn->elem, kn->col_cell[c], cap);
				for (e = 0; e < n; e++) to[e] = gpu_cell(got[e], kn->col_ty[c]);
				got += n;
			}
			g_gpu.wgpuBufferUnmap(rb);
		}
	}
	for (c = 0; c < kn->ncols; c++) {
		if (cols[c]) g_gpu.wgpuBufferRelease(cols[c]);
	}
	if (ubuf) g_gpu.wgpuBufferRelease(ubuf);
	if (fb) g_gpu.wgpuBufferRelease(fb);
	if (rb) g_gpu.wgpuBufferRelease(rb);
	if (bg) g_gpu.wgpuBindGroupRelease(bg);
	if (!ok) { /* before any column came back: the VM runs the loop instead */
		fprintf(stderr, "gael: the GPU failed the gpu-each at %s:%ld, so it runs on the VM: %s\n", kn->file, (long)kn->line, g_gpu.why);
		return 0;
	}
	return rbsize || !fb ? 1 : 0;
}
