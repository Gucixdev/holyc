/* wgpu89.h - the part of WebGPU's C API (webgpu.h) that gpu.c calls, written out again in C89: the real header needs
 * C99, and gael loads wgpu-native at run time with dlopen, so only the layout of these structs and the values of these
 * constants have to match it. They are the ones of wgpu-native v29 (webgpu.h of 2026-03); tests/gpu/abi.c checks
 * every size, offset and value here against that header, so a newer wgpu that moves something fails there first. */

#ifndef GAEL_WGPU89_H_
#define GAEL_WGPU89_H_

#include <stddef.h>

typedef void *WgHandle;

typedef struct {
	const char *data;
	size_t length;
} WgStringView;

typedef struct WgChained {
	struct WgChained *next;
	u32 sType;
} WgChained;

typedef struct {
	u64 id;
} WgFuture;

typedef void (*WgRequestCallback)(u32 status, WgHandle got, WgStringView message, void *userdata1, void *userdata2);
typedef void (*WgBufferMapCallback)(u32 status, WgStringView message, void *userdata1, void *userdata2);
typedef void (*WgErrorCallback)(WgHandle const *device, u32 type, WgStringView message, void *userdata1, void *userdata2);
typedef void (*WgLostCallback)(WgHandle const *device, u32 reason, WgStringView message, void *userdata1, void *userdata2);

typedef struct { /* WGPURequestAdapterCallbackInfo and WGPURequestDeviceCallbackInfo */
	WgChained *nextInChain;
	u32 mode;
	WgRequestCallback callback;
	void *userdata1, *userdata2;
} WgRequestCallbackInfo;

typedef struct {
	WgChained *nextInChain;
	u32 mode;
	WgBufferMapCallback callback;
	void *userdata1, *userdata2;
} WgBufferMapCallbackInfo;

typedef struct {
	WgChained *nextInChain;
	WgErrorCallback callback;
	void *userdata1, *userdata2;
} WgUncapturedErrorCallbackInfo;

typedef struct {
	WgChained *nextInChain;
	u32 mode;
	WgLostCallback callback;
	void *userdata1, *userdata2;
} WgDeviceLostCallbackInfo;

typedef struct {
	WgChained *nextInChain;
	u32 featureLevel, powerPreference, forceFallbackAdapter, backendType;
	WgHandle compatibleSurface;
} WgRequestAdapterOptions;

typedef struct {
	WgChained *nextInChain;
	WgStringView label;
} WgQueueDescriptor;

typedef struct {
	WgChained *nextInChain;
	WgStringView label;
	size_t requiredFeatureCount;
	const u32 *requiredFeatures;
	const void *requiredLimits;
	WgQueueDescriptor defaultQueue;
	WgDeviceLostCallbackInfo deviceLostCallbackInfo;
	WgUncapturedErrorCallbackInfo uncapturedErrorCallbackInfo;
} WgDeviceDescriptor;

typedef struct {
	WgChained *nextInChain;
	WgStringView label;
	u64 usage, size;
	u32 mappedAtCreation;
} WgBufferDescriptor;

typedef struct {
	WgChained chain;
	WgStringView code;
} WgShaderSourceWGSL;

typedef struct {
	WgChained *nextInChain;
	WgStringView label;
} WgShaderModuleDescriptor;

typedef struct {
	WgChained *nextInChain;
	WgHandle module;
	WgStringView entryPoint;
	size_t constantCount;
	const void *constants;
} WgComputeState;

typedef struct {
	WgChained *nextInChain;
	WgStringView label;
	WgHandle layout;
	WgComputeState compute;
} WgComputePipelineDescriptor;

typedef struct {
	WgChained *nextInChain;
	u32 binding;
	WgHandle buffer;
	u64 offset, size;
	WgHandle sampler, textureView;
} WgBindGroupEntry;

typedef struct {
	WgChained *nextInChain;
	WgStringView label;
	WgHandle layout;
	size_t entryCount;
	const WgBindGroupEntry *entries;
} WgBindGroupDescriptor;

typedef struct {
	u32 width, height, depthOrArrayLayers;
} WgExtent3D;

typedef struct {
	u32 x, y, z;
} WgOrigin3D;

typedef struct {
	WgChained *nextInChain;
	WgStringView label;
	u64 usage;
	u32 dimension;
	WgExtent3D size;
	u32 format, mipLevelCount, sampleCount;
	size_t viewFormatCount;
	const u32 *viewFormats;
} WgTextureDescriptor;

typedef struct {
	WgChained *nextInChain;
	u32 format;
	u64 offset;
	u32 shaderLocation;
} WgVertexAttribute;

typedef struct {
	WgChained *nextInChain;
	u32 stepMode;
	u64 arrayStride;
	size_t attributeCount;
	const WgVertexAttribute *attributes;
} WgVertexBufferLayout;

typedef struct {
	WgChained *nextInChain;
	WgHandle module;
	WgStringView entryPoint;
	size_t constantCount;
	const void *constants;
	size_t bufferCount;
	const WgVertexBufferLayout *buffers;
} WgVertexState;

typedef struct {
	WgChained *nextInChain;
	u32 topology, stripIndexFormat, frontFace, cullMode, unclippedDepth;
} WgPrimitiveState;

typedef struct {
	WgChained *nextInChain;
	u32 count, mask, alphaToCoverageEnabled;
} WgMultisampleState;

typedef struct {
	u32 operation, srcFactor, dstFactor;
} WgBlendComponent;

