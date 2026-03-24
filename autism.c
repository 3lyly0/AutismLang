/*
 * AutismLang Compiler v0.3.0 - Static typed systems language
 * Build output: gcc out.c -o out.exe
 * 
 * New in v0.3.0:
 * - Pointer operations: alloc(), free(), * (dereference), & (address-of)
 * - Pointer type annotation: ptr x = alloc(8)
 * - Pointer casting: ptr(int), int(ptr)
 * - Hexadecimal integer literals: 0xB8000
 * - Null pointer: null, NULL
 */
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

#define AUTISMLANG_VERSION "0.3.0"
#define AUTISMLANG_BACKEND "c-static"

/* ---- helpers ---- */
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

/* ---- file/lines ---- */
static char*read_file(const char*path){
    FILE*f=fopen(path,"rb");if(!f){fprintf(stderr,"Cannot open '%s'\n",path);return NULL;}
    fseek(f,0,SEEK_END);long sz=ftell(f);fseek(f,0,SEEK_SET);
    char*buf=malloc((size_t)sz+1);if(!buf){fclose(f);return NULL;}
    if(fread(buf,1,(size_t)sz,f)!=(size_t)sz){fclose(f);free(buf);fprintf(stderr,"Failed to read '%s'\n",path);return NULL;}
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

/* ======================================================
 * TYPE SYSTEM
 * ====================================================== */
typedef enum {
    TYPE_UNKNOWN = 0,
    TYPE_INT,       /* 64-bit integer */
    TYPE_PTR,       /* raw pointer */
    TYPE_BOOL,      /* boolean */
    TYPE_STRING,    /* string literal */
    TYPE_VOID       /* void (for functions) */
} TypeKind;

static const char* type_name(TypeKind t) {
    switch(t) {
        case TYPE_INT: return "int";
        case TYPE_PTR: return "ptr";
        case TYPE_BOOL: return "bool";
        case TYPE_STRING: return "string";
        case TYPE_VOID: return "void";
        default: return "unknown";
    }
}

/* ======================================================
 * AST WITH TYPE INFORMATION
 * ====================================================== */
 typedef enum{EK_INT,EK_BOOL,EK_STRING,EK_VAR,EK_BINOP,EK_CALL,EK_PTR_NULL,EK_INT_CAST,EK_DEREF,EK_ADDROF,EK_ALLOC,EK_FREE,EK_PTR_CAST}EK;
typedef struct Expr{EK kind;TypeKind type;long long ival;char*sval;char*name;char*fn;char op[3];struct Expr*left,*right,**args;size_t argc;}Expr;
typedef enum{SK_PRINT,SK_ASSIGN,SK_IF,SK_WHILE,SK_FOR,SK_RETURN,SK_BREAK,SK_CONTINUE,SK_CALL,SK_VAR_DECL}SK;
typedef struct Stmt{SK kind;char*var;TypeKind var_type;Expr*expr,*cond;struct Stmt**then;size_t nthen;struct Stmt**els;size_t nels;struct Stmt**loop;size_t nloop;
    char*loop_var;
    Expr*range_start,*range_end,*range_step;
}Stmt;
typedef struct{char*name;char**params;TypeKind*param_types;TypeKind return_type;Stmt**body;size_t nbody;size_t nparams;}FnDef;
typedef struct{FnDef*fns;size_t nfns,cap;}Program;

static void free_expr(Expr*e){if(!e)return;free(e->sval);free(e->name);free(e->fn);free_expr(e->left);free_expr(e->right);for(size_t i=0;i<e->argc;i++)free_expr(e->args[i]);free(e->args);free(e);}
static void free_stmt(Stmt*s){if(!s)return;free(s->var);free_expr(s->expr);free_expr(s->cond);for(size_t i=0;i<s->nthen;i++)free_stmt(s->then[i]);free(s->then);for(size_t i=0;i<s->nels;i++)free_stmt(s->els[i]);free(s->els);for(size_t i=0;i<s->nloop;i++)free_stmt(s->loop[i]);free(s->loop);free(s->loop_var);free_expr(s->range_start);free_expr(s->range_end);free_expr(s->range_step);free(s);}
static void free_fn(FnDef*f){if(!f)return;free(f->name);for(size_t i=0;i<f->nparams;i++)free(f->params[i]);free(f->params);free(f->param_types);for(size_t i=0;i<f->nbody;i++)free_stmt(f->body[i]);free(f->body);}
static void free_program(Program*p){for(size_t i=0;i<p->nfns;i++)free_fn(&p->fns[i]);free(p->fns);}

/* ======================================================
 * SYMBOL TABLE FOR TYPE CHECKING
 * ====================================================== */
typedef struct Symbol {
    char* name;
    TypeKind type;
    int is_param;
} Symbol;

typedef struct Scope {
    Symbol* syms;
    size_t count, cap;
    struct Scope* parent;
} Scope;

static void scope_init(Scope* sc, Scope* parent) {
    sc->syms = NULL;
    sc->count = 0;
    sc->cap = 0;
    sc->parent = parent;
}

static void scope_free(Scope* sc) {
    for(size_t i = 0; i < sc->count; i++) free(sc->syms[i].name);
    free(sc->syms);
    sc->syms = NULL;
    sc->count = 0;
    sc->cap = 0;
}

static Symbol* scope_find(Scope* sc, const char* name) {
    for(size_t i = 0; i < sc->count; i++) {
        if(strcmp(sc->syms[i].name, name) == 0) return &sc->syms[i];
    }
    if(sc->parent) return scope_find(sc->parent, name);
    return NULL;
}

static bool scope_add(Scope* sc, const char* name, TypeKind type, int is_param) {
    for(size_t i = 0; i < sc->count; i++) {
        if(strcmp(sc->syms[i].name, name) == 0) {
            ERR("variable '%s' already declared", name);
            return false;
        }
    }
    if(sc->count == sc->cap) {
        size_t nc = sc->cap ? sc->cap * 2 : 8;
        Symbol* np = realloc(sc->syms, nc * sizeof(Symbol));
        if(!np) return false;
        sc->syms = np;
        sc->cap = nc;
    }
    sc->syms[sc->count].name = xdup(name);
    sc->syms[sc->count].type = type;
    sc->syms[sc->count].is_param = is_param;
    sc->count++;
    return true;
}

/* ======================================================
 * PARSER
 * ====================================================== */
static Expr*parse_expr(const char**pp);
static void skip(const char**pp){while(**pp&&isspace((unsigned char)**pp))(*pp)++;}

/* Escape sequence processing for string literals */
static char* process_string_escape(const char* src, size_t len) {
    char* result = malloc(len + 1);
    if (!result) return NULL;
    
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '\\' && i + 1 < len) {
            switch (src[i + 1]) {
                case 'n': result[j++] = '\n'; i++; break;
                case 't': result[j++] = '\t'; i++; break;
                case 'r': result[j++] = '\r'; i++; break;
                case '\\': result[j++] = '\\'; i++; break;
                case '"': result[j++] = '"'; i++; break;
                case '0': result[j++] = '\0'; i++; break;
                default: result[j++] = src[i]; break;
            }
        } else {
            result[j++] = src[i];
        }
    }
    result[j] = '\0';
    return result;
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
static Expr*parse_factor(const char**pp){
    skip(pp);const char*p=*pp;
    /* Dereference: *expr */
    if(*p=='*'&&p[1]!='='){
        (*pp)++;
        Expr*inner=parse_factor(pp);
        if(!inner)return NULL;
        Expr*e=calloc(1,sizeof(Expr));
        if(!e){free_expr(inner);return NULL;}
        e->kind=EK_DEREF;
        e->left=inner;
        return e;
    }
    /* Address-of: &var */
    if(*p=='&'){
        (*pp)++;
        Expr*inner=parse_factor(pp);
        if(!inner)return NULL;
        Expr*e=calloc(1,sizeof(Expr));
        if(!e){free_expr(inner);return NULL;}
        e->kind=EK_ADDROF;
        e->left=inner;
        return e;
    }
    if(*p=='('){(*pp)++;Expr*e=parse_expr(pp);if(!e)return NULL;skip(pp);if(**pp!=')'){free_expr(e);ERR("missing ')'");return NULL;}(*pp)++;return e;}
    if(*p=='"'){
        /* Parse string literal */
        (*pp)++; /* skip opening quote */
        const char* start = *pp;
        size_t len = 0;
        while(**pp && **pp != '"'){
            if(**pp == '\\' && (*pp)[1]) (*pp)++; /* skip escape char */
            (*pp)++;
            len++;
        }
        if(**pp != '"'){ERR("unterminated string literal");return NULL;}
        char* raw_str = xndup(start, (size_t)(*pp - start));
        (*pp)++; /* skip closing quote */
        char* processed = process_string_escape(raw_str, strlen(raw_str));
        free(raw_str);
        if(!processed){ERR("failed to process string");return NULL;}
        Expr*e=calloc(1,sizeof(Expr));
        if(!e){free(processed);return NULL;}
        e->kind=EK_STRING;
        e->type=TYPE_STRING;
        e->sval=processed;
        return e;
    }
    bool neg=false;if(*p=='-'&&isdigit((unsigned char)p[1])){neg=true;(*pp)++;p++;}
    if(isdigit((unsigned char)*p)){
        char*end;int base=10;
        /* Check for hex prefix */
        if(*p=='0'&&(p[1]=='x'||p[1]=='X')){base=16;}
        long long v=strtoll(*pp,&end,base);*pp=end;
        Expr*e=calloc(1,sizeof(Expr));if(!e)return NULL;e->kind=EK_INT;e->type=TYPE_INT;e->ival=neg?-v:v;return e;
    }(void)neg;
    if(is_ic(*p)){
        char id[128];size_t n=0;while(is_icc(**pp)){if(n+1>=sizeof(id)){ERR("ident too long");return NULL;}id[n++]=**pp;(*pp)++;}id[n]=0;skip(pp);
        if(**pp=='('){
            if(strcmp(id,"int")==0){
                Expr*e=calloc(1,sizeof(Expr));if(!e)return NULL;e->kind=EK_INT_CAST;
                if(!parse_args(pp,&e->args,&e->argc)){free(e);return NULL;}
                if(e->argc!=1){ERR("int() takes exactly 1 arg");free_expr(e);return NULL;}
                return e;
            }
            if(strcmp(id,"ptr")==0){
                Expr*e=calloc(1,sizeof(Expr));if(!e)return NULL;e->kind=EK_PTR_CAST;
                if(!parse_args(pp,&e->args,&e->argc)){free(e);return NULL;}
                if(e->argc!=1){ERR("ptr() takes exactly 1 arg");free_expr(e);return NULL;}
                return e;
            }
            if(strcmp(id,"alloc")==0){
                Expr*e=calloc(1,sizeof(Expr));if(!e)return NULL;e->kind=EK_ALLOC;
                if(!parse_args(pp,&e->args,&e->argc)){free(e);return NULL;}
                if(e->argc!=1){ERR("alloc() takes exactly 1 arg (size)");free_expr(e);return NULL;}
                return e;
            }
            if(strcmp(id,"free")==0){
                Expr*e=calloc(1,sizeof(Expr));if(!e)return NULL;e->kind=EK_FREE;
                if(!parse_args(pp,&e->args,&e->argc)){free(e);return NULL;}
                if(e->argc!=1){ERR("free() takes exactly 1 arg (pointer)");free_expr(e);return NULL;}
                return e;
            }
            if(strcmp(id,"range")==0){
                ERR("range() can only be used in for-in loop");
                return NULL;
            }
            Expr*e=calloc(1,sizeof(Expr));if(!e)return NULL;e->kind=EK_CALL;e->fn=xdup(id);if(!parse_args(pp,&e->args,&e->argc)){free_expr(e);return NULL;}return e;
        }
        if(strcmp(id,"True")==0||strcmp(id,"true")==0){Expr*e=calloc(1,sizeof(Expr));if(!e)return NULL;e->kind=EK_BOOL;e->type=TYPE_BOOL;e->ival=1;return e;}
        if(strcmp(id,"False")==0||strcmp(id,"false")==0){Expr*e=calloc(1,sizeof(Expr));if(!e)return NULL;e->kind=EK_BOOL;e->type=TYPE_BOOL;e->ival=0;return e;}
        if(strcmp(id,"null")==0||strcmp(id,"NULL")==0){Expr*e=calloc(1,sizeof(Expr));if(!e)return NULL;e->kind=EK_PTR_NULL;e->type=TYPE_PTR;return e;}
        Expr*e=calloc(1,sizeof(Expr));if(!e)return NULL;e->kind=EK_VAR;e->name=xdup(id);return e;
    }
    ERR("unexpected '%c'",(int)*p);return NULL;
}
static Expr*parse_product(const char**pp){Expr*l=parse_factor(pp);if(!l)return NULL;while(1){skip(pp);const char* save = *pp;
        /* Check if * is dereference (prefix) or multiplication (binary) */
        char op = *save;
        if(op == '*'){
            /* Look ahead to see if this is a binary operator */
            const char* next = save + 1;
            while(*next && isspace((unsigned char)*next)) next++;
            /* If followed by an expression factor, it's multiplication */
            if(*next && (*next == '(' || *next == '"' || isdigit((unsigned char)*next) || is_ic(*next))) {
                /* It's multiplication */
                (*pp)++;
                Expr*r=parse_factor(pp);
                if(!r){free_expr(l);return NULL;}
                Expr*e=calloc(1,sizeof(Expr));
                if(!e){free_expr(l);free_expr(r);return NULL;}
                e->kind=EK_BINOP;
                e->op[0]=op;
                e->left=l;e->right=r;
                l=e;
                continue;
            }
            /* It's dereference - already handled in parse_factor */
            break;
        }
        if(op!='/')break;
        (*pp)++;
        Expr*r=parse_factor(pp);
        if(!r){free_expr(l);return NULL;}
        Expr*e=calloc(1,sizeof(Expr));
        if(!e){free_expr(l);free_expr(r);return NULL;}
        e->kind=EK_BINOP;
        e->op[0]=op;
        e->left=l;e->right=r;l=e;
    }
    return l;
}
static Expr*parse_sum(const char**pp){Expr*l=parse_product(pp);if(!l)return NULL;while(1){skip(pp);char op=**pp;if(op!='+'&&op!='-')break;(*pp)++;Expr*r=parse_product(pp);if(!r){free_expr(l);return NULL;}Expr*e=calloc(1,sizeof(Expr));if(!e){free_expr(l);free_expr(r);return NULL;}e->kind=EK_BINOP;e->op[0]=op;e->left=l;e->right=r;l=e;}return l;}
static Expr*parse_expr(const char**pp){
    Expr*l=parse_sum(pp);if(!l)return NULL;skip(pp);const char*p=*pp;char op[3]={0};
    if((p[0]=='='&&p[1]=='=')||(p[0]=='!'&&p[1]=='=')||(p[0]=='<'&&p[1]=='=')||(p[0]=='>'&&p[1]=='=')){op[0]=p[0];op[1]=p[1];(*pp)+=2;}
    else if(*p=='<'||*p=='>'){op[0]=*p;(*pp)++;}
    if(op[0]){Expr*r=parse_sum(pp);if(!r){free_expr(l);return NULL;}Expr*e=calloc(1,sizeof(Expr));if(!e){free_expr(l);free_expr(r);return NULL;}e->kind=EK_BINOP;memcpy(e->op,op,3);e->left=l;e->right=r;return e;}
    return l;
}
static Expr*parse_expr_s(const char*s){const char*p=s;Expr*e=parse_expr(&p);if(!e)return NULL;skip(&p);if(*p){ERR("extra: %s",p);free_expr(e);return NULL;}return e;}

