#include <doctest/doctest.h>
#include <unicorn/unicorn.h>

TEST_CASE("unicorn runs ARM code") {
    uc_engine *uc = nullptr;
    REQUIRE(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) == UC_ERR_OK);
    REQUIRE(uc_ctl_set_cpu_model(uc, UC_CPU_ARM_946) == UC_ERR_OK);
    REQUIRE(uc_mem_map(uc, 0, 0x1000, UC_PROT_ALL) == UC_ERR_OK);

    const uint32_t insn = 0xE3A0002A; // mov r0, #42
    REQUIRE(uc_mem_write(uc, 0, &insn, sizeof insn) == UC_ERR_OK);
    REQUIRE(uc_emu_start(uc, 0, 4, 0, 1) == UC_ERR_OK);

    uint32_t r0 = 0;
    REQUIRE(uc_reg_read(uc, UC_ARM_REG_R0, &r0) == UC_ERR_OK);
    CHECK(r0 == 42);

    uc_close(uc);
}
