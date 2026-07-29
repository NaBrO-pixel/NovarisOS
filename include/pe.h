#ifndef PE_H
#define PE_H

#include <stdint.h>

/* PE32 (.exe) image loading.
 *
 * Milestone 8b got a PE image's bytes into memory and jumped to its entry
 * point, but deliberately refused anything with a non-empty import
 * directory - which is every real Windows binary - because there was no
 * Win32 API underneath to import *from*. Milestone 10 added that API
 * (include/win32.h), so this file's job grew accordingly. It now does the
 * things a real loader does:
 *
 *   - reserves the whole SizeOfImage range in one piece and maps the
 *     headers at the image base, the way Windows does, so a program that
 *     walks its own PE headers through GetModuleHandle() finds them;
 *   - honors ImageBase when the address range is available and applies
 *     the base relocation table when it isn't (or when the image is
 *     /DYNAMICBASE and asked to move);
 *   - resolves the import directory - by name and by ordinal, including
 *     bound-import tables - through a caller-supplied resolver, so the
 *     loader itself stays independent of what's providing the DLLs
 *     (kernel/win32.c in the kernel, a test double in tests/);
 *   - sets up the TLS directory's index slot and runs its
 *     DLL_PROCESS_ATTACH callbacks.
 *
 * What it still does not do: PE32+ (64-bit) images, .NET/CLR images,
 * loading real DLL files as separate modules (LoadLibrary is emulated,
 * not a real loader - see win32.h), forwarded exports, and delay-import
 * descriptors (those go through the CRT's own helper, which calls the
 * emulated LoadLibrary/GetProcAddress, so they work by a different
 * route). pe_inspect() names each of these conditions specifically so
 * `run` and `peinfo` can say what is wrong rather than "malformed". */

#define IMAGE_DOS_SIGNATURE 0x5A4D      /* "MZ" */
#define IMAGE_NT_SIGNATURE  0x00004550u /* "PE\0\0" */

#define IMAGE_FILE_MACHINE_I386  0x14c
#define IMAGE_FILE_MACHINE_AMD64 0x8664
#define IMAGE_FILE_MACHINE_ARM64 0xAA64

#define IMAGE_NT_OPTIONAL_HDR32_MAGIC 0x10b
#define IMAGE_NT_OPTIONAL_HDR64_MAGIC 0x20b

#define IMAGE_FILE_DLL             0x2000
#define IMAGE_FILE_RELOCS_STRIPPED 0x0001

#define IMAGE_SUBSYSTEM_NATIVE      1
#define IMAGE_SUBSYSTEM_WINDOWS_GUI 2
#define IMAGE_SUBSYSTEM_WINDOWS_CUI 3

typedef struct {
    uint16_t e_magic;
    uint8_t  e_ignore1[58]; /* rest of the DOS header we don't care about */
    uint32_t e_lfanew;      /* file offset to the PE signature */
} __attribute__((packed)) dos_header_t;

typedef struct {
    uint16_t Machine;
    uint16_t NumberOfSections;
    uint32_t TimeDateStamp;
    uint32_t PointerToSymbolTable;
    uint32_t NumberOfSymbols;
    uint16_t SizeOfOptionalHeader;
    uint16_t Characteristics;
} __attribute__((packed)) coff_header_t;

typedef struct {
    uint32_t VirtualAddress;
    uint32_t Size;
} __attribute__((packed)) data_directory_t;

#define IMAGE_DIRECTORY_ENTRY_EXPORT         0
#define IMAGE_DIRECTORY_ENTRY_IMPORT         1
#define IMAGE_DIRECTORY_ENTRY_RESOURCE       2
#define IMAGE_DIRECTORY_ENTRY_BASERELOC      5
#define IMAGE_DIRECTORY_ENTRY_TLS            9
#define IMAGE_DIRECTORY_ENTRY_IAT           12
#define IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT  13
#define IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR 14
#define IMAGE_NUMBEROF_DIRECTORY_ENTRIES    16

