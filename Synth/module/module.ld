/* RAM overlay 模块的链接脚本（独立编译、独立链接到 AXI SRAM 顶部 64 KB）。
 *
 * 由 CMake 的 ui_module 目标使用：-T Synth/module/module.ld -nostdlib -nostartfiles
 * 模块里不能有 C 运行时（无 startup、无 .data 拷贝、无 libc），所有输入都走 module_api.h
 * 的函数指针表；因此链接要 -nostdlib，且实现里别引用 memcpy/memset/printf 之类。
 *
 * 镜像从上电后由宿主 memcpy 到 SYNTH_MODULE_RAM_BASE（见 module.c），
 * .bss 的零也包含在镜像里（tools/make_module.py 会把 .bss 区间补零后算进 image_bytes）。
 */
ENTRY(ui_entry)

MEMORY
{
    MODULE_RAM (rwx) : ORIGIN = 0x24070000, LENGTH = 64K
}

SECTIONS
{
    .text : {
        *(.text.ui_entry)
        *(.text*)
        *(.rodata*)
    } > MODULE_RAM

    .data : {
        *(.data*)
    } > MODULE_RAM

    .bss (NOLOAD) : {
        *(.bss*)
        *(COMMON)
    } > MODULE_RAM

    /* ARM 异常索引表在裸模块里没用，且会占几字节 */
    /DISCARD/ : {
        *(.ARM.exidx*)
        *(.ARM.extab*)
        *(.comment)
        *(.note*)
    }
}
