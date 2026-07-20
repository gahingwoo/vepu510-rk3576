// SPDX-License-Identifier: MIT
/*
 * vepu-test.c — minimal V4L2 mem2mem smoke test for the VEPU510 H.264 encoder
 * driver (rockchip-vepu510.ko). No v4l2-ctl/ffmpeg/gstreamer on this rootfs,
 * so this talks to /dev/videoN directly via ioctl(), mirroring the same
 * "small standalone cross-compiled probe tool" style as replay/replay_rocket.c.
 *
 * Encodes exactly ONE synthetic NV12 frame (a horizontal luma gradient, flat
 * 128 chroma) and dumps the resulting bitstream, so this only exercises the
 * IDR/SPS-PPS path (gop_pos==0 on a fresh open) -- see the driver README's
 * bring-up checklist for why that's the right first test (P-frames touch the
 * FBC reference-buffer path, which is the least-verified part of the driver).
 *
 * Usage: vepu-test [/dev/video0] [width] [height] [out.h264]
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/videodev2.h>

#define CHECK(cond, msg) do { if (!(cond)) { perror(msg); exit(1); } } while (0)

static int xioctl(int fd, unsigned long req, void *arg)
{
	int ret;

	do {
		ret = ioctl(fd, req, arg);
	} while (ret == -1 && errno == EINTR);

	return ret;
}

static void fill_nv12_gradient(unsigned char *buf, unsigned int width,
			       unsigned int height)
{
	unsigned int x, y;
	unsigned char *y_plane = buf;
	unsigned char *uv_plane = buf + (size_t)width * height;

	for (y = 0; y < height; y++)
		for (x = 0; x < width; x++)
			y_plane[y * width + x] = (unsigned char)(x * 255 / (width - 1));

	memset(uv_plane, 128, (size_t)width * height / 2);
}

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/video0";
	unsigned int width = argc > 2 ? (unsigned int)atoi(argv[2]) : 176;
	unsigned int height = argc > 3 ? (unsigned int)atoi(argv[3]) : 144;
	const char *outpath = argc > 4 ? argv[4] : "/opt/npu-test/vepu-out.h264";

	struct v4l2_capability cap = {0};
	struct v4l2_format fmt = {0};
	struct v4l2_requestbuffers reqbuf = {0};
	struct v4l2_buffer buf = {0};
	int fd, i;
	void *out_map, *cap_map;
	size_t out_len, cap_len;
	FILE *out;

	fd = open(dev, O_RDWR);
	CHECK(fd >= 0, "open");

	CHECK(xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0, "VIDIOC_QUERYCAP");
	printf("driver=%s card=%s\n", cap.driver, cap.card);

	/* OUTPUT: raw NV12 source frame */
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
	fmt.fmt.pix.width = width;
	fmt.fmt.pix.height = height;
	CHECK(xioctl(fd, VIDIOC_S_FMT, &fmt) == 0, "VIDIOC_S_FMT(OUTPUT)");
	width = fmt.fmt.pix.width;
	height = fmt.fmt.pix.height;
	printf("OUTPUT: %ux%u sizeimage=%u\n", width, height, fmt.fmt.pix.sizeimage);
	out_len = fmt.fmt.pix.sizeimage;

	/* CAPTURE: H.264 Annex-B bitstream */
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
	fmt.fmt.pix.width = width;
	fmt.fmt.pix.height = height;
	CHECK(xioctl(fd, VIDIOC_S_FMT, &fmt) == 0, "VIDIOC_S_FMT(CAPTURE)");
	printf("CAPTURE: %ux%u sizeimage=%u\n", fmt.fmt.pix.width, fmt.fmt.pix.height,
	       fmt.fmt.pix.sizeimage);
	cap_len = fmt.fmt.pix.sizeimage;

	/* OUTPUT queue: one buffer, fill it, queue it */
	reqbuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	reqbuf.memory = V4L2_MEMORY_MMAP;
	reqbuf.count = 1;
	CHECK(xioctl(fd, VIDIOC_REQBUFS, &reqbuf) == 0, "VIDIOC_REQBUFS(OUTPUT)");

	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.index = 0;
	CHECK(xioctl(fd, VIDIOC_QUERYBUF, &buf) == 0, "VIDIOC_QUERYBUF(OUTPUT)");
	out_map = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		      buf.m.offset);
	CHECK(out_map != MAP_FAILED, "mmap(OUTPUT)");

	fill_nv12_gradient(out_map, width, height);
	buf.bytesused = out_len;
	CHECK(xioctl(fd, VIDIOC_QBUF, &buf) == 0, "VIDIOC_QBUF(OUTPUT)");

	/* CAPTURE queue: one buffer, queue it empty to receive the encode */
	memset(&reqbuf, 0, sizeof(reqbuf));
	reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	reqbuf.memory = V4L2_MEMORY_MMAP;
	reqbuf.count = 1;
	CHECK(xioctl(fd, VIDIOC_REQBUFS, &reqbuf) == 0, "VIDIOC_REQBUFS(CAPTURE)");

	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.index = 0;
	CHECK(xioctl(fd, VIDIOC_QUERYBUF, &buf) == 0, "VIDIOC_QUERYBUF(CAPTURE)");
	cap_map = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		      buf.m.offset);
	CHECK(cap_map != MAP_FAILED, "mmap(CAPTURE)");
	CHECK(xioctl(fd, VIDIOC_QBUF, &buf) == 0, "VIDIOC_QBUF(CAPTURE)");

	i = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	CHECK(xioctl(fd, VIDIOC_STREAMON, &i) == 0, "VIDIOC_STREAMON(OUTPUT)");
	i = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	CHECK(xioctl(fd, VIDIOC_STREAMON, &i) == 0, "VIDIOC_STREAMON(CAPTURE)");

	/* Blocks until the hardware (or the watchdog, on error) finishes the frame. */
	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_MMAP;
	CHECK(xioctl(fd, VIDIOC_DQBUF, &buf) == 0, "VIDIOC_DQBUF(CAPTURE)");

	printf("encoded frame: bytesused=%u flags=0x%x\n", buf.bytesused, buf.flags);
	printf("first 16 bytes:");
	for (unsigned int k = 0; k < 16 && k < buf.bytesused; k++)
		printf(" %02x", ((unsigned char *)cap_map)[k]);
	printf("\n(expect 00 00 00 01 27 = SPS, then 00 00 00 01 28 = PPS,"
	       " then 00 00 00 01 25 = IDR slice)\n");

	out = fopen(outpath, "wb");
	CHECK(out != NULL, "fopen(outpath)");
	fwrite(cap_map, 1, buf.bytesused, out);
	fclose(out);
	printf("wrote %u bytes to %s\n", buf.bytesused, outpath);

	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	buf.memory = V4L2_MEMORY_MMAP;
	CHECK(xioctl(fd, VIDIOC_DQBUF, &buf) == 0, "VIDIOC_DQBUF(OUTPUT)");

	i = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	xioctl(fd, VIDIOC_STREAMOFF, &i);
	i = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	xioctl(fd, VIDIOC_STREAMOFF, &i);

	munmap(out_map, out_len);
	munmap(cap_map, cap_len);
	close(fd);

	return 0;
}
