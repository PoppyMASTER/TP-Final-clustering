#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/kvm.h>

#define MEM_SIZE  0x1000
#define LOAD_ADDR 0x1000

static const uint8_t guest_code[] = {
    0xb8, 0x05, 0x00,
    0x83, 0xc0, 0x03,
    0xf4
};

int main(void) {
    int kvm_fd = open("/dev/kvm", O_RDWR);
    if (kvm_fd < 0) { perror("open /dev/kvm"); return 1; }
    printf("[1] /dev/kvm ouvert\n");

    int vm_fd = ioctl(kvm_fd, KVM_CREATE_VM, 0);
    if (vm_fd < 0) { perror("KVM_CREATE_VM"); return 1; }
    printf("[2] VM creee\n");

    void *mem = mmap(NULL, MEM_SIZE, PROT_READ|PROT_WRITE,
                     MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    memcpy(mem, guest_code, sizeof(guest_code));

    struct kvm_userspace_memory_region region = {
        .slot            = 0,
        .guest_phys_addr = LOAD_ADDR,
        .memory_size     = MEM_SIZE,
        .userspace_addr  = (uint64_t)(uintptr_t)mem,
    };
    ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &region);
    printf("[3] RAM allouee\n");

    int vcpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 0);
    if (vcpu_fd < 0) { perror("KVM_CREATE_VCPU"); return 1; }
    printf("[4] vCPU cree\n");

    size_t mmap_size = ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    struct kvm_run *run = mmap(NULL, mmap_size, PROT_READ|PROT_WRITE,
                                MAP_SHARED, vcpu_fd, 0);

    struct kvm_sregs sregs;
    ioctl(vcpu_fd, KVM_GET_SREGS, &sregs);
    sregs.cs.base = 0; sregs.cs.selector = 0;
    ioctl(vcpu_fd, KVM_SET_SREGS, &sregs);

    struct kvm_regs regs = { .rip = LOAD_ADDR, .rflags = 0x2 };
    ioctl(vcpu_fd, KVM_SET_REGS, &regs);

    printf("[5] Lancement...\n");
    while (1) {
        ioctl(vcpu_fd, KVM_RUN, 0);
        if (run->exit_reason == KVM_EXIT_HLT) {
            ioctl(vcpu_fd, KVM_GET_REGS, &regs);
            printf("[OK] HLT atteint - AX = %llu (attendu 8)\n",
                   (unsigned long long)regs.rax);
            break;
        }
    }
    return 0;
}
