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

#define AUTISMLANG_VERSION "0.6.0"
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

typedef enum{BASE_UNKNOWN=0,BASE_INT,BASE_BOOL,BASE_STRING,BASE_VOID}BaseType;
typedef struct{BaseType base;unsigned ptr_depth;}Type;
static void skip(const char**pp);

static Type type_make(BaseType b,unsigned d){Type t;t.base=b;t.ptr_depth=d;return t;}
static Type type_unknown(void){return type_make(BASE_UNKNOWN,0);}
static Type type_int(void){return type_make(BASE_INT,0);}
static Type type_bool(void){return type_make(BASE_BOOL,0);}
static Type type_string(void){return type_make(BASE_STRING,0);}
static Type type_void(void){return type_make(BASE_VOID,0);}
static Type type_null_ptr(void){return type_make(BASE_VOID,1);} /* ptr<void> */
static bool type_is_unknown(Type t){return t.base==BASE_UNKNOWN&&t.ptr_depth==0;}
static bool type_is_ptr(Type t){return t.ptr_depth>0;}
static bool type_eq(Type a,Type b){return a.base==b.base&&a.ptr_depth==b.ptr_depth;}
static Type type_addr_of(Type t){if(type_is_unknown(t))return type_unknown();return type_make(t.base,t.ptr_depth+1);}
static Type type_deref(Type t){if(!type_is_ptr(t))return type_unknown();return type_make(t.base,t.ptr_depth-1);}
static bool type_assignable(Type dst,Type src){
    if(type_eq(dst,src))return true;
    if(type_is_ptr(dst)&&type_eq(src,type_null_ptr()))return true;
    if(type_is_ptr(dst)&&type_is_ptr(src)&&dst.ptr_depth==src.ptr_depth&&(dst.base==BASE_VOID||src.base==BASE_VOID))return true;
    return false;
}
static const char*type_name(Type t){
    static char bufs[8][96];static int bi=0;char inner[96];
    const char*base="unknown";
    if(t.base==BASE_INT)base="int";
    else if(t.base==BASE_BOOL)base="bool";
    else if(t.base==BASE_STRING)base="str";
    else if(t.base==BASE_VOID)base="void";
    snprintf(inner,sizeof(inner),"%s",base);
    for(unsigned i=0;i<t.ptr_depth;i++){
        char wrapped[96];snprintf(wrapped,sizeof(wrapped),"ptr<%s>",inner);snprintf(inner,sizeof(inner),"%s",wrapped);
    }
    bi=(bi+1)%8;snprintf(bufs[bi],sizeof(bufs[bi]),"%s",inner);return bufs[bi];
}
static bool parse_type_ref(const char**pp,Type*out){
    skip(pp);
    if(strncmp(*pp,"ptr",3)==0&&!is_icc((*pp)[3])){
        (*pp)+=3;skip(pp);
        if(**pp!='<'){*out=type_null_ptr();return true;}
        (*pp)++;
        Type inner;if(!parse_type_ref(pp,&inner))return false;
        skip(pp);if(**pp!='>'){ERR("expected '>' in pointer type");return false;}
        (*pp)++;
        if(inner.ptr_depth==255){ERR("pointer nesting too deep");return false;}
        *out=type_make(inner.base,inner.ptr_depth+1);return true;
    }
    if(strncmp(*pp,"int",3)==0&&!is_icc((*pp)[3])){(*pp)+=3;*out=type_int();return true;}
    if(strncmp(*pp,"bool",4)==0&&!is_icc((*pp)[4])){(*pp)+=4;*out=type_bool();return true;}
    if(strncmp(*pp,"str",3)==0&&!is_icc((*pp)[3])){(*pp)+=3;*out=type_string();return true;}
    if(strncmp(*pp,"string",6)==0&&!is_icc((*pp)[6])){(*pp)+=6;*out=type_string();return true;}
    if(strncmp(*pp,"void",4)==0&&!is_icc((*pp)[4])){(*pp)+=4;*out=type_void();return true;}
    ERR("unknown type");return false;
}

