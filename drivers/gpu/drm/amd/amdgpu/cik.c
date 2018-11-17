/*
 * Copyright 2012 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: Alex Deucher
 */
#include <linux/firmware.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <drm/drmP.h>
#include "amdgpu.h"
#include "amdgpu_atombios.h"
#include "amdgpu_ih.h"
#include "amdgpu_uvd.h"
#include "amdgpu_vce.h"
#include "cikd.h"
#include "atom.h"
#include "amd_pcie.h"

#include "cik.h"
#include "gmc_v7_0.h"
#include "cik_ih.h"
#include "dce_v8_0.h"
#include "gfx_v7_0.h"
#include "cik_sdma.h"
#include "uvd_v4_2.h"
#include "vce_v2_0.h"
#include "cik_dpm.h"

#include "uvd/uvd_4_2_d.h"

#include "smu/smu_7_0_1_d.h"
#include "smu/smu_7_0_1_sh_mask.h"

#include "dce/dce_8_0_d.h"
#include "dce/dce_8_0_sh_mask.h"

#include "bif/bif_4_1_d.h"
#include "bif/bif_4_1_sh_mask.h"

#include "gca/gfx_7_2_d.h"
#include "gca/gfx_7_2_enum.h"
#include "gca/gfx_7_2_sh_mask.h"

#include "gmc/gmc_7_1_d.h"
#include "gmc/gmc_7_1_sh_mask.h"

#include "oss/oss_2_0_d.h"
#include "oss/oss_2_0_sh_mask.h"

#include "amdgpu_amdkfd.h"
#include "amdgpu_powerplay.h"
#include "dce_virtual.h"

/*
 * Indirect registers accessor
 */
static u32 cik_pcie_rreg(struct amdgpu_device *adev, u32 reg)
{
	unsigned long flags;
	u32 r;

	spin_lock_irqsave(&adev->pcie_idx_lock, flags);
	WREG32(mmPCIE_INDEX, reg);
	(void)RREG32(mmPCIE_INDEX);
	r = RREG32(mmPCIE_DATA);
	spin_unlock_irqrestore(&adev->pcie_idx_lock, flags);
	return r;
}

static void cik_pcie_wreg(struct amdgpu_device *adev, u32 reg, u32 v)
{
	unsigned long flags;

	spin_lock_irqsave(&adev->pcie_idx_lock, flags);
	WREG32(mmPCIE_INDEX, reg);
	(void)RREG32(mmPCIE_INDEX);
	WREG32(mmPCIE_DATA, v);
	(void)RREG32(mmPCIE_DATA);
	spin_unlock_irqrestore(&adev->pcie_idx_lock, flags);
}

static u32 cik_smc_rreg(struct amdgpu_device *adev, u32 reg)
{
	unsigned long flags;
	u32 r;

	spin_lock_irqsave(&adev->smc_idx_lock, flags);
	WREG32(mmSMC_IND_INDEX_0, (reg));
	r = RREG32(mmSMC_IND_DATA_0);
	spin_unlock_irqrestore(&adev->smc_idx_lock, flags);
	return r;
}

static void cik_smc_wreg(struct amdgpu_device *adev, u32 reg, u32 v)
{
	unsigned long flags;

	spin_lock_irqsave(&adev->smc_idx_lock, flags);
	WREG32(mmSMC_IND_INDEX_0, (reg));
	WREG32(mmSMC_IND_DATA_0, (v));
	spin_unlock_irqrestore(&adev->smc_idx_lock, flags);
}

static u32 cik_uvd_ctx_rreg(struct amdgpu_device *adev, u32 reg)
{
	unsigned long flags;
	u32 r;

	spin_lock_irqsave(&adev->uvd_ctx_idx_lock, flags);
	WREG32(mmUVD_CTX_INDEX, ((reg) & 0x1ff));
	r = RREG32(mmUVD_CTX_DATA);
	spin_unlock_irqrestore(&adev->uvd_ctx_idx_lock, flags);
	return r;
}

static void cik_uvd_ctx_wreg(struct amdgpu_device *adev, u32 reg, u32 v)
{
	unsigned long flags;

	spin_lock_irqsave(&adev->uvd_ctx_idx_lock, flags);
	WREG32(mmUVD_CTX_INDEX, ((reg) & 0x1ff));
	WREG32(mmUVD_CTX_DATA, (v));
	spin_unlock_irqrestore(&adev->uvd_ctx_idx_lock, flags);
}

static u32 cik_didt_rreg(struct amdgpu_device *adev, u32 reg)
{
	unsigned long flags;
	u32 r;

	spin_lock_irqsave(&adev->didt_idx_lock, flags);
	WREG32(mmDIDT_IND_INDEX, (reg));
	r = RREG32(mmDIDT_IND_DATA);
	spin_unlock_irqrestore(&adev->didt_idx_lock, flags);
	return r;
}

static void cik_didt_wreg(struct amdgpu_device *adev, u32 reg, u32 v)
{
	unsigned long flags;

	spin_lock_irqsave(&adev->didt_idx_lock, flags);
	WREG32(mmDIDT_IND_INDEX, (reg));
	WREG32(mmDIDT_IND_DATA, (v));
	spin_unlock_irqrestore(&adev->didt_idx_lock, flags);
}

static const u32 bonaire_golden_spm_registers[] =
{
	0xc200, 0xe0ffffff, 0xe0000000
};

static const u32 bonaire_golden_common_registers[] =
{
	0x31dc, 0xffffffff, 0x00000800,
	0x31dd, 0xffffffff, 0x00000800,
	0x31e6, 0xffffffff, 0x00007fbf,
	0x31e7, 0xffffffff, 0x00007faf
};

static const u32 bonaire_golden_registers[] =
{
	0xcd5, 0x00000333, 0x00000333,
	0xcd4, 0x000c0fc0, 0x00040200,
	0x2684, 0x00010000, 0x00058208,
	0xf000, 0xffff1fff, 0x00140000,
	0xf080, 0xfdfc0fff, 0x00000100,
	0xf08d, 0x40000000, 0x40000200,
	0x260c, 0xffffffff, 0x00000000,
	0x260d, 0xf00fffff, 0x00000400,
	0x260e, 0x0002021c, 0x00020200,
	0x31e, 0x00000080, 0x00000000,
	0x16ec, 0x000000f0, 0x00000070,
	0x16f0, 0xf0311fff, 0x80300000,
	0x263e, 0x73773777, 0x12010001,
	0xd43, 0x00810000, 0x408af000,
	0x1c0c, 0x31000111, 0x00000011,
	0xbd2, 0x73773777, 0x12010001,
	0x883, 0x00007fb6, 0x0021a1b1,
	0x884, 0x00007fb6, 0x002021b1,
	0x860, 0x00007fb6, 0x00002191,
	0x886, 0x00007fb6, 0x002121b1,
	0x887, 0x00007fb6, 0x002021b1,
	0x877, 0x00007fb6, 0x00002191,
	0x878, 0x00007fb6, 0x00002191,
	0xd8a, 0x0000003f, 0x0000000a,
	0xd8b, 0x0000003f, 0x0000000a,
	0xab9, 0x00073ffe, 0x000022a2,
	0x903, 0x000007ff, 0x00000000,
	0x2285, 0xf000003f, 0x00000007,
	0x22fc, 0x00002001, 0x00000001,
	0x22c9, 0xffffffff, 0x00ffffff,
	0xc281, 0x0000ff0f, 0x00000000,
	0xa293, 0x07ffffff, 0x06000000,
	0x136, 0x00000fff, 0x00000100,
	0xf9e, 0x00000001, 0x00000002,
	0x2440, 0x03000000, 0x0362c688,
	0x2300, 0x000000ff, 0x00000001,
	0x390, 0x00001fff, 0x00001fff,
	0x2418, 0x0000007f, 0x00000020,
	0x2542, 0x00010000, 0x00010000,
	0x2b05, 0x000003ff, 0x000000f3,
	0x2b03, 0xffffffff, 0x00001032
};

static const u32 bonaire_mgcg_cgcg_init[] =
{
	0x3108, 0xffffffff, 0xfffffffc,
	0xc200, 0xffffffff, 0xe0000000,
	0xf0a8, 0xffffffff, 0x00000100,
	0xf082, 0xffffffff, 0x00000100,
	0xf0b0, 0xffffffff, 0xc0000100,
	0xf0b2, 0xffffffff, 0xc0000100,
	0xf0b1, 0xffffffff, 0xc0000100,
	0x1579, 0xffffffff, 0x00600100,
	0xf0a0, 0xffffffff, 0x00000100,
	0xf085, 0xffffffff, 0x06000100,
	0xf088, 0xffffffff, 0x00000100,
	0xf086, 0xffffffff, 0x06000100,
	0xf081, 0xffffffff, 0x00000100,
	0xf0b8, 0xffffffff, 0x00000100,
	0xf089, 0xffffffff, 0x00000100,
	0xf080, 0xffffffff, 0x00000100,
	0xf08c, 0xffffffff, 0x00000100,
	0xf08d, 0xffffffff, 0x00000100,
	0xf094, 0xffffffff, 0x00000100,
	0xf095, 0xffffffff, 0x00000100,
	0xf096, 0xffffffff, 0x00000100,
	0xf097, 0xffffffff, 0x00000100,
	0xf098, 0xffffffff, 0x00000100,
	0xf09f, 0xffffffff, 0x00000100,
	0xf09e, 0xffffffff, 0x00000100,
	0xf084, 0xffffffff, 0x06000100,
	0xf0a4, 0xffffffff, 0x00000100,
	0xf09d, 0xffffffff, 0x00000100,
	0xf0ad, 0xffffffff, 0x00000100,
	0xf0ac, 0xffffffff, 0x00000100,
	0xf09c, 0xffffffff, 0x00000100,
	0xc200, 0xffffffff, 0xe0000000,
	0xf008, 0xffffffff, 0x00010000,
	0xf009, 0xffffffff, 0x00030002,
	0xf00a, 0xffffffff, 0x00040007,
	0xf00b, 0xffffffff, 0x00060005,
	0xf00c, 0xffffffff, 0x00090008,
	0xf00d, 0xffffffff, 0x00010000,
	0xf00e, 0xffffffff, 0x00030002,
	0xf00f, 0xffffffff, 0x00040007,
	0xf010, 0xffffffff, 0x00060005,
	0xf011, 0xffffffff, 0x00090008,
	0xf012, 0xffffffff, 0x00010000,
	0xf013, 0xffffffff, 0x00030002,
	0xf014, 0xffffffff, 0x00040007,
	0xf015, 0xffffffff, 0x00060005,
	0xf016, 0xffffffff, 0x00090008,
	0xf017, 0xffffffff, 0x00010000,
	0xf018, 0xffffffff, 0x00030002,
	0xf019, 0xffffffff, 0x00040007,
	0xf01a, 0xffffffff, 0x00060005,
	0xf01b, 0xffffffff, 0x00090008,
	0xf01c, 0xffffffff, 0x00010000,
	0xf01d, 0xffffffff, 0x00030002,
	0xf01e, 0xffffffff, 0x00040007,
	0xf01f, 0xffffffff, 0x00060005,
	0xf020, 0xffffffff, 0x00090008,
	0xf021, 0xffffffff, 0x00010000,
	0xf022, 0xffffffff, 0x00030002,
	0xf023, 0xffffffff, 0x00040007,
	0xf024, 0xffffffff, 0x00060005,
	0xf025, 0xffffffff, 0x00090008,
	0xf026, 0xffffffff, 0x00010000,
	0xf027, 0xffffffff, 0x00030002,
	0xf028, 0xffffffff, 0x00040007,
	0xf029, 0xffffffff, 0x00060005,
	0xf02a, 0xffffffff, 0x00090008,
	0xf000, 0xffffffff, 0x96e00200,
	0x21c2, 0xffffffff, 0x00900100,
	0x3109, 0xffffffff, 0x0020003f,
	0xe, 0xffffffff, 0x0140001c,
	0xf, 0x000f0000, 0x000f0000,
	0x88, 0xffffffff, 0xc060000c,
	0x89, 0xc0000fff, 0x00000100,
	0x3e4, 0xffffffff, 0x00000100,
	0x3e6, 0x00000101, 0x00000000,
	0x82a, 0xffffffff, 0x00000104,
	0x1579, 0xff000fff, 0x00000100,
	0xc33, 0xc0000fff, 0x00000104,
	0x3079, 0x00000001, 0x00000001,
	0x3403, 0xff000ff0, 0x00000100,
	0x3603, 0xff000ff0, 0x00000100
};

static const u32 spectre_golden_spm_registers[] =
{
	0xc200, 0xe0ffffff, 0xe0000000
};

static const u32 spectre_golden_common_registers[] =
{
	0x31dc, 0xffffffff, 0x00000800,
	0x31dd, 0xffffffff, 0x00000800,
	0x31e6, 0xffffffff, 0x00007fbf,
	0x31e7, 0xffffffff, 0x00007faf
};

static const u32 spectre_golden_registers[] =
{
	0xf000, 0xffff1fff, 0x96940200,
	0xf003, 0xffff0001, 0xff000000,
	0xf080, 0xfffc0fff, 0x00000100,
	0x1bb6, 0x00010101, 0x00010000,
	0x260d, 0xf00fffff, 0x00000400,
	0x260e, 0xfffffffc, 0x00020200,
	0x16ec, 0x000000f0, 0x00000070,
	0x16f0, 0xf0311fff, 0x80300000,
	0x263e, 0x73773777, 0x12010001,
	0x26df, 0x00ff0000, 0x00fc0000,
	0xbd2, 0x73773777, 0x12010001,
	0x2285, 0xf000003f, 0x00000007,
	0x22c9, 0xffffffff, 0x00ffffff,
	0xa0d4, 0x3f3f3fff, 0x00000082,
	0xa0d5, 0x0000003f, 0x00000000,
	0xf9e, 0x00000001, 0x00000002,
	0x244f, 0xffff03df, 0x00000004,
	0x31da, 0x00000008, 0x00000008,
	0x2300, 0x000008ff, 0x00000800,
	0x2542, 0x00010000, 0x00010000,
	0x2b03, 0xffffffff, 0x54763210,
	0x853e, 0x01ff01ff, 0x00000002,
	0x8526, 0x007ff800, 0x00200000,
	0x8057, 0xffffffff, 0x00000f40,
	0xc24d, 0xffffffff, 0x00000001
};

