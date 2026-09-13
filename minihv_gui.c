
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/kvm.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#define RAM_SIZE     (128*1024*1024)
#define LOAD_ADDR    0x1000
#define SERIAL_PORT  0x3F8
#define LOG_FILE     "kvm.log"
#define WIN_W        800
#define WIN_H        500
#define FONT_SIZE    14
#define COLS         100
#define ROWS         35
#define CIRC_SIZE    4096
#define NAME_MAX_LEN 64

typedef enum { GUI_NONE=0, GUI_BIOS=1, GUI_TERM=2 } GuiMode;
typedef enum { VM_EMPTY=0,VM_CREATED=1,VM_RUNNING=2,VM_STOPPED=3 } VMState;

typedef struct { char buf[CIRC_SIZE]; int head,tail; pthread_mutex_t lock; } CircBuf;
typedef struct { char buf[256];       int head,tail; pthread_mutex_t lock; } KeyBuf;

typedef struct {
    char      name[NAME_MAX_LEN];
    VMState   state;
    int       kvm_fd,vm_fd,vcpu_fd;
    void     *mem; size_t mem_size;
    struct kvm_run *run; size_t run_size;
    GuiMode   gui_mode;
    CircBuf  *out_buf;
    KeyBuf   *key_buf;
    pthread_t thread;
    int       stop_req;
} MiniVM;

static int   kvm_fd_global=-1;
static FILE *logfp=NULL;

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

