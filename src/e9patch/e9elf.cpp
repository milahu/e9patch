/*
 * e9elf.cpp
 * Copyright (C) 2021 National University of Singapore
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <utility>

#include "e9alloc.h"
#include "e9elf.h"
#include "e9loader.h"
#include "e9mapping.h"
#include "e9patch.h"

static const
#include "e9loader_elf.c"

/*
 * Patch refactoring for the dynamic loader.
 */
struct Refactor
{
    intptr_t addr;                      // Mapping address
    size_t size;                        // Mapping size
    struct
    {
        off_t offset;                   // Original offset
    } original;
    struct
    {
        off_t offset;                   // Patched offset
    } patched;

    Refactor(intptr_t addr, off_t offset, size_t size) :
        addr(addr), size(size)
    {
        original.offset = offset;
        patched.offset  = 0;
    }
};
typedef std::vector<Refactor> RefactorSet;

/*
 * Parse the ELF file & reserve any occupied address space.
 */
bool parseElf(Binary *B)
{
    const char *filename = B->filename;
    uint8_t *data = B->patched.bytes;
    size_t size = B->size;
    Mode mode = B->mode;
    ElfInfo &info = B->elf;

    if (size < sizeof(Elf64_Ehdr))
        error("failed to parse ELF EHDR from file \"%s\"; file is too small",
            filename);
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)data;

    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
            ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
            ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
            ehdr->e_ident[EI_MAG3] != ELFMAG3)
        error("failed to parse ELF file \"%s\"; invalid magic number",
            filename);
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64)
        error("failed to parse ELF file \"%s\"; file is not 64bit",
            filename);
    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB)
        error("failed to parse ELF file \"%s\"; file is not little endian",
            filename);
    if (ehdr->e_ident[EI_VERSION] != EV_CURRENT)
        error("failed to parse ELF file \"%s\"; invalid version",
            filename);
    if (ehdr->e_machine != EM_X86_64)
        error("failed to parse ELF file \"%s\"; file is not x86_64",
            filename);
    if (ehdr->e_phoff < sizeof(Elf64_Ehdr) || ehdr->e_phoff >= size)
        error("failed to parse ELF file \"%s\"; invalid program header "
            "offset", filename);
    if (ehdr->e_phnum > PN_XNUM)
        error("failed to parse ELF file \"%s\"; too many program headers",
            filename);
    if (ehdr->e_phoff + ehdr->e_phnum * sizeof(Elf64_Phdr) > size)
        error("failed to parse ELF file \"%s\"; invalid program headers",
            filename);

    bool pic = false, pie = false;
    switch (ehdr->e_type)
    {
        case ET_EXEC:
        {
            if (mode == MODE_ELF_DSO)
                error("failed to parse ELF file \"%s\": file is an "
                    "executable and not a shared object", filename);
            if (!reserve(B, 0x0, 0x10000))
                error("failed to reserve low-address range");
            break;
        }
        case ET_DYN:
            pic = true;
            pie = (mode == MODE_ELF_EXE);
            break;
        default:
            error("failed to parse ELF file \"%s\"; file is not executable",
                filename);
    }
    if (!pie)
    {
        // Only PIEs can use the negative address range.  Other PIC such
        // as shared objects cannot use this range since the dynamic
        // linker tends to use it for other libraries.
        if (!reserve(B, RELATIVE_ADDRESS_MIN, 0x0))
            error("failed to reserve negative-address range");
    }

    Elf64_Phdr *phdrs = (Elf64_Phdr *)(data + ehdr->e_phoff);
    Elf64_Phdr *phdr_note = nullptr, *phdr_gnu_relro = nullptr,
        *phdr_gnu_stack = nullptr, *phdr_dynamic = nullptr;
    for (unsigned i = 0; i < ehdr->e_phnum; i++)
    {
        Elf64_Phdr *phdr = phdrs + i;
        switch (phdr->p_type)
        {
            case PT_LOAD:
            {
                intptr_t vstart = (intptr_t)phdr->p_vaddr;
                intptr_t vend   = vstart + phdr->p_memsz;
                if (!reserve(B, vstart, vend))
                    error("failed to reserve address space range %p..%p",
                        vstart, vend);
                break;
            }
            case PT_DYNAMIC:
                phdr_dynamic = phdr;
                break;
            case PT_NOTE:
                phdr_note = phdr;
                break;
            case PT_GNU_RELRO:
                phdr_gnu_relro = phdr;
                break;
            case PT_GNU_STACK:
                phdr_gnu_stack = phdr;
                break;
        }
    }
    if (phdr_dynamic != nullptr &&
            phdr_dynamic->p_offset + phdr_dynamic->p_memsz > size)
        error("failed to parse ELF file \"%s\": invalid dynamic section",
            filename);
    info.ehdr           = ehdr;
    info.phdr_note      = phdr_note;
    info.phdr_gnu_relro = phdr_gnu_relro;
    info.phdr_gnu_stack = phdr_gnu_stack;
    info.phdr_dynamic   = phdr_dynamic;

    return pic;
}

