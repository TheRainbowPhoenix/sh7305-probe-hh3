// SPDX-FileCopyrightText: 2026 Artem Novak
// SPDX-License-Identifier: BSD-3-Clause
// Standalone SH7305 / Hollyhock-3 diagnostic; no gint dependency.
#include <appdef.h>
#include <sdk/os/debug.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

APP_NAME("SH7305 Probe")
APP_AUTHOR("Artem Novak")
APP_DESCRIPTION("SH7305 CPU, MMU and external ROM probe")
APP_VERSION("1.1.0")

static constexpr uint32_t ROM_SIZE = 0x180000u;
static constexpr uint32_t ROM_MID = 0x080000u;
static constexpr uint32_t ROM_LAST = ROM_SIZE - 1u;
static constexpr char ROM_FILE[] = "/romprobe.bin";
static constexpr char REPORT_FILE[] = "/results.txt";
static uint8_t chunk[4096];

__attribute__((used, noinline)) static void probe_code_anchor()
{
    __asm__ volatile("" ::: "memory");
}

static uint32_t reg(uint32_t address)
{
    return *reinterpret_cast<volatile const uint32_t *>(address);
}

static uint32_t address_of(const void *p)
{
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));
}

struct Mapping {
    uint32_t va = 0xffffffffu;
    uint32_t pa = 0xffffffffu;
    uint32_t p1 = 0xffffffffu;
    uint32_t page = 0;
    bool valid = false;
    bool direct = false;
};

/* P1/P2 are physical aliases even when MMUCR.AT is set. Do not infer
   physical addresses for P0 without an independently verified UTLB
   translation; reading a stale P4 TLB array in HH3 can fault on hardware. */
static Mapping mapping_of_va(uint32_t va)
{
    Mapping m;
    m.va = va;
    m.direct = m.va >= 0x80000000u && m.va < 0xc0000000u;
    if(m.direct) {
        m.pa = m.va & 0x1fffffffu;
        m.p1 = m.pa | 0x80000000u;
        m.valid = true;
    }
    return m;
}

static Mapping mapping_of(const void *p)
{
    return mapping_of_va(address_of(p));
}

struct RomResult {
    bool opened = false;
    bool size_ok = false;
    bool complete = false;
    bool marks[3] = {};
    uint32_t read_bytes = 0;
    uint32_t first_bad = 0xffffffffu;
    uint8_t *resident = nullptr;
};

/* Reading into an explicitly allocated 1.5 MiB buffer creates an actual
   RAM-resident image independent of HH3 .rodata. If allocation fails, the
   streaming integrity test still runs and the sample map is reported N/A. */
static RomResult verify_rom()
{
    RomResult r;
    FILE *f = std::fopen(ROM_FILE, "rb");
    if(!f) return r;
    r.opened = true;
    if(std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        return r;
    }
    long length = std::ftell(f);
    r.size_ok = length == static_cast<long>(ROM_SIZE);
    if(!r.size_ok || std::fseek(f, 0, SEEK_SET) != 0) {
        std::fclose(f);
        return r;
    }

    r.resident = static_cast<uint8_t *>(std::malloc(ROM_SIZE));
    while(r.read_bytes < ROM_SIZE) {
        size_t count = ROM_SIZE - r.read_bytes;
        if(count > sizeof(chunk)) count = sizeof(chunk);
        /* Both the allocation-backed and fallback paths use bounded I/O. */
        size_t n = std::fread(chunk, 1, count, f);
        if(n == 0) break;
        for(size_t i = 0; i < n; ++i) {
            uint32_t offset = r.read_bytes + static_cast<uint32_t>(i);
            uint8_t expected = offset == 0 ? 0x5a :
                offset == ROM_MID ? 0xa5 :
                offset == ROM_LAST ? 0x3c : 0;
            if(chunk[i] != expected && r.first_bad == 0xffffffffu)
                r.first_bad = offset;
            if(offset == 0) r.marks[0] = chunk[i] == 0x5a;
            if(offset == ROM_MID) r.marks[1] = chunk[i] == 0xa5;
            if(offset == ROM_LAST) r.marks[2] = chunk[i] == 0x3c;
        }
        if(r.resident)
            std::memcpy(r.resident + r.read_bytes, chunk, n);
        r.read_bytes += static_cast<uint32_t>(n);
    }
    r.complete = r.read_bytes == ROM_SIZE && r.first_bad == 0xffffffffu;
    std::fclose(f);
    if(!r.complete) {
        std::free(r.resident);
        r.resident = nullptr;
    }
    return r;
}

static const char *yesno(bool value) { return value ? "YES" : "NO"; }

static const char *mapped(bool value) { return value ? "YES" : "N/A"; }

static void print_sample(FILE *out, const char *label, const uint8_t *sample,
                         uint32_t expected)
{
    std::fprintf(out, "\n%s\n", label);
    if(!sample) {
        std::fprintf(out, "VA N/A\nPA N/A\nP1 N/A\nPAGE bytes N/A\n"
                          "VA value N/A\nP1 value N/A\nAlias match: N/A\n");
        return;
    }
    Mapping m = mapping_of(sample);
    std::fprintf(out, "VA %08lX\n", m.va);
    if(!m.valid) {
        std::fprintf(out, "PA N/A\nP1 N/A\nPAGE bytes N/A\n"
                          "VA value %08lX\nP1 value N/A\n"
                          "Alias match: N/A\n", static_cast<uint32_t>(*sample));
        return;
    }
    uint32_t v = *sample;
    uint32_t p1 = *reinterpret_cast<volatile const uint8_t *>(m.p1);
    std::fprintf(out,
        "PA %08lX\nP1 %08lX\nPAGE bytes N/A (direct P1/P2)\n"
        "VA value %08lX\nP1 value %08lX\nAlias match: %s\n"
        "Expected %08lX\n",
        m.pa, m.p1, v, p1, yesno(v == p1), expected);
}