static bool parse_block(const SList*lines,size_t start,size_t end,size_t ind,Stmt***out,size_t*outn);
static Stmt*parse_one(const SList*lines,size_t*idx,size_t lim,size_t ind){
    const char*raw=lines->items[*idx];char*s=xdup(raw+ind);rtrim(s);strip_comment(s);Stmt*res=NULL;
    if(strncmp(s,"return",6)==0&&(s[6]==0||isspace((unsigned char)s[6]))){Stmt*st=calloc(1,sizeof(Stmt));if(!st)goto done;st->kind=SK_RETURN;const char*ex=ltrim(s+6);if(*ex){st->expr=parse_expr_s(ex);if(!st->expr){free(st);goto done;}}(*idx)++;res=st;goto done;}
    if(strcmp(s,"break")==0){Stmt*st=calloc(1,sizeof(Stmt));if(st){st->kind=SK_BREAK;(*idx)++;res=st;}goto done;}
    if(strcmp(s,"continue")==0){Stmt*st=calloc(1,sizeof(Stmt));if(st){st->kind=SK_CONTINUE;(*idx)++;res=st;}goto done;}
    if(strncmp(s,"print",5)==0&&s[5]=='('){const char*p=s+5;Expr**args;size_t ac;if(!parse_args(&p,&args,&ac))goto done;skip(&p);if(*p||ac!=1){for(size_t i=0;i<ac;i++)free_expr(args[i]);free(args);ERR(ac!=1?"print needs 1 arg":"extra tokens");goto done;}Stmt*st=calloc(1,sizeof(Stmt));if(!st){free_expr(args[0]);free(args);goto done;}st->kind=SK_PRINT;st->expr=args[0];free(args);(*idx)++;res=st;goto done;}
    if(strncmp(s,"for",3)==0&&isspace((unsigned char)s[3])){
        const char*fs=ltrim(s+3);
        char varname[128];size_t vn=0;
        while(*fs&&is_icc(*fs)&&vn<sizeof(varname)-1)varname[vn++]=*fs++;
        varname[vn]=0;
        if(vn==0){ERR("for needs variable");goto done;}
        skip(&fs);
        if(strncmp(fs,"in",2)!=0||!isspace((unsigned char)fs[2])){ERR("for needs 'in'");goto done;}
        fs+=2;skip(&fs);
        if(strncmp(fs,"range",5)!=0||fs[5]!='('){ERR("for needs range()");goto done;}
        const char*rp=fs+5;
        Expr**args;size_t ac;
        Expr*range_expr=calloc(1,sizeof(Expr));if(!range_expr)goto done;
        range_expr->kind=EK_BINOP;
        const char*tmp=rp;
        if(!parse_args(&tmp,&range_expr->args,&range_expr->argc)){free_expr(range_expr);goto done;}
        skip(&tmp);
        if(*tmp!=':'){ERR("for needs ':'");free_expr(range_expr);goto done;}
        
        size_t bs=*idx+1,be=blk_end(lines,bs,lim,ind+4);
        if(bs==be){free_expr(range_expr);ERR("for needs body");goto done;}
        Stmt**lb;size_t lc;
        if(!parse_block(lines,bs,be,ind+4,&lb,&lc)){free_expr(range_expr);goto done;}
        *idx=be;
        
        Stmt*st=calloc(1,sizeof(Stmt));if(!st){free_expr(range_expr);for(size_t i=0;i<lc;i++)free_stmt(lb[i]);free(lb);goto done;}
        st->kind=SK_FOR;
        st->loop_var=xdup(varname);
        st->loop=lb;st->nloop=lc;
        if(range_expr->argc==1){
            st->range_start=calloc(1,sizeof(Expr));st->range_start->kind=EK_INT;st->range_start->type=TYPE_INT;st->range_start->ival=0;
            st->range_end=range_expr->args[0];
            st->range_step=calloc(1,sizeof(Expr));st->range_step->kind=EK_INT;st->range_step->type=TYPE_INT;st->range_step->ival=1;
        }else if(range_expr->argc==2){
            st->range_start=range_expr->args[0];
            st->range_end=range_expr->args[1];
            st->range_step=calloc(1,sizeof(Expr));st->range_step->kind=EK_INT;st->range_step->type=TYPE_INT;st->range_step->ival=1;
        }else{
            st->range_start=range_expr->args[0];
            st->range_end=range_expr->args[1];
            st->range_step=range_expr->args[2];
        }
        free(range_expr->args);free(range_expr);
        res=st;goto done;
    }
    if(strncmp(s,"if",2)==0&&isspace((unsigned char)s[2])){
        const char*cs=ltrim(s+2);size_t cl=strlen(cs);if(!cl||cs[cl-1]!=':'){ERR("if needs ':'");goto done;}
        char*cstr=xndup(cs,cl-1);rtrim(cstr);Expr*cond=parse_expr_s(cstr);free(cstr);if(!cond)goto done;
        size_t bs=*idx+1,be=blk_end(lines,bs,lim,ind+4);if(bs==be){free_expr(cond);ERR("if needs body");goto done;}
        Stmt**tb;size_t tc;if(!parse_block(lines,bs,be,ind+4,&tb,&tc)){free_expr(cond);goto done;}
        *idx=be;while(*idx<lim){char*pr=xdup(lines->items[*idx]);rtrim(pr);if(!blank(pr)){free(pr);break;}free(pr);(*idx)++;}
        Stmt**eb=NULL;size_t ec=0;
        if(*idx<lim){
            const char*er=lines->items[*idx];
            if(strlen(er)>=ind&&has_ind(er,ind)&&er[ind]!=' '){
                char*ec2=xdup(er+ind);rtrim(ec2);strip_comment(ec2);
                if(strcmp(ec2,"else:")==0){
                    size_t es=*idx+1,ee=blk_end(lines,es,lim,ind+4);
                    if(es<ee&&!parse_block(lines,es,ee,ind+4,&eb,&ec)){free(ec2);for(size_t i=0;i<tc;i++)free_stmt(tb[i]);free(tb);free_expr(cond);goto done;}
                    *idx=ee;
                }else if(strncmp(ec2,"else if ",8)==0){
                    Stmt*chain_root=NULL;Stmt*chain_tail=NULL;
                    while(*idx<lim){
                        const char*cr=lines->items[*idx];
                        if(strlen(cr)<ind||!has_ind(cr,ind)||cr[ind]==' ')break;
                        char*line=xdup(cr+ind);rtrim(line);strip_comment(line);
                        if(strncmp(line,"else if ",8)==0){
                            const char*cc=ltrim(line+8);size_t ccl=strlen(cc);
                            if(!ccl||cc[ccl-1]!=':'){free(line);free(ec2);for(size_t i=0;i<tc;i++)free_stmt(tb[i]);free(tb);free_expr(cond);ERR("else if needs ':'");goto done;}
                            char*cond_src=xndup(cc,ccl-1);rtrim(cond_src);Expr*ec=parse_expr_s(cond_src);free(cond_src);
                            if(!ec){free(line);free(ec2);for(size_t i=0;i<tc;i++)free_stmt(tb[i]);free(tb);free_expr(cond);goto done;}
                            size_t ebs=*idx+1,ebe=blk_end(lines,ebs,lim,ind+4);
                            if(ebs==ebe){free_expr(ec);free(line);free(ec2);for(size_t i=0;i<tc;i++)free_stmt(tb[i]);free(tb);free_expr(cond);ERR("else if needs body");goto done;}
                            Stmt**etb=NULL;size_t etc=0;
                            if(!parse_block(lines,ebs,ebe,ind+4,&etb,&etc)){free_expr(ec);free(line);free(ec2);for(size_t i=0;i<tc;i++)free_stmt(tb[i]);free(tb);free_expr(cond);goto done;}
                            Stmt*node=calloc(1,sizeof(Stmt));
                            if(!node){free_expr(ec);for(size_t i=0;i<etc;i++)free_stmt(etb[i]);free(etb);free(line);free(ec2);for(size_t i=0;i<tc;i++)free_stmt(tb[i]);free(tb);free_expr(cond);goto done;}
                            node->kind=SK_IF;node->cond=ec;node->then=etb;node->nthen=etc;
                            if(!chain_root){chain_root=node;chain_tail=node;}else{chain_tail->els=malloc(sizeof(Stmt*));if(!chain_tail->els){free_stmt(node);free(line);free(ec2);for(size_t i=0;i<tc;i++)free_stmt(tb[i]);free(tb);free_expr(cond);goto done;}chain_tail->els[0]=node;chain_tail->nels=1;chain_tail=node;}
                            *idx=ebe;
                            while(*idx<lim){char*sp=xdup(lines->items[*idx]);rtrim(sp);if(!blank(sp)){free(sp);break;}free(sp);(*idx)++;}
                            free(line);
                            continue;
                        }
                        if(strcmp(line,"else:")==0&&chain_tail){
                            size_t ebs=*idx+1,ebe=blk_end(lines,ebs,lim,ind+4);
                            Stmt**etb=NULL;size_t etc=0;
                            if(ebs<ebe&&!parse_block(lines,ebs,ebe,ind+4,&etb,&etc)){free(line);free(ec2);for(size_t i=0;i<tc;i++)free_stmt(tb[i]);free(tb);free_expr(cond);free_stmt(chain_root);goto done;}
                            chain_tail->els=etb;chain_tail->nels=etc;
                            *idx=ebe;
                            free(line);
                            break;
                        }
                        free(line);
                        break;
                    }
                    if(chain_root){
                        eb=malloc(sizeof(Stmt*));
                        if(!eb){free(ec2);for(size_t i=0;i<tc;i++)free_stmt(tb[i]);free(tb);free_expr(cond);free_stmt(chain_root);goto done;}
                        eb[0]=chain_root;ec=1;
                    }
                }
                free(ec2);
            }
        }
        Stmt*st=calloc(1,sizeof(Stmt));if(!st){free_expr(cond);for(size_t i=0;i<tc;i++)free_stmt(tb[i]);free(tb);for(size_t i=0;i<ec;i++)free_stmt(eb[i]);free(eb);goto done;}
        st->kind=SK_IF;st->cond=cond;st->then=tb;st->nthen=tc;st->els=eb;st->nels=ec;res=st;goto done;
    }
    if(strncmp(s,"while",5)==0&&isspace((unsigned char)s[5])){
        const char*cs=ltrim(s+5);size_t cl=strlen(cs);if(!cl||cs[cl-1]!=':'){ERR("while needs ':'");goto done;}
        char*cstr=xndup(cs,cl-1);rtrim(cstr);Expr*cond=parse_expr_s(cstr);free(cstr);if(!cond)goto done;
        size_t bs=*idx+1,be=blk_end(lines,bs,lim,ind+4);if(bs==be){free_expr(cond);ERR("while needs body");goto done;}
        Stmt**lb;size_t lc;if(!parse_block(lines,bs,be,ind+4,&lb,&lc)){free_expr(cond);goto done;}
        *idx=be;Stmt*st=calloc(1,sizeof(Stmt));if(!st){free_expr(cond);for(size_t i=0;i<lc;i++)free_stmt(lb[i]);free(lb);goto done;}
        st->kind=SK_WHILE;st->cond=cond;st->loop=lb;st->nloop=lc;res=st;goto done;
    }
    if(strcmp(s,"else:")==0)goto done;
    {bool in=false,esc=false;char*eq=NULL;for(char*p2=s;*p2;p2++){if(*p2=='\\'&&!esc&&in){esc=true;continue;}if(*p2=='"'&&!esc)in=!in;if(*p2=='='&&!in&&p2[1]!='='){eq=p2;break;}esc=false;}
     if(eq){char*lhs=xndup(s,(size_t)(eq-s));rtrim(lhs);const char*lt2=ltrim(lhs);
        /* Check for dereference assignment: *expr = value */
        if(*lt2=='*' && lt2[1]!='='){
            Expr*target=parse_expr_s(lt2);
            if(!target){free(lhs);goto done;}
            Expr*val=parse_expr_s(ltrim(eq+1));
            if(!val){free(lhs);free_expr(target);goto done;}
            Stmt*st=calloc(1,sizeof(Stmt));
            if(!st){free(lhs);free_expr(target);free_expr(val);goto done;}
            st->kind=SK_ASSIGN;
            st->var=NULL;  /* Mark as dereference assignment */
            st->expr=val;
            st->cond=target;  /* Reuse cond for the target expression */
            (*idx)++;
            res=st;
            free(lhs);
            goto done;
        }
        /* Check for type annotation: type var = value */
        char type_id[128];size_t tn=0;const char*tp=lt2;
        while(is_icc(*tp)){if(tn+1>=sizeof(type_id)){free(lhs);ERR("type too long");goto done;}type_id[tn++]=*tp++;}type_id[tn]=0;
        skip(&tp);
        /* Check if this is a type annotation (type followed by variable name) */
        if(tn>0 && is_ic(*tp)){
            TypeKind var_type=TYPE_INT;
            if(strcmp(type_id,"ptr")==0)var_type=TYPE_PTR;
            else if(strcmp(type_id,"int")==0)var_type=TYPE_INT;
            else if(strcmp(type_id,"bool")==0)var_type=TYPE_BOOL;
            else if(strcmp(type_id,"string")==0)var_type=TYPE_STRING;
            /* Parse variable name */
            char id[128];size_t n=0;while(is_icc(*tp)){if(n+1>=sizeof(id)){free(lhs);ERR("ident too long");goto done;}id[n++]=*tp++;}id[n]=0;
            if(n && !*ltrim(tp)){
                free(lhs);
                Expr*val=parse_expr_s(ltrim(eq+1));
                if(!val)goto done;
                Stmt*st=calloc(1,sizeof(Stmt));
                if(!st){free_expr(val);goto done;}
                st->kind=SK_ASSIGN;
                st->var=xdup(id);
                st->expr=val;
                st->var_type=var_type;
                (*idx)++;
                res=st;
                goto done;
            }
        }
        /* Regular variable assignment (no type annotation) */
        char id[128];size_t n=0;const char*lp=lt2;while(is_icc(*lp)){if(n+1>=sizeof(id)){free(lhs);ERR("ident too long");goto done;}id[n++]=*lp++;}id[n]=0;
        if(n && !*ltrim(lp)){
            free(lhs);
            Expr*val=parse_expr_s(ltrim(eq+1));
            if(!val)goto done;
            Stmt*st=calloc(1,sizeof(Stmt));
            if(!st){free_expr(val);goto done;}
            st->kind=SK_ASSIGN;
            st->var=xdup(id);
            st->expr=val;
            (*idx)++;
            res=st;
            goto done;
        }
        free(lhs);
    }}
    {const char*p2=s;Expr*e=parse_expr(&p2);skip(&p2);if(e&&!*p2&&(e->kind==EK_CALL||e->kind==EK_FREE)){Stmt*st=calloc(1,sizeof(Stmt));if(!st){free_expr(e);goto done;}st->kind=(e->kind==EK_FREE)?SK_CALL:SK_CALL;st->expr=e;(*idx)++;res=st;goto done;}free_expr(e);}
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
static bool parse_fn_hdr(const char*raw,char**oname,char***oparams,size_t*opc){
    const char*p=raw;if(strncmp(p,"fn",2)||!isspace((unsigned char)p[2])){ERR("expected 'fn'");return false;}p+=2;while(*p&&isspace((unsigned char)*p))p++;
    char name[128];size_t n=0;if(!is_ic(*p)){ERR("bad fn name");return false;}while(is_icc(*p)){if(n+1>=sizeof(name)){ERR("name too long");return false;}name[n++]=*p++;}name[n]=0;
    while(*p&&isspace((unsigned char)*p))p++;if(*p!='('){ERR("expected '('");return false;}p++;
    char**params=NULL;size_t pc=0,pcap=0;
    while(*p&&*p!=')'){while(*p&&isspace((unsigned char)*p))p++;if(!is_ic(*p)){for(size_t i=0;i<pc;i++)free(params[i]);free(params);ERR("bad param");return false;}
        char pn[128];size_t pnn=0;while(is_icc(*p)){if(pnn+1>=sizeof(pn)){for(size_t i=0;i<pc;i++)free(params[i]);free(params);ERR("param too long");return false;}pn[pnn++]=*p++;}pn[pnn]=0;
        if(pc==pcap){size_t nc=pcap?pcap*2:4;char**np=realloc(params,nc*sizeof(char*));if(!np){for(size_t i=0;i<pc;i++)free(params[i]);free(params);return false;}params=np;pcap=nc;}
        params[pc++]=xdup(pn);while(*p&&isspace((unsigned char)*p))p++;if(*p==',')p++;
    }
    if(*p!=')'){for(size_t i=0;i<pc;i++)free(params[i]);free(params);ERR("missing ')'");return false;}p++;
    while(*p&&isspace((unsigned char)*p))p++;if(*p!=':'){for(size_t i=0;i<pc;i++)free(params[i]);free(params);ERR("expected ':'");return false;}
    *oname=xdup(name);*oparams=params;*opc=pc;return true;
}
static bool parse_program(const SList*lines,Program*prog){
    prog->fns=NULL;prog->nfns=0;prog->cap=0;size_t i=0;
    while(i<lines->len){
        const char*r=lines->items[i];char*cp=xdup(r);rtrim(cp);if(blank(cp)){free(cp);i++;continue;}free(cp);
        if(r[0]==' '||r[0]=='\t'){fprintf(stderr,"Error line %zu: unexpected indent\n",i+1);return false;}
        char*rl=xdup(r);rtrim(rl);strip_comment(rl);char*fn_name;char**params;size_t pc;
        if(!parse_fn_hdr(rl,&fn_name,&params,&pc)){fprintf(stderr,"Error line %zu: %s\n",i+1,g_err);free(rl);return false;}free(rl);
        size_t bs=i+1,be=blk_end(lines,bs,lines->len,4);if(bs==be){fprintf(stderr,"Error: fn '%s' empty\n",fn_name);free(fn_name);for(size_t j=0;j<pc;j++)free(params[j]);free(params);return false;}
        Stmt**body;size_t bc;if(!parse_block(lines,bs,be,4,&body,&bc)){fprintf(stderr,"Error in '%s': %s\n",fn_name,g_err);free(fn_name);for(size_t j=0;j<pc;j++)free(params[j]);free(params);return false;}
        if(prog->nfns==prog->cap){size_t nc=prog->cap?prog->cap*2:4;FnDef*np=realloc(prog->fns,nc*sizeof(FnDef));if(!np){free(fn_name);for(size_t j=0;j<pc;j++)free(params[j]);free(params);for(size_t j=0;j<bc;j++)free_stmt(body[j]);free(body);return false;}prog->fns=np;prog->cap=nc;}
        FnDef*fn=&prog->fns[prog->nfns++];fn->name=fn_name;fn->params=params;fn->nparams=pc;fn->param_types=calloc(pc,sizeof(TypeKind));fn->return_type=TYPE_INT;fn->body=body;fn->nbody=bc;
        i=be;
    }
    return true;
}

/* ======================================================
 * TYPE CHECKER
 * ====================================================== */
static TypeKind type_check_expr(Expr*e,Scope*sc,Program*prog);

static FnDef* find_fn(Program*prog,const char*name){
    for(size_t i=0;i<prog->nfns;i++)if(strcmp(prog->fns[i].name,name)==0)return &prog->fns[i];
    return NULL;
}

static TypeKind type_check_expr(Expr*e,Scope*sc,Program*prog){
    if(!e)return TYPE_VOID;
    switch(e->kind){
    case EK_INT:
        e->type=TYPE_INT;
        return TYPE_INT;
    case EK_BOOL:
        e->type=TYPE_BOOL;
        return TYPE_BOOL;
    case EK_STRING:
        e->type=TYPE_STRING;
        return TYPE_STRING;
    case EK_PTR_NULL:
        e->type=TYPE_PTR;
        return TYPE_PTR;
    case EK_VAR:{
        Symbol*sym=scope_find(sc,e->name);
        if(!sym){ERR("undefined variable '%s'",e->name);return TYPE_UNKNOWN;}
        e->type=sym->type;
        return sym->type;
    }
    case EK_INT_CAST:{
        if(e->argc!=1)return TYPE_UNKNOWN;
        TypeKind arg_type=type_check_expr(e->args[0],sc,prog);
        if(arg_type==TYPE_UNKNOWN)return TYPE_UNKNOWN;
        e->type=TYPE_INT;
        return TYPE_INT;
    }
    case EK_PTR_CAST:{
        if(e->argc!=1)return TYPE_UNKNOWN;
        TypeKind arg_type=type_check_expr(e->args[0],sc,prog);
        if(arg_type==TYPE_UNKNOWN)return TYPE_UNKNOWN;
        if(arg_type!=TYPE_INT){
            ERR("ptr() requires int argument, got %s",type_name(arg_type));
            return TYPE_UNKNOWN;
        }
        e->type=TYPE_PTR;
        return TYPE_PTR;
    }
    case EK_ALLOC:{
        if(e->argc!=1)return TYPE_UNKNOWN;
        TypeKind arg_type=type_check_expr(e->args[0],sc,prog);
        if(arg_type==TYPE_UNKNOWN)return TYPE_UNKNOWN;
        if(arg_type!=TYPE_INT){
            ERR("alloc() requires int size argument, got %s",type_name(arg_type));
            return TYPE_UNKNOWN;
        }
        e->type=TYPE_PTR;
        return TYPE_PTR;
    }
    case EK_FREE:{
        if(e->argc!=1)return TYPE_UNKNOWN;
        TypeKind arg_type=type_check_expr(e->args[0],sc,prog);
        if(arg_type==TYPE_UNKNOWN)return TYPE_UNKNOWN;
        if(arg_type!=TYPE_PTR){
            ERR("free() requires pointer argument, got %s",type_name(arg_type));
            return TYPE_UNKNOWN;
        }
        e->type=TYPE_VOID;
        return TYPE_VOID;
    }
    case EK_DEREF:{
        TypeKind inner_type=type_check_expr(e->left,sc,prog);
        if(inner_type==TYPE_UNKNOWN)return TYPE_UNKNOWN;
        if(inner_type!=TYPE_PTR){
            ERR("cannot dereference non-pointer type %s",type_name(inner_type));
            return TYPE_UNKNOWN;
        }
        e->type=TYPE_INT;  /* Dereferencing gives int for now */
        return TYPE_INT;
    }
    case EK_ADDROF:{
        TypeKind inner_type=type_check_expr(e->left,sc,prog);
        if(inner_type==TYPE_UNKNOWN)return TYPE_UNKNOWN;
        if(e->left->kind!=EK_VAR){
            ERR("address-of requires a variable");
            return TYPE_UNKNOWN;
        }
        e->type=TYPE_PTR;
        return TYPE_PTR;
    }
    case EK_CALL:{
        FnDef*fn=find_fn(prog,e->fn);
        if(!fn){ERR("undefined function '%s'",e->fn);return TYPE_UNKNOWN;}
        if(fn->nparams!=e->argc){ERR("function '%s' expects %zu args, got %zu",e->fn,fn->nparams,e->argc);return TYPE_UNKNOWN;}
        for(size_t i=0;i<e->argc;i++){
            TypeKind at=type_check_expr(e->args[i],sc,prog);
            if(at==TYPE_UNKNOWN)return TYPE_UNKNOWN;
            if(at!=fn->param_types[i]&&!(at==TYPE_BOOL&&fn->param_types[i]==TYPE_INT)){
                ERR("argument %zu: expected %s, got %s",i+1,type_name(fn->param_types[i]),type_name(at));
                return TYPE_UNKNOWN;
            }
        }
        e->type=fn->return_type;
        return fn->return_type;
    }
    case EK_BINOP:{
        TypeKind lt=type_check_expr(e->left,sc,prog);
        TypeKind rt=type_check_expr(e->right,sc,prog);
        if(lt==TYPE_UNKNOWN||rt==TYPE_UNKNOWN)return TYPE_UNKNOWN;
        const char*op=e->op;
        if(strcmp(op,"+")==0||strcmp(op,"-")==0||strcmp(op,"*")==0||strcmp(op,"/")==0){
            if(lt!=TYPE_INT||rt!=TYPE_INT){
                ERR("operator '%s' requires int operands, got %s and %s",op,type_name(lt),type_name(rt));
                return TYPE_UNKNOWN;
            }
            e->type=TYPE_INT;
            return TYPE_INT;
        }
        if(strcmp(op,"<")==0||strcmp(op,">")==0||strcmp(op,"<=")==0||strcmp(op,">=")==0){
            if(lt!=TYPE_INT||rt!=TYPE_INT){
                ERR("comparison '%s' requires int operands, got %s and %s",op,type_name(lt),type_name(rt));
                return TYPE_UNKNOWN;
            }
            e->type=TYPE_BOOL;
            return TYPE_BOOL;
        }
        if(strcmp(op,"==")==0||strcmp(op,"!=")==0){
            if(lt!=rt&&!(lt==TYPE_BOOL&&rt==TYPE_INT)&&!(lt==TYPE_INT&&rt==TYPE_BOOL)){
                ERR("equality '%s' requires matching types, got %s and %s",op,type_name(lt),type_name(rt));
                return TYPE_UNKNOWN;
            }
            e->type=TYPE_BOOL;
            return TYPE_BOOL;
        }
        ERR("unknown operator '%s'",op);
        return TYPE_UNKNOWN;
    }
    default:
        ERR("unknown expression kind");
        return TYPE_UNKNOWN;
    }
}

/* Forward declaration for type_check_stmts */
static bool type_check_stmts(Stmt**stmts,size_t count,Scope*sc,Program*prog,bool in_loop);

static bool type_check_stmt(Stmt*s,Scope*sc,Program*prog,bool in_loop){
    switch(s->kind){
    case SK_PRINT:{
        TypeKind t=type_check_expr(s->expr,sc,prog);
        if(t==TYPE_UNKNOWN)return false;
        if(t!=TYPE_INT&&t!=TYPE_BOOL&&t!=TYPE_STRING){
            ERR("print only supports int, bool, and string, got %s",type_name(t));
            return false;
        }
        return true;
    }
    case SK_ASSIGN:{
        /* Check for dereference assignment (var is NULL) */
        if(!s->var){
            /* s->cond holds the target expression (e.g., *x) */
            TypeKind target_type=type_check_expr(s->cond,sc,prog);
            if(target_type==TYPE_UNKNOWN)return false;
            if(s->cond->kind!=EK_DEREF){
                ERR("left side of assignment must be a variable or dereference");
                return false;
            }
            TypeKind val_type=type_check_expr(s->expr,sc,prog);
            if(val_type==TYPE_UNKNOWN)return false;
            if(val_type!=TYPE_INT){
                ERR("can only assign int to dereferenced pointer, got %s",type_name(val_type));
                return false;
            }
            return true;
        }
        Symbol*sym=scope_find(sc,s->var);
        if(!sym){
            TypeKind t=type_check_expr(s->expr,sc,prog);
            if(t==TYPE_UNKNOWN)return false;
            if(!scope_add(sc,s->var,t,0))return false;
            s->var_type=t;
        }else{
            TypeKind t=type_check_expr(s->expr,sc,prog);
            if(t==TYPE_UNKNOWN)return false;
            if(t!=sym->type&&!(t==TYPE_BOOL&&sym->type==TYPE_INT)){
                ERR("cannot assign %s to variable '%s' of type %s",type_name(t),s->var,type_name(sym->type));
                return false;
            }
            s->var_type=sym->type;
        }
        return true;
    }
    case SK_IF:{
        TypeKind ct=type_check_expr(s->cond,sc,prog);
        if(ct==TYPE_UNKNOWN)return false;
        if(ct!=TYPE_INT&&ct!=TYPE_BOOL){
            ERR("condition must be int or bool, got %s",type_name(ct));
            return false;
        }
        Scope inner;scope_init(&inner,sc);
        if(!type_check_stmts(s->then,s->nthen,&inner,prog,in_loop)){scope_free(&inner);return false;}
        scope_free(&inner);
        if(s->nels>0){
            Scope e_scope;scope_init(&e_scope,sc);
            if(!type_check_stmts(s->els,s->nels,&e_scope,prog,in_loop)){scope_free(&e_scope);return false;}
            scope_free(&e_scope);
        }
        return true;
    }
    case SK_WHILE:{
        TypeKind ct=type_check_expr(s->cond,sc,prog);
        if(ct==TYPE_UNKNOWN)return false;
        if(ct!=TYPE_INT&&ct!=TYPE_BOOL){
            ERR("condition must be int or bool, got %s",type_name(ct));
            return false;
        }
        Scope inner;scope_init(&inner,sc);
        if(!type_check_stmts(s->loop,s->nloop,&inner,prog,true)){scope_free(&inner);return false;}
        scope_free(&inner);
        return true;
    }
    case SK_FOR:{
        TypeKind st=type_check_expr(s->range_start,sc,prog);
        if(st!=TYPE_INT){ERR("range start must be int");return false;}
        TypeKind et=type_check_expr(s->range_end,sc,prog);
        if(et!=TYPE_INT){ERR("range end must be int");return false;}
        TypeKind stp=type_check_expr(s->range_step,sc,prog);
        if(stp!=TYPE_INT){ERR("range step must be int");return false;}
        Scope inner;scope_init(&inner,sc);
        if(!scope_add(&inner,s->loop_var,TYPE_INT,0)){scope_free(&inner);return false;}
        if(!type_check_stmts(s->loop,s->nloop,&inner,prog,true)){scope_free(&inner);return false;}
        scope_free(&inner);
        return true;
    }
    case SK_RETURN:{
        if(s->expr){
            TypeKind t=type_check_expr(s->expr,sc,prog);
            if(t==TYPE_UNKNOWN)return false;
        }
        return true;
    }
    case SK_BREAK:
    case SK_CONTINUE:
        if(!in_loop){ERR("'%s' outside loop",s->kind==SK_BREAK?"break":"continue");return false;}
        return true;
    case SK_CALL:{
        TypeKind t=type_check_expr(s->expr,sc,prog);
        return t!=TYPE_UNKNOWN;
    }
    default:
        return true;
    }
}

static bool type_check_stmts(Stmt**stmts,size_t count,Scope*sc,Program*prog,bool in_loop){
    for(size_t i=0;i<count;i++){
        if(!type_check_stmt(stmts[i],sc,prog,in_loop))return false;
    }
    return true;
}

static bool type_check_fn(FnDef*fn,Program*prog){
    Scope sc;scope_init(&sc,NULL);
    for(size_t i=0;i<fn->nparams;i++){
        fn->param_types[i]=TYPE_INT;
        if(!scope_add(&sc,fn->params[i],fn->param_types[i],1)){scope_free(&sc);return false;}
    }
    if(!type_check_stmts(fn->body,fn->nbody,&sc,prog,false)){scope_free(&sc);return false;}
    scope_free(&sc);
    return true;
}

static bool type_check_program(Program*prog){
    for(size_t i=0;i<prog->nfns;i++){
        if(!type_check_fn(&prog->fns[i],prog)){
            fprintf(stderr,"TypeError in '%s': %s\n",prog->fns[i].name,g_err);
            return false;
        }
    }
    return true;
}

/* ======================================================
 * C CODE GENERATOR (Static Types)
 * ====================================================== */
typedef struct{SBuf out;size_t lbl;}CG;
static void cg_init(CG*g){sb_init(&g->out);g->lbl=0;}
static void cg_free(CG*g){sb_free(&g->out);}
static size_t L(CG*g){return g->lbl++;}
static void E(CG*g,const char*fmt,...){char tmp[1024];va_list ap;va_start(ap,fmt);vsnprintf(tmp,sizeof(tmp),fmt,ap);va_end(ap);sb_cat(&g->out,tmp);}

static const char* cg_type(TypeKind t){
    switch(t){
        case TYPE_INT: return "long long";
        case TYPE_PTR: return "void*";
        case TYPE_BOOL: return "int";
        case TYPE_STRING: return "const char*";
        case TYPE_VOID: return "void";
        default: return "long long";
    }
}

/* Helper to escape string for C output */
static char* escape_for_c(const char* s) {
    size_t len = strlen(s);
    char* result = malloc(len * 4 + 1); /* worst case: each char becomes \xNN */
    if (!result) return NULL;
    
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '\n': result[j++] = '\\'; result[j++] = 'n'; break;
            case '\t': result[j++] = '\\'; result[j++] = 't'; break;
            case '\r': result[j++] = '\\'; result[j++] = 'r'; break;
            case '\\': result[j++] = '\\'; result[j++] = '\\'; break;
            case '"': result[j++] = '\\'; result[j++] = '"'; break;
            case '\0': result[j++] = '\\'; result[j++] = '0'; break;
            default:
                if (c >= 32 && c < 127) {
                    result[j++] = c;
                } else {
                    /* Output as hex escape */
                    j += sprintf(result + j, "\\x%02x", c);
                }
                break;
        }
    }
    result[j] = '\0';
    return result;
}

