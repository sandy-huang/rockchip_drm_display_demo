#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <sys/mman.h>
#include <drm_fourcc.h>
#include <rockchip/rockchip_drmif.h>
#include <drm.h>
#include "libdrm_macros.h"
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define GRALLOC_ALIGN( value, base ) (((value) + ((base) - 1)) & ~((base) - 1))

struct crtc_prop {
	int crtc_active;
	int pdaf_type;
	int work_mode;
};

struct conn_prop {
	int crtc_id;
	int csi_tx_path;
};

struct plane_prop {
	int crtc_id;
	int fb_id;

	int src_x;
	int src_y;
	int src_w;
	int src_h;

	int crtc_x;
	int crtc_y;
	int crtc_w;
	int crtc_h;

	int zpos;
	int feature;
};

struct drm_planes {
	struct plane_prop plane_prop;
	drmModePlanePtr plane;

	int support_scale;
};

struct rockchip_drm_handle_t {
	char *file; /* file path and name */
	int format; /* data format define at drm_fourcc.h */
	int width; /* pixel */
	int height; /* pixel */
	int afbc;  /* gpu compress format */

	/* file descriptors */
	int prime_fd; /* input fd, maybe from different process after dup */
	int flag;

	/* return value */
	int byte_stride;
	int size;
	int pixel_stride;
};

struct rockchip_buff_info {
	struct rockchip_bo *bo;
	int fb_id;

	int src_w;
	int src_h;
};

struct rockchip_device *drm_dev = NULL;

enum csi_path_mode {
	VOP_PATH,
	BYPASS_PATH
};

enum vop_pdaf_mode {
	VOP_HOLD_MODE = 0,
	VOP_NORMAL_MODE,
	VOP_PINGPONG_MODE,
	VOP_BYPASS_MODE,
	VOP_BACKGROUND_MODE,
	VOP_ONEFRAME_MODE,
	VOP_ONEFRAME_NOSEND_MODE
};

enum vop_pdaf_type {
	VOP_PDAF_TYPE_DEFAULT = 0,
	VOP_PDAF_TYPE_HBLANK,
	VOP_PDAF_TYPE_VBLANK,
};

static void readbin(char *filename, void * address)
{
	int fd = -1, size;
		
	fd = open(filename, O_RDONLY);
	if(fd < 0) {
		printf("can not open file %s\n", filename);
		return;
	}
	
	size = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);
	read(fd, address, size);
}

static void read_pic_buffer(void *address, char *filename)
{
	//filename = "/data/1536_2048_argb.bin";
	readbin(filename, address);
}

/*
 * Computes the strides and size for an RGB buffer
 *
 * width               width of the buffer in pixels
 * height              height of the buffer in pixels
 * pixel_size          size of one pixel in bytes
 *
 * pixel_stride (out)  stride of the buffer in pixels
 * byte_stride  (out)  stride of the buffer in bytes
 * size         (out)  size of the buffer in bytes
 */
static void get_rgb_stride_and_size(int width, int height, int pixel_size,
                                    int *pixel_stride, int *byte_stride, size_t *size)
{
	int stride;

	stride = width * pixel_size;

	/* Align the lines to 64 bytes.
	 * It's more efficient to write to 64-byte aligned addresses
	 * because it's the burst size on the bus
	 * at drm gem driver will align 64 byte, so we align 64 byte align
	 * keep same.
	 */
	stride = GRALLOC_ALIGN(stride, 64);
	if (size != NULL)
	{
		*size = stride * height;
		fprintf(stderr, "size:%zd\n", size);
	}
	if (byte_stride != NULL)
	{
		*byte_stride = stride;
	}
	if (pixel_stride != NULL)
	{
		*pixel_stride = stride / pixel_size;
	}	
}

static struct rockchip_bo *rockchip_drm_gem_alloc(struct rockchip_drm_handle_t *handle)
{
	int width, height, format, pixel_size, pixel_stride, byte_stride, size;
	struct rockchip_bo *bo;
	uint32_t gem_handle;
	int ret = 0;
	
	width = handle->width;
	height = handle->height;
	format = handle->format;

