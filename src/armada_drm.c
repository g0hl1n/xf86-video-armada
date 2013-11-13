/*
 * Marvell Armada DRM-based driver
 *
 * Written by Russell King, 2012, derived in part from the
 * Intel xorg X server driver.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#include <armada_bufmgr.h>

#include "armada_drm.h"
#include "common_drm.h"
#include "xf86_OSproc.h"
#include "xf86Crtc.h"
#include "xf86cmap.h"
#include "fb.h"
#include "micmap.h"
#include <xf86DDC.h>
#include <X11/extensions/dpmsconst.h>
#include <X11/Xatom.h>

#ifdef HAVE_UDEV
#include <libudev.h>
#endif

#include "compat-api.h"
#include "vivante.h"
#include "vivante_dri2.h"

#define CURSOR_MAX_WIDTH	64
#define CURSOR_MAX_HEIGHT	32

#define DRM_MODULE_NAME		"armada-drm"
#define DRM_DEFAULT_BUS_ID	NULL

const OptionInfoRec armada_drm_options[] = {
	{ OPTION_HW_CURSOR,	"HWcursor",	OPTV_BOOLEAN, {0}, FALSE },
	{ OPTION_XV_ACCEL,	"XvAccel",	OPTV_BOOLEAN, {0}, FALSE },
	{ OPTION_USE_GPU,	"UseGPU",	OPTV_BOOLEAN, {0}, FALSE },
	{ OPTION_HOTPLUG,	"HotPlug",	OPTV_BOOLEAN, {0}, TRUE  },
	{ -1,			NULL,		OPTV_NONE,    {0}, FALSE }
};

static void armada_drm_ModifyScreenPixmap(ScreenPtr pScreen,
	struct armada_drm_info *drm, int width, int height, int depth, int bpp,
	struct drm_armada_bo *bo)
{
	PixmapPtr pixmap = pScreen->GetScreenPixmap(pScreen);

	pScreen->ModifyPixmapHeader(pixmap, width, height, depth, bpp,
				    bo->pitch, bo->ptr);
	if (drm->accel)
		vivante_set_pixmap_bo(pixmap, bo);
}

static void drmmode_ConvertToKMode(drmModeModeInfoPtr kmode, DisplayModePtr mode)
{
	memset(kmode, 0, sizeof(*kmode));

	kmode->clock = mode->Clock;
	kmode->hdisplay = mode->HDisplay;
	kmode->hsync_start = mode->HSyncStart;
	kmode->hsync_end = mode->HSyncEnd;
	kmode->htotal = mode->HTotal;
	kmode->hskew = mode->HSkew;
	kmode->vdisplay = mode->VDisplay;
	kmode->vsync_start = mode->VSyncStart;
	kmode->vsync_end = mode->VSyncEnd;
	kmode->vtotal = mode->VTotal;
	kmode->vscan = mode->VScan;
	kmode->flags = mode->Flags;
	if (mode->name)
		strncpy(kmode->name, mode->name, DRM_DISPLAY_MODE_LEN);
	kmode->name[DRM_DISPLAY_MODE_LEN-1] = 0;
}

static struct drm_armada_bo *armada_bo_alloc_framebuffer(ScrnInfoPtr pScrn,
	int width, int height, int bpp)
{
	struct armada_drm_info *drm = GET_DRM_INFO(pScrn);
	struct drm_armada_bo *bo;
	int ret;

	bo = drm_armada_bo_dumb_create(drm->bufmgr, width, height, bpp);
	if (!bo) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] failed to allocate new bo: %s\n",
			   strerror(errno));
		return NULL;
	}

	ret = drm_armada_bo_map(bo);
	if (ret) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] failed to map fb bo: %s\n", strerror(errno));
		drm_armada_bo_put(bo);
		return NULL;
	}

//	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
//		   "Allocated new frame buffer %dx%d stride %d\n",
//		   width, height, bo->pitch);

	return bo;
}

/*
 * CRTC support
 */
