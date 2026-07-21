# RK3576 VEPU510 encoder driver — status

v1 upstream-style V4L2 stateful mem2mem driver for the VEPU510 H.264
hardware encoder (RK3576, e.g. Radxa ROCK 4D). **Confirmed producing a
real, correctly-structured H.264 bitstream on real hardware** — see bug 4
below. This is a from-scratch clean-room driver built by cross-reading two
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

0. **Isolated `rk_iommu` write fault, UNRESOLVED but not a functional
   blocker.** On an otherwise fully successful encode (`bytesused`
   correct, no `V4L2_BUF_FLAG_ERROR`, correct SPS/PPS/IDR bytes), a single
   `rk_iommu: Page fault at 0x...of type write` sometimes appears,
   followed ~150ms later by `rk_iommu_suspend()` (triggered by this
   driver's own `pm_runtime_put_autosuspend()`, since `vepu0`/`vepu0_mmu`
   share a power domain) hitting `Disable paging/stall request timed out`.
   The fault IOVA changes between boots but has a consistent byte
   structure (fixed middle bytes, varying top byte).

   The downstream vendor driver's `rkvenc2_iommu_fault_handle()`
   (`drivers/video/rockchip/mpp/mpp_rkvenc2.c`) confirms this exact class
   of fault is a *known, tolerated* condition on real VEPU510 silicon —
   its own comment: *"Mask iommu irq, in order for iommu not repeatedly
   trigger pagefault. Until the pagefault task finish by hw timeout."* It
   doesn't try to prevent the fault via register content; it masks the
   IOMMU's IRQ (a vendor-kernel-only `rockchip_iommu_mask_irq()`
   extension, absent from mainline `drivers/iommu/rockchip-iommu.c`) and
   lets the task finish anyway.

   Tried and reverted: resetting the core before every frame (made things
   *worse* — a hardware `enc_err` status and a wildly-oversized
   bitstream). Also tried and reverted: registering a generic mainline
   `iommu_set_fault_handler()` — this made things worse too. This
   device's IOMMU domain is the ordinary DMA-API domain (already claimed
   by `iommu-dma` for this driver's own `dma_alloc_coherent`/vb2-dma-contig
   buffers), and that API refuses (`WARN_ON`) to install a handler on a
   domain whose cookie is already in use for something else — the
   registration silently no-op'd while printing a scary `WARN_ON`
   backtrace + tainting the kernel on every single boot, for zero actual
   benefit. Reverted in full.

   Also tried and reverted, from a real working reference driver for the
   sibling VEPU580 IP (rcawston/rockchip-rk3588-mainline-patches): an
   "ENC_CLR force-clear" before the main register writes (their own
   comment: *"Add force clear to avoid pagefault (VEPU580 workaround)"*,
   gated behind `hw->vepu_type == RKVENC_VEPU_580` in their source — this
   driver tried it unconditionally on VEPU510, which turned out to be
   unsafe: under multi-frame testing it caused every frame to fail with a
   genuine hardware watchdog, not just the cosmetic IOMMU fault); an
   explicit `iommu_flush_iotlb_all()` right before the kick (same
   reference driver, unconditional there too); and `rc_qp.rc_qp_range=1`
   instead of `0` (matching a real successful mpp capture's adaptive-RC
   value, on the theory that `range=0` might be a degenerate case for the
   hardware's internal AQ search — even though `rc_min_qp==rc_max_qp`
   should make the range moot for actual output). None of the three
   stopped the IOMMU fault, and stacked together they caused the
   multi-frame regression above — reverted as a set (not individually
   bisected, since the whole investigation was shelved regardless of
   which one was the actual culprit).

   Kept (independently justified, not reverted): a one-time reset in
   `probe()` via a real `pm_runtime` activation (the core was never
   explicitly reset to a defined state on a fresh boot at all before —
   fixed regardless of whether it affects this fault), and matching the
   CCU dual-core handshake register (`RKVENC_REG_DUAL_CORE`, 0x304) to
   the vendor's own real captured value instead of leaving it unwritten
   (also a real, independently-confirmed gap). A temporary `dev_info()`
   dump of every DMA address `rkvenc_h264_run()` uses is still in the
   code (`rkvenc-h264.c`, right after `RKVENC_REG_DBG_CLR`) for future
   correlation — remove once resolved.

   **Shelved.** After 8 total attempts across register content, timing,
   and IOMMU-API angles, none stopped the fault, and the encode itself
   has been byte-correct on every single test run regardless of it. A
   real fix likely needs to live in `drivers/iommu/rockchip-iommu.c`
   itself (e.g. an upstream IRQ-mask primitive equivalent to the vendor's,
   or making `rk_iommu_disable()`'s disable-paging/stall polling more
   tolerant of a very recent fault), not this driver alone. Treated as a
   known cosmetic issue (alarming dmesg output, no observed functional
   impact) rather than a blocker — don't resume blind attempts here
   without a genuinely new lead (new instrumentation data, not another
   register guess).
