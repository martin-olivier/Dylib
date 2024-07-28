/**
 * @file symbols.cpp
 * 
 * @author Martin Olivier <martin.olivier@live.fr>
 * @copyright (c) 2024 Martin Olivier
 *
 * This library is released under MIT license
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>
#include <string>
#include <algorithm>

std::string get_demangled_name(const char *symbol);

#if (defined(_WIN32) || defined(_WIN64))

#include <windows.h>
#include <tchar.h>

std::vector<std::string> get_symbols(HMODULE hModule, bool demangle) {
    std::vector<std::string> result;

    // Get the DOS header
    PIMAGE_DOS_HEADER pDosHeader = (PIMAGE_DOS_HEADER)hModule;
    if (pDosHeader->e_magic != IMAGE_DOS_SIGNATURE)
        throw std::string("Invalid DOS header");

    // Get the NT headers
    PIMAGE_NT_HEADERS pNTHeaders = (PIMAGE_NT_HEADERS)((BYTE*)hModule + pDosHeader->e_lfanew);
    if (pNTHeaders->Signature != IMAGE_NT_SIGNATURE)
        throw std::string("Invalid NT headers");

    // Get the export directory
    DWORD exportDirRVA = pNTHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    if (exportDirRVA == 0)
        throw std::string("No export directory found");

    PIMAGE_EXPORT_DIRECTORY pExportDir = (PIMAGE_EXPORT_DIRECTORY)((BYTE*)hModule + exportDirRVA);

    // Get the list of exported function names
    DWORD* pNames = (DWORD*)((BYTE*)hModule + pExportDir->AddressOfNames);
    DWORD* pFunctions = (DWORD*)((BYTE*)hModule + pExportDir->AddressOfFunctions);
    WORD* pNameOrdinals = (WORD*)((BYTE*)hModule + pExportDir->AddressOfNameOrdinals);

    for (DWORD i = 0; i < pExportDir->NumberOfNames; ++i) {
        const char* name = (const char*)((BYTE*)hModule + pNames[i]);

        if (!name)
            continue;

        if (strcmp(name, "") == 0)
            continue;

        if (demangle) {
            std::string demangled = get_demangled_name(name);
            if (!demangled.empty()) {
                if (std::find(result.begin(), result.end(), name) == result.end())
                    result.push_back(demangled);
                continue;
            }
        }

        if (std::find(result.begin(), result.end(), name) != result.end())
            continue;

        result.push_back(name);
    }

    return result;
}

#elif (defined(__APPLE__) && defined(__MACH__))

#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach-o/fat.h>
#include <fcntl.h>
#include <unistd.h>

std::vector<std::string> get_symbols_at_off(int fd, bool demangle, off_t offset, bool is_64_bit) {
    std::vector<std::string> result;

    lseek(fd, offset, SEEK_SET);

    struct mach_header_64 mh64;
    struct mach_header mh;

    if (is_64_bit)
        read(fd, &mh64, sizeof(mh64));
    else
        read(fd, &mh, sizeof(mh));

    uint32_t ncmds = is_64_bit ? mh64.ncmds : mh.ncmds;
    off_t load_commands_offset = is_64_bit ? sizeof(struct mach_header_64) : sizeof(struct mach_header);
    lseek(fd, offset + load_commands_offset, SEEK_SET);

    for (uint32_t i = 0; i < ncmds; i++) {
        struct load_command lc;
        read(fd, &lc, sizeof(lc));
        off_t current_command_offset = lseek(fd, 0, SEEK_CUR);

        if (lc.cmd == LC_SYMTAB) {
            struct symtab_command symtab;
            lseek(fd, current_command_offset - sizeof(lc), SEEK_SET);
            read(fd, &symtab, sizeof(symtab));

            struct nlist_64 *symbols64 = NULL;
            struct nlist *symbols = NULL;
            if (is_64_bit) {
                symbols64 = reinterpret_cast<struct nlist_64 *>(malloc(symtab.nsyms * sizeof(struct nlist_64)));
                if (symbols64 == nullptr)
                    throw std::bad_alloc();

                lseek(fd, offset + symtab.symoff, SEEK_SET);
                read(fd, symbols64, symtab.nsyms * sizeof(struct nlist_64));
            } else {
                symbols = reinterpret_cast<struct nlist *>(malloc(symtab.nsyms * sizeof(struct nlist)));
                if (symbols == nullptr)
                    throw std::bad_alloc();

                lseek(fd, offset + symtab.symoff, SEEK_SET);
                read(fd, symbols, symtab.nsyms * sizeof(struct nlist));
            }

            char *strtab = reinterpret_cast<char *>(malloc(symtab.strsize));
            if (strtab == nullptr)
                throw std::bad_alloc();

            lseek(fd, offset + symtab.stroff, SEEK_SET);
            read(fd, strtab, symtab.strsize);

            for (uint32_t j = 0; j < symtab.nsyms; j++) {
                uint32_t strx;
                if (is_64_bit)
                    strx = symbols64[j].n_un.n_strx;
                else
                    strx = symbols[j].n_un.n_strx;

                const char *name = &strtab[strx];
                if (!name)
                    continue;

                if (strcmp(name, "") == 0)
                    continue;

                if (demangle) {
                    std::string demangled = get_demangled_name(name);
                    if (!demangled.empty()) {
                        if (std::find(result.begin(), result.end(), name) == result.end())
                            result.push_back(demangled);
                        continue;
                    }
                }

                if (name[0] == '_')
                    name++;
                if (std::find(result.begin(), result.end(), name) != result.end())
                    continue;
                result.push_back(name);
            }

            free(symbols64);
            free(symbols);
            free(strtab);
        }

        lseek(fd, current_command_offset + lc.cmdsize - sizeof(lc), SEEK_SET);
    }

    return result;
}

std::vector<std::string> get_symbols(int fd, bool demangle) {
    std::vector<std::string> result;
    std::vector<std::string> tmp;
    uint32_t magic;

    lseek(fd, 0, SEEK_SET);
    read(fd, &magic, sizeof(magic));
    lseek(fd, 0, SEEK_SET);

    if (magic == FAT_MAGIC || magic == FAT_CIGAM) {
        struct fat_header fat_header;
        read(fd, &fat_header, sizeof(fat_header));

        struct fat_arch *fat_arches = reinterpret_cast<struct fat_arch *>(malloc(sizeof(struct fat_arch) * ntohl(fat_header.nfat_arch)));
        if (fat_arches == nullptr)
            throw std::bad_alloc();
        read(fd, fat_arches, sizeof(struct fat_arch) * ntohl(fat_header.nfat_arch));

        for (uint32_t i = 0; i < ntohl(fat_header.nfat_arch); i++) {
            tmp = get_symbols_at_off(
                fd,
                demangle,
                ntohl(fat_arches[i].offset),
                ntohl(fat_arches[i].cputype) == CPU_TYPE_X86_64);
            std::move(tmp.begin(), tmp.end(), std::back_inserter(result));
        }

        free(fat_arches);
    } else if (magic == MH_MAGIC_64 || magic == MH_CIGAM_64) {
        result = get_symbols_at_off(fd, demangle, 0, true);
    } else if (magic == MH_MAGIC || magic == MH_CIGAM) {
        result = get_symbols_at_off(fd, demangle, 0, false);
    } else {
        throw std::string("Unsupported file format");
    }

    return result;
}

#else  // Linux

#include <libelf.h>
#include <gelf.h>
#include <fcntl.h>
#include <unistd.h>

std::vector<std::string> get_symbols(int fd, bool demangle) {
    std::vector<std::string> result;

    if (elf_version(EV_CURRENT) == EV_NONE)
        throw std::string("ELF library initialization failed");

    Elf *elf = elf_begin(fd, ELF_C_READ, NULL);
    if (!elf)
        throw std::string("elf_begin() failed");

    size_t shstrndx;
    if (elf_getshdrstrndx(elf, &shstrndx) != 0) {
        elf_end(elf);
        throw std::string("elf_getshdrstrndx() failed");
    }

    Elf_Scn *scn = NULL;
    GElf_Shdr shdr;
    while ((scn = elf_nextscn(elf, scn)) != NULL) {
        if (gelf_getshdr(scn, &shdr) != &shdr) {
            elf_end(elf);
            throw std::string("gelf_getshdr() failed");
        }

        if (shdr.sh_type == SHT_SYMTAB || shdr.sh_type == SHT_DYNSYM) {
            Elf_Data *data = NULL;
            data = elf_getdata(scn, data);
            if (!data) {
                elf_end(elf);
                throw std::string("elf_getdata() failed");
            }

            int count = shdr.sh_size / shdr.sh_entsize;
            for (int i = 0; i < count; i++) {
                GElf_Sym sym;
                if (!gelf_getsym(data, i, &sym)) {
                    elf_end(elf);
                    throw std::string("gelf_getsym() failed");
                }

                const char *name = elf_strptr(elf, shdr.sh_link, sym.st_name);
                if (!name)
                    continue;

                if (strcmp(name, "") == 0)
                    continue;

                if (demangle) {
                    std::string demangled = get_demangled_name(name);
                    if (!demangled.empty()) {
                        if (std::find(result.begin(), result.end(), name) == result.end())
                            result.push_back(demangled);
                        continue;
                    }
                }

                if (std::find(result.begin(), result.end(), name) != result.end())
                    continue;
                result.push_back(name);
            }
        }
    }

    elf_end(elf);

    return result;
}

#endif