static Bool armada_drm_crtc_apply(xf86CrtcPtr crtc, drmModeModeInfoPtr kmode)
{
	ScrnInfoPtr pScrn = crtc->scrn;
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	struct armada_crtc_info *drmc = armada_crtc(crtc);
	uint32_t fb_id, *output_ids;
	int x, y, i, ret, output_num;

	if (!kmode)
		kmode = &drmc->kmode;

	output_ids = calloc(xf86_config->num_output, sizeof *output_ids);
	if (!output_ids)
		return FALSE;

	for (output_num = i = 0; i < xf86_config->num_output; i++) {
		xf86OutputPtr output = xf86_config->output[i];
		struct armada_conn_info *conn;

		if (output->crtc == crtc) {
			conn = output->driver_private;
			output_ids[output_num++] = conn->mode_output->connector_id;
		}
	}

	if (!xf86CrtcRotate(crtc)) {
		ret = FALSE;
		goto done;
	}

	crtc->funcs->gamma_set(crtc, crtc->gamma_red, crtc->gamma_green,
			       crtc->gamma_blue, crtc->gamma_size);

	if (drmc->rotate_fb_id) {
		fb_id = drmc->rotate_fb_id;
		x = y = 0;
	} else {
		fb_id = drmc->drm->fb_id;
		x = crtc->x;
		y = crtc->y;
	}

	ret = drmModeSetCrtc(drmc->drm_fd, drmc->mode_crtc->crtc_id, fb_id,
			     x, y, output_ids, output_num, kmode);
	if (ret) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] failed to set mode on crtc %u: %s\n",
			   drmc->mode_crtc->crtc_id, strerror(errno));
		ret = FALSE;
	} else {
		ret = TRUE;

		for (i = 0; i < xf86_config->num_output; i++) {
			xf86OutputPtr output = xf86_config->output[i];

			if (output->crtc != crtc)
				continue;

			output->funcs->dpms(output, DPMSModeOn);
		}
	}

	/* Work around stricter checks in X */
	if (pScrn->pScreen && drmc->drm->hw_cursor)
		xf86_reload_cursors(pScrn->pScreen);

done:
	free(output_ids);
	return ret;
}

static void armada_drm_crtc_dpms(xf86CrtcPtr crtc, int mode)
{
}

static Bool
armada_drm_crtc_set_mode_major(xf86CrtcPtr crtc, DisplayModePtr mode,
	Rotation rotation, int x, int y)
{
	ScrnInfoPtr pScrn = crtc->scrn;
	struct armada_crtc_info *drmc = armada_crtc(crtc);
	struct armada_drm_info *drm = drmc->drm;
	drmModeModeInfo kmode;
	DisplayModeRec saved_mode;
	Rotation saved_rotation;
	int ret, saved_x, saved_y;

	if (drm->fb_id == 0) {
		ret = drmModeAddFB(drmc->drm_fd,
				   pScrn->virtualX, pScrn->virtualY,
				   pScrn->depth, pScrn->bitsPerPixel,
				   drm->front_bo->pitch, drm->front_bo->handle,
				   &drm->fb_id);
		if (ret < 0) {
			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				   "[drm] failed to add fb: %s\n",
				   strerror(errno));
			return FALSE;
		}
	}

	saved_mode = crtc->mode;
	saved_x = crtc->x;
	saved_y = crtc->y;
	saved_rotation = crtc->rotation;
	crtc->mode = *mode;
	crtc->x = x;
	crtc->y = y;
	crtc->rotation = rotation;

	drmmode_ConvertToKMode(&kmode, mode);

	ret = armada_drm_crtc_apply(crtc, &kmode);
	if (!ret) {
		crtc->mode = saved_mode;
		crtc->x = saved_x;
		crtc->y = saved_y;
		crtc->rotation = saved_rotation;
	} else
		drmc->kmode = kmode;
	return ret;
}

static void armada_drm_crtc_gamma_set(xf86CrtcPtr crtc, CARD16 *red,
	CARD16 *green, CARD16 *blue, int size)
{
	struct armada_crtc_info *drmc = armada_crtc(crtc);

	drmModeCrtcSetGamma(drmc->drm_fd, drmc->mode_crtc->crtc_id,
			    size, red, green, blue);
}

static void armada_drm_crtc_set_cursor_position(xf86CrtcPtr crtc, int x, int y)
{
	struct armada_crtc_info *drmc = armada_crtc(crtc);

	drmModeMoveCursor(drmc->drm_fd, drmc->mode_crtc->crtc_id, x, y);
}

static void armada_drm_crtc_show_cursor(xf86CrtcPtr crtc)
{
	struct armada_crtc_info *drmc = armada_crtc(crtc);

	drmModeSetCursor(drmc->drm_fd, drmc->mode_crtc->crtc_id,
			 drmc->cursor_bo->handle,
			 drmc->cursor_max_width, drmc->cursor_max_height);
}

static void armada_drm_crtc_hide_cursor(xf86CrtcPtr crtc)
{
	struct armada_crtc_info *drmc = armada_crtc(crtc);

	drmModeSetCursor(drmc->drm_fd, drmc->mode_crtc->crtc_id, 0, 0, 0);
}

static void armada_drm_crtc_load_cursor_argb(xf86CrtcPtr crtc, CARD32 *image)
{
	struct armada_crtc_info *drmc = armada_crtc(crtc);

	drm_armada_bo_subdata(drmc->cursor_bo, 0,
			      drmc->cursor_max_width *
			      drmc->cursor_max_height * 4, image);
}