static bool cg_expr(CG*g,Expr*e);
static bool cg_stmts(CG*g,Stmt**s,size_t n,bool inlp);

static bool cg_expr_val(CG*g,Expr*e,SBuf*result){
    switch(e->kind){
    case EK_INT:
        sb_fmt(result,"%lld",e->ival);
        return true;
    case EK_BOOL:
        sb_cat(result,e->ival?"1":"0");
        return true;
    case EK_STRING:{
        char* escaped = escape_for_c(e->sval);
        if (!escaped) { ERR("failed to escape string"); return false; }
        sb_fmt(result,"\"%s\"", escaped);
        free(escaped);
        return true;
    }
    case EK_PTR_NULL:
        sb_cat(result,"NULL");
        return true;
    case EK_VAR:
        sb_cat(result,e->name);
        return true;
    case EK_INT_CAST:{
        SBuf arg;sb_init(&arg);
        if(!cg_expr_val(g,e->args[0],&arg)){sb_free(&arg);return false;}
        sb_fmt(result,"((long long)(%s))",arg.d);
        sb_free(&arg);
        return true;
    }
    case EK_PTR_CAST:{
        SBuf arg;sb_init(&arg);
        if(!cg_expr_val(g,e->args[0],&arg)){sb_free(&arg);return false;}
        sb_fmt(result,"((void*)(size_t)(%s))",arg.d);
        sb_free(&arg);
        return true;
    }
    case EK_ALLOC:{
        SBuf arg;sb_init(&arg);
        if(!cg_expr_val(g,e->args[0],&arg)){sb_free(&arg);return false;}
        sb_fmt(result,"malloc((size_t)(%s))",arg.d);
        sb_free(&arg);
        return true;
    }
    case EK_FREE:{
        SBuf arg;sb_init(&arg);
        if(!cg_expr_val(g,e->args[0],&arg)){sb_free(&arg);return false;}
        sb_fmt(result,"(free(%s), (void*)0)",arg.d);
        sb_free(&arg);
        return true;
    }
    case EK_DEREF:{
        SBuf inner;sb_init(&inner);
        if(!cg_expr_val(g,e->left,&inner)){sb_free(&inner);return false;}
        sb_fmt(result,"(*((long long*)(%s))",inner.d);
        sb_cat(result,")");
        sb_free(&inner);
        return true;
    }
    case EK_ADDROF:{
        if(e->left->kind==EK_VAR){
            sb_fmt(result,"((void*)&%s)",e->left->name);
        }else{
            ERR("address-of requires a variable");
            return false;
        }
        return true;
    }
    case EK_CALL:{
        SBuf*args=malloc(e->argc*sizeof(SBuf));
        for(size_t i=0;i<e->argc;i++){
            sb_init(&args[i]);
            if(!cg_expr_val(g,e->args[i],&args[i])){
                for(size_t j=0;j<=i;j++)sb_free(&args[j]);
                free(args);
                return false;
            }
        }
        sb_fmt(result,"_fn_%s(",e->fn);
        for(size_t i=0;i<e->argc;i++){
            if(i)sb_cat(result,", ");
            sb_cat(result,args[i].d);
            sb_free(&args[i]);
        }
        sb_cat(result,")");
        free(args);
        return true;
    }
    case EK_BINOP:{
        SBuf left,right;sb_init(&left);sb_init(&right);
        if(!cg_expr_val(g,e->left,&left)){sb_free(&left);sb_free(&right);return false;}
        if(!cg_expr_val(g,e->right,&right)){sb_free(&left);sb_free(&right);return false;}
        const char*op=e->op;
        if(strcmp(op,"==")==0||strcmp(op,"!=")==0||strcmp(op,"<")==0||strcmp(op,">")==0||strcmp(op,"<=")==0||strcmp(op,">=")==0){
            sb_fmt(result,"(%s %s %s ? 1 : 0)",left.d,op,right.d);
        }else{
            sb_fmt(result,"(%s %s %s)",left.d,op,right.d);
        }
        sb_free(&left);sb_free(&right);
        return true;
    }
    default:
        ERR("unknown expression kind in codegen");
        return false;
    }
}

