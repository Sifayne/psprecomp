ENTRY(module_start)
PHDRS { image PT_LOAD FLAGS(7); }
SECTIONS {
  . = 0x08804000;
  .text : { *(.text*) } :image
  .sceStub.text : { *(.sceStub.text) }
  .rodata : { *(.rodata) *(.rodata.str*) *(.rodata.cst*) }
  .rodata.sceModuleInfo : { *(.rodata.sceModuleInfo) }
  .lib.stub : { *(.lib.stub) }
  .data : { *(.data*) *(.sdata*) }
  .bss : { *(.bss*) *(.sbss*) }
  /DISCARD/ : { *(.MIPS.abiflags) *(.reginfo) *(.pdr) *(.comment) }
}