static void *
armada_drm_crtc_shadow_allocate(xf86CrtcPtr crtc, int width, int height)
{
	struct armada_crtc_info *drmc = armada_crtc(crtc);
	ScrnInfoPtr pScrn = crtc->scrn;
	struct drm_armada_bo *bo;
	int ret;

	bo = armada_bo_alloc_framebuffer(pScrn, width, height,
					 pScrn->bitsPerPixel);
	if (!bo) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Failed to allocate shadow memory for rotated CRTC\n");
		return NULL;
	}

	ret = drmModeAddFB(drmc->drm_fd, width, height, pScrn->depth,
			   pScrn->bitsPerPixel, bo->pitch, bo->handle,
			   &drmc->rotate_fb_id);
	if (ret) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Failed to add rotate fb: %s\n",
			   strerror(errno));
		drm_armada_bo_put(bo);
		return NULL;
	}

	return bo;
}

static PixmapPtr
armada_drm_crtc_shadow_create(xf86CrtcPtr crtc, void *data,
	int width, int height)
{
	struct armada_crtc_info *drmc = armada_crtc(crtc);
	ScrnInfoPtr scrn = crtc->scrn;
	PixmapPtr rotate_pixmap;
	struct drm_armada_bo *bo;

	if (!data)
		data = armada_drm_crtc_shadow_allocate(crtc, width, height);
	if (!data) {
		xf86DrvMsg(scrn->scrnIndex, X_ERROR,
			   "Failed to allocate shadow pixmap data for rotated CRTC\n");
		return NULL;
	}

	bo = data;

	rotate_pixmap = GetScratchPixmapHeader(scrn->pScreen, width, height,
					   scrn->depth, scrn->bitsPerPixel,
					   bo->pitch, bo->ptr);
	if (!rotate_pixmap) {
		xf86DrvMsg(scrn->scrnIndex, X_ERROR,
			   "Failed to allocate shadow pixmap for rotated CRTC\n");
		return NULL;
	}

	if (drmc->drm->accel)
		vivante_set_pixmap_bo(rotate_pixmap, bo);

	return rotate_pixmap;
}

static void
armada_drm_crtc_shadow_destroy(xf86CrtcPtr crtc, PixmapPtr rot_pixmap,
	void *data)
{
	if (rot_pixmap) {
		struct armada_drm_info *drm = GET_DRM_INFO(crtc->scrn);
		if (drm->accel)
			vivante_free_pixmap(rot_pixmap);
		FreeScratchPixmapHeader(rot_pixmap);
	}
	if (data) {
		struct armada_crtc_info *drmc = armada_crtc(crtc);

		drmModeRmFB(drmc->drm_fd, drmc->rotate_fb_id);
		drmc->rotate_fb_id = 0;

		drm_armada_bo_put(data);
	}
}

static void armada_drm_crtc_destroy(xf86CrtcPtr crtc)
{
	struct armada_crtc_info *drmc = armada_crtc(crtc);

	if (drmc->cursor_bo) {
		drmModeSetCursor(drmc->drm_fd, drmc->mode_crtc->crtc_id,
				 0, 0, 0);
		drm_armada_bo_put(drmc->cursor_bo);
	}
	drmModeFreeCrtc(drmc->mode_crtc);

	free(drmc);
}

static const xf86CrtcFuncsRec drm_crtc_funcs = {
	.dpms = armada_drm_crtc_dpms,
	.gamma_set = armada_drm_crtc_gamma_set,
	.set_mode_major = armada_drm_crtc_set_mode_major,
	.set_cursor_position = armada_drm_crtc_set_cursor_position,
	.show_cursor = armada_drm_crtc_show_cursor,
	.hide_cursor = armada_drm_crtc_hide_cursor,
	.load_cursor_argb = armada_drm_crtc_load_cursor_argb,
	.shadow_create = armada_drm_crtc_shadow_create,
	.shadow_allocate = armada_drm_crtc_shadow_allocate,
	.shadow_destroy = armada_drm_crtc_shadow_destroy,
	.destroy = armada_drm_crtc_destroy,
};

static Bool
armada_drm_crtc_init(ScrnInfoPtr pScrn, struct armada_drm_info *drm,
	unsigned num)
{
	struct armada_crtc_info *drmc;
	xf86CrtcPtr crtc;
	uint32_t id;

	id = drm->mode_res->crtcs[num];

	crtc = xf86CrtcCreate(pScrn, &drm_crtc_funcs);
	if (!crtc)
		return FALSE;

	drmc = xnfcalloc(1, sizeof *drmc);
	if (!drmc)
		return FALSE;

	drmc->drm_fd = drm->fd;
	drmc->drm = drm;
	drmc->num = num;
	drmc->cursor_max_width = CURSOR_MAX_WIDTH;
	drmc->cursor_max_height = CURSOR_MAX_HEIGHT;
	drmc->mode_crtc = drmModeGetCrtc(drmc->drm_fd, id);
	crtc->driver_private = drmc;

	/* Test whether hardware cursor is supported */
	if (drmModeSetCursor(drmc->drm_fd, id, 0, 0, 0))
		drm->has_hw_cursor = FALSE;

	drmc->cursor_bo = drm_armada_bo_dumb_create(drm->bufmgr,
						    drmc->cursor_max_width,
						    drmc->cursor_max_height,
						    32);

	return TRUE;
}