static bool cg_expr(CG*g,Expr*e){
    SBuf tmp;sb_init(&tmp);
    bool ok=cg_expr_val(g,e,&tmp);
    sb_free(&tmp);
    return ok;
}

static bool cg_stmts(CG*g,Stmt**stmts,size_t count,bool inlp){
    for(size_t i=0;i<count;i++){
        Stmt*s=stmts[i];
        switch(s->kind){
        case SK_PRINT:{
            SBuf expr;sb_init(&expr);
            if(!cg_expr_val(g,s->expr,&expr)){sb_free(&expr);return false;}
            if(s->expr->type==TYPE_BOOL){
                E(g,"    printf(\"%%s\\n\", %s ? \"true\" : \"false\");\n",expr.d);
            }else if(s->expr->type==TYPE_STRING){
                E(g,"    printf(\"%%s\\n\", %s);\n",expr.d);
            }else{
                E(g,"    printf(\"%%lld\\n\", (long long)(%s));\n",expr.d);
            }
            sb_free(&expr);
            break;
        }
        case SK_ASSIGN:{
            SBuf expr;sb_init(&expr);
            if(!cg_expr_val(g,s->expr,&expr)){sb_free(&expr);return false;}
            /* Check for dereference assignment (var is NULL) */
            if(!s->var){
                /* s->cond holds the target expression (EK_DEREF) */
                /* Generate: (*((long long*)(inner_ptr)) = value; */
                SBuf inner;sb_init(&inner);
                if(s->cond && s->cond->kind == EK_DEREF && s->cond->left){
                    if(!cg_expr_val(g,s->cond->left,&inner)){sb_free(&inner);sb_free(&expr);return false;}
                    E(g,"    (*((long long*)(%s)) = %s);\n",inner.d,expr.d);
                }else{
                    sb_free(&inner);sb_free(&expr);
                    ERR("invalid dereference assignment");
                    return false;
                }
                sb_free(&inner);
            }else{
                E(g,"    %s = %s;\n",s->var,expr.d);
            }
            sb_free(&expr);
            break;
        }
        case SK_IF:{
            SBuf cond;sb_init(&cond);
            if(!cg_expr_val(g,s->cond,&cond)){sb_free(&cond);return false;}
            size_t le=L(g);
            E(g,"    if(%s) {\n",cond.d);
            sb_free(&cond);
            if(!cg_stmts(g,s->then,s->nthen,inlp))return false;
            if(s->nels>0){
                E(g,"    } else {\n");
                if(!cg_stmts(g,s->els,s->nels,inlp))return false;
            }
            E(g,"    }\n");
            (void)le;
            break;
        }
        case SK_WHILE:{
            size_t lw=L(g);
            E(g,"    while(1) {\n");
            SBuf cond;sb_init(&cond);
            if(!cg_expr_val(g,s->cond,&cond)){sb_free(&cond);return false;}
            E(g,"        if(!(%s)) break;\n",cond.d);
            sb_free(&cond);
            if(!cg_stmts(g,s->loop,s->nloop,true))return false;
            E(g,"    }\n");
            (void)lw;
            break;
        }
        case SK_FOR:{
            size_t lf=L(g);
            SBuf start,end,step;sb_init(&start);sb_init(&end);sb_init(&step);
            if(!cg_expr_val(g,s->range_start,&start)){sb_free(&start);sb_free(&end);sb_free(&step);return false;}
            if(!cg_expr_val(g,s->range_end,&end)){sb_free(&start);sb_free(&end);sb_free(&step);return false;}
            if(!cg_expr_val(g,s->range_step,&step)){sb_free(&start);sb_free(&end);sb_free(&step);return false;}
            E(g,"    {\n");
            E(g,"        long long _for_start_%zu = %s;\n",lf,start.d);
            E(g,"        long long _for_end_%zu = %s;\n",lf,end.d);
            E(g,"        long long _for_step_%zu = %s;\n",lf,step.d);
            sb_free(&start);sb_free(&end);sb_free(&step);
            E(g,"        if(_for_step_%zu == 0) { fprintf(stderr, \"Error: range step cannot be 0\\n\"); exit(1); }\n",lf);
            E(g,"        for(long long %s = _for_start_%zu; ",s->loop_var,lf);
            E(g,"(_for_step_%zu > 0) ? (%s < _for_end_%zu) : (%s > _for_end_%zu); ",lf,s->loop_var,lf,s->loop_var,lf);
            E(g,"%s += _for_step_%zu) {\n",s->loop_var,lf);
            if(!cg_stmts(g,s->loop,s->nloop,true))return false;
            E(g,"        }\n");
            E(g,"    }\n");
            break;
        }
        case SK_RETURN:{
            if(s->expr){
                SBuf expr;sb_init(&expr);
                if(!cg_expr_val(g,s->expr,&expr)){sb_free(&expr);return false;}
                E(g,"    return %s;\n",expr.d);
                sb_free(&expr);
            }else{
                E(g,"    return 0;\n");
            }
            break;
        }
        case SK_BREAK:
            E(g,"    break;\n");
            break;
        case SK_CONTINUE:
            E(g,"    continue;\n");
            break;
        case SK_CALL:{
            SBuf expr;sb_init(&expr);
            if(!cg_expr_val(g,s->expr,&expr)){sb_free(&expr);return false;}
            E(g,"    (void)%s;\n",expr.d);
            sb_free(&expr);
            break;
        }
        default:
            break;
        }
    }
    return true;
}