typedef struct {
	WgBlendComponent color, alpha;
} WgBlendState;

typedef struct {
	WgChained *nextInChain;
	u32 format;
	const WgBlendState *blend;
	u64 writeMask;
} WgColorTargetState;

typedef struct {
	WgChained *nextInChain;
	WgHandle module;
	WgStringView entryPoint;
	size_t constantCount;
	const void *constants;
	size_t targetCount;
	const WgColorTargetState *targets;
} WgFragmentState;

typedef struct {
	WgChained *nextInChain;
	WgStringView label;
	WgHandle layout;
	WgVertexState vertex;
	WgPrimitiveState primitive;
	const void *depthStencil;
	WgMultisampleState multisample;
	const WgFragmentState *fragment;
} WgRenderPipelineDescriptor;

typedef struct {
	f64 r, g, b, a;
} WgColor;

typedef struct {
	WgChained *nextInChain;
	WgHandle view;
	u32 depthSlice;
	WgHandle resolveTarget;
	u32 loadOp, storeOp;
	WgColor clearValue;
} WgRenderPassColorAttachment;

typedef struct {
	WgChained *nextInChain;
	WgStringView label;
	size_t colorAttachmentCount;
	const WgRenderPassColorAttachment *colorAttachments;
	const void *depthStencilAttachment;
	WgHandle occlusionQuerySet;
	const void *timestampWrites;
} WgRenderPassDescriptor;

typedef struct {
	WgHandle texture;
	u32 mipLevel;
	WgOrigin3D origin;
	u32 aspect;
} WgTexelCopyTextureInfo;

typedef struct {
	u64 offset;
	u32 bytesPerRow, rowsPerImage;
} WgTexelCopyBufferLayout;

typedef struct {
	WgTexelCopyBufferLayout layout;
	WgHandle buffer;
} WgTexelCopyBufferInfo;

typedef struct {
	WgChained *nextInChain;
	WgStringView label;
} WgSurfaceDescriptor;

typedef struct {
	WgChained chain;
	void *display, *surface;
} WgSurfaceSourceWaylandSurface;

typedef struct {
	WgChained *nextInChain;
	u64 usages;
	size_t formatCount;
	const u32 *formats;
	size_t presentModeCount;
	const u32 *presentModes;
	size_t alphaModeCount;
	const u32 *alphaModes;
} WgSurfaceCapabilities;

typedef struct {
	WgChained *nextInChain;
	WgHandle device;
	u32 format;
	u64 usage;
	u32 width, height;
	size_t viewFormatCount;
	const u32 *viewFormats;
	u32 alphaMode, presentMode;
} WgSurfaceConfiguration;

typedef struct {
	WgChained *nextInChain;
	WgHandle texture;
	u32 status;
} WgSurfaceTexture;

#define WG_STYPE_SHADER_SOURCE_WGSL 0x2u
#define WG_STYPE_SURFACE_SOURCE_WAYLAND 0x7u
#define WG_PRESENT_FIFO 0x1u
#define WG_SURFACE_TEXTURE_OPTIMAL 0x1u
#define WG_SURFACE_TEXTURE_SUBOPTIMAL 0x2u
#define WG_STATUS_OK 0x1u                /* WGPUStatus */
#define WG_CALLBACK_ALLOW_PROCESS_EVENTS 0x2u
#define WG_FEATURE_LEVEL_CORE 0x2u
#define WG_STATUS_SUCCESS 0x1u           /* WGPURequestAdapterStatus, WGPURequestDeviceStatus, WGPUMapAsyncStatus */
#define WG_BUFFER_MAP_READ 0x1u
#define WG_BUFFER_COPY_SRC 0x4u
#define WG_BUFFER_COPY_DST 0x8u
#define WG_BUFFER_UNIFORM 0x40u
#define WG_BUFFER_STORAGE 0x80u
#define WG_MAP_READ 0x1u
#define WG_WHOLE_MAP_SIZE ((size_t)-1)   /* WGPU_WHOLE_MAP_SIZE */
#define WG_BUFFER_VERTEX 0x20u
#define WG_TEXTURE_COPY_SRC 0x1u
#define WG_TEXTURE_RENDER_ATTACHMENT 0x10u
#define WG_TEXTURE_DIMENSION_2D 0x2u
#define WG_FORMAT_RGBA8_UNORM 0x16u
#define WG_FORMAT_BGRA8_UNORM 0x1Bu
#define WG_VERTEX_STEP_VERTEX 0x1u
#define WG_VERTEX_FLOAT32 0x1Cu
#define WG_VERTEX_FLOAT32X2 0x1Du
#define WG_VERTEX_UNORM8X4 0x9u
#define WG_TOPOLOGY_TRIANGLE_LIST 0x4u
#define WG_FRONT_CCW 0x1u
#define WG_CULL_NONE 0x1u
#define WG_BLEND_ADD 0x1u
#define WG_BLEND_ONE 0x2u
#define WG_BLEND_SRC_ALPHA 0x5u
#define WG_BLEND_ONE_MINUS_SRC_ALPHA 0x6u
#define WG_COLOR_WRITE_ALL 0xFu
#define WG_LOAD_LOAD 0x1u
#define WG_LOAD_CLEAR 0x2u
#define WG_STORE_STORE 0x1u
#define WG_DEPTH_SLICE_UNDEFINED 0xFFFFFFFFu
#define WG_ASPECT_ALL 0x1u

#endif