static Bool armada_drm_xf86crtc_resize(ScrnInfoPtr pScrn, int width, int height)
{
	ScreenPtr screen = screenInfo.screens[pScrn->scrnIndex];
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	struct armada_drm_info *drm = GET_DRM_INFO(pScrn);
	struct drm_armada_bo *bo, *old_bo;
	uint32_t old_fb_id;
	int ret, i;

	if (pScrn->virtualX == width && pScrn->virtualY == height)
		return TRUE;

	bo = armada_bo_alloc_framebuffer(pScrn, width, height,
					 pScrn->bitsPerPixel);
	if (!bo)
		return FALSE;

	old_fb_id = drm->fb_id;
	ret = drmModeAddFB(drm->fd, width, height,
			   pScrn->depth, pScrn->bitsPerPixel,
			   bo->pitch, bo->handle, &drm->fb_id);
	if (ret) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] failed to add fb: %s\n", strerror(errno)); 
		drm_armada_bo_put(bo);
		return FALSE;
	}

	/* Okay, now switch everything */
	pScrn->virtualX = width;
	pScrn->virtualY = height;
	pScrn->displayWidth = bo->pitch / drm->cpp;
	old_bo = drm->front_bo;
	drm->front_bo = bo;

	armada_drm_ModifyScreenPixmap(screen, drm, width, height, -1, -1, bo);

	for (i = 0; i < xf86_config->num_crtc; i++) {
		xf86CrtcPtr crtc = xf86_config->crtc[i];

		if (!crtc->enabled)
			continue;

		armada_drm_crtc_apply(crtc, NULL);
	}

	drmModeRmFB(drm->fd, old_fb_id);
	drm_armada_bo_put(old_bo);
	return TRUE;
}

static const xf86CrtcConfigFuncsRec armada_drm_config_funcs = {
	armada_drm_xf86crtc_resize,
};

#ifdef HAVE_UDEV
static void armada_drm_handle_uevent(int fd, pointer data)
{
	ScrnInfoPtr pScrn = data;
	struct armada_drm_info *drm = GET_DRM_INFO(pScrn);
	struct udev_device *ud;

	ud = udev_monitor_receive_device(drm->udev.monitor);
	if (ud) {
		dev_t dev = udev_device_get_devnum(ud);
		const char *hp = udev_device_get_property_value(ud, "HOTPLUG");

		if (dev == drm->udev.drm_dev && hp && strtol(hp, NULL, 10) == 1)
			RRGetInfo(screenInfo.screens[pScrn->scrnIndex], TRUE);

		udev_device_unref(ud);
	}
}

static Bool armada_drm_udev_init(ScrnInfoPtr pScrn)
{
	struct armada_drm_info *drm = GET_DRM_INFO(pScrn);
	struct udev_monitor *udev_mon;
	struct udev *udev;
	struct stat st;
	MessageType from = X_CONFIG;
	Bool hotplug;

	if (!xf86GetOptValBool(drm->Options, OPTION_HOTPLUG, &hotplug)) {
		from = X_DEFAULT;
		hotplug = TRUE;
	}

	xf86DrvMsg(pScrn->scrnIndex, from, "hotplug detection %sabled\n",
		   hotplug ? "en" : "dis");
	if (!hotplug)
		return TRUE;

	if (fstat(drm->fd, &st) || !S_ISCHR(st.st_mode))
		return FALSE;

	drm->udev.drm_dev = st.st_rdev;

	udev = udev_new();
	if (!udev)
		return FALSE;

	udev_mon = udev_monitor_new_from_netlink(udev, "udev");
	if (!udev_mon) {
		udev_unref(udev);
		return FALSE;
	}

	if (udev_monitor_filter_add_match_subsystem_devtype(udev_mon,
				   "drm", "drm_minor") ||
		udev_monitor_enable_receiving(udev_mon)) {
		udev_monitor_unref(udev_mon);
		udev_unref(udev);
		return FALSE;
	}

	drm->udev.monitor = udev_mon;
	drm->udev.handler = xf86AddGeneralHandler(udev_monitor_get_fd(udev_mon),
						  armada_drm_handle_uevent,
						  pScrn);

	return TRUE;
}

static void armada_drm_udev_fini(ScrnInfoPtr pScrn, struct armada_drm_info *drm)
{
	if (drm->udev.monitor) {
		struct udev *udev = udev_monitor_get_udev(drm->udev.monitor);

		xf86RemoveGeneralHandler(drm->udev.handler);
		udev_monitor_unref(drm->udev.monitor);
		udev_unref(udev);
	}
}
#endif

