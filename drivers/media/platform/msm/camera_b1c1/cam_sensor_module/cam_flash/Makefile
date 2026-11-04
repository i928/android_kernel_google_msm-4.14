ccflags-y += -Idrivers/media/platform/msm/camera_b1c1/cam_utils
ccflags-y += -Idrivers/media/platform/msm/camera_b1c1/cam_sync
ccflags-y += -Idrivers/media/platform/msm/camera_b1c1/cam_sensor_module/cam_res_mgr
ccflags-y += -Idrivers/media/platform/msm/camera_b1c1/cam_sensor_module/cam_sensor_utils
ccflags-y += -Idrivers/media/platform/msm/camera_b1c1/cam_sensor_module/cam_sensor_io
ccflags-y += -Idrivers/media/platform/msm/camera_b1c1/cam_sensor_module/cam_cci
ccflags-y += -Idrivers/media/platform/msm/camera_b1c1/cam_req_mgr
ccflags-y += -Idrivers/media/platform/msm/camera_b1c1/cam_smmu/

obj-$(CONFIG_SPECTRA_CAMERA) += cam_flash_dev.o cam_flash_core.o cam_flash_soc.o

# TODO: remove me b/62058353
subdir-ccflags-y += \
	-Wno-enum-conversion \