/* Output to both a text file and the debug console, without storing a
   second large buffer of formatted text. */
static void write_report(FILE *out, uint32_t pvr, uint32_t prr,
                         uint32_t cvr, uint32_t cpidr, uint32_t mmucr,
                         uint32_t frqcr, uint32_t pllcr, uint32_t fllfrq,
                         Mapping code, uint16_t code_word, RomResult const &r)
{
    std::fprintf(out,
        "SH7305 PROBE RESULTS\n"
        "====================\n\n"
        "PVR   %08lX\nPRR   %08lX\nCVR   %08lX\nCPIDR %08lX\n"
        "MMUCR %08lX (AT %s)\n\n"
        "USER RAM\n"
        "VA 08100000\n"
        "PA N/A (no P0 translation available)\n"
        "P1 N/A\nPAGE bytes N/A\nRAM span bytes N/A\n\n"
        "SH7305 CLOCKS (raw)\n"
        "FRQCR  %08lX\nPLLCR  %08lX\nFLLFRQ %08lX\n"
        "Frequency Hz: N/A\n\n",
        pvr, prr, cvr, cpidr, mmucr, yesno((mmucr & 1u) != 0),
        frqcr, pllcr, fllfrq);

    std::fprintf(out, "ADDIN CODE\nVA %08lX\n", code.va);
    if(code.valid) {
        uint16_t alias = *reinterpret_cast<volatile const uint16_t *>(code.p1);
        std::fprintf(out,
            "PA %08lX\nP1 %08lX\nPAGE bytes N/A (direct P1/P2)\n"
            "VA value %08lX\nP1 value %08lX\nAlias match: %s\n",
            code.pa, code.p1, static_cast<uint32_t>(code_word),
            static_cast<uint32_t>(alias), yesno(alias == code_word));
    }
    else
        std::fprintf(out, "PA N/A\nP1 N/A\nPAGE bytes N/A\n"
                          "VA value %08lX\nP1 value N/A\nAlias match: N/A\n",
                          static_cast<uint32_t>(code_word));

    print_sample(out, "ROM 1.5M START",
                 r.resident ? r.resident : nullptr, 0x5a);
    print_sample(out, "ROM 1.5M MID",
                 r.resident ? r.resident + ROM_MID : nullptr, 0xa5);
    print_sample(out, "ROM 1.5M END",
                 r.resident ? r.resident + ROM_LAST : nullptr, 0x3c);

    bool sample_map = r.resident &&
        mapping_of(r.resident).valid &&
        mapping_of(r.resident + ROM_MID).valid &&
        mapping_of(r.resident + ROM_LAST).valid;
    bool contiguous = false;
    if(sample_map) {
        Mapping a = mapping_of(r.resident);
        Mapping b = mapping_of(r.resident + ROM_MID);
        Mapping c = mapping_of(r.resident + ROM_LAST);
        contiguous = b.pa - a.pa == ROM_MID &&
                     c.pa - a.pa == ROM_LAST;
    }
    std::fprintf(out,
        "\nRAM-BACKED ROM IMAGE TEST\n"
        "ROM bytes %08lX\n"
        "Sample map: %s\n"
        "Samples contiguous in RAM: %s\n"
        "Resident allocation: %s\n"
        "Source file: %s\n"
        "Opened: %s\nSize OK: %s\nBytes read: %lu\n"
        "Marker start 5A: %s\nMarker mid A5: %s\nMarker end 3C: %s\n"
        "First bad offset: %08lX\nDATA: %s\n"
        "NOTE: RAM sample contiguity is NOT storage/ROM contiguity.\n",
        ROM_SIZE, mapped(sample_map),
        sample_map ? yesno(contiguous) : "N/A",
        yesno(r.resident != nullptr), ROM_FILE,
        yesno(r.opened), yesno(r.size_ok), r.read_bytes,
        yesno(r.marks[0]), yesno(r.marks[1]), yesno(r.marks[2]),
        r.first_bad, r.complete ? "PASS" : "FAIL");
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    uint32_t pvr = reg(0xff000030);
    uint32_t prr = reg(0xff000044);
    uint32_t cvr = reg(0xff000040);
    uint32_t cpidr = reg(0xff000048);
    uint32_t mmucr = reg(0xff000010);
    uint32_t frqcr = reg(0xa4150000);
    uint32_t pllcr = reg(0xa4150024);
    uint32_t fllfrq = reg(0xa4150050);
    Mapping code = mapping_of_va(static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(&probe_code_anchor)));
    uint16_t code_word = *reinterpret_cast<volatile const uint16_t *>(code.va);

    RomResult r = verify_rom();
    FILE *out = std::fopen(REPORT_FILE, "wb");
    bool saved = false;
    if(out) {
        write_report(out, pvr, prr, cvr, cpidr, mmucr,
                     frqcr, pllcr, fllfrq, code, code_word, r);
        saved = std::fclose(out) == 0;
    }
    write_report(stdout, pvr, prr, cvr, cpidr, mmucr,
                 frqcr, pllcr, fllfrq, code, code_word, r);
    std::printf("\nRESULTS /results.txt: %s\n", saved ? "SAVED" : "FAILED");
    std::fflush(stdout);
    std::free(r.resident);
    Debug_WaitKey();
    return saved && r.complete ? 0 : 1;
}
