/*
 * MiniHV — Jour 1
 * Moteur KVM complet :
 *   - mode protégé 32 bits
 *   - gestion KVM_EXIT_IO (port série 0x3F8)
 *   - chargeur de binaire flat
 *   - logs horodatés
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/kvm.h>

/* ── Configuration ─────────────────────────────────────── */
#define RAM_SIZE     (128 * 1024 * 1024)  /* 128 Mo */
#define LOAD_ADDR    0x1000             /* 1 Mo — adresse standard en mode protégé */
#define SERIAL_PORT  0x3F8               /* port série COM1 */
#define LOG_FILE     "kvm.log"

/* ── Logging ─────────────────────────────────────────────*/
static FILE *logfp = NULL;

static void log_action(const char *level, const char *fmt, ...) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", t);

    va_list ap;
    char msg[512];
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    printf("[%s] %s\n", level, msg);
    if (logfp) {
        fprintf(logfp, "[%s][%s] %s\n", ts, level, msg);
        fflush(logfp);
    }
}

#define LOG_INFO(...)  log_action("INFO",  __VA_ARGS__)
#define LOG_OK(...)    log_action("OK",    __VA_ARGS__)
#define LOG_ERR(...)   log_action("ERROR", __VA_ARGS__)

/* ── Structure principale ────────────────────────────────*/
typedef struct {
    int      kvm_fd;
    int      vm_fd;
    int      vcpu_fd;
    void    *mem;
    size_t   mem_size;
    struct kvm_run *run;
    size_t   run_size;
    char     name[64];
    int      running;
} MiniVM;

/* ── GDT minimale pour le mode protégé 32 bits ───────────
 *
 * La GDT (Global Descriptor Table) dit au CPU comment
 * accéder à la mémoire. En mode protégé on a besoin d'au
 * moins 3 entrées :
 *   0 : descripteur null (obligatoire)
 *   1 : segment de code (exécutable, lecture)
 *   2 : segment de données (lecture/écriture)
 */
static const uint64_t gdt[] = {
    0x0000000000000000ULL,  /* 0x00 null */
    0x00CF9A000000FFFFULL,  /* 0x08 code 32 bits ring 0 */
    0x00CF92000000FFFFULL,  /* 0x10 data 32 bits ring 0 */
};


/* ── Étape 1 : ouvrir KVM ────────────────────────────────*/
static int kvm_open(MiniVM *vm) {
    vm->kvm_fd = open("/dev/kvm", O_RDWR);
    if (vm->kvm_fd < 0) {
        LOG_ERR("Impossible d'ouvrir /dev/kvm — KVM dispo ?");
        return -1;
    }

    int ver = ioctl(vm->kvm_fd, KVM_GET_API_VERSION, 0);
    if (ver != 12) {
        LOG_ERR("Version KVM inattendue : %d", ver);
        return -1;
    }
    LOG_INFO("KVM v%d ouvert", ver);
    return 0;
}

/* ── Étape 2 : créer la VM ───────────────────────────────*/
static int vm_create(MiniVM *vm) {
    vm->vm_fd = ioctl(vm->kvm_fd, KVM_CREATE_VM, 0);
    if (vm->vm_fd < 0) {
        LOG_ERR("KVM_CREATE_VM échoué");
        return -1;
    }
    LOG_OK("VM '%s' créée", vm->name);
    return 0;
}