static const u32 spectre_mgcg_cgcg_init[] =
{
	0x3108, 0xffffffff, 0xfffffffc,
	0xc200, 0xffffffff, 0xe0000000,
	0xf0a8, 0xffffffff, 0x00000100,
	0xf082, 0xffffffff, 0x00000100,
	0xf0b0, 0xffffffff, 0x00000100,
	0xf0b2, 0xffffffff, 0x00000100,
	0xf0b1, 0xffffffff, 0x00000100,
	0x1579, 0xffffffff, 0x00600100,
	0xf0a0, 0xffffffff, 0x00000100,
	0xf085, 0xffffffff, 0x06000100,
	0xf088, 0xffffffff, 0x00000100,
	0xf086, 0xffffffff, 0x06000100,
	0xf081, 0xffffffff, 0x00000100,
	0xf0b8, 0xffffffff, 0x00000100,
	0xf089, 0xffffffff, 0x00000100,
	0xf080, 0xffffffff, 0x00000100,
	0xf08c, 0xffffffff, 0x00000100,
	0xf08d, 0xffffffff, 0x00000100,
	0xf094, 0xffffffff, 0x00000100,
	0xf095, 0xffffffff, 0x00000100,
	0xf096, 0xffffffff, 0x00000100,
	0xf097, 0xffffffff, 0x00000100,
	0xf098, 0xffffffff, 0x00000100,
	0xf09f, 0xffffffff, 0x00000100,
	0xf09e, 0xffffffff, 0x00000100,
	0xf084, 0xffffffff, 0x06000100,
	0xf0a4, 0xffffffff, 0x00000100,
	0xf09d, 0xffffffff, 0x00000100,
	0xf0ad, 0xffffffff, 0x00000100,
	0xf0ac, 0xffffffff, 0x00000100,
	0xf09c, 0xffffffff, 0x00000100,
	0xc200, 0xffffffff, 0xe0000000,
	0xf008, 0xffffffff, 0x00010000,
	0xf009, 0xffffffff, 0x00030002,
	0xf00a, 0xffffffff, 0x00040007,
	0xf00b, 0xffffffff, 0x00060005,
	0xf00c, 0xffffffff, 0x00090008,
	0xf00d, 0xffffffff, 0x00010000,
	0xf00e, 0xffffffff, 0x00030002,
	0xf00f, 0xffffffff, 0x00040007,
	0xf010, 0xffffffff, 0x00060005,
	0xf011, 0xffffffff, 0x00090008,
	0xf012, 0xffffffff, 0x00010000,
	0xf013, 0xffffffff, 0x00030002,
	0xf014, 0xffffffff, 0x00040007,
	0xf015, 0xffffffff, 0x00060005,
	0xf016, 0xffffffff, 0x00090008,
	0xf017, 0xffffffff, 0x00010000,
	0xf018, 0xffffffff, 0x00030002,
	0xf019, 0xffffffff, 0x00040007,
	0xf01a, 0xffffffff, 0x00060005,
	0xf01b, 0xffffffff, 0x00090008,
	0xf01c, 0xffffffff, 0x00010000,
	0xf01d, 0xffffffff, 0x00030002,
	0xf01e, 0xffffffff, 0x00040007,
	0xf01f, 0xffffffff, 0x00060005,
	0xf020, 0xffffffff, 0x00090008,
	0xf021, 0xffffffff, 0x00010000,
	0xf022, 0xffffffff, 0x00030002,
	0xf023, 0xffffffff, 0x00040007,
	0xf024, 0xffffffff, 0x00060005,
	0xf025, 0xffffffff, 0x00090008,
	0xf026, 0xffffffff, 0x00010000,
	0xf027, 0xffffffff, 0x00030002,
	0xf028, 0xffffffff, 0x00040007,
	0xf029, 0xffffffff, 0x00060005,
	0xf02a, 0xffffffff, 0x00090008,
	0xf02b, 0xffffffff, 0x00010000,
	0xf02c, 0xffffffff, 0x00030002,
	0xf02d, 0xffffffff, 0x00040007,
	0xf02e, 0xffffffff, 0x00060005,
	0xf02f, 0xffffffff, 0x00090008,
	0xf000, 0xffffffff, 0x96e00200,
	0x21c2, 0xffffffff, 0x00900100,
	0x3109, 0xffffffff, 0x0020003f,
	0xe, 0xffffffff, 0x0140001c,
	0xf, 0x000f0000, 0x000f0000,
	0x88, 0xffffffff, 0xc060000c,
	0x89, 0xc0000fff, 0x00000100,
	0x3e4, 0xffffffff, 0x00000100,
	0x3e6, 0x00000101, 0x00000000,
	0x82a, 0xffffffff, 0x00000104,
	0x1579, 0xff000fff, 0x00000100,
	0xc33, 0xc0000fff, 0x00000104,
	0x3079, 0x00000001, 0x00000001,
	0x3403, 0xff000ff0, 0x00000100,
	0x3603, 0xff000ff0, 0x00000100
};

static const u32 kalindi_golden_spm_registers[] =
{
	0xc200, 0xe0ffffff, 0xe0000000
};

static const u32 kalindi_golden_common_registers[] =
{
	0x31dc, 0xffffffff, 0x00000800,
	0x31dd, 0xffffffff, 0x00000800,
	0x31e6, 0xffffffff, 0x00007fbf,
	0x31e7, 0xffffffff, 0x00007faf
};

static const u32 kalindi_golden_registers[] =
{
	0xf000, 0xffffdfff, 0x6e944040,
	0x1579, 0xff607fff, 0xfc000100,
	0xf088, 0xff000fff, 0x00000100,
	0xf089, 0xff000fff, 0x00000100,
	0xf080, 0xfffc0fff, 0x00000100,
	0x1bb6, 0x00010101, 0x00010000,
	0x260c, 0xffffffff, 0x00000000,
	0x260d, 0xf00fffff, 0x00000400,
	0x16ec, 0x000000f0, 0x00000070,
	0x16f0, 0xf0311fff, 0x80300000,
	0x263e, 0x73773777, 0x12010001,
	0x263f, 0xffffffff, 0x00000010,
	0x26df, 0x00ff0000, 0x00fc0000,
	0x200c, 0x00001f0f, 0x0000100a,
	0xbd2, 0x73773777, 0x12010001,
	0x902, 0x000fffff, 0x000c007f,
	0x2285, 0xf000003f, 0x00000007,
	0x22c9, 0x3fff3fff, 0x00ffcfff,
	0xc281, 0x0000ff0f, 0x00000000,
	0xa293, 0x07ffffff, 0x06000000,
	0x136, 0x00000fff, 0x00000100,
	0xf9e, 0x00000001, 0x00000002,
	0x31da, 0x00000008, 0x00000008,
	0x2300, 0x000000ff, 0x00000003,
	0x853e, 0x01ff01ff, 0x00000002,
	0x8526, 0x007ff800, 0x00200000,
	0x8057, 0xffffffff, 0x00000f40,
	0x2231, 0x001f3ae3, 0x00000082,
	0x2235, 0x0000001f, 0x00000010,
	0xc24d, 0xffffffff, 0x00000000
};

static const u32 kalindi_mgcg_cgcg_init[] =
{
	0x3108, 0xffffffff, 0xfffffffc,
	0xc200, 0xffffffff, 0xe0000000,
	0xf0a8, 0xffffffff, 0x00000100,
	0xf082, 0xffffffff, 0x00000100,
	0xf0b0, 0xffffffff, 0x00000100,
	0xf0b2, 0xffffffff, 0x00000100,
	0xf0b1, 0xffffffff, 0x00000100,
	0x1579, 0xffffffff, 0x00600100,
	0xf0a0, 0xffffffff, 0x00000100,
	0xf085, 0xffffffff, 0x06000100,
	0xf088, 0xffffffff, 0x00000100,
	0xf086, 0xffffffff, 0x06000100,
	0xf081, 0xffffffff, 0x00000100,
	0xf0b8, 0xffffffff, 0x00000100,
	0xf089, 0xffffffff, 0x00000100,
	0xf080, 0xffffffff, 0x00000100,
	0xf08c, 0xffffffff, 0x00000100,
	0xf08d, 0xffffffff, 0x00000100,
	0xf094, 0xffffffff, 0x00000100,
	0xf095, 0xffffffff, 0x00000100,
	0xf096, 0xffffffff, 0x00000100,
	0xf097, 0xffffffff, 0x00000100,
	0xf098, 0xffffffff, 0x00000100,
	0xf09f, 0xffffffff, 0x00000100,
	0xf09e, 0xffffffff, 0x00000100,
	0xf084, 0xffffffff, 0x06000100,
	0xf0a4, 0xffffffff, 0x00000100,
	0xf09d, 0xffffffff, 0x00000100,
	0xf0ad, 0xffffffff, 0x00000100,
	0xf0ac, 0xffffffff, 0x00000100,
	0xf09c, 0xffffffff, 0x00000100,
	0xc200, 0xffffffff, 0xe0000000,
	0xf008, 0xffffffff, 0x00010000,
	0xf009, 0xffffffff, 0x00030002,
	0xf00a, 0xffffffff, 0x00040007,
	0xf00b, 0xffffffff, 0x00060005,
	0xf00c, 0xffffffff, 0x00090008,
	0xf00d, 0xffffffff, 0x00010000,
	0xf00e, 0xffffffff, 0x00030002,
	0xf00f, 0xffffffff, 0x00040007,
	0xf010, 0xffffffff, 0x00060005,
	0xf011, 0xffffffff, 0x00090008,
	0xf000, 0xffffffff, 0x96e00200,
	0x21c2, 0xffffffff, 0x00900100,
	0x3109, 0xffffffff, 0x0020003f,
	0xe, 0xffffffff, 0x0140001c,
	0xf, 0x000f0000, 0x000f0000,
	0x88, 0xffffffff, 0xc060000c,
	0x89, 0xc0000fff, 0x00000100,
	0x82a, 0xffffffff, 0x00000104,
	0x1579, 0xff000fff, 0x00000100,
	0xc33, 0xc0000fff, 0x00000104,
	0x3079, 0x00000001, 0x00000001,
	0x3403, 0xff000ff0, 0x00000100,
	0x3603, 0xff000ff0, 0x00000100
};

static const u32 hawaii_golden_spm_registers[] =
{
	0xc200, 0xe0ffffff, 0xe0000000
};

static const u32 hawaii_golden_common_registers[] =
{
	0xc200, 0xffffffff, 0xe0000000,
	0xa0d4, 0xffffffff, 0x3a00161a,
	0xa0d5, 0xffffffff, 0x0000002e,
	0x2684, 0xffffffff, 0x00018208,
	0x263e, 0xffffffff, 0x12011003
};

static const u32 hawaii_golden_registers[] =
{
	0xcd5, 0x00000333, 0x00000333,
	0x2684, 0x00010000, 0x00058208,
	0x260c, 0xffffffff, 0x00000000,
	0x260d, 0xf00fffff, 0x00000400,
	0x260e, 0x0002021c, 0x00020200,
	0x31e, 0x00000080, 0x00000000,
	0x16ec, 0x000000f0, 0x00000070,
	0x16f0, 0xf0311fff, 0x80300000,
	0xd43, 0x00810000, 0x408af000,
	0x1c0c, 0x31000111, 0x00000011,
	0xbd2, 0x73773777, 0x12010001,
	0x848, 0x0000007f, 0x0000001b,
	0x877, 0x00007fb6, 0x00002191,
	0xd8a, 0x0000003f, 0x0000000a,
	0xd8b, 0x0000003f, 0x0000000a,
	0xab9, 0x00073ffe, 0x000022a2,
	0x903, 0x000007ff, 0x00000000,
	0x22fc, 0x00002001, 0x00000001,
	0x22c9, 0xffffffff, 0x00ffffff,
	0xc281, 0x0000ff0f, 0x00000000,
	0xa293, 0x07ffffff, 0x06000000,
	0xf9e, 0x00000001, 0x00000002,
	0x31da, 0x00000008, 0x00000008,
	0x31dc, 0x00000f00, 0x00000800,
	0x31dd, 0x00000f00, 0x00000800,
	0x31e6, 0x00ffffff, 0x00ff7fbf,
	0x31e7, 0x00ffffff, 0x00ff7faf,
	0x2300, 0x000000ff, 0x00000800,
	0x390, 0x00001fff, 0x00001fff,
	0x2418, 0x0000007f, 0x00000020,
	0x2542, 0x00010000, 0x00010000,
	0x2b80, 0x00100000, 0x000ff07c,
	0x2b05, 0x000003ff, 0x0000000f,
	0x2b04, 0xffffffff, 0x7564fdec,
	0x2b03, 0xffffffff, 0x3120b9a8,
	0x2b02, 0x20000000, 0x0f9c0000
};

