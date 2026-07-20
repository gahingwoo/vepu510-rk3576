# RK3576 VEPU510 encoder driver — status

v1 upstream-style V4L2 stateful mem2mem driver for the VEPU510 H.264
hardware encoder (RK3576, e.g. Radxa ROCK 4D). Not yet run on hardware —
this is a from-scratch clean-room driver built by cross-reading two
sources, no vendor kernel code copied:

- `drivers/video/rockchip/mpp/mpp_rkvenc2.c` (rockchip-linux/kernel,
  downstream) — hardware control/IRQ/quirk behavior. Register field
  *names* are not in this source; it treats most classes as opaque blobs.
- `mpp/hal/rkenc/{common,h264e}/*` (rockchip-linux/mpp, Apache-2.0,
  upstream on GitHub) — the actual register bitfield layout. This is
  where every `struct rkvenc_reg_*` in rkvenc-regs.h comes from.

Architecture: modelled on `drivers/media/platform/verisilicon`'s hantro
`device_run()`/`codec_ops{run,done}` split (stateful encode), not
rkvdec's request-API decode pattern. Register access follows rkvdec's
struct-mapped-bitfield idiom. rkvenc0/rkvenc1 are two independent V4L2
nodes — no CCU/DCHS cross-core coordination (see comment block at the
top of rkvenc.c for why that's the right scope for v1).

## Compiles clean

`make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- M=drivers/media/platform/rockchip/rkvenc modules`
— zero warnings. DTB with `CHECK_DTBS=y` — zero schema violations against
the new binding for the vepu0/vepu1/vepu*_mmu nodes.

## Hardware bring-up log (real board, ROCK 4D)

Three real bugs found and fixed via actual `vepu-test` runs on hardware
(see `vepu-test/vepu-test.c`), each confirmed by reading serial console
output, not guessed:

1. **NULL-pointer Oops in every `g/s/try_fmt_vid_cap/out` handler.**
   Modern V4L2 core (`v4l2-ioctl.c`) unconditionally passes `NULL` for
   the `fh`/`priv` argument on these ioctls — universal V4L2 behavior,
   confirmed by reading `v4l_s_fmt()`/`v4l_reqbufs()` etc. in this exact
   kernel tree. Fixed by deriving `ctx` via `file_to_v4l2_fh(file)`
   instead, matching what `v4l2_m2m_ioctl_reqbufs()` etc. do internally.
2. **The encoder never actually started.** `ENC_START` (0x10) isn't a
   bare `1` — its real layout (`Vepu510ControlCfg.enc_strt` in mpp) is
   `lkt_num:8, vepu_cmd:3`; the "go" trigger is `vepu_cmd=1` at bits
   [10:8], i.e. `0x100`. Writing `1` only set the irrelevant `lkt_num`
   field, so the core silently never ran and every frame timed out.
   Also fixed in the same pass, once BASE-class registers were
   cross-checked against mpp's `Vepu510ControlCfg` instead of the
   vaguer downstream-kernel-comment reading used originally: `INT_EN`
   was missing `enc_done_en` (bit0, the primary completion signal) and
   most error bits; `INT_MASK` was never written at all; `ENC_WDG`'s
   `vs_load_thd` field is 24 bits at bits[23:0], not "the top byte" —
   the old code shifted the threshold into bits[31:24], leaving the
   real field permanently 0. `OPT_STRG`/`DTRNS_MAP` (never written
   before) were added too, matching mpp's `setup_vepu510_normal()`.
3. **IOMMU write-fault storm at iova 0x0.** Once the kick was fixed the
   core actually ran — and then hammered `rk_iommu` with continuous
   write faults at address 0. mpp's `setup_vepu510_recn_refr()` shows
   VEPU510 writes **three** scratch regions per frame on every single
   frame, unconditionally, regardless of which encoder features are
   enabled: the FBC pixel buffer (`rfpw_h/b_addr`, already allocated),
   a "thumbnail" downsample buffer (`dspw_addr`), and a "smear" buffer
   (`adr_smear_wr`). The last two were never allocated — left at a NULL
   pointer, hence the fault storm instead of a clean error. Fixed by
   allocating both alongside the pixel buffer, using mpp's *exact* size
   formulas (`aligned_w = ALIGN(w,64)`, `aligned_h = ALIGN(h,16)+16`,
   `thumb = ALIGN(aligned_w/64 * aligned_h/64 * 256, 8K)`,
   `smear = ALIGN(aligned_w/64,16) * ALIGN(aligned_h/16,16)`) rather
   than a guess — see `rkvenc_h264_ctx` in rkvenc.h and
   `rkvenc_h264_start()`.
4. **Genuine hardware-internal-watchdog stall (`int_sta.wdg_sta`,
   ~50ms), root-caused via ground-truth capture, not guesswork.** After
   bug 3, the encoder ran and produced *zero bytes* every time, hitting
   its own internal watchdog. Months of register-level hypotheses (the
   0x74/0x308 quirk, roi_qthd0-3, ME search config, IOMMU TLB timing)
   all came back negative. Root cause was finally found by building
   rockchip-linux/mpp's own `mpi_enc_test` CLI from source, running it
   successfully on the *same* hardware/kernel via the vendor's raw MPP
   ioctl protocol (no vendor userspace library exists on this rootfs —
   see `vepu-test/vepu-vendor-test.c`), and diffing its real register
   writes (captured via a `wtrace`-style instrumentation of the vendor
   kernel driver) against this driver's own writes byte-for-byte. Eight
   real gaps, all now fixed:
   - `adr_src2` must equal `adr_src1` (not 0) even for 2-plane NV12.
   - The recon/thumbnail/smear **read**-side pointers (`rfpr_h/b`,
     `dspr`, `smear_rd`) must alias the **write**-side buffer on the
     first frame of a session (no previously-written reference exists
     yet) — this driver's 2-deep ping-pong pool previously pointed the
     read side at a second buffer that had never been written.
   - **The entire PARAM (RDO lambda tables), SQI (subjective-adjust),
     and SCL (scaling-list) register classes were never written at
     all** — confirmed as the single biggest gap; nothing in the
     userspace HAL documents them as required (mpp's own FIXQP RC path
     never touches the RC_ROI threshold ladder either), but the
     hardware genuinely needs them populated to complete a task at all,
     not just to produce good-quality output. See the RC_ROI/PARAM/SQI/
     SCL banner comments in rkvenc-regs.h for the full story.
   - The RC_ROI class's `rc_adj0/rc_adj1`/`rc_dthd_0_8` bit-count-
     deviation threshold ladder (0x1000-0x1028) must be populated — this
     driver's original `rc_cfg.rc_en = 0` fixed-QP design left these at
     0, which alone reproduces the exact same stall. Fixed by switching
     to a real `rc_en=1`/`aq_en=1` configuration with `rc_qp_range=0`
     and `rc_min_qp==rc_max_qp` clamping AQ to a no-op — genuine
     fixed-QP *output* via a real, non-degenerate RC *configuration*.
   - `dtrns_map`/`opt_strg` were already correct in this driver (cross-
     checked against a real capture: both matched exactly) — not a bug,
     confirmed rather than assumed.
   - `meiw_addr` (motion-detection-info pointer) needs a real allocated
     buffer, not 0.
   - `rfpt_h_addr`/`rfpt_b_addr` (interlaced-field addresses, unused —
     this driver is progressive-only) need the literal sentinel
     `0xffffffff`, not 0.
   - `src_fmt` needs an additional `out_fmt=1` bit beyond `src_cfmt`.

   With all eight applied, the from-scratch vendor-ioctl test client
   completed a real encode (non-zero bitstream, ~1ms real completion IRQ,
   byte-for-byte the same completion signature as `mpi_enc_test`) on the
   same board. **Not yet re-confirmed against this actual V4L2 driver on
   real hardware** — the fixes were ported here immediately after
   confirming them via the standalone ioctl client; next board run
   should confirm this driver itself now completes real encodes.

## Known gaps / best-effort areas (in rough priority order for board bring-up)

1. **`0x74`/`0x308` VEPU510 quirk (`rkvenc_vepu510_quirk()`)** — required
   per the downstream vendor kernel driver, confirmed absent from the
   userspace mpp HAL entirely (so it can't be cross-checked there).
   Treated as an opaque required write.
2. **RC's per-frame bit budget is a rough approximation.** `rc_cfg.rc_en`
   is now genuinely 1 (see bug 4 above) with a real threshold-ladder
   formula from mpp, but the `bit_target` feeding that formula is just
   `V4L2_CID_MPEG_VIDEO_BITRATE / 30` (an assumed 30fps — this driver
   doesn't negotiate real frame intervals via `G/S_PARM` yet), not true
   adaptive bitrate tracking. Since `rc_qp_range=0` clamps the AQ engine
   to a no-op regardless, this doesn't affect output QP — it only needs
   to be sane, not accurate. Real CBR/VBR (letting the hardware actually
   vary QP per-CTU against a real bit budget) is unimplemented.
3. **HEVC is not implemented.** Same register family
   (`hal_h265e_vepu510_reg.h` in mpp) but a different FRAME-class
   trailer and NAL/slice-header field layout; add as
   `rkvenc-hevc.c` + a second `rkvenc_coded_fmt_desc` entry, following
   the exact pattern `rkvenc-h264.c` establishes.
4. **Multi-slice / CCU / DCHS** — out of scope by design for v1, see the
   architecture comment in rkvenc.c. Not a bug, just unimplemented.

## First hardware bring-up checklist

- Confirm probe succeeds and both `/dev/videoN` nodes appear.
- Run `/opt/npu-test/vepu-test` for a single IDR frame (GOP size 1) —
  this is the actual test loop that found bugs 1-3 above. Check
  `encoded frame: bytesused=` is nonzero and `flags` does NOT have
  `V4L2_BUF_FLAG_ERROR` (0x40) set; check the first 16 bytes match
  `00 00 00 01 27 ... 00 00 00 01 28 ... 00 00 00 01 25`.
  Watch `dmesg` for `rk_iommu` faults (missing buffer pointer) or
  `encode timeout, resetting core` (watchdog fired — item 1 above).
- P-frames exercise the FBC reference-*read* path (as opposed to the
  write path every frame already touches) for the first time — bring
  up IDR-only first.
