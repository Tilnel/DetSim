#include "state/state.h"
#include "ncurses_ui.h"
#include "cereal/archives/binary.hpp"
#include "cereal/types/vector.hpp"

#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <string>

extern "C" detsim::ui::NCursesUI *get_ncurses_ui() { return nullptr; }

static void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

template<class T> static std::string encoded(const T &value)
{
    std::ostringstream stream;
    { cereal::BinaryOutputArchive archive(stream); archive(value); }
    return stream.str();
}

template<class T> static T decoded(const std::string &data)
{
    T value{};
    std::istringstream stream(data);
    { cereal::BinaryInputArchive archive(stream); archive(value); }
    return value;
}

static void check_thread_archive()
{
    thread_state original;
    original.tid = 2;
    original.physical_tid = 10002;
    original.alive = true;
    original.ptid = 1;
    original.regs.rip = 0x12340000;
    original.regs.fs_base = 0x56780000;
    original.xstate = {1, 2, 3, 4};
    auto rebound = original;
    rebound.physical_tid = 20002;
    require(encoded(original) == encoded(rebound), "physical TID changes semantic thread archive");
    const auto loaded = decoded<thread_state>(encoded(original));
    require(loaded.physical_tid == 0 && loaded.alive && loaded.tid == 2 && loaded.ptid == 1,
            "thread archive does not separate logical liveness from physical binding");
    require(loaded.regs.rip == original.regs.rip && loaded.regs.fs_base == original.regs.fs_base &&
            loaded.xstate == original.xstate, "thread context lost in archive");
    rebound = original;
    rebound.alive = false;
    require(encoded(original) != encoded(rebound), "thread liveness absent from identity");
    auto exited = rebound;
    exited.exit_code = 7;
    require(encoded(exited) != encoded(rebound), "thread exit code absent from identity");
    require(decoded<thread_state>(encoded(exited)).exit_code == 7,
            "thread exit code lost in archive");
    rebound = original;
    ++rebound.regs.rip;
    require(encoded(original) != encoded(rebound), "registers absent from identity");
    rebound = original;
    ++rebound.regs.fs_base;
    require(encoded(original) != encoded(rebound), "TLS base absent from identity");
    rebound = original;
    rebound.xstate[0] ^= 1;
    require(encoded(original) != encoded(rebound), "extended registers absent from identity");

    thread_create_info record{};
    record.virtual_tid = 2;
    record.physical_tid = 10002;
    record.clone_flags = 0x10000;
    record.stack_addr = 0x4000;
    record.ptid = 1;
    auto other = record;
    other.physical_tid = 20002;
    require(encoded(record) == encoded(other), "creation history stores physical identity");
    const auto loaded_record = decoded<thread_create_info>(encoded(record));
    require(loaded_record.physical_tid == 0 && loaded_record.ptid == 1 &&
            loaded_record.virtual_tid == 2, "creation history lost logical parent");
    ++other.ptid;
    require(encoded(record) != encoded(other), "logical parent absent from creation identity");
}

static void check_tracee_archive()
{
    tracee_state first, second;
    first.add_thread(10001, 0, 0, 0, true);
    first.add_thread(10002, 0x10000, 0x4000, 10001, false);
    second.add_thread(20001, 0, 0, 0, true);
    second.add_thread(20002, 0x10000, 0x4000, 20001, false);
    require(first.threads[1].ptid == 1 && first.thread_create_records[1].ptid == 1,
            "physical parent was not normalized at creation");
    require(first.main_tid == 1 && second.main_tid == 1, "main thread identity is physical");
    require(first.metadata_equal(second) && encoded(first) == encoded(second),
            "equivalent logical thread metadata differs across physical bindings");
    const auto loaded = decoded<tracee_state>(encoded(first));
    require(loaded.main_tid == 1 && loaded.threads.size() == 2 &&
            loaded.threads[0].physical_tid == 0 && loaded.threads[1].physical_tid == 0 &&
            loaded.threads[0].alive && loaded.threads[1].alive && loaded.threads[1].ptid == 1,
            "tracee archive lost logical thread information");
    bool restore_rejected = false;
    try { loaded.recover_running_state(0, 0x6e6f70726f76656eULL); }
    catch (const std::runtime_error &error) {
        restore_rejected = std::string(error.what()) ==
            "Snapshot has no local thread binding provenance";
    }
    require(restore_rejected, "standalone metadata restored without owned binding provenance");
    auto branch = first;
    branch.threads[1].alive = false;
    require(first.threads[1].alive && !first.metadata_equal(branch),
            "branch liveness mutation escaped copy or was excluded from identity");
}

int main()
{
    try {
        check_thread_archive();
        check_tracee_archive();
        std::puts("PASS logical thread archives, liveness, TLS, parent identity and branch isolation");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "thread identity: %s\n", error.what());
        return 1;
    }
}
