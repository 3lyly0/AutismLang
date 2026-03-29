#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir((p),0755)
#endif

#define AUTISMLANG_VERSION "0.9.0"
static char g_err[512];
#define ERR(...) snprintf(g_err,sizeof(g_err),__VA_ARGS__)

typedef struct{char*d;size_t len,cap;}SBuf;
static void sb_init(SBuf*b){b->d=NULL;b->len=0;b->cap=0;}
static void sb_free(SBuf*b){free(b->d);sb_init(b);}
static bool sb_cat(SBuf*b,const char*s){
    size_t n=strlen(s);
    if(b->len+n+1>b->cap){size_t nc=b->cap?b->cap*2:256;while(nc<b->len+n+1)nc*=2;char*np=realloc(b->d,nc);if(!np)return false;b->d=np;b->cap=nc;}
    memcpy(b->d+b->len,s,n+1);b->len+=n;return true;
}
__attribute__((format(printf,2,3)))
static bool sb_fmt(SBuf*b,const char*fmt,...){
    char tmp[512];va_list ap;va_start(ap,fmt);int n=vsnprintf(tmp,sizeof(tmp),fmt,ap);va_end(ap);
    if(n<0)return false;if((size_t)n<sizeof(tmp))return sb_cat(b,tmp);
    char*big=malloc((size_t)n+1);if(!big)return false;va_start(ap,fmt);vsnprintf(big,(size_t)n+1,fmt,ap);va_end(ap);bool ok=sb_cat(b,big);free(big);return ok;
}
typedef struct{char**items;size_t len,cap;}SList;
static void sl_init(SList*l){l->items=NULL;l->len=0;l->cap=0;}
static void sl_free(SList*l){for(size_t i=0;i<l->len;i++)free(l->items[i]);free(l->items);sl_init(l);}
static bool sl_push(SList*l,char*s){
    if(l->len==l->cap){size_t nc=l->cap?l->cap*2:8;char**np=realloc(l->items,nc*sizeof(char*));if(!np)return false;l->items=np;l->cap=nc;}
    l->items[l->len++]=s;return true;
}
static char*xdup(const char*s){size_t n=strlen(s)+1;char*p=malloc(n);if(p)memcpy(p,s,n);return p;}
static char*xndup(const char*s,size_t n){char*p=malloc(n+1);if(p){memcpy(p,s,n);p[n]=0;}return p;}

static char*read_file(const char*path){
    FILE*f=fopen(path,"rb");if(!f){fprintf(stderr,"Cannot open '%s'\n",path);return NULL;}
    fseek(f,0,SEEK_END);long sz=ftell(f);fseek(f,0,SEEK_SET);
    char*buf=malloc((size_t)sz+1);if(!buf){fclose(f);return NULL;}
    if(fread(buf,1,(size_t)sz,f)!=(size_t)sz){fclose(f);free(buf);return NULL;}
    buf[sz]=0;fclose(f);return buf;
}
static bool split_lines(const char*src,SList*out){
    const char*p=src,*start=src;
    while(*p){if(*p=='\n'){size_t n=(size_t)(p-start);if(n>0&&start[n-1]=='\r')n--;char*l=xndup(start,n);if(!l||!sl_push(out,l)){free(l);return false;}start=p+1;}p++;}
    if(start!=p){size_t n=(size_t)(p-start);if(n>0&&start[n-1]=='\r')n--;char*l=xndup(start,n);if(!l||!sl_push(out,l)){free(l);return false;}}
    return true;
}
static const char*ltrim(const char*s){while(*s&&isspace((unsigned char)*s))s++;return s;}
static void rtrim(char*s){size_t n=strlen(s);while(n&&isspace((unsigned char)s[n-1]))s[--n]=0;}
static bool blank(const char*s){const char*p=ltrim(s);return!*p||*p=='#';}
static void strip_comment(char*s){bool in=false,esc=false;for(size_t i=0;s[i];i++){if(s[i]=='\\'&&!esc&&in){esc=true;continue;}if(s[i]=='"'&&!esc)in=!in;if(s[i]=='#'&&!in){s[i]=0;break;}esc=false;}rtrim(s);}
static bool has_ind(const char*r,size_t ind){for(size_t i=0;i<ind;i++)if(r[i]!=' ')return false;return true;}
static bool is_ic(char c){return isalpha((unsigned char)c)||c=='_';}
static bool is_icc(char c){return isalnum((unsigned char)c)||c=='_';}
static size_t blk_end(const SList*lines,size_t start,size_t lim,size_t ind){
    size_t i=start;
    while(i<lim){char*cp=xdup(lines->items[i]);rtrim(cp);if(blank(cp)){free(cp);i++;continue;}free(cp);if(strlen(lines->items[i])<ind||!has_ind(lines->items[i],ind))break;i++;}
    return i;
}

typedef enum{BASE_UNKNOWN=0,BASE_INT,BASE_BOOL,BASE_STRING,BASE_VOID,BASE_STRUCT}BaseType;
typedef struct{BaseType base;unsigned ptr_depth;char* struct_name;bool is_array;size_t array_size;}Type;
static void skip(const char**pp);

static Type type_make(BaseType b,unsigned d){Type t;t.base=b;t.ptr_depth=d;t.struct_name=NULL;t.is_array=false;t.array_size=0;return t;}
static Type type_unknown(void){return type_make(BASE_UNKNOWN,0);}
static Type type_int(void){return type_make(BASE_INT,0);}
static Type type_bool(void){return type_make(BASE_BOOL,0);}
static Type type_string(void){return type_make(BASE_STRING,0);}
static Type type_void(void){return type_make(BASE_VOID,0);}
static Type type_null_ptr(void){return type_make(BASE_VOID,1);} /* ptr<void> */
static Type type_struct(const char* name){Type t=type_make(BASE_STRUCT,0);t.struct_name=(char*)name;return t;}
static bool type_is_unknown(Type t){return t.base==BASE_UNKNOWN&&t.ptr_depth==0;}
static bool type_is_ptr(Type t){return t.ptr_depth>0&&!t.is_array;}
static bool type_eq(Type a,Type b){if(a.base!=b.base||a.ptr_depth!=b.ptr_depth||a.is_array!=b.is_array||a.array_size!=b.array_size)return false;if(a.base==BASE_STRUCT){if(!a.struct_name||!b.struct_name||strcmp(a.struct_name,b.struct_name)!=0)return false;}return true;}
static Type type_addr_of(Type t){if(type_is_unknown(t))return type_unknown();if(t.is_array){Type p=t;p.is_array=false;p.array_size=0;p.ptr_depth++;return p;}Type p=t;p.ptr_depth++;return p;}
static Type type_deref(Type t){if(!type_is_ptr(t))return type_unknown();Type inner=t;inner.ptr_depth--;return inner;}
static bool type_assignable(Type dst,Type src){
    if(type_eq(dst,src))return true;
    if(type_is_ptr(dst)&&type_eq(src,type_null_ptr()))return true;
    if(type_is_ptr(dst)&&type_is_ptr(src)&&dst.ptr_depth==src.ptr_depth&&(dst.base==BASE_VOID||src.base==BASE_VOID))return true;
    return false;
}
static const char*type_name(Type t){
    static char bufs[8][128];static int bi=0;char inner[128];
    const char*base="unknown";
    if(t.base==BASE_INT)base="int";
    else if(t.base==BASE_BOOL)base="bool";
    else if(t.base==BASE_STRING)base="str";
    else if(t.base==BASE_VOID)base="void";
    else if(t.base==BASE_STRUCT)base=t.struct_name?t.struct_name:"struct";
    snprintf(inner,sizeof(inner),"%s",base);
    for(unsigned i=0;i<t.ptr_depth;i++){
        char wrapped[128];snprintf(wrapped,sizeof(wrapped),"ptr<%s>",inner);snprintf(inner,sizeof(inner),"%s",wrapped);
    }
    if(t.is_array){
        char arr[128];snprintf(arr,sizeof(arr),"%s[%zu]",inner,t.array_size);snprintf(inner,sizeof(inner),"%s",arr);
    }
    bi=(bi+1)%8;snprintf(bufs[bi],sizeof(bufs[bi]),"%s",inner);return bufs[bi];
}
static bool parse_type_ref(const char**pp,Type*out){
    skip(pp);
    Type t = type_unknown();
    if(strncmp(*pp,"ptr",3)==0&&!is_icc((*pp)[3])){
        (*pp)+=3;skip(pp);
        if(**pp!='<'){t=type_null_ptr();goto arr_chk;}
        (*pp)++;
        if(!parse_type_ref(pp,&t))return false;
        skip(pp);if(**pp!='>'){ERR("expected '>' in pointer type");return false;}
        (*pp)++;
        if(t.ptr_depth==255){ERR("pointer nesting too deep");return false;}
        t.ptr_depth++;goto arr_chk;
    }
    if(strncmp(*pp,"int",3)==0&&!is_icc((*pp)[3])){(*pp)+=3;t=type_int();goto arr_chk;}
    if(strncmp(*pp,"bool",4)==0&&!is_icc((*pp)[4])){(*pp)+=4;t=type_bool();goto arr_chk;}
    if(strncmp(*pp,"str",3)==0&&!is_icc((*pp)[3])){(*pp)+=3;t=type_string();goto arr_chk;}
    if(strncmp(*pp,"string",6)==0&&!is_icc((*pp)[6])){(*pp)+=6;t=type_string();goto arr_chk;}
    if(strncmp(*pp,"void",4)==0&&!is_icc((*pp)[4])){(*pp)+=4;t=type_void();goto arr_chk;}
    if(is_ic(**pp)){
        char id[128];size_t n=0;
        while(is_icc(**pp)){if(n+1>=sizeof(id)){ERR("type name too long");return false;}id[n++]=**pp;(*pp)++;}
        id[n]=0;t=type_struct(xdup(id));goto arr_chk;
    }
    ERR("unknown type");return false;
arr_chk:
    skip(pp);
    if(**pp=='['){
        (*pp)++;skip(pp);char*end;long long v=strtoll(*pp,&end,10);
        if(*pp==end||v<=0){ERR("expected positive array size");return false;}
        *pp=end;skip(pp);if(**pp!=']'){ERR("expected ']' in array type");return false;}(*pp)++;
        if(t.is_array){ERR("multi-dimensional array not supported");return false;}
        t.is_array=true;t.array_size=(size_t)v;
    }
    *out=t;return true;
}

typedef enum{EK_INT,EK_BOOL,EK_STRING,EK_VAR,EK_BINOP,EK_CALL,EK_PTR_NULL,EK_INT_CAST,EK_DEREF,EK_ADDROF,EK_ALLOC,EK_FREE,EK_PTR_CAST,EK_IN,EK_RANGE,EK_FIELD,EK_PTR_FIELD,EK_INDEX,EK_SIZEOF,EK_OFFSETOF,EK_STRUCT_INIT}EK;
typedef struct Expr{
    EK kind;Type type;Type cast_type;long long ival;char*sval;char*name;char*fn;char op[3];
    struct Expr*left,*right,**args;size_t argc;
    struct Expr*range_start,*range_end,*range_step;bool range_inclusive;
}Expr;
typedef enum{SK_PRINT,SK_DECL,SK_ASSIGN,SK_IF,SK_WHILE,SK_UNSAFE,SK_ASM,SK_OUT,SK_FOR,SK_RETURN,SK_BREAK,SK_CONTINUE,SK_CALL}SK;
typedef struct Stmt{
    SK kind;char*var;Type var_type;Expr*expr,*cond;
    struct Stmt**then;size_t nthen;struct Stmt**els;size_t nels;struct Stmt**loop;size_t nloop;
    char*loop_var;Expr*range_expr;char*asm_text;int var_volatile;size_t line;
}Stmt;
typedef struct{char*name;char**params;Type*param_types;Type return_type;Stmt**body;size_t nbody;size_t nparams;}FnDef;