typedef struct {
    uint16_t Magic;
    uint8_t  MajorLinkerVersion;
    uint8_t  MinorLinkerVersion;
    uint32_t SizeOfCode;
    uint32_t SizeOfInitializedData;
    uint32_t SizeOfUninitializedData;
    uint32_t AddressOfEntryPoint;
    uint32_t BaseOfCode;
    uint32_t BaseOfData;
    uint32_t ImageBase;
    uint32_t SectionAlignment;
    uint32_t FileAlignment;
    uint16_t MajorOperatingSystemVersion;
    uint16_t MinorOperatingSystemVersion;
    uint16_t MajorImageVersion;
    uint16_t MinorImageVersion;
    uint16_t MajorSubsystemVersion;
    uint16_t MinorSubsystemVersion;
    uint32_t Win32VersionValue;
    uint32_t SizeOfImage;
    uint32_t SizeOfHeaders;
    uint32_t CheckSum;
    uint16_t Subsystem;
    uint16_t DllCharacteristics;
    uint32_t SizeOfStackReserve;
    uint32_t SizeOfStackCommit;
    uint32_t SizeOfHeapReserve;
    uint32_t SizeOfHeapCommit;
    uint32_t LoaderFlags;
    uint32_t NumberOfRvaAndSizes;
    data_directory_t DataDirectory[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
} __attribute__((packed)) optional_header32_t;

/* PE32+ differs from PE32 from ImageBase onward (64-bit fields), but we
 * only ever need enough of it to say "this is a 64-bit binary" - the
 * Magic word at offset 0 already does that, so there's no second struct. */

typedef struct {
    uint8_t  Name[8];
    uint32_t VirtualSize;
    uint32_t VirtualAddress;
    uint32_t SizeOfRawData;
    uint32_t PointerToRawData;
    uint32_t PointerToRelocations;
    uint32_t PointerToLinenumbers;
    uint16_t NumberOfRelocations;
    uint16_t NumberOfLinenumbers;
    uint32_t Characteristics;
} __attribute__((packed)) section_header_t;

#define IMAGE_SCN_MEM_EXECUTE 0x20000000u
#define IMAGE_SCN_MEM_READ    0x40000000u
#define IMAGE_SCN_MEM_WRITE   0x80000000u

/* IMAGE_IMPORT_DESCRIPTOR, 20 bytes. The import directory is an array of
 * these terminated by one that's all zero. */
typedef struct {
    uint32_t OriginalFirstThunk; /* RVA of the import name table (may be 0) */
    uint32_t TimeDateStamp;
    uint32_t ForwarderChain;
    uint32_t Name;               /* RVA of the DLL's name */
    uint32_t FirstThunk;         /* RVA of the IAT to write addresses into */
} __attribute__((packed)) import_descriptor_t;

#define IMAGE_ORDINAL_FLAG32 0x80000000u

/* Base relocation block header, followed by (SizeOfBlock - 8) / 2 16-bit
 * entries, each (type << 12) | offset-within-the-4KB-page. */
typedef struct {
    uint32_t VirtualAddress;
    uint32_t SizeOfBlock;
} __attribute__((packed)) base_reloc_block_t;

#define IMAGE_REL_BASED_ABSOLUTE 0
#define IMAGE_REL_BASED_HIGH     1
#define IMAGE_REL_BASED_LOW      2
#define IMAGE_REL_BASED_HIGHLOW  3
#define IMAGE_REL_BASED_HIGHADJ  4
#define IMAGE_REL_BASED_DIR64   10

typedef struct {
    uint32_t StartAddressOfRawData;
    uint32_t EndAddressOfRawData;
    uint32_t AddressOfIndex;
    uint32_t AddressOfCallBacks;
    uint32_t SizeOfZeroFill;
    uint32_t Characteristics;
} __attribute__((packed)) tls_directory32_t;

/* Why a load failed. Every value here maps to a specific sentence the
 * shell can print, which is the whole point - "this is a 64-bit binary"
 * and "this image is malformed" are very different things to be told. */
typedef enum {
    PE_OK = 0,
    PE_ERR_NOT_PE,           /* no MZ, or MZ with no PE header (DOS .exe) */
    PE_ERR_BAD_HEADER,       /* headers present but inconsistent/truncated */
    PE_ERR_PE32PLUS,         /* 64-bit image on a 32-bit kernel */
    PE_ERR_BAD_MACHINE,      /* not i386 */
    PE_ERR_IS_DLL,           /* a library, not a program */
    PE_ERR_DOTNET,           /* managed/CLR image - needs a .NET runtime */
    PE_ERR_BAD_SECTION,      /* a section header points outside the file */
    PE_ERR_TOO_LARGE,        /* SizeOfImage beyond what we'll map */
    PE_ERR_OUT_OF_MEMORY,
    PE_ERR_NO_ADDRESS_SPACE, /* wanted an address we can't give it, and it
                              * has no relocation table to be moved with */
    PE_ERR_BAD_IMPORTS,      /* import directory is unreadable */
    PE_ERR_MISSING_IMPORTS,  /* strict mode: some import couldn't resolve */
} pe_load_result_t;

const char* pe_result_string(pe_load_result_t r);

/* Everything `peinfo` prints, and everything pe_load() needs in order to
 * decide how to treat an image. Filled in as far as parsing got even for
 * images that can't run, so the shell can explain why. */
typedef struct {
    int      valid;              /* MZ + PE signatures both present */
    int      is_pe32plus;
    int      is_dll;
    int      is_dotnet;
    int      has_relocs;
    int      relocs_stripped;
    uint16_t machine;
    uint16_t subsystem;
    uint16_t characteristics;
    uint32_t image_base;
    uint32_t size_of_image;
    uint32_t entry_rva;
    uint32_t num_sections;
    uint32_t stack_reserve;
    uint32_t import_dll_count;
    uint32_t import_func_count;
} pe_info_t;

/* Parses headers only - never maps anything. Returns PE_OK if the image
 * is a loadable 32-bit i386 program, or the specific reason it isn't. */
pe_load_result_t pe_inspect(const uint8_t* image, uint32_t size, pe_info_t* out);

/* Cheap signature test used by the shell to pick a loader. True for any
 * MZ+PE image, including ones pe_inspect() goes on to reject - the point
 * is "this is a Windows binary", so that e.g. a 64-bit .exe gets a
 * Windows-specific diagnostic instead of being run as a flat blob. */
int pe_is_valid(const uint8_t* image, uint32_t size);

/* Walks the import directory without loading anything, calling `visit`
 * once per imported symbol (name is 0 for an ordinal import). Used by
 * `peinfo` to show which of a binary's imports would resolve. */
typedef void (*pe_import_visitor_t)(const char* dll, const char* name,
                                    uint16_t ordinal, void* ctx);
pe_load_result_t pe_visit_imports(const uint8_t* image, uint32_t size,
                                  pe_import_visitor_t visit, void* ctx);

/* How the loader gets addresses for imported symbols. Keeping this a
 * callback rather than calling into kernel/win32.c directly is what lets
 * tests/pe_host_test.c exercise this exact code on the build host with a
 * scripted resolver. */
typedef struct pe_loader_ops {
    /* Return an address the image can call, or 0 if this DLL/symbol pair
     * isn't available. `name` is 0 for an import by ordinal. */
    uint32_t (*resolve_import)(const char* dll, const char* name,
                               uint16_t ordinal, void* ctx);
    /* Called only when resolve_import() returned 0 and strict is 0.
     * Returning an address (typically a stub that reports the call at the
     * moment it happens) lets the image load anyway. */
    uint32_t (*missing_import)(const char* dll, const char* name,
                               uint16_t ordinal, void* ctx);
    void* ctx;
    int strict; /* 1 = fail the load if anything is unresolved */
} pe_loader_ops_t;

/* 16MB. Generous for anything this OS can realistically run, and small
 * enough that a corrupt SizeOfImage can't ask us to map a gigabyte. */
#define PE_MAX_IMAGE_PAGES 4096

/* Bookkeeping for one loaded image, so it can be torn down again. */
typedef struct {
    uint32_t base;          /* address actually loaded at */
    uint32_t size;          /* SizeOfImage, rounded up to a page */
    uint32_t entry;         /* absolute entry point */
    uint32_t pages;         /* pages mapped starting at base */
    uint32_t reloc_delta;   /* base - ImageBase */
    uint32_t* saved_ptes;   /* previous PTE per page - see paging.h */
    uint32_t unresolved;    /* imports that fell back to missing_import() */
    uint32_t resolved;      /* imports bound to a real implementation */
    uint16_t subsystem;
    uint32_t stack_reserve;
    uint32_t tls_callbacks; /* absolute address of the TLS callback array */
} pe_image_t;

/* Maps the image, applies relocations if it had to move, binds imports
 * through `ops`, and records the TLS callback list. On PE_OK, `out`
 * describes the loaded image and out->entry is where to start executing.
 * On failure nothing stays mapped. `ops` may be 0, meaning "nothing can
 * be resolved". */
pe_load_result_t pe_load(const uint8_t* image, uint32_t size,
                         const pe_loader_ops_t* ops, pe_image_t* out);

/* Unmaps the image and restores whatever was mapped at those addresses
 * beforehand (see paging.h's note about borrowing the low identity map). */
void pe_unload(pe_image_t* img);

#endif
