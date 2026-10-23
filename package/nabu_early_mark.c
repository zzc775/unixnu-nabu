// 实现：只做普通内存写（base + rowBytes*y + 4*x），不碰任何寄存器，
// 不做内存分配，失败绝不 panic —— 最坏情况就是屏幕上什么也不多。
//
// 这份文件与 UniXNU/scripts/build-xnu-4k-baseline.sh 内联的 MKC / MKH 保持一致
// （CI 用的是那份内联版本，本目录是带注释的镜像源）。

#include "nabu_early_mark.h"
#include <stddef.h>

static volatile unsigned int *g_fb = NULL;
static unsigned long g_px = 0;      // 每行像素数 = rowBytes / 4
static unsigned int  g_w  = 0;
static unsigned int  g_h  = 0;
static int           g_slot = 0;    // 下一个标记画在第几槽

static void fill(unsigned int x0, unsigned int y0, unsigned int w, unsigned int h, unsigned int c)
{
	unsigned int x, y, xe = x0 + w, ye = y0 + h;
	if (g_fb == NULL) {
		return;
	}
	if (xe > g_w) {
		xe = g_w;
	}
	if (ye > g_h) {
		ye = g_h;
	}
	for (y = y0; y < ye; y++) {
		for (x = x0; x < xe; x++) {
			g_fb[(unsigned long)y * g_px + x] = c;
		}
	}
}

static int sane(unsigned long row, unsigned long w, unsigned long h)
{
	return row >= 4 && row <= 65536 && w >= 1 && w <= 8192 && h >= 1 && h <= 8192;
}

void nabu_mark_init(const unsigned long *v)
{
	unsigned long row, w, h;
	int fallback = 0;

	g_fb = NULL;
	g_px = 0;
	g_w = 0;
	g_h = 0;
	g_slot = 0;

	if (v == NULL || v[0] == 0) {
		return;                 // 没有帧缓冲：静默放弃
	}

	// struct Boot_Video 顺序：base, display, rowBytes, width, height, depth
	row = v[2];
	w   = v[3];
	h   = v[4];
	if (!sane(row, w, h)) {
		// 旧数组顺序：base, rowBytes, width, height, depth, display
		row = v[1];
		w   = v[2];
		h   = v[3];
		fallback = 1;
		if (!sane(row, w, h)) {
			// 几何信息完全不可用，但 base 看着可写：
			// 画一小块红点，让「帧缓冲没映射」和「映射了但参数是垃圾」可区分
			g_fb = (volatile unsigned int *)(unsigned long)v[0];
			g_px = 4096;
			g_w  = 4096;
			g_h  = 4096;
			fill(0, 0, 16, 16, 0xFFFF0000);
			g_fb = NULL;
			return;
		}
	}

	g_fb = (volatile unsigned int *)(unsigned long)v[0];
	g_px = row / 4;
	g_w  = (unsigned int)w;
	g_h  = (unsigned int)h;

	// 先铺一条黑底，保证色块对比度
	fill(0, 0, g_w, 112, 0xFF000000);

	// 若走了 fallback（或 depth 不是 32bpp），在横条最右画红块提示布局有问题
	if (fallback || (v[5] & 0xFF) != 32) {
		fill(32 + 7 * 64, 32, 48, 48, 0xFFFF0000);
	}
}

void nabu_mark(int n)
{
	static const unsigned int colors[9] = {
		0,          // 0 未用
		0xFFFFFFFF, // 1 白
		0xFFFFFF00, // 2 黄
		0xFF00FFFF, // 3 青
		0xFF00FF00, // 4 绿
		0xFFFF00FF, // 5 品红
		0xFFFF8000, // 6 橙
		0xFF0000FF, // 7 蓝
		0xFFFF0000, // 8 红（报错专用）
	};

	if (g_fb == NULL || n < 1 || n > 8 || g_slot >= 8) {
		return;
	}
	fill(32 + (unsigned int)g_slot * 64, 32, 48, 48, colors[n]);
	g_slot++;
}
