// SPDX-FileCopyrightText: 2026 Artem Novak
// SPDX-License-Identifier: BSD-3-Clause
// Standalone SH7305 / Hollyhock-3 diagnostic; no gint dependency.
// Memory/file handling follows NTOS_SH4's P1-resident SDK loader.
#include <appdef.h>
#include <sdk/os/debug.h>
#include <sdk/os/file.h>
#include <sdk/os/mem.h>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdarg>

APP_NAME("SH7305 Probe")
APP_AUTHOR("Artem Novak")
APP_DESCRIPTION("SH7305 CPU, MMU and external ROM probe")
APP_VERSION("1.2.0")

static constexpr uint32_t ROM_SIZE = 0x180000u;
static constexpr uint32_t ROM_MID = 0x080000u;
static constexpr uint32_t ROM_LAST = ROM_SIZE - 1u;
static constexpr char ROM_PATH[] = "\\fls0\\romprobe.bin";
static constexpr char REPORT_PATH[] = "\\fls0\\results.txt";
static constexpr uint32_t UNKNOWN = 0xffffffffu;
static uint8_t read_buffer[4096];
static char report[3500];
static size_t report_length;

__attribute__((used, noinline)) static void probe_code_anchor()
{
    __asm__ volatile("" ::: "memory");
}

static uint32_t reg32(uint32_t address)
{
    return *reinterpret_cast<volatile const uint32_t *>(address);
}

static uint32_t address_of(void const *ptr)
{
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ptr));
}

struct Mapping {
    uint32_t va, pa, p1;
    bool valid;
};

static Mapping map_pointer(void const *ptr)
{
    uint32_t va = address_of(ptr);
    if(va >= 0x80000000u && va < 0xc0000000u) {
        uint32_t pa = va & 0x1fffffffu;
        return {va, pa, pa | 0x80000000u, true};
    }
    // No inference for translated P0 addresses. HH3 does not own an
    // NTOS UTLB mapping table and should not read P4 arrays or flush MMU.
    return {va, UNKNOWN, UNKNOWN, false};
}

struct RomResult {
    uint8_t *resident = nullptr;
    uint32_t bytes = 0;
    uint32_t first_bad = UNKNOWN;
    int open_rc = -99;
    int stat_rc = -99;
    int length_rc = -99;
    int io_rc = 0;
    int close_rc = -99;
    bool opened = false;
    bool size_ok = false;
    bool marks[3] = {false, false, false};
    bool complete = false;
    bool memory_ok = false;
};

static RomResult load_rom()
{
    RomResult r;
    // One SDK file descriptor at a time; close before opening results.txt.
    int fd = File_Open(ROM_PATH, FILE_OPEN_READ);
    r.open_rc = fd;
    if(fd < 0) return r;
    r.opened = true;
    File_Stat stat{};
    r.stat_rc = static_cast<int>(File_Fstat(fd, &stat));
    if(r.stat_rc == 0) {
        r.length_rc = static_cast<int>(stat.fileSize);
        r.size_ok = stat.fileSize == ROM_SIZE;
    }
    if(!r.size_ok) {
        r.close_rc = static_cast<int>(File_Close(fd));
        return r;
    }

    // NTOS loads PE images with Mem_Malloc into physical P1 heap memory.
    // This is a real, contiguous RAM allocation, NOT a 384-entry UTLB map.
    r.resident = static_cast<uint8_t *>(Mem_Malloc(ROM_SIZE));
    r.memory_ok = r.resident && map_pointer(r.resident).valid;
    // Failure to allocate must not prevent the independent file check.
    while(r.bytes < ROM_SIZE) {
        uint32_t count = ROM_SIZE - r.bytes;
        if(count > sizeof(read_buffer)) count = sizeof(read_buffer);
        int n = File_Read(fd, read_buffer, static_cast<int>(count));
        if(n <= 0 || static_cast<uint32_t>(n) > count) {
            r.io_rc = n <= 0 ? n : -2;
            if(n == 0) r.io_rc = -19; // premature EOF
            break;
        }
        for(int i = 0; i < n; ++i) {
            uint32_t offset = r.bytes + static_cast<uint32_t>(i);
            uint8_t expected = offset == 0u ? 0x5au :
                offset == ROM_MID ? 0xa5u :
                offset == ROM_LAST ? 0x3cu : 0u;
            if(read_buffer[i] != expected && r.first_bad == UNKNOWN)
                r.first_bad = offset;
            if(offset == 0u) r.marks[0] = read_buffer[i] == 0x5au;
            if(offset == ROM_MID) r.marks[1] = read_buffer[i] == 0xa5u;
            if(offset == ROM_LAST) r.marks[2] = read_buffer[i] == 0x3cu;
        }
        if(r.memory_ok)
            Mem_Memcpy(r.resident + r.bytes, read_buffer, static_cast<size_t>(n));
        r.bytes += static_cast<uint32_t>(n);
    }
    r.close_rc = static_cast<int>(File_Close(fd));
    r.complete = r.bytes == ROM_SIZE && r.first_bad == UNKNOWN &&
        r.io_rc == 0 && r.close_rc == 0;
    if(!r.complete && r.resident) {
        Mem_Free(r.resident);
        r.resident = nullptr;
        r.memory_ok = false;
    }
    return r;
}