typedef struct StructField {char* name;Type type;} StructField;
typedef struct StructDef {char* name;StructField* fields;size_t nfields;size_t cap;} StructDef;
#define STRUCT_MAP_CAP 256
typedef struct StructMap {char* keys[STRUCT_MAP_CAP];StructDef* vals[STRUCT_MAP_CAP];} StructMap;
static unsigned long djb2(const char* str) {unsigned long hash = 5381;int c;while ((c = *str++)) hash = ((hash << 5) + hash) + c;return hash;}
static bool sm_put(StructMap* m, const char* key, StructDef* val) {
    unsigned long h = djb2(key) % STRUCT_MAP_CAP;
    for(size_t i=0; i<STRUCT_MAP_CAP; i++) {
        size_t idx = (h + i) % STRUCT_MAP_CAP;
        if (!m->keys[idx]) {m->keys[idx] = xdup(key);m->vals[idx] = val;return true;}
        if (strcmp(m->keys[idx], key) == 0) return false;
    }
    return false;
}
static StructDef* sm_get(const StructMap* m, const char* key) {
    unsigned long h = djb2(key) % STRUCT_MAP_CAP;
    for(size_t i=0; i<STRUCT_MAP_CAP; i++) {
        size_t idx = (h + i) % STRUCT_MAP_CAP;
        if (!m->keys[idx]) return NULL;
        if (strcmp(m->keys[idx], key) == 0) return m->vals[idx];
    }
    return NULL;
}
static void sm_free(StructMap* m) {
    for(size_t i=0; i<STRUCT_MAP_CAP; i++) {
        if(m->keys[i]) {free(m->keys[i]);free(m->vals[i]->name);
            for(size_t j=0;j<m->vals[i]->nfields;j++)free(m->vals[i]->fields[j].name);
            free(m->vals[i]->fields);free(m->vals[i]);
        }
    }
}
typedef struct{FnDef*fns;size_t nfns,cap;StructMap structs;StructDef* ordered_structs[256];size_t n_ordered_structs;}Program;
static bool is_valid_type(Type t, const StructMap* sm) {
    if (t.base == BASE_STRUCT) return sm_get(sm, t.struct_name) != NULL;
    return true;
}

static void free_expr(Expr*e){if(!e)return;free(e->sval);free(e->name);free(e->fn);free_expr(e->left);free_expr(e->right);for(size_t i=0;i<e->argc;i++)free_expr(e->args[i]);free(e->args);free_expr(e->range_start);free_expr(e->range_end);free_expr(e->range_step);free(e);}
static void free_stmt(Stmt*s){if(!s)return;free(s->var);free_expr(s->expr);free_expr(s->cond);for(size_t i=0;i<s->nthen;i++)free_stmt(s->then[i]);free(s->then);for(size_t i=0;i<s->nels;i++)free_stmt(s->els[i]);free(s->els);for(size_t i=0;i<s->nloop;i++)free_stmt(s->loop[i]);free(s->loop);free(s->loop_var);free_expr(s->range_expr);free(s->asm_text);free(s);}
static void free_fn(FnDef*f){if(!f)return;free(f->name);for(size_t i=0;i<f->nparams;i++)free(f->params[i]);free(f->params);free(f->param_types);for(size_t i=0;i<f->nbody;i++)free_stmt(f->body[i]);free(f->body);}
static void free_program(Program*p){for(size_t i=0;i<p->nfns;i++)free_fn(&p->fns[i]);free(p->fns);}

typedef struct Symbol{char*name;Type type;int is_param;}Symbol;
typedef struct Scope{Symbol*syms;size_t count,cap;struct Scope*parent;}Scope;
static void scope_init(Scope*sc,Scope*parent){sc->syms=NULL;sc->count=0;sc->cap=0;sc->parent=parent;}
static void scope_free(Scope*sc){for(size_t i=0;i<sc->count;i++)free(sc->syms[i].name);free(sc->syms);sc->syms=NULL;sc->count=0;sc->cap=0;}
static Symbol*scope_find(Scope*sc,const char*name){for(size_t i=0;i<sc->count;i++)if(strcmp(sc->syms[i].name,name)==0)return&sc->syms[i];if(sc->parent)return scope_find(sc->parent,name);return NULL;}
static bool scope_add(Scope*sc,const char*name,Type type,int is_param){
    for(size_t i=0;i<sc->count;i++)if(strcmp(sc->syms[i].name,name)==0){ERR("variable '%s' already declared",name);return false;}
    if(sc->count==sc->cap){size_t nc=sc->cap?sc->cap*2:8;Symbol*np=realloc(sc->syms,nc*sizeof(Symbol));if(!np)return false;sc->syms=np;sc->cap=nc;}
    sc->syms[sc->count].name=xdup(name);sc->syms[sc->count].type=type;sc->syms[sc->count].is_param=is_param;sc->count++;return true;
}
static bool parse_qualified_type_ref(const char**pp,Type*out,int*is_volatile){
    *is_volatile=0;skip(pp);
    if(strncmp(*pp,"volatile",8)==0&&!is_icc((*pp)[8])){*is_volatile=1;(*pp)+=8;skip(pp);}
    return parse_type_ref(pp,out);
}

static Expr*parse_expr(const char**pp);
static Expr*parse_sum(const char**pp);
static void skip(const char**pp){while(**pp&&isspace((unsigned char)**pp))(*pp)++;}
static char*process_string_escape(const char*src,size_t len){
    char*result=malloc(len+1);if(!result)return NULL;size_t j=0;
    for(size_t i=0;i<len;i++){if(src[i]=='\\'&&i+1<len){switch(src[i+1]){case 'n':result[j++]='\n';i++;break;case 't':result[j++]='\t';i++;break;case 'r':result[j++]='\r';i++;break;case '\\':result[j++]='\\';i++;break;case '"':result[j++]='"';i++;break;case '0':result[j++]='\0';i++;break;default:result[j++]=src[i];break;}}else result[j++]=src[i];}
    result[j]='\0';return result;
}
static bool parse_args(const char**pp,Expr***oa,size_t*on){
    skip(pp);if(**pp!='('){ERR("expected '('");return false;}(*pp)++;
    Expr**args=NULL;size_t n=0,cap=0;skip(pp);
    while(**pp&&**pp!=')'){
        Expr*a=parse_expr(pp);if(!a){for(size_t i=0;i<n;i++)free_expr(args[i]);free(args);return false;}
        if(n==cap){size_t nc=cap?cap*2:4;Expr**np=realloc(args,nc*sizeof(Expr*));if(!np){free_expr(a);for(size_t i=0;i<n;i++)free_expr(args[i]);free(args);return false;}args=np;cap=nc;}
        args[n++]=a;skip(pp);if(**pp==','){(*pp)++;skip(pp);}
    }
    if(**pp!=')'){for(size_t i=0;i<n;i++)free_expr(args[i]);free(args);ERR("expected ')'");return false;}
    (*pp)++;*oa=args;*on=n;return true;
}
static Expr*parse_range_expr(const char**pp,Expr*start_expr){
    const char*p=*pp;bool inclusive=false;
    if(p[0]=='.'&&p[1]=='.'){if(p[2]=='='){inclusive=true;(*pp)=p+3;}else{inclusive=false;(*pp)=p+2;}}else return start_expr;
    skip(pp);Expr*end_expr=parse_sum(pp);if(!end_expr){free_expr(start_expr);return NULL;}
    Expr*step_expr=NULL;skip(pp);
    if((*pp)[0]=='.'&&(*pp)[1]=='.'&&(*pp)[2]!='='){(*pp)+=2;skip(pp);step_expr=parse_sum(pp);if(!step_expr){free_expr(start_expr);free_expr(end_expr);ERR("expected step");return NULL;}}
    Expr*e=calloc(1,sizeof(Expr));if(!e){free_expr(start_expr);free_expr(end_expr);free_expr(step_expr);return NULL;}
    e->kind=EK_RANGE;e->type=type_int();e->range_start=start_expr;e->range_end=end_expr;e->range_step=step_expr;e->range_inclusive=inclusive;
    return e;
}
static Expr*parse_primary(const char**pp){
    skip(pp);const char*p=*pp;
    if(*p=='('){(*pp)++;Expr*e=parse_expr(pp);if(!e)return NULL;skip(pp);if(**pp!=')'){free_expr(e);ERR("missing ')'");return NULL;}(*pp)++;return e;}
    if(*p=='"'){
        (*pp)++;const char*start=*pp;size_t len=0;
        while(**pp&&**pp!='"'){if(**pp=='\\'&&(*pp)[1])(*pp)++;(*pp)++;len++;}
        if(**pp!='"'){ERR("unterminated string");return NULL;}
        char*raw=xndup(start,(size_t)(*pp-start));(*pp)++;
        char*processed=process_string_escape(raw,strlen(raw));free(raw);if(!processed)return NULL;
        Expr*e=calloc(1,sizeof(Expr));if(!e){free(processed);return NULL;}e->kind=EK_STRING;e->type=type_string();e->sval=processed;return e;
    }
    bool neg=false;if(*p=='-'&&isdigit((unsigned char)p[1])){neg=true;(*pp)++;p++;}
    if(isdigit((unsigned char)*p)){
        char*end;int base=10;if(*p=='0'&&(p[1]=='x'||p[1]=='X'))base=16;
        long long v=strtoll(*pp,&end,base);*pp=end;
        Expr*e=calloc(1,sizeof(Expr));if(!e)return NULL;e->kind=EK_INT;e->type=type_int();e->ival=neg?-v:v;return e;
    }(void)neg;
    if(is_ic(*p)){
        char id[128];size_t n=0;while(is_icc(**pp)){if(n+1>=sizeof(id))return NULL;id[n++]=**pp;(*pp)++;}id[n]=0;skip(pp);
        if(strcmp(id,"sizeof")==0){if(**pp!='('){ERR("expected '(' after sizeof");return NULL;}(*pp)++;Type t;if(!parse_type_ref(pp,&t))return NULL;skip(pp);if(**pp!=')'){ERR("expected ')'");return NULL;}(*pp)++;Expr*e=calloc(1,sizeof(Expr));if(!e)return NULL;e->kind=EK_SIZEOF;e->cast_type=t;return e;}
        if(strcmp(id,"offsetof")==0){if(**pp!='('){ERR("expected '(' after offsetof");return NULL;}(*pp)++;Type t;if(!parse_type_ref(pp,&t))return NULL;skip(pp);if(**pp!=','){ERR("expected ',' in offsetof");return NULL;}(*pp)++;skip(pp);char fid[128];size_t fn=0;if(!is_ic(**pp)){ERR("expected field name");return NULL;}while(is_icc(**pp)){if(fn+1>=sizeof(fid))return NULL;fid[fn++]=**pp;(*pp)++;}fid[fn]=0;skip(pp);if(**pp!=')'){ERR("expected ')'");return NULL;}(*pp)++;Expr*e=calloc(1,sizeof(Expr));if(!e)return NULL;e->kind=EK_OFFSETOF;e->cast_type=t;e->name=xdup(fid);return e;}
        if(**pp=='('){
            if(strcmp(id,"int")==0){Expr*e=calloc(1,sizeof(Expr));if(!e)return NULL;e->kind=EK_INT_CAST;if(!parse_args(pp,&e->args,&e->argc)){free(e);return NULL;}if(e->argc!=1){free_expr(e);return NULL;}return e;}
            if(strcmp(id,"in")==0){Expr*e=calloc(1,sizeof(Expr));if(!e)return NULL;e->kind=EK_IN;if(!parse_args(pp,&e->args,&e->argc)){free(e);return NULL;}if(e->argc!=1){free_expr(e);ERR("TypeError: in() expects one argument");return NULL;}return e;}
            if(strcmp(id,"alloc")==0){Expr*e=calloc(1,sizeof(Expr));if(!e)return NULL;e->kind=EK_ALLOC;if(!parse_args(pp,&e->args,&e->argc)){free(e);return NULL;}if(e->argc!=1){free_expr(e);return NULL;}return e;}
            if(strcmp(id,"free")==0){Expr*e=calloc(1,sizeof(Expr));if(!e)return NULL;e->kind=EK_FREE;if(!parse_args(pp,&e->args,&e->argc)){free(e);return NULL;}if(e->argc!=1){free_expr(e);return NULL;}return e;}
            Expr*e=calloc(1,sizeof(Expr));if(!e)return NULL;e->kind=EK_CALL;e->fn=xdup(id);if(!parse_args(pp,&e->args,&e->argc)){free_expr(e);return NULL;}return e;
        }
        if(strcmp(id,"ptr")==0&&**pp=='<'){
            Expr*e=calloc(1,sizeof(Expr));if(!e)return NULL;e->kind=EK_PTR_CAST;
            (*pp)++;
            Type inner=type_unknown();
            if(!parse_type_ref(pp,&inner)){free(e);return NULL;}
            skip(pp);if(**pp!='>'){free(e);ERR("expected '>' in ptr cast");return NULL;}
            (*pp)++;
            e->cast_type=type_make(inner.base,inner.ptr_depth+1);
            skip(pp);
            if(!parse_args(pp,&e->args,&e->argc)){free(e);return NULL;}
            if(e->argc!=1||!type_is_ptr(e->cast_type)){free_expr(e);ERR("ptr cast must be ptr<...>(value)");return NULL;}
            return e;
        }
        if(strcmp(id,"True")==0||strcmp(id,"true")==0){Expr*e=calloc(1,sizeof(Expr));if(!e)return NULL;e->kind=EK_BOOL;e->type=type_bool();e->ival=1;return e;}
        if(strcmp(id,"False")==0||strcmp(id,"false")==0){Expr*e=calloc(1,sizeof(Expr));if(!e)return NULL;e->kind=EK_BOOL;e->type=type_bool();e->ival=0;return e;}
        if(strcmp(id,"null")==0||strcmp(id,"NULL")==0){Expr*e=calloc(1,sizeof(Expr));if(!e)return NULL;e->kind=EK_PTR_NULL;e->type=type_null_ptr();return e;}
        Expr*e=calloc(1,sizeof(Expr));if(!e)return NULL;e->kind=EK_VAR;e->name=xdup(id);return e;
    }
    ERR("unexpected '%c'",(int)*p);return NULL;
}
static Expr*parse_postfix(const char**pp){
    Expr*l=parse_primary(pp);if(!l)return NULL;
    while(1){
        skip(pp);const char*p=*pp;
        if(*p=='.'&&p[1]!='.'){
            (*pp)++;skip(pp);char id[128];size_t n=0;
            if(!is_ic(**pp)){free_expr(l);ERR("expected field name");return NULL;}
            while(is_icc(**pp)){if(n+1>=sizeof(id)){free_expr(l);return NULL;}id[n++]=**pp;(*pp)++;}id[n]=0;
            Expr*e=calloc(1,sizeof(Expr));if(!e){free_expr(l);return NULL;}
            e->kind=EK_FIELD;e->left=l;e->name=xdup(id);l=e;continue;
        }
        if(*p=='-'&&p[1]=='>'){
            (*pp)+=2;skip(pp);char id[128];size_t n=0;
            if(!is_ic(**pp)){free_expr(l);ERR("expected field name");return NULL;}
            while(is_icc(**pp)){if(n+1>=sizeof(id)){free_expr(l);return NULL;}id[n++]=**pp;(*pp)++;}id[n]=0;
            Expr*e=calloc(1,sizeof(Expr));if(!e){free_expr(l);return NULL;}
            e->kind=EK_PTR_FIELD;e->left=l;e->name=xdup(id);l=e;continue;
        }
        if(*p=='['){
            (*pp)++;Expr*idx=parse_expr(pp);if(!idx){free_expr(l);return NULL;}
            skip(pp);if(**pp!=']'){free_expr(l);free_expr(idx);ERR("expected ']'");return NULL;}(*pp)++;
            Expr*e=calloc(1,sizeof(Expr));if(!e){free_expr(l);free_expr(idx);return NULL;}
            e->kind=EK_INDEX;e->left=l;e->right=idx;l=e;continue;
        }
        break;
    }
    return l;
}
static Expr*parse_prefix(const char**pp){
    skip(pp);const char*p=*pp;
    if(*p=='*'&&p[1]!='='){(*pp)++;Expr*inner=parse_prefix(pp);if(!inner)return NULL;Expr*e=calloc(1,sizeof(Expr));if(!e){free_expr(inner);return NULL;}e->kind=EK_DEREF;e->left=inner;return e;}
    if(*p=='&'){(*pp)++;Expr*inner=parse_prefix(pp);if(!inner)return NULL;Expr*e=calloc(1,sizeof(Expr));if(!e){free_expr(inner);return NULL;}e->kind=EK_ADDROF;e->left=inner;return e;}
    return parse_postfix(pp);
}
static Expr*parse_product(const char**pp){
    Expr*l=parse_prefix(pp);if(!l)return NULL;
    while(1){skip(pp);const char*save=*pp;char op=*save;
        if(op=='*'){const char*next=save+1;while(*next&&isspace((unsigned char)*next))next++;
            if(*next&&(*next=='('||*next=='"'||isdigit((unsigned char)*next)||is_ic(*next))){(*pp)++;Expr*r=parse_prefix(pp);if(!r){free_expr(l);return NULL;}Expr*e=calloc(1,sizeof(Expr));if(!e){free_expr(l);free_expr(r);return NULL;}e->kind=EK_BINOP;e->op[0]=op;e->left=l;e->right=r;l=e;continue;}break;}
        if(op!='/')break;(*pp)++;Expr*r=parse_prefix(pp);if(!r){free_expr(l);return NULL;}Expr*e=calloc(1,sizeof(Expr));if(!e){free_expr(l);free_expr(r);return NULL;}e->kind=EK_BINOP;e->op[0]=op;e->left=l;e->right=r;l=e;}
    return l;
}
static Expr*parse_sum(const char**pp){
    Expr*l=parse_product(pp);if(!l)return NULL;
    while(1){skip(pp);char op=**pp;if(op!='+'&&op!='-')break;(*pp)++;Expr*r=parse_product(pp);if(!r){free_expr(l);return NULL;}Expr*e=calloc(1,sizeof(Expr));if(!e){free_expr(l);free_expr(r);return NULL;}e->kind=EK_BINOP;e->op[0]=op;e->left=l;e->right=r;l=e;}
    return l;
}
static Expr*parse_expr(const char**pp){
    Expr*l=parse_sum(pp);if(!l)return NULL;skip(pp);const char*p=*pp;char op[3]={0};
    if(p[0]=='.'&&p[1]=='.')return parse_range_expr(pp,l);
    if((p[0]=='='&&p[1]=='=')||(p[0]=='!'&&p[1]=='=')||(p[0]=='<'&&p[1]=='=')||(p[0]=='>'&&p[1]=='=')){op[0]=p[0];op[1]=p[1];(*pp)+=2;}
    else if(*p=='<'||*p=='>'){op[0]=*p;(*pp)++;}
    if(op[0]){Expr*r=parse_sum(pp);if(!r){free_expr(l);return NULL;}Expr*e=calloc(1,sizeof(Expr));if(!e){free_expr(l);free_expr(r);return NULL;}e->kind=EK_BINOP;memcpy(e->op,op,3);e->left=l;e->right=r;skip(pp);if((*pp)[0]=='.'&&(*pp)[1]=='.')return parse_range_expr(pp,e);return e;}
    return l;
}
static Expr*parse_expr_s(const char*s){const char*p=s;Expr*e=parse_expr(&p);if(!e){ERR("parse_expr failed for '%s'", s);return NULL;}skip(&p);if(*p){ERR("trailing garbage after expression: '%s' inside '%s'", p, s);free_expr(e);return NULL;}return e;}

