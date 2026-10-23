// nabu_early_mark — 内核最早期「进度标记」，不需要字库、不依赖任何子系统。
//
// 设计动机：内核在 PE_create_console 之前就静默死亡，任何 console 都用不上。
// 但 boot_args.Video 已经给出帧缓冲（base / rowBytes / width / height / depth），
// 所以内核只要往显存里画几块色块，就能把「执行到哪一步」传达到屏幕上。
//
// ⚠️ 布局（xnu-10063.141.1 实测）：boot_args.Video 不是数组，而是
//    struct Boot_Video { v_baseAddr; v_display; v_rowBytes; v_width; v_height; v_depth; }
//    注意 v_display 在**第 2 个** ulong！pexpert/arm/pe_init.c:419-429 就是这么读的。
//    所以这里按「ulong 字流」接收，既能对结构体用，也能兼容旧的 [base,rowBytes,...] 数组。
//
// 用法（arm_init.c 最早期）：
//     nabu_mark_init((const unsigned long *)&args->Video);
//     NABU_MARK(1);            // arm_init 进入
//     ...
//
// 读法：
//   屏幕左上角出现 N 个色块 ⇒ 内核走到了第 N 个检查点
//   一个都没有            ⇒ 连 arm_init 都没进（死在汇编入口/MMU）
//   右上角出现**红色块**  ⇒ 帧缓冲可写，但 Video 布局对不上（错位 / depth 非 32bpp）

void nabu_mark_init(const unsigned long *video);
void nabu_mark(int n);

// 标记编号 → 颜色（1 白 2 黄 3 青 4 绿 5 品红 6 橙 7 蓝 8 红）
#define NABU_MARK(n) nabu_mark(n)