static void armada_drm_LoadPalette(ScrnInfoPtr pScrn, int num, int *indices,
	LOCO *colors, VisualPtr pVisual)
{
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	uint16_t lut_r[256], lut_g[256], lut_b[256];
	int i, p;

	for (i = 0; i < num; i++) {
		int index = indices[i];
		lut_r[index] = colors[index].red << 8;
		lut_g[index] = colors[index].green << 8;
		lut_b[index] = colors[index].blue << 8;
	}

	for (p = 0; p < xf86_config->num_crtc; p++) {
		xf86CrtcPtr crtc = xf86_config->crtc[p];

#ifdef RANDR_12_INTERFACE
		RRCrtcGammaSet(crtc->randr_crtc, lut_r, lut_g, lut_b);
#else
		crtc->funcs->gamma_set(crtc, lut_r, lut_g, lut_b, 256);
#endif
	}
}

static void armada_drm_AdjustFrame(ADJUST_FRAME_ARGS_DECL)
{
	SCRN_INFO_PTR(arg);
	xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(pScrn);
	xf86OutputPtr output = config->output[config->compat_output];
	xf86CrtcPtr crtc = output->crtc;

	if (crtc && crtc->enabled) {
		int saved_x = crtc->x;
		int saved_y = crtc->y;
		int ret;

		crtc->x = x;
		crtc->y = y;

		ret = crtc->funcs->set_mode_major(crtc, &crtc->mode,
						  crtc->rotation, x, y);
		if (!ret) {
			crtc->x = saved_x;
			crtc->y = saved_y;
		}
	}
}

static Bool armada_drm_EnterVT(VT_FUNC_ARGS_DECL)
{
	SCRN_INFO_PTR(arg);
	xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(pScrn);
	struct armada_drm_info *drm = GET_DRM_INFO(pScrn);
	int i;

	if (drmSetMaster(drm->fd))
		xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
			   "[drm] set master failed: %s\n", strerror(errno));

	if (!xf86SetDesiredModes(pScrn))
		return FALSE;

	/* Disable unused CRTCs */
	for (i = 0; i < config->num_crtc; i++) {
		xf86CrtcPtr crtc = config->crtc[i];
		struct armada_crtc_info *drmc = armada_crtc(crtc);
		if (!crtc->enabled)
			drmModeSetCrtc(drm->fd, drmc->mode_crtc->crtc_id,
				       0, 0, 0, NULL, 0, NULL);
	}

	return TRUE;
}

static void armada_drm_LeaveVT(VT_FUNC_ARGS_DECL)
{
	SCRN_INFO_PTR(arg);
	struct armada_drm_info *drm = GET_DRM_INFO(pScrn);

	xf86RotateFreeShadow(pScrn);

	xf86_hide_cursors(pScrn);

	drmDropMaster(drm->fd);
}

static ModeStatus
armada_drm_ValidMode(SCRN_ARG_TYPE arg1, DisplayModePtr mode, Bool verbose,
		     int flags)
{
	SCRN_INFO_PTR(arg1);

	if (mode->Flags & V_DBLSCAN) {
		if (verbose)
			xf86DrvMsg(pScrn->scrnIndex, X_PROBED,
				   "Removing double-scanned mode \"%s\"\n",
				   mode->name);
		return MODE_BAD;
	}

	return MODE_OK;
}

static Bool armada_drm_SwitchMode(SWITCH_MODE_ARGS_DECL)
{
	SCRN_INFO_PTR(arg);

	return xf86SetSingleMode(pScrn, mode, RR_Rotate_0);
}

static Bool armada_drm_CloseScreen(CLOSE_SCREEN_ARGS_DECL)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	struct armada_drm_info *drm = GET_DRM_INFO(pScrn);
	PixmapPtr pixmap = pScreen->GetScreenPixmap(pScreen);
	Bool ret;

#ifdef HAVE_UDEV
	armada_drm_udev_fini(pScrn, drm);
#endif

	if (drm->fb_id) {
		drmModeRmFB(drm->fd, drm->fb_id);
		drm->fb_id = 0;
	}
	if (drm->front_bo) {
		drm_armada_bo_put(drm->front_bo);
		drm->front_bo = NULL;
	}
	if (drm->accel)
		vivante_free_pixmap(pixmap);

	if (drm->hw_cursor)
		xf86_cursors_fini(pScreen);

	pScreen->CloseScreen = drm->CloseScreen;
	ret = (*pScreen->CloseScreen)(CLOSE_SCREEN_ARGS);

	if (pScrn->vtSema)
		armada_drm_LeaveVT(VT_FUNC_ARGS(0));

	pScrn->vtSema = FALSE;

	return ret;
}

static Bool armada_drm_CreateScreenResources(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
	struct armada_drm_info *drm = GET_DRM_INFO(pScrn);
	Bool ret;

	pScreen->CreateScreenResources = drm->CreateScreenResources;
	ret = pScreen->CreateScreenResources(pScreen);
	if (ret) {
		struct drm_armada_bo *bo = drm->front_bo;

		armada_drm_ModifyScreenPixmap(pScreen, drm, -1, -1, -1, -1, bo);
	}
	return ret;
}

