// SPDX-License-Identifier: LGPL-2.1
/*
 * ALSA ioplug "ac108" —— 4ch/S32/48k 直录虚拟设备（tag 解码，免定相）
 *
 * 板上 micgen 卡 (hw:1,0) 的线上流 = 96k/2ch/S32，每个 32bit 字 =
 * [30bit 音频][2bit 通道 tag]，tag = ADC序号-1。本插件把它重组成
 * 4 通道 48k 流：ch N = mic N，S32 输出 = 线上字清掉低 2 位。
 *
 * asound.conf:
 *   pcm.ac108 { type ac108  slavepcm "hw:1,0" }
 * 用法:
 *   arecord -D ac108 -c 4 -f S32_LE -r 48000 out.wav
 *   （-c/-f/-r 可省略，默认即 4ch/S32/48k）
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <errno.h>
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#endif

/* BCLK-PLL：把 AC108 的 PLL 输入从板载晶振翻到 I2S BCLK（H3 主模式），
 * speaker 与 mic 完全同钟（实测钟差 -0.04ppm vs 晶振 -3.8ppm）。
 * 必须在流启动后（BCLK 在跑）执行；PLL 已锁状态下翻源会自动重捕获。
 * 分频: 6.144MHz(BCLK) ×400/((4+1)(9+1)(1+1)) = 24.576MHz（seeed 原厂表） */
static int dbg;
#define DBG(...) do { if (dbg) fprintf(stderr, __VA_ARGS__); } while (0)
/* 速率/PLL 配置全部由内核驱动 ac108_init 负责（sysfs 参数, 寄存器时序在内核侧）。
 * 本插件仅在首次 transfer 时把流速率(+可选 pll_bclk=1, 0ppm 相干)通知驱动。 */
static void ac108_notify_driver(unsigned int rate, int pll_bclk)
{
	FILE *f;

	if (pll_bclk > 0) {
		f = fopen("/sys/module/ac108_init/parameters/pll_bclk", "w");
		if (f) { fprintf(f, "1"); fclose(f); }
		else SNDERR("ac108: ac108_init driver not loaded, PLL left as-is");
	}
	f = fopen("/sys/module/ac108_init/parameters/rate", "w");
	if (f) { fprintf(f, "%u", rate); fclose(f); }
	else SNDERR("ac108: ac108_init driver not loaded, chip rate not switched");
}

static int dbg;
#define DBG(...) do { if (dbg) fprintf(stderr, __VA_ARGS__); } while (0)
struct ac108_plug {
	snd_pcm_ioplug_t io;
	snd_pcm_t *slave;
	snd_pcm_uframes_t hw_ptr;	/* 已产出的 io 帧计数（pointer 推进） */
	snd_pcm_uframes_t last_avail;
	int pll_bclk;			/* 1 = 流启动后把 AC108 PLL 翻到 BCLK */
	int flipped;			/* 本次流是否已翻过 */
};

static int ac108_start(snd_pcm_ioplug_t *io)
{
	struct ac108_plug *p = io->private_data;
	return snd_pcm_start(p->slave);
}

static int ac108_stop(snd_pcm_ioplug_t *io)
{
	struct ac108_plug *p = io->private_data;
	return snd_pcm_drop(p->slave);
}

static snd_pcm_sframes_t ac108_pointer(snd_pcm_ioplug_t *io)
{
	/* 关键：hw_ptr 必须独立于 transfer 推进（跟踪 slave 进度），
	 * 否则 avail 永不增长 -> readi 死锁 -> slave xrun */
	struct ac108_plug *p = io->private_data;
	snd_pcm_sframes_t avail = snd_pcm_avail(p->slave) / 2;	/* io 帧单位 */

	if (avail < 0)
		avail = 0;
	if ((snd_pcm_uframes_t)avail > p->last_avail)
		p->hw_ptr += avail - p->last_avail;
	p->last_avail = avail;
	DBG("pointer: slave_avail=%ld hw_ptr=%lu buf=%lu -> %lu\n",
	    (long)snd_pcm_avail(p->slave), (unsigned long)p->hw_ptr,
	    (unsigned long)io->buffer_size,
	    (unsigned long)(p->hw_ptr % io->buffer_size));
	return p->hw_ptr % io->buffer_size;
}