static const u32 hawaii_mgcg_cgcg_init[] =
{
	0x3108, 0xffffffff, 0xfffffffd,
	0xc200, 0xffffffff, 0xe0000000,
	0xf0a8, 0xffffffff, 0x00000100,
	0xf082, 0xffffffff, 0x00000100,
	0xf0b0, 0xffffffff, 0x00000100,
	0xf0b2, 0xffffffff, 0x00000100,
	0xf0b1, 0xffffffff, 0x00000100,
	0x1579, 0xffffffff, 0x00200100,
	0xf0a0, 0xffffffff, 0x00000100,
	0xf085, 0xffffffff, 0x06000100,
	0xf088, 0xffffffff, 0x00000100,
	0xf086, 0xffffffff, 0x06000100,
	0xf081, 0xffffffff, 0x00000100,
	0xf0b8, 0xffffffff, 0x00000100,
	0xf089, 0xffffffff, 0x00000100,
	0xf080, 0xffffffff, 0x00000100,
	0xf08c, 0xffffffff, 0x00000100,
	0xf08d, 0xffffffff, 0x00000100,
	0xf094, 0xffffffff, 0x00000100,
	0xf095, 0xffffffff, 0x00000100,
	0xf096, 0xffffffff, 0x00000100,
	0xf097, 0xffffffff, 0x00000100,
	0xf098, 0xffffffff, 0x00000100,
	0xf09f, 0xffffffff, 0x00000100,
	0xf09e, 0xffffffff, 0x00000100,
	0xf084, 0xffffffff, 0x06000100,
	0xf0a4, 0xffffffff, 0x00000100,
	0xf09d, 0xffffffff, 0x00000100,
	0xf0ad, 0xffffffff, 0x00000100,
	0xf0ac, 0xffffffff, 0x00000100,
	0xf09c, 0xffffffff, 0x00000100,
	0xc200, 0xffffffff, 0xe0000000,
	0xf008, 0xffffffff, 0x00010000,
	0xf009, 0xffffffff, 0x00030002,
	0xf00a, 0xffffffff, 0x00040007,
	0xf00b, 0xffffffff, 0x00060005,
	0xf00c, 0xffffffff, 0x00090008,
	0xf00d, 0xffffffff, 0x00010000,
	0xf00e, 0xffffffff, 0x00030002,
	0xf00f, 0xffffffff, 0x00040007,
	0xf010, 0xffffffff, 0x00060005,
	0xf011, 0xffffffff, 0x00090008,
	0xf012, 0xffffffff, 0x00010000,
	0xf013, 0xffffffff, 0x00030002,
	0xf014, 0xffffffff, 0x00040007,
	0xf015, 0xffffffff, 0x00060005,
	0xf016, 0xffffffff, 0x00090008,
	0xf017, 0xffffffff, 0x00010000,
	0xf018, 0xffffffff, 0x00030002,
	0xf019, 0xffffffff, 0x00040007,
	0xf01a, 0xffffffff, 0x00060005,
	0xf01b, 0xffffffff, 0x00090008,
	0xf01c, 0xffffffff, 0x00010000,
	0xf01d, 0xffffffff, 0x00030002,
	0xf01e, 0xffffffff, 0x00040007,
	0xf01f, 0xffffffff, 0x00060005,
	0xf020, 0xffffffff, 0x00090008,
	0xf021, 0xffffffff, 0x00010000,
	0xf022, 0xffffffff, 0x00030002,
	0xf023, 0xffffffff, 0x00040007,
	0xf024, 0xffffffff, 0x00060005,
	0xf025, 0xffffffff, 0x00090008,
	0xf026, 0xffffffff, 0x00010000,
	0xf027, 0xffffffff, 0x00030002,
	0xf028, 0xffffffff, 0x00040007,
	0xf029, 0xffffffff, 0x00060005,
	0xf02a, 0xffffffff, 0x00090008,
	0xf02b, 0xffffffff, 0x00010000,
	0xf02c, 0xffffffff, 0x00030002,
	0xf02d, 0xffffffff, 0x00040007,
	0xf02e, 0xffffffff, 0x00060005,
	0xf02f, 0xffffffff, 0x00090008,
	0xf030, 0xffffffff, 0x00010000,
	0xf031, 0xffffffff, 0x00030002,
	0xf032, 0xffffffff, 0x00040007,
	0xf033, 0xffffffff, 0x00060005,
	0xf034, 0xffffffff, 0x00090008,
	0xf035, 0xffffffff, 0x00010000,
	0xf036, 0xffffffff, 0x00030002,
	0xf037, 0xffffffff, 0x00040007,
	0xf038, 0xffffffff, 0x00060005,
	0xf039, 0xffffffff, 0x00090008,
	0xf03a, 0xffffffff, 0x00010000,
	0xf03b, 0xffffffff, 0x00030002,
	0xf03c, 0xffffffff, 0x00040007,
	0xf03d, 0xffffffff, 0x00060005,
	0xf03e, 0xffffffff, 0x00090008,
	0x30c6, 0xffffffff, 0x00020200,
	0xcd4, 0xffffffff, 0x00000200,
	0x570, 0xffffffff, 0x00000400,
	0x157a, 0xffffffff, 0x00000000,
	0xbd4, 0xffffffff, 0x00000902,
	0xf000, 0xffffffff, 0x96940200,
	0x21c2, 0xffffffff, 0x00900100,
	0x3109, 0xffffffff, 0x0020003f,
	0xe, 0xffffffff, 0x0140001c,
	0xf, 0x000f0000, 0x000f0000,
	0x88, 0xffffffff, 0xc060000c,
	0x89, 0xc0000fff, 0x00000100,
	0x3e4, 0xffffffff, 0x00000100,
	0x3e6, 0x00000101, 0x00000000,
	0x82a, 0xffffffff, 0x00000104,
	0x1579, 0xff000fff, 0x00000100,
	0xc33, 0xc0000fff, 0x00000104,
	0x3079, 0x00000001, 0x00000001,
	0x3403, 0xff000ff0, 0x00000100,
	0x3603, 0xff000ff0, 0x00000100
};

static const u32 godavari_golden_registers[] =
{
	0x1579, 0xff607fff, 0xfc000100,
	0x1bb6, 0x00010101, 0x00010000,
	0x260c, 0xffffffff, 0x00000000,
	0x260c0, 0xf00fffff, 0x00000400,
	0x184c, 0xffffffff, 0x00010000,
	0x16ec, 0x000000f0, 0x00000070,
	0x16f0, 0xf0311fff, 0x80300000,
	0x263e, 0x73773777, 0x12010001,
	0x263f, 0xffffffff, 0x00000010,
	0x200c, 0x00001f0f, 0x0000100a,
	0xbd2, 0x73773777, 0x12010001,
	0x902, 0x000fffff, 0x000c007f,
	0x2285, 0xf000003f, 0x00000007,
	0x22c9, 0xffffffff, 0x00ff0fff,
	0xc281, 0x0000ff0f, 0x00000000,
	0xa293, 0x07ffffff, 0x06000000,
	0x136, 0x00000fff, 0x00000100,
	0x3405, 0x00010000, 0x00810001,
	0x3605, 0x00010000, 0x00810001,
	0xf9e, 0x00000001, 0x00000002,
	0x31da, 0x00000008, 0x00000008,
	0x31dc, 0x00000f00, 0x00000800,
	0x31dd, 0x00000f00, 0x00000800,
	0x31e6, 0x00ffffff, 0x00ff7fbf,
	0x31e7, 0x00ffffff, 0x00ff7faf,
	0x2300, 0x000000ff, 0x00000001,
	0x853e, 0x01ff01ff, 0x00000002,
	0x8526, 0x007ff800, 0x00200000,
	0x8057, 0xffffffff, 0x00000f40,
	0x2231, 0x001f3ae3, 0x00000082,
	0x2235, 0x0000001f, 0x00000010,
	0xc24d, 0xffffffff, 0x00000000
};

static const u32 liverpool_golden_common_registers[] =
{
		0x31dc, 0xffffffff, 0x00000800, /* SPI_RESOURCE_RESERVE_CU_0 */
		0x31dd, 0xffffffff, 0x00000800, /* SPI_RESOURCE_RESERVE_CU_1 */
		0x31e6, 0xffffffff, 0x00ffffbf, /* SPI_RESOURCE_RESERVE_EN_CU_0 */
		0x31e7, 0xffffffff, 0x00ffffaf, /* SPI_RESOURCE_RESERVE_EN_CU_1 */
		0x31e8, 0xffffffff, 0x00fffffe, /* SPI_RESOURCE_RESERVE_EN_CU_2 */
		0x31e9, 0xffffffff, 0x00fffffe, /* SPI_RESOURCE_RESERVE_EN_CU_3*/
		0x31ea, 0xffffffff, 0x00fffffe, /* SPI_RESOURCE_RESERVE_EN_CU_4 */
		0x31eb, 0xffffffff, 0x00fffffe, /* SPI_RESOURCE_RESERVE_EN_CU_5 */
		0x31ec, 0xffffffff, 0x00fffffe, /* SPI_RESOURCE_RESERVE_EN_CU_6 */
		0x31ed, 0xffffffff, 0x00fffffe, /* SPI_RESOURCE_RESERVE_EN_CU_7 */
		0x31ee, 0xffffffff, 0x00fffffe, /* SPI_RESOURCE_RESERVE_EN_CU_8 */
		0x31ef, 0xffffffff, 0x00fffffe, /* SPI_RESOURCE_RESERVE_EN_CU_9 */
		0xa0d4, 0xffffffff, 0x2a00161a, /* PA_SC_RASTER_CONFIG */
		0xa0d5, 0xffffffff, 0x00000000, /* PA_SC_RASTER_CONFIG_1 */
		0x1401, 0x00002000, 0x00002000, /* GARLIC_FLUSH_CNTL */
};