	switch (format) {
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_RGBX8888:
	case DRM_FORMAT_BGRX8888:
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_RGBA8888:
	case DRM_FORMAT_BGRA8888:
		pixel_size = 4;
		break;
	case DRM_FORMAT_RGB888:
	case DRM_FORMAT_BGR888:
		pixel_size = 3;
		break;
	case DRM_FORMAT_RGB565:
	case DRM_FORMAT_BGR565:
		pixel_size = 2;
		break;
	case DRM_FORMAT_RGB332:
	case DRM_FORMAT_BGR233:
		pixel_size = 1;
		break;
	default:
		pixel_size = 4;
		fprintf(stderr, "unsupport format: %d\n", format);
		break;
	}
	
	get_rgb_stride_and_size(width, height, pixel_size, &pixel_stride, &byte_stride, &size);
	fprintf(stderr, "%d x %d pixel size:%d, pixel stride :%d, byte stride:%d, size:%zd\n",
		width, height, pixel_size, pixel_stride, byte_stride, size);

	if (handle->prime_fd > 0) {
		ret = drmPrimeFDToHandle(drm_dev->fd, handle->prime_fd,
			&gem_handle);
		if (ret) {
			fprintf(stderr, "failed to convert prime fd to handle %d ret=%d",
				handle->prime_fd, ret);
			goto err;
		}
		bo = rockchip_bo_from_handle(drm_dev, gem_handle,
					     handle->flag, size);
		if (!bo) {
			struct drm_gem_close args;

			fprintf(stderr, "failed to wrap bo handle=%d size=%zd\n", 
				handle->prime_fd, size);
			memset(&args, 0, sizeof(args));
			args.handle = gem_handle;
			drmIoctl(drm_dev->fd, DRM_IOCTL_GEM_CLOSE, &args);
			return NULL;
		}
		bo->fd = handle->prime_fd;
	} else {
		bo = rockchip_bo_create(drm_dev, size, handle->flag);
		if (!bo) {
			fprintf(stderr, "failed to allocate bo %dx%dx%dx%zd\n", 
				handle->height, pixel_stride, byte_stride, size);
		}
		handle->size = size;
		handle->byte_stride = byte_stride;
		handle->pixel_stride = pixel_stride;

		gem_handle = rockchip_bo_handle(bo);
		ret = drmPrimeHandleToFD(drm_dev->fd, gem_handle, 0,
					 &bo->fd);
		if (ret) {
			fprintf(stderr, "failed to get prime fd %d", ret);
			goto err_unref;
		}
	}

	return bo;

err_unref:
	rockchip_bo_destroy(bo);
err:
	return NULL;
}

static int rockchip_drm_gem_free(struct rockchip_bo *bo)
{
	fprintf(stderr, "rockchip_drm_gem_free1 bo->fd:%d\n", bo->fd);
	if (bo->fd > 0)
		close(bo->fd);
	bo->fd = -1;
fprintf(stderr, "rockchip_drm_gem_free2 bo->fd:%d\n", bo->fd);
	usleep(5000 * 1000);
	
	rockchip_bo_destroy(bo);
	usleep(5000 * 1000);
fprintf(stderr, "rockchip_drm_gem_free3 bo->fd:%d\n", bo->fd);
	return 0;
}

static int rockchip_drm_gem_map(struct rockchip_bo *bo)
{
	rockchip_bo_map(bo);

	return 0;
}

static int rockchip_drm_gem_unmap(struct rockchip_bo *bo)
{
	/*
	 * the munmap(bo->vaddr, bo->size) will be called at
	 * rockchip_drm_gem_free() -> rockchip_bo_destroy()
	 * so here do nothing.
	 */
	return 0;
}

