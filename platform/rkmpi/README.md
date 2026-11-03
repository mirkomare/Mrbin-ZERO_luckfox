# Rockchip MPI (Luckfox SDK only)

Enable with CMake `-DLUCKFOX_RKMPI=ON` and `LUCKFOX_SDK` pointing at the
unpacked Luckfox Pico SDK.

Expected vendor headers (Luckfox SDK, not shipped in this tree):

- `rk_mpi_sys.h` / `rk_mpi_vi.h` / `rk_mpi_venc.h` / `rk_comm_vi.h` / `rk_comm_venc.h`
  (`media/rockit/rockit/mpi/sdk/include`)
- Link `librockit` + `librockchip_mpp` + `librga` (RV1103 uses the rv1106 uClibc set)
- IMX708 / OV5647 CSI lane config from the board DTS

`platform/hal_capture.cpp` hook (`-DLUCKFOX_RKMPI=ON`): VI → VENC H.264 (or MJPEG),
2 stream buffers, then `esp_capture_sink_acquire_frame` copies Annex-B NALUs.
If MPI init fails, V4L2 / fake frames stay as fallback. Keep rings small (64 MB DDR2).

Until those headers are present, HOST_SIM / V4L2 fallback keeps every CORE
module and all 52 HTTP routes compiling.
