#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "CpuMonitor.h"
#include "DiskMonitor.h"
#include "MemoryMonitor.h"
#include "ProcessMonitor.h"
#include "SystemMonitor.h"

using namespace sysmon;

TEST_CASE("parseCpuLine sums the accounted fields") {
    const auto times = parseCpuLine("cpu  100 20 30 900 40 5 5 0 0 0");
    REQUIRE(times.has_value());
    CHECK(times->idleTime == 940);  // idle + iowait
    // guest and guest_nice are excluded: the kernel already counts them in
    // user and nice, so including them would inflate the total.
    CHECK(times->totalTime == 1100);
}

TEST_CASE("parseCpuLine tolerates kernels reporting fewer fields") {
    const auto times = parseCpuLine("cpu 10 20 30 40");
    REQUIRE(times.has_value());
    CHECK(times->idleTime == 40);
    CHECK(times->totalTime == 100);
}

TEST_CASE("parseCpuLine rejects non-CPU and truncated lines") {
    CHECK_FALSE(parseCpuLine("intr 12345 0 0").has_value());
    CHECK_FALSE(parseCpuLine("cpu 1 2").has_value());
    CHECK_FALSE(parseCpuLine("").has_value());
}

TEST_CASE("parseProcStat separates the aggregate from the per-core lines") {
    const std::string contents =
        "cpu  200 0 100 700 0 0 0 0 0 0\n"
        "cpu0 100 0 50 350 0 0 0 0 0 0\n"
        "cpu1 100 0 50 350 0 0 0 0 0 0\n"
        "intr 99 1 2 3\n"
        "ctxt 12345\n";

    const auto snapshot = parseProcStat(contents);
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->aggregate.totalTime == 1000);
    CHECK(snapshot->perCore.size() == 2);
    CHECK(snapshot->perCore[0].idleTime == 350);
}

TEST_CASE("parseProcStat fails without an aggregate line") {
    CHECK_FALSE(parseProcStat("intr 1 2 3\nctxt 4\n").has_value());
}

TEST_CASE("computeUsagePercent measures non-idle time between two samples") {
    const CpuTimes previous{800, 1000};
    const CpuTimes current{850, 1100};  // 100 jiffies elapsed, 50 of them idle

    const auto usage = computeUsagePercent(previous, current);
    REQUIRE(usage.has_value());
    CHECK(*usage == doctest::Approx(50.0));
}

TEST_CASE("computeUsagePercent reports nothing when the counters do not advance") {
    const CpuTimes sample{800, 1000};
    CHECK_FALSE(computeUsagePercent(sample, sample).has_value());
    // A suspend/resume or a container restart can move counters backwards.
    CHECK_FALSE(computeUsagePercent(CpuTimes{800, 2000}, sample).has_value());
}

TEST_CASE("computeUsagePercent stays within 0 and 100") {
    const auto idleOnly = computeUsagePercent(CpuTimes{0, 0}, CpuTimes{100, 100});
    REQUIRE(idleOnly.has_value());
    CHECK(*idleOnly == doctest::Approx(0.0));

    const auto busyOnly = computeUsagePercent(CpuTimes{0, 0}, CpuTimes{0, 100});
    REQUIRE(busyOnly.has_value());
    CHECK(*busyOnly == doctest::Approx(100.0));
}

TEST_CASE("parseMemInfo prefers MemAvailable") {
    const std::string contents =
        "MemTotal:       16000 kB\n"
        "MemFree:         1000 kB\n"
        "MemAvailable:    8000 kB\n"
        "Buffers:          500 kB\n"
        "Cached:          4000 kB\n";

    const auto memory = parseMemInfo(contents);
    REQUIRE(memory.has_value());
    CHECK(memory->totalBytes == 16000ULL * 1024);
    CHECK(memory->availableBytes == 8000ULL * 1024);
    CHECK(memory->usedBytes == 8000ULL * 1024);
    CHECK(memory->usagePercent == doctest::Approx(50.0));
}

TEST_CASE("parseMemInfo falls back when MemAvailable is absent") {
    // Kernels older than 3.14 do not publish MemAvailable.
    const std::string contents =
        "MemTotal:       16000 kB\n"
        "MemFree:         2000 kB\n"
        "Buffers:         1000 kB\n"
        "Cached:          4500 kB\n"
        "SReclaimable:     500 kB\n";

    const auto memory = parseMemInfo(contents);
    REQUIRE(memory.has_value());
    CHECK(memory->availableBytes == 8000ULL * 1024);
    CHECK(memory->usagePercent == doctest::Approx(50.0));
}