static int rockchip_drm_add_fb(struct rockchip_buff_info *buf_info, struct rockchip_drm_handle_t *drm_handle,
			       int afbdc)
{
	uint32_t handles[4], pitches[4], offsets[4];
	int fb_id, ret;
	struct rockchip_bo *bo = buf_info->bo;

	handles[0] = bo->handle;
	pitches[0] = drm_handle->byte_stride;
	offsets[0] = 0;

	if (!afbdc) {
		ret = drmModeAddFB2(drm_dev->fd,
			drm_handle->width, drm_handle->height, drm_handle->format,
			handles, pitches, offsets, &fb_id, 0);
	} else {
		__u64 modifier[4];
		memset(modifier, 0, sizeof(modifier));
		modifier[0] = DRM_FORMAT_MOD_ARM_AFBC;
		ret = drmModeAddFB2_ext(drm_dev->fd,
			drm_handle->width, drm_handle->height, drm_handle->format,
			handles, pitches, offsets, modifier,
			&fb_id, DRM_MODE_FB_MODIFIERS);
	}
	if (ret) {
		printf("failed to create fb ret=%d:%d %d, format:%d, handles:%d, pitch:%d, offset:%d, fb id:%d\n",
			ret, drm_handle->width, drm_handle->height, drm_handle->format, handles[0],
			pitches[0], offsets[0], fb_id);
		return ret;
	}

	buf_info->fb_id = fb_id;

	return 0;
}

static int rockchip_buf_init(struct rockchip_drm_handle_t *drm_handle, struct rockchip_buff_info *buf_info)
{
	struct rockchip_bo *bo;

	fprintf(stderr, "file:%s\n", drm_handle->file);
	bo = rockchip_drm_gem_alloc(drm_handle);
	buf_info->bo = bo;
	rockchip_drm_gem_map(bo);
	read_pic_buffer(bo->vaddr, drm_handle->file);/* update buffer */
	rockchip_drm_gem_unmap(bo);
	rockchip_drm_add_fb(buf_info, drm_handle, drm_handle->afbc);
	buf_info->src_w = drm_handle->width;
	buf_info->src_h = drm_handle->height;

	return 0;
}

#define DRM_ATOMIC_ADD_CRTC_PROP(object_id, value) \
do { \
	ret = drmModeAtomicAddProperty(req, crtc->crtc_id, object_id, value); \
	if (ret < 0) \
		fprintf(stderr, "Failed to add prop[%d] to [%d]", value, object_id); \
} while (0)

#define DRM_ATOMIC_ADD_CONN_PROP(object_id, value) \
do { \
	ret = drmModeAtomicAddProperty(req, connector->connector_id, object_id, value); \
	if (ret < 0) \
		fprintf(stderr, "Failed to add prop[%d] to [%d]", value, object_id); \
} while (0)

#define DRM_ATOMIC_ADD_PLANE_PROP(plane, object_id, value) \
do { \
	ret = drmModeAtomicAddProperty(req, plane->plane_id, object_id, value); \
	if (ret < 0) \
		fprintf(stderr, "Failed to add prop[%d] to [%d]", value, object_id); \
} while (0)

static int rockchip_drm_commit(struct rockchip_buff_info *buf_info, int file_num,
			   struct drm_planes *drm_planes,
			   drmModeCrtc *crtc, struct crtc_prop *crtc_prop,
			   drmModeConnector *connector, struct conn_prop *conn_prop)
{
	drmModeAtomicReq *req;
	drmModePlanePtr plane;
	struct plane_prop *plane_prop;
	int i, ret = 0;
	uint32_t flags = 0;
	
	req = drmModeAtomicAlloc();

	/* add crtc prop */
	DRM_ATOMIC_ADD_CRTC_PROP(crtc_prop->pdaf_type, VOP_PDAF_TYPE_VBLANK);
	DRM_ATOMIC_ADD_CRTC_PROP(crtc_prop->work_mode, VOP_NORMAL_MODE);
	/* add connect prop */
	DRM_ATOMIC_ADD_CONN_PROP(conn_prop->csi_tx_path, BYPASS_PATH);

	/* add plane prop */
	for (i = 0; i < file_num; i++) {
		fprintf(stderr, "i:%d\n", i);
		plane = drm_planes[i].plane;
		plane_prop = &drm_planes[i].plane_prop;
		fprintf(stderr, "i:%d, plane:%p, prop:%p, id:%d, fb id:%d\n",
			i,plane, plane_prop, plane->plane_id, buf_info[i].fb_id);
		DRM_ATOMIC_ADD_PLANE_PROP(plane, plane_prop->crtc_id, crtc->crtc_id);
		DRM_ATOMIC_ADD_PLANE_PROP(plane, plane_prop->fb_id, buf_info[i].fb_id);
		DRM_ATOMIC_ADD_PLANE_PROP(plane, plane_prop->src_x, 0);	/*src data position */
		DRM_ATOMIC_ADD_PLANE_PROP(plane, plane_prop->src_y, 0);
		DRM_ATOMIC_ADD_PLANE_PROP(plane, plane_prop->src_w, buf_info[i].src_w << 16); /*src data size */
		DRM_ATOMIC_ADD_PLANE_PROP(plane, plane_prop->src_h, buf_info[i].src_h << 16);
		DRM_ATOMIC_ADD_PLANE_PROP(plane, plane_prop->crtc_x, 0);	/* display position */
		DRM_ATOMIC_ADD_PLANE_PROP(plane, plane_prop->crtc_y, 0);
		DRM_ATOMIC_ADD_PLANE_PROP(plane, plane_prop->crtc_w, buf_info[i].src_w); /* display size */
		DRM_ATOMIC_ADD_PLANE_PROP(plane, plane_prop->crtc_h, buf_info[i].src_h);
		DRM_ATOMIC_ADD_PLANE_PROP(plane, plane_prop->zpos, i); /* zpos, from 0 to zpos_max */
	}
	/*
	 * commit one frame
	 */
	//flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
	ret = drmModeAtomicCommit(drm_dev->fd, req, flags, NULL);
	if (ret)
		fprintf(stderr, "atomic: couldn't commit new state: %s, ret:%d\n", strerror(errno), ret);

	drmModeAtomicFree(req);

	return ret;
}

