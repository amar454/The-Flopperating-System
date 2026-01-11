local cc = require("lang_c")
local nasm = require("lang_nasm")
local ld = require("ld")

local cflags = {
    "-m32",
    "-ffreestanding",
    "-fno-stack-protector",
    "-std=gnu2x -g",
    "-Werror",
    "-Wall",
    "-Wextra",
    "-Wno-unused-parameter",
    "-Wno-unused-variable",
    "-Wno-unused-function",
    "-Wno-unused-but-set-variable",
    "-Wno-missing-field-initializers",
    "-Wno-sign-compare",
    "-Wno-format-truncation",
    "-Wno-format-overflow",
    "-Wno-format-extra-args",
    "-Wno-format-zero-length",
    "-Wno-maybe-uninitialized",
    "-Wno-implicit-fallthrough",
    "-Wno-pointer-to-int-cast",
}

local asm_flags = {
    "-f elf32"
}

local ldflags = {
    "-m elf_i386"
}

local srcfiles = sources(fab.glob("**/*.{c,asm}"))

local compiler = cc.get_gcc()
local assembler = nasm.get_nasm()
local linker = ld.get_linker()

assert(compiler, "failed to find viable compiler")
assert(assembler, "failed to find viable assembler")
assert(linker, "failed to find viable linker")

local objfiles = generate(srcfiles, {
    c = function(sources) return compiler:generate(sources, cflags, {}) end,
    asm = function(sources) return assembler:generate(sources, asm_flags) end
})

local ld_script = fab.def_source("kernel/linker.ld")

local elf = linker:link("kernel.elf", objfiles, ldflags, ld_script)

local iso_rule = fab.def_rule(
    "iso",
    fab.path_rel("iso.sh") .. " @IN@ @OUT@",
    "generating the iso"
)

iso_rule:build("floppaos.iso", { elf }, {})