static bool parse_block(const SList*lines,size_t start,size_t end,size_t ind,Stmt***out,size_t*outn);
static Stmt*parse_one(const SList*lines,size_t*idx,size_t lim,size_t ind){
    const char*raw=lines->items[*idx];char*s=xdup(raw+ind);rtrim(s);strip_comment(s);Stmt*res=NULL;
    size_t line_no=*idx+1;
    if(strncmp(s,"return",6)==0&&(s[6]==0||isspace((unsigned char)s[6]))){Stmt*st=calloc(1,sizeof(Stmt));if(!st)goto done;st->kind=SK_RETURN;st->line=line_no;const char*ex=ltrim(s+6);if(*ex){st->expr=parse_expr_s(ex);if(!st->expr){free(st);goto done;}}(*idx)++;res=st;goto done;}
    if(strcmp(s,"break")==0){Stmt*st=calloc(1,sizeof(Stmt));if(st){st->kind=SK_BREAK;st->line=line_no;(*idx)++;res=st;}goto done;}
    if(strcmp(s,"continue")==0){Stmt*st=calloc(1,sizeof(Stmt));if(st){st->kind=SK_CONTINUE;st->line=line_no;(*idx)++;res=st;}goto done;}
    if(strncmp(s,"print",5)==0&&s[5]=='('){const char*p=s+5;Expr**args;size_t ac;if(!parse_args(&p,&args,&ac))goto done;skip(&p);if(*p||ac!=1){for(size_t i=0;i<ac;i++)free_expr(args[i]);free(args);goto done;}Stmt*st=calloc(1,sizeof(Stmt));if(!st){free_expr(args[0]);free(args);goto done;}st->kind=SK_PRINT;st->line=line_no;st->expr=args[0];free(args);(*idx)++;res=st;goto done;}
        if(strncmp(s,"asm",3)==0&&s[3]=='('){
            const char*p=s+3;Expr**args;size_t ac;if(!parse_args(&p,&args,&ac))goto done;skip(&p);
            if(*p||ac!=1||args[0]->kind!=EK_STRING){for(size_t i=0;i<ac;i++)free_expr(args[i]);free(args);ERR("TypeError: asm() expects a single string literal");goto done;}
            Stmt*st=calloc(1,sizeof(Stmt));if(!st){free_expr(args[0]);free(args);goto done;}
            st->kind=SK_ASM;st->line=line_no;st->asm_text=xdup(args[0]->sval);
            free_expr(args[0]);free(args);(*idx)++;res=st;goto done;
        }
        if(strncmp(s,"out",3)==0&&s[3]=='('){
            const char*p=s+3;Expr**args;size_t ac;if(!parse_args(&p,&args,&ac))goto done;skip(&p);
            if(*p||ac!=2){for(size_t i=0;i<ac;i++)free_expr(args[i]);free(args);ERR("TypeError: out() expects two arguments");goto done;}
            Stmt*st=calloc(1,sizeof(Stmt));if(!st){for(size_t i=0;i<ac;i++)free_expr(args[i]);free(args);goto done;}
            st->kind=SK_OUT;st->line=line_no;st->cond=args[0];st->expr=args[1];free(args);(*idx)++;res=st;goto done;
        }
    if(strncmp(s,"for",3)==0&&isspace((unsigned char)s[3])){
        const char*fs=ltrim(s+3);char varname[128];size_t vn=0;
        while(*fs&&is_icc(*fs)&&vn<sizeof(varname)-1)varname[vn++]=*fs++;varname[vn]=0;
        if(vn==0){ERR("for needs variable");goto done;}skip(&fs);
        if(strncmp(fs,"in",2)!=0||!isspace((unsigned char)fs[2])){ERR("for needs 'in'");goto done;}fs+=2;skip(&fs);
        size_t fsl=strlen(fs);if(!fsl||fs[fsl-1]!=':'){ERR("for needs ':'");goto done;}
        char*fstr=xndup(fs,fsl-1);rtrim(fstr);
        Expr*range_expr=parse_expr_s(fstr);free(fstr);if(!range_expr){goto done;}
        if(range_expr->kind!=EK_RANGE){ERR("for needs range (e.g., 0..10)");free_expr(range_expr);goto done;}
        size_t bs=*idx+1,be=blk_end(lines,bs,lim,ind+4);if(bs==be){free_expr(range_expr);ERR("for needs body");goto done;}
        Stmt**lb;size_t lc;if(!parse_block(lines,bs,be,ind+4,&lb,&lc)){free_expr(range_expr);goto done;}*idx=be;
        Stmt*st=calloc(1,sizeof(Stmt));if(!st){free_expr(range_expr);for(size_t i=0;i<lc;i++)free_stmt(lb[i]);free(lb);goto done;}
        st->kind=SK_FOR;st->line=line_no;st->loop_var=xdup(varname);st->loop=lb;st->nloop=lc;st->range_expr=range_expr;res=st;goto done;
    }
    if((strncmp(s,"if",2)==0&&isspace((unsigned char)s[2]))||(strncmp(s,"elif",4)==0&&isspace((unsigned char)s[4]))){
        const char*kw=(s[1]=='f')?s+2:s+4;(void)kw; /* skip keyword; cs computed below */
        const char*cs=ltrim((s[1]=='f')?s+2:s+4);size_t cl=strlen(cs);if(!cl||cs[cl-1]!=':'){ERR("if/elif needs ':'");goto done;}
        char*cstr=xndup(cs,cl-1);rtrim(cstr);Expr*cond=parse_expr_s(cstr);free(cstr);if(!cond)goto done;
        size_t bs=*idx+1,be=blk_end(lines,bs,lim,ind+4);if(bs==be){free_expr(cond);ERR("if needs body");goto done;}
        Stmt**tb;size_t tc;if(!parse_block(lines,bs,be,ind+4,&tb,&tc)){free_expr(cond);goto done;}*idx=be;
        while(*idx<lim){char*pr=xdup(lines->items[*idx]);rtrim(pr);if(!blank(pr)){free(pr);break;}free(pr);(*idx)++;}
        Stmt**eb=NULL;size_t ec=0;
        if(*idx<lim){const char*er=lines->items[*idx];if(strlen(er)>=ind&&has_ind(er,ind)&&er[ind]!=' '){
            char*ec2=xdup(er+ind);rtrim(ec2);strip_comment(ec2);
            if(strcmp(ec2,"else:")==0){size_t es=*idx+1,ee=blk_end(lines,es,lim,ind+4);if(es<ee&&!parse_block(lines,es,ee,ind+4,&eb,&ec)){free(ec2);for(size_t i=0;i<tc;i++)free_stmt(tb[i]);free(tb);free_expr(cond);goto done;}*idx=ee;}
            else if(strncmp(ec2,"elif ",5)==0||strncmp(ec2,"elif\t",5)==0){
                /* treat 'elif COND:' as a nested if inside an else block */
                Stmt*elif_st=parse_one(lines,idx,lim,ind);
                if(!elif_st){free(ec2);for(size_t i=0;i<tc;i++)free_stmt(tb[i]);free(tb);free_expr(cond);goto done;}
                eb=malloc(sizeof(Stmt*));if(!eb){free_stmt(elif_st);free(ec2);for(size_t i=0;i<tc;i++)free_stmt(tb[i]);free(tb);free_expr(cond);goto done;}
                eb[0]=elif_st;ec=1;
            }
            free(ec2);
        }}
        Stmt*st=calloc(1,sizeof(Stmt));if(!st){free_expr(cond);for(size_t i=0;i<tc;i++)free_stmt(tb[i]);free(tb);for(size_t i=0;i<ec;i++)free_stmt(eb[i]);free(eb);goto done;}
        st->kind=SK_IF;st->line=line_no;st->cond=cond;st->then=tb;st->nthen=tc;st->els=eb;st->nels=ec;res=st;goto done;
    }
    if(strncmp(s,"while",5)==0&&isspace((unsigned char)s[5])){
        const char*cs=ltrim(s+5);size_t cl=strlen(cs);if(!cl||cs[cl-1]!=':'){ERR("while needs ':'");goto done;}
        char*cstr=xndup(cs,cl-1);rtrim(cstr);Expr*cond=parse_expr_s(cstr);free(cstr);if(!cond)goto done;
        size_t bs=*idx+1,be=blk_end(lines,bs,lim,ind+4);if(bs==be){free_expr(cond);ERR("while needs body");goto done;}
        Stmt**lb;size_t lc;if(!parse_block(lines,bs,be,ind+4,&lb,&lc)){free_expr(cond);goto done;}*idx=be;
        Stmt*st=calloc(1,sizeof(Stmt));if(!st){free_expr(cond);for(size_t i=0;i<lc;i++)free_stmt(lb[i]);free(lb);goto done;}
        st->kind=SK_WHILE;st->line=line_no;st->cond=cond;st->loop=lb;st->nloop=lc;res=st;goto done;
    }
    if(strncmp(s,"unsafe",6)==0){
        const char*us=s+6;skip(&us);if(*us!=':'||us[1]!=0){ERR("unsafe needs ':'");goto done;}
        size_t bs=*idx+1,be=blk_end(lines,bs,lim,ind+4);if(bs==be){ERR("unsafe needs body");goto done;}
        Stmt**lb;size_t lc;if(!parse_block(lines,bs,be,ind+4,&lb,&lc))goto done;*idx=be;
        Stmt*st=calloc(1,sizeof(Stmt));if(!st){for(size_t i=0;i<lc;i++)free_stmt(lb[i]);free(lb);goto done;}
        st->kind=SK_UNSAFE;st->line=line_no;st->loop=lb;st->nloop=lc;res=st;goto done;
    }
    if(strcmp(s,"else:")==0)goto done;
    {bool in=false,esc=false;char*eq=NULL;for(char*p2=s;*p2;p2++){if(*p2=='\\'&&!esc&&in){esc=true;continue;}if(*p2=='"'&&!esc)in=!in;if(*p2=='='&&!in&&p2[1]!='='){eq=p2;break;}esc=false;}
     if(eq){char*lhs=xndup(s,(size_t)(eq-s));rtrim(lhs);const char*lt2=ltrim(lhs);
        if(*lt2=='*'&&lt2[1]!='='){Expr*target=parse_expr_s(lt2);if(!target){free(lhs);goto done;}Expr*val=parse_expr_s(ltrim(eq+1));if(!val){free(lhs);free_expr(target);goto done;}Stmt*st=calloc(1,sizeof(Stmt));if(!st){free(lhs);free_expr(target);free_expr(val);goto done;}st->kind=SK_ASSIGN;st->line=line_no;st->var=NULL;st->expr=val;st->cond=target;(*idx)++;res=st;free(lhs);goto done;}
        const char*tp=lt2;Type declared=type_unknown();int is_volatile=0;
        if(parse_qualified_type_ref(&tp,&declared,&is_volatile)){
            skip(&tp);char id[128];size_t n=0;while(is_icc(*tp)){if(n+1>=sizeof(id)){free(lhs);goto done;}id[n++]=*tp++;}id[n]=0;
            if(n&&!*ltrim(tp)){free(lhs);Expr*val=parse_expr_s(ltrim(eq+1));if(!val)goto done;Stmt*st=calloc(1,sizeof(Stmt));if(!st){free_expr(val);goto done;}st->kind=SK_ASSIGN;st->line=line_no;st->var=xdup(id);st->expr=val;st->var_type=declared;st->var_volatile=is_volatile;(*idx)++;res=st;goto done;}
        }
        char id[128];size_t n=0;const char*lp=lt2;while(is_icc(*lp)){if(n+1>=sizeof(id)){free(lhs);goto done;}id[n++]=*lp++;}id[n]=0;
        if(n&&!*ltrim(lp)){free(lhs);Expr*val=parse_expr_s(ltrim(eq+1));if(!val)goto done;Stmt*st=calloc(1,sizeof(Stmt));if(!st){free_expr(val);goto done;}st->kind=SK_ASSIGN;st->line=line_no;st->var=xdup(id);st->expr=val;(*idx)++;res=st;goto done;}
        Expr*tgt=parse_expr_s(lt2);
        if(tgt){Expr*v=parse_expr_s(ltrim(eq+1));if(v){Stmt*st=calloc(1,sizeof(Stmt));if(st){st->kind=SK_ASSIGN;st->line=line_no;st->cond=tgt;st->expr=v;(*idx)++;res=st;free(lhs);goto done;}}free_expr(tgt);}
        free(lhs);
    }}
    {
        const char*dp=s;Type decl_t=type_unknown();int is_volatile=0;
        if(parse_qualified_type_ref(&dp,&decl_t,&is_volatile)){
            skip(&dp);if(is_ic(*dp)){
                char id[128];size_t n=0;while(is_icc(*dp)){if(n+1>=sizeof(id)){goto done;}id[n++]=*dp++;}id[n]=0;skip(&dp);
                if(n&&!*dp){Stmt*st=calloc(1,sizeof(Stmt));if(!st)goto done;st->kind=SK_DECL;st->line=line_no;st->var=xdup(id);st->var_type=decl_t;st->var_volatile=is_volatile;(*idx)++;res=st;goto done;}
            }
        }
    }
    {const char*p2=s;Expr*e=parse_expr(&p2);skip(&p2);if(e&&!*p2&&(e->kind==EK_CALL||e->kind==EK_FREE||e->kind==EK_IN)){Stmt*st=calloc(1,sizeof(Stmt));if(!st){free_expr(e);goto done;}st->kind=SK_CALL;st->line=line_no;st->expr=e;(*idx)++;res=st;goto done;}free_expr(e);}
    ERR("unknown statement: %.80s",s);
done:free(s);return res;
}
static bool parse_block(const SList*lines,size_t start,size_t end,size_t ind,Stmt***out,size_t*outn){
    Stmt**stmts=NULL;size_t count=0,cap=0;size_t i=start;
    while(i<end){const char*r=lines->items[i];char*cp=xdup(r);rtrim(cp);if(blank(cp)){free(cp);i++;continue;}free(cp);if(strlen(r)<ind||!has_ind(r,ind))break;
        Stmt*st=parse_one(lines,&i,end,ind);if(!st){for(size_t j=0;j<count;j++)free_stmt(stmts[j]);free(stmts);return false;}
        if(count==cap){size_t nc=cap?cap*2:8;Stmt**np=realloc(stmts,nc*sizeof(Stmt*));if(!np){free_stmt(st);for(size_t j=0;j<count;j++)free_stmt(stmts[j]);free(stmts);return false;}stmts=np;cap=nc;}
        stmts[count++]=st;
    }
    *out=stmts;*outn=count;return true;
}
static bool parse_fn_hdr(const char*raw,char**oname,char***oparams,Type**optypes,size_t*opc){
    const char*p=raw;if(strncmp(p,"fn",2)||!isspace((unsigned char)p[2])){ERR("expected 'fn'");return false;}p+=2;while(*p&&isspace((unsigned char)*p))p++;
    char name[128];size_t n=0;if(!is_ic(*p)){ERR("bad fn name");return false;}while(is_icc(*p)){if(n+1>=sizeof(name)){ERR("name too long");return false;}name[n++]=*p++;}name[n]=0;
    while(*p&&isspace((unsigned char)*p))p++;if(*p!='('){ERR("expected '('");return false;}p++;
    char**params=NULL;Type*ptypes=NULL;size_t pc=0,pcap=0;
    while(*p&&*p!=')'){
        Type pt=type_unknown();
        if(!parse_type_ref(&p,&pt)){for(size_t i=0;i<pc;i++)free(params[i]);free(params);free(ptypes);ERR("function params require explicit type");return false;}
        while(*p&&isspace((unsigned char)*p))p++;if(!is_ic(*p)){for(size_t i=0;i<pc;i++)free(params[i]);free(params);free(ptypes);ERR("bad param name");return false;}
        char pn[128];size_t pnn=0;while(is_icc(*p)){if(pnn+1>=sizeof(pn)){for(size_t i=0;i<pc;i++)free(params[i]);free(params);free(ptypes);ERR("param name too long");return false;}pn[pnn++]=*p++;}pn[pnn]=0;
        if(pc==pcap){size_t nc=pcap?pcap*2:4;char**np=realloc(params,nc*sizeof(char*));Type*tp=realloc(ptypes,nc*sizeof(Type));if(!np||!tp){for(size_t i=0;i<pc;i++)free(params[i]);free(np?np:params);free(tp?tp:ptypes);ERR("out of memory");return false;}params=np;ptypes=tp;pcap=nc;}
        params[pc]=xdup(pn);ptypes[pc]=pt;pc++;
        while(*p&&isspace((unsigned char)*p))p++;if(*p==',')p++;
    }
    if(*p!=')'){for(size_t i=0;i<pc;i++)free(params[i]);free(params);free(ptypes);ERR("missing ')'");return false;}p++;
    while(*p&&isspace((unsigned char)*p))p++;if(*p!=':'){for(size_t i=0;i<pc;i++)free(params[i]);free(params);free(ptypes);ERR("expected ':'");return false;}
    *oname=xdup(name);*oparams=params;*optypes=ptypes;*opc=pc;return true;
}
static bool parse_program(const SList*lines,Program*prog){
    prog->fns=NULL;prog->nfns=0;prog->cap=0;prog->n_ordered_structs=0;
    for(size_t i=0;i<STRUCT_MAP_CAP;i++){prog->structs.keys[i]=NULL;prog->structs.vals[i]=NULL;}
    size_t i=0;
    while(i<lines->len){
        const char*r=lines->items[i];char*cp=xdup(r);rtrim(cp);if(blank(cp)){free(cp);i++;continue;}free(cp);
        if(r[0]==' '||r[0]=='\t'){fprintf(stderr,"Error line %zu: unexpected indent\n",i+1);return false;}
        char*rl=xdup(r);rtrim(rl);strip_comment(rl);
        if(strncmp(rl,"struct ",7)==0||strncmp(rl,"struct\t",7)==0){
            const char*p=rl+7;skip(&p);char sname[128];size_t sn=0;
            if(!is_ic(*p)){fprintf(stderr,"Error line %zu: bad struct name\n",i+1);free(rl);return false;}
            while(is_icc(*p)){if(sn+1>=sizeof(sname)){fprintf(stderr,"Error line %zu: struct name too long\n",i+1);free(rl);return false;}sname[sn++]=*p++;}sname[sn]=0;
            skip(&p);if(*p!=':'){fprintf(stderr,"Error line %zu: expected ':'\n",i+1);free(rl);return false;}
            size_t bs=i+1,be=blk_end(lines,bs,lines->len,4);
            if(bs==be){fprintf(stderr,"Error in '%s': struct needs fields\n",sname);free(rl);return false;}
            StructDef*sd=calloc(1,sizeof(StructDef));if(!sd){free(rl);return false;}
            sd->name=xdup(sname);
            if(!sm_put(&prog->structs,sname,sd)){fprintf(stderr,"Error line %zu: duplicate struct '%s'\n",i+1,sname);free(sd->name);free(sd);free(rl);return false;}
            for(size_t j=bs;j<be;j++){
                const char*fr=lines->items[j];char*fcp=xdup(fr);rtrim(fcp);if(blank(fcp)){free(fcp);continue;}
                strip_comment(fcp);const char*fp=ltrim(fcp);char fname[128];size_t fn=0;
                if(!is_ic(*fp)){fprintf(stderr,"Error line %zu: bad field name\n",j+1);free(fcp);free(rl);return false;}
                while(is_icc(*fp)){if(fn+1>=sizeof(fname)){fprintf(stderr,"Error line %zu: field too long\n",j+1);free(fcp);free(rl);return false;}fname[fn++]=*fp++;}fname[fn]=0;
                Type ft;if(!parse_type_ref(&fp,&ft)){fprintf(stderr,"Error line %zu: bad type for field '%s'\n",j+1,fname);free(fcp);free(rl);return false;}
                if(!is_valid_type(ft, &prog->structs)){fprintf(stderr,"Error line %zu: unknown struct '%s'\n",j+1,ft.struct_name);free(fcp);free(rl);return false;}
                if(sd->nfields==sd->cap){size_t nc=sd->cap?sd->cap*2:4;StructField*np=realloc(sd->fields,nc*sizeof(StructField));if(!np){free(fcp);free(rl);return false;}sd->fields=np;sd->cap=nc;}
                sd->fields[sd->nfields].name=xdup(fname);sd->fields[sd->nfields].type=ft;sd->nfields++;free(fcp);
            }
            if(prog->n_ordered_structs<256)prog->ordered_structs[prog->n_ordered_structs++]=sd;
            free(rl);i=be;continue;
        }
        char*fn_name;char**params;Type*ptypes;size_t pc;
        if(!parse_fn_hdr(rl,&fn_name,&params,&ptypes,&pc)){fprintf(stderr,"Error line %zu: %s\n",i+1,g_err);free(rl);return false;}free(rl);
        for(size_t k=0;k<pc;k++){if(!is_valid_type(ptypes[k],&prog->structs)){fprintf(stderr,"Error line %zu: unknown struct in parameter '%s'\n",i+1,params[k]);return false;}}
        size_t bs=i+1,be=blk_end(lines,bs,lines->len,4);if(bs==be){fprintf(stderr,"Error: fn '%s' empty\n",fn_name);free(fn_name);for(size_t j=0;j<pc;j++)free(params[j]);free(params);free(ptypes);return false;}
        Stmt**body;size_t bc;if(!parse_block(lines,bs,be,4,&body,&bc)){fprintf(stderr,"Error in '%s': %s\n",fn_name,g_err);free(fn_name);for(size_t j=0;j<pc;j++)free(params[j]);free(params);free(ptypes);return false;}
        if(prog->nfns==prog->cap){size_t nc=prog->cap?prog->cap*2:4;FnDef*np=realloc(prog->fns,nc*sizeof(FnDef));if(!np){free(fn_name);for(size_t j=0;j<pc;j++)free(params[j]);free(params);free(ptypes);for(size_t j=0;j<bc;j++)free_stmt(body[j]);free(body);return false;}prog->fns=np;prog->cap=nc;}
        FnDef*fn=&prog->fns[prog->nfns++];fn->name=fn_name;fn->params=params;fn->nparams=pc;fn->param_types=ptypes;fn->return_type=type_int();fn->body=body;fn->nbody=bc;
        i=be;
    }
    return true;
}

