# vepu510-rk3576

A from-scratch, upstream-style Linux V4L2 driver for the Rockchip VEPU510
H.264 hardware video encoder found on the RK3576 SoC (e.g. Radxa ROCK 4D).

This is a *stateful* V4L2 mem2mem driver (`device_run()`/`codec_ops{run,
done}`, modelled on `drivers/media/platform/verisilicon`'s hantro driver),
not a copy of Rockchip's downstream out-of-tree MPP-service/task-queue
driver. It talks directly to the VEPU510 hardware registers through the
standard V4L2 OUTPUT (raw NV12) / CAPTURE (H.264 Annex-B) queue model.

Status: **confirmed working on real hardware** (Radxa ROCK 4D, RK3576) —
produces a complete, correctly structured H.264 bitstream (SPS/PPS/IDR
slice) from a live encode. See `drivers/media/platform/rockchip/rkvenc/BRINGUP.md`
for the full hardware bring-up log, including every bug found and fixed
along the way, and the current list of known gaps (HEVC, real CBR/VBR
rate control, multi-slice/CCU).

## Layout

```
drivers/media/platform/rockchip/rkvenc/
  rkvenc.c         probe, m2m/vb2 plumbing, IRQ handling, watchdog
  rkvenc.h         shared driver/context structs
  rkvenc-h264.c    H.264 codec backend (register programming, SPS/PPS synth)
  rkvenc-regs.h    VEPU510 register bitfield layout
  Kconfig/Makefile
  BRINGUP.md       hardware bring-up log / known gaps
Documentation/devicetree/bindings/media/
  rockchip,rk3576-vepu.yaml   DT binding (schema)
  rockchip,rk3576-vepu.dts.example   the two VEPU510 core + IOMMU nodes
                                     as added to rk3576.dtsi
vepu-test/
  vepu-test.c      standalone V4L2 ioctl test tool (no v4l2-ctl/ffmpeg
                   needed) -- opens /dev/videoN, sets NV12/H264 formats,
                   streams one synthetic frame, prints the result
```

## Using this driver

This repo holds just the driver, its DT binding, and its test tool — it's
meant to be dropped into a kernel tree that already has the RK3576 SoC
enabled, not built standalone.

1. Copy `drivers/media/platform/rockchip/rkvenc/` into your kernel tree at
   the same path, and add it to
   `drivers/media/platform/rockchip/Kconfig`/`Makefile`:
   ```
   source "drivers/media/platform/rockchip/rkvenc/Kconfig"
   obj-y += rkvenc/
   ```
2. Add the two VEPU510 core nodes (see
   `Documentation/devicetree/bindings/media/rockchip,rk3576-vepu.dts.example`)
   to your `rk3576.dtsi`, if your tree doesn't have them yet.
3. Enable `CONFIG_VIDEO_ROCKCHIP_VEPU510=m` (or `=y`).
4. Build and boot. Two V4L2 M2M nodes should appear (one per VEPU510
   core, `rkvenc0`/`rkvenc1` in dmesg).

Register field names and layout were cross-checked against Rockchip's own
open-source userspace codec library
([rockchip-linux/mpp](https://github.com/rockchip-linux/mpp), Apache-2.0) —
that's the only place the register bitfield names exist publicly; the
downstream kernel driver treats most of the register space as an opaque
blob. Everything else (whether a given register actually needs to be
written, and what for) was pinned down through direct hardware bring-up:
running this driver on a real board, reading serial console/`dmesg`
output, and — for the one stall that source-reading alone couldn't
explain — building Rockchip's own `mpi_enc_test` reference tool from the
same mpp source and comparing its real register writes against this
driver's, one class at a time, until they matched and the encode
completed. See `BRINGUP.md` for the details.

## Testing

```
/opt/vepu-test /dev/video0 176 144 out.h264
```
(or wherever you install `vepu-test.c`'s build output). Defaults to
176x144, encodes one synthetic NV12 gradient frame as an IDR, and prints
`bytesused`/`flags`/the first 16 bytes of the result.

## License

GPL-2.0, matching the kernel driver convention (see `LICENSE` and each
file's `SPDX-License-Identifier` line).