static snd_pcm_sframes_t ac108_transfer(snd_pcm_ioplug_t *io,
					const snd_pcm_channel_area_t *areas,
					snd_pcm_uframes_t offset,
					snd_pcm_uframes_t size)
{
	struct ac108_plug *p = io->private_data;
	int32_t *src = malloc(size * 2 * 2 * sizeof(int32_t));	/* slave 帧 × 2ch × 4B */
	snd_pcm_sframes_t r;
	snd_pcm_uframes_t n, k;
	int ch;

	DBG("transfer size=%lu slave=%lu state=%d\n", size, size * 2, snd_pcm_state(p->slave));
	if (!p->flipped) {
		p->flipped = 1;
		ac108_notify_driver(io->rate, p->pll_bclk);	/* 通知内核驱动切速率/PLL */
	}
	r = snd_pcm_readi(p->slave, src, size * 2);
	DBG("  readi=%ld state=%d\n", (long)r, snd_pcm_state(p->slave));
	if (r < 0 && (r == -EPIPE || r == -ESTRPIPE)) {
		/* xrun 恢复 */
		if (snd_pcm_prepare(p->slave) >= 0) {
			r = snd_pcm_readi(p->slave, src, size * 2);
			DBG("  retry readi=%ld state=%d\n", (long)r, snd_pcm_state(p->slave));
		}
	}
	if (r <= 0) {
		free(src);
		return r ? r : -EIO;
	}
	n = r / 2;			/* 实得 io 帧数（slave 帧的一半） */

	for (ch = 0; ch < 4; ch++) {
		char *base = (char *)areas[ch].addr +
			(areas[ch].first / 8) +
			offset * (areas[ch].step / 8);
		if (io->format == SND_PCM_FORMAT_S16_LE) {
			int16_t *dst = (int16_t *)base;
			int step = areas[ch].step / 8 / sizeof(int16_t);
			for (k = 0; k < n; k++)
				dst[k * step] = src[k * 4 + ch] >> 16;
		} else {
			int32_t *dst = (int32_t *)base;
			int step = areas[ch].step / 8 / sizeof(int32_t);
			for (k = 0; k < n; k++)
				dst[k * step] = src[k * 4 + ch] & ~3;	/* 清 tag 位 */
		}
	}
	free(src);
	return n;
}

static int ac108_hw_params(snd_pcm_ioplug_t *io, snd_pcm_hw_params_t *params)
{
	struct ac108_plug *p = io->private_data;
	snd_pcm_hw_params_t *sp;
	unsigned int ptime = 50000, btime = 400000;	/* slave 50ms 周期 / 400ms 缓冲 */
	int err;

	(void)params;	/* 插件自身的 params 不能直接喂给 slave */
	snd_pcm_hw_params_alloca(&sp);
	if ((err = snd_pcm_hw_params_any(p->slave, sp)) < 0)
		return err;
	if ((err = snd_pcm_hw_params_set_access(p->slave, sp, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
	    (err = snd_pcm_hw_params_set_format(p->slave, sp, SND_PCM_FORMAT_S32_LE)) < 0 ||
	    (err = snd_pcm_hw_params_set_channels(p->slave, sp, 2)) < 0 ||
	    (err = snd_pcm_hw_params_set_rate(p->slave, sp, io->rate * 2, 0)) < 0)
		return err;
	snd_pcm_hw_params_set_period_time_near(p->slave, sp, &ptime, NULL);
	snd_pcm_hw_params_set_buffer_time_near(p->slave, sp, &btime, NULL);
	return snd_pcm_hw_params(p->slave, sp);
}

static int ac108_hw_free(snd_pcm_ioplug_t *io)
{
	struct ac108_plug *p = io->private_data;
	return snd_pcm_hw_free(p->slave);
}

static int ac108_prepare(snd_pcm_ioplug_t *io)
{
	struct ac108_plug *p = io->private_data;
	p->hw_ptr = 0;
	p->last_avail = 0;
	p->flipped = 0;
	return snd_pcm_prepare(p->slave);
}

static int ac108_close(snd_pcm_ioplug_t *io)
{
	struct ac108_plug *p = io->private_data;
	if (p->slave)
		snd_pcm_close(p->slave);
	free(p);
	return 0;
}

static int ac108_drain(snd_pcm_ioplug_t *io)
{
	struct ac108_plug *p = io->private_data;
	return snd_pcm_drain(p->slave);
}

static int ac108_delay(snd_pcm_ioplug_t *io, snd_pcm_sframes_t *delayp)
{
	*delayp = 0;
	return 0;
}

static int ac108_poll_descriptors_count(snd_pcm_ioplug_t *io)
{
	struct ac108_plug *p = io->private_data;
	return snd_pcm_poll_descriptors_count(p->slave);
}

static int ac108_poll_descriptors(snd_pcm_ioplug_t *io, struct pollfd *pfd, unsigned int space)
{
	struct ac108_plug *p = io->private_data;
	return snd_pcm_poll_descriptors(p->slave, pfd, space);
}

static int ac108_poll_revents(snd_pcm_ioplug_t *io, struct pollfd *pfd, unsigned int nfds, unsigned short *revents)
{
	struct ac108_plug *p = io->private_data;
	return snd_pcm_poll_descriptors_revents(p->slave, pfd, nfds, revents);
}

static snd_pcm_ioplug_callback_t ac108_ops = {
	.poll_descriptors_count = ac108_poll_descriptors_count,
	.poll_descriptors	= ac108_poll_descriptors,
	.poll_revents		= ac108_poll_revents,
	.start		= ac108_start,
	.stop		= ac108_stop,
	.pointer	= ac108_pointer,
	.transfer	= ac108_transfer,
	.hw_params	= ac108_hw_params,
	.hw_free	= ac108_hw_free,
	.prepare	= ac108_prepare,
	.close		= ac108_close,
	.drain		= ac108_drain,
	.delay		= ac108_delay,
};

static int ac108_set_constraints(snd_pcm_ioplug_t *io)
{
	static const unsigned int access[] = { SND_PCM_ACCESS_RW_INTERLEAVED };
	static const unsigned int formats[] = { SND_PCM_FORMAT_S32_LE, SND_PCM_FORMAT_S16_LE };
	static const unsigned int rates[] = { 48000, 16000 };
	static const unsigned int channels[] = { 4 };
	int err;

	if ((err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_ACCESS,
						 1, (unsigned int *)access)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_FORMAT,
						 2, (unsigned int *)formats)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_CHANNELS,
						 1, (unsigned int *)channels)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_RATE,
						 ARRAY_SIZE(rates), rates)) < 0)
		return err;
	err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_BUFFER_BYTES,
					      1, 4 * 1024 * 1024);
	if (err < 0)
		return err;
	err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIOD_BYTES,
					      128, 2 * 1024 * 1024);
	if (err < 0)
		return err;
	return snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIODS, 3, 1024);
}

