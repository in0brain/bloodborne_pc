/* Menu probe, the first step of mouse support in the menus. FromSoftware's menus are C++
 * dialogs and widgets (FrpgMenuDlg*) over Scaleform movies without mouse handlers, so the mouse
 * has to find their items through those objects. With BB_MENU_PROBE=1, F8 writes
 * logs/menu-probe-*.txt (BB_LOG_DIR: another folder): every live object of a menu class, its
 * first bytes with what they point at, and what points at it; then what those bytes point at,
 * two levels down (Bloodborne's dialogs are one unnamed class held by InGameMenuStep; most
 * FrpgMenuDlg* classes have no instances). A count of every named class comes first.
 *
 * The engine's runtime classes (DLRF) name the objects: a class's vtable starts with its
 * GetRuntimeClass, which loads the class from a slot ("mov rax,[rip+slot]; test rax,rax")
 * within its first bytes, and the runtime class holds the name at +0x40. Guest memory is read
 * through the backing view (runtime_memory_views): no GPU page protection, no faults. The probe
 * runs on the game thread reading the pad, so the menus stand still meanwhile. F8 instead
 * records the next seconds of the game's calls into Scaleform (see "Trace" below). Offsets are
 * Bloodborne v1.09 image offsets. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/bbgpu.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#define TEXT_END 0x50D96DCu       /* code and read-only data (class names) */
#define DATA_START 0x50DC000u     /* data and bss */
#define DATA_END 0x56D30F4u
#define VTABLES_START 0x5200000u  /* relocated data holding the vtables */
#define VTABLES_END 0x53E4000u
#define CHECK_NAME 0x493071Au     /* "FrpgMenuDlgCursorGroup" in the v1.09 image */
#define NAME_FIELD 0x40           /* runtime class: const char *name */
#define GETTER_BYTES 0x40         /* the slot load is this close to the getter's start */
#define OBJECT_BYTES 0x480       /* dumped of an object (the 0x440-byte dialogs fit) */
#define DATA_BYTES 0x100         /* dumped of plain data a dumped object points at */
#define MAX_DUMPED 4000
#define FOLLOW_DEPTH 2
#define MAX_FOLLOWED 1500
#define MAX_REFERRERS 8
#define NEAR_OBJECT 0x400         /* a pointer this far into a named object is labelled with it */
#define MAX_VIEWS 65536
#define VT_CACHE (1u << 18)       /* covers every 8-byte value of the vtable range */

typedef struct { uintptr_t vt; const char *name; int resolved; } VtEntry;
typedef struct { uintptr_t address; const char *name; } Object;
typedef struct { size_t object; int count; uintptr_t referrers[MAX_REFERRERS]; } Menu;
typedef struct { uintptr_t address, parent; uint32_t offset; int depth; } Followed;

static unsigned char *image;
static int enabled;
static unsigned probes;
static VtEntry *vt_cache;
static size_t vt_cached;
static RuntimeView *views;
static size_t view_count;
static Object *objects;
static size_t object_count, object_capacity;
static Menu *menus;
static size_t menu_count;
static FILE *probe_file;     /* open while a probe writes it (closed after a fault) */
/* Result lines stand out in a console that shows colours: a terminal, or the Windows console
 * run_game.py enabled them in (BB_CONSOLE_COLOR=1). */
static const char *on="", *off="";
static uint32_t *menu_index; /* open addressing: 1 + index into menus by address, 0 free */
static size_t menu_index_mask;
static Followed followed[MAX_FOLLOWED];
static size_t followed_count;
static uintptr_t *dumped;    /* open addressing: addresses already dumped or queued, 0 free */
static size_t dumped_mask;

static const char *log_dir(void) {
    const char *dir=getenv("BB_LOG_DIR");
    return dir && dir[0] ? dir : "logs";
}
/* [p, p+bytes) lies within image offsets [lo, hi). Offsets, not sums: guest data holds values
 * such as ~0, which p+bytes would wrap past the check. */
static int in_image(uintptr_t p, uint64_t lo, uint64_t hi, uint64_t bytes) {
    const uint64_t offset=p-(uintptr_t)image; /* p below the image wraps far above hi */
    return offset>=lo && offset<=hi && hi-offset>=bytes;
}
/* The class name a vtable's GetRuntimeClass leads to; NULL for other data. */
static const char *resolve(uintptr_t vt) {
    uintptr_t getter, slot, runtime_class, name;
    if ((vt & 7) || !in_image(vt,VTABLES_START,VTABLES_END,8)) return NULL;
    memcpy(&getter,(const void *)vt,8);
    if (!in_image(getter,0,TEXT_END,GETTER_BYTES)) return NULL;
    const unsigned char *g=(const unsigned char *)getter;
    for (int i=0;i+10<=GETTER_BYTES;++i) {
        if (g[i]!=0x48 || g[i+1]!=0x8B || g[i+2]!=0x05 || g[i+7]!=0x48 || g[i+8]!=0x85 || g[i+9]!=0xC0) continue;
        int32_t rel;
        memcpy(&rel,g+i+3,4);
        slot=getter+(uintptr_t)i+7+(uintptr_t)(intptr_t)rel;
        if (!in_image(slot,DATA_START,DATA_END,8)) return NULL;
        memcpy(&runtime_class,(const void *)slot,8);
        if (!in_image(runtime_class,DATA_START,DATA_END,NAME_FIELD+8)) return NULL;
        memcpy(&name,(const void *)(runtime_class+NAME_FIELD),8);
        if (!in_image(name,0,TEXT_END,1)) return NULL;
        const char *s=(const char *)name;
        size_t n=0, room=(uintptr_t)image+TEXT_END-name;
        while (n<room && n<96 && s[n]>=0x20 && s[n]<0x7f) ++n;
        return n>=3 && n<room && !s[n] ? s : NULL;
    }
    return NULL;
}
static const char *class_name(uintptr_t vt) {
    if (vt_cached>=VT_CACHE/4*3) return resolve(vt);
    for (size_t h=(size_t)((vt>>3)*UINT64_C(0x9E3779B97F4A7C15)>>46);;h=(h+1)&(VT_CACHE-1)) {
        if (vt_cache[h].resolved && vt_cache[h].vt==vt) return vt_cache[h].name;
        if (!vt_cache[h].resolved) {
            ++vt_cached;
            vt_cache[h]=(VtEntry){vt,resolve(vt),1};
            return vt_cache[h].name;
        }
    }
}
static int menu_class(const char *name) {
    if (!strncmp(name,"FD4Debug",8) || !strncmp(name,"Dbg",3)) return 0;
    return strstr(name,"Menu") || strstr(name,"Dlg") || strstr(name,"Dialog") || strstr(name,"Scaleform") ||
           strstr(name,"TitleFlow");
}
/* The view among sorted views holding a guest address, or NULL. */
static const RuntimeView *view_in(const RuntimeView *list, size_t count, uintptr_t address) {
    size_t lo=0, hi=count;
    while (lo<hi) {
        size_t mid=(lo+hi)/2;
        if (list[mid].start+list[mid].size<=address) lo=mid+1; else hi=mid;
    }
    return lo<count && list[lo].start<=address ? &list[lo] : NULL;
}
static const RuntimeView *view_of(uintptr_t address) { return view_in(views,view_count,address); }
/* The bytes at a guest address (backing view or the image's data) and how many follow. */
static const unsigned char *bytes_at(uintptr_t address, uint64_t *room) {
    if (in_image(address,DATA_START,DATA_END,0)) {
        *room=(uintptr_t)image+DATA_END-address;
        return (const unsigned char *)address;
    }
    const RuntimeView *v=view_of(address);
    *room=v ? v->start+v->size-address : 0;
    return v ? v->data+(address-v->start) : NULL;
}
/* The last named object starting at or before an address. */
static const Object *object_at(uintptr_t address) {
    size_t lo=0, hi=object_count;
    while (lo<hi) {
        size_t mid=(lo+hi)/2;
        if (objects[mid].address<=address) lo=mid+1; else hi=mid;
    }
    return lo ? &objects[lo-1] : NULL;
}
static int by_address(const void *a, const void *b) {
    uintptr_t x=((const Object *)a)->address, y=((const Object *)b)->address;
    return x<y ? -1 : x>y;
}
static int by_name(const void *a, const void *b) {
    return strcmp(*(const char *const *)a,*(const char *const *)b);
}
static int by_class(const void *a, const void *b) {
    const Object *x=&objects[((const Menu *)a)->object], *y=&objects[((const Menu *)b)->object];
    int c=strcmp(x->name,y->name);
    return c ? c : x->address<y->address ? -1 : x->address>y->address;
}
static void add_object(uintptr_t address, const char *name) {
    if (object_count==object_capacity) {
        size_t capacity=object_capacity ? object_capacity*2 : 65536;
        Object *next=realloc(objects,capacity*sizeof(*objects));
        if (!next) return;
        objects=next; object_capacity=capacity;
    }
    objects[object_count++]=(Object){address,name};
}
/* Every qword of guest memory, then of the image's data (image addresses are guest addresses). */
typedef void (*QwordVisitor)(uintptr_t at, uint64_t value);
static void each_qword(QwordVisitor visit, int with_image) {
    for (size_t v=0;v<view_count;++v) {
        const uint64_t *q=(const uint64_t *)views[v].data;
        for (uint64_t i=0, n=views[v].size/8;i<n;++i) visit(views[v].start+i*8,q[i]);
    }
    if (with_image)
        for (uint64_t off=DATA_START;off+8<=DATA_END;off+=8) visit((uintptr_t)image+off,*(const uint64_t *)(image+off));
}
static void find_object(uintptr_t at, uint64_t value) {
    if (value-((uintptr_t)image+VTABLES_START)>=VTABLES_END-VTABLES_START) return;
    const char *name=class_name((uintptr_t)value);
    if (name) add_object(at,name);
}
static uintptr_t menu_lo, menu_hi;
static size_t address_hash(uint64_t address) { return (size_t)((address>>3)*UINT64_C(0x9E3779B97F4A7C15)>>32); }
static void find_referrer(uintptr_t at, uint64_t value) {
    if (value<menu_lo || value>menu_hi || (value & 7)) return;
    for (size_t h=address_hash(value)&menu_index_mask;menu_index[h];h=(h+1)&menu_index_mask) {
        Menu *m=&menus[menu_index[h]-1];
        if (objects[m->object].address!=value) continue;
        if (m->count<MAX_REFERRERS) m->referrers[m->count++]=at;
        return;
    }
}
static void print_address(FILE *f, uintptr_t x) {
    if (in_image(x,0,DATA_END,0)) {
        fprintf(f,"image+%" PRIxPTR,x-(uintptr_t)image);
        return;
    }
    const Object *o=object_at(x);
    if (o && x-o->address<NEAR_OBJECT) {
        fprintf(f,"%s@%" PRIxPTR,o->name,o->address);
        if (x!=o->address) fprintf(f,"+%" PRIxPTR,x-o->address);
        return;
    }
    fprintf(f,"%" PRIxPTR,x);
}
/* What a qword could be: a pointer to something known, a string, or two floats. */
static void describe(FILE *f, uint64_t x) {
    uintptr_t p=(uintptr_t)x;
    if (in_image(p,VTABLES_START,VTABLES_END,8)) {
        const char *name=class_name(p);
        if (name) { fprintf(f,"  vtable %s",name); return; }
    }
    if (in_image(p,0,TEXT_END,1)) {
        const char *s=(const char *)p;
        size_t n=0, room=(uintptr_t)image+TEXT_END-p;
        while (n<room && n<48 && s[n]>=0x20 && s[n]<0x7f) ++n;
        fprintf(f,"  image+%" PRIxPTR,p-(uintptr_t)image);
        if (n>=4 && n<room && !s[n]) fprintf(f," \"%s\"",s);
        return;
    }
    uint64_t room;
    const unsigned char *target=bytes_at(p,&room);
    if (target) {
        fputs(in_image(p,DATA_START,DATA_END,0) ? "  " : "  -> ",f);
        print_address(f,p);
        const Object *o=object_at(p);
        if ((!o || p-o->address>=NEAR_OBJECT) && !(p&7) && room>=8) {
            uint64_t first;
            memcpy(&first,target,8);
            if (in_image((uintptr_t)first,VTABLES_START,VTABLES_END,8)) fprintf(f," (vt image+%" PRIxPTR ")",(uintptr_t)first-(uintptr_t)image);
        }
        return;
    }
    float halves[2];
    memcpy(halves,&x,8);
    int plausible=1;
    for (int i=0;i<2;++i) {
        float a=halves[i]<0 ? -halves[i] : halves[i];
        if (!(a==0.0f || (a>=1e-3f && a<=1e5f))) plausible=0;
    }
    if (plausible) fprintf(f,"  (%g, %g)",halves[0],halves[1]);
}
/* Adds an address to the dumped set; 0 when it was there already. */
static int mark_dumped(uintptr_t address) {
    for (size_t h=address_hash(address)&dumped_mask;;h=(h+1)&dumped_mask) {
        if (dumped[h]==address) return 0;
        if (!dumped[h]) { dumped[h]=address; return 1; }
    }
}
/* An object's bytes, one line per nonzero qword; pointers to guest data are queued to be
 * dumped in turn while depth allows. */
