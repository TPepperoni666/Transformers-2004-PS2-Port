#include "emulator/core/iop_cpu.h"
#include "emulator/core/iop_kernel.h"
#include "emulator/core/iop_memory.h"
#include "emulator/imports/iop_cdvd.h"
#include "emulator/imports/iop_imports.h"
#include "emulator/imports/iop_loadcore.h"
#include "emulator/imports/iop_timrman.h"
#include "emulator/services/iop_rpc.h"
#include "ps2x/iop/iop_host.h"

#include <cstdint>
#include <chrono>
#include <cstring>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using namespace ps2x::iop;
    using namespace ps2x::iop::detail;

    constexpr uint32_t kExportMagic = 0x41C00000u;
    constexpr int32_t kLibraryNotFound = -213;
    constexpr int32_t kIllegalLibrary = -214;

    class NullHost : public IopHost
    {
    public:
        bool readGuest(uint32_t, void *, size_t) const override { return false; }
        bool writeGuest(uint32_t, const void *, size_t) override { return false; }
        bool zeroGuest(uint32_t, size_t) override { return false; }
        bool normalizeGuestAddress(uint32_t, uint32_t &) const override { return false; }
        uint32_t allocateIopHandle(IopHandleKind) override { return 1u; }
        uint32_t allocateGuest(uint32_t, uint32_t) override { return 0u; }
        void freeGuest(uint32_t) override {}
        void audioCommand(uint32_t, uint32_t, GuestBuffer, GuestBuffer) override {}
        std::string hostPath(HostPathKind) const override { return {}; }
        std::string translateGuestPath(std::string_view path) const override { return std::string(path); }
        uint64_t openHostFile(std::string_view) override { return 0u; }
        bool hostFileSize(uint64_t, uint64_t &) const override { return false; }
        bool readHostFile(uint64_t, uint64_t, void *, size_t, size_t &) override { return false; }
        void closeHostFile(uint64_t) override {}
        int32_t memoryCard(const MemoryCardRequest &) override { return 0; }
        bool hasGuestFunction(uint32_t) const override { return false; }
        bool invokeGuestFunction(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *) override { return false; }
        void log(LogLevel, std::string_view) override {}
    };

    class CdRootHost final : public NullHost
    {
    public:
        explicit CdRootHost(std::filesystem::path rootPath)
            : root(std::move(rootPath))
        {
        }

        std::string hostPath(HostPathKind kind) const override
        {
            return kind == HostPathKind::CdRoot ? root.string() : std::string{};
        }

    private:
        std::filesystem::path root;
    };

    class RecordingExecutor final : public IopGuestExecutor
    {
    public:
        uint32_t executeGuestFunction(uint32_t address,
                                      uint32_t a0,
                                      uint32_t,
                                      uint32_t,
                                      uint32_t,
                                      uint32_t gp) override
        {
            ++calls;
            lastAddress = address;
            lastArgument = a0;
            lastGp = gp;
            return callbackResult;
        }

        uint32_t callbackResult = 0u;
        uint32_t calls = 0u;
        uint32_t lastAddress = 0u;
        uint32_t lastArgument = 0u;
        uint32_t lastGp = 0u;
    };

    bool expect(bool condition, std::string_view message)
    {
        if (condition)
            return true;
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }

    bool testLoadcoreRebootLibraryMode()
    {
        IopMemory memory;
        IopImportRegistry imports(memory);
        IopLoadcore loadcore(memory, imports);

        IopCpuState cpu{};
        cpu.gpr[4] = 0u;
        cpu.gpr[5] = 2u;
        if (!expect(loadcore.dispatchImport(27u, cpu), "loadcore:27 was not handled") ||
            !expect(static_cast<int32_t>(cpu.gpr[2]) == kIllegalLibrary,
                    "loadcore:27 did not reject a null export table"))
            return false;

        constexpr uint32_t table = 0x1000u;
        memory.write32(table, kExportMagic);
        memory.write16(table + 8u, 0x0101u);
        memory.write16(table + 10u, 0x1234u);
        const char name[8] = {'t', 'e', 's', 't', 'l', 'i', 'b', '\0'};
        (void)memory.writeRam(table + 12u, name, sizeof(name));
        memory.write32(table + 20u, 0u);

        cpu = {};
        cpu.gpr[4] = table;
        cpu.gpr[5] = 2u;
        if (!expect(loadcore.dispatchImport(27u, cpu), "loadcore:27 rejected a valid export table") ||
            !expect(cpu.gpr[2] == 0u, "loadcore:27 returned an error for a valid export table") ||
            !expect(memory.read16(table + 10u) == 0x1232u,
                    "loadcore:27 did not replace only export mode bits 1 and 2"))
            return false;

        constexpr uint32_t invalidTable = 0x1100u;
        memory.write32(invalidTable, 0xDEADBEEFu);
        cpu = {};
        cpu.gpr[4] = invalidTable;
        cpu.gpr[5] = 6u;
        if (!expect(loadcore.dispatchImport(27u, cpu), "loadcore:27 did not consume an invalid-table call") ||
            !expect(static_cast<int32_t>(cpu.gpr[2]) == kLibraryNotFound,
                    "loadcore:27 returned the wrong invalid-table error"))
            return false;

        if (!expect(imports.registerExportTable(table), "test export table did not register"))
            return false;
        memory.write32(table, 0u);
        cpu = {};
        cpu.gpr[4] = table;
        cpu.gpr[5] = 6u;
        return expect(loadcore.dispatchImport(27u, cpu), "loadcore:27 rejected a registered table") &&
               expect(cpu.gpr[2] == 0u, "loadcore:27 returned an error for a registered table") &&
               expect(memory.read16(table + 10u) == 0x1236u,
                      "loadcore:27 did not update a registered table's mode");
    }

    bool pollEvent(IopKernel &kernel, int eventId, uint32_t bits, uint32_t resultAddress, int32_t expected)
    {
        IopCpuState cpu{};
        cpu.gpr[4] = static_cast<uint32_t>(eventId);
        cpu.gpr[5] = bits;
        cpu.gpr[6] = 0u; // WEF_AND
        cpu.gpr[7] = resultAddress;
        return expect(kernel.dispatchEventImport(11u, cpu), "PollEventFlag was not handled") &&
               expect(static_cast<int32_t>(cpu.gpr[2]) == expected, "PollEventFlag returned an unexpected result");
    }

    bool testCdvdSpecialControl()
    {
        NullHost host;
        IopMemory memory;
        IopKernel kernel(memory);
        kernel.reset();
        IopCdvd cdvd(host, memory, kernel);
        cdvd.reset();

        constexpr uint32_t param = 0x2000u;
        constexpr uint32_t eventResult = 0x2010u;
        IopCpuState cpu{};
        cpu.gpr[4] = static_cast<uint32_t>(-11); // sceCdSC: return cdvdman interrupt event flag
        cpu.gpr[5] = param;
        if (!expect(cdvd.dispatchImport(50u, cpu), "cdvdman:50 was not handled") ||
            !expect(static_cast<int32_t>(cpu.gpr[2]) > 0, "sceCdSC(-11) did not return a valid event flag"))
            return false;
        const int eventId = static_cast<int>(cpu.gpr[2]);

        if (!pollEvent(kernel, eventId, 0x29u, eventResult, 0) ||
            !expect(memory.read32(eventResult) == 0x29u, "cdvdman event flag did not start with bits 0x29"))
            return false;

        IopCpuState clear{};
        clear.gpr[4] = static_cast<uint32_t>(eventId);
        clear.gpr[5] = ~0x29u;
        if (!expect(kernel.dispatchEventImport(8u, clear), "ClearEventFlag was not handled") ||
            !pollEvent(kernel, eventId, 0x29u, eventResult, -418))
            return false;

        cpu = {};
        cpu.gpr[4] = 0x12345u;
        if (!expect(cdvd.dispatchImport(7u, cpu), "sceCdSeek was not handled") ||
            !pollEvent(kernel, eventId, 0x29u, eventResult, 0))
            return false;

        memory.write8(param, 0x30u);
        cpu = {};
        cpu.gpr[4] = static_cast<uint32_t>(-2);
        cpu.gpr[5] = param;
        if (!expect(cdvd.dispatchImport(50u, cpu), "sceCdSC(-2) was not handled") ||
            !expect(cpu.gpr[2] == 0x30u, "sceCdSC(-2) did not store the low-byte error"))
            return false;

        memory.write32(param, 0u);
        cpu = {};
        cpu.gpr[4] = static_cast<uint32_t>(-1);
        cpu.gpr[5] = param;
        if (!expect(cdvd.dispatchImport(50u, cpu), "sceCdSC(-1) was not handled") ||
            !expect(cpu.gpr[2] == 0u, "sceCdSC(-1) returned the wrong initial stream state") ||
            !expect(memory.read32(param) == 0x30u, "sceCdSC(-1) did not publish the last error"))
            return false;

        cpu = {};
        cpu.gpr[4] = 2u;
        cpu.gpr[5] = param;
        if (!expect(cdvd.dispatchImport(50u, cpu), "sceCdSC(2) was not handled") ||
            !expect(cpu.gpr[2] == 2u, "sceCdSC(2) did not update the stream state"))
            return false;

        cpu = {};
        cpu.gpr[4] = static_cast<uint32_t>(-1);
        cpu.gpr[5] = param;
        return expect(cdvd.dispatchImport(50u, cpu), "second sceCdSC(-1) was not handled") &&
               expect(cpu.gpr[2] == 2u, "sceCdSC(-1) did not preserve the stream state");
    }

    bool testCdvdSearchFile()
    {
        const auto suffix = std::to_string(
            static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path root =
            std::filesystem::temp_directory_path() / ("ps2x-iop-cdvd-search-" + suffix);
        const std::filesystem::path movieDirectory = root / "MOVIE";
        const std::filesystem::path moviePath = movieDirectory / "OPENING.PSS";
        std::error_code error;
        std::filesystem::create_directories(movieDirectory, error);
        if (!expect(!error, "could not create the temporary CD root"))
            return false;
        {
            std::ofstream movie(moviePath, std::ios::binary);
            movie.write("PSS!", 4);
        }

        CdRootHost host(root);
        IopMemory memory;
        IopKernel kernel(memory);
        kernel.reset();
        IopCdvd cdvd(host, memory, kernel);
        cdvd.reset();

        constexpr uint32_t resultAddress = 0x2400u;
        constexpr uint32_t pathAddress = 0x2480u;
        const char path[] = "cdrom0:\\movie\\opening.pss;1";
        (void)memory.writeRam(pathAddress, path, sizeof(path));

        IopCpuState cpu{};
        cpu.gpr[4] = resultAddress;
        cpu.gpr[5] = pathAddress;
        const bool handled = cdvd.dispatchImport(10u, cpu);
        const bool passed =
            expect(handled, "cdvdman:10 was not handled") &&
            expect(cpu.gpr[2] == 1u, "sceCdSearchFile did not find a case-insensitive ISO path") &&
            expect(memory.read32(resultAddress) >= 20u, "sceCdSearchFile returned an invalid LSN") &&
            expect(memory.read32(resultAddress + 4u) == 4u, "sceCdSearchFile returned the wrong size") &&
            expect(memory.readString(resultAddress + 8u, 16u) == "OPENING.PSS",
                   "sceCdSearchFile returned the wrong file name");

        std::filesystem::remove_all(root, error);
        return passed;
    }

    bool testTimrmanPeriodicCallback()
    {
        IopTimrman timrman;
        timrman.reset();
        IopCpuState cpu{};

        cpu.gpr[4] = 1u; // SYSCLK
        cpu.gpr[5] = 32u;
        cpu.gpr[6] = 1u;
        if (!expect(timrman.dispatchImport(4u, cpu, 100u), "AllocHardTimer was not handled") ||
            !expect(static_cast<int32_t>(cpu.gpr[2]) > 0, "AllocHardTimer did not allocate a 32-bit timer"))
            return false;
        const uint32_t timerId = cpu.gpr[2];

        cpu = {};
        cpu.gpr[4] = timerId;
        cpu.gpr[5] = 100u;
        cpu.gpr[6] = 0x12340u;
        cpu.gpr[7] = 0x45670u;
        cpu.gpr[28] = 0x89AB0u;
        if (!expect(timrman.dispatchImport(20u, cpu, 100u), "SetTimerHandler was not handled") ||
            !expect(cpu.gpr[2] == 0u, "SetTimerHandler failed"))
            return false;

        cpu = {};
        cpu.gpr[4] = timerId;
        cpu.gpr[5] = 1u;
        cpu.gpr[6] = 0u;
        cpu.gpr[7] = 1u;
        if (!expect(timrman.dispatchImport(22u, cpu, 100u), "SetupHardTimer was not handled") ||
            !expect(cpu.gpr[2] == 0u, "SetupHardTimer failed"))
            return false;

        cpu = {};
        cpu.gpr[4] = timerId;
        if (!expect(timrman.dispatchImport(23u, cpu, 100u), "StartHardTimer was not handled") ||
            !expect(cpu.gpr[2] == 0u, "StartHardTimer failed") ||
            !expect(timrman.nextEventCycle(1000u) == 200u, "timer compare was scheduled at the wrong cycle"))
            return false;

        RecordingExecutor executor;
        executor.callbackResult = 100u;
        timrman.serviceDue(199u, executor);
        if (!expect(executor.calls == 0u, "timer callback ran too early"))
            return false;
        timrman.serviceDue(200u, executor);
        return expect(executor.calls == 1u, "timer callback did not run") &&
               expect(executor.lastAddress == 0x12340u, "timer called the wrong handler") &&
               expect(executor.lastArgument == 0x45670u, "timer passed the wrong common argument") &&
               expect(executor.lastGp == 0x89AB0u, "timer callback lost the registering module GP") &&
               expect(timrman.nextEventCycle(1000u) == 300u, "timer callback return did not rearm compare");
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Ported from KIexster/god-of-war-recomp patches/ps2recomp-iop-sound.patch + ps2recomp-memcard.patch (GPL-3.0)
    // (their ps2xTest/src/ps2_iop_tests.cpp cases, adapted to this harness).

    class GuestRamHost final : public NullHost
    {
    public:
        bool readGuest(uint32_t address, void *destination, size_t size) const override
        {
            if (address > ram.size() || size > ram.size() - address)
                return false;
            std::memcpy(destination, ram.data() + address, size);
            return true;
        }
        bool writeGuest(uint32_t address, const void *source, size_t size) override
        {
            if (address > ram.size() || size > ram.size() - address)
                return false;
            std::memcpy(ram.data() + address, source, size);
            return true;
        }

        std::vector<uint8_t> ram = std::vector<uint8_t>(0x10000u, 0u);
    };

    // SIRENS3 Mutex::Lock (and GoW's 989snd tick lock) call WaitSema from an RPC server function, i.e. with no
    // current thread, and ignore the result.
    bool testWaitSemaOutsideThread()
    {
        IopMemory memory;
        IopKernel kernel(memory);
        kernel.reset();
        constexpr uint32_t kDescriptor = 0x2000u;
        memory.write32(kDescriptor + 0u, 0u);
        memory.write32(kDescriptor + 4u, 0u);
        memory.write32(kDescriptor + 8u, 0u); // count 0: held by another thread
        memory.write32(kDescriptor + 12u, 1u);
        IopCpuState create{};
        create.gpr[4] = kDescriptor;
        if (!expect(kernel.dispatchSemaphoreImport(4u, create), "CreateSema was not handled"))
            return false;
        const uint32_t id = create.gpr[2];

        IopCpuState wait{};
        wait.gpr[2] = 0u; // a stale "success" in v0 must be overwritten
        wait.gpr[4] = id;
        if (!expect(kernel.dispatchSemaphoreImport(8u, wait), "WaitSema was not handled") ||
            !expect(static_cast<int32_t>(wait.gpr[2]) == -419,
                    "WaitSema outside a thread with nobody to release it must not report success"))
            return false;

        uint32_t waits = 0u;
        kernel.setOutsideThreadWait([&](const std::function<bool()> &ready)
        {
            ++waits;
            IopCpuState signal{};
            signal.gpr[4] = id; // the holder runs and releases it
            (void)kernel.dispatchSemaphoreImport(6u, signal);
            return ready();
        });
        wait = {};
        wait.gpr[2] = 0xDEADBEEFu;
        wait.gpr[4] = id;
        if (!expect(kernel.dispatchSemaphoreImport(8u, wait), "WaitSema was not handled") ||
            !expect(wait.gpr[2] == 0u, "WaitSema outside a thread should succeed once another thread signals") ||
            !expect(waits == 1u, "WaitSema outside a thread should let other threads run while it waits"))
            return false;
        IopCpuState poll{};
        poll.gpr[4] = id;
        return expect(kernel.dispatchSemaphoreImport(9u, poll), "PollSema was not handled") &&
               expect(static_cast<int32_t>(poll.gpr[2]) == -419, "the outside-thread waiter should now own the semaphore");
    }

    bool testWaitEventFlagOutsideThread()
    {
        IopMemory memory;
        IopKernel kernel(memory);
        kernel.reset();
        constexpr uint32_t kDescriptor = 0x2000u;
        constexpr uint32_t kResult = 0x2040u;
        memory.write32(kDescriptor + 0u, 0u);
        memory.write32(kDescriptor + 4u, 0u);
        memory.write32(kDescriptor + 8u, 0u);
        IopCpuState create{};
        create.gpr[4] = kDescriptor;
        if (!expect(kernel.dispatchEventImport(4u, create), "CreateEventFlag was not handled"))
            return false;
        const uint32_t id = create.gpr[2];

        IopCpuState wait{};
        wait.gpr[4] = id;
        wait.gpr[5] = 0x1000u;
        wait.gpr[6] = 0x10u; // WEF_AND | WEF_CLEAR
        wait.gpr[7] = kResult;
        if (!expect(kernel.dispatchEventImport(10u, wait), "WaitEventFlag was not handled") ||
            !expect(static_cast<int32_t>(wait.gpr[2]) == -418, "without anyone to set it the wait must fail"))
            return false;

        kernel.setOutsideThreadWait([&](const std::function<bool()> &ready)
        {
            IopCpuState set{};
            set.gpr[4] = id;
            set.gpr[5] = 0x1000u; // e.g. the transfer-end interrupt
            (void)kernel.dispatchEventImport(6u, set);
            return ready();
        });
        wait.gpr[2] = 0xFFFFFFFFu;
        if (!expect(kernel.dispatchEventImport(10u, wait), "WaitEventFlag was not handled") ||
            !expect(wait.gpr[2] == 0u, "WaitEventFlag outside a thread should succeed once the flag is set") ||
            !expect(memory.read32(kResult) == 0x1000u, "the satisfied bits should be reported"))
            return false;
        IopCpuState poll{};
        poll.gpr[4] = id;
        poll.gpr[5] = 0x1000u;
        return expect(kernel.dispatchEventImport(11u, poll), "PollEventFlag was not handled") &&
               expect(static_cast<int32_t>(poll.gpr[2]) == -418, "WEF_CLEAR should have cleared the flag");
    }

    bool testSifSetDmaIntrQueuesCallback()
    {
        GuestRamHost host;
        IopMemory memory;
        IopKernel kernel(memory);
        kernel.reset();
        IopRpcBridge rpc(host, memory, kernel);
        rpc.reset();
        constexpr uint32_t kSource = 0x30000u;
        constexpr uint32_t kDescriptor = 0x30100u;
        constexpr uint32_t kEeDestination = 0x1800u;
        uint8_t payload[32];
        for (uint32_t i = 0; i < 32u; ++i)
            payload[i] = static_cast<uint8_t>(0x60u + i);
        const uint32_t descriptor[4] = {kSource, kEeDestination, static_cast<uint32_t>(sizeof(payload)), 0u};
        if (!expect(memory.writeRam(kSource, payload, sizeof(payload)), "payload was not written") ||
            !expect(memory.writeRam(kDescriptor, descriptor, sizeof(descriptor)), "descriptor was not written"))
            return false;

        IopCpuState plain{};
        plain.gpr[4] = kDescriptor;
        plain.gpr[5] = 1u;
        plain.gpr[6] = 0x112F4u; // ignored by sceSifSetDma
        if (!expect(rpc.dispatchSifManImport(7u, plain), "sifman:7 was not handled") ||
            !expect(!rpc.takeDmaCompletionCallback().has_value(), "sceSifSetDma must not queue a callback"))
            return false;

        IopCpuState cpu{};
        cpu.gpr[4] = kDescriptor;
        cpu.gpr[5] = 1u;
        cpu.gpr[6] = 0x112F4u; // callback
        cpu.gpr[7] = 0x55u;    // argument
        cpu.gpr[28] = 0x9000u; // gp
        if (!expect(rpc.dispatchSifManImport(32u, cpu), "sifman:32 was not handled") ||
            !expect(cpu.gpr[2] != 0u, "sceSifSetDmaIntr should return a DMA id") ||
            !expect(std::memcmp(host.ram.data() + kEeDestination, payload, sizeof(payload)) == 0,
                    "sceSifSetDmaIntr should copy the payload to EE memory"))
            return false;
        const auto callback = rpc.takeDmaCompletionCallback();
        return expect(callback.has_value(), "sceSifSetDmaIntr should queue its completion callback") &&
               expect(callback->function == 0x112F4u && callback->data == 0x55u && callback->gp == 0x9000u,
                      "completion callback function/argument/gp") &&
               expect(!rpc.takeDmaCompletionCallback().has_value(), "the callback should be taken once");
    }

    bool testSifGetOtherDataCopiesEeMemory()
    {
        GuestRamHost host;
        IopMemory memory;
        IopKernel kernel(memory);
        kernel.reset();
        IopRpcBridge rpc(host, memory, kernel);
        rpc.reset();
        constexpr uint32_t kEeSource = 0x1000u;
        constexpr uint32_t kIopDestination = 0x30000u;
        constexpr uint32_t kReceiveData = 0x2800u;
        for (uint32_t i = 0; i < 64u; ++i)
            host.ram[kEeSource + i] = static_cast<uint8_t>(0x40u + i);
        IopCpuState cpu{};
        cpu.gpr[4] = kReceiveData;
        cpu.gpr[5] = kEeSource;
        cpu.gpr[6] = kIopDestination;
        cpu.gpr[7] = 64u;
        if (!expect(rpc.dispatchSifCmdImport(23u, cpu), "sifcmd:23 was not handled") ||
            !expect(cpu.gpr[2] == 0u, "sceSifGetOtherData should succeed"))
            return false;
        bool copied = true;
        for (uint32_t i = 0; i < 64u; ++i)
            copied = copied && memory.read8(kIopDestination + i) == static_cast<uint8_t>(0x40u + i);
        return expect(copied, "the EE bytes should land at the IOP destination") &&
               expect(memory.read32(kReceiveData + 0x10u) == kEeSource, "receive data should record the source") &&
               expect(memory.read32(kReceiveData + 0x14u) == kIopDestination, "receive data should record the dest") &&
               expect(memory.read32(kReceiveData + 0x18u) == 64u, "receive data should record the size");
    }

    bool testLwrMergesWithPendingLwl()
    {
        IopMemory memory;
        IopCpuCore core(memory);
        constexpr uint32_t kCode = 0x40000u;
        constexpr uint32_t kData = 0x40100u;
        const uint8_t bytes[8] = {0x10u, 0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u};
        const uint32_t code[3] = {
            (0x22u << 26) | (5u << 21) | (8u << 16) | 6u, // lwl t0, 6(a1): unaligned word at kData+3
            (0x26u << 26) | (5u << 21) | (8u << 16) | 3u, // lwr t0, 3(a1) - in the LWL's load delay slot
            0u,                                           // nop
        };
        if (!expect(memory.writeRam(kData, bytes, sizeof(bytes)), "data was not written") ||
            !expect(memory.writeRam(kCode, code, sizeof(code)), "code was not written"))
            return false;
        IopCpuState cpu{};
        cpu.pc = kCode;
        cpu.gpr[5] = kData;
        cpu.gpr[8] = 0xAAAAAAAAu;
        for (int i = 0; i < 3; ++i)
            (void)core.executeInstruction(cpu);
        return expect(cpu.gpr[8] == 0x16151413u, "LWL/LWR pair should assemble the unaligned word from both halves");
    }

    bool testCdReadClockWritesBcd()
    {
        NullHost host;
        IopMemory memory;
        IopKernel kernel(memory);
        kernel.reset();
        IopCdvd cdvd(host, memory, kernel);
        cdvd.reset();
        constexpr uint32_t kClock = 0x2400u;
        IopCpuState cpu{};
        cpu.gpr[4] = kClock;
        if (!expect(cdvd.dispatchImport(24u, cpu), "cdvdman:24 was not handled") ||
            !expect(cpu.gpr[2] == 1u, "sceCdReadClock should succeed"))
            return false;
        const auto value = [&](uint32_t offset)
        {
            const uint8_t b = memory.read8(kClock + offset);
            return static_cast<uint32_t>((b >> 4) * 10u + (b & 0xFu));
        };
        return expect(value(1) < 60u && value(2) < 60u && value(3) < 24u, "time fields should be valid BCD") &&
               expect(value(5) >= 1u && value(5) <= 31u, "day should be valid") &&
               expect(value(6) >= 1u && value(6) <= 12u, "month should be valid");
    }
}

int main()
{
    if (!testLoadcoreRebootLibraryMode() || !testCdvdSpecialControl() || !testCdvdSearchFile() ||
        !testTimrmanPeriodicCallback())
        return 1;
    // Ported from KIexster/god-of-war-recomp (GPL-3.0): IOP/SIF/CD fixes (upstream IOP/SIF/CD integration).
    if (!testWaitSemaOutsideThread() || !testWaitEventFlagOutsideThread() || !testSifSetDmaIntrQueuesCallback() ||
        !testSifGetOtherDataCopiesEeMemory() || !testLwrMergesWithPendingLwl() || !testCdReadClockWritesBcd())
        return 1;
    std::cout << "ps2xIOP import tests passed\n";
    return 0;
}
