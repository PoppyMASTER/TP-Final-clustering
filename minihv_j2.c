
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/kvm.h>
#include <readline/readline.h>
#include <readline/history.h>

#define MAX_VMS      8
#define RAM_SIZE     (128*1024*1024)
#define LOAD_ADDR    0x1000
#define SERIAL_PORT  0x3F8
#define LOG_FILE     "kvm.log"
#define NAME_MAX_LEN 64
#define CMD_MAX_LEN  256
#define SNAP_MAGIC   0x4D484956

typedef enum { VM_EMPTY=0,VM_CREATED=1,VM_RUNNING=2,VM_STOPPED=3 } VMState;
static const char *state_str[]={"vide","creee","en cours","arretee"};

typedef struct {
    char     name[NAME_MAX_LEN];
    char     binary[256];
    VMState  state;
    int      kvm_fd,vm_fd,vcpu_fd;
    void    *mem; size_t mem_size;
    struct kvm_run *run; size_t run_size;
    time_t   created_at;
} MiniVM;

typedef struct {
    uint32_t magic; uint64_t ram_size;
    char vm_name[NAME_MAX_LEN]; char timestamp[32];
} SnapHeader;

static MiniVM vms[MAX_VMS];
static int    kvm_fd_global=-1;
static FILE  *logfp=NULL;

static void log_action(const char *lvl,const char *fmt,...){
    time_t now=time(NULL);struct tm *t=localtime(&now);
    char ts[32];strftime(ts,sizeof(ts),"%Y-%m-%d %H:%M:%S",t);
    va_list ap;char msg[512];
    va_start(ap,fmt);vsnprintf(msg,sizeof(msg),fmt,ap);va_end(ap);
    printf("[%s] %s\n",lvl,msg);
    if(logfp){fprintf(logfp,"[%s][%s] %s\n",ts,lvl,msg);fflush(logfp);}
}
#define LOG_INFO(...) log_action("INFO", __VA_ARGS__)
#define LOG_OK(...)   log_action("OK",   __VA_ARGS__)
#define LOG_ERR(...)  log_action("ERROR",__VA_ARGS__)
#define LOG_WARN(...) log_action("WARN", __VA_ARGS__)

static int validate_name(const char *n){
    if(!n||!*n||strlen(n)>=NAME_MAX_LEN){LOG_ERR("Nom invalide");return -1;}
    for(const char *p=n;*p;p++)
        if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='-'||*p=='_'))
            {LOG_ERR("Nom '%s' invalide — alphanum -_ uniquement",n);return -1;}
    return 0;
}
static MiniVM *vm_find(const char *n){
    for(int i=0;i<MAX_VMS;i++)
        if(vms[i].state!=VM_EMPTY&&strcmp(vms[i].name,n)==0)return &vms[i];
    return NULL;
}
static MiniVM *vm_find_empty(void){
    for(int i=0;i<MAX_VMS;i++)if(vms[i].state==VM_EMPTY)return &vms[i];
    return NULL;
}