static const u32 liverpool_golden_registers[] =
{
	0x3108, 0xffffffff, 0xfffffffc, /* RLC_CGTT_MGCG_OVERRIDE */
	0xc200, 0xffffffff, 0xe0000000, /* GRBM_GFX_INDEX */
	/* These are all setting OFF_HYSTERESIS = 0x10 */
	0xf0a8, 0xffffffff, 0x00000100, /* CB_CGTT_SCLK_CTRL */
	0xf082, 0xffffffff, 0x00000100, /* CGTT_BCI_CLK_CTRL */
	0xf0b0, 0xffffffff, 0x00000100, /* CGTT_CP_CLK_CTRL */
	0xf0b2, 0xffffffff, 0x00000100, /* CGTT_CPC_CLK_CTRL */
	0xf0b1, 0xffffffff, 0x00000100, /* CGTT_CPF_CLK_CTRL */
	0x1579, 0xffffffff, 0x00600100, /* CGTT_DRM_CLK_CTRL0 */
	0xf0a0, 0xffffffff, 0x00000100, /* CGTT_GDS_CLK_CTRL */
	0xf085, 0xffffffff, 0x06000100, /* CGTT_IA_CLK_CTRL */
	0xf088, 0xffffffff, 0x00000100, /* CGTT_PA_CLK_CTRL */
	0xf086, 0xffffffff, 0x06000100, /* CGTT_WD_CLK_CTRL */
	0xf081, 0xffffffff, 0x00000100, /* CGTT_PC_CLK_CTRL */
	0xf0b8, 0xffffffff, 0x00000100, /* CGTT_RLC_CLK_CTRL */
	0xf089, 0xffffffff, 0x00000100, /* CGTT_SC_CLK_CTRL */
	0xf080, 0xffffffff, 0x00000100, /* CGTT_SPI_CLK_CTRL */
	0xf08c, 0xffffffff, 0x00000100, /* CGTT_SQ_CLK_CTRL */
	0xf08d, 0xffffffff, 0x00000100, /* CGTT_SQG_CLK_CTRL */
	0xf094, 0xffffffff, 0x00000100, /* CGTT_SX_CLK_CTRL0 */
	0xf095, 0xffffffff, 0x00000100, /* CGTT_SX_CLK_CTRL1 */
	0xf096, 0xffffffff, 0x00000100, /* CGTT_SX_CLK_CTRL2 */
	0xf097, 0xffffffff, 0x00000100, /* CGTT_SX_CLK_CTRL3 */
	0xf098, 0xffffffff, 0x00000100, /* CGTT_SX_CLK_CTRL4 */
	0xf09f, 0xffffffff, 0x00000100, /* CGTT_TCI_CLK_CTRL */
	0xf09e, 0xffffffff, 0x00000100, /* CGTT_TCP_CLK_CTRL */
	0xf084, 0xffffffff, 0x06000100, /* CGTT_VGT_CLK_CTRL */
	0xf0a4, 0xffffffff, 0x00000100, /* DB_CGTT_CLK_CTRL_0 */
	0xf09d, 0xffffffff, 0x00000100, /* TA_CGTT_CTRL */
	0xf0ad, 0xffffffff, 0x00000100, /* TCA_CGTT_SCLK_CTRL */
	0xf0ac, 0xffffffff, 0x00000100, /* TCC_CGTT_SCLK_CTRL */
	0xf09c, 0xffffffff, 0x00000100, /* TD_CGTT_CTRL */
	/* */
	0xc200, 0xffffffff, 0xe0000000, /* GRBM_GFX_INDEX */
	0xf008, 0xffffffff, 0x00010000, /* CGTS_CU0_SP0_CTRL_REG */
	0xf009, 0xffffffff, 0x00030002, /* CGTS_CU0_LDS_SQ_CTRL_REG */
	0xf00a, 0xffffffff, 0x00040007, /* CGTS_CU0_TA_SQC_CTRL_REG */
	0xf00b, 0xffffffff, 0x00060005, /* CGTS_CU0_SP1_CTRL_REG */
	0xf00c, 0xffffffff, 0x00090008, /* CGTS_CU0_TD_TCP_CTRL_REG */
	0xf00d, 0xffffffff, 0x00010000, /* CGTS_CU1_SP0_CTRL_REG */
	0xf00e, 0xffffffff, 0x00030002, /* CGTS_CU1_LDS_SQ_CTRL_REG */
	0xf00f, 0xffffffff, 0x00040007, /* CGTS_CU1_TA_CTRL_REG */
	0xf010, 0xffffffff, 0x00060005, /* CGTS_CU1_SP1_CTRL_REG */
	0xf011, 0xffffffff, 0x00090008, /* CGTS_CU1_TD_TCP_CTRL_REG */
	0xf012, 0xffffffff, 0x00010000, /* CGTS_CU2_SP0_CTRL_REG */
	0xf013, 0xffffffff, 0x00030002, /* CGTS_CU2_LDS_SQ_CTRL_REG */
	0xf014, 0xffffffff, 0x00040007, /* CGTS_CU2_TA_CTRL_REG */
	0xf015, 0xffffffff, 0x00060005, /* CGTS_CU2_SP1_CTRL_REG */
	0xf016, 0xffffffff, 0x00090008, /* CGTS_CU2_TD_TCP_CTRL_REG */
	0xf017, 0xffffffff, 0x00010000, /* CGTS_CU3_SP0_CTRL_REG */
	0xf018, 0xffffffff, 0x00030002, /* CGTS_CU3_LDS_SQ_CTRL_REG */
	0xf019, 0xffffffff, 0x00040007, /* CGTS_CU3_TA_SQC_CTRL_REG */
	0xf01a, 0xffffffff, 0x00060005, /* CGTS_CU3_SP1_CTRL_REG */
	0xf01b, 0xffffffff, 0x00090008, /* CGTS_CU3_TD_TCP_CTRL_REG */
	0xf01c, 0xffffffff, 0x00010000, /* CGTS_CU4_SP0_CTRL_REG */
	0xf01d, 0xffffffff, 0x00030002, /* CGTS_CU4_LDS_SQ_CTRL_REG */
	0xf01e, 0xffffffff, 0x00040007, /* CGTS_CU4_TA_CTRL_REG */
	0xf01f, 0xffffffff, 0x00060005, /* CGTS_CU4_SP1_CTRL_REG */
	0xf020, 0xffffffff, 0x00090008, /* CGTS_CU4_TD_TCP_CTRL_REG */
	0xf021, 0xffffffff, 0x00010000, /* CGTS_CU5_SP0_CTRL_REG */
	0xf022, 0xffffffff, 0x00030002, /* CGTS_CU5_LDS_SQ_CTRL_REG */
	0xf023, 0xffffffff, 0x00040007, /* CGTS_CU5_TA_CTRL_REG */
	0xf024, 0xffffffff, 0x00060005, /* CGTS_CU5_SP1_CTRL_REG */
	0xf025, 0xffffffff, 0x00090008, /* CGTS_CU5_TD_TCP_CTRL_REG */
	0xf026, 0xffffffff, 0x00010000, /* CGTS_CU6_SP0_CTRL_REG */
	0xf027, 0xffffffff, 0x00030002, /* CGTS_CU6_LDS_SQ_CTRL_REG */
	0xf028, 0xffffffff, 0x00040007, /* CGTS_CU6_TA_SQC_CTRL_REG */
	0xf029, 0xffffffff, 0x00060005, /* CGTS_CU6_SP1_CTRL_REG */
	0xf02a, 0xffffffff, 0x00090008, /* CGTS_CU6_TD_TCP_CTRL_REG */
	0xf02b, 0xffffffff, 0x00010000, /* CGTS_CU7_SP0_CTRL_REG */
	0xf02c, 0xffffffff, 0x00030002, /* CGTS_CU7_LDS_SQ_CTRL_REG */
	0xf02d, 0xffffffff, 0x00040007, /* CGTS_CU7_TA_SQC_CTRL_REG */
	0xf02e, 0xffffffff, 0x00060005, /* CGTS_CU7_SP1_CTRL_REG */
	0xf02f, 0xffffffff, 0x00090008, /* CGTS_CU7_TD_TCP_CTRL_REG */
	0xf030, 0xffffffff, 0x00010000, /* CGTS_CU8_SP0_CTRL_REG */
	0xf031, 0xffffffff, 0x00030002, /* CGTS_CU8_LDS_SQ_CTRL_REG */
	0xf032, 0xffffffff, 0x00040007, /* CGTS_CU8_TA_CTRL_REG */
	0xf033, 0xffffffff, 0x00060005, /* CGTS_CU8_SP1_CTRL_REG */
	0xf034, 0xffffffff, 0x00090008, /* CGTS_CU8_TD_TCP_CTRL_REG */
	0xf035, 0xffffffff, 0x00010000, /* CGTS_CU9_SP0_CTRL_REG */
	0xf036, 0xffffffff, 0x00030002, /* CGTS_CU9_LDS_SQ_CTRL_REG */
	0xf037, 0xffffffff, 0x00040007, /* CGTS_CU9_TA_CTRL_REG */
	0xf038, 0xffffffff, 0x00060005, /* CGTS_CU9_SP1_CTRL_REG */
	0xf039, 0xffffffff, 0x00090008, /* CGTS_CU9_TD_TCP_CTRL_REG */
	0xf000, 0xffffffff, 0x96940200, /* CGTS_SM_CTRL_REG */
	0x21c2, 0xffffffff, 0x00900100, /* CP_RB_WPTR_POLL_CNTL */
	0x3109, 0xffffffff, 0x0020003f, /* RLC_CGCG_CGLS_CTRL */
	0x2684, 0x00210000, 0x00018208, /* CB_HW_CONTROL */
	0xf000, 0xffff1fff, 0x96940200, /* CGTS_SM_CTRL_REG */
	0xf003, 0xffff0001, 0xff000000, /* CGTS_TCC_DISABLE */
	0xf004, 0xffff0000, 0xff000000, /* CGTS_USER_TCC_DISABLE */
	0x1579, 0xff607fff, 0xfc000100, /* CGTT_DRM_CLK_CTRL0 */
	0xf080, 0xfdfc0fff, 0x00000100, /* CGTT_SPI_CLK_CTRL */
	0x1bb6, 0x00010000, 0x00010000, /* CRTC_DOUBLE_BUFFER_CONTROL */
	0x260d, 0xf00fffff, 0x00004400, /* DB_DEBUG2 */
	0x16ec, 0x000000f0, 0x00000070, /* FBC_DEBUG_COMP */
	0x263e, 0x73773777, 0x12011003, /* GB_ADDR_CONFIG */
	0xbd2, 0x73773777, 0x12010001, /* HDP_ADDR_CONFIG */
	0x2285, 0xf000003f, 0x00000007, /* PA_CL_ENHANCE */
	0x22fc, 0x00000001, 0x00000001, /* PA_SC_ENHANCE */
	0x22c9, 0xffffffff, 0x00ffffff, /* PA_SC_FORCE_EOV_MAX_CNTS */
	0xc281, 0x0000ff0f, 0x00000000, /* PA_SC_LINE_STIPPLE_STATE */
	0xa293, 0x07ffffff, 0x06000000, /* PA_SC_MODE_CNTL_1 */
	0x30df, 0xffffffff, 0x00000b00, /* RLC_PG_DELAY_2 */
	0x136, 0x00000fff, 0x00000100, /* SCLK_CGTT_BLK_CTRL_REG */
	0xf9e, 0x00000001, 0x00000002, /* SEM_CHICKEN_BITS */
	0x31da, 0x00000008, 0x00000008, /* SPI_RESET_DEBUG */
	0x2300, 0x000000ff, 0x00000001, /* SQ_CONFIG */
	0x2542, 0x00010000, 0x00010000, /* TA_CNTL_AUX */
	0x2b03, 0xffffffff, 0x76325410, /* TCP_CHAN_STEER_LO */
	0x31dc, 0xffffffff, 0x00000800, /* SPI_RESOURCE_RESERVE_CU_0 */
	0x31dd, 0xffffffff, 0x00000800, /* SPI_RESOURCE_RESERVE_CU_1 */
	0x31e6, 0xffffffff, 0x00ffffbf, /* SPI_RESOURCE_RESERVE_EN_CU_0 */
	0x31e7, 0xffffffff, 0x00ffffaf, /* SPI_RESOURCE_RESERVE_EN_CU_1  */
	0x31e8, 0xffffffff, 0x00fffffe, /* SPI_RESOURCE_RESERVE_EN_CU_2 */
	0x31e9, 0xffffffff, 0x00fffffe, /* SPI_RESOURCE_RESERVE_EN_CU_3 */
	0x31ea, 0xffffffff, 0x00fffffe, /* SPI_RESOURCE_RESERVE_EN_CU_4 */
	0x31eb, 0xffffffff, 0x00fffffe, /* SPI_RESOURCE_RESERVE_EN_CU_5*/
	0x31ec, 0xffffffff, 0x00fffffe, /* SPI_RESOURCE_RESERVE_EN_CU_6 */
	0x31ed, 0xffffffff, 0x00fffffe, /* SPI_RESOURCE_RESERVE_EN_CU_7 */
	0x31ee, 0xffffffff, 0x00fffffe, /* SPI_RESOURCE_RESERVE_EN_CU_8 */
	0x31ef, 0xffffffff, 0x00fffffe, /* SPI_RESOURCE_RESERVE_EN_CU_9 */
	0xa0d4, 0xffffffff, 0x2a00161a, /* PA_SC_RASTER_CONFIG */
	0xa0d5, 0xffffffff, 0x00000000, /* PA_SC_RASTER_CONFIG_1 */
	0x1401, 0x00002000, 0x00002000, /* GARLIC_FLUSH_CNTL */
	0x535, 0xffffffff, 0x00000000, /* VM_CONTEXTS_DISABLE */
};

static const u32 gladius_golden_common_registers[] =
{
	mmATC_MISC_CG, 0x000c0fc0, 0x000c0200,
	mmCB_HW_CONTROL, 0x0001f3cf, 0x00007208,	// 0x2684
	mmCB_HW_CONTROL_2, 0x0f000000, 0x0f000000,
	mmCB_HW_CONTROL_3, 0x000001ff, 0x00000040,
	mmDB_DEBUG2, 0xf00fffff, 0x00000400,
	mmPA_SC_ENHANCE, 0xffffffff, 0x20000001,
	mmPA_SC_LINE_STIPPLE_STATE, 0x0000ff0f, 0x00000000,
	mmPA_SC_RASTER_CONFIG, 0x3f3fffff, 0x2a00161a,
	mmPA_SC_RASTER_CONFIG_1, 0x0000003f, 0x0000002e,
	mmRLC_CGCG_CGLS_CTRL, 0x00000003, 0x0020003c, //0x0001003c,
	0xec9d, 0xffffffff, 0x0001003c,//mmRLC_CGCG_CGLS_CTRL_3D, 0xffffffff, 0x0001003c,
	mmSQ_CONFIG, 0x07f80000, 0x07180000,
	mmTA_CNTL_AUX, 0x000f000f, 0x000b0000,
	mmTCC_CTRL, 0x00100000, 0xf31fff7f,
	mmTCP_ADDR_CONFIG, 0x000003ff, 0x000000f7,
	mmTCP_CHAN_STEER_HI, 0xffffffff, 0x00000000,
	mmVGT_RESET_DEBUG, 0x00000004, 0x00000004,
	mmDCI_CLK_CNTL, 0x00000080, 0x00000000,
	mmFBC_DEBUG_COMP, 0x000000f0, 0x00000070,
	mmFBC_MISC, 0x9f313fff, 0x14302008,
	mmHDMI_CONTROL, 0x313f031f, 0x00000011,
};

static const u32 gladius_golden_registers[] =
{
	mmGRBM_GFX_INDEX, 0xffffffff, 0xe0000000,
	mmPA_SC_RASTER_CONFIG, 0xffffffff, 0x2a00161a,
	mmPA_SC_RASTER_CONFIG_1, 0xffffffff, 0x0000002e,
	mmGB_ADDR_CONFIG, 0xffffffff, 0x22011003, //0x22011003,
	mmSPI_RESOURCE_RESERVE_CU_0, 0xffffffff, 0x00000800,
	mmSPI_RESOURCE_RESERVE_CU_1, 0xffffffff, 0x00000800,
	mmSPI_RESOURCE_RESERVE_EN_CU_0, 0xffffffff, 0x00FF7FBF,
	mmSPI_RESOURCE_RESERVE_EN_CU_1, 0xffffffff, 0x00FF7FAF,
	0x535, 0xffffffff, 0x00000000, /* VM_CONTEXTS_DISABLE */

};

static const u32 gladius_mgcg_cgcg_init[] =
{
	0x0000313a, 0xffffffff, 0x00000003,
	0x00003079, 0xffffffff, 0x00020201,
	0x00003108, 0xffffffff, 0xfffffffd,
	0x0000c200, 0xffffffff, 0xe0000000,
	0x0000311d, 0xffffffff, 0xffffffff,
	0x0000311e, 0xffffffff, 0xffffffff,
	0x0000311f, 0xffffffff, 0x004000ff,
	0x0000313a, 0xffffffff, 0x00000001,
};

static void cik_init_golden_registers(struct amdgpu_device *adev)
{
	/* Some of the registers might be dependent on GRBM_GFX_INDEX */
	mutex_lock(&adev->grbm_idx_mutex);

	switch (adev->asic_type) {
	case CHIP_BONAIRE:
		amdgpu_program_register_sequence(adev,
						 bonaire_mgcg_cgcg_init,
						 (const u32)ARRAY_SIZE(bonaire_mgcg_cgcg_init));
		amdgpu_program_register_sequence(adev,
						 bonaire_golden_registers,
						 (const u32)ARRAY_SIZE(bonaire_golden_registers));
		amdgpu_program_register_sequence(adev,
						 bonaire_golden_common_registers,
						 (const u32)ARRAY_SIZE(bonaire_golden_common_registers));
		amdgpu_program_register_sequence(adev,
						 bonaire_golden_spm_registers,
						 (const u32)ARRAY_SIZE(bonaire_golden_spm_registers));
		break;
	case CHIP_KABINI:
		amdgpu_program_register_sequence(adev,
						 kalindi_mgcg_cgcg_init,
						 (const u32)ARRAY_SIZE(kalindi_mgcg_cgcg_init));
		amdgpu_program_register_sequence(adev,
						 kalindi_golden_registers,
						 (const u32)ARRAY_SIZE(kalindi_golden_registers));
		amdgpu_program_register_sequence(adev,
						 kalindi_golden_common_registers,
						 (const u32)ARRAY_SIZE(kalindi_golden_common_registers));
		amdgpu_program_register_sequence(adev,
						 kalindi_golden_spm_registers,
						 (const u32)ARRAY_SIZE(kalindi_golden_spm_registers));
		break;
	case CHIP_MULLINS:
		amdgpu_program_register_sequence(adev,
						 kalindi_mgcg_cgcg_init,
						 (const u32)ARRAY_SIZE(kalindi_mgcg_cgcg_init));
		amdgpu_program_register_sequence(adev,
						 godavari_golden_registers,
						 (const u32)ARRAY_SIZE(godavari_golden_registers));
		amdgpu_program_register_sequence(adev,
						 kalindi_golden_common_registers,
						 (const u32)ARRAY_SIZE(kalindi_golden_common_registers));
		amdgpu_program_register_sequence(adev,
						 kalindi_golden_spm_registers,
						 (const u32)ARRAY_SIZE(kalindi_golden_spm_registers));
		break;
	case CHIP_KAVERI:
		amdgpu_program_register_sequence(adev,
						 spectre_mgcg_cgcg_init,
						 (const u32)ARRAY_SIZE(spectre_mgcg_cgcg_init));
		amdgpu_program_register_sequence(adev,
						 spectre_golden_registers,
						 (const u32)ARRAY_SIZE(spectre_golden_registers));
		amdgpu_program_register_sequence(adev,
						 spectre_golden_common_registers,
						 (const u32)ARRAY_SIZE(spectre_golden_common_registers));
		amdgpu_program_register_sequence(adev,
						 spectre_golden_spm_registers,
						 (const u32)ARRAY_SIZE(spectre_golden_spm_registers));
		break;
	case CHIP_HAWAII:
		amdgpu_program_register_sequence(adev,
						 hawaii_mgcg_cgcg_init,
						 (const u32)ARRAY_SIZE(hawaii_mgcg_cgcg_init));
		amdgpu_program_register_sequence(adev,
						 hawaii_golden_registers,
						 (const u32)ARRAY_SIZE(hawaii_golden_registers));
		amdgpu_program_register_sequence(adev,
						 hawaii_golden_common_registers,
						 (const u32)ARRAY_SIZE(hawaii_golden_common_registers));
		amdgpu_program_register_sequence(adev,
						 hawaii_golden_spm_registers,
						 (const u32)ARRAY_SIZE(hawaii_golden_spm_registers));
		break;
	case CHIP_LIVERPOOL:
		/*amdgpu_program_register_sequence(adev,
						 liverpool_mgcg_cgcg_init,
						 (const u32)ARRAY_SIZE(liverpool_mgcg_cgcg_init));*/
		amdgpu_program_register_sequence(adev,
						 liverpool_golden_registers,
						 (const u32)ARRAY_SIZE(liverpool_golden_registers));
		amdgpu_program_register_sequence(adev,
						 liverpool_golden_common_registers,
						 (const u32)ARRAY_SIZE(liverpool_golden_common_registers));
		break;
	case CHIP_GLADIUS:
		amdgpu_program_register_sequence(adev,
						 gladius_mgcg_cgcg_init,
						 (const u32)ARRAY_SIZE(gladius_mgcg_cgcg_init));
		amdgpu_program_register_sequence(adev,
						 gladius_golden_registers,
						 (const u32)ARRAY_SIZE(gladius_golden_registers));
		amdgpu_program_register_sequence(adev,
						 gladius_golden_common_registers,
						 (const u32)ARRAY_SIZE(gladius_golden_common_registers));
		amdgpu_program_register_sequence(adev,
						 hawaii_golden_spm_registers,
						 (const u32)ARRAY_SIZE(hawaii_golden_spm_registers));
		break;
	default:
		break;
	}
	mutex_unlock(&adev->grbm_idx_mutex);
}