static void dump_bytes(FILE *f, uintptr_t address, const unsigned char *bytes, uint64_t size, int depth) {
    for (uint64_t off=0;off+8<=size;off+=8) {
        uint64_t x;
        memcpy(&x,bytes+off,8);
        if (!x) continue;
        fprintf(f,"   +%03" PRIx64 " %016" PRIx64,off,x);
        describe(f,x);
        fputc('\n',f);
        uint64_t room;
        if (depth<FOLLOW_DEPTH && followed_count<MAX_FOLLOWED && !(x & 7) &&
            !in_image((uintptr_t)x,0,VTABLES_END,0) && bytes_at((uintptr_t)x,&room) && room>=8 && mark_dumped((uintptr_t)x))
            followed[followed_count++]=(Followed){(uintptr_t)x,address,(uint32_t)off,depth+1};
    }
}
static void dump(FILE *f, const Menu *m) {
    const Object *o=&objects[m->object];
    uint64_t room, first;
    const unsigned char *bytes=bytes_at(o->address,&room);
    if (!bytes || room<8) return;
    memcpy(&first,bytes,8);
    fprintf(f,"\n-- %s @ %" PRIxPTR "  vt image+%" PRIx64 "\n   referenced by:",o->name,o->address,first-(uintptr_t)image);
    for (int i=0;i<m->count;++i) { fputs(i ? ", " : " ",f); print_address(f,m->referrers[i]); }
    if (m->count==MAX_REFERRERS) fputs(", ...",f);
    fputc('\n',f);
    dump_bytes(f,o->address,bytes,room<OBJECT_BYTES ? room : OBJECT_BYTES,0);
}
/* Something a dumped object points at: an object when it starts with a vtable, else data. */
static void dump_followed(FILE *f, const Followed *d) {
    uint64_t room, first;
    const unsigned char *bytes=bytes_at(d->address,&room);
    if (!bytes || room<8) return;
    memcpy(&first,bytes,8);
    const int object=in_image((uintptr_t)first,VTABLES_START,VTABLES_END,8);
    const char *name=object ? class_name((uintptr_t)first) : NULL;
    fputs("\n-- ",f);
    if (name) fputs(name,f);
    else if (object) fprintf(f,"vt image+%" PRIx64,first-(uintptr_t)image);
    else fputs("data",f);
    fprintf(f," @ %" PRIxPTR "  (depth %d, from ",d->address,d->depth);
    print_address(f,d->parent);
    fprintf(f," +%03x)\n",d->offset);
    const uint64_t size=object ? OBJECT_BYTES : DATA_BYTES;
    dump_bytes(f,d->address,bytes,room<size ? room : size,d->depth);
}

/* ---- Trace (F8): the game's calls into Scaleform's GFx::Value::ObjectInterface ----
 * Every operation the menus' C++ code does on a movie (member lookups, ActionScript calls,
 * frames, text, display info) goes through this interface's vtable, with names as strings.
 * Each slot gets a thunk: "push slot; jmp common". The common code saves the argument registers
 * (xmm0-7 too), asks trace_call for the original (logging the call while a trace runs), restores
 * them and jumps there. */
#define OBJECT_INTERFACE 0x51B9570u /* the AS3 ValueObjectInterface vtable */
#define INTERFACE_SLOTS 49
#define TRACE_NS UINT64_C(4000000000)
#define TRACE_BYTES (48u << 20)
#define STACK_WINDOW 0x100000       /* strings this far above a call's saved registers: its caller's stack */
#define COMMON_IMM 118              /* the trace_call address in the common code */
#define THUNKS 256                  /* offset of the slot thunks, 8 bytes each */
#define DISPLAY_VISIBLE 48          /* GFx::Value::DisplayInfo: six doubles, then bool Visible */

/* Slots whose v1.09 targets identify the vtable: GetMember, SetMember, Invoke, a text method,
 * AttachMovie. */