static void circ_push(CircBuf *c,char ch){
    pthread_mutex_lock(&c->lock);
    int next=(c->tail+1)%CIRC_SIZE;
    if(next!=c->head){c->buf[c->tail]=ch;c->tail=next;}
    pthread_mutex_unlock(&c->lock);
}
static int circ_pop(CircBuf *c,char *ch){
    pthread_mutex_lock(&c->lock);
    if(c->head==c->tail){pthread_mutex_unlock(&c->lock);return 0;}
    *ch=c->buf[c->head];c->head=(c->head+1)%CIRC_SIZE;
    pthread_mutex_unlock(&c->lock);return 1;
}
static void key_push(KeyBuf *k,char ch){
    pthread_mutex_lock(&k->lock);
    int next=(k->tail+1)%256;
    if(next!=k->head){k->buf[k->tail]=ch;k->tail=next;}
    pthread_mutex_unlock(&k->lock);
}
static int key_pop(KeyBuf *k,char *ch){
    pthread_mutex_lock(&k->lock);
    if(k->head==k->tail){pthread_mutex_unlock(&k->lock);return 0;}
    *ch=k->buf[k->head];k->head=(k->head+1)%256;
    pthread_mutex_unlock(&k->lock);return 1;
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

static void *vm_thread(void *arg){
    MiniVM *vm=(MiniVM*)arg;
    while(!vm->stop_req){
        if(ioctl(vm->vcpu_fd,KVM_RUN,0)<0) break;
        switch(vm->run->exit_reason){
            case KVM_EXIT_IO:
                if(vm->run->io.direction==KVM_EXIT_IO_OUT&&
                   (vm->run->io.port==SERIAL_PORT||vm->run->io.port==0xF8)){
                    uint8_t *d=(uint8_t*)vm->run+vm->run->io.data_offset;
                    for(unsigned i=0;i<vm->run->io.count;i++){
                        if(vm->out_buf) circ_push(vm->out_buf,d[i]);
                        else{putchar(d[i]);fflush(stdout);}
                    }
                }
                if(vm->run->io.direction==KVM_EXIT_IO_IN&&
                   (vm->run->io.port==SERIAL_PORT||vm->run->io.port==0xF8)){
                    uint8_t *d=(uint8_t*)vm->run+vm->run->io.data_offset;
                    char ch=0;
                    if(vm->key_buf) key_pop(vm->key_buf,&ch);
                    *d=(uint8_t)ch;
                }
                break;
            case KVM_EXIT_HLT:
                LOG_OK("VM '%s' — HLT",vm->name);
                vm->state=VM_STOPPED;return NULL;
            case KVM_EXIT_SHUTDOWN:
                LOG_WARN("VM '%s' — shutdown",vm->name);
                vm->state=VM_STOPPED;return NULL;
            default:break;
        }
    }
    vm->state=VM_STOPPED;return NULL;
}

/* ── Chercher la fonte DejaVu ── */
static const char *find_font(void){
    const char *paths[]={
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/freefont/FreeMono.ttf",
        NULL
    };
    for(int i=0;paths[i];i++){
        if(access(paths[i],R_OK)==0) return paths[i];
    }
    return NULL;
}

static void run_gui(MiniVM *vm,GuiMode mode){
    if(SDL_Init(SDL_INIT_VIDEO)<0){
        LOG_ERR("SDL_Init: %s",SDL_GetError());return;
    }
    if(TTF_Init()<0){
        LOG_ERR("TTF_Init: %s",TTF_GetError());SDL_Quit();return;
    }

    const char *font_path=find_font();
    if(!font_path){
        LOG_ERR("Aucune fonte trouvée");TTF_Quit();SDL_Quit();return;
    }
    TTF_Font *font=TTF_OpenFont(font_path,FONT_SIZE);
    if(!font){
        LOG_ERR("TTF_OpenFont: %s",TTF_GetError());TTF_Quit();SDL_Quit();return;
    }
    LOG_INFO("Fonte chargée: %s",font_path);

    int fw,fh;
    TTF_SizeText(font,"X",&fw,&fh);
    int win_w=fw*COLS, win_h=fh*ROWS;

    const char *title=mode==GUI_BIOS
        ?"MiniHV — BIOS Mode  [h=aide s=statut r=reboot q=quitter]"
        :"MiniHV — Terminal Mode  [h=aide s=statut r=reboot q=quitter]";

    SDL_Window *win=SDL_CreateWindow(title,
        SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,
        win_w,win_h,SDL_WINDOW_SHOWN);
    if(!win){TTF_CloseFont(font);TTF_Quit();SDL_Quit();return;}

    SDL_Renderer *rend=SDL_CreateRenderer(win,-1,
        SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
    if(!rend){SDL_DestroyWindow(win);TTF_CloseFont(font);TTF_Quit();SDL_Quit();return;}

    SDL_StartTextInput();

    /* Buffer écran */
    char lines[ROWS][COLS+1];
    memset(lines,0,sizeof(lines));
    int cur_row=0,cur_col=0,running=1;
    SDL_Event ev;

    /* Couleurs selon le mode */
    SDL_Color fg = mode==GUI_BIOS
        ?(SDL_Color){220,220,220,255}
        :(SDL_Color){0,255,70,255};
    SDL_Color cursor_col=(SDL_Color){255,255,255,255};

    while(running){
        while(SDL_PollEvent(&ev)){
            if(ev.type==SDL_QUIT){running=0;vm->stop_req=1;}
            if(ev.type==SDL_KEYDOWN&&vm->key_buf){
                char ch=0;
                if(ev.key.keysym.sym==SDLK_RETURN)      ch='\r';
                else if(ev.key.keysym.sym==SDLK_BACKSPACE) ch='\b';
                else if(ev.key.keysym.sym==SDLK_ESCAPE)    {running=0;vm->stop_req=1;}
                if(ch) key_push(vm->key_buf,ch);
            }
            if(ev.type==SDL_TEXTINPUT&&vm->key_buf)
                for(int i=0;ev.text.text[i];i++)
                    key_push(vm->key_buf,ev.text.text[i]);
        }

        /* Lire buffer VM */
        char ch;
        while(circ_pop(vm->out_buf,&ch)){
            if(ch=='\n'){
                cur_row++;cur_col=0;
                if(cur_row>=ROWS){
                    memmove(lines[0],lines[1],(ROWS-1)*(COLS+1));
                    memset(lines[ROWS-1],0,COLS+1);
                    cur_row=ROWS-1;
                    cur_col=0;
                }
            } else if(ch=='\r'){
                cur_col=0;
            } else if(ch=='\b'){
                if(cur_col>0){cur_col--;lines[cur_row][cur_col]=0;}
            } else if(ch>=32&&ch<127){
                if(cur_col<COLS){lines[cur_row][cur_col]=ch;cur_col++;}
                if(cur_col>=COLS){
                    cur_col=0;cur_row++;
                    if(cur_row>=ROWS){
                        memmove(lines[0],lines[1],(ROWS-1)*(COLS+1));
                        memset(lines[ROWS-1],0,COLS+1);
                        cur_row=ROWS-1;
                    }
                }
            }
        }

        /* Rendu */
        SDL_SetRenderDrawColor(rend,0,0,0,255);
        SDL_RenderClear(rend);

        for(int row=0;row<ROWS;row++){
            if(!lines[row][0]) continue;
            SDL_Surface *surf=TTF_RenderText_Solid(font,lines[row],fg);
            if(surf){
                SDL_Texture *tex=SDL_CreateTextureFromSurface(rend,surf);
                SDL_Rect dst={0,row*fh,surf->w,surf->h};
                SDL_RenderCopy(rend,tex,NULL,&dst);
                SDL_DestroyTexture(tex);
                SDL_FreeSurface(surf);
            }
        }

        /* Curseur clignotant — position calculée sur la ligne réelle */
        if((SDL_GetTicks()/500)%2==0){
            SDL_SetRenderDrawColor(rend,
                cursor_col.r,cursor_col.g,cursor_col.b,255);
            int cx=0;
            if(cur_col>0 && lines[cur_row][0]){
                char tmp_line[COLS+1];
                int copy_len=cur_col<COLS?cur_col:COLS;
                strncpy(tmp_line,lines[cur_row],copy_len);
                tmp_line[copy_len]=0;
                int tw=0,th=0;
                TTF_SizeText(font,tmp_line,&tw,&th);
                cx=tw;
            }
            SDL_Rect c={cx,cur_row*fh+fh-2,fw,2};
            SDL_RenderFillRect(rend,&c);
        }

        SDL_RenderPresent(rend);
        SDL_Delay(16);
        if(vm->state==VM_STOPPED){SDL_Delay(2000);running=0;}
    }

    SDL_StopTextInput();
    TTF_CloseFont(font);
    SDL_DestroyRenderer(rend);
    SDL_DestroyWindow(win);
    TTF_Quit();
    SDL_Quit();
}

static int launch_vm(const char *binary,GuiMode mode){
    CircBuf *out=NULL;KeyBuf *key=NULL;
    if(mode!=GUI_NONE){
        out=calloc(1,sizeof(CircBuf));key=calloc(1,sizeof(KeyBuf));
        pthread_mutex_init(&out->lock,NULL);pthread_mutex_init(&key->lock,NULL);
    }
    MiniVM vm;memset(&vm,0,sizeof(vm));
    strncpy(vm.name,"vm0",NAME_MAX_LEN-1);
    vm.mem_size=RAM_SIZE;vm.kvm_fd=kvm_fd_global;
    vm.vcpu_fd=-1;vm.gui_mode=mode;vm.out_buf=out;vm.key_buf=key;
    vm.vm_fd=ioctl(kvm_fd_global,KVM_CREATE_VM,0);
    if(vm.vm_fd<0){LOG_ERR("KVM_CREATE_VM echoue");return -1;}
    vm.mem=mmap(NULL,RAM_SIZE,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    if(vm.mem==MAP_FAILED){LOG_ERR("mmap echoue");return -1;}
    memset(vm.mem,0,RAM_SIZE);
    struct kvm_userspace_memory_region reg={.slot=0,.guest_phys_addr=0,
        .memory_size=RAM_SIZE,.userspace_addr=(uint64_t)(uintptr_t)vm.mem};
    ioctl(vm.vm_fd,KVM_SET_USER_MEMORY_REGION,&reg);
    FILE *f=fopen(binary,"rb");
    if(!f){LOG_ERR("Impossible d'ouvrir '%s'",binary);return -1;}
    size_t n=fread((uint8_t*)vm.mem+LOAD_ADDR,1,RAM_SIZE-LOAD_ADDR,f);fclose(f);
    LOG_OK("Binaire '%s' charge (%zu octets)",binary,n);
    if(vcpu_setup(&vm)<0) return -1;
    vm.state=VM_RUNNING;
    LOG_INFO("VM demarree — mode %s",
             mode==GUI_BIOS?"BIOS":mode==GUI_TERM?"Terminal":"CLI");
    if(mode!=GUI_NONE){
        pthread_create(&vm.thread,NULL,vm_thread,&vm);
        run_gui(&vm,mode);
        vm.stop_req=1;pthread_join(vm.thread,NULL);
        if(out){pthread_mutex_destroy(&out->lock);free(out);}
        if(key){pthread_mutex_destroy(&key->lock);free(key);}
    } else {
        vm_thread(&vm);
    }
    if(vm.run)       munmap(vm.run,vm.run_size);
    if(vm.mem)       munmap(vm.mem,RAM_SIZE);
    if(vm.vcpu_fd>0) close(vm.vcpu_fd);
    if(vm.vm_fd>0)   close(vm.vm_fd);
    LOG_OK("VM terminee");return 0;
}

int main(int argc,char *argv[]){
    logfp=fopen(LOG_FILE,"a");
    LOG_INFO("=== MiniHV GUI demarrage ===");
    kvm_fd_global=open("/dev/kvm",O_RDWR);
    if(kvm_fd_global<0){LOG_ERR("Impossible d'ouvrir /dev/kvm");return 1;}
    LOG_INFO("KVM v%d initialise",ioctl(kvm_fd_global,KVM_GET_API_VERSION,0));
    if(argc<2){
        fprintf(stderr,
            "Usage:\n  %s <bin>\n  %s --gui <bin>\n  %s --term <bin>\n",
            argv[0],argv[0],argv[0]);return 1;
    }
    GuiMode mode=GUI_NONE;const char *binary=NULL;
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--gui"))       mode=GUI_BIOS;
        else if(!strcmp(argv[i],"--term")) mode=GUI_TERM;
        else binary=argv[i];
    }
    if(!binary){LOG_ERR("Aucun binaire specifie");return 1;}
    int ret=launch_vm(binary,mode);
    close(kvm_fd_global);
    if(logfp) fclose(logfp);
    LOG_INFO("=== MiniHV GUI arret ===");
    return ret;
}