/* ── Étape 3 : allouer la RAM ───────────────────────────*/
static int vm_alloc_ram(MiniVM *vm) {
    vm->mem = mmap(NULL, vm->mem_size,
                   PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (vm->mem == MAP_FAILED) {
        LOG_ERR("mmap RAM échoué");
        return -1;
    }
    memset(vm->mem, 0, vm->mem_size);

    struct kvm_userspace_memory_region region = {
        .slot            = 0,
        .flags           = 0,
        .guest_phys_addr = 0x0,
        .memory_size     = vm->mem_size,
        .userspace_addr  = (uint64_t)(uintptr_t)vm->mem,
    };
    if (ioctl(vm->vm_fd, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
        LOG_ERR("KVM_SET_USER_MEMORY_REGION échoué");
        return -1;
    }

    /* Copier la GDT en mémoire guest (à l'adresse 0x500) */
    memcpy((uint8_t *)vm->mem + 0x500, gdt, sizeof(gdt));

    LOG_OK("RAM allouée : %zu Mo", vm->mem_size / (1024 * 1024));
    return 0;
}

/* ── Étape 4 : charger le binaire guest ──────────────────*/
static int vm_load_binary(MiniVM *vm, const char *path) {
    /* Validation du chemin */
    if (!path || strlen(path) == 0) {
        LOG_ERR("Chemin du binaire invalide");
        return -1;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        LOG_ERR("Impossible d'ouvrir '%s'", path);
        return -1;
    }

    struct stat st;
    if (stat(path, &st) < 0 || st.st_size == 0) {
        LOG_ERR("Fichier '%s' vide ou illisible", path);
        fclose(f);
        return -1;
    }

    if ((size_t)st.st_size > vm->mem_size - LOAD_ADDR) {
        LOG_ERR("Binaire trop grand (%ld octets)", st.st_size);
        fclose(f);
        return -1;
    }

    size_t n = fread((uint8_t *)vm->mem + LOAD_ADDR, 1, st.st_size, f);
    fclose(f);

    if ((long)n != st.st_size) {
        LOG_ERR("Lecture incomplète (%zu/%ld octets)", n, st.st_size);
        return -1;
    }

    LOG_OK("Binaire '%s' chargé à 0x%x (%zu octets)", path, LOAD_ADDR, n);
    return 0;
}

/* ── Étape 5 : créer et configurer le vCPU ───────────────*/
static int vcpu_setup(MiniVM *vm) {
    vm->vcpu_fd = ioctl(vm->vm_fd, KVM_CREATE_VCPU, 0);
    if (vm->vcpu_fd < 0) {
        LOG_ERR("KVM_CREATE_VCPU échoué");
        return -1;
    }

    vm->run_size = ioctl(vm->kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    vm->run = mmap(NULL, vm->run_size,
                   PROT_READ | PROT_WRITE,
                   MAP_SHARED, vm->vcpu_fd, 0);
    if (vm->run == MAP_FAILED) {
        LOG_ERR("mmap kvm_run échoué");
        return -1;
    }

    /* ── Configurer les registres spéciaux (mode protégé) ── */
    struct kvm_sregs sregs;
    if (ioctl(vm->vcpu_fd, KVM_GET_SREGS, &sregs) < 0) {
        LOG_ERR("KVM_GET_SREGS échoué");
        return -1;
    }

    /*
     * CR0 bit 0 (PE) = 1 → active le mode protégé
     * Sans ça on reste en mode réel 16 bits
     */
    /* Mode réel 16 bits — CR0 bit PE = 0 */
    sregs.cr0 = 0x0;

    /* CS en mode réel : base=0, selector=0, limit=0xFFFF */
    sregs.cs.base     = 0x0;
    sregs.cs.selector = 0x0;
    sregs.cs.limit    = 0xFFFF;
    sregs.cs.type     = 0x3;
    sregs.cs.present  = 1;
    sregs.cs.dpl      = 0;
    sregs.cs.db       = 0;
    sregs.cs.s        = 1;
    sregs.cs.l        = 0;
    sregs.cs.g        = 0;

    /* DS/ES/SS en mode réel */
    sregs.ds.base = sregs.es.base = sregs.ss.base = 0x0;
    sregs.ds.selector = sregs.es.selector = sregs.ss.selector = 0x0;
    sregs.ds.limit = sregs.es.limit = sregs.ss.limit = 0xFFFF;
    sregs.ds.type = sregs.es.type = sregs.ss.type = 0x3;
    sregs.ds.present = sregs.es.present = sregs.ss.present = 1;
    sregs.ds.s = sregs.es.s = sregs.ss.s = 1;

    if (ioctl(vm->vcpu_fd, KVM_SET_SREGS, &sregs) < 0) {
        LOG_ERR("KVM_SET_SREGS échoué");
        return -1;
    }

    /* ── Configurer les registres généraux ── */
    struct kvm_regs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rip    = LOAD_ADDR;   /* début du code guest */
    regs.rsp    = 0x200000;    /* pile à 2 Mo */
    regs.rflags = 0x2;         /* bit réservé toujours à 1 */

    if (ioctl(vm->vcpu_fd, KVM_SET_REGS, &regs) < 0) {
        LOG_ERR("KVM_SET_REGS échoué");
        return -1;
    }

    LOG_OK("vCPU configuré en mode réel 16 bits, RIP=0x%x", LOAD_ADDR);
    return 0;
}

/* ── Étape 6 : boucle d'exécution ───────────────────────*/
static int vm_run(MiniVM *vm) {
    LOG_INFO("Démarrage de la VM '%s'...", vm->name);
    vm->running = 1;

    while (vm->running) {
        if (ioctl(vm->vcpu_fd, KVM_RUN, 0) < 0) {
            LOG_ERR("KVM_RUN échoué");
            return -1;
        }

        switch (vm->run->exit_reason) {

            /* ── Port I/O : le guest écrit sur le port série ── */
            case KVM_EXIT_IO:
                if (vm->run->io.direction == KVM_EXIT_IO_OUT &&
                    (vm->run->io.port == SERIAL_PORT || vm->run->io.port == 0xF8)) {
                    /*
                     * Le guest a fait un "out" sur 0x3F8.
                     * Les données sont dans kvm_run juste après
                     * la struct, à l'offset data_offset.
                     */
                    uint8_t *data = (uint8_t *)vm->run +
                                    vm->run->io.data_offset;
                    for (int i = 0; (unsigned int)i < vm->run->io.count; i++) {
                        putchar(data[i]);
                    }
                    fflush(stdout);
                }
                break;

            /* ── HLT : le guest a fini ── */
            case KVM_EXIT_HLT:
                LOG_OK("VM '%s' — HLT reçu, arrêt propre", vm->name);
                vm->running = 0;
                break;

            /* ── Shutdown ── */
            case KVM_EXIT_SHUTDOWN:
                LOG_OK("VM '%s' — shutdown", vm->name);
                vm->running = 0;
                break;

            /* ── Erreur interne KVM ── */
            case KVM_EXIT_INTERNAL_ERROR:
                LOG_ERR("Erreur interne KVM : suberror=%u",
                        vm->run->internal.suberror);
                vm->running = 0;
                return -1;

            /* ── Cas non géré ── */
            default:
                LOG_ERR("Exit inattendu : %u — on ignore",
                        vm->run->exit_reason);
                break;
        }
    }
    return 0;
}

/* ── Nettoyage ───────────────────────────────────────────*/
static void vm_destroy(MiniVM *vm) {
    if (vm->run)     munmap(vm->run, vm->run_size);
    if (vm->mem)     munmap(vm->mem, vm->mem_size);
    if (vm->vcpu_fd > 0) close(vm->vcpu_fd);
    if (vm->vm_fd   > 0) close(vm->vm_fd);
    if (vm->kvm_fd  > 0) close(vm->kvm_fd);
    LOG_INFO("VM '%s' détruite, ressources libérées", vm->name);
}

/* ── Main ────────────────────────────────────────────────*/
int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <binaire_guest>\n", argv[0]);
        fprintf(stderr, "  ex: %s mini_os.bin\n", argv[0]);
        return 1;
    }

    /* Ouvrir le fichier de log */
    logfp = fopen(LOG_FILE, "a");
    if (!logfp) {
        fprintf(stderr, "Impossible d'ouvrir %s\n", LOG_FILE);
    }

    LOG_INFO("=== MiniHV démarrage ===");

    MiniVM vm = {0};
    strncpy(vm.name, "vm0", sizeof(vm.name) - 1);
    vm.mem_size = RAM_SIZE;

    /* Séquence d'initialisation */
    if (kvm_open(&vm)             < 0) goto fail;
    if (vm_create(&vm)            < 0) goto fail;
    if (vm_alloc_ram(&vm)         < 0) goto fail;
    if (vm_load_binary(&vm, argv[1]) < 0) goto fail;
    if (vcpu_setup(&vm)           < 0) goto fail;

    /* Lancement */
    int ret = vm_run(&vm);

    vm_destroy(&vm);
    if (logfp) fclose(logfp);
    LOG_INFO("=== MiniHV arrêt ===");
    return ret == 0 ? 0 : 1;

fail:
    vm_destroy(&vm);
    if (logfp) fclose(logfp);
    return 1;
}