static Type type_check_expr(Expr*e,Scope*sc,Program*prog,bool in_unsafe);
static size_t g_type_line=0;
static void type_errorf(const char*fmt,...){
    char msg[384];va_list ap;va_start(ap,fmt);vsnprintf(msg,sizeof(msg),fmt,ap);va_end(ap);
    ERR("line %zu: %s",g_type_line,msg);
}
#define TERR(...) type_errorf(__VA_ARGS__)
static FnDef*find_fn(Program*prog,const char*name){for(size_t i=0;i<prog->nfns;i++)if(strcmp(prog->fns[i].name,name)==0)return&prog->fns[i];return NULL;}
static Type type_check_expr(Expr*e,Scope*sc,Program*prog,bool in_unsafe){
    if(!e)return type_void();
    switch(e->kind){
    case EK_INT:e->type=type_int();return e->type;
    case EK_BOOL:e->type=type_bool();return e->type;
    case EK_STRING:e->type=type_string();return e->type;
    case EK_PTR_NULL:e->type=type_null_ptr();return e->type;
    case EK_VAR:{Symbol*sym=scope_find(sc,e->name);if(!sym){TERR("TypeError: undefined '%s'",e->name);return type_unknown();}e->type=sym->type;return e->type;}
    case EK_INT_CAST:{if(e->argc!=1)return type_unknown();Type at=type_check_expr(e->args[0],sc,prog,in_unsafe);if(type_is_unknown(at))return type_unknown();if(type_is_ptr(at)||type_eq(at,type_int())||type_eq(at,type_bool())){e->type=type_int();return e->type;}TERR("TypeError: int() cannot cast %s",type_name(at));return type_unknown();}
    case EK_PTR_CAST:{if(e->argc!=1)return type_unknown();if(!is_valid_type(e->cast_type,&prog->structs)){TERR("TypeError: unknown struct target in cast");return type_unknown();}if(!type_is_ptr(e->cast_type)){TERR("TypeError: cast target must be pointer type");return type_unknown();}Type at=type_check_expr(e->args[0],sc,prog,in_unsafe);if(type_is_unknown(at))return type_unknown();if(!(type_eq(at,type_int())||type_is_ptr(at))){TERR("TypeError: cannot cast %s to %s",type_name(at),type_name(e->cast_type));return type_unknown();}e->type=e->cast_type;return e->type;}
    case EK_ALLOC:{if(e->argc!=1)return type_unknown();Type at=type_check_expr(e->args[0],sc,prog,in_unsafe);if(type_is_unknown(at))return type_unknown();if(!type_eq(at,type_int())){TERR("TypeError: alloc() requires int, got %s",type_name(at));return type_unknown();}e->type=type_null_ptr();return e->type;}
    case EK_FREE:{if(e->argc!=1)return type_unknown();Type at=type_check_expr(e->args[0],sc,prog,in_unsafe);if(type_is_unknown(at))return type_unknown();if(!type_is_ptr(at)){TERR("TypeError: free() requires pointer, got %s",type_name(at));return type_unknown();}e->type=type_void();return e->type;}
    case EK_IN:{if(e->argc!=1)return type_unknown();if(!in_unsafe){TERR("UnsafeError: in() requires unsafe block");return type_unknown();}Type at=type_check_expr(e->args[0],sc,prog,in_unsafe);if(type_is_unknown(at))return type_unknown();if(!type_eq(at,type_int())){TERR("TypeError: in() port must be int, got %s",type_name(at));return type_unknown();}e->type=type_int();return e->type;}
    case EK_DEREF:{Type it=type_check_expr(e->left,sc,prog,in_unsafe);if(type_is_unknown(it))return type_unknown();if(!in_unsafe){TERR("UnsafeError: dereference requires unsafe block");return type_unknown();}if(!type_is_ptr(it)){TERR("TypeError: cannot dereference %s",type_name(it));return type_unknown();}if(it.base==BASE_VOID&&it.ptr_depth==1){TERR("TypeError: cannot dereference ptr<void>");return type_unknown();}e->type=type_deref(it);return e->type;}
    case EK_ADDROF:{Type it=type_check_expr(e->left,sc,prog,in_unsafe);if(type_is_unknown(it))return type_unknown();if(e->left->kind!=EK_VAR&&e->left->kind!=EK_FIELD&&e->left->kind!=EK_INDEX){TERR("TypeError: address-of requires variable or location");return type_unknown();}e->type=type_addr_of(it);return e->type;}
    case EK_CALL:{
        StructDef* sd=sm_get(&prog->structs,e->fn);
        if(sd){
            e->kind=EK_STRUCT_INIT;
            if(sd->nfields!=e->argc){TERR("TypeError: '%s' expects %zu arguments, got %zu",e->fn,sd->nfields,e->argc);return type_unknown();}
            for(size_t i=0;i<e->argc;i++){
                Type at=type_check_expr(e->args[i],sc,prog,in_unsafe);if(type_is_unknown(at))return type_unknown();
                if(!type_assignable(sd->fields[i].type,at)){TERR("TypeError: field '%s' expects %s, got %s",sd->fields[i].name,type_name(sd->fields[i].type),type_name(at));return type_unknown();}
            }
            e->type=type_struct(sd->name);return e->type;
        }
        FnDef*fn=find_fn(prog,e->fn);if(!fn){TERR("TypeError: undefined function or struct '%s'",e->fn);return type_unknown();}if(fn->nparams!=e->argc){TERR("TypeError: function '%s' expects %zu args, got %zu",e->fn,fn->nparams,e->argc);return type_unknown();}for(size_t i=0;i<e->argc;i++){Type at=type_check_expr(e->args[i],sc,prog,in_unsafe);if(type_is_unknown(at))return type_unknown();if(!type_assignable(fn->param_types[i],at)){TERR("TypeError: arg %zu of '%s' expects %s, got %s",i+1,e->fn,type_name(fn->param_types[i]),type_name(at));return type_unknown();}}e->type=fn->return_type;return e->type;
    }
    case EK_FIELD:{
        Type lt=type_check_expr(e->left,sc,prog,in_unsafe);if(type_is_unknown(lt))return type_unknown();
        if(lt.base!=BASE_STRUCT||type_is_ptr(lt)||lt.is_array){TERR("TypeError: cannot access field on %s",type_name(lt));return type_unknown();}
        StructDef*sd=sm_get(&prog->structs,lt.struct_name);if(!sd)return type_unknown();
        for(size_t i=0;i<sd->nfields;i++)if(strcmp(sd->fields[i].name,e->name)==0){e->type=sd->fields[i].type;return e->type;}
        TERR("TypeError: struct '%s' has no field '%s'",sd->name,e->name);return type_unknown();
    }
    case EK_PTR_FIELD:{
        Type lt=type_check_expr(e->left,sc,prog,in_unsafe);if(type_is_unknown(lt))return type_unknown();
        if(lt.base!=BASE_STRUCT||!type_is_ptr(lt)){TERR("TypeError: pointer field access requires ptr<struct>");return type_unknown();}
        if(!in_unsafe){TERR("UnsafeError: pointer field access requires unsafe block");return type_unknown();}
        StructDef*sd=sm_get(&prog->structs,lt.struct_name);if(!sd)return type_unknown();
        for(size_t i=0;i<sd->nfields;i++)if(strcmp(sd->fields[i].name,e->name)==0){e->type=sd->fields[i].type;return e->type;}
        TERR("TypeError: struct '%s' has no field '%s'",sd->name,e->name);return type_unknown();
    }
    case EK_INDEX:{
        Type lt=type_check_expr(e->left,sc,prog,in_unsafe);if(type_is_unknown(lt))return type_unknown();
        Type rt=type_check_expr(e->right,sc,prog,in_unsafe);if(type_is_unknown(rt))return type_unknown();
        if(!type_eq(rt,type_int())){TERR("TypeError: index must be integer");return type_unknown();}
        if(lt.is_array){Type el=lt;el.is_array=false;el.array_size=0;e->type=el;return el;}
        fprintf(stderr, "DEBUG INDEX: base=%d is_array=%d ptr_depth=%d\n", lt.base, lt.is_array, lt.ptr_depth);
        if(type_is_ptr(lt)){if(!in_unsafe){TERR("UnsafeError: pointer indexing requires unsafe block");return type_unknown();}Type el=lt;el.ptr_depth--;e->type=el;return el;}
        TERR("TypeError: cannot index %s",type_name(lt));return type_unknown();
    }
    case EK_SIZEOF:{
        if(e->cast_type.base==BASE_STRUCT){if(!sm_get(&prog->structs,e->cast_type.struct_name)){TERR("TypeError: unknown struct '%s'",e->cast_type.struct_name);return type_unknown();}}
        e->type=type_int();return e->type;
    }
    case EK_OFFSETOF:{
        if(e->cast_type.base!=BASE_STRUCT||e->cast_type.ptr_depth>0||e->cast_type.is_array){TERR("TypeError: offsetof requires struct");return type_unknown();}
        StructDef*sd=sm_get(&prog->structs,e->cast_type.struct_name);if(!sd){TERR("TypeError: unknown struct");return type_unknown();}
        bool f=false;for(size_t i=0;i<sd->nfields;i++)if(strcmp(sd->fields[i].name,e->name)==0){f=true;break;}
        if(!f){TERR("TypeError: struct '%s' has no field '%s'",sd->name,e->name);return type_unknown();}
        e->type=type_int();return e->type;
    }
    case EK_BINOP:{Type lt=type_check_expr(e->left,sc,prog,in_unsafe);Type rt=type_check_expr(e->right,sc,prog,in_unsafe);if(type_is_unknown(lt)||type_is_unknown(rt))return type_unknown();const char*op=e->op;
        if(strcmp(op,"+")==0){if(type_eq(lt,type_int())&&type_eq(rt,type_int())){e->type=type_int();return e->type;}if(type_eq(lt,type_string())&&type_eq(rt,type_string())){e->type=type_string();return e->type;}if(type_is_ptr(lt)&&type_eq(rt,type_int())){e->type=lt;return e->type;}TERR("TypeError: invalid operation %s + %s",type_name(lt),type_name(rt));return type_unknown();}
        if(strcmp(op,"-")==0){if(type_eq(lt,type_int())&&type_eq(rt,type_int())){e->type=type_int();return e->type;}if(type_is_ptr(lt)&&type_eq(rt,type_int())){e->type=lt;return e->type;}TERR("TypeError: invalid operation %s - %s",type_name(lt),type_name(rt));return type_unknown();}
        if(strcmp(op,"*")==0||strcmp(op,"/")==0){if(!(type_eq(lt,type_int())&&type_eq(rt,type_int()))){TERR("TypeError: invalid operation %s %s %s",type_name(lt),op,type_name(rt));return type_unknown();}e->type=type_int();return e->type;}
        if(strcmp(op,"<")==0||strcmp(op,">")==0||strcmp(op,"<=")==0||strcmp(op,">=")==0){if(!(type_eq(lt,type_int())&&type_eq(rt,type_int()))){TERR("TypeError: invalid comparison %s %s %s",type_name(lt),op,type_name(rt));return type_unknown();}e->type=type_bool();return e->type;}
        if(strcmp(op,"==")==0||strcmp(op,"!=")==0){if(!(type_eq(lt,rt)||(type_is_ptr(lt)&&type_eq(rt,type_null_ptr()))||(type_is_ptr(rt)&&type_eq(lt,type_null_ptr())))){TERR("TypeError: invalid comparison %s %s %s",type_name(lt),op,type_name(rt));return type_unknown();}e->type=type_bool();return e->type;}
        TERR("TypeError: unknown operator '%s'",op);return type_unknown();}
    case EK_RANGE:{Type st=type_check_expr(e->range_start,sc,prog,in_unsafe);if(type_is_unknown(st))return type_unknown();if(!type_eq(st,type_int())){TERR("TypeError: range start must be int, got %s",type_name(st));return type_unknown();}Type et=type_check_expr(e->range_end,sc,prog,in_unsafe);if(type_is_unknown(et))return type_unknown();if(!type_eq(et,type_int())){TERR("TypeError: range end must be int, got %s",type_name(et));return type_unknown();}if(e->range_step){Type pt=type_check_expr(e->range_step,sc,prog,in_unsafe);if(type_is_unknown(pt))return type_unknown();if(!type_eq(pt,type_int())){TERR("TypeError: range step must be int, got %s",type_name(pt));return type_unknown();}if(e->range_step->kind==EK_INT&&e->range_step->ival==0){TERR("TypeError: range step cannot be zero");return type_unknown();}}e->type=type_int();return e->type;}
    default:TERR("TypeError: unknown expression");return type_unknown();
    }
}
static bool type_check_stmts(Stmt**stmts,size_t count,Scope*sc,Program*prog,bool in_loop,bool in_unsafe);
static bool type_check_stmt(Stmt*s,Scope*sc,Program*prog,bool in_loop,bool in_unsafe){
    g_type_line=s?s->line:0;
    switch(s->kind){
    case SK_PRINT:{Type t=type_check_expr(s->expr,sc,prog,in_unsafe);if(type_is_unknown(t))return false;if(!(type_eq(t,type_int())||type_eq(t,type_bool())||type_eq(t,type_string()))){TERR("TypeError: print supports int, bool, str");return false;}return true;}
    case SK_DECL:{if(!is_valid_type(s->var_type,&prog->structs)){TERR("TypeError: unknown struct in declaration");return false;}if(scope_find(sc,s->var)){TERR("TypeError: variable '%s' already declared",s->var);return false;}return scope_add(sc,s->var,s->var_type,0);} 
    case SK_ASSIGN:{if(!is_valid_type(s->var_type,&prog->structs)){TERR("TypeError: unknown struct in assignment");return false;}if(!s->var){Type tt=type_check_expr(s->cond,sc,prog,in_unsafe);if(type_is_unknown(tt))return false;if(s->cond->kind!=EK_DEREF&&s->cond->kind!=EK_FIELD&&s->cond->kind!=EK_PTR_FIELD&&s->cond->kind!=EK_INDEX){TERR("TypeError: left side must be variable, dereference, field, or index");return false;}if(tt.is_array){TERR("TypeError: cannot manually reassign complete arrays");return false;}Type vt=type_check_expr(s->expr,sc,prog,in_unsafe);if(type_is_unknown(vt))return false;if(!type_assignable(tt,vt)){TERR("TypeError: cannot assign %s to %s",type_name(vt),type_name(tt));return false;}return true;}
        Symbol*sym=scope_find(sc,s->var);if(!sym){Type t=type_check_expr(s->expr,sc,prog,in_unsafe);if(type_is_unknown(t))return false;Type final_t=t;if(!type_is_unknown(s->var_type)){if(!type_assignable(s->var_type,t)){TERR("TypeError: cannot assign %s to %s",type_name(t),type_name(s->var_type));return false;}final_t=s->var_type;}if(final_t.is_array){TERR("TypeError: assigned arrays require local definition mapping limits");return false;}if(!scope_add(sc,s->var,final_t,0))return false;s->var_type=final_t;}else{if(sym->type.is_array){TERR("TypeError: cannot reassign array natively");return false;}Type t=type_check_expr(s->expr,sc,prog,in_unsafe);if(type_is_unknown(t))return false;if(!type_assignable(sym->type,t)){TERR("TypeError: cannot assign %s to %s",type_name(t),type_name(sym->type));return false;}s->var_type=sym->type;}return true;}
    case SK_IF:{Type ct=type_check_expr(s->cond,sc,prog,in_unsafe);if(type_is_unknown(ct))return false;if(!(type_eq(ct,type_int())||type_eq(ct,type_bool()))){TERR("TypeError: condition must be bool or int, got %s",type_name(ct));return false;}Scope inner;scope_init(&inner,sc);if(!type_check_stmts(s->then,s->nthen,&inner,prog,in_loop,in_unsafe)){scope_free(&inner);return false;}scope_free(&inner);if(s->nels>0){Scope e_scope;scope_init(&e_scope,sc);if(!type_check_stmts(s->els,s->nels,&e_scope,prog,in_loop,in_unsafe)){scope_free(&e_scope);return false;}scope_free(&e_scope);}return true;}
    case SK_WHILE:{Type ct=type_check_expr(s->cond,sc,prog,in_unsafe);if(type_is_unknown(ct))return false;if(!(type_eq(ct,type_int())||type_eq(ct,type_bool()))){TERR("TypeError: condition must be bool or int, got %s",type_name(ct));return false;}Scope inner;scope_init(&inner,sc);if(!type_check_stmts(s->loop,s->nloop,&inner,prog,true,in_unsafe)){scope_free(&inner);return false;}scope_free(&inner);return true;}
    case SK_UNSAFE:{Scope inner;scope_init(&inner,sc);if(!type_check_stmts(s->loop,s->nloop,&inner,prog,in_loop,true)){scope_free(&inner);return false;}scope_free(&inner);return true;}
    case SK_ASM:{if(!in_unsafe){TERR("UnsafeError: asm requires unsafe block");return false;}return true;}
    case SK_OUT:{if(!in_unsafe){TERR("UnsafeError: out() requires unsafe block");return false;}Type pt=type_check_expr(s->cond,sc,prog,in_unsafe);if(type_is_unknown(pt))return false;if(!type_eq(pt,type_int())){TERR("TypeError: out() port must be int, got %s",type_name(pt));return false;}Type vt=type_check_expr(s->expr,sc,prog,in_unsafe);if(type_is_unknown(vt))return false;if(!type_eq(vt,type_int())){TERR("TypeError: out() value must be int, got %s",type_name(vt));return false;}return true;}
    case SK_FOR:{Type rt=type_check_expr(s->range_expr,sc,prog,in_unsafe);if(type_is_unknown(rt))return false;if(!type_eq(rt,type_int())){TERR("TypeError: range expression must be int, got %s",type_name(rt));return false;}Scope inner;scope_init(&inner,sc);if(!scope_add(&inner,s->loop_var,type_int(),0)){scope_free(&inner);return false;}if(!type_check_stmts(s->loop,s->nloop,&inner,prog,true,in_unsafe)){scope_free(&inner);return false;}scope_free(&inner);return true;}
    case SK_RETURN:{if(s->expr){Type t=type_check_expr(s->expr,sc,prog,in_unsafe);if(type_is_unknown(t))return false;}return true;}
    case SK_BREAK:case SK_CONTINUE:if(!in_loop){TERR("TypeError: break/continue outside loop");return false;}return true;
    case SK_CALL:{Type t=type_check_expr(s->expr,sc,prog,in_unsafe);return!type_is_unknown(t);}
    default:return true;
    }
}
static bool type_check_stmts(Stmt**stmts,size_t count,Scope*sc,Program*prog,bool in_loop,bool in_unsafe){for(size_t i=0;i<count;i++)if(!type_check_stmt(stmts[i],sc,prog,in_loop,in_unsafe))return false;return true;}
static bool type_check_fn(FnDef*fn,Program*prog){Scope sc;scope_init(&sc,NULL);for(size_t i=0;i<fn->nparams;i++){if(!scope_add(&sc,fn->params[i],fn->param_types[i],1)){scope_free(&sc);return false;}}if(!type_check_stmts(fn->body,fn->nbody,&sc,prog,false,false)){scope_free(&sc);return false;}scope_free(&sc);return true;}
static bool type_check_program(Program*prog){for(size_t i=0;i<prog->nfns;i++)if(!type_check_fn(&prog->fns[i],prog)){fprintf(stderr,"TypeError in '%s': %s\n",prog->fns[i].name,g_err);return false;}return true;}