typedef enum{EK_INT,EK_BOOL,EK_STRING,EK_VAR,EK_BINOP,EK_CALL,EK_PTR_NULL,EK_INT_CAST,EK_DEREF,EK_ADDROF,EK_ALLOC,EK_FREE,EK_PTR_CAST,EK_IN,EK_RANGE}EK;
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
typedef struct{FnDef*fns;size_t nfns,cap;}Program;

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
static Expr*parse_factor(const char**pp){
    skip(pp);const char*p=*pp;
    if(*p=='*'&&p[1]!='='){(*pp)++;Expr*inner=parse_factor(pp);if(!inner)return NULL;Expr*e=calloc(1,sizeof(Expr));if(!e){free_expr(inner);return NULL;}e->kind=EK_DEREF;e->left=inner;return e;}
    if(*p=='&'){(*pp)++;Expr*inner=parse_factor(pp);if(!inner)return NULL;Expr*e=calloc(1,sizeof(Expr));if(!e){free_expr(inner);return NULL;}e->kind=EK_ADDROF;e->left=inner;return e;}
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
static Expr*parse_product(const char**pp){
    Expr*l=parse_factor(pp);if(!l)return NULL;
    while(1){skip(pp);const char*save=*pp;char op=*save;
        if(op=='*'){const char*next=save+1;while(*next&&isspace((unsigned char)*next))next++;
            if(*next&&(*next=='('||*next=='"'||isdigit((unsigned char)*next)||is_ic(*next))){(*pp)++;Expr*r=parse_factor(pp);if(!r){free_expr(l);return NULL;}Expr*e=calloc(1,sizeof(Expr));if(!e){free_expr(l);free_expr(r);return NULL;}e->kind=EK_BINOP;e->op[0]=op;e->left=l;e->right=r;l=e;continue;}break;}
        if(op!='/')break;(*pp)++;Expr*r=parse_factor(pp);if(!r){free_expr(l);return NULL;}Expr*e=calloc(1,sizeof(Expr));if(!e){free_expr(l);free_expr(r);return NULL;}e->kind=EK_BINOP;e->op[0]=op;e->left=l;e->right=r;l=e;}
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
static Expr*parse_expr_s(const char*s){const char*p=s;Expr*e=parse_expr(&p);if(!e)return NULL;skip(&p);if(*p){free_expr(e);return NULL;}return e;}

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
        Expr*range_expr=parse_expr_s(fstr);free(fstr);if(!range_expr){ERR("invalid range");goto done;}
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
    prog->fns=NULL;prog->nfns=0;prog->cap=0;size_t i=0;
    while(i<lines->len){
        const char*r=lines->items[i];char*cp=xdup(r);rtrim(cp);if(blank(cp)){free(cp);i++;continue;}free(cp);
        if(r[0]==' '||r[0]=='\t'){fprintf(stderr,"Error line %zu: unexpected indent\n",i+1);return false;}
        char*rl=xdup(r);rtrim(rl);strip_comment(rl);char*fn_name;char**params;Type*ptypes;size_t pc;
        if(!parse_fn_hdr(rl,&fn_name,&params,&ptypes,&pc)){fprintf(stderr,"Error line %zu: %s\n",i+1,g_err);free(rl);return false;}free(rl);
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
    case EK_PTR_CAST:{if(e->argc!=1)return type_unknown();if(!type_is_ptr(e->cast_type)){TERR("TypeError: cast target must be pointer type");return type_unknown();}Type at=type_check_expr(e->args[0],sc,prog,in_unsafe);if(type_is_unknown(at))return type_unknown();if(!(type_eq(at,type_int())||type_is_ptr(at))){TERR("TypeError: cannot cast %s to %s",type_name(at),type_name(e->cast_type));return type_unknown();}e->type=e->cast_type;return e->type;}
    case EK_ALLOC:{if(e->argc!=1)return type_unknown();Type at=type_check_expr(e->args[0],sc,prog,in_unsafe);if(type_is_unknown(at))return type_unknown();if(!type_eq(at,type_int())){TERR("TypeError: alloc() requires int, got %s",type_name(at));return type_unknown();}e->type=type_null_ptr();return e->type;}
    case EK_FREE:{if(e->argc!=1)return type_unknown();Type at=type_check_expr(e->args[0],sc,prog,in_unsafe);if(type_is_unknown(at))return type_unknown();if(!type_is_ptr(at)){TERR("TypeError: free() requires pointer, got %s",type_name(at));return type_unknown();}e->type=type_void();return e->type;}
    case EK_IN:{if(e->argc!=1)return type_unknown();if(!in_unsafe){TERR("UnsafeError: in() requires unsafe block");return type_unknown();}Type at=type_check_expr(e->args[0],sc,prog,in_unsafe);if(type_is_unknown(at))return type_unknown();if(!type_eq(at,type_int())){TERR("TypeError: in() port must be int, got %s",type_name(at));return type_unknown();}e->type=type_int();return e->type;}
    case EK_DEREF:{Type it=type_check_expr(e->left,sc,prog,in_unsafe);if(type_is_unknown(it))return type_unknown();if(!in_unsafe){TERR("UnsafeError: dereference requires unsafe block");return type_unknown();}if(!type_is_ptr(it)){TERR("TypeError: cannot dereference %s",type_name(it));return type_unknown();}if(it.base==BASE_VOID&&it.ptr_depth==1){TERR("TypeError: cannot dereference ptr<void>");return type_unknown();}e->type=type_deref(it);return e->type;}
    case EK_ADDROF:{Type it=type_check_expr(e->left,sc,prog,in_unsafe);if(type_is_unknown(it))return type_unknown();if(e->left->kind!=EK_VAR){TERR("TypeError: address-of requires variable");return type_unknown();}e->type=type_addr_of(it);return e->type;}
    case EK_CALL:{FnDef*fn=find_fn(prog,e->fn);if(!fn){TERR("TypeError: undefined function '%s'",e->fn);return type_unknown();}if(fn->nparams!=e->argc){TERR("TypeError: function '%s' expects %zu args, got %zu",e->fn,fn->nparams,e->argc);return type_unknown();}for(size_t i=0;i<e->argc;i++){Type at=type_check_expr(e->args[i],sc,prog,in_unsafe);if(type_is_unknown(at))return type_unknown();if(!type_assignable(fn->param_types[i],at)){TERR("TypeError: arg %zu of '%s' expects %s, got %s",i+1,e->fn,type_name(fn->param_types[i]),type_name(at));return type_unknown();}}e->type=fn->return_type;return e->type;}
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
    case SK_DECL:{if(scope_find(sc,s->var)){TERR("TypeError: variable '%s' already declared",s->var);return false;}return scope_add(sc,s->var,s->var_type,0);} 
    case SK_ASSIGN:{if(!s->var){Type tt=type_check_expr(s->cond,sc,prog,in_unsafe);if(type_is_unknown(tt))return false;if(s->cond->kind!=EK_DEREF){TERR("TypeError: left side must be pointer dereference");return false;}Type vt=type_check_expr(s->expr,sc,prog,in_unsafe);if(type_is_unknown(vt))return false;if(!type_assignable(tt,vt)){TERR("TypeError: cannot assign %s to %s",type_name(vt),type_name(tt));return false;}return true;}
        Symbol*sym=scope_find(sc,s->var);if(!sym){Type t=type_check_expr(s->expr,sc,prog,in_unsafe);if(type_is_unknown(t))return false;Type final_t=t;if(!type_is_unknown(s->var_type)){if(!type_assignable(s->var_type,t)){TERR("TypeError: cannot assign %s to %s",type_name(t),type_name(s->var_type));return false;}final_t=s->var_type;}if(!scope_add(sc,s->var,final_t,0))return false;s->var_type=final_t;}else{Type t=type_check_expr(s->expr,sc,prog,in_unsafe);if(type_is_unknown(t))return false;if(!type_assignable(sym->type,t)){TERR("TypeError: cannot assign %s to %s",type_name(t),type_name(sym->type));return false;}s->var_type=sym->type;}return true;}
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

typedef struct{SBuf out;size_t lbl;}CG;
static void cg_init(CG*g){sb_init(&g->out);g->lbl=0;}
static void cg_free(CG*g){sb_free(&g->out);}
static size_t L(CG*g){return g->lbl++;}
static void E(CG*g,const char*fmt,...){char tmp[1024];va_list ap;va_start(ap,fmt);vsnprintf(tmp,sizeof(tmp),fmt,ap);va_end(ap);sb_cat(&g->out,tmp);}
static const char*cg_type(Type t){
    static char bufs[8][96];static int bi=0;char base[64];
    if(t.ptr_depth==0){
        if(t.base==BASE_INT)return"long long";
        if(t.base==BASE_BOOL)return"int";
        if(t.base==BASE_STRING)return"const char*";
        if(t.base==BASE_VOID)return"void";
        return"long long";
    }
    if(t.base==BASE_INT)snprintf(base,sizeof(base),"long long");
    else if(t.base==BASE_BOOL)snprintf(base,sizeof(base),"int");
    else if(t.base==BASE_STRING)snprintf(base,sizeof(base),"const char*");
    else snprintf(base,sizeof(base),"void");
    bi=(bi+1)%8;snprintf(bufs[bi],sizeof(bufs[bi]),"%s",base);
    for(unsigned i=0;i<t.ptr_depth;i++)strncat(bufs[bi],"*",sizeof(bufs[bi])-strlen(bufs[bi])-1);
    return bufs[bi];
}
static char*escape_for_c(const char*s){
    size_t len=strlen(s);char*result=malloc(len*4+1);if(!result)return NULL;size_t j=0;
    for(size_t i=0;i<len;i++){unsigned char c=(unsigned char)s[i];
        switch(c){case'\n':result[j++]='\\';result[j++]='n';break;case'\t':result[j++]='\\';result[j++]='t';break;case'\r':result[j++]='\\';result[j++]='r';break;case'\\':result[j++]='\\';result[j++]='\\';break;case'"':result[j++]='\\';result[j++]='"';break;default:if(c>=32&&c<127)result[j++]=c;else j+=sprintf(result+j,"\\x%02x",c);break;}}
    result[j]='\0';return result;
}
static bool cg_expr_val(CG*g,Expr*e,SBuf*result,bool no_runtime);
static bool cg_stmts(CG*g,Stmt**s,size_t n,bool inlp,bool no_runtime);
static bool cg_expr_val(CG*g,Expr*e,SBuf*result,bool no_runtime){
    switch(e->kind){
    case EK_INT:sb_fmt(result,"%lld",e->ival);return true;
    case EK_BOOL:sb_cat(result,e->ival?"1":"0");return true;
    case EK_STRING:{char*esc=escape_for_c(e->sval);if(!esc)return false;sb_fmt(result,"\"%s\"",esc);free(esc);return true;}
    case EK_PTR_NULL:sb_cat(result,"((void*)0)");return true;
    case EK_VAR:sb_cat(result,e->name);return true;
    case EK_INT_CAST:{SBuf a;sb_init(&a);if(!cg_expr_val(g,e->args[0],&a,no_runtime)){sb_free(&a);return false;}sb_fmt(result,"((long long)(%s))",a.d);sb_free(&a);return true;}
    case EK_IN:{SBuf a;sb_init(&a);if(!cg_expr_val(g,e->args[0],&a,no_runtime)){sb_free(&a);return false;}sb_fmt(result,"((long long)aut_in8((unsigned short)(%s)))",a.d);sb_free(&a);return true;}
    case EK_PTR_CAST:{SBuf a;sb_init(&a);if(!cg_expr_val(g,e->args[0],&a,no_runtime)){sb_free(&a);return false;}sb_fmt(result,"((%s)(unsigned long long)(%s))",cg_type(e->cast_type),a.d);sb_free(&a);return true;}
    case EK_ALLOC:{SBuf a;sb_init(&a);if(!cg_expr_val(g,e->args[0],&a,no_runtime)){sb_free(&a);return false;}if(no_runtime)sb_fmt(result,"aut_alloc((unsigned long long)(%s))",a.d);else sb_fmt(result,"malloc((size_t)(%s))",a.d);sb_free(&a);return true;}
    case EK_FREE:{SBuf a;sb_init(&a);if(!cg_expr_val(g,e->args[0],&a,no_runtime)){sb_free(&a);return false;}if(no_runtime)sb_fmt(result,"(aut_free(%s),(void*)0)",a.d);else sb_fmt(result,"(free(%s),(void*)0)",a.d);sb_free(&a);return true;}
    case EK_DEREF:{SBuf a;sb_init(&a);if(!cg_expr_val(g,e->left,&a,no_runtime)){sb_free(&a);return false;}sb_fmt(result,"(*((%s*)(%s)))",cg_type(e->type),a.d);sb_free(&a);return true;}
    case EK_ADDROF:{if(e->left->kind==EK_VAR){sb_fmt(result,"((%s)(&%s))",cg_type(e->type),e->left->name);}else return false;return true;}
    case EK_CALL:{SBuf*args=malloc(e->argc*sizeof(SBuf));for(size_t i=0;i<e->argc;i++){sb_init(&args[i]);if(!cg_expr_val(g,e->args[i],&args[i],no_runtime)){for(size_t j=0;j<=i;j++)sb_free(&args[j]);free(args);return false;}}sb_fmt(result,"_fn_%s(",e->fn);for(size_t i=0;i<e->argc;i++){if(i)sb_cat(result,", ");sb_cat(result,args[i].d);sb_free(&args[i]);}sb_cat(result,")");free(args);return true;}
    case EK_BINOP:{
        const char*op=e->op;
        if(strcmp(op,"+")==0&&type_eq(e->type,type_string())){
            if(e->left->kind!=EK_STRING||e->right->kind!=EK_STRING){ERR("str + str is supported only for string literals");return false;}
            size_t ln=strlen(e->left->sval),rn=strlen(e->right->sval);char*joined=malloc(ln+rn+1);if(!joined)return false;
            memcpy(joined,e->left->sval,ln);memcpy(joined+ln,e->right->sval,rn+1);
            char*esc=escape_for_c(joined);free(joined);if(!esc)return false;sb_fmt(result,"\"%s\"",esc);free(esc);return true;
        }
        SBuf l,r;sb_init(&l);sb_init(&r);
        if(!cg_expr_val(g,e->left,&l,no_runtime)||!cg_expr_val(g,e->right,&r,no_runtime)){sb_free(&l);sb_free(&r);return false;}
        if(strcmp(op,"==")==0||strcmp(op,"!=")==0||strcmp(op,"<")==0||strcmp(op,">")==0||strcmp(op,"<=")==0||strcmp(op,">=")==0)sb_fmt(result,"(%s %s %s ? 1 : 0)",l.d,op,r.d);
        else sb_fmt(result,"(%s %s %s)",l.d,op,r.d);
        sb_free(&l);sb_free(&r);return true;
    }
    case EK_RANGE:ERR("range outside for");return false;
    default:return false;
    }
}
static bool cg_stmts(CG*g,Stmt**stmts,size_t count,bool inlp,bool no_runtime){
    for(size_t i=0;i<count;i++){Stmt*s=stmts[i];
        switch(s->kind){
        case SK_PRINT:{SBuf e;sb_init(&e);if(!cg_expr_val(g,s->expr,&e,no_runtime)){sb_free(&e);return false;}if(no_runtime){if(type_eq(s->expr->type,type_bool()))E(g,"    aut_print_str(%s?\"true\":\"false\");\n",e.d);else if(type_eq(s->expr->type,type_string()))E(g,"    aut_print_str(%s);\n",e.d);else E(g,"    aut_print_i64((long long)(%s));\n",e.d);}else{if(type_eq(s->expr->type,type_bool()))E(g,"    printf(\"%%s\\n\",%s?\"true\":\"false\");\n",e.d);else if(type_eq(s->expr->type,type_string()))E(g,"    printf(\"%%s\\n\",%s);\n",e.d);else E(g,"    printf(\"%%lld\\n\",(long long)(%s));\n",e.d);}sb_free(&e);break;}
        case SK_DECL:break;
        case SK_ASSIGN:{SBuf e;sb_init(&e);if(!cg_expr_val(g,s->expr,&e,no_runtime)){sb_free(&e);return false;}if(!s->var){SBuf inner;sb_init(&inner);if(s->cond&&s->cond->kind==EK_DEREF&&s->cond->left){if(!cg_expr_val(g,s->cond->left,&inner,no_runtime)){sb_free(&inner);sb_free(&e);return false;}E(g,"    (*((%s*)(%s)) = %s);\n",cg_type(s->cond->type),inner.d,e.d);}sb_free(&inner);}else E(g,"    %s = %s;\n",s->var,e.d);sb_free(&e);break;}
        case SK_IF:{SBuf c;sb_init(&c);if(!cg_expr_val(g,s->cond,&c,no_runtime)){sb_free(&c);return false;}E(g,"    if(%s) {\n",c.d);sb_free(&c);if(!cg_stmts(g,s->then,s->nthen,inlp,no_runtime))return false;if(s->nels>0){E(g,"    } else {\n");if(!cg_stmts(g,s->els,s->nels,inlp,no_runtime))return false;}E(g,"    }\n");break;}
        case SK_WHILE:{E(g,"    while(1) {\n");SBuf c;sb_init(&c);if(!cg_expr_val(g,s->cond,&c,no_runtime)){sb_free(&c);return false;}E(g,"        if(!(%s)) break;\n",c.d);sb_free(&c);if(!cg_stmts(g,s->loop,s->nloop,true,no_runtime))return false;E(g,"    }\n");break;}
        case SK_UNSAFE:{if(!cg_stmts(g,s->loop,s->nloop,inlp,no_runtime))return false;break;}
        case SK_ASM:{char*esc=escape_for_c(s->asm_text?s->asm_text:"");if(!esc)return false;E(g,"    __asm__ __volatile__(\"%s\");\n",esc);free(esc);break;}
        case SK_OUT:{SBuf p,v;sb_init(&p);sb_init(&v);if(!cg_expr_val(g,s->cond,&p,no_runtime)||!cg_expr_val(g,s->expr,&v,no_runtime)){sb_free(&p);sb_free(&v);return false;}E(g,"    aut_out8((unsigned short)(%s), (unsigned char)(%s));\n",p.d,v.d);sb_free(&p);sb_free(&v);break;}
        case SK_FOR:{size_t lf=L(g);Expr*r=s->range_expr;SBuf st,ed,sp;sb_init(&st);sb_init(&ed);sb_init(&sp);if(!cg_expr_val(g,r->range_start,&st,no_runtime)||!cg_expr_val(g,r->range_end,&ed,no_runtime)){sb_free(&st);sb_free(&ed);sb_free(&sp);return false;}if(r->range_step&&!cg_expr_val(g,r->range_step,&sp,no_runtime)){sb_free(&st);sb_free(&ed);sb_free(&sp);return false;}E(g,"    {\n");E(g,"        long long _s%zu = %s, _e%zu = %s;\n",lf,st.d,lf,ed.d);if(r->range_step)E(g,"        long long _p%zu = %s;\n",lf,sp.d);else E(g,"        long long _p%zu = (_s%zu <= _e%zu) ? 1 : -1;\n",lf,lf,lf);sb_free(&st);sb_free(&ed);sb_free(&sp);E(g,"        for(%s = _s%zu; ",s->loop_var,lf);if(r->range_inclusive)E(g,"(_p%zu > 0) ? (%s <= _e%zu) : (%s >= _e%zu); ",lf,s->loop_var,lf,s->loop_var,lf);else E(g,"(_p%zu > 0) ? (%s < _e%zu) : (%s > _e%zu); ",lf,s->loop_var,lf,s->loop_var,lf);E(g,"%s += _p%zu) {\n",s->loop_var,lf);if(!cg_stmts(g,s->loop,s->nloop,true,no_runtime))return false;E(g,"        }\n");E(g,"    }\n");break;}
        case SK_RETURN:{if(s->expr){SBuf e;sb_init(&e);if(!cg_expr_val(g,s->expr,&e,no_runtime)){sb_free(&e);return false;}E(g,"    return %s;\n",e.d);sb_free(&e);}else E(g,"    return 0;\n");break;}
        case SK_BREAK:E(g,"    break;\n");break;
        case SK_CONTINUE:E(g,"    continue;\n");break;
        case SK_CALL:{SBuf e;sb_init(&e);if(!cg_expr_val(g,s->expr,&e,no_runtime)){sb_free(&e);return false;}E(g,"    (void)%s;\n",e.d);sb_free(&e);break;}
        default:break;
        }
    }
    return true;
}
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
static bool cg_fn(CG*g,FnDef*fn,const Program*prog,bool no_runtime){
    (void)prog;E(g,"static %s _fn_%s(",cg_type(fn->return_type),fn->name);
    for(size_t i=0;i<fn->nparams;i++){if(i)E(g,", ");E(g,"%s %s",cg_type(fn->param_types[i]),fn->params[i]);}
    E(g,") {\n");VarInfo*vars=NULL;size_t vcount=0,vcap=0;
    if(!collect_vars(fn->body,fn->nbody,&vars,&vcount,&vcap,fn)){free(vars);return false;}
    for(size_t i=0;i<vcount;i++){E(g,"    %s%s %s = 0;\n",vars[i].is_volatile?"volatile ":"",cg_type(vars[i].type),vars[i].name);free(vars[i].name);}
    free(vars);if(!cg_stmts(g,fn->body,fn->nbody,false,no_runtime))return false;E(g,"    return 0;\n");E(g,"}\n\n");return true;
}
static bool codegen(const Program*prog,const char*out_path,bool no_runtime){
    CG g;cg_init(&g);E(&g,"/* AutismLang v%s */\n",AUTISMLANG_VERSION);
    if(no_runtime){
        E(&g,"typedef unsigned long long aut_u64;\n");
        E(&g,"extern void aut_print_i64(long long value);\n");
        E(&g,"extern void aut_print_str(const char* value);\n");
        E(&g,"extern void* aut_alloc(aut_u64 size);\n");
        E(&g,"extern void aut_free(void* ptr);\n\n");
    }else{
        E(&g,"#include <stdio.h>\n#include <stdlib.h>\n\n");
    }
    E(&g,"static inline unsigned char aut_in8(unsigned short port) { unsigned char value; __asm__ __volatile__(\"inb %1, %0\" : \"=a\"(value) : \"Nd\"(port)); return value; }\n");
    E(&g,"static inline void aut_out8(unsigned short port, unsigned char value) { __asm__ __volatile__(\"outb %0, %1\" :: \"a\"(value), \"Nd\"(port)); }\n\n");
    for(size_t i=0;i<prog->nfns;i++){E(&g,"static %s _fn_%s(",cg_type(prog->fns[i].return_type),prog->fns[i].name);for(size_t j=0;j<prog->fns[i].nparams;j++){if(j)E(&g,", ");E(&g,"%s",cg_type(prog->fns[i].param_types[j]));}E(&g,");\n");}E(&g,"\n");
    for(size_t i=0;i<prog->nfns;i++){if(!cg_fn(&g,(FnDef*)&prog->fns[i],prog,no_runtime)){cg_free(&g);return false;}}
    if(no_runtime)E(&g,"void aut_entry(void) { (void)_fn_main(); }\n");
    else E(&g,"int main(void) { _fn_main(); return 0; }\n");
    char*pc=xdup(out_path);for(size_t i=0;pc[i];i++){if(pc[i]=='/'||pc[i]=='\\'){char sv=pc[i];pc[i]=0;if(pc[0])MKDIR(pc);pc[i]=sv;}}free(pc);
    FILE*fp=fopen(out_path,"wb");if(!fp){cg_free(&g);return false;}fwrite(g.out.d,1,g.out.len,fp);fclose(fp);cg_free(&g);return true;
}
int main(int argc,char**argv){
    const char*in=NULL,*out="build/out.c";
    bool no_runtime=false;
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"--help")==0){printf("Usage: %s <input.aut> [-o output.c] [--no-runtime]\n",argv[0]);return 0;}
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