static void appendf(char const *fmt, ...)
{
    if(report_length >= sizeof(report)) return;
    va_list args;
    va_start(args, fmt);
    int n = std::vsnprintf(report + report_length,
        sizeof(report) - report_length, fmt, args);
    va_end(args);
    if(n < 0) return;
    size_t available = sizeof(report) - report_length;
    report_length += static_cast<size_t>(n) < available ?
        static_cast<size_t>(n) : available;
}

static void line_hex(char const *name, uint32_t value, bool available = true)
{
    if(available) appendf("%s %08lX\n", name, value);
    else appendf("%s N/A\n", name);
}

static void sample(char const *name, uint8_t const *ptr)
{
    appendf("\n%s\n", name);
    if(!ptr) {
        appendf("VA N/A\nPA N/A\nP1 N/A\nPAGE bytes N/A\n"
                "VA value N/A\nP1 value N/A\nAlias match: N/A\n");
        return;
    }
    Mapping m = map_pointer(ptr);
    line_hex("VA", m.va);
    line_hex("PA", m.pa, m.valid);
    line_hex("P1", m.p1, m.valid);
    appendf("PAGE bytes N/A\n"); // Direct P1/P2 aliases have no UTLB page.
    line_hex("VA value", *ptr);
    if(m.valid) {
        uint8_t alias = *reinterpret_cast<volatile const uint8_t *>(m.p1);
        line_hex("P1 value", alias);
        appendf("Alias match: %s\n", *ptr == alias ? "yes" : "no");
    }
    else appendf("P1 value N/A\nAlias match: N/A\n");
}