/*
 * Refactor out the patched pages & restore the original pages.
 * For some programs/libraries, it is difficult to ensure the loader is
 * run before the code segment is executed.  This is especially difficult
 * with some advanced uses of the dynamic linker.  This refactoring provides
 * a simple solution: have the loader also patch the code.
 */
static size_t emitRefactoredPatch(const uint8_t *original, uint8_t *data,
    size_t size, size_t mapping_size, const InstrSet &Is,
    RefactorSet &refactors)
{
    if (option_loader_static)
        return 0;

    assert(size % PAGE_SIZE == 0);

    // Step #1: Find refactorings:
    intptr_t curr_addr   = INTPTR_MIN;
    off_t    curr_offset = -1;
    size_t   curr_size   = 0;
    for (off_t offset = 0; offset < (off_t)size; offset += PAGE_SIZE)
    {
        if (memcmp(original + offset, data + offset, PAGE_SIZE) == 0)
            continue;
        auto i = Is.lower_bound(offset);
        assert(i != Is.end());
        const Instr *I = i->second;
        intptr_t page_addr   = I->addr - (I->addr % PAGE_SIZE);
        off_t    page_offset = I->offset - (I->offset % PAGE_SIZE);
        assert(page_offset == offset);
 
        if (curr_addr < 0 || page_addr < curr_addr ||
                (intptr_t)(curr_addr + curr_size + mapping_size) < page_addr)
        {
            if (curr_addr >= 0)
            {
                Refactor r(curr_addr, curr_offset, curr_size);
                refactors.push_back(r);
            }
            curr_addr   = page_addr;
            curr_offset = page_offset;
            curr_size   = PAGE_SIZE;
        }
        else
            curr_size += (page_addr + PAGE_SIZE) - (curr_addr + curr_size);
    }
    if (curr_addr >= 0)
    {
        Refactor r(curr_addr, curr_offset, curr_size);
        refactors.push_back(r);
    }

    // Step #2: Write out a copy of the patched pages & restore original pages:
    size_t size_0 = size;
    for (auto &r: refactors)
    {
        r.patched.offset = (off_t)size;
        memcpy(data + size, data + r.original.offset, r.size);
        memcpy(data + r.original.offset, original + r.original.offset, r.size);
        size += r.size;
    }

    return size - size_0;
}

/*
 * Emit a mapping.
 */
size_t emitLoaderMap(uint8_t *data, intptr_t addr, size_t len, off_t offset,
    bool r, bool w, bool x, intptr_t *ub)
{
    bool abs = IS_ABSOLUTE(addr);
    if (ub != nullptr && !abs)
        *ub = std::max(*ub, addr);
    addr = BASE_ADDRESS(addr);

    size_t size = 0;
    struct e9_map_s *map = (struct e9_map_s *)data;
    size += sizeof(struct e9_map_s);

    addr   /= (intptr_t)PAGE_SIZE;
    len    /= PAGE_SIZE;
    offset /= PAGE_SIZE;

    if (addr < INT32_MIN || addr > INT32_MAX)
        error("mapping address (" ADDRESS_FORMAT ") %sflow detected",
            ADDRESS(addr), (addr < 0? "under": "over"));
    if (len >= (1 << 21))
        error("mapping size (%zu) overflow detected", len);
    if (offset > UINT32_MAX)
        error("mapping offset (%+zd) overflow detected", offset);

    map->addr   = (int32_t)addr;
    map->offset = (uint32_t)offset;
    map->size   = (uint16_t)len;
    map->r      = (r? 1: 0);
    map->w      = (w? 1: 0);
    map->x      = (x? 1: 0);
    map->abs    = (abs? 1: 0);

    return size;
}

/*
 * Emit the (modified) ELF binary.
 */
