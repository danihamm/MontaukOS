SECTIONS
{
    . = 0x400000;
    .text : {
        *(.text)
    }
    .eh_frame : {
        *(.eh_frame)
    }
}