static void build_report(RomResult const &r)
{
    report_length = 0;
    report[0] = 0;
    uint32_t pvr=reg32(0xff000030u);
    uint32_t prr=reg32(0xff000044u);
    uint32_t cvr=reg32(0xff000040u);
    uint32_t cpidr=reg32(0xff000048u);
    uint32_t mmucr=reg32(0xff000010u);
    uint32_t frqcr=reg32(0xa4150000u);
    uint32_t pllcr=reg32(0xa4150024u);
    uint32_t pllfrq=reg32(0xa4150050u);

    appendf("SH7305 PROBE RESULTS\n====================\n\n");
    line_hex("PVR  ",pvr);line_hex("PRR  ",prr);
    line_hex("CVR  ",cvr);line_hex("CPIDR",cpidr);
    appendf("\nUSER RAM\nVA 08100000\nPA N/A\nP1 N/A\n"
            "PAGE bytes N/A\nRAM span bytes N/A\n");

    appendf("\nSH7305 CLOCKS (raw)\n");
    line_hex("FRQCR ",frqcr);line_hex("PLLCR ",pllcr);
    line_hex("PLLFRQ",pllfrq);
    appendf("Frequency Hz: N/A\n");

    appendf("\nADDIN CODE\n");
    Mapping code = map_pointer(reinterpret_cast<void const *>(
        static_cast<uintptr_t>(reinterpret_cast<uintptr_t>(&probe_code_anchor))));
    line_hex("VA",code.va);
    line_hex("PA",code.pa,code.valid);
    line_hex("P1",code.p1,code.valid);
    appendf("PAGE bytes N/A\n");
    auto value=*reinterpret_cast<volatile const uint16_t *>(code.va);
    line_hex("VA value",value);
    if(code.valid) {
        uint16_t alias=*reinterpret_cast<volatile const uint16_t *>(code.p1);
        line_hex("P1 value",alias);
        appendf("Alias match: %s\n",value==alias?"yes":"no");
    }
    else appendf("P1 value N/A\nAlias match: N/A\n");

    sample("ROM 1.5M START",r.memory_ok?r.resident:nullptr);
    sample("ROM 1.5M MID",r.memory_ok?r.resident+ROM_MID:nullptr);
    sample("ROM 1.5M END",r.memory_ok?r.resident+ROM_LAST:nullptr);

    // P1 is a physical linear alias. The 1.5 MiB malloc region is a
    // contiguous RAM block even though the original flash file needn't be.
    bool mapped = r.memory_ok &&
        map_pointer(r.resident).valid &&
        map_pointer(r.resident+ROM_MID).valid &&
        map_pointer(r.resident+ROM_LAST).valid;
    bool contiguous = false;
    if(mapped) {
        uint32_t first=map_pointer(r.resident).pa;
        uint32_t mid=map_pointer(r.resident+ROM_MID).pa;
        uint32_t last=map_pointer(r.resident+ROM_LAST).pa;
        contiguous = mid-first == ROM_MID && last-first == ROM_LAST;
    }
    appendf("\nLINUX ROM TEST\n");
    line_hex("ROM bytes",ROM_SIZE);
    appendf("Sample map: %s\nSamples contiguous: %s\n",
        mapped?"YES":"N/A",
        mapped?(contiguous?"YES":"NO"):"N/A");
    appendf("File integrity: %s\n",r.complete?"PASS":"FAIL");
    line_hex("File first bad:",r.first_bad);
    line_hex("File read bytes:",r.bytes);
    appendf("Note: samples are RAM backing; the file is not mapped ROM.\n");
    appendf("\nHH3 SDK DIAGNOSTICS\n");
    appendf("MMUCR %08lX (AT %s)\n",mmucr,mmucr&1u?"ON":"OFF");
    appendf("ROM path: %s\n",ROM_PATH);
    appendf("File open rc: %d\n",r.open_rc);
    appendf("File fstat rc: %d\n",r.stat_rc);
    appendf("File length: %d\n",r.length_rc);
    appendf("File read rc: %d\n",r.io_rc);
    appendf("File close rc: %d\n",r.close_rc);
    appendf("RAM allocation: %s\n",r.resident?"YES":"NO");
    appendf("Marker 5A/A5/3C: %s/%s/%s\n",
        r.marks[0]?"YES":"NO",r.marks[1]?"YES":"NO",r.marks[2]?"YES":"NO");
    if(report_length >= sizeof(report)) report_length = sizeof(report)-1;
    report[report_length] = 0;
}

static int save_report()
{
    // SDK FILE_OPEN_CREATE does not truncate existing files. Remove first.
    int removed=static_cast<int>(File_Remove(REPORT_PATH));
    if(removed<0 && removed!=FILE_ENOENT && removed!=FILE_ENOPATH)
        return removed;
    int fd=File_Open(REPORT_PATH,FILE_OPEN_WRITE|FILE_OPEN_CREATE);
    if(fd<0)return fd;
    size_t offset=0;
    int error=0;
    while(offset<report_length) {
        size_t wanted=report_length-offset;
        if(wanted>4096u)wanted=4096u;
        int n=File_Write(fd,report+offset,static_cast<int>(wanted));
        if(n<=0 || static_cast<size_t>(n)>wanted) {
            error=n<=0?n:-2;
            if(n==0)error=-19;
            break;
        }
        offset+=static_cast<size_t>(n);
    }
    int closed=static_cast<int>(File_Close(fd));
    return error?error:closed;
}

int main(int argc,char **argv,char **envp)
{
    (void)argc;(void)argv;(void)envp;
    RomResult r=load_rom();
    build_report(r);
    int save_rc=save_report();
    std::printf("%s",report);
    std::printf("\nRESULTS %s: %s (rc=%d)\n",REPORT_PATH,
        save_rc==0?"SAVED":"FAILED",save_rc);
    std::fflush(stdout);
    if(r.resident)Mem_Free(r.resident);
    Debug_WaitKey();
    return r.complete && save_rc==0 ? 0 : 1;
}