typedef struct {char*name;TypeKind type;}VarInfo;
static bool collect_vars(Stmt**body,size_t nbody,VarInfo**vars,size_t*count,size_t*cap,FnDef*fn){
    for(size_t i=0;i<nbody;i++){
        Stmt*s=body[i];
        if(s->kind==SK_ASSIGN && s->var){  /* Skip dereference assignments (s->var is NULL) */
            bool found=false;
            for(size_t j=0;j<*count;j++)if(strcmp((*vars)[j].name,s->var)==0){found=true;break;}
            if(!found){
                for(size_t j=0;j<fn->nparams;j++)if(strcmp(fn->params[j],s->var)==0){found=true;break;}
            }
            if(!found){
                if(*count==*cap){
                    size_t nc=*cap?*cap*2:8;
                    VarInfo*np=realloc(*vars,nc*sizeof(VarInfo));
                    if(!np)return false;
                    *vars=np;*cap=nc;
                }
                (*vars)[*count].name=xdup(s->var);
                (*vars)[*count].type=s->var_type?s->var_type:TYPE_INT;
                (*count)++;
            }
        }
        if(s->kind==SK_IF){
            if(!collect_vars(s->then,s->nthen,vars,count,cap,fn))return false;
            if(!collect_vars(s->els,s->nels,vars,count,cap,fn))return false;
        }
        if(s->kind==SK_WHILE){
            if(!collect_vars(s->loop,s->nloop,vars,count,cap,fn))return false;
        }
        if(s->kind==SK_FOR){
            bool found=false;
            for(size_t j=0;j<*count;j++)if(strcmp((*vars)[j].name,s->loop_var)==0){found=true;break;}
            for(size_t j=0;j<fn->nparams;j++)if(strcmp(fn->params[j],s->loop_var)==0){found=true;break;}
            if(!found){
                if(*count==*cap){
                    size_t nc=*cap?*cap*2:8;
                    VarInfo*np=realloc(*vars,nc*sizeof(VarInfo));
                    if(!np)return false;
                    *vars=np;*cap=nc;
                }
                (*vars)[*count].name=xdup(s->loop_var);
                (*vars)[*count].type=TYPE_INT;
                (*count)++;
            }
            if(!collect_vars(s->loop,s->nloop,vars,count,cap,fn))return false;
        }
    }
    return true;
}