static int cmd_create(const char *name,const char *binary){
    if(validate_name(name)<0)return -1;
    if(vm_find(name)){LOG_ERR("VM '%s' existe deja",name);return -1;}
    MiniVM *vm=vm_find_empty();
    if(!vm){LOG_ERR("Max VMs atteint (%d)",MAX_VMS);return -1;}
    struct stat st;
    if(stat(binary,&st)<0||st.st_size==0){LOG_ERR("Binaire '%s' invalide",binary);return -1;}
    memset(vm,0,sizeof(MiniVM));
    strncpy(vm->name,name,NAME_MAX_LEN-1);
    strncpy(vm->binary,binary,255);
    vm->mem_size=RAM_SIZE;vm->created_at=time(NULL);
    vm->kvm_fd=kvm_fd_global;vm->vm_fd=-1;vm->vcpu_fd=-1;
    vm->vm_fd=ioctl(kvm_fd_global,KVM_CREATE_VM,0);
    if(vm->vm_fd<0){LOG_ERR("KVM_CREATE_VM echoue");memset(vm,0,sizeof(MiniVM));return -1;}
    vm->mem=mmap(NULL,vm->mem_size,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    if(vm->mem==MAP_FAILED){LOG_ERR("mmap echoue");close(vm->vm_fd);memset(vm,0,sizeof(MiniVM));return -1;}
    memset(vm->mem,0,vm->mem_size);
    struct kvm_userspace_memory_region r={.slot=0,.guest_phys_addr=0,.memory_size=vm->mem_size,.userspace_addr=(uint64_t)(uintptr_t)vm->mem};
    ioctl(vm->vm_fd,KVM_SET_USER_MEMORY_REGION,&r);
    FILE *f=fopen(binary,"rb");
    if(!f){LOG_ERR("Impossible d'ouvrir '%s'",binary);return -1;}
    size_t n=fread((uint8_t*)vm->mem+LOAD_ADDR,1,vm->mem_size-LOAD_ADDR,f);fclose(f);
    vm->state=VM_CREATED;
    LOG_OK("VM '%s' creee — binaire '%s' (%zu octets)",name,binary,n);
    return 0;
}

static int vcpu_setup(MiniVM *vm){
    vm->vcpu_fd=ioctl(vm->vm_fd,KVM_CREATE_VCPU,0);
    if(vm->vcpu_fd<0){LOG_ERR("KVM_CREATE_VCPU echoue");return -1;}
    vm->run_size=ioctl(vm->kvm_fd,KVM_GET_VCPU_MMAP_SIZE,0);
    vm->run=mmap(NULL,vm->run_size,PROT_READ|PROT_WRITE,MAP_SHARED,vm->vcpu_fd,0);
    if(vm->run==MAP_FAILED){LOG_ERR("mmap run echoue");return -1;}
    struct kvm_sregs sregs;
    ioctl(vm->vcpu_fd,KVM_GET_SREGS,&sregs);
    sregs.cr0=0x0;
    sregs.cs.base=0;sregs.cs.selector=0;sregs.cs.limit=0xFFFF;
    sregs.cs.type=0x3;sregs.cs.present=1;sregs.cs.dpl=0;
    sregs.cs.db=0;sregs.cs.s=1;sregs.cs.l=0;sregs.cs.g=0;
    sregs.ds.base=sregs.es.base=sregs.ss.base=0;
    sregs.ds.selector=sregs.es.selector=sregs.ss.selector=0;
    sregs.ds.limit=sregs.es.limit=sregs.ss.limit=0xFFFF;
    sregs.ds.type=sregs.es.type=sregs.ss.type=0x3;
    sregs.ds.present=sregs.es.present=sregs.ss.present=1;
    sregs.ds.s=sregs.es.s=sregs.ss.s=1;
    ioctl(vm->vcpu_fd,KVM_SET_SREGS,&sregs);
    struct kvm_regs regs;memset(&regs,0,sizeof(regs));
    regs.rip=LOAD_ADDR;regs.rsp=0x7000;regs.rflags=0x2;
    ioctl(vm->vcpu_fd,KVM_SET_REGS,&regs);
    return 0;
}

static int vm_run_loop(MiniVM *vm){
    while(1){
        if(ioctl(vm->vcpu_fd,KVM_RUN,0)<0){LOG_ERR("KVM_RUN echoue");return -1;}
        switch(vm->run->exit_reason){
            case KVM_EXIT_IO:
                if(vm->run->io.direction==KVM_EXIT_IO_OUT&&
                   (vm->run->io.port==SERIAL_PORT||vm->run->io.port==0xF8)){
                    uint8_t *d=(uint8_t*)vm->run+vm->run->io.data_offset;
                    for(unsigned int i=0;i<vm->run->io.count;i++)putchar(d[i]);
                    fflush(stdout);
                }break;
            case KVM_EXIT_HLT:LOG_OK("VM '%s' — HLT",vm->name);return 0;
            case KVM_EXIT_SHUTDOWN:LOG_WARN("VM '%s' — shutdown",vm->name);return 0;
            default:break;
        }
    }
}

static int cmd_start(const char *name){
    if(validate_name(name)<0)return -1;
    MiniVM *vm=vm_find(name);
    if(!vm){LOG_ERR("VM '%s' introuvable",name);return -1;}
    if(vm->state==VM_RUNNING){LOG_WARN("VM '%s' deja en cours",name);return -1;}
    if(vcpu_setup(vm)<0)return -1;
    vm->state=VM_RUNNING;
    LOG_INFO("Demarrage VM '%s'...",name);
    int ret=vm_run_loop(vm);
    vm->state=VM_STOPPED;
    if(vm->run){munmap(vm->run,vm->run_size);vm->run=NULL;}
    if(vm->vcpu_fd>0){close(vm->vcpu_fd);vm->vcpu_fd=-1;}
    return ret;
}

static void cmd_list(void){
    int found=0;
    printf("\n  %-16s %-12s %-24s\n","NOM","ETAT","BINAIRE");
    printf("  %-16s %-12s %-24s\n","----------------","------------","------------------------");
    for(int i=0;i<MAX_VMS;i++)
        if(vms[i].state!=VM_EMPTY){
            printf("  %-16s %-12s %-24s\n",vms[i].name,state_str[vms[i].state],vms[i].binary);
            found++;
        }
    if(!found)printf("  (aucune VM)\n");
    printf("\n");
}

static int cmd_snapshot(const char *name,const char *file){
    if(validate_name(name)<0)return -1;
    if(!file||!*file){LOG_ERR("Nom de snapshot invalide");return -1;}
    MiniVM *vm=vm_find(name);
    if(!vm){LOG_ERR("VM '%s' introuvable",name);return -1;}
    FILE *f=fopen(file,"wb");
    if(!f){LOG_ERR("Impossible de creer '%s'",file);return -1;}
    SnapHeader hdr;memset(&hdr,0,sizeof(hdr));
    hdr.magic=SNAP_MAGIC;hdr.ram_size=vm->mem_size;
    strncpy(hdr.vm_name,vm->name,NAME_MAX_LEN-1);
    time_t now=time(NULL);struct tm *t=localtime(&now);
    strftime(hdr.timestamp,sizeof(hdr.timestamp),"%Y-%m-%d %H:%M:%S",t);
    fwrite(&hdr,sizeof(hdr),1,f);
    fwrite(vm->mem,1,vm->mem_size,f);
    fclose(f);
    struct stat st;stat(file,&st);
    LOG_OK("Snapshot '%s' cree pour VM '%s' (%ld Mo)",file,name,st.st_size/(1024*1024));
    return 0;
}

static int cmd_restore(const char *name,const char *file){
    if(validate_name(name)<0)return -1;
    MiniVM *vm=vm_find(name);
    if(!vm){LOG_ERR("VM '%s' introuvable",name);return -1;}
    FILE *f=fopen(file,"rb");
    if(!f){LOG_ERR("Snapshot '%s' introuvable",file);return -1;}
    SnapHeader hdr;
    if(fread(&hdr,sizeof(hdr),1,f)!=1||hdr.magic!=SNAP_MAGIC){
        LOG_ERR("Snapshot invalide ou corrompu");fclose(f);return -1;
    }
    fread(vm->mem,1,vm->mem_size,f);fclose(f);
    vm->state=VM_CREATED;
    LOG_OK("VM '%s' restauree depuis '%s' (snapshot du %s)",name,file,hdr.timestamp);
    return 0;
}

static int cmd_destroy(const char *name){
    if(validate_name(name)<0)return -1;
    MiniVM *vm=vm_find(name);
    if(!vm){LOG_ERR("VM '%s' introuvable",name);return -1;}
    if(vm->state==VM_RUNNING){LOG_ERR("VM '%s' en cours — arretez-la d'abord",name);return -1;}
    if(vm->run)munmap(vm->run,vm->run_size);
    if(vm->mem)munmap(vm->mem,vm->mem_size);
    if(vm->vcpu_fd>0)close(vm->vcpu_fd);
    if(vm->vm_fd>0)close(vm->vm_fd);
    char n[NAME_MAX_LEN];strncpy(n,vm->name,NAME_MAX_LEN-1);
    memset(vm,0,sizeof(MiniVM));
    LOG_OK("VM '%s' detruite",n);
    return 0;
}

static void print_help(void){
    printf("\n  Commandes MiniHV:\n\n");
    printf("  create <nom> <binaire>     Creer une VM\n");
    printf("  start  <nom>               Demarrer une VM\n");
    printf("  list                       Lister les VMs\n");
    printf("  snapshot <nom> <fichier>   Sauvegarder l etat\n");
    printf("  restore  <nom> <fichier>   Restaurer un snapshot\n");
    printf("  destroy  <nom>             Supprimer une VM\n");
    printf("  help                       Cette aide\n");
    printf("  quit                       Quitter\n\n");
}

static const char *cli_cmds[]={"create","start","list","snapshot","restore","destroy","help","quit",NULL};
static char *cli_complete(const char *text,int state){
    static int idx;static size_t len;
    if(!state){idx=0;len=strlen(text);}
    while(cli_cmds[idx]){
        const char *c=cli_cmds[idx++];
        if(strncmp(c,text,len)==0)return strdup(c);
    }
    return NULL;
}
static char **cli_completion(const char *text,int start,int end){
    (void)end;(void)start;
    rl_attempted_completion_over=1;
    return rl_completion_matches(text,cli_complete);
}

static void parse_cmd(char *line){
    line[strcspn(line,"\n")]=0;
    if(!*line||line[0]=='#')return;
    char *a[4]={NULL};int ac=0;
    char *tok=strtok(line," \t");
    while(tok&&ac<4){a[ac++]=tok;tok=strtok(NULL," \t");}
    if(!a[0])return;
    if(!strcmp(a[0],"create")){if(ac<3){LOG_ERR("Usage: create <nom> <binaire>");return;}cmd_create(a[1],a[2]);}
    else if(!strcmp(a[0],"start")){if(ac<2){LOG_ERR("Usage: start <nom>");return;}cmd_start(a[1]);}
    else if(!strcmp(a[0],"list")){cmd_list();}
    else if(!strcmp(a[0],"snapshot")){if(ac<3){LOG_ERR("Usage: snapshot <nom> <fichier>");return;}cmd_snapshot(a[1],a[2]);}
    else if(!strcmp(a[0],"restore")){if(ac<3){LOG_ERR("Usage: restore <nom> <fichier>");return;}cmd_restore(a[1],a[2]);}
    else if(!strcmp(a[0],"destroy")){if(ac<2){LOG_ERR("Usage: destroy <nom>");return;}cmd_destroy(a[1]);}
    else if(!strcmp(a[0],"help")){print_help();}
    else if(!strcmp(a[0],"quit")||!strcmp(a[0],"exit")){LOG_INFO("Au revoir.");exit(0);}
    else{LOG_ERR("Commande inconnue '%s' — tapez 'help'",a[0]);}
}

int main(int argc,char *argv[]){
    logfp=fopen(LOG_FILE,"a");
    LOG_INFO("=== MiniHV v2 demarrage ===");
    kvm_fd_global=open("/dev/kvm",O_RDWR);
    if(kvm_fd_global<0){LOG_ERR("Impossible d'ouvrir /dev/kvm");return 1;}
    LOG_INFO("KVM v%d initialise",ioctl(kvm_fd_global,KVM_GET_API_VERSION,0));
    if(argc==2){cmd_create("vm0",argv[1]);cmd_start("vm0");cmd_destroy("vm0");if(logfp)fclose(logfp);return 0;}
    printf("\n  MiniHV v2 — tapez 'help'\n\n");
    rl_attempted_completion_function=cli_completion;
    rl_bind_key('\t',rl_complete);
    using_history();
    char *line;
    while(1){
        line=readline("minihv> ");
        if(!line)break;
        if(*line){add_history(line);parse_cmd(line);}
        free(line);
    }
    if(logfp)fclose(logfp);
    return 0;
}