/**
 * cik_get_xclk - get the xclk
 *
 * @adev: amdgpu_device pointer
 *
 * Returns the reference clock used by the gfx engine
 * (CIK).
 */
static u32 cik_get_xclk(struct amdgpu_device *adev)
{
	u32 reference_clock = adev->clock.spll.reference_freq;

	if (adev->flags & AMD_IS_APU) {
		if (RREG32_SMC(ixGENERAL_PWRMGT) & GENERAL_PWRMGT__GPU_COUNTER_CLK_MASK)
			return reference_clock / 2;
	} else {
		if (RREG32_SMC(ixCG_CLKPIN_CNTL) & CG_CLKPIN_CNTL__XTALIN_DIVIDE_MASK)
			return reference_clock / 4;
	}
	return reference_clock;
}

/**
 * cik_srbm_select - select specific register instances
 *
 * @adev: amdgpu_device pointer
 * @me: selected ME (micro engine)
 * @pipe: pipe
 * @queue: queue
 * @vmid: VMID
 *
 * Switches the currently active registers instances.  Some
 * registers are instanced per VMID, others are instanced per
 * me/pipe/queue combination.
 */
void cik_srbm_select(struct amdgpu_device *adev,
		     u32 me, u32 pipe, u32 queue, u32 vmid)
{
	u32 srbm_gfx_cntl =
		(((pipe << SRBM_GFX_CNTL__PIPEID__SHIFT) & SRBM_GFX_CNTL__PIPEID_MASK)|
		((me << SRBM_GFX_CNTL__MEID__SHIFT) & SRBM_GFX_CNTL__MEID_MASK)|
		((vmid << SRBM_GFX_CNTL__VMID__SHIFT) & SRBM_GFX_CNTL__VMID_MASK)|
		((queue << SRBM_GFX_CNTL__QUEUEID__SHIFT) & SRBM_GFX_CNTL__QUEUEID_MASK));
	WREG32(mmSRBM_GFX_CNTL, srbm_gfx_cntl);
}

static void cik_vga_set_state(struct amdgpu_device *adev, bool state)
{
	uint32_t tmp;

	tmp = RREG32(mmCONFIG_CNTL);
	if (!state)
		tmp |= CONFIG_CNTL__VGA_DIS_MASK;
	else
		tmp &= ~CONFIG_CNTL__VGA_DIS_MASK;
	WREG32(mmCONFIG_CNTL, tmp);
}

static bool cik_read_disabled_bios(struct amdgpu_device *adev)
{
	u32 bus_cntl;
	u32 d1vga_control = 0;
	u32 d2vga_control = 0;
	u32 vga_render_control = 0;
	u32 rom_cntl;
	bool r;

	bus_cntl = RREG32(mmBUS_CNTL);
	if (adev->mode_info.num_crtc) {
		d1vga_control = RREG32(mmD1VGA_CONTROL);
		d2vga_control = RREG32(mmD2VGA_CONTROL);
		vga_render_control = RREG32(mmVGA_RENDER_CONTROL);
	}
	rom_cntl = RREG32_SMC(ixROM_CNTL);

	/* enable the rom */
	WREG32(mmBUS_CNTL, (bus_cntl & ~BUS_CNTL__BIOS_ROM_DIS_MASK));
	if (adev->mode_info.num_crtc) {
		/* Disable VGA mode */
		WREG32(mmD1VGA_CONTROL,
		       (d1vga_control & ~(D1VGA_CONTROL__D1VGA_MODE_ENABLE_MASK |
					  D1VGA_CONTROL__D1VGA_TIMING_SELECT_MASK)));
		WREG32(mmD2VGA_CONTROL,
		       (d2vga_control & ~(D1VGA_CONTROL__D1VGA_MODE_ENABLE_MASK |
					  D1VGA_CONTROL__D1VGA_TIMING_SELECT_MASK)));
		WREG32(mmVGA_RENDER_CONTROL,
		       (vga_render_control & ~VGA_RENDER_CONTROL__VGA_VSTATUS_CNTL_MASK));
	}
	WREG32_SMC(ixROM_CNTL, rom_cntl | ROM_CNTL__SCK_OVERWRITE_MASK);

	r = amdgpu_read_bios(adev);

	/* restore regs */
	WREG32(mmBUS_CNTL, bus_cntl);
	if (adev->mode_info.num_crtc) {
		WREG32(mmD1VGA_CONTROL, d1vga_control);
		WREG32(mmD2VGA_CONTROL, d2vga_control);
		WREG32(mmVGA_RENDER_CONTROL, vga_render_control);
	}
	WREG32_SMC(ixROM_CNTL, rom_cntl);
	return r;
}

static bool cik_read_bios_from_rom(struct amdgpu_device *adev,
				   u8 *bios, u32 length_bytes)
{
	u32 *dw_ptr;
	unsigned long flags;
	u32 i, length_dw;

	if (bios == NULL)
		return false;
	if (length_bytes == 0)
		return false;
	/* APU vbios image is part of sbios image */
	if (adev->flags & AMD_IS_APU)
		return false;

	dw_ptr = (u32 *)bios;
	length_dw = ALIGN(length_bytes, 4) / 4;
	/* take the smc lock since we are using the smc index */
	spin_lock_irqsave(&adev->smc_idx_lock, flags);
	/* set rom index to 0 */
	WREG32(mmSMC_IND_INDEX_0, ixROM_INDEX);
	WREG32(mmSMC_IND_DATA_0, 0);
	/* set index to data for continous read */
	WREG32(mmSMC_IND_INDEX_0, ixROM_DATA);
	for (i = 0; i < length_dw; i++)
		dw_ptr[i] = RREG32(mmSMC_IND_DATA_0);
	spin_unlock_irqrestore(&adev->smc_idx_lock, flags);

	return true;
}

static const struct amdgpu_allowed_register_entry cik_allowed_read_registers[] = {
	{mmGRBM_STATUS},
	{mmGB_ADDR_CONFIG},
	{mmMC_ARB_RAMCFG},
	{mmGB_TILE_MODE0},
	{mmGB_TILE_MODE1},
	{mmGB_TILE_MODE2},
	{mmGB_TILE_MODE3},
	{mmGB_TILE_MODE4},
	{mmGB_TILE_MODE5},
	{mmGB_TILE_MODE6},
	{mmGB_TILE_MODE7},
	{mmGB_TILE_MODE8},
	{mmGB_TILE_MODE9},
	{mmGB_TILE_MODE10},
	{mmGB_TILE_MODE11},
	{mmGB_TILE_MODE12},
	{mmGB_TILE_MODE13},
	{mmGB_TILE_MODE14},
	{mmGB_TILE_MODE15},
	{mmGB_TILE_MODE16},
	{mmGB_TILE_MODE17},
	{mmGB_TILE_MODE18},
	{mmGB_TILE_MODE19},
	{mmGB_TILE_MODE20},
	{mmGB_TILE_MODE21},
	{mmGB_TILE_MODE22},
	{mmGB_TILE_MODE23},
	{mmGB_TILE_MODE24},
	{mmGB_TILE_MODE25},
	{mmGB_TILE_MODE26},
	{mmGB_TILE_MODE27},
	{mmGB_TILE_MODE28},
	{mmGB_TILE_MODE29},
	{mmGB_TILE_MODE30},
	{mmGB_TILE_MODE31},
	{mmGB_MACROTILE_MODE0},
	{mmGB_MACROTILE_MODE1},
	{mmGB_MACROTILE_MODE2},
	{mmGB_MACROTILE_MODE3},
	{mmGB_MACROTILE_MODE4},
	{mmGB_MACROTILE_MODE5},
	{mmGB_MACROTILE_MODE6},
	{mmGB_MACROTILE_MODE7},
	{mmGB_MACROTILE_MODE8},
	{mmGB_MACROTILE_MODE9},
	{mmGB_MACROTILE_MODE10},
	{mmGB_MACROTILE_MODE11},
	{mmGB_MACROTILE_MODE12},
	{mmGB_MACROTILE_MODE13},
	{mmGB_MACROTILE_MODE14},
	{mmGB_MACROTILE_MODE15},
	{mmCC_RB_BACKEND_DISABLE, true},
	{mmGC_USER_RB_BACKEND_DISABLE, true},
	{mmGB_BACKEND_MAP, false},
	{mmPA_SC_RASTER_CONFIG, true},
	{mmPA_SC_RASTER_CONFIG_1, true},
};

static uint32_t cik_read_indexed_register(struct amdgpu_device *adev,
					  u32 se_num, u32 sh_num,
					  u32 reg_offset)
{
	uint32_t val;

	mutex_lock(&adev->grbm_idx_mutex);
	if (se_num != 0xffffffff || sh_num != 0xffffffff)
		amdgpu_gfx_select_se_sh(adev, se_num, sh_num, 0xffffffff);

	val = RREG32(reg_offset);

	if (se_num != 0xffffffff || sh_num != 0xffffffff)
		amdgpu_gfx_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff);
	mutex_unlock(&adev->grbm_idx_mutex);
	return val;
}

static int cik_read_register(struct amdgpu_device *adev, u32 se_num,
			     u32 sh_num, u32 reg_offset, u32 *value)
{
	uint32_t i;

	*value = 0;
	for (i = 0; i < ARRAY_SIZE(cik_allowed_read_registers); i++) {
		if (reg_offset != cik_allowed_read_registers[i].reg_offset)
			continue;

		*value = cik_allowed_read_registers[i].grbm_indexed ?
			 cik_read_indexed_register(adev, se_num,
						   sh_num, reg_offset) :
			 RREG32(reg_offset);
		return 0;
	}
	return -EINVAL;
}

struct kv_reset_save_regs {
	u32 gmcon_reng_execute;
	u32 gmcon_misc;
	u32 gmcon_misc3;
};

static void kv_save_regs_for_reset(struct amdgpu_device *adev,
				   struct kv_reset_save_regs *save)
{
	save->gmcon_reng_execute = RREG32(mmGMCON_RENG_EXECUTE);
	save->gmcon_misc = RREG32(mmGMCON_MISC);
	save->gmcon_misc3 = RREG32(mmGMCON_MISC3);

	WREG32(mmGMCON_RENG_EXECUTE, save->gmcon_reng_execute &
		~GMCON_RENG_EXECUTE__RENG_EXECUTE_ON_PWR_UP_MASK);
	WREG32(mmGMCON_MISC, save->gmcon_misc &
		~(GMCON_MISC__RENG_EXECUTE_ON_REG_UPDATE_MASK |
			GMCON_MISC__STCTRL_STUTTER_EN_MASK));
}

static void kv_restore_regs_for_reset(struct amdgpu_device *adev,
				      struct kv_reset_save_regs *save)
{
	int i;

	WREG32(mmGMCON_PGFSM_WRITE, 0);
	WREG32(mmGMCON_PGFSM_CONFIG, 0x200010ff);

	for (i = 0; i < 5; i++)
		WREG32(mmGMCON_PGFSM_WRITE, 0);

	WREG32(mmGMCON_PGFSM_WRITE, 0);
	WREG32(mmGMCON_PGFSM_CONFIG, 0x300010ff);

	for (i = 0; i < 5; i++)
		WREG32(mmGMCON_PGFSM_WRITE, 0);

	WREG32(mmGMCON_PGFSM_WRITE, 0x210000);
	WREG32(mmGMCON_PGFSM_CONFIG, 0xa00010ff);

	for (i = 0; i < 5; i++)
		WREG32(mmGMCON_PGFSM_WRITE, 0);

	WREG32(mmGMCON_PGFSM_WRITE, 0x21003);
	WREG32(mmGMCON_PGFSM_CONFIG, 0xb00010ff);

	for (i = 0; i < 5; i++)
		WREG32(mmGMCON_PGFSM_WRITE, 0);

	WREG32(mmGMCON_PGFSM_WRITE, 0x2b00);
	WREG32(mmGMCON_PGFSM_CONFIG, 0xc00010ff);

	for (i = 0; i < 5; i++)
		WREG32(mmGMCON_PGFSM_WRITE, 0);

	WREG32(mmGMCON_PGFSM_WRITE, 0);
	WREG32(mmGMCON_PGFSM_CONFIG, 0xd00010ff);

	for (i = 0; i < 5; i++)
		WREG32(mmGMCON_PGFSM_WRITE, 0);

	WREG32(mmGMCON_PGFSM_WRITE, 0x420000);
	WREG32(mmGMCON_PGFSM_CONFIG, 0x100010ff);

	for (i = 0; i < 5; i++)
		WREG32(mmGMCON_PGFSM_WRITE, 0);

	WREG32(mmGMCON_PGFSM_WRITE, 0x120202);
	WREG32(mmGMCON_PGFSM_CONFIG, 0x500010ff);

	for (i = 0; i < 5; i++)
		WREG32(mmGMCON_PGFSM_WRITE, 0);

	WREG32(mmGMCON_PGFSM_WRITE, 0x3e3e36);
	WREG32(mmGMCON_PGFSM_CONFIG, 0x600010ff);

	for (i = 0; i < 5; i++)
		WREG32(mmGMCON_PGFSM_WRITE, 0);

	WREG32(mmGMCON_PGFSM_WRITE, 0x373f3e);
	WREG32(mmGMCON_PGFSM_CONFIG, 0x700010ff);