static const struct { int slot; uint32_t target; } interface_check[]={
    {5,0x2F1E30u}, {6,0x2F2490u}, {7,0x2F2A10u}, {37,0x2F7040u}, {41,0x2F76F0u},
};
static const char *const slot_names[INTERFACE_SLOTS]={
    [2]="AddRef", [3]="Release", [4]="HasMember", [5]="GetMember", [6]="SetMember", [7]="Invoke",
    [37]="Text37", [38]="Text38", [39]="Text39", [40]="CreateEmptyMovieClip", [41]="AttachMovie",
};
static const uint8_t trace_common[]={
    0x55, 0x48,0x89,0xE5, 0x48,0x83,0xE4,0xF0, 0x48,0x81,0xEC,0xC0,0x00,0x00,0x00,
    0x48,0x89,0x3C,0x24, 0x48,0x89,0x74,0x24,0x08, 0x48,0x89,0x54,0x24,0x10, 0x48,0x89,0x4C,0x24,0x18,
    0x4C,0x89,0x44,0x24,0x20, 0x4C,0x89,0x4C,0x24,0x28, 0x48,0x89,0x44,0x24,0x30,
    0xF3,0x0F,0x7F,0x44,0x24,0x40, 0xF3,0x0F,0x7F,0x4C,0x24,0x50, 0xF3,0x0F,0x7F,0x54,0x24,0x60,
    0xF3,0x0F,0x7F,0x5C,0x24,0x70, 0xF3,0x0F,0x7F,0xA4,0x24,0x80,0x00,0x00,0x00,
    0xF3,0x0F,0x7F,0xAC,0x24,0x90,0x00,0x00,0x00, 0xF3,0x0F,0x7F,0xB4,0x24,0xA0,0x00,0x00,0x00,
    0xF3,0x0F,0x7F,0xBC,0x24,0xB0,0x00,0x00,0x00,
    0x48,0x8B,0x7D,0x08, 0x48,0x89,0xE6, 0x48,0xB8,0,0,0,0,0,0,0,0, 0xFF,0xD0, 0x49,0x89,0xC3,
    0xF3,0x0F,0x6F,0x44,0x24,0x40, 0xF3,0x0F,0x6F,0x4C,0x24,0x50, 0xF3,0x0F,0x6F,0x54,0x24,0x60,
    0xF3,0x0F,0x6F,0x5C,0x24,0x70, 0xF3,0x0F,0x6F,0xA4,0x24,0x80,0x00,0x00,0x00,
    0xF3,0x0F,0x6F,0xAC,0x24,0x90,0x00,0x00,0x00, 0xF3,0x0F,0x6F,0xB4,0x24,0xA0,0x00,0x00,0x00,
    0xF3,0x0F,0x6F,0xBC,0x24,0xB0,0x00,0x00,0x00,
    0x48,0x8B,0x44,0x24,0x30, 0x4C,0x8B,0x4C,0x24,0x28, 0x4C,0x8B,0x44,0x24,0x20, 0x48,0x8B,0x4C,0x24,0x18,
    0x48,0x8B,0x54,0x24,0x10, 0x48,0x8B,0x74,0x24,0x08, 0x48,0x8B,0x3C,0x24,
    0x48,0x89,0xEC, 0x5D, 0x48,0x83,0xC4,0x08, 0x41,0xFF,0xE3,
};
_Static_assert(sizeof(trace_common)==236 && sizeof(trace_common)<=THUNKS,"trace common code");

static uintptr_t interface_original[INTERFACE_SLOTS];
static int traceable, tracing;
static unsigned traces;
static uint64_t trace_start, trace_end;
static HostMutex trace_lock=HOST_MUTEX_INIT;
static char *trace_buffer;
static size_t trace_used, trace_dropped;
static uint64_t slot_calls[INTERFACE_SLOTS];
static float trace_pointer_x, trace_pointer_y;
static int trace_window_w, trace_window_h;
/* Guest memory views for reading call arguments (names live in the game's heap too): two
 * buffers, the pad thread refreshes the idle one every VIEWS_NS and switches; a reader takes
 * microseconds, far from the next refresh of the buffer it read. The probe's views are its own. */
#define VIEWS_NS UINT64_C(1000000000)
static RuntimeView *arg_views[2];
static size_t arg_view_count[2];
static int arg_views_current;
static uint64_t arg_views_ns;
static void refresh_arg_views(void) {
    const int next=1-__atomic_load_n(&arg_views_current,__ATOMIC_ACQUIRE);
    arg_view_count[next]=runtime_memory_views(arg_views[next],MAX_VIEWS);
    __atomic_store_n(&arg_views_current,next,__ATOMIC_RELEASE);
    arg_views_ns=host_monotonic_ns();
}
static uint32_t pad_buttons=UINT32_MAX;
static uint8_t pad_x, pad_y;

/* Up to size bytes at a host address outside guest memory, stopping at a fault. */
static size_t read_host(uintptr_t p, unsigned char *out, size_t size) {
    RuntimeRecoverBuf recover;
    RuntimeRecoverBuf *previous=runtime_fault_recover;
    volatile size_t done=0;
    if (RUNTIME_RECOVER_SET(recover)) { runtime_fault_recover=previous; return done; }
    runtime_fault_recover=&recover;
    for (size_t i=0;i<size;++i) { out[i]=((const volatile unsigned char *)p)[i]; done=i+1; }
    runtime_fault_recover=previous;
    return done;
}
/* Bytes an argument points at: guest memory (backing view), the image, or the caller's stack. */
static size_t read_arg(uintptr_t p, uintptr_t stack, unsigned char *out, size_t size) {
    const int current=__atomic_load_n(&arg_views_current,__ATOMIC_ACQUIRE);
    const RuntimeView *v=view_in(arg_views[current],arg_view_count[current],p);
    if (v) {
        const uint64_t room=v->start+v->size-p;
        const size_t n=room<size ? (size_t)room : size;
        memcpy(out,v->data+(p-v->start),n);
        return n;
    }
    if (in_image(p,0,DATA_END,1)) { size_t n=(uintptr_t)image+DATA_END-p; n=n<size ? n : size; memcpy(out,(const void *)p,n); return n; }
    if (p>=stack && p-stack<STACK_WINDOW) return read_host(p,out,size);
    return 0;
}
/* Appends a C string at p, if p holds one; 0 otherwise. */
static int append_string(char **at, char *end, uintptr_t p, uintptr_t stack) {
    unsigned char s[97];
    size_t n=read_arg(p,stack,s,96), len=0;
    while (len<n && s[len]>=0x20 && s[len]<0x7F) ++len;
    if (!len || len==n || s[len]) return 0;
    s[len]=0;
    *at+=snprintf(*at,(size_t)(end-*at),"\"%s\"",(const char *)s);
    return 1;
}
/* A GFx::Value as qwords: {next, prev, ObjectInterface*, type, value, aux} (managed values are
 * linked into their movie's list). The type's low bits: 2 bool, 3 int, 4 uint, 5 double,
 * 6 string, 7 wide string, 8 object, 9 array, 10 display object, 11 closure; 0x40: managed
 * (strings then point at the string pointer). */
#define VALUE_QWORDS 6
static void append_value(char **at, char *end, const uint64_t q[VALUE_QWORDS], uintptr_t stack) {
    const uint64_t *v=q+2; /* v[0] interface, v[1] type, v[2] value */
    const unsigned type=(unsigned)v[1]&0xFF, base=type&0x0F;
    double d;
    if (*at>=end) return;
    switch (base) {
    case 0: *at+=snprintf(*at,(size_t)(end-*at),"undefined"); return;
    case 1: *at+=snprintf(*at,(size_t)(end-*at),"null"); return;
    case 2: *at+=snprintf(*at,(size_t)(end-*at),"%s",(v[2]&0xFF) ? "true" : "false"); return;
    case 3: *at+=snprintf(*at,(size_t)(end-*at),"%d",(int32_t)v[2]); return;
    case 4: *at+=snprintf(*at,(size_t)(end-*at),"%uu",(uint32_t)v[2]); return;
    case 5: memcpy(&d,&v[2],8); *at+=snprintf(*at,(size_t)(end-*at),"%g",d); return;
    case 6: {
        uintptr_t s=(uintptr_t)v[2];
        if (type&0x40) { uint64_t inner; if (read_arg(s,stack,(unsigned char *)&inner,8)!=8) break; s=(uintptr_t)inner; }
        if (append_string(at,end,s,stack)) return;
        break;
    }
    case 7: {
        uint16_t w[48];
        size_t n=read_arg((uintptr_t)v[2],stack,(unsigned char *)w,sizeof(w))/2, len=0;
        *at+=snprintf(*at,(size_t)(end-*at),"L\"");
        while (len<n && w[len] && *at<end-8) {
            *at+=w[len]>=0x20 && w[len]<0x7F ? snprintf(*at,(size_t)(end-*at),"%c",(char)w[len])
                                             : snprintf(*at,(size_t)(end-*at),"\\u%04x",w[len]);
            ++len;
        }
        *at+=snprintf(*at,(size_t)(end-*at),"\"");
        return;
    }
    case 8: case 9: case 10: case 11:
        *at+=snprintf(*at,(size_t)(end-*at),"%s@%" PRIx64,base==8 ? "object" : base==9 ? "array" : base==10 ? "clip" : "closure",v[2]);
        return;
    default: break;
    }
    *at+=snprintf(*at,(size_t)(end-*at),"type%x:%" PRIx64,type,v[2]);
}
/* An argument register: a string, a GFx::Value (its ObjectInterface is this one), or a number. */
static void append_arg(char **at, char *end, uint64_t x, uint64_t self, uintptr_t stack) {
    uint64_t v[VALUE_QWORDS];
    if (*at>=end) return;
    *at+=snprintf(*at,(size_t)(end-*at)," ");
    if (x>=0x10000 && append_string(at,end,(uintptr_t)x,stack)) return;
    if (x>=0x10000 && read_arg((uintptr_t)x,stack,(unsigned char *)v,sizeof(v))==sizeof(v) && v[2]==self) {
        *at+=snprintf(*at,(size_t)(end-*at),"{");
        append_value(at,end,v,stack);
        *at+=snprintf(*at,(size_t)(end-*at),"}");
        return;
    }
    *at+=x<0x100000 ? snprintf(*at,(size_t)(end-*at),"%" PRIu64,x) : snprintf(*at,(size_t)(end-*at),"%" PRIx64,x);
}
static void trace_append(const char *line, size_t n) {
    host_lock(&trace_lock);
    if (__atomic_load_n(&tracing,__ATOMIC_ACQUIRE)) {
        if (trace_used+n<=TRACE_BYTES) { memcpy(trace_buffer+trace_used,line,n); trace_used+=n; }
        else ++trace_dropped;
    }
    host_unlock(&trace_lock);
}
/* Called by the common code for every interface call; returns where the call goes. Logging:
 * the slot, the thread, the object (pdata) and the arguments; Invoke's argument array. */