static void armada_drm_wakeup_handler(pointer data, int err, pointer p)
{
	struct armada_drm_info *drm = data;
	fd_set *read_mask = p;

	if (data == NULL || err < 0)
		return;

	if (FD_ISSET(drm->fd, read_mask))
		drmHandleEvent(drm->fd, &drm->event_context);
}

static Bool
armada_drm_ScreenInit(SCREEN_INIT_ARGS_DECL)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	struct armada_drm_info *drm = GET_DRM_INFO(pScrn);
	struct drm_armada_bo *bo;
	int visuals, preferredCVC;

	if (drmSetMaster(drm->fd)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] set master failed: %s\n", strerror(errno));
		return FALSE;
	}

	drm->accel = xf86ReturnOptValBool(drm->Options, OPTION_USE_GPU, TRUE);

	bo = armada_bo_alloc_framebuffer(pScrn, pScrn->virtualX,
					 pScrn->virtualY, pScrn->bitsPerPixel);
	if (!bo)
		return FALSE;

	drm->front_bo = bo;
	pScrn->displayWidth = bo->pitch / drm->cpp;

	miClearVisualTypes();
	if (pScrn->bitsPerPixel > 8) {
		visuals = TrueColorMask;
		preferredCVC = TrueColor;
	} else {
		visuals = miGetDefaultVisualMask(pScrn->depth);
		preferredCVC = pScrn->defaultVisual;
	}

	if (!miSetVisualTypes(pScrn->depth, visuals, pScrn->rgbBits, preferredCVC)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] failed to set visual types for %d bpp depth %d\n",
			   pScrn->bitsPerPixel, pScrn->depth);
		return FALSE;
	}

	if (!miSetPixmapDepths()) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] failed to set pixmap depths\n");
		return FALSE;
	}

	if (!fbScreenInit(pScreen, NULL, pScrn->virtualX, pScrn->virtualY,
			  pScrn->xDpi, pScrn->yDpi, pScrn->displayWidth,
			  pScrn->bitsPerPixel)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] fbScreenInit failed\n");
		return FALSE;
	}

	if (pScrn->bitsPerPixel > 8) {
		/* Fixup RGB ordering */
		VisualPtr visual = pScreen->visuals + pScreen->numVisuals;
		while (--visual >= pScreen->visuals) {
			if ((visual->class | DynamicClass) == DirectColor) {
				visual->offsetRed   = pScrn->offset.red;
				visual->offsetGreen = pScrn->offset.green;
				visual->offsetBlue  = pScrn->offset.blue;
				visual->redMask     = pScrn->mask.red;
				visual->greenMask   = pScrn->mask.green;
				visual->blueMask    = pScrn->mask.blue;
			}
		}
	}

	if (!fbPictureInit(pScreen, NULL, 0)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] fbPictureInit failed\n");
		return FALSE;
	}

	xf86SetBlackWhitePixels(pScreen);

	if (drm->accel && !vivante_ScreenInit(pScreen, drm->bufmgr)) {
		xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
			   "[drm] Vivante initialization failed, running unaccelerated\n");
		drm->accel = FALSE;
	}

	xf86SetBackingStore(pScreen);
	xf86SetSilkenMouse(pScreen);

	/* software cursor */
	miDCInitialize(pScreen, xf86GetPointerScreenFuncs());

	drm->hw_cursor = xf86ReturnOptValBool(drm->Options, OPTION_HW_CURSOR,
					      drm->has_hw_cursor);
	if (drm->hw_cursor && !drm->has_hw_cursor) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "No hardware cursor support - disabling hardware cursors\n");
		drm->hw_cursor = FALSE;
	}
	if (drm->hw_cursor &&
		xf86_cursors_init(pScreen,
				  CURSOR_MAX_WIDTH, CURSOR_MAX_HEIGHT,
				  HARDWARE_CURSOR_TRUECOLOR_AT_8BPP |
				  HARDWARE_CURSOR_BIT_ORDER_MSBFIRST |
				  HARDWARE_CURSOR_INVERT_MASK |
				  HARDWARE_CURSOR_SWAP_SOURCE_AND_MASK |
				  HARDWARE_CURSOR_AND_SOURCE_WITH_MASK |
				  HARDWARE_CURSOR_SOURCE_MASK_INTERLEAVE_64 |
				  HARDWARE_CURSOR_UPDATE_UNHIDDEN |
				  HARDWARE_CURSOR_ARGB)) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "Using hardware cursors\n");
	} else {
		drm->hw_cursor = FALSE;
	}

	pScreen->SaveScreen = xf86SaveScreen;
	drm->CloseScreen = pScreen->CloseScreen;
	pScreen->CloseScreen = armada_drm_CloseScreen;
	drm->CreateScreenResources = pScreen->CreateScreenResources;
	pScreen->CreateScreenResources = armada_drm_CreateScreenResources;

	if (!xf86CrtcScreenInit(pScreen)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] failed to initialize screen\n");
		return FALSE;
	}

	if (!miCreateDefColormap(pScreen)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] failed to initialize default colormap\n");
		return FALSE;
	}

	if (!xf86HandleColormaps(pScreen, 256, 8, armada_drm_LoadPalette, NULL,
				 CMAP_RELOAD_ON_MODE_SWITCH |
				 CMAP_PALETTED_TRUECOLOR)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] failed to initialize colormap handler\n");
		return FALSE;
	}

	xf86DPMSInit(pScreen, xf86DPMSSet, 0);

	if (xf86ReturnOptValBool(drm->Options, OPTION_XV_ACCEL, TRUE))
		armada_drm_XvInit(pScrn);

	/* Setup the synchronisation feedback */
	AddGeneralSocket(drm->fd);
	RegisterBlockAndWakeupHandlers((BlockHandlerProcPtr)NoopDDA,
				       armada_drm_wakeup_handler, drm);

