// SPDX-License-Identifier: GPL-2.0-only

#include <linux/pci.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "mgag200_drv.h"

static void mgag200_g200ew3_init_registers(struct mga_device *mdev)
{
	mgag200_g200wb_init_registers(mdev); // same as G200WB

	WREG_ECRT(0x34, 0x5); // G200EW3 specific
}

/*
 * PIXPLLC
 */

static int mgag200_g200ew3_pixpllc_atomic_check(struct drm_crtc *crtc,
						struct drm_atomic_state *new_state)
{
	static const unsigned int vcomax = 800000;
	static const unsigned int vcomin = 400000;
	static const unsigned int pllreffreq = 25000;

	struct drm_crtc_state *new_crtc_state = drm_atomic_get_new_crtc_state(new_state, crtc);
	struct mgag200_crtc_state *new_mgag200_crtc_state = to_mgag200_crtc_state(new_crtc_state);
	long clock = new_crtc_state->mode.clock;
	struct mgag200_pll_values *pixpllc = &new_mgag200_crtc_state->pixpllc;
	unsigned int delta, tmpdelta;
	unsigned int testp, testm, testn, testp2;
	unsigned int p, m, n, s;
	unsigned int computed;

	m = n = p = s = 0;
	delta = 0xffffffff;

	for (testp = 1; testp < 8; testp++) {
		for (testp2 = 1; testp2 < 8; testp2++) {
			if (testp < testp2)
				continue;
			if ((clock * testp * testp2) > vcomax)
				continue;
			if ((clock * testp * testp2) < vcomin)
				continue;
			for (testm = 1; testm < 26; testm++) {
				for (testn = 32; testn < 2048 ; testn++) {
					computed = (pllreffreq * testn) / (testm * testp * testp2);
					if (computed > clock)
						tmpdelta = computed - clock;
					else
						tmpdelta = clock - computed;
					if (tmpdelta < delta) {
						delta = tmpdelta;
						m = testm + 1;
						n = testn + 1;
						p = testp + 1;
						s = testp2;
					}
				}
			}
		}
	}

	pixpllc->m = m;
	pixpllc->n = n;
	pixpllc->p = p;
	pixpllc->s = s;

	return 0;
}

/*
 * Mode-setting pipeline
 */

static const struct drm_plane_helper_funcs mgag200_g200ew3_primary_plane_helper_funcs = {
	MGAG200_PRIMARY_PLANE_HELPER_FUNCS,
};

static const struct drm_plane_funcs mgag200_g200ew3_primary_plane_funcs = {
	MGAG200_PRIMARY_PLANE_FUNCS,
};

static const struct drm_crtc_helper_funcs mgag200_g200ew3_crtc_helper_funcs = {
	MGAG200_CRTC_HELPER_FUNCS,
};

static const struct drm_crtc_funcs mgag200_g200ew3_crtc_funcs = {
	MGAG200_CRTC_FUNCS,
};

static int mgag200_g200ew3_pipeline_init(struct mga_device *mdev)
{
	struct drm_device *dev = &mdev->base;
	struct drm_plane *primary_plane = &mdev->primary_plane;
	struct drm_crtc *crtc = &mdev->crtc;
	int ret;

	ret = drm_universal_plane_init(dev, primary_plane, 0,
				       &mgag200_g200ew3_primary_plane_funcs,
				       mgag200_primary_plane_formats,
				       mgag200_primary_plane_formats_size,
				       mgag200_primary_plane_fmtmods,
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret) {
		drm_err(dev, "drm_universal_plane_init() failed: %d\n", ret);
		return ret;
	}
	drm_plane_helper_add(primary_plane, &mgag200_g200ew3_primary_plane_helper_funcs);
	drm_plane_enable_fb_damage_clips(primary_plane);

	ret = drm_crtc_init_with_planes(dev, crtc, primary_plane, NULL,
					&mgag200_g200ew3_crtc_funcs, NULL);
	if (ret) {
		drm_err(dev, "drm_crtc_init_with_planes() failed: %d\n", ret);
		return ret;
	}
	drm_crtc_helper_add(crtc, &mgag200_g200ew3_crtc_helper_funcs);

	/* FIXME: legacy gamma tables, but atomic gamma doesn't work without */
	drm_mode_crtc_set_gamma_size(crtc, MGAG200_LUT_SIZE);
	drm_crtc_enable_color_mgmt(crtc, 0, false, MGAG200_LUT_SIZE);

	ret = mgag200_vga_output_init(mdev);
	if (ret)
		return ret;

	ret = mgag200_bmc_output_init(mdev, &mdev->output.vga.connector);
	if (ret)
		return ret;

	return 0;
}

/*
 * DRM device
 */

static const struct mgag200_device_info mgag200_g200ew3_device_info =
	MGAG200_DEVICE_INFO_INIT(2048, 2048, 0, true, 0, 1, false);

static const struct mgag200_device_funcs mgag200_g200ew3_device_funcs = {
	.pixpllc_atomic_check = mgag200_g200ew3_pixpllc_atomic_check,
	.pixpllc_atomic_update = mgag200_g200wb_pixpllc_atomic_update, // same as G200WB
};

static resource_size_t mgag200_g200ew3_device_probe_vram(struct mga_device *mdev)
{
	resource_size_t vram_size = resource_size(mdev->vram_res);

	if (vram_size >= 0x1000000)
		vram_size = vram_size - 0x400000;
	return mgag200_probe_vram(mdev->vram, vram_size);
}

struct mga_device *mgag200_g200ew3_device_create(struct pci_dev *pdev,
						 const struct drm_driver *drv)
{
	struct mga_device *mdev;
	struct drm_device *dev;
	resource_size_t vram_available;
	int ret;

	mdev = devm_drm_dev_alloc(&pdev->dev, drv, struct mga_device, base);
	if (IS_ERR(mdev))
		return mdev;
	dev = &mdev->base;

	pci_set_drvdata(pdev, dev);

	ret = mgag200_init_pci_options(pdev, 0x41049120, 0x0000b000);
	if (ret)
		return ERR_PTR(ret);

	ret = mgag200_device_preinit(mdev);
	if (ret)
		return ERR_PTR(ret);

	ret = mgag200_device_init(mdev, &mgag200_g200ew3_device_info,
				  &mgag200_g200ew3_device_funcs);
	if (ret)
		return ERR_PTR(ret);

	mgag200_g200ew3_init_registers(mdev);

	vram_available = mgag200_g200ew3_device_probe_vram(mdev);

	ret = mgag200_mode_config_init(mdev, vram_available);
	if (ret)
		return ERR_PTR(ret);

	ret = mgag200_g200ew3_pipeline_init(mdev);
	if (ret)
		return ERR_PTR(ret);

	drm_mode_config_reset(dev);
	drm_kms_helper_poll_init(dev);

	ret = drm_vblank_init(dev, 1);
	if (ret)
		return ERR_PTR(ret);

	return mdev;
}