static void menu_observe(uint64_t slot, const uint64_t *regs);
static ABI uint64_t trace_call(uint64_t slot, const uint64_t *regs) {
    if (slot>=INTERFACE_SLOTS) slot=0;
    const uint64_t original=interface_original[slot];
    menu_observe(slot,regs);
    if (!__atomic_load_n(&tracing,__ATOMIC_ACQUIRE)) return original;
    __atomic_add_fetch(&slot_calls[slot],1,__ATOMIC_RELAXED);
    const uint64_t now=host_monotonic_ns();
    if (slot==2 || slot==3 || now>=trace_end) return original;
    char line[1536], *at=line, *end=line+sizeof(line)-2;
    const uintptr_t stack=(uintptr_t)regs;
    at+=snprintf(at,(size_t)(end-at),"%9.3f T%-6" PRIu64 " %-11s obj=%" PRIx64,(now-trace_start)/1e6,
                 (uint64_t)host_thread_id(),slot_names[slot] ? slot_names[slot] : "",regs[1]);
    if (!slot_names[slot]) at+=snprintf(at,(size_t)(end-at)," slot%" PRIu64,slot);
    for (int r=2;r<6 && at<end;++r) append_arg(&at,end,regs[r],regs[0],stack);
    if (slot==7 && regs[5] && regs[5]<=16 && at<end) { /* Invoke(pdata, result, name, args, count) */
        at+=snprintf(at,(size_t)(end-at)," args(");
        for (uint64_t i=0;i<regs[5] && at<end;++i) {
            uint64_t v[VALUE_QWORDS];
            if (read_arg((uintptr_t)(regs[4]+i*sizeof(v)),stack,(unsigned char *)v,sizeof(v))!=sizeof(v)) break;
            if (i) at+=snprintf(at,(size_t)(end-at),", ");
            append_value(&at,end,v,stack);
        }
        if (at<end) at+=snprintf(at,(size_t)(end-at),")");
    }
    if (slot==29 && at<end) { /* SetDisplayInfo: the values set (checks the DisplayInfo layout) */
        double d[6];
        unsigned char visible;
        if (read_arg((uintptr_t)regs[2],stack,(unsigned char *)d,sizeof(d))==sizeof(d) &&
            read_arg((uintptr_t)regs[2]+DISPLAY_VISIBLE,stack,&visible,1)==1)
            at+=snprintf(at,(size_t)(end-at)," info(x=%g y=%g rot=%g xs=%g ys=%g alpha=%g visible=%u)",
                         d[0],d[1],d[2],d[3],d[4],d[5],visible);
    }
    if (at>end) at=end;
    *at++='\n';
    trace_append(line,(size_t)(at-line));
    return original;
}
/* Hooks the interface's vtable once the image is mapped (BB_MENU_PROBE=1). */
static void trace_configure(void) {
    uintptr_t *table=(uintptr_t *)(image+OBJECT_INTERFACE);
    for (size_t i=0;i<sizeof(interface_check)/sizeof(*interface_check);++i)
        if (table[interface_check[i].slot]!=(uintptr_t)image+interface_check[i].target) {
            puts("Menu trace: not the v1.09 Scaleform interface, off");
            return;
        }
    trace_buffer=malloc(TRACE_BYTES);
    arg_views[0]=malloc(MAX_VIEWS*sizeof(*arg_views[0]));
    arg_views[1]=malloc(MAX_VIEWS*sizeof(*arg_views[1]));
#ifdef _WIN32
    unsigned char *code=VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
#else
    unsigned char *code=mmap(NULL,4096,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (code==MAP_FAILED) code=NULL;
#endif
    if (!code || !trace_buffer || !arg_views[0] || !arg_views[1]) { puts("Menu trace: off (no memory)"); return; }
    memcpy(code,trace_common,sizeof(trace_common));
    const uint64_t target=(uint64_t)(uintptr_t)trace_call;
    memcpy(code+COMMON_IMM,&target,8);
    for (int i=0;i<INTERFACE_SLOTS;++i) {
        unsigned char *thunk=code+THUNKS+i*8;
        const int32_t rel=-(int32_t)(THUNKS+i*8+7); /* jmp back to the common code at 0 */
        thunk[0]=0x6A; thunk[1]=(unsigned char)i; thunk[2]=0xE9;
        memcpy(thunk+3,&rel,4);
        interface_original[i]=table[i];
    }
    for (int i=0;i<INTERFACE_SLOTS;++i) __atomic_store_n(&table[i],(uintptr_t)(code+THUNKS+i*8),__ATOMIC_RELEASE);
    traceable=1;
}
static void trace_begin(float pointer_x, float pointer_y, int window_w, int window_h) {
    if (!traceable || __atomic_load_n(&tracing,__ATOMIC_ACQUIRE)) return;
    host_lock(&trace_lock);
    refresh_arg_views();
    trace_used=trace_dropped=0;
    memset(slot_calls,0,sizeof(slot_calls));
    trace_pointer_x=pointer_x; trace_pointer_y=pointer_y; trace_window_w=window_w; trace_window_h=window_h;
    trace_start=host_monotonic_ns();
    trace_end=trace_start+TRACE_NS;
    pad_buttons=UINT32_MAX;
    ++traces;
    __atomic_store_n(&tracing,1,__ATOMIC_RELEASE);
    host_unlock(&trace_lock);
    printf("%sMenu trace %u: recording %.0f s of menu calls%s\n",on,traces,TRACE_NS/1e9,off);
}
/* Pad changes during a trace become lines too: when the game saw a direction or a button. */
static void trace_pad(uint32_t buttons, uint8_t left_x, uint8_t left_y) {
    const uint8_t x=left_x<64 ? 0 : left_x>192 ? 2 : 1, y=left_y<64 ? 0 : left_y>192 ? 2 : 1;
    if (buttons==pad_buttons && x==pad_x && y==pad_y) return;
    pad_buttons=buttons; pad_x=x; pad_y=y;
    char line[128];
    const int n=snprintf(line,sizeof(line),"%9.3f == pad buttons=%x stick=%s%s\n",(host_monotonic_ns()-trace_start)/1e6,buttons,
                         y==0 ? "up" : y==2 ? "down" : "",x==0 ? "left" : x==2 ? "right" : (y==1 ? "center" : ""));
    trace_append(line,(size_t)n);
}
static void trace_finish(void) {
    int running=1;
    if (host_monotonic_ns()<trace_end ||
        !__atomic_compare_exchange_n(&tracing,&running,0,0,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED)) return;
    host_lock(&trace_lock); /* appends still in flight finish first */
    host_unlock(&trace_lock);
    char path[512], stamp[32];
    time_t now=time(NULL);
    strftime(stamp,sizeof(stamp),"%Y%m%d-%H%M%S",localtime(&now));
#ifdef _WIN32
    _mkdir(log_dir());
#else
    mkdir(log_dir(),0755);
#endif
    snprintf(path,sizeof(path),"%s/menu-trace-%s-%u.txt",log_dir(),stamp,traces);
    FILE *f=fopen(path,"w");
    if (!f) { printf("Menu trace %u: cannot write %s\n",traces,path); return; }
    fprintf(f,"Menu trace %u, %s\nPointer %.0f,%.0f in a %dx%d window (points)\nCalls per slot:",traces,stamp,
            trace_pointer_x,trace_pointer_y,trace_window_w,trace_window_h);
    for (int i=0;i<INTERFACE_SLOTS;++i)
        if (slot_calls[i] && slot_names[i]) fprintf(f," %s=%" PRIu64,slot_names[i],slot_calls[i]);
        else if (slot_calls[i]) fprintf(f," slot%d=%" PRIu64,i,slot_calls[i]);
    fprintf(f,"\n%zu lines dropped\n\n",trace_dropped);
    fwrite(trace_buffer,1,trace_used,f);
    fclose(f);
    printf("%sMenu trace %u: %zu bytes -> %s (done)%s\n",on,traces,trace_used,path,off);
}

/* ---- Mouse in menus ----
 * A menu's selectable items are clips named Item_<a>_<b> under a container clip, each holding
 * a "Cursor" clip: GetMember(container,"Item_1_0"), GetMember(item,"Cursor"), then calls on the
 * cursor. Each refresh (the menu opening, the selection moving) restarts Item_0_0's cursor
 * ("Loop", then SetDisplayInfo, slot 29), looks at every cursor (GetDisplayInfo, slot 28) and
 * ends with SetDisplayInfo on the selected item's cursor: the last cursor set is the selection.
 * The hook learns items and selections from those calls; a container the game asks for its
 * "CurrentItem" is also searched for Item_<a>_<b> clips with a cursor right away, so a menu is
 * known before its first refresh. On the game's own thread it asks Scaleform for each item's
 * bounds (getBounds(stage): the cursor's, which frame what is shown, else the item's, which may
 * hold hidden text). While a menu is on screen (the game touches its container every frame) the
 * window frees the mouse; the item under the pointer is reached with d-pad presses toward its
 * top left corner, each waiting for the selection to move (a selection unknown yet is revealed
 * by one press; one that comes back to an item already passed gives up: a list too short, an
 * empty row); the left button confirms, the right one cancels. Menu movies are 1920x1080
 * stages over the window. */
#define MAX_ITEMS 256
#define MAX_CONTAINERS 16
#define STAGE_W 1920.0f
#define STAGE_H 1080.0f
#define MENU_IDLE_NS UINT64_C(300000000)   /* a container untouched this long: its menu closed */
#define BOUNDS_NS UINT64_C(1000000000)     /* item bounds are measured again after this */
#define MEASURES_PER_CALL 4
#define PRESS_NS UINT64_C(34000000)        /* two frames held, two released: the shortest steps the game takes */
#define GAP_NS UINT64_C(34000000)          /* released between presses: each is a new press */
#define SETTLE_NS UINT64_C(350000000)      /* a press that moved nothing this long: out of reach */
#define REFRESH_NS UINT64_C(5000000)       /* a selection this fresh may be mid-refresh (Item_0_0 first) */
#define MAX_STEPS 12                       /* presses toward one target before giving up on it */
#define APPROX_RADIUS 0.6f                 /* items known as points: a pointer within this many pitches */
#define PROBE_NS UINT64_C(2000000000)      /* a container searched for items is searched again after this */
#define PROBE_ROWS 16
#define PROBE_COLUMNS 10
#define MAX_VISITED 24
/* libScePad buttons (runtime_pad.c). */
#define PAD_UP 0x10u
#define PAD_RIGHT 0x20u
#define PAD_DOWN 0x40u
#define PAD_LEFT 0x80u
#define PAD_CIRCLE 0x2000u
#define PAD_CROSS 0x4000u

/* GFx::Value (0x30 bytes): a managed value (type bit 0x40) is linked into its movie's list
 * (next, prev) until released, so every one returned to us is released before it goes. */
typedef struct { uint64_t next, prev, oi; uint32_t type, pad; uint64_t value, aux; } GfxValue;
_Static_assert(sizeof(GfxValue)==VALUE_QWORDS*8,"GFx::Value layout");
typedef ABI uint64_t (*DisplayFn)(uint64_t oi, uint64_t pdata, double *info);
typedef ABI uint64_t (*GetMemberFn)(uint64_t oi, uint64_t pdata, const char *name, GfxValue *out, uint64_t isdobj);
typedef ABI uint64_t (*InvokeFn)(uint64_t oi, uint64_t pdata, GfxValue *result, const char *name, const GfxValue *args,
                                 uint64_t count, uint64_t isdobj);
typedef ABI void (*ReleaseFn)(uint64_t oi, GfxValue *value, uint64_t data);

typedef struct {
    uint64_t oi, container, item, cursor;
    int a, b;                  /* Item_<a>_<b> */
    float x0, y0, x1, y1;      /* stage bounds, valid with bounds_ns; a point when approx */
    uint64_t bounds_ns, seen_ns;
    int approx;                /* from display info: the item's origin only */
} MenuItem;
/* active_ns: when its selection last moved or its items were first found (the newest is on top:
 * a submenu over the menu that is still drawn behind it). */
typedef struct {
    uint64_t container, touch_ns, select_ns, probe_ns, active_ns;
    uint64_t behind;           /* a press here moved this other container: in the background while it lives;
                                * COVERED: presses here went unanswered (a screen over it the mouse does not
                                * know, like the equipment slots), until its selection moves again */
    uint64_t covered_ns;
    int selected, unanswered;
} MenuContainer;
#define COVERED UINT64_MAX
#define UNANSWERED_MAX 2

static int menu_mouse;
static HostMutex menu_lock=HOST_MUTEX_INIT;
static MenuItem menu_items[MAX_ITEMS];
static int menu_item_count;
static MenuContainer containers[MAX_CONTAINERS];
static __thread int pending_item=-1, pending_cursor=-1, measuring;
static int bounds_broken;            /* a measurement failed badly: no more */
/* Pad thread: the navigation. */
static int menu_shown, blocked=-1, nav_target=-1, confirm_target=-1, press_from=-1, steps;
static int visited[MAX_VISITED], visited_count;
static float last_pointer_x=-1, last_pointer_y=-1;
static uint32_t press_button, previous_free;
static uint64_t press_container, press_ns; /* where the last d-pad press was aimed, and when */
static uint64_t press_until, gap_until, settle_until;
static uint32_t confirm_button=PAD_CROSS, cancel_button=PAD_CIRCLE;

static void menu_note(const char *format, ...) __attribute__((format(printf,1,2)));
static void menu_note(const char *format, ...) {
    char line[512];
    int n=snprintf(line,sizeof(line),"%9.3f == menu: ",(host_monotonic_ns()-trace_start)/1e6);
    va_list args;
    va_start(args,format);
    n+=vsnprintf(line+n,sizeof(line)-(size_t)n-1,format,args);
    va_end(args);
    if (n>(int)sizeof(line)-2) n=(int)sizeof(line)-2;
    line[n++]='\n';
    trace_append(line,(size_t)n);
}
static int read_name(uintptr_t p, uintptr_t stack, char *out, size_t size) {
    size_t n=p>=0x10000 ? read_arg(p,stack,(unsigned char *)out,size-1) : 0, len=0;
    while (len<n && out[len]>=0x20 && out[len]<0x7F) ++len;
    if (len==n || out[len]) return 0;
    return 1;
}
static MenuContainer *container_of(uint64_t container, int add) {
    MenuContainer *oldest=&containers[0];
    for (int i=0;i<MAX_CONTAINERS;++i) {
        if (containers[i].container==container) return &containers[i];
        if (containers[i].touch_ns<oldest->touch_ns) oldest=&containers[i];
    }
    if (!add) return NULL;
    *oldest=(MenuContainer){container,host_monotonic_ns(),0,0,0,0,0,-1,0};
    return oldest;
}
static int item_index(uint64_t container, int a, int b) {
    int oldest=0;
    for (int i=0;i<menu_item_count;++i) {
        if (menu_items[i].container==container && menu_items[i].a==a && menu_items[i].b==b) return i;
        if (menu_items[i].seen_ns<menu_items[oldest].seen_ns) oldest=i;
    }
    if (menu_item_count<MAX_ITEMS) return menu_item_count++;
    for (int c=0;c<MAX_CONTAINERS;++c) if (containers[c].selected==oldest) containers[c].selected=-1;
    return oldest;
}
static int item_by(uint64_t clip, int cursor) {
    for (int i=0;i<menu_item_count;++i)
        if (clip && (cursor ? menu_items[i].cursor : menu_items[i].item)==clip) return i;
    return -1;
}
static void value_release(GfxValue *v) {
    if ((v->type&0x40) && v->oi) ((ReleaseFn)interface_original[3])(v->oi,v,v->value);
    memset(v,0,sizeof(*v));
}
static int number_of(const GfxValue *v, double *out) {
    switch (v->type&0x0F) {
    case 5: memcpy(out,&v->value,8); return 1;
    case 3: *out=(int32_t)v->value; return 1;
    case 4: *out=(uint32_t)v->value; return 1;
    default: return 0;
    }
}
/* A clip's bounds on the stage: getBounds(stage), then the rectangle's x, y, width, height. */
static int bounds_of(uint64_t oi, uint64_t clip, const GfxValue *stage, float out[4]) {
    const GetMemberFn get=(GetMemberFn)interface_original[5];
    const InvokeFn invoke=(InvokeFn)interface_original[7];
    GfxValue rect;
    memset(&rect,0,sizeof(rect));
    int ok=0;
    if ((invoke(oi,clip,&rect,"getBounds",stage,1,1)&0xFF) && (rect.type&0x0F)==8) {
        static const char *const names[4]={"x","y","width","height"};
        double r[4];
        ok=1;
        for (int k=0;k<4 && ok;++k) {
            GfxValue v;
            memset(&v,0,sizeof(v));
            ok=(get(oi,rect.value,names[k],&v,0)&0xFF) && number_of(&v,&r[k]);
            value_release(&v);
        }
        if (ok) { out[0]=(float)r[0]; out[1]=(float)r[1]; out[2]=(float)(r[0]+r[2]); out[3]=(float)(r[1]+r[3]); }
    }
    value_release(&rect);
    return ok;
}
/* An item's bounds on the stage, through the interface's originals (not traced). The container is
 * alive (the game is calling it), so the item is fetched from it afresh and never kept: menus
 * rebuild their clips. getBounds(stage) where the interface reaches AS3 properties; else the
 * item's and the container's display info (x, y, scale) give the item's origin (*approx). */
static int measure(uint64_t oi, uint64_t container, int a, int b, float out[4], int *approx) {
    const GetMemberFn get=(GetMemberFn)interface_original[5];
    const DisplayFn display=(DisplayFn)interface_original[28];
    GfxValue item, stage;
    memset(&item,0,sizeof(item)); memset(&stage,0,sizeof(stage));
    char name[24];
    snprintf(name,sizeof(name),"Item_%d_%d",a,b);
    int ok=0;
    *approx=0;
    if ((get(oi,container,name,&item,1)&0xFF) && (item.type&0x0F)==10) {
        if ((get(oi,item.value,"stage",&stage,1)&0xFF) && ((stage.type&0x0F)==10 || (stage.type&0x0F)==8)) {
            GfxValue cursor;
            memset(&cursor,0,sizeof(cursor));
            if ((get(oi,item.value,"Cursor",&cursor,1)&0xFF) && (cursor.type&0x0F)==10)
                ok=bounds_of(oi,cursor.value,&stage,out) && out[2]-out[0]>4 && out[3]-out[1]>4;
            value_release(&cursor);
            if (!ok) ok=bounds_of(oi,item.value,&stage,out);
        }
        if (!ok) {
            double di[64], ci[64]; /* DisplayInfo: x, y, rotation, xscale, yscale (percent), alpha, ... */
            memset(di,0,sizeof(di)); memset(ci,0,sizeof(ci));
            display(oi,item.value,di);
            display(oi,container,ci);
            if (ci[3]>0 && ci[3]<10000 && ci[4]>0 && ci[4]<10000) {
                out[0]=out[2]=(float)(ci[0]+di[0]*ci[3]/100);
                out[1]=out[3]=(float)(ci[1]+di[1]*ci[4]/100);
                ok=*approx=1;
            }
        }
    }
    value_release(&stage);
    value_release(&item);
    return ok;
}
/* The container's Item_<a>_<b> clips that have a cursor, fetched afresh (the container is alive:
 * the game is calling it): rows until one has no Item_<a>_0, columns until a gap. */
typedef struct { int a, b; uint64_t item, cursor; } Found;
static int probe_items(uint64_t oi, uint64_t container, Found *found, int max) {
    const GetMemberFn get=(GetMemberFn)interface_original[5];
    int count=0;
    for (int a=0;a<PROBE_ROWS;++a) {
        int row=0;
        for (int b=0;b<PROBE_COLUMNS && count<max;++b) {
            char name[24];
            snprintf(name,sizeof(name),"Item_%d_%d",a,b);
            GfxValue item, cursor;
            memset(&item,0,sizeof(item)); memset(&cursor,0,sizeof(cursor));
            const int is_item=(get(oi,container,name,&item,1)&0xFF) && (item.type&0x0F)==10;
            if (is_item) {
                ++row;
                if ((get(oi,item.value,"Cursor",&cursor,1)&0xFF) && (cursor.type&0x0F)==10)
                    found[count++]=(Found){a,b,item.value,cursor.value};
            }
            value_release(&cursor);
            value_release(&item);
            if (!is_item) break;
        }
        if (!row) break;
    }
    return count;
}
static int cursor_items(uint64_t container);
/* Every interface call (the hook, before the original runs). */
static void menu_observe(uint64_t slot, const uint64_t *regs) {
    if (!menu_mouse || measuring || slot==2 || slot==3) return;
    const uint64_t obj=regs[1], now=host_monotonic_ns();
    const uintptr_t stack=(uintptr_t)regs;
    int measure_now=0, probe_now=0;
    host_lock(&menu_lock);
    /* The call after GetMember(container,"Item_a_b") is on that item; the one after
     * GetMember(item,"Cursor") on its cursor. */
    if (pending_item>=0) {
        if (obj!=menu_items[pending_item].container) menu_items[pending_item].item=obj;
        pending_item=-1;
    } else if (pending_cursor>=0) {
        if (obj!=menu_items[pending_cursor].item) menu_items[pending_cursor].cursor=obj;
        pending_cursor=-1;
    }
    char name[40];
    if (slot==5 && read_name((uintptr_t)regs[2],stack,name,sizeof(name))) {
        int a, b, end=0;
        if (!strncmp(name,"Item_",5) && sscanf(name,"Item_%d_%d%n",&a,&b,&end)==2 && !name[end]) {
            const int i=item_index(obj,a,b);
            MenuItem *m=&menu_items[i];
            if (m->container!=obj || m->a!=a || m->b!=b) *m=(MenuItem){regs[0],obj,0,0,a,b,0,0,0,0,0,now,0};
            m->seen_ns=now;
            pending_item=i;
            if (__atomic_load_n(&menu_shown,__ATOMIC_RELAXED)) {
                MenuContainer *c=container_of(obj,1);
                if (!c->probe_ns || now-c->probe_ns>PROBE_NS) { c->probe_ns=now; probe_now=1; }
            }
        } else if (!strcmp(name,"Cursor")) {
            pending_cursor=item_by(obj,0);
        } else if (!strcmp(name,"CurrentItem")) {
            MenuContainer *c=container_of(obj,1);
            if (!c->probe_ns || now-c->probe_ns>PROBE_NS) { c->probe_ns=now; probe_now=1; }
        }
    }
    if (slot==29) { /* SetDisplayInfo on a cursor: the last one of a refresh is the selection */
        const int i=item_by(obj,1);
        if (i>=0) {
            MenuContainer *c=container_of(menu_items[i].container,1);
            if (c->selected!=i) menu_note("selected Item_%d_%d (container %" PRIx64 ")",menu_items[i].a,menu_items[i].b,c->container);
            c->selected=i; c->select_ns=c->touch_ns=c->active_ns=now; c->unanswered=0;
        }
    }
    MenuContainer *c=container_of(obj,0);
    if (c) { c->touch_ns=now; measure_now=!bounds_broken; }
    host_unlock(&menu_lock);
    if (probe_now) {
        Found found[PROBE_ROWS*PROBE_COLUMNS];
        measuring=1;
        const int count=probe_items(regs[0],obj,found,PROBE_ROWS*PROBE_COLUMNS);
        measuring=0;
        host_lock(&menu_lock);
        MenuContainer *found_in=container_of(obj,1);
        if (count && !cursor_items(obj)) found_in->active_ns=now;
        for (int k=0;k<count;++k) {
            const int i=item_index(obj,found[k].a,found[k].b);
            MenuItem *m=&menu_items[i];
            if (m->container!=obj || m->a!=found[k].a || m->b!=found[k].b)
                *m=(MenuItem){regs[0],obj,0,0,found[k].a,found[k].b,0,0,0,0,0,now,0};
            m->item=found[k].item; m->cursor=found[k].cursor; m->seen_ns=now;
        }
        host_unlock(&menu_lock);
        if (count) menu_note("found %d items with a cursor (container %" PRIx64 ")",count,obj);
    }
    if (!measure_now) return;
    /* On the container's frame call: measure a few of its items whose bounds are old. */
    for (int n=0;n<MEASURES_PER_CALL;) {
        MenuItem m;
        int index=-1;
        host_lock(&menu_lock);
        for (int i=0;i<menu_item_count;++i)
            if (menu_items[i].container==obj && menu_items[i].cursor &&
                (!menu_items[i].bounds_ns || now-menu_items[i].bounds_ns>BOUNDS_NS)) { index=i; m=menu_items[i]; break; }
        if (index>=0) menu_items[index].bounds_ns=now; /* tried, even if it fails */
        host_unlock(&menu_lock);
        if (index<0) break;
        ++n;
        float r[4]={0,0,0,0};
        int approx=0;
        measuring=1;
        const int ok=measure(regs[0],obj,m.a,m.b,r,&approx);
        measuring=0;
        host_lock(&menu_lock);
        MenuItem *t=&menu_items[index];
        if (t->container==obj && t->a==m.a && t->b==m.b) {
            if (ok) { t->x0=r[0]; t->y0=r[1]; t->x1=r[2]; t->y1=r[3]; t->approx=approx; }
            else { t->x1=t->x0; t->y1=t->y0; t->approx=0; } /* never hit; tried again after BOUNDS_NS */
        }
        host_unlock(&menu_lock);
        menu_note("bounds Item_%d_%d %s (%.0f,%.0f)-(%.0f,%.0f)",m.a,m.b,!ok ? "failed" : approx ? "origin" : "ok",
                  r[0],r[1],r[2],r[3]);
    }
}
/* The item under a stage point: the smallest whose bounds hold it; for items known by their
 * origins (display info), the nearest one within APPROX_RADIUS pitches of its cell's middle
 * (origins at the top left, as the cursors' offsets suggest), along the axes the items spread on
 * (a column gives no width: the pointer only has to be near its side of the screen). */
/* The spacing of a container's measured items: the smallest gap between top left corners along a
 * row (x) and along a column (y); 0 where they do not spread. */
static void pitches(uint64_t container, int approx, float *pitch_x, float *pitch_y) {
    *pitch_x=*pitch_y=0;
    for (int i=0;i<menu_item_count;++i) {
        const MenuItem *m=&menu_items[i];
        if (m->container!=container || !m->cursor || !m->bounds_ns || m->approx!=approx || (!approx && m->x1<=m->x0)) continue;
        for (int j=i+1;j<menu_item_count;++j) {
            const MenuItem *n=&menu_items[j];
            if (n->container!=container || !n->cursor || !n->bounds_ns || n->approx!=approx || (!approx && n->x1<=n->x0)) continue;
            const float dx=m->x0>n->x0 ? m->x0-n->x0 : n->x0-m->x0, dy=m->y0>n->y0 ? m->y0-n->y0 : n->y0-m->y0;
            if (dx>2 && dy<=8 && (!*pitch_x || dx<*pitch_x)) *pitch_x=dx;
            if (dy>2 && dx<=8 && (!*pitch_y || dy<*pitch_y)) *pitch_y=dy;
        }
    }
}
/* An item's area to point at: its bounds, but no further than one pitch from its top left corner
 * (bounds can swell with hidden text or the selected item's description panel). */
static void hit_rect(const MenuItem *m, float pitch_x, float pitch_y, float r[4]) {
    r[0]=m->x0; r[1]=m->y0;
    r[2]=pitch_x && m->x1>m->x0+pitch_x ? m->x0+pitch_x : m->x1;
    r[3]=pitch_y && m->y1>m->y0+pitch_y ? m->y0+pitch_y : m->y1;
}
static int hit(uint64_t container, float sx, float sy) {
    int found=-1, approx=0;
    float best=0, pitch_x=0, pitch_y=0;
    pitches(container,0,&pitch_x,&pitch_y);
    for (int i=0;i<menu_item_count;++i) {
        const MenuItem *m=&menu_items[i];
        if (m->container!=container || !m->cursor || !m->bounds_ns) continue;
        if (m->approx) { approx=1; continue; }
        float r[4];
        hit_rect(m,pitch_x,pitch_y,r);
        const float area=(r[2]-r[0])*(r[3]-r[1]);
        if (r[2]>r[0] && sx>=r[0] && sx<r[2] && sy>=r[1] && sy<r[3] && (found<0 || area<best)) { found=i; best=area; }
    }
    if (found>=0 || !approx) return found;
    pitch_x=pitch_y=0;
    for (int i=0;i<menu_item_count;++i) {
        const MenuItem *m=&menu_items[i];
        if (m->container!=container || !m->cursor || !m->bounds_ns || !m->approx) continue;
        for (int j=i+1;j<menu_item_count;++j) {
            const MenuItem *n=&menu_items[j];
            if (n->container!=container || !n->cursor || !n->bounds_ns || !n->approx) continue;
            const float dx=m->x0>n->x0 ? m->x0-n->x0 : n->x0-m->x0, dy=m->y0>n->y0 ? m->y0-n->y0 : n->y0-m->y0;
            if (dx>2 && (!pitch_x || dx<pitch_x)) pitch_x=dx;
            if (dy>2 && (!pitch_y || dy<pitch_y)) pitch_y=dy;
        }
    }
    for (int i=0;i<menu_item_count;++i) {
        const MenuItem *m=&menu_items[i];
        if (m->container!=container || !m->cursor || !m->bounds_ns || !m->approx) continue;
        float ex=0, ey=0;
        if (pitch_x) ex=(sx-(m->x0+pitch_x/2))/pitch_x;
        else if (sx<m->x0-100 || sx>m->x0+900) continue;
        if (pitch_y) ey=(sy-(m->y0+pitch_y/2))/pitch_y;
        else if (sy<m->y0-100 || sy>m->y0+300) continue;
        if (ex<0) ex=-ex;
        if (ey<0) ey=-ey;
        if (ex>APPROX_RADIUS || ey>APPROX_RADIUS) continue;
        if (found<0 || ex*ex+ey*ey<best) { found=i; best=ex*ex+ey*ey; }
    }
    return found;
}
/* The direction from one item to another by their names: Item_<row>_<column> in every menu seen
 * (title, in-game menu, lists, options), rows down, columns right. Their bounds would mislead:
 * a last entry's cursor can sit elsewhere (an options list's last line, to the right). */
static uint32_t toward(const MenuItem *from, const MenuItem *to) {
    if (to->a!=from->a) return to->a>from->a ? PAD_DOWN : PAD_UP;
    return to->b>from->b ? PAD_RIGHT : PAD_LEFT;
}
static int cursor_items(uint64_t container) {
    int n=0;
    for (int i=0;i<menu_item_count;++i) n+=menu_items[i].container==container && menu_items[i].cursor;
    return n;
}
/* Pad thread: whether a menu is on screen (the window frees the mouse), and the buttons the
 * pointer adds. Moving onto an item makes it the target, reached with d-pad presses toward it;
 * keys and the wheel stay free otherwise (a resting pointer does not pull the selection back).
 * The left button targets the item under the pointer and confirms on it; the right one cancels. */
static uint32_t menu_pad(float pointer_x, float pointer_y, int window_w, int window_h, uint32_t free_buttons) {
    const uint64_t now=host_monotonic_ns();
    uint32_t out=0;
    host_lock(&menu_lock);
    /* The menu on screen: the container whose selection moved last, among those touched lately. */
    MenuContainer *menu=NULL;
    int menus=0;
    for (int i=0;i<MAX_CONTAINERS;++i) {
        MenuContainer *c=&containers[i];
        if (c->behind==COVERED) { if (c->select_ns>c->covered_ns) c->behind=0; }
        else if (c->behind) { /* back in front once the container that took the focus has closed */
            const MenuContainer *front=container_of(c->behind,0);
            if (!front || now-front->touch_ns>=MENU_IDLE_NS) c->behind=0;
        }
        if (!c->container || now-c->touch_ns>=MENU_IDLE_NS || (c->selected<0 && cursor_items(c->container)<2)) continue;
        ++menus;
        if (!c->behind && (!menu || c->active_ns>menu->active_ns)) menu=c;
    }
    /* A press aimed at one container that another one answered: that one has the focus. */
    if (press_container && menu && menu->container!=press_container && menu->select_ns>press_ns) {
        MenuContainer *aimed=container_of(press_container,0);
        if (aimed) {
            aimed->behind=menu->container;
            menu_note("container %" PRIx64 " answered a press aimed at %" PRIx64 ": it has the focus",menu->container,press_container);
        }
        press_container=0;
    }
    const int shown=menus>0; /* the mouse stays free over a covered menu, but takes no steps there */
    int hovered=-1;
    const int selected=menu ? menu->selected : -1;
    if (menu && window_w>0 && window_h>0) hovered=hit(menu->container,pointer_x*STAGE_W/window_w,pointer_y*STAGE_H/window_h);
    int moved=pointer_x!=last_pointer_x || pointer_y!=last_pointer_y;
    last_pointer_x=pointer_x; last_pointer_y=pointer_y;
    if (shown!=menu_shown) {
        menu_shown=shown;
        /* The pointer starts on the selected item, where the highlight is. */
        float wx=-1, wy=-1;
        if (shown && selected>=0 && menu_items[selected].bounds_ns && window_w>0 && window_h>0) {
            float pitch_x, pitch_y, r[4];
            pitches(menu->container,menu_items[selected].approx,&pitch_x,&pitch_y);
            hit_rect(&menu_items[selected],pitch_x,pitch_y,r);
            if (menu_items[selected].approx) { r[2]=r[0]+pitch_x; r[3]=r[1]+pitch_y; }
            wx=(r[0]+r[2])/2*window_w/STAGE_W; wy=(r[1]+r[3])/2*window_h/STAGE_H;
            last_pointer_x=wx; last_pointer_y=wy;
        }
        bbgpu_menu_active(shown,wx,wy);
        press_until=settle_until=0; blocked=nav_target=confirm_target=-1; steps=0;
        moved=0; /* where the pointer rests when a menu opens is not a choice */
        if (menu) menu_note("on screen (container %" PRIx64 ")%s",menu->container,wx>=0 ? ", pointer on the selection" : "");
    }
    const uint32_t pressed=free_buttons&~previous_free;
    previous_free=free_buttons;
    if (!shown || !menu) { host_unlock(&menu_lock); return 0; }
    if (moved && hovered>=0 && hovered!=nav_target && hovered!=blocked) { nav_target=hovered; steps=visited_count=0; }
    if (hovered!=blocked) blocked=-1;
    if ((pressed&1) && hovered>=0) { nav_target=confirm_target=hovered; steps=visited_count=0; }
    if (nav_target>=0 && menu_items[nav_target].container!=menu->container) nav_target=confirm_target=-1;
    if (press_until) {
        if (now<press_until) { out=press_button; goto done; }
        gap_until=press_until+GAP_NS; /* from the press's end, however late this sample is */
        if (press_button&(PAD_UP|PAD_DOWN|PAD_LEFT|PAD_RIGHT)) settle_until=press_until+SETTLE_NS;
        press_until=0;
    }
    if (now<gap_until || now-menu->select_ns<REFRESH_NS) goto done;
    if (settle_until) {
        if (selected!=press_from) {
            settle_until=0;
            /* Back at an item already passed on the way: the target is not reachable this way. */
            int seen=0;
            for (int k=0;k<visited_count;++k) seen|=visited[k]==selected;
            if (seen && nav_target>=0 && selected!=nav_target) {
                menu_note("Item_%d_%d: the selection came back, given up",menu_items[nav_target].a,menu_items[nav_target].b);
                blocked=nav_target; nav_target=confirm_target=-1;
            } else if (visited_count<MAX_VISITED) visited[visited_count++]=selected;
        }
        else if (now<settle_until) goto done;
        else {
            settle_until=0;
            if (nav_target>=0) menu_note("Item_%d_%d out of reach",menu_items[nav_target].a,menu_items[nav_target].b);
            blocked=nav_target; nav_target=confirm_target=-1;
            /* Presses here keep going unanswered: something over it has the focus. */
            MenuContainer *aimed=container_of(press_container,0);
            if (aimed && ++aimed->unanswered>=UNANSWERED_MAX) {
                aimed->behind=COVERED; aimed->covered_ns=now; aimed->unanswered=0;
                menu_note("container %" PRIx64 " does not answer: covered",aimed->container);
            }
            press_container=0;
        }
    }
    if (nav_target>=0 && nav_target==selected) nav_target=-1;
    if (nav_target>=0 && steps>=MAX_STEPS) {
        menu_note("Item_%d_%d not reached in %d steps",menu_items[nav_target].a,menu_items[nav_target].b,steps);
        blocked=nav_target; nav_target=confirm_target=-1;
    }
    if (pressed&4) {
        press_button=cancel_button; press_until=now+PRESS_NS; nav_target=confirm_target=-1; menu_note("cancel");
    } else if (nav_target>=0 && selected<0) { /* the selection is unknown: one press shows it */
        press_button=PAD_DOWN; press_until=now+PRESS_NS; press_from=selected; ++steps;
        press_container=menu->container; press_ns=now;
        menu_note("toward Item_%d_%d: down (finds the selection)",menu_items[nav_target].a,menu_items[nav_target].b);
    } else if (nav_target>=0 && selected>=0) {
        if (!visited_count) visited[visited_count++]=selected;
        press_button=toward(&menu_items[selected],&menu_items[nav_target]);
        press_until=now+PRESS_NS; press_from=selected; ++steps;
        press_container=menu->container; press_ns=now;
        menu_note("toward Item_%d_%d: %s",menu_items[nav_target].a,menu_items[nav_target].b,
                  press_button==PAD_UP ? "up" : press_button==PAD_DOWN ? "down" : press_button==PAD_LEFT ? "left" : "right");
    } else if (confirm_target>=0 && confirm_target==selected) {
        press_button=confirm_button; press_until=now+PRESS_NS; confirm_target=-1; menu_note("confirm");
    }
    if (press_until) out=press_button;
done:
    host_unlock(&menu_lock);
    return out;
}

void runtime_menu_configure(unsigned char *base, uint64_t size) {
    const char *env=getenv("BB_MENU_PROBE");
    if (!env || strcmp(env,"1")) return;
    if (size<DATA_END || memcmp(base+CHECK_NAME,"FrpgMenuDlgCursorGroup",23)) {
        puts("Menu probe: not the v1.09 image, off");
        return;
    }
    vt_cache=calloc(VT_CACHE,sizeof(*vt_cache));
    views=malloc(MAX_VIEWS*sizeof(*views));
    if (!vt_cache || !views) return;
    image=base;
    enabled=1;
    const char *color=getenv("BB_CONSOLE_COLOR");
    if ((color && !strcmp(color,"1")) || isatty(STDOUT_FILENO)) { on="\x1b[1;93m"; off="\x1b[0m"; }
    trace_configure();
    const char *enter=getenv("BB_ENTER_BUTTON");
    if (enter && !strcmp(enter,"circle")) { confirm_button=PAD_CIRCLE; cancel_button=PAD_CROSS; }
    menu_mouse=traceable;
    if (menu_mouse) printf("%sMenu mouse: on (menus free the mouse; point, left button confirms, right cancels)%s\n",on,off);
    printf("Menu probe: F7 writes %s/menu-probe-*.txt%s\n",log_dir(),
           traceable ? ", F8 menu-trace-*.txt (4 s of menu calls)" : "");
}

static double seconds(void) {
    struct timespec t;
    timespec_get(&t,TIME_UTC);
    return (double)t.tv_sec+t.tv_nsec*1e-9;
}

static void probe(float pointer_x, float pointer_y, int window_w, int window_h) {
    const double start=seconds();
    ++probes;
    view_count=runtime_memory_views(views,MAX_VIEWS);
    uint64_t bytes=0;
    for (size_t v=0;v<view_count;++v) bytes+=views[v].size;
    object_count=0;
    each_qword(find_object,1);
    qsort(objects,object_count,sizeof(*objects),by_address);
    free(menus);
    menus=NULL; menu_count=0;
    size_t named_menus=0;
    for (size_t i=0;i<object_count;++i) named_menus+=menu_class(objects[i].name)!=0;
    menus=calloc(named_menus ? named_menus : 1,sizeof(*menus));
    if (!menus) return;
    for (size_t i=0;i<object_count;++i) if (menu_class(objects[i].name)) menus[menu_count++].object=i;
    if (menu_count) {
        menu_lo=objects[menus[0].object].address;
        menu_hi=objects[menus[menu_count-1].object].address;
        size_t slots=16;
        while (slots<menu_count*4) slots*=2;
        free(menu_index);
        menu_index=calloc(slots,sizeof(*menu_index));
        if (!menu_index) return;
        menu_index_mask=slots-1;
        for (size_t i=0;i<menu_count;++i) {
            size_t h=address_hash(objects[menus[i].object].address)&menu_index_mask;
            while (menu_index[h]) h=(h+1)&menu_index_mask;
            menu_index[h]=(uint32_t)i+1;
        }
        each_qword(find_referrer,1);
    }
    char path[512], stamp[32];
    time_t now=time(NULL);
    strftime(stamp,sizeof(stamp),"%Y%m%d-%H%M%S",localtime(&now));
#ifdef _WIN32
    _mkdir(log_dir());
#else
    mkdir(log_dir(),0755);
#endif
    snprintf(path,sizeof(path),"%s/menu-probe-%s-%u.txt",log_dir(),stamp,probes);
    FILE *f=probe_file=fopen(path,"w");
    if (!f) { printf("Menu probe %u: cannot write %s\n",probes,path); return; }
    fprintf(f,"Menu probe %u, %s\nPointer %.0f,%.0f in a %dx%d window (points)\n",probes,stamp,pointer_x,pointer_y,window_w,window_h);
    fprintf(f,"Guest memory: %zu ranges, %.2f GiB; runtime class objects: %zu, menu objects: %zu\n",
            view_count,bytes/1073741824.0,object_count,menu_count);
    const char **names=malloc((object_count ? object_count : 1)*sizeof(*names));
    if (names) {
        for (size_t i=0;i<object_count;++i) names[i]=objects[i].name;
        qsort(names,object_count,sizeof(*names),by_name);
        fputs("\nAll classes:\n",f);
        for (size_t i=0;i<object_count;) {
            size_t j=i;
            while (j<object_count && !strcmp(names[j],names[i])) ++j;
            fprintf(f,"  %6zu  %s\n",j-i,names[i]);
            i=j;
        }
        free(names);
    }
    qsort(menus,menu_count,sizeof(*menus),by_class);
    fputs("\nMenu classes:\n",f);
    for (size_t i=0;i<menu_count;) {
        size_t j=i;
        while (j<menu_count && !strcmp(objects[menus[j].object].name,objects[menus[i].object].name)) ++j;
        fprintf(f,"  %5zu  %s\n",j-i,objects[menus[i].object].name);
        i=j;
    }
    size_t slots=16;
    while (slots<(menu_count+MAX_FOLLOWED)*4) slots*=2;
    free(dumped);
    dumped=calloc(slots,sizeof(*dumped));
    if (!dumped) { fclose(f); probe_file=NULL; return; }
    dumped_mask=slots-1;
    followed_count=0;
    for (size_t i=0;i<menu_count;++i) mark_dumped(objects[menus[i].object].address);
    for (size_t i=0;i<menu_count && i<MAX_DUMPED;++i) dump(f,&menus[i]);
    if (menu_count>MAX_DUMPED) fprintf(f,"\n(%zu more not dumped)\n",menu_count-MAX_DUMPED);
    fputs("\n== What the menu objects point at\n",f);
    for (size_t i=0;i<followed_count;++i) dump_followed(f,&followed[i]);
    if (followed_count==MAX_FOLLOWED) fputs("\n(the follow limit was reached)\n",f);
    fclose(f);
    probe_file=NULL;
    printf("%sMenu probe %u: %zu menu objects of %zu named, %.1f s -> %s%s\n",on,probes,menu_count,object_count,seconds()-start,path,off);
}

/* A fault in the probe (a bug here: guest data is only read through checked pointers) ends
 * the probe, not the game. */
static void probe_guarded(float pointer_x, float pointer_y, int window_w, int window_h) {
    static int busy;
    static RuntimeRecoverBuf recover;
    if (__atomic_exchange_n(&busy,1,__ATOMIC_ACQUIRE)) return;
    RuntimeRecoverBuf *previous=runtime_fault_recover;
    if (RUNTIME_RECOVER_SET(recover)) {
        runtime_fault_recover=previous;
        if (probe_file) fclose(probe_file);
        probe_file=NULL;
        printf("%sMenu probe %u: stopped by a fault, the file is incomplete%s\n",on,probes,off);
    } else {
        runtime_fault_recover=&recover;
        probe(pointer_x,pointer_y,window_w,window_h);
        runtime_fault_recover=previous;
    }
    __atomic_store_n(&busy,0,__ATOMIC_RELEASE);
}

/* Every pad sample: new F7/F8 presses (several threads may read the pad, one acts on each),
 * then the running trace's pad lines and its end. */
uint32_t runtime_menu_update(unsigned probe_requests, unsigned trace_requests, float pointer_x, float pointer_y,
                             int window_w, int window_h, uint32_t free_buttons, uint32_t buttons, uint8_t left_x, uint8_t left_y) {
    static unsigned probes_seen, traces_seen;
    if (!enabled) return 0;
    unsigned seen=__atomic_load_n(&probes_seen,__ATOMIC_RELAXED);
    if (probe_requests!=seen &&
        __atomic_compare_exchange_n(&probes_seen,&seen,probe_requests,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED))
        probe_guarded(pointer_x,pointer_y,window_w,window_h);
    seen=__atomic_load_n(&traces_seen,__ATOMIC_RELAXED);
    if (trace_requests!=seen &&
        __atomic_compare_exchange_n(&traces_seen,&seen,trace_requests,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED))
        trace_begin(pointer_x,pointer_y,window_w,window_h);
    if (traceable && host_monotonic_ns()-arg_views_ns>VIEWS_NS) refresh_arg_views();
    const uint32_t added=menu_mouse ? menu_pad(pointer_x,pointer_y,window_w,window_h,free_buttons) : 0;
    if (__atomic_load_n(&tracing,__ATOMIC_ACQUIRE)) {
        trace_pad(buttons|added,left_x,left_y);
        trace_finish();
    }
    return added;
}