	for (i = 0; i < 5; i++)
		WREG32(mmGMCON_PGFSM_WRITE, 0);

	WREG32(mmGMCON_PGFSM_WRITE, 0x3e1332);
	WREG32(mmGMCON_PGFSM_CONFIG, 0xe00010ff);

	WREG32(mmGMCON_MISC3, save->gmcon_misc3);
	WREG32(mmGMCON_MISC, save->gmcon_misc);
	WREG32(mmGMCON_RENG_EXECUTE, save->gmcon_reng_execute);
}

static int cik_gpu_pci_config_reset(struct amdgpu_device *adev)
{
	struct kv_reset_save_regs kv_save = { 0 };
	u32 i;
	int r = -EINVAL;

	dev_info(adev->dev, "GPU pci config reset\n");

	if (adev->flags & AMD_IS_APU)
		kv_save_regs_for_reset(adev, &kv_save);

	/* disable BM */
	pci_clear_master(adev->pdev);
	/* reset */
	amdgpu_pci_config_reset(adev);

	udelay(100);

	/* wait for asic to come out of reset */
	for (i = 0; i < adev->usec_timeout; i++) {
		if (RREG32(mmCONFIG_MEMSIZE) != 0xffffffff) {
			/* enable BM */
			pci_set_master(adev->pdev);
			adev->has_hw_reset = true;
			r = 0;
			break;
		}
		udelay(1);
	}

	/* does asic init need to be run first??? */
	if (adev->flags & AMD_IS_APU)
		kv_restore_regs_for_reset(adev, &kv_save);

	return r;
}

/**
 * cik_asic_reset - soft reset GPU
 *
 * @adev: amdgpu_device pointer
 *
 * Look up which blocks are hung and attempt
 * to reset them.
 * Returns 0 for success.
 */
static int cik_asic_reset(struct amdgpu_device *adev)
{
	int r;

	amdgpu_atombios_scratch_regs_engine_hung(adev, true);

	r = cik_gpu_pci_config_reset(adev);

	amdgpu_atombios_scratch_regs_engine_hung(adev, false);

	return r;
}

static u32 cik_get_config_memsize(struct amdgpu_device *adev)
{
	return RREG32(mmCONFIG_MEMSIZE);
}

static int cik_set_uvd_clock(struct amdgpu_device *adev, u32 clock,
			      u32 cntl_reg, u32 status_reg)
{
	int r, i;
	struct atom_clock_dividers dividers;
	uint32_t tmp;

	r = amdgpu_atombios_get_clock_dividers(adev,
					       COMPUTE_GPUCLK_INPUT_FLAG_DEFAULT_GPUCLK,
					       clock, false, &dividers);
	if (r)
		return r;

	tmp = RREG32_SMC(cntl_reg);
	tmp &= ~(CG_DCLK_CNTL__DCLK_DIR_CNTL_EN_MASK |
		CG_DCLK_CNTL__DCLK_DIVIDER_MASK);
	tmp |= dividers.post_divider;
	WREG32_SMC(cntl_reg, tmp);

	for (i = 0; i < 100; i++) {
		if (RREG32_SMC(status_reg) & CG_DCLK_STATUS__DCLK_STATUS_MASK)
			break;
		mdelay(10);
	}
	if (i == 100)
		return -ETIMEDOUT;

	return 0;
}

static int cik_set_uvd_clocks(struct amdgpu_device *adev, u32 vclk, u32 dclk)
{
	int r = 0;

	r = cik_set_uvd_clock(adev, vclk, ixCG_VCLK_CNTL, ixCG_VCLK_STATUS);
	if (r)
		return r;

	r = cik_set_uvd_clock(adev, dclk, ixCG_DCLK_CNTL, ixCG_DCLK_STATUS);
	return r;
}

static int cik_set_vce_clocks(struct amdgpu_device *adev, u32 evclk, u32 ecclk)
{
	int r, i;
	struct atom_clock_dividers dividers;
	u32 tmp;

	r = amdgpu_atombios_get_clock_dividers(adev,
					       COMPUTE_GPUCLK_INPUT_FLAG_DEFAULT_GPUCLK,
					       ecclk, false, &dividers);
	if (r)
		return r;

	for (i = 0; i < 100; i++) {
		if (RREG32_SMC(ixCG_ECLK_STATUS) & CG_ECLK_STATUS__ECLK_STATUS_MASK)
			break;
		mdelay(10);
	}
	if (i == 100)
		return -ETIMEDOUT;

	tmp = RREG32_SMC(ixCG_ECLK_CNTL);
	tmp &= ~(CG_ECLK_CNTL__ECLK_DIR_CNTL_EN_MASK |
		CG_ECLK_CNTL__ECLK_DIVIDER_MASK);
	tmp |= dividers.post_divider;
	WREG32_SMC(ixCG_ECLK_CNTL, tmp);

	for (i = 0; i < 100; i++) {
		if (RREG32_SMC(ixCG_ECLK_STATUS) & CG_ECLK_STATUS__ECLK_STATUS_MASK)
			break;
		mdelay(10);
	}
	if (i == 100)
		return -ETIMEDOUT;

	return 0;
}

static void cik_pcie_gen3_enable(struct amdgpu_device *adev)
{
	struct pci_dev *root = adev->pdev->bus->self;
	int bridge_pos, gpu_pos;
	u32 speed_cntl, current_data_rate;
	int i;
	u16 tmp16;

	if (pci_is_root_bus(adev->pdev->bus))
		return;

	if (amdgpu_pcie_gen2 == 0)
		return;

	if (adev->flags & AMD_IS_APU)
		return;

	if (!(adev->pm.pcie_gen_mask & (CAIL_PCIE_LINK_SPEED_SUPPORT_GEN2 |
					CAIL_PCIE_LINK_SPEED_SUPPORT_GEN3)))
		return;

	speed_cntl = RREG32_PCIE(ixPCIE_LC_SPEED_CNTL);
	current_data_rate = (speed_cntl & PCIE_LC_SPEED_CNTL__LC_CURRENT_DATA_RATE_MASK) >>
		PCIE_LC_SPEED_CNTL__LC_CURRENT_DATA_RATE__SHIFT;
	if (adev->pm.pcie_gen_mask & CAIL_PCIE_LINK_SPEED_SUPPORT_GEN3) {
		if (current_data_rate == 2) {
			DRM_INFO("PCIE gen 3 link speeds already enabled\n");
			return;
		}
		DRM_INFO("enabling PCIE gen 3 link speeds, disable with amdgpu.pcie_gen2=0\n");
	} else if (adev->pm.pcie_gen_mask & CAIL_PCIE_LINK_SPEED_SUPPORT_GEN2) {
		if (current_data_rate == 1) {
			DRM_INFO("PCIE gen 2 link speeds already enabled\n");
			return;
		}
		DRM_INFO("enabling PCIE gen 2 link speeds, disable with amdgpu.pcie_gen2=0\n");
	}

	bridge_pos = pci_pcie_cap(root);
	if (!bridge_pos)
		return;

	gpu_pos = pci_pcie_cap(adev->pdev);
	if (!gpu_pos)
		return;

	if (adev->pm.pcie_gen_mask & CAIL_PCIE_LINK_SPEED_SUPPORT_GEN3) {
		/* re-try equalization if gen3 is not already enabled */
		if (current_data_rate != 2) {
			u16 bridge_cfg, gpu_cfg;
			u16 bridge_cfg2, gpu_cfg2;
			u32 max_lw, current_lw, tmp;

			pci_read_config_word(root, bridge_pos + PCI_EXP_LNKCTL, &bridge_cfg);
			pci_read_config_word(adev->pdev, gpu_pos + PCI_EXP_LNKCTL, &gpu_cfg);

			tmp16 = bridge_cfg | PCI_EXP_LNKCTL_HAWD;
			pci_write_config_word(root, bridge_pos + PCI_EXP_LNKCTL, tmp16);

			tmp16 = gpu_cfg | PCI_EXP_LNKCTL_HAWD;
			pci_write_config_word(adev->pdev, gpu_pos + PCI_EXP_LNKCTL, tmp16);

			tmp = RREG32_PCIE(ixPCIE_LC_STATUS1);
			max_lw = (tmp & PCIE_LC_STATUS1__LC_DETECTED_LINK_WIDTH_MASK) >>
				PCIE_LC_STATUS1__LC_DETECTED_LINK_WIDTH__SHIFT;
			current_lw = (tmp & PCIE_LC_STATUS1__LC_OPERATING_LINK_WIDTH_MASK)
				>> PCIE_LC_STATUS1__LC_OPERATING_LINK_WIDTH__SHIFT;

			if (current_lw < max_lw) {
				tmp = RREG32_PCIE(ixPCIE_LC_LINK_WIDTH_CNTL);
				if (tmp & PCIE_LC_LINK_WIDTH_CNTL__LC_RENEGOTIATION_SUPPORT_MASK) {
					tmp &= ~(PCIE_LC_LINK_WIDTH_CNTL__LC_LINK_WIDTH_MASK |
						PCIE_LC_LINK_WIDTH_CNTL__LC_UPCONFIGURE_DIS_MASK);
					tmp |= (max_lw <<
						PCIE_LC_LINK_WIDTH_CNTL__LC_LINK_WIDTH__SHIFT);
					tmp |= PCIE_LC_LINK_WIDTH_CNTL__LC_UPCONFIGURE_SUPPORT_MASK |
					PCIE_LC_LINK_WIDTH_CNTL__LC_RENEGOTIATE_EN_MASK |
					PCIE_LC_LINK_WIDTH_CNTL__LC_RECONFIG_NOW_MASK;
					WREG32_PCIE(ixPCIE_LC_LINK_WIDTH_CNTL, tmp);
				}
			}

			for (i = 0; i < 10; i++) {
				/* check status */
				pci_read_config_word(adev->pdev, gpu_pos + PCI_EXP_DEVSTA, &tmp16);
				if (tmp16 & PCI_EXP_DEVSTA_TRPND)
					break;

				pci_read_config_word(root, bridge_pos + PCI_EXP_LNKCTL, &bridge_cfg);
				pci_read_config_word(adev->pdev, gpu_pos + PCI_EXP_LNKCTL, &gpu_cfg);

				pci_read_config_word(root, bridge_pos + PCI_EXP_LNKCTL2, &bridge_cfg2);
				pci_read_config_word(adev->pdev, gpu_pos + PCI_EXP_LNKCTL2, &gpu_cfg2);

				tmp = RREG32_PCIE(ixPCIE_LC_CNTL4);
				tmp |= PCIE_LC_CNTL4__LC_SET_QUIESCE_MASK;
				WREG32_PCIE(ixPCIE_LC_CNTL4, tmp);

				tmp = RREG32_PCIE(ixPCIE_LC_CNTL4);
				tmp |= PCIE_LC_CNTL4__LC_REDO_EQ_MASK;
				WREG32_PCIE(ixPCIE_LC_CNTL4, tmp);

				mdelay(100);

				/* linkctl */
				pci_read_config_word(root, bridge_pos + PCI_EXP_LNKCTL, &tmp16);
				tmp16 &= ~PCI_EXP_LNKCTL_HAWD;
				tmp16 |= (bridge_cfg & PCI_EXP_LNKCTL_HAWD);
				pci_write_config_word(root, bridge_pos + PCI_EXP_LNKCTL, tmp16);

				pci_read_config_word(adev->pdev, gpu_pos + PCI_EXP_LNKCTL, &tmp16);
				tmp16 &= ~PCI_EXP_LNKCTL_HAWD;
				tmp16 |= (gpu_cfg & PCI_EXP_LNKCTL_HAWD);
				pci_write_config_word(adev->pdev, gpu_pos + PCI_EXP_LNKCTL, tmp16);

				/* linkctl2 */
				pci_read_config_word(root, bridge_pos + PCI_EXP_LNKCTL2, &tmp16);
				tmp16 &= ~((1 << 4) | (7 << 9));
				tmp16 |= (bridge_cfg2 & ((1 << 4) | (7 << 9)));
				pci_write_config_word(root, bridge_pos + PCI_EXP_LNKCTL2, tmp16);

				pci_read_config_word(adev->pdev, gpu_pos + PCI_EXP_LNKCTL2, &tmp16);
				tmp16 &= ~((1 << 4) | (7 << 9));
				tmp16 |= (gpu_cfg2 & ((1 << 4) | (7 << 9)));
				pci_write_config_word(adev->pdev, gpu_pos + PCI_EXP_LNKCTL2, tmp16);

				tmp = RREG32_PCIE(ixPCIE_LC_CNTL4);
				tmp &= ~PCIE_LC_CNTL4__LC_SET_QUIESCE_MASK;
				WREG32_PCIE(ixPCIE_LC_CNTL4, tmp);
			}
		}
	}

	/* set the link speed */
	speed_cntl |= PCIE_LC_SPEED_CNTL__LC_FORCE_EN_SW_SPEED_CHANGE_MASK |
		PCIE_LC_SPEED_CNTL__LC_FORCE_DIS_HW_SPEED_CHANGE_MASK;
	speed_cntl &= ~PCIE_LC_SPEED_CNTL__LC_FORCE_DIS_SW_SPEED_CHANGE_MASK;
	WREG32_PCIE(ixPCIE_LC_SPEED_CNTL, speed_cntl);

	pci_read_config_word(adev->pdev, gpu_pos + PCI_EXP_LNKCTL2, &tmp16);
	tmp16 &= ~0xf;
	if (adev->pm.pcie_gen_mask & CAIL_PCIE_LINK_SPEED_SUPPORT_GEN3)
		tmp16 |= 3; /* gen3 */
	else if (adev->pm.pcie_gen_mask & CAIL_PCIE_LINK_SPEED_SUPPORT_GEN2)
		tmp16 |= 2; /* gen2 */
	else
		tmp16 |= 1; /* gen1 */
	pci_write_config_word(adev->pdev, gpu_pos + PCI_EXP_LNKCTL2, tmp16);

	speed_cntl = RREG32_PCIE(ixPCIE_LC_SPEED_CNTL);
	speed_cntl |= PCIE_LC_SPEED_CNTL__LC_INITIATE_LINK_SPEED_CHANGE_MASK;
	WREG32_PCIE(ixPCIE_LC_SPEED_CNTL, speed_cntl);

	for (i = 0; i < adev->usec_timeout; i++) {
		speed_cntl = RREG32_PCIE(ixPCIE_LC_SPEED_CNTL);
		if ((speed_cntl & PCIE_LC_SPEED_CNTL__LC_INITIATE_LINK_SPEED_CHANGE_MASK) == 0)
			break;
		udelay(1);
	}
}

