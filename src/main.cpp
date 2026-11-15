// SPDX-FileCopyrightText: 2026 Artem Novak
// SPDX-License-Identifier: BSD-3-Clause
// Standalone SH7305 / Hollyhock-3 diagnostic; no gint dependency.
#include <appdef.h>
#include <sdk/os/debug.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

APP_NAME("SH7305 Probe")
APP_AUTHOR("Artem Novak")
APP_DESCRIPTION("SH7305 CPU, MMU and external ROM probe")
APP_VERSION("1.0.0")

static constexpr uint32_t ROM_SIZE = 0x180000u;
static constexpr uint32_t ROM_MID = 0x080000u;
static constexpr uint32_t ROM_LAST = ROM_SIZE - 1u;
static constexpr char ROM_FILE[] = "/romprobe.bin";
static constexpr char REPORT_FILE[] = "/results.txt";
static uint8_t chunk[4096];

__attribute__((used, noinline)) static void probe_code_anchor() { __asm__ volatile("" ::: "memory"); }

static uint32_t reg(uint32_t address)
{
    return *reinterpret_cast<volatile const uint32_t *>(address);
}

struct RomResult {
    bool opened = false;
    bool size_ok = false;
    bool complete = false;
    bool marks[3] = {};
    uint32_t read_bytes = 0;
    uint32_t first_bad = 0xffffffffu;
};

/* Only reads filesystem data; it never inserts/modifies TLB entries. */
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
    while(r.read_bytes < ROM_SIZE) {
        size_t count = ROM_SIZE - r.read_bytes;
        if(count > sizeof(chunk)) count = sizeof(chunk);
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
        r.read_bytes += static_cast<uint32_t>(n);
    }
    r.complete = r.read_bytes == ROM_SIZE && r.first_bad == 0xffffffffu;
    std::fclose(f);
    return r;
}

static const char *yesno(bool value) { return value ? "YES" : "NO"; }

int main(int argc, char **argv, char **envp)
{
    (void)argc;
    (void)argv;
    (void)envp;

    const uint32_t pvr = reg(0xff000030);
    const uint32_t cvr = reg(0xff000040);
    const uint32_t prr = reg(0xff000044);
    const uint32_t cpidr = reg(0xff000048);
    const uint32_t mmucr = reg(0xff000010);
    const uint32_t frqcr = reg(0xa4150000); // raw CPG values: verify on device
    const uint32_t code_va = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&probe_code_anchor));
    const bool direct = code_va >= 0x80000000u && code_va < 0xc0000000u;
    const uint32_t code_pa = direct ? (code_va & 0x1fffffffu) : 0xffffffffu;
    const uint32_t code_p1 = direct ? (code_pa | 0x80000000u) : 0xffffffffu;
    const uint32_t va_word = reg(code_va);
    const uint32_t p1_word = direct ? reg(code_p1) : 0;

    RomResult r = verify_rom();
    char report[1200];
    int n = std::snprintf(report, sizeof(report),
        "SH7305 STANDALONE PROBE\n"
        "CPU\nPVR=%08lX\nPRR=%08lX\nCVR=%08lX\nCPID=%08lX\n"
        "MMU\nMMUCR=%08lX\nAT=%s\nURC=%lu\nURB=%lu\n"
        "CLOCK\nFRQCR=%08lX\n"
        "CODE\nVA=%08lX\nPA=%08lX\nP1=%08lX\n"
        "VA_WORD=%08lX\nP1_WORD=%08lX\nMATCH=%s\n"
        "EXTERNAL ROM\nPATH=%s\nSIZE=%lu\nOPEN=%s\nSIZE_OK=%s\n"
        "READ=%lu\nSTART_5A=%s\nMID_A5=%s\nEND_3C=%s\n"
        "FIRST_BAD=%08lX\nDATA=%s\n"
        "NOTE: A filesystem test is not a physical ROM mapping test.\n",
        pvr, prr, cvr, cpidr, mmucr, yesno((mmucr & 1u) != 0),
        (mmucr >> 10) & 63u, (mmucr >> 18) & 63u,
        frqcr, code_va, code_pa, code_p1, va_word, p1_word,
        yesno(direct && va_word == p1_word),
        ROM_FILE, ROM_SIZE, yesno(r.opened), yesno(r.size_ok),
        r.read_bytes, yesno(r.marks[0]), yesno(r.marks[1]),
        yesno(r.marks[2]), r.first_bad, r.complete ? "PASS" : "FAIL");

    bool saved = false;
    if(n >= 0 && static_cast<size_t>(n) < sizeof(report)) {
        FILE *out = std::fopen(REPORT_FILE, "wb");
        if(out) {
            saved = std::fwrite(report, 1, static_cast<size_t>(n), out) ==
                static_cast<size_t>(n);
            if(std::fclose(out) != 0) saved = false;
        }
    }

    std::printf("%s", report);
    std::printf("RESULTS /results.txt: %s\n", saved ? "SAVED" : "FAILED");
    std::fflush(stdout);
    Debug_WaitKey();
    return saved ? 0 : 1;
}