static bool cg_fn(CG*g,FnDef*fn,const Program*prog){
    (void)prog;
    E(g,"static %s _fn_%s(",cg_type(fn->return_type),fn->name);
    for(size_t i=0;i<fn->nparams;i++){
        if(i)E(g,", ");
        E(g,"%s %s",cg_type(fn->param_types[i]),fn->params[i]);
    }
    E(g,") {\n");
    
    VarInfo*vars=NULL;size_t vcount=0,vcap=0;
    if(!collect_vars(fn->body,fn->nbody,&vars,&vcount,&vcap,fn)){
        free(vars);
        return false;
    }
    for(size_t i=0;i<vcount;i++){
        E(g,"    %s %s = 0;\n",cg_type(vars[i].type),vars[i].name);
        free(vars[i].name);
    }
    free(vars);
    
    if(!cg_stmts(g,fn->body,fn->nbody,false))return false;
    
    E(g,"    return 0;\n");
    E(g,"}\n\n");
    return true;
}

static const char*RUNTIME =
"#include <stdio.h>\n"
"#include <stdlib.h>\n"
"#include <string.h>\n\n"
"/* AutismLang v0.2.0 - Static typed runtime (minimal) */\n\n";

static bool codegen(const Program*prog,const char*out_path){
    CG g;cg_init(&g);
    sb_fmt(&g.out,"/* AutismLang v%s | backend=%s */\n",AUTISMLANG_VERSION,AUTISMLANG_BACKEND);
    sb_cat(&g.out,RUNTIME);
    for(size_t i=0;i<prog->nfns;i++){
        E(&g,"static %s _fn_%s(",cg_type(prog->fns[i].return_type),prog->fns[i].name);
        for(size_t j=0;j<prog->fns[i].nparams;j++){
            if(j)E(&g,", ");
            E(&g,"%s",cg_type(prog->fns[i].param_types[j]));
        }
        E(&g,");\n");
    }
    sb_cat(&g.out,"\n");
    for(size_t i=0;i<prog->nfns;i++){
        if(!cg_fn(&g,(FnDef*)&prog->fns[i],prog)){
            fprintf(stderr,"Codegen error in '%s': %s\n",prog->fns[i].name,g_err);
            cg_free(&g);return false;
        }
    }
    sb_cat(&g.out,"int main(void) { _fn_main(); return 0; }\n");
    
    char*pc=xdup(out_path);for(size_t i=0;pc[i];i++){if(pc[i]=='/'||pc[i]=='\\'){char sv=pc[i];pc[i]=0;if(pc[0])MKDIR(pc);pc[i]=sv;}}free(pc);
    FILE*fp=fopen(out_path,"wb");if(!fp){fprintf(stderr,"Cannot open '%s'\n",out_path);cg_free(&g);return false;}
    fwrite(g.out.d,1,g.out.len,fp);fclose(fp);
    cg_free(&g);return true;
}

