#include "rvemu.h"
bool cur_interp_mode = true;
bool pre_interp_mode = true;
int mode_switch_counter = 0;


void log_address_to_file(uint64_t address) {
    const char *filename = "log.txt";
    FILE *file = fopen(filename, "ab");  // 打开文件以二进制追加模式
    if (!file) {
        perror("Failed to open log file");
        exit(EXIT_FAILURE);
    }

    fwrite(&address, sizeof(address), 1, file);  // 写入地址
    fclose(file);
}

enum exit_reason_t machine_step(machine_t *m) {
    while(true) {
        bool hot = true;

        u8 *code = cache_lookup(m->cache, m->state.pc);
        if (code == NULL) {
            hot = cache_hot(m->cache, m->state.pc);
            if (hot) {
                str_t source = machine_genblock(m);
                // printf("source:\n");
                // printf("%s\n",source);
                code = machine_compile(m, source);
            }
        }

        if (!hot) {
            code = (u8 *)exec_block_interp;
        }

        while (true) {
            m->state.exit_reason = none;
            if( code == (u8 *)exec_block_interp){
                cur_interp_mode = true;
            }else{
                cur_interp_mode = false;
            }
            if(cur_interp_mode!=pre_interp_mode){
                mode_switch_counter++;
            }



            log_address_to_file((uint64_t)m->state.pc);
            ((exec_block_func_t)code)(&m->state);
            pre_interp_mode = cur_interp_mode;
            assert(m->state.exit_reason != none);

            if (m->state.exit_reason == indirect_branch ||
                m->state.exit_reason == direct_branch ) {
                code = cache_lookup(m->cache, m->state.reenter_pc);
                if (code != NULL) continue;
            }

            if (m->state.exit_reason == interp) {
                m->state.pc = m->state.reenter_pc;
                code = (u8 *)exec_block_interp;
                continue;
            }

            break;
        }

        m->state.pc = m->state.reenter_pc;
        switch (m->state.exit_reason) {
        case direct_branch:
        case indirect_branch:
            // continue execution
            break;
        case ecall:
            return ecall;
        default:
            unreachable();
        }
    }
}

void machine_load_program(machine_t *m, char *prog) {
    int fd = open(prog, O_RDONLY);
    if (fd == -1) {
        fatal(strerror(errno));
    }

    mmu_load_elf(&m->mmu, fd);
    close(fd);

    m->state.pc = (u64)m->mmu.entry;
}

void machine_setup(machine_t *m, int argc, char *argv[]) {
    size_t stack_size = 32 * 1024 * 1024;
    u64 stack = mmu_alloc(&m->mmu, stack_size);
    m->state.gp_regs[sp] = stack + stack_size;

    m->state.gp_regs[sp] -= 8; // auxp
    m->state.gp_regs[sp] -= 8; // envp
    m->state.gp_regs[sp] -= 8; // argv end

    u64 args = argc - 1;
    for (int i = args; i > 0; i--) {
        size_t len = strlen(argv[i]);
        u64 addr = mmu_alloc(&m->mmu, len+1);
        mmu_write(addr, (u8 *)argv[i], len);
        m->state.gp_regs[sp] -= 8; // argv[i]
        mmu_write(m->state.gp_regs[sp], (u8 *)&addr, sizeof(u64));
    }

    m->state.gp_regs[sp] -= 8; // argc
    mmu_write(m->state.gp_regs[sp], (u8 *)&args, sizeof(u64));
}
