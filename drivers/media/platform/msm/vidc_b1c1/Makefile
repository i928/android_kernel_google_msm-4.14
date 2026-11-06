# Pixel 3: the msm-4.9 vidc, matched to the crosshatch Codec2/OMX blobs.
# Its UAPI (VIDC control ids, buffer size macros) predates the sunfish one
# in include/; force-include the 4.9 copies so they win the header guards.
B1C1_VIDC := $(srctree)/drivers/media/platform/msm/vidc_b1c1
ccflags-y += -I$(B1C1_VIDC)/
subdir-ccflags-y += -include $(B1C1_VIDC)/b1c1_uapi/linux/v4l2-controls.h \
	-include $(B1C1_VIDC)/b1c1_uapi/media/msm_media_info.h \
	-include $(B1C1_VIDC)/b1c1_uapi/media/msm_vidc_kernel.h

msm-vidc-objs := msm_v4l2_vidc.o \
				msm_vidc_platform.o \
                                msm_vidc_common.o \
                                msm_vidc.o \
                                msm_vdec.o \
                                msm_venc.o \
                                msm_smem.o \
                                msm_vidc_debug.o \
                                msm_vidc_res_parse.o \
                                venus_hfi.o \
                                hfi_response_handler.o \
                                hfi_packetization.o \
                                vidc_hfi.o \
                                venus_boot.o \
                                msm_vidc_clocks.o

obj-$(CONFIG_MSM_VIDC_V4L2) := msm-vidc.o

obj-$(CONFIG_MSM_VIDC_V4L2) += governors/

obj-$(CONFIG_MSM_VIDC_VMEM) += vmem/

# TODO: remove me b/62058353
subdir-ccflags-y += \
  -Wno-pointer-bool-conversion \