int main(int argc,char**argv){
    const char*in=NULL,*out="build/out.c";
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"--help")==0){
            printf("Usage: %s <input.aut> [-o output.c] [--help] [--version] [--metadata]\n",argv[0]);
            printf("Backend: %s (static-typed)\n",AUTISMLANG_BACKEND);
            return 0;
        }else if(strcmp(argv[i],"--version")==0){
            printf("AutismLang Compiler %s (backend=%s)\n",AUTISMLANG_VERSION,AUTISMLANG_BACKEND);
            return 0;
        }else if(strcmp(argv[i],"--metadata")==0){
            printf("{\"version\":\"%s\",\"backend\":\"%s\"}\n",AUTISMLANG_VERSION,AUTISMLANG_BACKEND);
            return 0;
        }else if(strcmp(argv[i],"-o")==0){
            if(i+1>=argc){fprintf(stderr,"Usage: %s <input.aut> [-o output.c]\n",argv[0]);return 1;}
            out=argv[++i];
        }else if(argv[i][0]=='-'){
            fprintf(stderr,"Unknown option: %s\n",argv[i]);
            return 1;
        }else if(!in){
            in=argv[i];
        }else{
            fprintf(stderr,"Only one input file is allowed.\n");
            return 1;
        }
    }
    if(!in){fprintf(stderr,"Usage: %s <input.aut> [-o output.c] [--help] [--version] [--metadata]\n",argv[0]);return 1;}
    char*src=read_file(in);if(!src)return 1;
    SList lines;sl_init(&lines);if(!split_lines(src,&lines)){free(src);return 1;}free(src);
    Program prog;if(!parse_program(&lines,&prog)){sl_free(&lines);return 1;}
    bool has_main=false;for(size_t i=0;i<prog.nfns;i++)if(strcmp(prog.fns[i].name,"main")==0){has_main=true;break;}
    if(!has_main){fprintf(stderr,"Error: missing 'fn main():'\n");free_program(&prog);sl_free(&lines);return 1;}
    if(!type_check_program(&prog)){free_program(&prog);sl_free(&lines);return 1;}
    if(!codegen(&prog,out)){free_program(&prog);sl_free(&lines);return 1;}
    printf("Compiled %s -> %s\n",in,out);
    free_program(&prog);sl_free(&lines);return 0;
}