static void cik_program_aspm(struct amdgpu_device *adev)
{
	u32 data, orig;
	bool disable_l0s = false, disable_l1 = false, disable_plloff_in_l1 = false;
	bool disable_clkreq = false;

	if (amdgpu_aspm == 0)
		return;

	if (pci_is_root_bus(adev->pdev->bus))
		return;

	/* XXX double check APUs */
	if (adev->flags & AMD_IS_APU)
		return;

	orig = data = RREG32_PCIE(ixPCIE_LC_N_FTS_CNTL);
	data &= ~PCIE_LC_N_FTS_CNTL__LC_XMIT_N_FTS_MASK;
	data |= (0x24 << PCIE_LC_N_FTS_CNTL__LC_XMIT_N_FTS__SHIFT) |
		PCIE_LC_N_FTS_CNTL__LC_XMIT_N_FTS_OVERRIDE_EN_MASK;
	if (orig != data)
		WREG32_PCIE(ixPCIE_LC_N_FTS_CNTL, data);

	orig = data = RREG32_PCIE(ixPCIE_LC_CNTL3);
	data |= PCIE_LC_CNTL3__LC_GO_TO_RECOVERY_MASK;
	if (orig != data)
		WREG32_PCIE(ixPCIE_LC_CNTL3, data);

	orig = data = RREG32_PCIE(ixPCIE_P_CNTL);
	data |= PCIE_P_CNTL__P_IGNORE_EDB_ERR_MASK;
	if (orig != data)
		WREG32_PCIE(ixPCIE_P_CNTL, data);

	orig = data = RREG32_PCIE(ixPCIE_LC_CNTL);
	data &= ~(PCIE_LC_CNTL__LC_L0S_INACTIVITY_MASK |
		PCIE_LC_CNTL__LC_L1_INACTIVITY_MASK);
	data |= PCIE_LC_CNTL__LC_PMI_TO_L1_DIS_MASK;
	if (!disable_l0s)
		data |= (7 << PCIE_LC_CNTL__LC_L0S_INACTIVITY__SHIFT);

	if (!disable_l1) {
		data |= (7 << PCIE_LC_CNTL__LC_L1_INACTIVITY__SHIFT);
		data &= ~PCIE_LC_CNTL__LC_PMI_TO_L1_DIS_MASK;
		if (orig != data)
			WREG32_PCIE(ixPCIE_LC_CNTL, data);

		if (!disable_plloff_in_l1) {
			bool clk_req_support;

			orig = data = RREG32_PCIE(ixPB0_PIF_PWRDOWN_0);
			data &= ~(PB0_PIF_PWRDOWN_0__PLL_POWER_STATE_IN_OFF_0_MASK |
				PB0_PIF_PWRDOWN_0__PLL_POWER_STATE_IN_TXS2_0_MASK);
			data |= (7 << PB0_PIF_PWRDOWN_0__PLL_POWER_STATE_IN_OFF_0__SHIFT) |
				(7 << PB0_PIF_PWRDOWN_0__PLL_POWER_STATE_IN_TXS2_0__SHIFT);
			if (orig != data)
				WREG32_PCIE(ixPB0_PIF_PWRDOWN_0, data);

			orig = data = RREG32_PCIE(ixPB0_PIF_PWRDOWN_1);
			data &= ~(PB0_PIF_PWRDOWN_1__PLL_POWER_STATE_IN_OFF_1_MASK |
				PB0_PIF_PWRDOWN_1__PLL_POWER_STATE_IN_TXS2_1_MASK);
			data |= (7 << PB0_PIF_PWRDOWN_1__PLL_POWER_STATE_IN_OFF_1__SHIFT) |
				(7 << PB0_PIF_PWRDOWN_1__PLL_POWER_STATE_IN_TXS2_1__SHIFT);
			if (orig != data)
				WREG32_PCIE(ixPB0_PIF_PWRDOWN_1, data);

			orig = data = RREG32_PCIE(ixPB1_PIF_PWRDOWN_0);
			data &= ~(PB1_PIF_PWRDOWN_0__PLL_POWER_STATE_IN_OFF_0_MASK |
				PB1_PIF_PWRDOWN_0__PLL_POWER_STATE_IN_TXS2_0_MASK);
			data |= (7 << PB1_PIF_PWRDOWN_0__PLL_POWER_STATE_IN_OFF_0__SHIFT) |
				(7 << PB1_PIF_PWRDOWN_0__PLL_POWER_STATE_IN_TXS2_0__SHIFT);
			if (orig != data)
				WREG32_PCIE(ixPB1_PIF_PWRDOWN_0, data);

			orig = data = RREG32_PCIE(ixPB1_PIF_PWRDOWN_1);
			data &= ~(PB1_PIF_PWRDOWN_1__PLL_POWER_STATE_IN_OFF_1_MASK |
				PB1_PIF_PWRDOWN_1__PLL_POWER_STATE_IN_TXS2_1_MASK);
			data |= (7 << PB1_PIF_PWRDOWN_1__PLL_POWER_STATE_IN_OFF_1__SHIFT) |
				(7 << PB1_PIF_PWRDOWN_1__PLL_POWER_STATE_IN_TXS2_1__SHIFT);
			if (orig != data)
				WREG32_PCIE(ixPB1_PIF_PWRDOWN_1, data);

			orig = data = RREG32_PCIE(ixPCIE_LC_LINK_WIDTH_CNTL);
			data &= ~PCIE_LC_LINK_WIDTH_CNTL__LC_DYN_LANES_PWR_STATE_MASK;
			data |= ~(3 << PCIE_LC_LINK_WIDTH_CNTL__LC_DYN_LANES_PWR_STATE__SHIFT);
			if (orig != data)
				WREG32_PCIE(ixPCIE_LC_LINK_WIDTH_CNTL, data);

			if (!disable_clkreq) {
				struct pci_dev *root = adev->pdev->bus->self;
				u32 lnkcap;

				clk_req_support = false;
				pcie_capability_read_dword(root, PCI_EXP_LNKCAP, &lnkcap);
				if (lnkcap & PCI_EXP_LNKCAP_CLKPM)
					clk_req_support = true;
			} else {
				clk_req_support = false;
			}

			if (clk_req_support) {
				orig = data = RREG32_PCIE(ixPCIE_LC_CNTL2);
				data |= PCIE_LC_CNTL2__LC_ALLOW_PDWN_IN_L1_MASK |
					PCIE_LC_CNTL2__LC_ALLOW_PDWN_IN_L23_MASK;
				if (orig != data)
					WREG32_PCIE(ixPCIE_LC_CNTL2, data);

				orig = data = RREG32_SMC(ixTHM_CLK_CNTL);
				data &= ~(THM_CLK_CNTL__CMON_CLK_SEL_MASK |
					THM_CLK_CNTL__TMON_CLK_SEL_MASK);
				data |= (1 << THM_CLK_CNTL__CMON_CLK_SEL__SHIFT) |
					(1 << THM_CLK_CNTL__TMON_CLK_SEL__SHIFT);
				if (orig != data)
					WREG32_SMC(ixTHM_CLK_CNTL, data);

				orig = data = RREG32_SMC(ixMISC_CLK_CTRL);
				data &= ~(MISC_CLK_CTRL__DEEP_SLEEP_CLK_SEL_MASK |
					MISC_CLK_CTRL__ZCLK_SEL_MASK);
				data |= (1 << MISC_CLK_CTRL__DEEP_SLEEP_CLK_SEL__SHIFT) |
					(1 << MISC_CLK_CTRL__ZCLK_SEL__SHIFT);
				if (orig != data)
					WREG32_SMC(ixMISC_CLK_CTRL, data);

				orig = data = RREG32_SMC(ixCG_CLKPIN_CNTL);
				data &= ~CG_CLKPIN_CNTL__BCLK_AS_XCLK_MASK;
				if (orig != data)
					WREG32_SMC(ixCG_CLKPIN_CNTL, data);

				orig = data = RREG32_SMC(ixCG_CLKPIN_CNTL_2);
				data &= ~CG_CLKPIN_CNTL_2__FORCE_BIF_REFCLK_EN_MASK;
				if (orig != data)
					WREG32_SMC(ixCG_CLKPIN_CNTL_2, data);

				orig = data = RREG32_SMC(ixMPLL_BYPASSCLK_SEL);
				data &= ~MPLL_BYPASSCLK_SEL__MPLL_CLKOUT_SEL_MASK;
				data |= (4 << MPLL_BYPASSCLK_SEL__MPLL_CLKOUT_SEL__SHIFT);
				if (orig != data)
					WREG32_SMC(ixMPLL_BYPASSCLK_SEL, data);
			}
		}
	} else {
		if (orig != data)
			WREG32_PCIE(ixPCIE_LC_CNTL, data);
	}

	orig = data = RREG32_PCIE(ixPCIE_CNTL2);
	data |= PCIE_CNTL2__SLV_MEM_LS_EN_MASK |
		PCIE_CNTL2__MST_MEM_LS_EN_MASK |
		PCIE_CNTL2__REPLAY_MEM_LS_EN_MASK;
	if (orig != data)
		WREG32_PCIE(ixPCIE_CNTL2, data);

	if (!disable_l0s) {
		data = RREG32_PCIE(ixPCIE_LC_N_FTS_CNTL);
		if ((data & PCIE_LC_N_FTS_CNTL__LC_N_FTS_MASK) ==
				PCIE_LC_N_FTS_CNTL__LC_N_FTS_MASK) {
			data = RREG32_PCIE(ixPCIE_LC_STATUS1);
			if ((data & PCIE_LC_STATUS1__LC_REVERSE_XMIT_MASK) &&
			(data & PCIE_LC_STATUS1__LC_REVERSE_RCVR_MASK)) {
				orig = data = RREG32_PCIE(ixPCIE_LC_CNTL);
				data &= ~PCIE_LC_CNTL__LC_L0S_INACTIVITY_MASK;
				if (orig != data)
					WREG32_PCIE(ixPCIE_LC_CNTL, data);
			}
		}
	}
}

static uint32_t cik_get_rev_id(struct amdgpu_device *adev)
{
	return (RREG32(mmCC_DRM_ID_STRAPS) & CC_DRM_ID_STRAPS__ATI_REV_ID_MASK)
		>> CC_DRM_ID_STRAPS__ATI_REV_ID__SHIFT;
}

static void cik_detect_hw_virtualization(struct amdgpu_device *adev)
{
	if (is_virtual_machine()) /* passthrough mode */
		adev->virt.caps |= AMDGPU_PASSTHROUGH_MODE;
}

static const struct amdgpu_asic_funcs cik_asic_funcs =
{
	.read_disabled_bios = &cik_read_disabled_bios,
	.read_bios_from_rom = &cik_read_bios_from_rom,
	.read_register = &cik_read_register,
	.reset = &cik_asic_reset,
	.set_vga_state = &cik_vga_set_state,
	.get_xclk = &cik_get_xclk,
	.set_uvd_clocks = &cik_set_uvd_clocks,
	.set_vce_clocks = &cik_set_vce_clocks,
	.get_config_memsize = &cik_get_config_memsize,
};