/* ============ ASM x86_64 Code Generator (GAS syntax) ============ */
typedef struct {
    SBuf out,rodata;size_t lbl,str_count;
    char**var_names;int*var_offsets;size_t var_count,var_cap;
    size_t loop_start[64],loop_end[64];int loop_depth;
    size_t frame_size,for_counter;
} CG;
static void cg_init(CG*g){sb_init(&g->out);sb_init(&g->rodata);g->lbl=0;g->str_count=0;g->var_names=NULL;g->var_offsets=NULL;g->var_count=0;g->var_cap=0;g->loop_depth=0;g->frame_size=0;g->for_counter=0;}
static void cg_free(CG*g){sb_free(&g->out);sb_free(&g->rodata);for(size_t i=0;i<g->var_count;i++)free(g->var_names[i]);free(g->var_names);free(g->var_offsets);}
static size_t L(CG*g){return g->lbl++;}
static void E(CG*g,const char*fmt,...){char tmp[1024];va_list ap;va_start(ap,fmt);vsnprintf(tmp,sizeof(tmp),fmt,ap);va_end(ap);sb_cat(&g->out,tmp);}
static void R(CG*g,const char*fmt,...){char tmp[1024];va_list ap;va_start(ap,fmt);vsnprintf(tmp,sizeof(tmp),fmt,ap);va_end(ap);sb_cat(&g->rodata,tmp);}
static void cg_reset_vars(CG*g){for(size_t i=0;i<g->var_count;i++)free(g->var_names[i]);free(g->var_names);free(g->var_offsets);g->var_names=NULL;g->var_offsets=NULL;g->var_count=0;g->var_cap=0;}
static int cg_var_offset(CG*g,const char*name){for(size_t i=0;i<g->var_count;i++)if(strcmp(g->var_names[i],name)==0)return g->var_offsets[i];return 0;}
static void cg_add_var(CG*g,const char*name,int offset){
    if(g->var_count==g->var_cap){size_t nc=g->var_cap?g->var_cap*2:16;g->var_names=realloc(g->var_names,nc*sizeof(char*));g->var_offsets=realloc(g->var_offsets,nc*sizeof(int));g->var_cap=nc;}
    g->var_names[g->var_count]=xdup(name);g->var_offsets[g->var_count]=offset;g->var_count++;
}
static size_t cg_add_string(CG*g,const char*s){
    size_t idx=g->str_count++;R(g,".LC_str_%zu:\n    .string \"",idx);
    for(size_t i=0;s[i];i++){unsigned char c=(unsigned char)s[i];
        switch(c){case'\n':R(g,"\\n");break;case'\t':R(g,"\\t");break;case'\r':R(g,"\\r");break;case'\\':R(g,"\\\\");break;case'"':R(g,"\\\"");break;
        default:if(c>=32&&c<127){char t[2]={(char)c,0};R(g,"%s",t);}else R(g,"\\%03o",c);break;}}
    R(g,"\"\n");return idx;
}
static void cg_emit_runtime(CG*g){
    E(g,"\n_aut_strlen:\n    xorq %%rax, %%rax\n.L_sl_loop:\n    cmpb $0, (%%rdi,%%rax)\n    je .L_sl_done\n    incq %%rax\n    jmp .L_sl_loop\n.L_sl_done:\n    ret\n");
    E(g,"\n_aut_print_newline:\n    pushq %%rbp\n    movq %%rsp, %%rbp\n    subq $16, %%rsp\n    movb $10, -1(%%rbp)\n    movq $1, %%rax\n    movq $1, %%rdi\n    leaq -1(%%rbp), %%rsi\n    movq $1, %%rdx\n    syscall\n    leave\n    ret\n");
    E(g,"\n_aut_print_str:\n    pushq %%rbp\n    movq %%rsp, %%rbp\n    subq $16, %%rsp\n    movq %%rdi, -8(%%rbp)\n    call _aut_strlen\n    movq %%rax, %%rdx\n    movq $1, %%rax\n    movq $1, %%rdi\n    movq -8(%%rbp), %%rsi\n    syscall\n    call _aut_print_newline\n    leave\n    ret\n");
    E(g,"\n_aut_print_int:\n    pushq %%rbp\n    movq %%rsp, %%rbp\n    subq $48, %%rsp\n    movq %%rdi, %%rax\n    leaq -1(%%rbp), %%r8\n    xorq %%rcx, %%rcx\n    xorq %%r9, %%r9\n    testq %%rax, %%rax\n    jns .L_pi_pos\n    movq $1, %%r9\n    negq %%rax\n");
    E(g,".L_pi_pos:\n    testq %%rax, %%rax\n    jnz .L_pi_loop\n    movb $48, (%%r8)\n    incq %%rcx\n    jmp .L_pi_write\n");
    E(g,".L_pi_loop:\n    xorq %%rdx, %%rdx\n    movq $10, %%r10\n    divq %%r10\n    addb $48, %%dl\n    movb %%dl, (%%r8)\n    decq %%r8\n    incq %%rcx\n    testq %%rax, %%rax\n    jnz .L_pi_loop\n    incq %%r8\n");
    E(g,".L_pi_write:\n    testq %%r9, %%r9\n    jz .L_pi_nosign\n    decq %%r8\n    movb $45, (%%r8)\n    incq %%rcx\n.L_pi_nosign:\n    movq $1, %%rax\n    movq $1, %%rdi\n    movq %%r8, %%rsi\n    movq %%rcx, %%rdx\n    syscall\n    call _aut_print_newline\n    leave\n    ret\n");
}
/* ============ Expression Codegen ============ */
static bool cg_asm_expr(CG*g,Expr*e);
static bool cg_asm_stmts(CG*g,Stmt**s,size_t n);
static bool cg_asm_expr(CG*g,Expr*e){
    switch(e->kind){
    case EK_INT:
        if(e->ival>=-2147483647LL&&e->ival<=2147483647LL)E(g,"    pushq $%lld\n",e->ival);
        else{E(g,"    movabsq $%lld, %%rax\n",e->ival);E(g,"    pushq %%rax\n");}
        return true;
    case EK_BOOL:E(g,"    pushq $%lld\n",e->ival);return true;
    case EK_STRING:{size_t idx=cg_add_string(g,e->sval);E(g,"    leaq .LC_str_%zu(%%rip), %%rax\n",idx);E(g,"    pushq %%rax\n");return true;}
    case EK_VAR:{int off=cg_var_offset(g,e->name);if(!off){ERR("ASM: unknown var '%s'",e->name);return false;}E(g,"    movq %d(%%rbp), %%rax\n",off);E(g,"    pushq %%rax\n");return true;}
    case EK_BINOP:{
        const char*op=e->op;
        if(strcmp(op,"+")==0&&type_eq(e->type,type_string())){
            if(e->left->kind!=EK_STRING||e->right->kind!=EK_STRING){ERR("str+str only for literals");return false;}
            size_t ln=strlen(e->left->sval),rn=strlen(e->right->sval);char*j=malloc(ln+rn+1);if(!j)return false;
            memcpy(j,e->left->sval,ln);memcpy(j+ln,e->right->sval,rn+1);size_t idx=cg_add_string(g,j);free(j);
            E(g,"    leaq .LC_str_%zu(%%rip), %%rax\n",idx);E(g,"    pushq %%rax\n");return true;
        }
        if(!cg_asm_expr(g,e->left))return false;if(!cg_asm_expr(g,e->right))return false;
        E(g,"    popq %%rbx\n");E(g,"    popq %%rax\n");
        if(strcmp(op,"+")==0){E(g,"    addq %%rbx, %%rax\n");}
        else if(strcmp(op,"-")==0){E(g,"    subq %%rbx, %%rax\n");}
        else if(strcmp(op,"*")==0){E(g,"    imulq %%rbx, %%rax\n");}
        else if(strcmp(op,"/")==0){E(g,"    cqto\n    idivq %%rbx\n");}
        else if(strcmp(op,"==")==0){E(g,"    cmpq %%rbx, %%rax\n    sete %%al\n    movzbq %%al, %%rax\n");}
        else if(strcmp(op,"!=")==0){E(g,"    cmpq %%rbx, %%rax\n    setne %%al\n    movzbq %%al, %%rax\n");}
        else if(strcmp(op,"<")==0){E(g,"    cmpq %%rbx, %%rax\n    setl %%al\n    movzbq %%al, %%rax\n");}
        else if(strcmp(op,">")==0){E(g,"    cmpq %%rbx, %%rax\n    setg %%al\n    movzbq %%al, %%rax\n");}
        else if(strcmp(op,"<=")==0){E(g,"    cmpq %%rbx, %%rax\n    setle %%al\n    movzbq %%al, %%rax\n");}
        else if(strcmp(op,">=")==0){E(g,"    cmpq %%rbx, %%rax\n    setge %%al\n    movzbq %%al, %%rax\n");}
        else{ERR("ASM: unknown op '%s'",op);return false;}
        E(g,"    pushq %%rax\n");return true;
    }
    case EK_CALL:{
        static const char*argregs[]={"%rdi","%rsi","%rdx","%rcx","%r8","%r9"};
        if(e->argc>6){ERR("ASM v1: max 6 args");return false;}
        for(size_t i=0;i<e->argc;i++){if(!cg_asm_expr(g,e->args[i]))return false;}
        for(int i=(int)e->argc-1;i>=0;i--)E(g,"    popq %s\n",argregs[i]);
        E(g,"    movq %%rsp, %%r15\n    andq $-16, %%rsp\n    call _fn_%s\n    movq %%r15, %%rsp\n",e->fn);
        E(g,"    pushq %%rax\n");return true;
    }
    case EK_INT_CAST:if(e->argc!=1)return false;return cg_asm_expr(g,e->args[0]);
    case EK_RANGE:ERR("range outside for");return false;
    case EK_PTR_NULL:case EK_PTR_CAST:case EK_ALLOC:case EK_FREE:case EK_DEREF:case EK_ADDROF:case EK_IN:
    case EK_FIELD:case EK_PTR_FIELD:case EK_INDEX:case EK_SIZEOF:case EK_OFFSETOF:case EK_STRUCT_INIT:
        ERR("ASM backend v1 does not support this feature");return false;
    default:ERR("ASM: unknown expr kind");return false;
    }
}
/* ============ Statement Codegen ============ */
static bool cg_asm_stmts(CG*g,Stmt**stmts,size_t count){
    for(size_t i=0;i<count;i++){Stmt*s=stmts[i];
    switch(s->kind){
    case SK_PRINT:{
        if(!cg_asm_expr(g,s->expr))return false;E(g,"    popq %%rdi\n");
        if(type_eq(s->expr->type,type_int())){
            E(g,"    movq %%rsp, %%r15\n    andq $-16, %%rsp\n    call _aut_print_int\n    movq %%r15, %%rsp\n");
        }else if(type_eq(s->expr->type,type_bool())){
            size_t lb=L(g);size_t ti=cg_add_string(g,"true");size_t fi=cg_add_string(g,"false");
            E(g,"    testq %%rdi, %%rdi\n    jz .L_pb_f_%zu\n",lb);
            E(g,"    leaq .LC_str_%zu(%%rip), %%rdi\n    jmp .L_pb_e_%zu\n",ti,lb);
            E(g,".L_pb_f_%zu:\n    leaq .LC_str_%zu(%%rip), %%rdi\n",lb,fi);
            E(g,".L_pb_e_%zu:\n    movq %%rsp, %%r15\n    andq $-16, %%rsp\n    call _aut_print_str\n    movq %%r15, %%rsp\n",lb);
        }else if(type_eq(s->expr->type,type_string())){
            E(g,"    movq %%rsp, %%r15\n    andq $-16, %%rsp\n    call _aut_print_str\n    movq %%r15, %%rsp\n");
        }else{E(g,"    movq %%rsp, %%r15\n    andq $-16, %%rsp\n    call _aut_print_int\n    movq %%r15, %%rsp\n");}
        break;}
    case SK_DECL:break;
    case SK_ASSIGN:{
        if(!s->var){ERR("ASM v1: complex LHS assignment not supported");return false;}
        if(!cg_asm_expr(g,s->expr))return false;
        int off=cg_var_offset(g,s->var);if(!off){ERR("ASM: unknown var '%s'",s->var);return false;}
        E(g,"    popq %%rax\n    movq %%rax, %d(%%rbp)\n",off);break;}
    case SK_IF:{
        size_t lb=L(g);if(!cg_asm_expr(g,s->cond))return false;
        E(g,"    popq %%rax\n    testq %%rax, %%rax\n    jz .L_else_%zu\n",lb);
        if(!cg_asm_stmts(g,s->then,s->nthen))return false;
        if(s->nels>0){E(g,"    jmp .L_endif_%zu\n.L_else_%zu:\n",lb,lb);
            if(!cg_asm_stmts(g,s->els,s->nels))return false;E(g,".L_endif_%zu:\n",lb);
        }else E(g,".L_else_%zu:\n",lb);
        break;}
    case SK_WHILE:{
        size_t ls=L(g),le=L(g);
        g->loop_start[g->loop_depth]=ls;g->loop_end[g->loop_depth]=le;g->loop_depth++;
        E(g,".L_loop_%zu:\n",ls);if(!cg_asm_expr(g,s->cond))return false;
        E(g,"    popq %%rax\n    testq %%rax, %%rax\n    jz .L_lend_%zu\n",le);
        if(!cg_asm_stmts(g,s->loop,s->nloop))return false;
        E(g,"    jmp .L_loop_%zu\n.L_lend_%zu:\n",ls,le);g->loop_depth--;break;}
    case SK_FOR:{
        Expr*r=s->range_expr;
        int var_off=cg_var_offset(g,s->loop_var);
        if(!var_off){ERR("ASM: unknown loop var '%s'",s->loop_var);return false;}
        size_t fi=g->for_counter++;
        char en[32],sn[32];snprintf(en,sizeof(en),"__for_end_%zu",fi);snprintf(sn,sizeof(sn),"__for_step_%zu",fi);
        int end_off=cg_var_offset(g,en),step_off=cg_var_offset(g,sn);
        /* Compute start -> loop var */
        if(!cg_asm_expr(g,r->range_start))return false;
        E(g,"    popq %%rax\n    movq %%rax, %d(%%rbp)\n",var_off);
        /* Compute end */
        if(!cg_asm_expr(g,r->range_end))return false;
        E(g,"    popq %%rax\n    movq %%rax, %d(%%rbp)\n",end_off);
        /* Compute step */
        if(r->range_step){if(!cg_asm_expr(g,r->range_step))return false;E(g,"    popq %%rax\n    movq %%rax, %d(%%rbp)\n",step_off);}
        else{size_t al=L(g);E(g,"    movq %d(%%rbp), %%rax\n    cmpq %d(%%rbp), %%rax\n    jg .L_neg_step_%zu\n",var_off,end_off,al);
            E(g,"    movq $1, %%rax\n    jmp .L_step_done_%zu\n.L_neg_step_%zu:\n    movq $-1, %%rax\n.L_step_done_%zu:\n",al,al,al);
            E(g,"    movq %%rax, %d(%%rbp)\n",step_off);}
        /* Loop */
        size_t ls=L(g),le=L(g),cl=L(g);
        g->loop_start[g->loop_depth]=ls;g->loop_end[g->loop_depth]=le;g->loop_depth++;
        E(g,".L_loop_%zu:\n",ls);
        E(g,"    movq %d(%%rbp), %%rax\n",var_off);
        E(g,"    movq %d(%%rbp), %%rbx\n",step_off);
        E(g,"    testq %%rbx, %%rbx\n    js .L_for_neg_%zu\n",cl);
        /* Positive step */
        E(g,"    cmpq %d(%%rbp), %%rax\n",end_off);
        if(r->range_inclusive)E(g,"    jg .L_lend_%zu\n",le);else E(g,"    jge .L_lend_%zu\n",le);
        E(g,"    jmp .L_for_body_%zu\n.L_for_neg_%zu:\n",cl,cl);
        /* Negative step */
        E(g,"    cmpq %d(%%rbp), %%rax\n",end_off);
        if(r->range_inclusive)E(g,"    jl .L_lend_%zu\n",le);else E(g,"    jle .L_lend_%zu\n",le);
        E(g,".L_for_body_%zu:\n",cl);
        if(!cg_asm_stmts(g,s->loop,s->nloop))return false;
        /* Increment */
        E(g,"    movq %d(%%rbp), %%rax\n    addq %%rax, %d(%%rbp)\n",step_off,var_off);
        E(g,"    jmp .L_loop_%zu\n.L_lend_%zu:\n",ls,le);g->loop_depth--;break;}
    case SK_RETURN:
        if(s->expr){if(!cg_asm_expr(g,s->expr))return false;E(g,"    popq %%rax\n");}
        else E(g,"    xorq %%rax, %%rax\n");
        E(g,"    movq -8(%%rbp), %%r15\n    leave\n    ret\n");break;
    case SK_BREAK:
        if(g->loop_depth<=0){ERR("break outside loop");return false;}
        E(g,"    jmp .L_lend_%zu\n",g->loop_end[g->loop_depth-1]);break;
    case SK_CONTINUE:
        if(g->loop_depth<=0){ERR("continue outside loop");return false;}
        E(g,"    jmp .L_loop_%zu\n",g->loop_start[g->loop_depth-1]);break;
    case SK_UNSAFE:if(!cg_asm_stmts(g,s->loop,s->nloop))return false;break;
    case SK_ASM:E(g,"    %s\n",s->asm_text?s->asm_text:"nop");break;
    case SK_OUT:ERR("ASM v1: out() not supported");return false;
    case SK_CALL:if(!cg_asm_expr(g,s->expr))return false;E(g,"    addq $8, %%rsp\n");break;
    default:break;
    }}
    return true;
}
/* ============ Function + Entry Point ============ */
typedef struct{char*name;Type type;int is_volatile;}VarInfo;
static bool collect_vars(Stmt**body,size_t nbody,VarInfo**vars,size_t*count,size_t*cap,FnDef*fn){
    for(size_t i=0;i<nbody;i++){Stmt*s=body[i];
        if((s->kind==SK_ASSIGN||s->kind==SK_DECL)&&s->var){bool found=false;for(size_t j=0;j<*count;j++)if(strcmp((*vars)[j].name,s->var)==0){found=true;break;}for(size_t j=0;j<fn->nparams;j++)if(strcmp(fn->params[j],s->var)==0){found=true;break;}if(!found){if(*count==*cap){size_t nc=*cap?*cap*2:8;VarInfo*np=realloc(*vars,nc*sizeof(VarInfo));if(!np)return false;*vars=np;*cap=nc;}(*vars)[*count].name=xdup(s->var);(*vars)[*count].type=type_is_unknown(s->var_type)?type_int():s->var_type;(*vars)[*count].is_volatile=s->var_volatile;(*count)++;}}
        if(s->kind==SK_IF){if(!collect_vars(s->then,s->nthen,vars,count,cap,fn))return false;if(!collect_vars(s->els,s->nels,vars,count,cap,fn))return false;}
        if(s->kind==SK_WHILE){if(!collect_vars(s->loop,s->nloop,vars,count,cap,fn))return false;}
        if(s->kind==SK_UNSAFE){if(!collect_vars(s->loop,s->nloop,vars,count,cap,fn))return false;}
        if(s->kind==SK_FOR){bool found=false;for(size_t j=0;j<*count;j++)if(strcmp((*vars)[j].name,s->loop_var)==0){found=true;break;}for(size_t j=0;j<fn->nparams;j++)if(strcmp(fn->params[j],s->loop_var)==0){found=true;break;}if(!found){if(*count==*cap){size_t nc=*cap?*cap*2:8;VarInfo*np=realloc(*vars,nc*sizeof(VarInfo));if(!np)return false;*vars=np;*cap=nc;}(*vars)[*count].name=xdup(s->loop_var);(*vars)[*count].type=type_int();(*count)++;}if(!collect_vars(s->loop,s->nloop,vars,count,cap,fn))return false;}
    }
    return true;
}
static size_t count_for_stmts(Stmt**body,size_t nbody){
    size_t c=0;
    for(size_t i=0;i<nbody;i++){Stmt*s=body[i];
        if(s->kind==SK_FOR){c++;c+=count_for_stmts(s->loop,s->nloop);}
        if(s->kind==SK_IF){c+=count_for_stmts(s->then,s->nthen);c+=count_for_stmts(s->els,s->nels);}
        if(s->kind==SK_WHILE)c+=count_for_stmts(s->loop,s->nloop);
        if(s->kind==SK_UNSAFE)c+=count_for_stmts(s->loop,s->nloop);
    }
    return c;
}
static bool cg_asm_fn(CG*g,FnDef*fn){
    static const char*argregs[]={"%rdi","%rsi","%rdx","%rcx","%r8","%r9"};
    cg_reset_vars(g);
    VarInfo*vars=NULL;size_t vcount=0,vcap=0;
    if(!collect_vars(fn->body,fn->nbody,&vars,&vcount,&vcap,fn)){free(vars);return false;}
    size_t nfor=count_for_stmts(fn->body,fn->nbody);
    /* Layout: rbp-8=saved r15, then params, locals, for-hidden-vars */
    int offset=-8; /* rbp-8 reserved for r15 */
    for(size_t i=0;i<fn->nparams;i++){offset-=8;cg_add_var(g,fn->params[i],offset);}
    for(size_t i=0;i<vcount;i++){offset-=8;cg_add_var(g,vars[i].name,offset);free(vars[i].name);}
    free(vars);
    for(size_t i=0;i<nfor;i++){
        char en[32],sn[32];snprintf(en,sizeof(en),"__for_end_%zu",i);snprintf(sn,sizeof(sn),"__for_step_%zu",i);
        offset-=8;cg_add_var(g,en,offset);offset-=8;cg_add_var(g,sn,offset);
    }
    size_t frame_size=(size_t)(-offset);
    if(frame_size%16!=0)frame_size+=16-(frame_size%16);
    g->frame_size=frame_size;g->for_counter=0;g->loop_depth=0;
    /* Prologue */
    E(g,"\n.globl _fn_%s\n_fn_%s:\n",fn->name,fn->name);
    E(g,"    pushq %%rbp\n    movq %%rsp, %%rbp\n");
    if(frame_size>0)E(g,"    subq $%zu, %%rsp\n",frame_size);
    E(g,"    movq %%r15, -8(%%rbp)\n");
    /* Store params from registers */
    for(size_t i=0;i<fn->nparams;i++){int po=cg_var_offset(g,fn->params[i]);E(g,"    movq %s, %d(%%rbp)\n",argregs[i],po);}
    /* Zero-init locals */
    for(size_t i=fn->nparams;i<g->var_count;i++)E(g,"    movq $0, %d(%%rbp)\n",g->var_offsets[i]);
    /* Body */
    if(!cg_asm_stmts(g,fn->body,fn->nbody))return false;
    /* Epilogue */
    E(g,"    xorq %%rax, %%rax\n    movq -8(%%rbp), %%r15\n    leave\n    ret\n");
    return true;
}
static bool codegen(const Program*prog,const char*out_path,bool no_runtime){
    CG g;cg_init(&g);
    E(&g,"# AutismLang v%s - x86_64 ASM backend\n.section .text\n",AUTISMLANG_VERSION);
    cg_emit_runtime(&g);
    for(size_t i=0;i<prog->nfns;i++){if(!cg_asm_fn(&g,(FnDef*)&prog->fns[i])){cg_free(&g);return false;}}
    if(!no_runtime){
        E(&g,"\n.globl _start\n_start:\n    xorq %%rbp, %%rbp\n    andq $-16, %%rsp\n    call _fn_main\n    movq $60, %%rax\n    xorq %%rdi, %%rdi\n    syscall\n");
    }else{
        E(&g,"\n.globl aut_entry\naut_entry:\n    call _fn_main\n    ret\n");
    }
    E(&g,"\n.section .rodata\n");
    if(g.rodata.d)sb_cat(&g.out,g.rodata.d);
    char*pc=xdup(out_path);for(size_t i=0;pc[i];i++){if(pc[i]=='/'||pc[i]=='\\'){char sv=pc[i];pc[i]=0;if(pc[0])MKDIR(pc);pc[i]=sv;}}free(pc);
    FILE*fp=fopen(out_path,"wb");if(!fp){cg_free(&g);return false;}fwrite(g.out.d,1,g.out.len,fp);fclose(fp);cg_free(&g);return true;
}
int main(int argc,char**argv){
    const char*in=NULL,*out="build/out.s";bool no_runtime=false;
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"--help")==0){printf("Usage: %s <input.aut> [-o output.s] [--no-runtime]\n",argv[0]);return 0;}
        else if(strcmp(argv[i],"--version")==0){printf("AutismLang %s\n",AUTISMLANG_VERSION);return 0;}
        else if(strcmp(argv[i],"--no-runtime")==0){no_runtime=true;}
        else if(strcmp(argv[i],"-o")==0){if(i+1>=argc)return 1;out=argv[++i];}
        else if(argv[i][0]=='-'){fprintf(stderr,"Unknown: %s\n",argv[i]);return 1;}
        else if(!in)in=argv[i];else{fprintf(stderr,"One file only\n");return 1;}
    }
    if(!in){fprintf(stderr,"Usage: %s <input.aut>\n",argv[0]);return 1;}
    char*src=read_file(in);if(!src)return 1;SList lines;sl_init(&lines);if(!split_lines(src,&lines)){free(src);return 1;}free(src);
    Program prog;if(!parse_program(&lines,&prog)){sl_free(&lines);return 1;}
    bool has_main=false;for(size_t i=0;i<prog.nfns;i++)if(strcmp(prog.fns[i].name,"main")==0){has_main=true;break;}
    if(!has_main){fprintf(stderr,"Error: missing fn main\n");free_program(&prog);sl_free(&lines);return 1;}
    if(!type_check_program(&prog)){free_program(&prog);sl_free(&lines);return 1;}
    if(!codegen(&prog,out,no_runtime)){free_program(&prog);sl_free(&lines);return 1;}
    printf("Compiled %s -> %s\n",in,out);free_program(&prog);sl_free(&lines);return 0;
}