int main(int argc, char **argv)
{
	drmModePlaneResPtr plane_res;
	drmModePlanePtr plane;
	drmModeResPtr res;
	drmModeCrtcPtr crtc = NULL;
	drmModeAtomicReq *req;
	drmModeObjectPropertiesPtr props;
	drmModePropertyPtr prop;
	struct plane_prop plane_prop;
	struct drm_planes *drm_planes;
	struct crtc_prop crtc_prop;
	struct conn_prop conn_prop;
	uint32_t i, j;
	int fd, ret;
	int found_crtc = 0;
	int found_conn = 0;
	uint32_t fb_id;
	uint32_t flags = 0;
	int zpos_max = INT_MAX;
	int pic_w, pic_h, afbc = 0;
	int file_num = 0;
	uint32_t *conn_ids;
	char *file_name[4];
	char *file_name1;
	char *file_name2;
	struct rockchip_bo *bo;

	drmModeConnectorPtr connector;
	drmModeModeInfo *mode;

	struct rockchip_drm_handle_t drm_handle[4];
	struct rockchip_buff_info buf_info[4];
	struct rockchip_buff_info buf_info2[4];

	afbc = atoi(argv[1]);
	pic_w = atoi(argv[2]);
	pic_h = atoi(argv[3]);
	file_num = atoi(argv[4]);
	file_name[0] = argv[5];
	file_name[1] = argv[6];
	file_name[2] = argv[7];
	file_name[3] = argv[8];

	fprintf(stderr, "debug>>afbdc:%d, w:%d, h:%d, file_num:%d, file:%s\n",
		afbc, pic_w, pic_h, file_num, file_name[0]);

	fd = drmOpen("rockchip", NULL);
	if (fd < 0) {
		fprintf(stderr, "failed to open rockchip drm: %s\n",
			strerror(errno));
		return fd;
	}
	drm_dev = rockchip_device_create(fd);
	
	ret = drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	if (ret) {
		fprintf(stderr, "Failed to set atomic cap %s", strerror(errno));
		return ret;
	}

	ret = drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
	if (ret) {
		fprintf(stderr, "Failed to set atomic cap %s", strerror(errno));
		return ret;
	}

	res = drmModeGetResources(fd);
	if (!res) {
		fprintf(stderr, "Failed to get resources: %s\n",
			strerror(errno));
		return -ENODEV;
	}

	/*
	 * Found active crtc.
	 */
	for (i = 0; i < res->count_crtcs; ++i) {
		crtc = drmModeGetCrtc(fd, res->crtcs[i]);
		if (!crtc) {
			fprintf(stderr, "Could not get crtc %u: %s\n",
					res->crtcs[i], strerror(errno));
			continue;
		}

		props = drmModeObjectGetProperties(fd, crtc->crtc_id,
						   DRM_MODE_OBJECT_CRTC);
		if (!props) {
			fprintf(stderr, "failed to found props crtc[%d] %s\n",
				crtc->crtc_id, strerror(errno));
			continue;
		}
		for (j = 0; j < props->count_props; j++) {
			prop = drmModeGetProperty(fd, props->props[j]);
			if (!strcmp(prop->name, "ACTIVE")) {
				crtc_prop.crtc_active = prop->prop_id;
			} else if (!strcmp(prop->name, "PDAF_TYPE")) {
				crtc_prop.pdaf_type = prop->prop_id;
			} else if (!strcmp(prop->name, "WORK_MODE")) {
				crtc_prop.work_mode = prop->prop_id;
			}
		}
		/* maybe we need to find some special crtc, here we use the first crtc */
		found_crtc = 1;
		if (found_crtc)
			break;
	}
	if (i == res->count_crtcs) {
		fprintf(stderr, "failed to find usable crtc\n");
		return -ENODEV;
	}

	/*
	 * Found active connect.
	 */
	conn_ids = calloc(res->count_connectors, sizeof(*conn_ids));
	for (i = 0; i < res->count_connectors; ++i) {
		connector = drmModeGetConnector(fd, res->connectors[i]);

		mode = &connector->modes[0];
		conn_ids[i] = connector->connector_id;

		props = drmModeObjectGetProperties(fd, connector->connector_id,
						   DRM_MODE_OBJECT_CONNECTOR);
		if (!props) {
			fprintf(stderr, "failed to found props connect[%d] %s\n",
				connector->connector_id, strerror(errno));
			continue;
		}
		for (j = 0; j < props->count_props; j++) {
			prop = drmModeGetProperty(fd, props->props[j]);
			if (!strcmp(prop->name, "CRTC_ID")) {
				conn_prop.crtc_id = prop->prop_id;
			} else if (!strcmp(prop->name, "CSI-TX-PATH")) {
				conn_prop.csi_tx_path = prop->prop_id;
			} 
		}
		/* maybe we need to find some special connect, here we use the first connect */
		found_conn = 1;
		if (found_conn)
			break;
	}
	if (i == res->count_connectors) {
		fprintf(stderr, "failed to find usable connect\n");
		return -ENODEV;
	}

	/* get planes */
	plane_res = drmModeGetPlaneResources(fd);
	drm_planes = calloc(plane_res->count_planes, sizeof(*drm_planes));
	for (i = 0; i < plane_res->count_planes; i++) {
		memset(&plane_prop, 0, sizeof(plane_prop));

		plane = drmModeGetPlane(fd, plane_res->planes[i]);
		props = drmModeObjectGetProperties(fd, plane->plane_id,
						   DRM_MODE_OBJECT_PLANE);	
		if (!props) {
			fprintf(stderr, "failed to found props plane[%d] %s\n",
				plane->plane_id, strerror(errno));
			return -ENODEV;
		}

		for (j = 0; j < props->count_props; j++) {
			prop = drmModeGetProperty(fd, props->props[j]);
			if (!strcmp(prop->name, "CRTC_ID"))
				plane_prop.crtc_id = prop->prop_id;
			else if (!strcmp(prop->name, "FB_ID"))
				plane_prop.fb_id = prop->prop_id;
			else if (!strcmp(prop->name, "SRC_X"))
				plane_prop.src_x = prop->prop_id;
			else if (!strcmp(prop->name, "SRC_Y"))
				plane_prop.src_y = prop->prop_id;
			else if (!strcmp(prop->name, "SRC_W"))
				plane_prop.src_w = prop->prop_id;
			else if (!strcmp(prop->name, "SRC_H"))
				plane_prop.src_h = prop->prop_id;
			else if (!strcmp(prop->name, "CRTC_X"))
				plane_prop.crtc_x = prop->prop_id;
			else if (!strcmp(prop->name, "CRTC_Y"))
				plane_prop.crtc_y = prop->prop_id;
			else if (!strcmp(prop->name, "CRTC_W"))
				plane_prop.crtc_w = prop->prop_id;
			else if (!strcmp(prop->name, "CRTC_H"))
				plane_prop.crtc_h = prop->prop_id;
			else if (!strcmp(prop->name, "ZPOS")) {
				plane_prop.zpos = prop->prop_id;
				zpos_max = props->prop_values[j];
				fprintf(stderr, "zpos max:%d\n", zpos_max);
			} else if (!strcmp(prop->name, "FEATURE")) {
				plane_prop.feature = prop->prop_id;
				drm_planes[i].support_scale = props->prop_values[j] & 0x1;
				fprintf(stderr, "scale:0x%llx\n", props->prop_values[j]);
			} else
				continue;
		}

		drm_planes[i].plane = plane;
		memcpy(&drm_planes[i].plane_prop, &plane_prop, sizeof(plane_prop));
	}

	/* get the first plane support scale */
	for (i = 0; i < plane_res->count_planes; i++) {
		//if (drm_planes[i].support_scale) 
		{
			plane = drm_planes[i].plane;
			memcpy(&plane_prop, &drm_planes[i].plane_prop, sizeof(plane_prop));
			break;
		}	
	}
	if (i == plane_res->count_planes)
		fprintf(stderr, "can't find correct plane\n");

	memset(drm_handle, 0, 4 * sizeof(struct rockchip_drm_handle_t));
	/* alloc buffer */
	drm_handle[0].format = DRM_FORMAT_XBGR8888;
	drm_handle[0].width = pic_w;
	drm_handle[0].height = pic_h;
	drm_handle[0].afbc = afbc;
	drm_handle[0].prime_fd = -1;
	drm_handle[0].flag = 0;
	drm_handle[0].file = file_name[0];

	drm_handle[1].format = DRM_FORMAT_XBGR8888;
	drm_handle[1].width = pic_w;
	drm_handle[1].height = pic_h;
	drm_handle[1].afbc = afbc;
	drm_handle[1].prime_fd = -1;
	drm_handle[1].flag = 0;
	drm_handle[1].file = file_name[1];

	drm_handle[2].format = DRM_FORMAT_XBGR8888;
	drm_handle[2].width = pic_w;
	drm_handle[2].height = pic_h;
	drm_handle[2].afbc = afbc;
	drm_handle[2].prime_fd = -1;
	drm_handle[2].flag = 0;
	drm_handle[2].file = file_name[2];

	drm_handle[3].format = DRM_FORMAT_XBGR8888;
	drm_handle[3].width = pic_w;
	drm_handle[3].height = pic_h;
	drm_handle[3].afbc = afbc;
	drm_handle[3].prime_fd = -1;
	drm_handle[3].flag = 0;
	drm_handle[3].file = file_name[2];

	for (i = 0; i < file_num; i++) {
		rockchip_buf_init(&drm_handle[i], &buf_info[i]);
	}
	/* enable crtc and connect */
	ret = drmModeSetCrtc(fd, crtc->crtc_id, buf_info[0].fb_id,
			     0, 0, conn_ids, res->count_connectors,
			     mode);
	if (ret) {
		fprintf(stderr, "drmModeSetCrtc faild:%d\n", ret);
	}
	rockchip_drm_commit(buf_info, file_num, drm_planes,
			    crtc, &crtc_prop, connector, &conn_prop);

	if (0) {
		usleep(5000 * 1000);
		fprintf(stderr, "hjc>>>second commit file_num:%d\n", file_num);
		for (i = 0; i < file_num; i++) {
			rockchip_buf_init(&drm_handle[i], &buf_info2[i]);
		}
		rockchip_drm_commit(buf_info2, file_num, drm_planes,
				    crtc, &crtc_prop, connector, &conn_prop);	

		usleep(5000 * 1000);
	}
	usleep(5000 * 1000);
	fprintf(stderr, "free first commit buffer \n");
	for (i = 0; i < file_num; i++) {
		fprintf(stderr, "remove fb:%d\n", buf_info[i].fb_id);
		if (buf_info[0].fb_id > 0) {
			drmModeRmFB(fd, buf_info[i].fb_id);
			rockchip_drm_gem_free(buf_info[i].bo);
		}
	}

	while(1);

	return ret;
}