static int cik_common_early_init(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;

	adev->smc_rreg = &cik_smc_rreg;
	adev->smc_wreg = &cik_smc_wreg;
	adev->pcie_rreg = &cik_pcie_rreg;
	adev->pcie_wreg = &cik_pcie_wreg;
	adev->uvd_ctx_rreg = &cik_uvd_ctx_rreg;
	adev->uvd_ctx_wreg = &cik_uvd_ctx_wreg;
	adev->didt_rreg = &cik_didt_rreg;
	adev->didt_wreg = &cik_didt_wreg;

	adev->asic_funcs = &cik_asic_funcs;

	adev->rev_id = cik_get_rev_id(adev);
	adev->external_rev_id = 0xFF;
	switch (adev->asic_type) {
	case CHIP_BONAIRE:
		adev->cg_flags =
			AMD_CG_SUPPORT_GFX_MGCG |
			AMD_CG_SUPPORT_GFX_MGLS |
			/*AMD_CG_SUPPORT_GFX_CGCG |*/
			AMD_CG_SUPPORT_GFX_CGLS |
			AMD_CG_SUPPORT_GFX_CGTS |
			AMD_CG_SUPPORT_GFX_CGTS_LS |
			AMD_CG_SUPPORT_GFX_CP_LS |
			AMD_CG_SUPPORT_MC_LS |
			AMD_CG_SUPPORT_MC_MGCG |
			AMD_CG_SUPPORT_SDMA_MGCG |
			AMD_CG_SUPPORT_SDMA_LS |
			AMD_CG_SUPPORT_BIF_LS |
			AMD_CG_SUPPORT_VCE_MGCG |
			AMD_CG_SUPPORT_UVD_MGCG |
			AMD_CG_SUPPORT_HDP_LS |
			AMD_CG_SUPPORT_HDP_MGCG;
		adev->pg_flags = 0;
		adev->external_rev_id = adev->rev_id + 0x14;
		break;
	case CHIP_HAWAII:
		adev->cg_flags =
			AMD_CG_SUPPORT_GFX_MGCG |
			AMD_CG_SUPPORT_GFX_MGLS |
			/*AMD_CG_SUPPORT_GFX_CGCG |*/
			AMD_CG_SUPPORT_GFX_CGLS |
			AMD_CG_SUPPORT_GFX_CGTS |
			AMD_CG_SUPPORT_GFX_CP_LS |
			AMD_CG_SUPPORT_MC_LS |
			AMD_CG_SUPPORT_MC_MGCG |
			AMD_CG_SUPPORT_SDMA_MGCG |
			AMD_CG_SUPPORT_SDMA_LS |
			AMD_CG_SUPPORT_BIF_LS |
			AMD_CG_SUPPORT_VCE_MGCG |
			AMD_CG_SUPPORT_UVD_MGCG |
			AMD_CG_SUPPORT_HDP_LS |
			AMD_CG_SUPPORT_HDP_MGCG;
		adev->pg_flags = 0;
		adev->external_rev_id = 0x28;
		break;
	case CHIP_KAVERI:
		adev->cg_flags =
			AMD_CG_SUPPORT_GFX_MGCG |
			AMD_CG_SUPPORT_GFX_MGLS |
			/*AMD_CG_SUPPORT_GFX_CGCG |*/
			AMD_CG_SUPPORT_GFX_CGLS |
			AMD_CG_SUPPORT_GFX_CGTS |
			AMD_CG_SUPPORT_GFX_CGTS_LS |
			AMD_CG_SUPPORT_GFX_CP_LS |
			AMD_CG_SUPPORT_SDMA_MGCG |
			AMD_CG_SUPPORT_SDMA_LS |
			AMD_CG_SUPPORT_BIF_LS |
			AMD_CG_SUPPORT_VCE_MGCG |
			AMD_CG_SUPPORT_UVD_MGCG |
			AMD_CG_SUPPORT_HDP_LS |
			AMD_CG_SUPPORT_HDP_MGCG;
		adev->pg_flags =
			/*AMD_PG_SUPPORT_GFX_PG |
			  AMD_PG_SUPPORT_GFX_SMG |
			  AMD_PG_SUPPORT_GFX_DMG |*/
			AMD_PG_SUPPORT_UVD |
			AMD_PG_SUPPORT_VCE |
			/*  AMD_PG_SUPPORT_CP |
			  AMD_PG_SUPPORT_GDS |
			  AMD_PG_SUPPORT_RLC_SMU_HS |
			  AMD_PG_SUPPORT_ACP |
			  AMD_PG_SUPPORT_SAMU |*/
			0;
		if (adev->pdev->device == 0x1312 ||
			adev->pdev->device == 0x1316 ||
			adev->pdev->device == 0x1317)
			adev->external_rev_id = 0x41;
		else
			adev->external_rev_id = 0x1;
		break;
	case CHIP_KABINI:
	case CHIP_MULLINS:
		adev->cg_flags =
			AMD_CG_SUPPORT_GFX_MGCG |
			AMD_CG_SUPPORT_GFX_MGLS |
			/*AMD_CG_SUPPORT_GFX_CGCG |*/
			AMD_CG_SUPPORT_GFX_CGLS |
			AMD_CG_SUPPORT_GFX_CGTS |
			AMD_CG_SUPPORT_GFX_CGTS_LS |
			AMD_CG_SUPPORT_GFX_CP_LS |
			AMD_CG_SUPPORT_SDMA_MGCG |
			AMD_CG_SUPPORT_SDMA_LS |
			AMD_CG_SUPPORT_BIF_LS |
			AMD_CG_SUPPORT_VCE_MGCG |
			AMD_CG_SUPPORT_UVD_MGCG |
			AMD_CG_SUPPORT_HDP_LS |
			AMD_CG_SUPPORT_HDP_MGCG;
		adev->pg_flags =
			/*AMD_PG_SUPPORT_GFX_PG |
			  AMD_PG_SUPPORT_GFX_SMG | */
			AMD_PG_SUPPORT_UVD |
			/*AMD_PG_SUPPORT_VCE |
			  AMD_PG_SUPPORT_CP |
			  AMD_PG_SUPPORT_GDS |
			  AMD_PG_SUPPORT_RLC_SMU_HS |
			  AMD_PG_SUPPORT_SAMU |*/
			0;
		if (adev->asic_type == CHIP_KABINI) {
			if (adev->rev_id == 0)
				adev->external_rev_id = 0x81;
			else if (adev->rev_id == 1)
				adev->external_rev_id = 0x82;
			else if (adev->rev_id == 2)
				adev->external_rev_id = 0x85;
		} else
			adev->external_rev_id = adev->rev_id + 0xa1;
		break;
		case CHIP_LIVERPOOL:
			adev->cg_flags =
				AMD_CG_SUPPORT_GFX_MGCG |
				AMD_CG_SUPPORT_GFX_MGLS |
				/*AMD_CG_SUPPORT_GFX_CGCG |*/
				AMD_CG_SUPPORT_GFX_CGLS |
				AMD_CG_SUPPORT_GFX_CGTS |
				AMD_CG_SUPPORT_GFX_CGTS_LS |
				AMD_CG_SUPPORT_GFX_CP_LS |
				AMD_CG_SUPPORT_SDMA_MGCG |
				AMD_CG_SUPPORT_SDMA_LS |
				AMD_CG_SUPPORT_BIF_LS |
				AMD_CG_SUPPORT_VCE_MGCG |
				AMD_CG_SUPPORT_UVD_MGCG |
				AMD_CG_SUPPORT_HDP_LS |
				AMD_CG_SUPPORT_HDP_MGCG;
			adev->pg_flags =
				/*AMD_PG_SUPPORT_GFX_PG |
				  AMD_PG_SUPPORT_GFX_SMG | */
				/*AMD_PG_SUPPORT_UVD | */
				/*AMD_PG_SUPPORT_VCE |
				  AMD_PG_SUPPORT_CP |
				  AMD_PG_SUPPORT_GDS |
				  AMD_PG_SUPPORT_RLC_SMU_HS |
				  AMD_PG_SUPPORT_SAMU |*/
				0;
			adev->external_rev_id = adev->rev_id + 0x61;
			break;

		case CHIP_GLADIUS:
			adev->cg_flags =
				AMD_CG_SUPPORT_GFX_MGCG |
				AMD_CG_SUPPORT_GFX_MGLS |
				AMD_CG_SUPPORT_GFX_CGCG |
				AMD_CG_SUPPORT_GFX_CGLS |
				AMD_CG_SUPPORT_GFX_CGTS |
				AMD_CG_SUPPORT_GFX_CGTS_LS |
				AMD_CG_SUPPORT_GFX_CP_LS |
				AMD_CG_SUPPORT_SDMA_MGCG |
				AMD_CG_SUPPORT_SDMA_LS |
				AMD_CG_SUPPORT_BIF_LS |
				AMD_CG_SUPPORT_VCE_MGCG |
				AMD_CG_SUPPORT_UVD_MGCG |
				AMD_CG_SUPPORT_HDP_LS |
				AMD_CG_SUPPORT_HDP_MGCG;
			adev->pg_flags =
				/*AMD_PG_SUPPORT_GFX_PG |
					AMD_PG_SUPPORT_GFX_SMG | */
				/*AMD_PG_SUPPORT_UVD | */
				/*AMD_PG_SUPPORT_VCE |
					AMD_PG_SUPPORT_CP |
					AMD_PG_SUPPORT_GDS |
					AMD_PG_SUPPORT_RLC_SMU_HS |
					AMD_PG_SUPPORT_SAMU |*/
				0;
			adev->external_rev_id = adev->rev_id + 0x61;
			break;
	default:
		/* FIXME: not supported yet */
		return -EINVAL;
	}

	adev->firmware.load_type = amdgpu_ucode_get_load_type(adev, amdgpu_fw_load_type);

	amdgpu_get_pcie_info(adev);

	return 0;
}

static int cik_common_sw_init(void *handle)
{
	return 0;
}

static int cik_common_sw_fini(void *handle)
{
	return 0;
}

static int cik_common_hw_init(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;

	/* move the golden regs per IP block */
	cik_init_golden_registers(adev);
	/* enable pcie gen2/3 link */
	cik_pcie_gen3_enable(adev);
	/* enable aspm */
	cik_program_aspm(adev);

	return 0;
}

static int cik_common_hw_fini(void *handle)
{
	return 0;
}

static int cik_common_suspend(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;

	return cik_common_hw_fini(adev);
}

static int cik_common_resume(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;

	return cik_common_hw_init(adev);
}

static bool cik_common_is_idle(void *handle)
{
	return true;
}

static int cik_common_wait_for_idle(void *handle)
{
	return 0;
}

static int cik_common_soft_reset(void *handle)
{
	/* XXX hard reset?? */
	return 0;
}

static int cik_common_set_clockgating_state(void *handle,
					    enum amd_clockgating_state state)
{
	return 0;
}

static int cik_common_set_powergating_state(void *handle,
					    enum amd_powergating_state state)
{
	return 0;
}

static const struct amd_ip_funcs cik_common_ip_funcs = {
	.name = "cik_common",
	.early_init = cik_common_early_init,
	.late_init = NULL,
	.sw_init = cik_common_sw_init,
	.sw_fini = cik_common_sw_fini,
	.hw_init = cik_common_hw_init,
	.hw_fini = cik_common_hw_fini,
	.suspend = cik_common_suspend,
	.resume = cik_common_resume,
	.is_idle = cik_common_is_idle,
	.wait_for_idle = cik_common_wait_for_idle,
	.soft_reset = cik_common_soft_reset,
	.set_clockgating_state = cik_common_set_clockgating_state,
	.set_powergating_state = cik_common_set_powergating_state,
};

static const struct amdgpu_ip_block_version cik_common_ip_block =
{
	.type = AMD_IP_BLOCK_TYPE_COMMON,
	.major = 1,
	.minor = 0,
	.rev = 0,
	.funcs = &cik_common_ip_funcs,
};

int cik_set_ip_blocks(struct amdgpu_device *adev)
{
	cik_detect_hw_virtualization(adev);

	switch (adev->asic_type) {
	case CHIP_BONAIRE:
		amdgpu_ip_block_add(adev, &cik_common_ip_block);
		amdgpu_ip_block_add(adev, &gmc_v7_0_ip_block);
		amdgpu_ip_block_add(adev, &cik_ih_ip_block);
		amdgpu_ip_block_add(adev, &amdgpu_pp_ip_block);
		if (adev->enable_virtual_display)
			amdgpu_ip_block_add(adev, &dce_virtual_ip_block);
		else
			amdgpu_ip_block_add(adev, &dce_v8_2_ip_block);
		amdgpu_ip_block_add(adev, &gfx_v7_2_ip_block);
		amdgpu_ip_block_add(adev, &cik_sdma_ip_block);
		amdgpu_ip_block_add(adev, &uvd_v4_2_ip_block);
		amdgpu_ip_block_add(adev, &vce_v2_0_ip_block);
		break;
	case CHIP_HAWAII:
		amdgpu_ip_block_add(adev, &cik_common_ip_block);
		amdgpu_ip_block_add(adev, &gmc_v7_0_ip_block);
		amdgpu_ip_block_add(adev, &cik_ih_ip_block);
		amdgpu_ip_block_add(adev, &amdgpu_pp_ip_block);
		if (adev->enable_virtual_display)
			amdgpu_ip_block_add(adev, &dce_virtual_ip_block);
		else
			amdgpu_ip_block_add(adev, &dce_v8_5_ip_block);
		amdgpu_ip_block_add(adev, &gfx_v7_3_ip_block);
		amdgpu_ip_block_add(adev, &cik_sdma_ip_block);
		amdgpu_ip_block_add(adev, &uvd_v4_2_ip_block);
		amdgpu_ip_block_add(adev, &vce_v2_0_ip_block);
		break;
	case CHIP_KAVERI:
		amdgpu_ip_block_add(adev, &cik_common_ip_block);
		amdgpu_ip_block_add(adev, &gmc_v7_0_ip_block);
		amdgpu_ip_block_add(adev, &cik_ih_ip_block);
		amdgpu_ip_block_add(adev, &amdgpu_pp_ip_block);
		if (adev->enable_virtual_display)
			amdgpu_ip_block_add(adev, &dce_virtual_ip_block);
		else
			amdgpu_ip_block_add(adev, &dce_v8_1_ip_block);
		amdgpu_ip_block_add(adev, &gfx_v7_1_ip_block);
		amdgpu_ip_block_add(adev, &cik_sdma_ip_block);
		amdgpu_ip_block_add(adev, &uvd_v4_2_ip_block);
		amdgpu_ip_block_add(adev, &vce_v2_0_ip_block);
		break;
	case CHIP_KABINI:
	case CHIP_MULLINS:
		amdgpu_ip_block_add(adev, &cik_common_ip_block);
		amdgpu_ip_block_add(adev, &gmc_v7_0_ip_block);
		amdgpu_ip_block_add(adev, &cik_ih_ip_block);
		amdgpu_ip_block_add(adev, &amdgpu_pp_ip_block);
		if (adev->enable_virtual_display)
			amdgpu_ip_block_add(adev, &dce_virtual_ip_block);
		else
			amdgpu_ip_block_add(adev, &dce_v8_3_ip_block);
		amdgpu_ip_block_add(adev, &gfx_v7_2_ip_block);
		amdgpu_ip_block_add(adev, &cik_sdma_ip_block);
		amdgpu_ip_block_add(adev, &uvd_v4_2_ip_block);
		amdgpu_ip_block_add(adev, &vce_v2_0_ip_block);
		break;
	case CHIP_LIVERPOOL:
		amdgpu_ip_block_add(adev, &cik_common_ip_block);
		amdgpu_ip_block_add(adev, &gmc_v7_0_ip_block);
		amdgpu_ip_block_add(adev, &cik_ih_ip_block);
		//amdgpu_ip_block_add(adev, &pp_smu_ip_block);
		if (adev->enable_virtual_display)
			amdgpu_ip_block_add(adev, &dce_virtual_ip_block);
#if defined(CONFIG_DRM_AMD_DC)
		else if (amdgpu_device_has_dc_support(adev))
			amdgpu_ip_block_add(adev, &dm_ip_block);
#endif
		else
			amdgpu_ip_block_add(adev, &dce_v8_1_ip_block);
		amdgpu_ip_block_add(adev, &gfx_v7_1_ip_block);
		amdgpu_ip_block_add(adev, &cik_sdma_ip_block);
		/*amdgpu_ip_block_add(adev, &uvd_v4_2_ip_block)*/;
		/*amdgpu_ip_block_add(adev, &vce_v2_0_ip_block)*/;
		break;
	case CHIP_GLADIUS:
		amdgpu_ip_block_add(adev, &cik_common_ip_block);
		amdgpu_ip_block_add(adev, &gmc_v7_0_ip_block);
		amdgpu_ip_block_add(adev, &cik_ih_ip_block);
		//amdgpu_ip_block_add(adev, &pp_smu_ip_block);
		if (adev->enable_virtual_display)
			amdgpu_ip_block_add(adev, &dce_virtual_ip_block);
#if defined(CONFIG_DRM_AMD_DC)
		else if (amdgpu_device_has_dc_support(adev))
			amdgpu_ip_block_add(adev, &dm_ip_block);
#endif
		else
			amdgpu_ip_block_add(adev, &dce_v8_1_ip_block);
		amdgpu_ip_block_add(adev, &gfx_v7_1_ip_block);
		amdgpu_ip_block_add(adev, &cik_sdma_ip_block);
		/*amdgpu_ip_block_add(adev, &uvd_v4_2_ip_block)*/;
		/*amdgpu_ip_block_add(adev, &vce_v2_0_ip_block)*/;
		break;
	default:
		/* FIXME: not supported yet */
		return -EINVAL;
	}
	return 0;
}
