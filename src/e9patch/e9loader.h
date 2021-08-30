/*
 * e9loader.h
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

#ifndef __E9LOADER_H
#define __E9LOADER_H

#define E9_FLAG_EXE                 0x1

struct e9_map_s
{
    int32_t  addr;                  // Address (/ PAGE_SIZE)
    uint32_t offset;                // Offset  (/ PAGE_SIZE)
    uint32_t size:20;               // Size    (/ PAGE_SIZE)
    uint32_t __reserved:8;          // Reserved
    uint32_t r:1;                   // Read?
    uint32_t w:1;                   // Write?
    uint32_t x:1;                   // Execute?
    uint32_t abs:1;                 // Absolute?
};

struct e9_config_s
{
    char     magic[8];              // "E9PATCH\0"
    uint32_t flags;                 // Flags
    uint32_t size;                  // Loader total size
    intptr_t base;                  // Loader base address
    intptr_t entry;                 // Real entry point
    intptr_t dynamic;               // DYNAMIC, or 0x0
    intptr_t mmap;                  // mmap(), or 0x0
    uint32_t num_maps[2];           // # Mappings
    uint32_t maps[2];               // Mappings offset
    uint32_t num_inits;             // # Init functions
    uint32_t inits;                 // Init functions offset
};

#endif