TEST_CASE("parseMemInfo refuses to divide by a missing or zero total") {
    CHECK_FALSE(parseMemInfo("MemFree: 1000 kB\n").has_value());
    CHECK_FALSE(parseMemInfo("MemTotal: 0 kB\n").has_value());
    CHECK_FALSE(parseMemInfo("").has_value());
}

TEST_CASE("parseMemInfo never reports more available than total") {
    const auto memory = parseMemInfo("MemTotal: 1000 kB\nMemAvailable: 4000 kB\n");
    REQUIRE(memory.has_value());
    CHECK(memory->availableBytes == memory->totalBytes);
    CHECK(memory->usedBytes == 0);
}

TEST_CASE("computeDiskStats counts reserved blocks as used, like df") {
    // 1000 blocks total, 400 free, only 300 of those available unprivileged:
    // the 100 block difference is reserved for root.
    const auto disk = computeDiskStats(4096, 1000, 400, 300);
    REQUIRE(disk.has_value());
    CHECK(disk->totalBytes == 1000ULL * 4096);
    CHECK(disk->usedBytes == 600ULL * 4096);
    CHECK(disk->freeBytes == 300ULL * 4096);
    CHECK(disk->usagePercent == doctest::Approx(100.0 * 600 / 900));
}

TEST_CASE("computeDiskStats rejects a filesystem with no blocks") {
    CHECK_FALSE(computeDiskStats(4096, 0, 0, 0).has_value());
    CHECK_FALSE(computeDiskStats(0, 1000, 400, 300).has_value());
}

TEST_CASE("parseUptimeSeconds reads the first field") {
    const auto uptime = parseUptimeSeconds("274920.12 1234567.89\n");
    REQUIRE(uptime.has_value());
    CHECK(*uptime == doctest::Approx(274920.12));

    CHECK_FALSE(parseUptimeSeconds("not-a-number\n").has_value());
    CHECK_FALSE(parseUptimeSeconds("").has_value());
}

TEST_CASE("parseProcessStat reads the fields after the comm field") {
    const std::string contents =
        "4812 (chrome) S 1 4812 4812 0 -1 4194560 1000 0 0 0 "
        "1200 300 0 0 20 0 12 0 98765 123456789 4096 "
        "18446744073709551615 1 2 3\n";

    const auto stat = parseProcessStat(contents);
    REQUIRE(stat.has_value());
    CHECK(stat->pid == 4812);
    CHECK(stat->name == "chrome");
    CHECK(stat->cpuJiffies == 1500);  // utime 1200 + stime 300
    CHECK(stat->startTimeJiffies == 98765);
    CHECK(stat->residentPages == 4096);
}

TEST_CASE("parseProcessStat handles names containing spaces and parentheses") {
    // Kernel threads and PAM helpers really do look like this, which is why the
    // numeric fields are read from after the last ')'.
    const std::string contents =
        "921 ((sd-pam) with space) S 1 921 921 0 -1 4194560 10 0 0 0 "
        "40 60 0 0 20 0 1 0 555 1024 128 0 0 0\n";

    const auto stat = parseProcessStat(contents);
    REQUIRE(stat.has_value());
    CHECK(stat->name == "(sd-pam) with space");
    CHECK(stat->cpuJiffies == 100);
    CHECK(stat->startTimeJiffies == 555);
}

TEST_CASE("parseProcessStat rejects truncated or malformed lines") {
    CHECK_FALSE(parseProcessStat("4812 (chrome) S 1 4812\n").has_value());
    CHECK_FALSE(parseProcessStat("no parentheses here\n").has_value());
    CHECK_FALSE(parseProcessStat("").has_value());
}

TEST_CASE("parseResidentMemoryBytes converts VmRSS to bytes") {
    const std::string contents =
        "Name:\tchrome\n"
        "VmSize:\t 2000000 kB\n"
        "VmRSS:\t  812646 kB\n"
        "Threads:\t 24\n";

    const auto bytes = parseResidentMemoryBytes(contents);
    REQUIRE(bytes.has_value());
    CHECK(*bytes == 812646ULL * 1024);
}

TEST_CASE("parseResidentMemoryBytes reports nothing for kernel threads") {
    // Kernel threads have no VmRSS line at all.
    CHECK_FALSE(parseResidentMemoryBytes("Name:\tkworker/0:1\nThreads:\t1\n").has_value());
}

TEST_CASE("computeProcessCpuPercent is a share of total system time") {
    const auto percent = computeProcessCpuPercent(25, 200);
    REQUIRE(percent.has_value());
    CHECK(*percent == doctest::Approx(12.5));

    // On an idle system the system counter can fail to advance.
    CHECK_FALSE(computeProcessCpuPercent(10, 0).has_value());
}