#ifdef HAVE_UDEV
	if (!armada_drm_udev_init(pScrn)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] failed to connect with udev: %s\n",
			   strerror(errno));
		return FALSE;
	}
#endif

	pScrn->vtSema = TRUE;

	return armada_drm_EnterVT(VT_FUNC_ARGS(0));
}

static Bool armada_drm_pre_init(ScrnInfoPtr pScrn)
{
	struct armada_drm_info *drm = GET_DRM_INFO(pScrn);
	drmVersionPtr version;
	Gamma zeros = { 0.0, 0.0, 0.0 };
	int i;

	xf86CollectOptions(pScrn, NULL);
	drm->Options = malloc(sizeof(armada_drm_options));
	if (!drm->Options)
		return FALSE;
	memcpy(drm->Options, armada_drm_options, sizeof(armada_drm_options));
	xf86ProcessOptions(pScrn->scrnIndex, pScrn->options, drm->Options);

	version = drmGetVersion(drm->fd);
	if (version) { 
		xf86DrvMsg(pScrn->scrnIndex, X_INFO, "hardware: %s\n",
			   version->name);
		drmFreeVersion(version);
	}

	drm->cpp = (pScrn->bitsPerPixel + 7) / 8;

	xf86CrtcConfigInit(pScrn, &armada_drm_config_funcs);

	drm->mode_res = drmModeGetResources(drm->fd);
	if (!drm->mode_res) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "failed to get resources: %s\n", strerror(errno));
		return FALSE;
	}

	xf86CrtcSetSizeRange(pScrn, drm->mode_res->min_width,
			     drm->mode_res->min_height,
			     drm->mode_res->max_width,
			     drm->mode_res->max_height);

	drm->has_hw_cursor = TRUE;
	for (i = 0; i < drm->mode_res->count_crtcs; i++)
		if (!armada_drm_crtc_init(pScrn, drm, i))
			return FALSE;

	for (i = 0; i < drm->mode_res->count_connectors; i++)
		common_drm_conn_init(pScrn, drm->fd,
				     drm->mode_res->connectors[i]);

	xf86InitialConfiguration(pScrn, TRUE);

	/* Limit the maximum framebuffer size to 16MB */
	pScrn->videoRam = 16 * 1048576;

	drm->event_context.version = DRM_EVENT_CONTEXT_VERSION;
#ifdef HAVE_DRI2
	drm->event_context.vblank_handler = vivante_dri2_vblank;
#else
	drm->event_context.vblank_handler = NULL;
#endif
	drm->event_context.page_flip_handler = NULL;

	if (!xf86SetGamma(pScrn, zeros))
		return FALSE;

	if (pScrn->modes == NULL) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "No modes.\n");
		return FALSE;
	}

	pScrn->currentMode = pScrn->modes;

	/* Set display resolution */
	xf86SetDpi(pScrn, 0, 0);

	if (!xf86LoadSubModule(pScrn, "fb"))
		return FALSE;

	return TRUE;
}

static int armada_get_cap(struct armada_drm_info *drm, uint64_t cap,
	uint64_t *val, int scrnIndex, const char *name)
{
	int err;

	err = drmGetCap(drm->fd, cap, val);
	if (err)
		xf86DrvMsg(scrnIndex, X_ERROR,
			   "[drm] failed to get %s capability: %s\n",
			   name, strerror(errno));

	return err;
}