size_t emitElf(Binary *B, const MappingSet &mappings, size_t mapping_size)
{
    uint8_t *data = B->patched.bytes;
    size_t size = B->patched.size;

    // Step (1): Round-up to nearest page boundary (zero-fill)
    stat_input_file_size = size;
    size = (size % PAGE_SIZE == 0? size:
        size + PAGE_SIZE - (size % PAGE_SIZE));

    // Step (2): Refactor the patching (if necessary):
    RefactorSet refactors;
    size += emitRefactoredPatch(B->original.bytes, data, size, mapping_size,
        B->Is, refactors);
 
    // Step (3): Emit all mappings:
    B->config = option_loader_base;
    for (auto *mapping: mappings)
    {
        uint8_t *base = data + size;
        mapping->offset = (off_t)size;
        flattenMapping(B, base, mapping, /*int3=*/0xcc);
        size += mapping->size;
    }

    // Step (4): Emit the loader:
    size = (size % PAGE_SIZE == 0? size:
        size + PAGE_SIZE - (size % PAGE_SIZE));
    struct e9_config_s *config = (struct e9_config_s *)(data + size);
    size_t config_offset = size;
    size += sizeof(struct e9_config_s);
    struct e9_config_elf_s *config_elf =
        (struct e9_config_elf_s *)(data + size);
    size += sizeof(struct e9_config_elf_s);
    const char magic[] = "E9PATCH";
    memcpy(config->magic, magic, sizeof(magic));
    config->base = option_loader_base;
    if (B->mmap != INTPTR_MIN)
        config->mmap = B->mmap;

    config->inits = (uint32_t)(size - config_offset);
    for (auto init: B->inits)
    {
        memcpy(data + size, &init, sizeof(init));
        size += sizeof(init);
        config->num_inits++;
    }
    std::vector<Bounds> bounds;
    intptr_t ub = INTPTR_MIN;
    for (unsigned i = 0; i < 2; i++)
    {
        config->maps[i] = (uint32_t)(size - config_offset);
        bool preload = (i == 0);
        for (auto *mapping: mappings)
        {
            if (preload)
                stat_num_physical_bytes += mapping->size;
            off_t offset_0 = mapping->offset;
            for (; mapping != nullptr; mapping = mapping->merged)
            {
                if (mapping->preload != preload)
                    continue;
                bounds.clear();
                getVirtualBounds(mapping, PAGE_SIZE, bounds);
                bool r = ((mapping->prot & PROT_READ) != 0);
                bool w = ((mapping->prot & PROT_WRITE) != 0);
                bool x = ((mapping->prot & PROT_EXEC) != 0);
                for (const auto b: bounds)
                {
                    intptr_t base = mapping->base + b.lb;
                    size_t len    = b.ub - b.lb;
                    off_t offset  = offset_0 + b.lb;

                    debug("load trampoline: mmap(addr=" ADDRESS_FORMAT
                        ",size=%zu,offset=+%zu,prot=%c%c%c)",
                        ADDRESS(base), len, offset_0, (r? 'r': '-'),
                        (w? 'w': '-'), (x? 'x': '-'));
                    stat_num_virtual_bytes += len;

                    size += emitLoaderMap(data + size, base, len, offset,
                        r, w, x, &ub);
                    config->num_maps[i]++;
                }
            }
        }
    }
    if (ub > option_loader_base)
    {
        // This error may occur if the front-end changes `--loader-base'
        // mid-way through the patching process.  It is easiest to detect
        // the error here than earlier.
        error("loader base address (0x%lx) (see `--loader-base') must not "
            "exceed maximum mapping address (0x%lx) (see `--mem-ub')",
            option_loader_base, ub);
    }
    for (const auto &refactor: refactors)
    {
        debug("load refactoring: mmap(" ADDRESS_FORMAT ", %zu, "
            "PROT_READ | PROT_WRITE | 0, MAP_FIXED | MAP_PRIVATE, fd, +%zd)",
            ADDRESS(refactor.addr), refactor.size, refactor.patched.offset);
        size += emitLoaderMap(data + size, refactor.addr, refactor.size,
            refactor.patched.offset, /*r=*/true, /*w=*/false, /*x=*/true,
            nullptr);
        config->num_maps[1]++;
    }
    intptr_t entry = (option_loader_base + (size - config_offset));
    if (option_trap_entry)
        data[size++] = /*int3=*/0xCC;
    switch (B->mode)
    {
        case MODE_ELF_EXE:
            // mov (%rsp),%rdi
            // lea 0x8(%rsp),%rsi
            data[size++] = 0x48; data[size++] = 0x8B; data[size++] = 0x3C;
            data[size++] = 0x24;
            data[size++] = 0x48; data[size++] = 0x8D; data[size++] = 0x74;
            data[size++] = 0x24; data[size++] = 0x08;
            break;
        case MODE_ELF_DSO:
            // xorq %edi, %edi
            // xorq %esi, %esi
            data[size++] = 0x31; data[size++] = 0xFF;
            data[size++] = 0x31; data[size++] = 0xF6;
            break;
        default:
            error("invalid mode");
    }
    // lea config(%rip), %rdx
    data[size++] = 0x48; data[size++] = 0x8D; data[size++] = 0x15;
    int32_t config_rel32 =
        -(int32_t)((size + sizeof(int32_t)) - config_offset);
    memcpy(data + size, &config_rel32, sizeof(config_rel32));
    size += sizeof(config_rel32);
    memcpy(data + size, e9loader_elf_bin, sizeof(e9loader_elf_bin));
    size += sizeof(e9loader_elf_bin);
    size_t config_size = size - config_offset;
    config->size = (uint32_t)(config_size % PAGE_SIZE == 0? config_size:
        config_size + PAGE_SIZE - (config_size % PAGE_SIZE));

    // Step (5): Modify the entry address.
    Elf64_Phdr *phdr = B->elf.phdr_dynamic;
    intptr_t dynamic = -1;
    if (phdr != nullptr)
        config_elf->dynamic = dynamic = (intptr_t)phdr->p_vaddr;
    switch (B->mode)
    {
        case MODE_ELF_EXE:
        {
            Elf64_Ehdr *ehdr = B->elf.ehdr;
            config->entry = (intptr_t)B->elf.ehdr->e_entry;
            ehdr->e_entry = (Elf64_Addr)entry;
            config->flags |= E9_FLAG_EXE;
            break;
        }
        case MODE_ELF_DSO:
        {
            if (phdr == nullptr)
                error("failed to replace DT_INIT entry; missing PT_DYNAMIC "
                    "program header");
            Elf64_Dyn *dynamic = (Elf64_Dyn *)(data + phdr->p_offset);
            size_t num_dynamic = phdr->p_memsz / sizeof(Elf64_Dyn);
            bool found = false;
            for (size_t i = 0; !found && i < num_dynamic; i++)
            {
                if (dynamic[i].d_tag == DT_NULL)
                    break;
                if (dynamic[i].d_tag == DT_INIT)
                {
                    found = true;
                    config->entry = (intptr_t)dynamic[i].d_un.d_ptr;
                    dynamic[i].d_un.d_ptr = (Elf64_Addr)entry;
                }
            }
            if (!found)
                error("failed to replace DT_INIT entry; entry was not found");
            break;
        }
        default:
            error("invalid mode");
    }

    // Step (6): Modify the PHDR to load the loader.
    // NOTE: Currently we use the well-known and easy-to-implement PT_NOTE
    //       (or PT_GNU_*) injection method to load the loader.  Some
    //       alternative methods may also work, but are not yet implemented.
    const char *phdr_str = "PT_NOTE, PT_GNU_RELRO, or PT_GNU_STACK";
    switch (option_loader_phdr)
    {
        case PT_NOTE:
            phdr_str = "PT_NOTE";
            phdr = B->elf.phdr_note; break;
        case PT_GNU_RELRO:
            phdr_str = "PT_GNU_RELRO";
            phdr = B->elf.phdr_gnu_relro; break;
        case PT_GNU_STACK:
            phdr_str = "PT_GNU_STACK";
            phdr = B->elf.phdr_gnu_stack; break;
        default:
            phdr = B->elf.phdr_note;
            phdr = (phdr == nullptr? B->elf.phdr_gnu_relro: phdr);
            phdr = (phdr == nullptr? B->elf.phdr_gnu_stack: phdr);
            break;
    }
    if (phdr == nullptr)
        error("failed to replace PHDR entry; missing %s segment",
            phdr_str);
    phdr->p_type   = PT_LOAD;
    phdr->p_flags  = PF_X | PF_R;
    phdr->p_offset = config_offset;
    phdr->p_vaddr  = (Elf64_Addr)option_loader_base;
    phdr->p_paddr  = (Elf64_Addr)nullptr;
    phdr->p_filesz = config_size;
    phdr->p_memsz  = config_size;
    phdr->p_align  = PAGE_SIZE;

    stat_output_file_size = size;

    if (option_mem_rebase_set)
        warning("ignoring `--mem-rebase' option for Linux ELF binary");

    return size;
}