0.5 **CONFIRMED BROKEN: P-frames hang the hardware watchdog, every time.**
   Board-tested 2026-07-21 with a new multi-frame test mode (see
   `vepu-test.c`'s `num_frames` argument). Frame 0 (I-frame, `gop_pos==0`)
   succeeds exactly as always (`bytesused` correct, `KEYFRAME` flag, no
   error). **Frame 1 onward — every real P-frame — fails with a genuine
   hardware watchdog** (`encode error, int_sta=0x00000100 (error bits:
   wdg)`), not the cosmetic IOMMU fault above. Confirmed via the
   temporary `dev_info()` address dump that this is genuinely exercising
   the real, non-aliased ping-pong path for the first time ever:
   `recn[w=1]=<slot 1> recn[r=0]=<slot 0>` — reading frame 0's actual
   reconstruction, not an alias of the write buffer (frame 0's own
   `read_idx==write_idx` special case never touches this). This is
   exactly the FBC-reference-buffer read path this README has always
   flagged as "the least-verified part of the driver" (see the bring-up
   checklist below) — now confirmed actually broken, not just
   theoretically risky.

   Notable: `bs_lgth` was nonzero (134 bytes) at the moment the watchdog
   fired — the hardware made real partial progress before hanging, not
   an instant failure. The per-frame control registers looked internally
   consistent for the P-frame case (QP correctly switched `i_qp`→`p_qp`,
   `synt_sli0` correctly showed `sli_type=P`/incremented `frm_num`) — no
   obviously-wrong value spotted yet in what this driver already
   computes. Most likely explanation: something about how the hardware
   actually consumes real (non-self-referencing) recon/thumb/smear
   content, or motion-estimation search behavior against genuine
   reference data, that frame 0's degenerate aliased case never
   exercised — not yet root-caused.

   **Update 2026-07-21, theoretical audit (not yet board-tested)**: instead
   of a fresh multi-frame board capture, this was root-caused by diffing
   this driver's register writes directly against a real checkout of
   rockchip-linux/mpp's own `hal_h264e_vepu510.c` (the actual field-level
   source for every register this driver reconstructs manually, as opposed
   to the raw wtrace byte capture used for PARAM/SQI). That diff found
   three concrete, real gaps, all now fixed:
   - `enc_pic.mei_stor` was always left 0, even though this driver always
     provides a real `meiw_addr` (motion-detection-info buffer). mpp's
     `hal_h264e_vepu510_gen_regs()` sets `mei_stor = task->md_info ? 1 : 0`
     unconditionally — i.e. real hardware expects `mei_stor=1` whenever a
     valid address is given, not just "if the caller feels like it." Since
     real motion-vector output only exists on inter (P) frames — an I-frame
     has no genuine ME search to write out — this specific mismatch
     (a valid address paired with "don't actually store to it") is a
     strong candidate for something in the ME-output write path never
     signaling completion, backing up until the watchdog fires. Fixed:
     `enc_pic.mei_stor = 1;` in `rkvenc_h264_run()`.
   - `rdo_cfg.chrm_spcl`/`ccwa_e`/`atr_mult_sel_e` — three bits mpp's
     `setup_vepu510_rdo_pred()` sets unconditionally to 1 on every frame
     (unlike `rect_size`/`vlc_lmt`/`atf_e`/`atr_e`, which really are
     profile/tune-conditional) — were simply never set by this driver at
     all. Fixed.
   - A whole tail of the RC_ROI register class (0x1044-0x107c: the AQ
     activity threshold/step LUT, MAD-statistics thresholds, and a chroma
     KLUT offset — real fields in mpp's `Vepu510RcRoi` struct, set by
     `setup_vepu510_aq()`/`setup_vepu510_me()`/`setup_vepu510_rdo_pred()`)
     was never written at all — not wrong values, just entirely absent
     writes, left at POR/leftover 0. This sits in the *exact* register
     class board bring-up already proved needs non-degenerate content for
     the hardware to complete at all (see the RC_ROI banner comment in
     rkvenc-regs.h re: `rc_dthd_0_8`) — a very plausible source of a real
     internal stall, that would only get exercised once genuine per-block
     activity/MAD statistics start getting computed and compared against
     these thresholds during real inter-mode (P-frame) analysis. Fixed —
     see `RKVENC_REG_AQ_TTHD` and friends in rkvenc-regs.h/rkvenc-h264.c.

   None of these are guesses about *values* — all three are grounded in
   reading mpp's actual field-setup functions line by line, not another
   round of "try a plausible constant and see." Compiles clean (`W=1`, zero
   warnings) and is packaged into a fresh `sdcard.img`, but deliberately
   **not board-tested yet** in this pass — the point of doing the diff
   this way was to stop guess-and-flash iteration and fix everything the
   ground truth actually disagrees with in one pass before the next board
   test, rather than trying one experimental register at a time again.
   **Next step**: flash and re-run the multi-frame test tool. If P-frames
   still hang after this, the next escalation is the original plan (a
   genuine multi-frame mpp capture for a byte-level diff), since at that
   point the remaining gap would no longer be an omitted field but
   something this driver's manual per-field reconstruction still gets
   wrong in a way a source read didn't catch.

   **Update 2026-07-21, board-tested — those 3 fixes made ZERO difference**
   (byte-identical failure: same `bs_lgth=0x86`, same `enc_wdg=0x00021795`).
   Confirmed via register readback that `mei_stor=1` really was live on
   hardware, so this ruled those three out as the cause, not just as
   unconfirmed. Mined the *original* single-I-frame wtrace capture further
   (real silicon, not source) and found two more confirmed gaps: `bs_scp`
   must be 1 (confirmed both in mpp source and in the real capture's
   `0x300=0x4000191c`, bit4 set) — fixed. Also programmatically verified
   the RC_ROI fix above field-by-field against that same real capture:
   everything matched exactly except `aq_stp2`'s middle two fields, which
   used the real captured values (3,5,7,7,8) instead of the compiled-in
   default table (which itself doesn't quite match what this specific
   `mpi_enc_test` invocation produced) — fixed.

   **Then built and ran a real multi-frame (3-frame) vendor wtrace
   capture** — this is the actual escalation, and it paid off. Diffing
   frame0(I)/frame1(P)/frame2(P)'s real register writes against each other
   found:
   - **PARAM class ATR (anti-ringing) weights are genuinely I/P-slice-
     dependent**, not the frame-invariant tuning blob this driver assumed.
     `0x1744`/`0x1750`/`0x1754`/`0x1758` were identical across the two
     P-frames but different from the I-frame — traced to mpp's
     `setup_vepu510_anti_ringing()`, which has a real I-vs-P branch. This
     driver's static `rkvenc_par_class` blob was captured from an
     I-frame-only session, so every P-frame silently got the wrong
     (I-slice) ATR weights the whole time. Fixed with a small per-frame
     patch on top of the static blob, values confirmed exactly against the
     real capture (see `RKVENC_REG_ATR_THD1`/`WGT16`/`WGT8`/`WGT4` in
     rkvenc-regs.h).
   - **SQI `smear_opt_cfg.stated_mode`** similarly depends on slice-type
     *history* (1 if this slice or the *previous* one was I, else 2) —
     confirmed exactly across all 3 frames (I:1, P-after-I:1, P-after-P:2).
     Pure state this driver can and now does track (`last_frame_was_idr`).
     Fixed. Its sibling field `rdo_smear_dlt_qp` genuinely depends on real
     per-block smear statistics read back from the previous frame's
     hardware counters (mpp's `ctx->last_frame_fb`) — this driver has no
     such readback implemented, so it's approximated as a static per-slice-
     type constant (-1 I / -3 P, matching what the real capture happened to
     produce) rather than fully replicated; a real gap, but RDO-cost tuning
     rather than known hang-relevant.
   - **`synt_nal.nal_ref_idc` was hardcoded 1 for every frame** — the real
     capture shows 3 for the IDR slice and 2 for P slices (both real
     references, matching this driver's `cur_frm_ref=1`). Fixed. Spec-
     compliance bug in the hardware-generated slice NAL header, not
     expected to be hang-related (the hardware just emits whatever value
     it's given).

   All of the above are grounded in an exact, programmatically-verified
   diff against real register writes on real silicon across a genuine
   3-frame session — not source-reading, not another guess. Compiles clean,
   packaged into a fresh `sdcard.img`. **Still not board-tested with this
   combined set** (the ATR/smear/nal_ref_idc fixes came right after the
   multi-frame capture, in the same pass) — next step is exactly that.
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
- Run `/opt/npu-test/vepu-test` for a single IDR frame (default, or pass
  `1` as the 5th arg / omit it and Ctrl-C after frame 0) — this is the
  test loop that found bugs 1-3 above and is confirmed reliably working.
  Check `encoded frame: bytesused=` is nonzero and `flags` does NOT have
  `V4L2_BUF_FLAG_ERROR` (0x40) set; check the first 16 bytes match
  `00 00 00 01 27 ... 00 00 00 01 28 ... 00 00 00 01 25`.
  Watch `dmesg` for `rk_iommu` faults (see known gap 0 — cosmetic, not
  blocking) or `encode timeout, resetting core` (watchdog fired).
- `vepu-test` also takes a `num_frames` argument (default 10) to test
  P-frames — **currently known to fail every time from frame 1 onward**,
  see known gap 0.5 above. Don't spend more bring-up time on multi-frame
  testing until that's root-caused; single-IDR-frame use is solid.