static Bool armada_drm_open_master(ScrnInfoPtr pScrn)
{
	struct armada_drm_info *drm;
	EntityInfoPtr pEnt;
	drmSetVersion sv;
	const char *busid = DRM_DEFAULT_BUS_ID;
	uint64_t val;
	int err;

	pEnt = xf86GetEntityInfo(pScrn->entityList[0]);
	if (pEnt) {
		if (pEnt->device->busID)
			busid = pEnt->device->busID;
		free(pEnt);
	}

	if (busid)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Using BusID \"%s\"\n",
			   busid);

	drm = calloc(1, sizeof *drm);
	if (!drm)
		return FALSE;

	drm->fd = drmOpen(DRM_MODULE_NAME, busid);
	if (drm->fd < 0) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] Failed to open DRM device for %s: %s\n",
			   busid, strerror(errno));
		goto err_free;
	}

	/*
	 * Check that what we opened was a master or a master-capable FD
	 * by setting the version of the interface we'll use to talk to it.
	 */
	sv.drm_di_major = 1;
	sv.drm_di_minor = 1;
	sv.drm_dd_major = -1;
	sv.drm_dd_minor = -1;
	err = drmSetInterfaceVersion(drm->fd, &sv);
	if (err) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] failed to set DRM interface version: %s\n",
			   strerror(errno));
		goto err_drm_close;
	}

	if (armada_get_cap(drm, DRM_CAP_PRIME, &val,
			   pScrn->scrnIndex, "DRM_CAP_PRIME"))
		goto err_drm_close;
	if (!(val & DRM_PRIME_CAP_EXPORT)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] kernel doesn't support prime export.\n");
		goto err_drm_close;
	}

	if (armada_get_cap(drm, DRM_CAP_DUMB_BUFFER, &val,
			   pScrn->scrnIndex, "DRM_CAP_DUMB_BUFFER"))
		goto err_drm_close;
	if (!val) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] kernel doesn't support dumb buffer.\n");
		goto err_drm_close;
	}

	err = drm_armada_init(drm->fd, &drm->bufmgr);
	if (err) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] failed to initialize Armada DRM manager.\n");
		goto err_drm_close;
	}

	SET_DRM_INFO(pScrn, drm);

	return TRUE;

 err_drm_close:
	drmClose(drm->fd);
 err_free:
	free(drm);
	return FALSE;
}

static void armada_drm_close_master(ScrnInfoPtr pScrn)
{
	struct armada_drm_info *drm = GET_DRM_INFO(pScrn);

	if (drm) {
		drm_armada_fini(drm->bufmgr);
		drmClose(drm->fd);
		SET_DRM_INFO(pScrn, NULL);
		free(drm);
	}
}

static void armada_drm_FreeScreen(FREE_SCREEN_ARGS_DECL)
{
	SCRN_INFO_PTR(arg);

	armada_drm_close_master(pScrn);
}

static Bool armada_drm_PreInit(ScrnInfoPtr pScrn, int flags)
{
	rgb defaultWeight = { 0, 0, 0 };
	int flags24;

	if (pScrn->numEntities != 1)
		return FALSE;

	if (flags & PROBE_DETECT)
		return FALSE;

	if (!armada_drm_open_master(pScrn)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Failed to become DRM master.\n");
		return FALSE;
	}

	pScrn->monitor = pScrn->confScreen->monitor;
	pScrn->progClock = TRUE;
	pScrn->rgbBits = 8;
	pScrn->chipset = "fbdev";
	pScrn->displayWidth = 640;

	flags24 = Support24bppFb | Support32bppFb | SupportConvert24to32;
	if (!xf86SetDepthBpp(pScrn, 0, 0, 0, flags24))
		goto fail;

	switch (pScrn->depth) {
	case 8:
	case 15:
	case 16:
	case 24:
		break;
	default:
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Given depth (%d) is not supported.\n",
			   pScrn->depth);
		goto fail;
	}

	xf86PrintDepthBpp(pScrn);

	if (!xf86SetWeight(pScrn, defaultWeight, defaultWeight))
		goto fail;
	if (!xf86SetDefaultVisual(pScrn, -1))
		goto fail;

	if (pScrn->depth > 8 && pScrn->defaultVisual != TrueColor) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Requested default visual  (%s) is not supported at depth %d\n",
			   xf86GetVisualName(pScrn->defaultVisual),
			   pScrn->depth);
		goto fail;
	}

	if (!armada_drm_pre_init(pScrn))
		goto fail;

	return TRUE;

 fail:
	armada_drm_FreeScreen(FREE_SCREEN_ARGS(pScrn));
	return FALSE;
}

Bool armada_drm_init_screen(ScrnInfoPtr pScrn)
{
	pScrn->PreInit = armada_drm_PreInit;
	pScrn->ScreenInit = armada_drm_ScreenInit;
	pScrn->SwitchMode = armada_drm_SwitchMode;
	pScrn->AdjustFrame = armada_drm_AdjustFrame;
	pScrn->EnterVT = armada_drm_EnterVT;
	pScrn->LeaveVT = armada_drm_LeaveVT;
	pScrn->FreeScreen = armada_drm_FreeScreen;
	pScrn->ValidMode = armada_drm_ValidMode;

	return TRUE;
}