/* 注意: 编译必须 -DPIC（alsa global.h 的 out-of-tree 动态插件路径），
 * 否则宏展开引用 libasound 未导出的 snd_dlsym_start，dlopen 失败 */

SND_PCM_PLUGIN_DEFINE_FUNC(ac108)
{
	snd_config_iterator_t i, next;
	struct ac108_plug *p;
	const char *slavepcm = "hw:1,0";
	int pll_bclk_cfg = 0;
	int err;

	snd_config_for_each(i, next, conf) {
		snd_config_t *n = snd_config_iterator_entry(i);
		const char *id;
		if (snd_config_get_id(n, &id) < 0)
			continue;
		if (!strcmp(id, "comment") || !strcmp(id, "type") || !strcmp(id, "hint"))
			continue;
		if (!strcmp(id, "slavepcm")) {
			if (snd_config_get_string(n, &slavepcm) < 0) {
				SNDERR("ac108: slavepcm must be a string");
				return -EINVAL;
			}
			continue;
		}
		if (!strcmp(id, "pll_bclk")) {
			long val;
			if (snd_config_get_integer(n, &val) < 0) {
				SNDERR("ac108: pll_bclk must be an integer 0/1");
				return -EINVAL;
			}
			pll_bclk_cfg = (val != 0);
			continue;
		}
		SNDERR("ac108: unknown field %s", id);
		return -EINVAL;
	}

	p = calloc(1, sizeof(*p));
	if (!p)
		return -ENOMEM;

	err = snd_pcm_open(&p->slave, slavepcm, SND_PCM_STREAM_CAPTURE, mode);
	if (err < 0)
		goto err_free;

	p->io.version = SND_PCM_IOPLUG_VERSION;
	p->io.name = "AC108 4ch tag-decoder";
	p->io.flags = SND_PCM_IOPLUG_FLAG_LISTED;
	p->io.mmap_rw = 0;
	p->io.callback = &ac108_ops;
	p->io.private_data = p;
	p->io.channels = 4;
	p->pll_bclk = pll_bclk_cfg;
	dbg = getenv("AC108_DEBUG") != NULL;

	err = snd_pcm_ioplug_create(&p->io, name, stream, mode);
	if (err < 0)
		goto err_slave;

	if ((err = ac108_set_constraints(&p->io)) < 0) {
		snd_pcm_ioplug_delete(&p->io);
		return err;
	}
	*pcmp = p->io.pcm;
	return 0;

err_slave:
	snd_pcm_close(p->slave);
err_free:
	free(p);
	return err;
}

SND_PCM_PLUGIN_SYMBOL(ac108);
