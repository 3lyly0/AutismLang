/*
 * AutismLang Compiler v0.1.0 - outputs C code instead of ASM
 * Build output: gcc out.c -o out.exe
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

#define AUTISMLANG_VERSION "0.1.0"
#define AUTISMLANG_BACKEND "c"

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
    fread(buf,1,(size_t)sz,f);buf[sz]=0;fclose(f);return buf;
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

/* ---- AST ---- */
typedef enum{EK_INT,EK_STR,EK_VAR,EK_BINOP,EK_CALL,EK_INPUT,EK_INT_CAST,EK_STR_CAST,EK_RANGE}EK;
typedef struct Expr{EK kind;long long ival;char*sval,*name,*fn;char op[3];struct Expr*left,*right,**args;size_t argc;}Expr;
typedef enum{SK_PRINT,SK_ASSIGN,SK_IF,SK_WHILE,SK_FOR,SK_RETURN,SK_BREAK,SK_CONTINUE,SK_CALL}SK;
typedef struct Stmt{SK kind;char*var;Expr*expr,*cond;struct Stmt**then;size_t nthen;struct Stmt**els;size_t nels;struct Stmt**loop;size_t nloop;
    char*loop_var;
    Expr*range_start,*range_end,*range_step;
}Stmt;
typedef struct{char*name;char**params;size_t nparams;Stmt**body;size_t nbody;}FnDef;
typedef struct{FnDef*fns;size_t nfns,cap;}Program;

static void free_expr(Expr*e){if(!e)return;free(e->sval);free(e->name);free(e->fn);free_expr(e->left);free_expr(e->right);for(size_t i=0;i<e->argc;i++)free_expr(e->args[i]);free(e->args);free(e);}
static void free_stmt(Stmt*s){if(!s)return;free(s->var);free_expr(s->expr);free_expr(s->cond);for(size_t i=0;i<s->nthen;i++)free_stmt(s->then[i]);free(s->then);for(size_t i=0;i<s->nels;i++)free_stmt(s->els[i]);free(s->els);for(size_t i=0;i<s->nloop;i++)free_stmt(s->loop[i]);free(s->loop);free(s->loop_var);free_expr(s->range_start);free_expr(s->range_end);free_expr(s->range_step);free(s);}
static void free_fn(FnDef*f){if(!f)return;free(f->name);for(size_t i=0;i<f->nparams;i++)free(f->params[i]);free(f->params);for(size_t i=0;i<f->nbody;i++)free_stmt(f->body[i]);free(f->body);}
static void free_program(Program*p){for(size_t i=0;i<p->nfns;i++)free_fn(&p->fns[i]);free(p->fns);}

/* ---- parser ---- */
static Expr*parse_expr(const char**pp);
static void skip(const char**pp){while(**pp&&isspace((unsigned char)**pp))(*pp)++;}
static char*parse_str_lit(const char**pp){
    if(**pp!='"'){ERR("expected '\"'");return NULL;}(*pp)++;
    SBuf b;sb_init(&b);
    while(**pp&&**pp!='"'){char c=**pp;if(c=='\\'){(*pp)++;switch(**pp){case'n':sb_cat(&b,"\n");break;case't':sb_cat(&b,"\t");break;case'r':sb_cat(&b,"\r");break;case'"':sb_cat(&b,"\"");break;case'\\':sb_cat(&b,"\\");break;default:ERR("bad escape \\%c",**pp);sb_free(&b);return NULL;}}else{char t[2]={c,0};sb_cat(&b,t);}(*pp)++;}
    if(**pp!='"'){ERR("unterminated string");sb_free(&b);return NULL;}(*pp)++;
    if(!b.d)return xdup("");return b.d;
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
    if(*p=='('){(*pp)++;Expr*e=parse_expr(pp);if(!e)return NULL;skip(pp);if(**pp!=')'){free_expr(e);ERR("missing ')'");return NULL;}(*pp)++;return e;}
    if(*p=='"'){char*s=parse_str_lit(pp);if(!s)return NULL;Expr*e=calloc(1,sizeof(Expr));if(!e){free(s);return NULL;}e->kind=EK_STR;e->sval=s;return e;}
    bool neg=false;if(*p=='-'&&isdigit((unsigned char)p[1])){neg=true;(*pp)++;p++;}
    if(isdigit((unsigned char)*p)){char*end;long long v=strtoll(*pp,&end,10);*pp=end;Expr*e=calloc(1,sizeof(Expr));if(!e)return NULL;e->kind=EK_INT;e->ival=neg?-v:v;return e;}(void)neg;
    if(is_ic(*p)){
        char id[128];size_t n=0;while(is_icc(**pp)){if(n+1>=sizeof(id)){ERR("ident too long");return NULL;}id[n++]=**pp;(*pp)++;}id[n]=0;skip(pp);
        if(**pp=='('){
            if(strcmp(id,"int")==0){
                Expr*e=calloc(1,sizeof(Expr));if(!e)return NULL;e->kind=EK_INT_CAST;
                if(!parse_args(pp,&e->args,&e->argc)){free(e);return NULL;}
                if(e->argc!=1){ERR("int() takes exactly 1 arg");free_expr(e);return NULL;}
                return e;
            }
            if(strcmp(id,"str")==0){
                Expr*e=calloc(1,sizeof(Expr));if(!e)return NULL;e->kind=EK_STR_CAST;
                if(!parse_args(pp,&e->args,&e->argc)){free(e);return NULL;}
                if(e->argc!=1){ERR("str() takes exactly 1 arg");free_expr(e);return NULL;}
                return e;
            }
            if(strcmp(id,"range")==0){
                Expr*e=calloc(1,sizeof(Expr));if(!e)return NULL;e->kind=EK_RANGE;
                if(!parse_args(pp,&e->args,&e->argc)){free(e);return NULL;}
                if(e->argc<1||e->argc>3){ERR("range() takes 1-3 args");free_expr(e);return NULL;}
                return e;
            }
            Expr*e=calloc(1,sizeof(Expr));if(!e)return NULL;e->kind=(strcmp(id,"input")==0)?EK_INPUT:EK_CALL;e->fn=xdup(id);if(!parse_args(pp,&e->args,&e->argc)){free_expr(e);return NULL;}if(e->kind==EK_INPUT&&e->argc>1){ERR("input() takes 0 or 1 arg");free_expr(e);return NULL;}return e;
        }
        if(strcmp(id,"True")==0||strcmp(id,"true")==0){Expr*e=calloc(1,sizeof(Expr));if(!e)return NULL;e->kind=EK_INT;e->ival=1;return e;}
        if(strcmp(id,"False")==0||strcmp(id,"false")==0){Expr*e=calloc(1,sizeof(Expr));if(!e)return NULL;e->kind=EK_INT;e->ival=0;return e;}
        Expr*e=calloc(1,sizeof(Expr));if(!e)return NULL;e->kind=EK_VAR;e->name=xdup(id);return e;
    }
    ERR("unexpected '%c'",(int)*p);return NULL;
}
static Expr*parse_product(const char**pp){Expr*l=parse_factor(pp);if(!l)return NULL;while(1){skip(pp);char op=**pp;if(op!='*'&&op!='/')break;(*pp)++;Expr*r=parse_factor(pp);if(!r){free_expr(l);return NULL;}Expr*e=calloc(1,sizeof(Expr));if(!e){free_expr(l);free_expr(r);return NULL;}e->kind=EK_BINOP;e->op[0]=op;e->left=l;e->right=r;l=e;}return l;}
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
        range_expr->kind=EK_RANGE;
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
            st->range_start=calloc(1,sizeof(Expr));st->range_start->kind=EK_INT;st->range_start->ival=0;
            st->range_end=range_expr->args[0];
            st->range_step=calloc(1,sizeof(Expr));st->range_step->kind=EK_INT;st->range_step->ival=1;
        }else if(range_expr->argc==2){
            st->range_start=range_expr->args[0];
            st->range_end=range_expr->args[1];
            st->range_step=calloc(1,sizeof(Expr));st->range_step->kind=EK_INT;st->range_step->ival=1;
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
     if(eq){char*lhs=xndup(s,(size_t)(eq-s));rtrim(lhs);const char*lt2=ltrim(lhs);char id[128];size_t n=0;const char*lp=lt2;while(is_icc(*lp)){if(n+1>=sizeof(id)){free(lhs);ERR("ident too long");goto done;}id[n++]=*lp++;}id[n]=0;if(n&&!*ltrim(lp)){free(lhs);Expr*val=parse_expr_s(ltrim(eq+1));if(!val)goto done;Stmt*st=calloc(1,sizeof(Stmt));if(!st){free_expr(val);goto done;}st->kind=SK_ASSIGN;st->var=xdup(id);st->expr=val;(*idx)++;res=st;goto done;}free(lhs);}}
    {const char*p2=s;Expr*e=parse_expr(&p2);skip(&p2);if(e&&!*p2&&e->kind==EK_CALL){Stmt*st=calloc(1,sizeof(Stmt));if(!st){free_expr(e);goto done;}st->kind=SK_CALL;st->expr=e;(*idx)++;res=st;goto done;}free_expr(e);}
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
        FnDef*fn=&prog->fns[prog->nfns++];fn->name=fn_name;fn->params=params;fn->nparams=pc;fn->body=body;fn->nbody=bc;
        i=be;
    }
    return true;
}

/* ======================================================
 * C CODE GENERATOR
 * ====================================================== */
typedef struct{SBuf out;size_t lbl;}CG;
static void cg_init(CG*g){sb_init(&g->out);g->lbl=0;}
static void cg_free(CG*g){sb_free(&g->out);}
static size_t L(CG*g){return g->lbl++;}
static void E(CG*g,const char*fmt,...){char tmp[1024];va_list ap;va_start(ap,fmt);vsnprintf(tmp,sizeof(tmp),fmt,ap);va_end(ap);sb_cat(&g->out,tmp);}

static void emit_c_str(CG*g,const char*s){
    E(g,"\"");
    for(size_t i=0;s[i];i++){
        unsigned char c=(unsigned char)s[i];
        if(c=='"')E(g,"\\\"");
        else if(c=='\\')E(g,"\\\\");
        else if(c=='\n')E(g,"\\n");
        else if(c=='\r')E(g,"\\r");
        else if(c=='\t')E(g,"\\t");
        else if(c<32||c>126){char t[8];snprintf(t,sizeof(t),"\\x%02x",c);E(g,"%s",t);}
        else{char t[2]={(char)c,0};E(g,"%s",t);}
    }
    E(g,"\"");
}

static bool cg_expr(CG*g,Expr*e,const Program*prog);
static bool cg_stmts(CG*g,Stmt**s,size_t n,const Program*prog,bool inlp);

static bool cg_expr_into(CG*g,Expr*e,const Program*prog,size_t tmp){
    switch(e->kind){
    case EK_INT:
        E(g,"    aut_val _tmp%zu = aut_int(%lld);\n",tmp,e->ival);
        return true;
    case EK_STR:
        E(g,"    aut_val _tmp%zu = aut_str_static(",tmp);
        emit_c_str(g,e->sval);
        E(g,");\n");
        return true;
    case EK_VAR:
        E(g,"    aut_val _tmp%zu = aut_copy(%s);\n",tmp,e->name);
        return true;
    case EK_INPUT:{
        if(e->argc==1){
            size_t pt=L(g);
            if(!cg_expr_into(g,e->args[0],prog,pt))return false;
            E(g,"    aut_print(_tmp%zu);\n",pt);
            E(g,"    aut_free(_tmp%zu);\n",pt);
        }
        E(g,"    aut_val _tmp%zu = aut_input();\n",tmp);
        return true;
    }
    case EK_INT_CAST:{
        if(e->argc!=1)return false;
        size_t at=L(g);
        if(!cg_expr_into(g,e->args[0],prog,at))return false;
        E(g,"    aut_val _tmp%zu = aut_to_int(_tmp%zu);\n",tmp,at);
        E(g,"    aut_free(_tmp%zu);\n",at);
        return true;
    }
    case EK_STR_CAST:{
        if(e->argc!=1)return false;
        size_t at=L(g);
        if(!cg_expr_into(g,e->args[0],prog,at))return false;
        E(g,"    aut_val _tmp%zu = aut_to_str(_tmp%zu);\n",tmp,at);
        E(g,"    aut_free(_tmp%zu);\n",at);
        return true;
    }
    case EK_CALL:{
        FnDef*fn=NULL;for(size_t i=0;i<prog->nfns;i++)if(strcmp(prog->fns[i].name,e->fn)==0){fn=&prog->fns[i];break;}
        if(!fn){ERR("unknown fn: %s",e->fn);return false;}
        if(fn->nparams!=e->argc){ERR("'%s' expects %zu args",e->fn,fn->nparams);return false;}
        size_t*tmps=malloc(e->argc*sizeof(size_t));if(!tmps)return false;
        for(size_t i=0;i<e->argc;i++){tmps[i]=L(g);if(!cg_expr_into(g,e->args[i],prog,tmps[i])){free(tmps);return false;}}
        E(g,"    aut_val _tmp%zu = _fn_%s(",tmp,e->fn);
        for(size_t i=0;i<e->argc;i++){if(i)E(g,", ");E(g,"_tmp%zu",tmps[i]);}
        E(g,");\n");
        for(size_t i=0;i<e->argc;i++)E(g,"    aut_free(_tmp%zu);\n",tmps[i]);
        free(tmps);
        return true;
    }
    case EK_BINOP:{
        size_t lt=L(g),rt=L(g);
        if(!cg_expr_into(g,e->left,prog,lt))return false;
        if(!cg_expr_into(g,e->right,prog,rt))return false;
        const char*op=e->op;
        if(strcmp(op,"+")==0)     E(g,"    aut_val _tmp%zu = aut_add(_tmp%zu, _tmp%zu);\n",tmp,lt,rt);
        else if(strcmp(op,"-")==0)E(g,"    aut_val _tmp%zu = aut_int(aut_expect_int(\"-\", _tmp%zu) - aut_expect_int(\"-\", _tmp%zu));\n",tmp,lt,rt);
        else if(strcmp(op,"*")==0)E(g,"    aut_val _tmp%zu = aut_int(aut_expect_int(\"*\", _tmp%zu) * aut_expect_int(\"*\", _tmp%zu));\n",tmp,lt,rt);
        else if(strcmp(op,"/")==0)E(g,"    aut_val _tmp%zu = aut_int(aut_expect_int(\"/\", _tmp%zu) / aut_expect_int(\"/\", _tmp%zu));\n",tmp,lt,rt);
        else if(strcmp(op,"==")==0)E(g,"    aut_val _tmp%zu = aut_int(aut_eq(_tmp%zu, _tmp%zu));\n",tmp,lt,rt);
        else if(strcmp(op,"!=")==0)E(g,"    aut_val _tmp%zu = aut_int(!aut_eq(_tmp%zu, _tmp%zu));\n",tmp,lt,rt);
        else if(strcmp(op,"<")==0) E(g,"    aut_val _tmp%zu = aut_int(aut_expect_int(\"<\", _tmp%zu) < aut_expect_int(\"<\", _tmp%zu));\n",tmp,lt,rt);
        else if(strcmp(op,">")==0) E(g,"    aut_val _tmp%zu = aut_int(aut_expect_int(\">\", _tmp%zu) > aut_expect_int(\">\", _tmp%zu));\n",tmp,lt,rt);
        else if(strcmp(op,"<=")==0)E(g,"    aut_val _tmp%zu = aut_int(aut_expect_int(\"<=\", _tmp%zu) <= aut_expect_int(\"<=\", _tmp%zu));\n",tmp,lt,rt);
        else if(strcmp(op,">=")==0)E(g,"    aut_val _tmp%zu = aut_int(aut_expect_int(\">=\", _tmp%zu) >= aut_expect_int(\">=\", _tmp%zu));\n",tmp,lt,rt);
        else{ERR("unknown op: %s",op);return false;}
        E(g,"    aut_free(_tmp%zu); aut_free(_tmp%zu);\n",lt,rt);
        return true;
    }
    case EK_RANGE:
        ERR("range() can only be used in for-in loop");
        return false;
    }
    return false;
}

static bool cg_expr(CG*g,Expr*e,const Program*prog){
    size_t t=L(g);return cg_expr_into(g,e,prog,t);
}

static bool cg_stmts(CG*g,Stmt**stmts,size_t count,const Program*prog,bool inlp){
    for(size_t i=0;i<count;i++){
        Stmt*s=stmts[i];
        switch(s->kind){
        case SK_PRINT:{
            size_t t=L(g);if(!cg_expr_into(g,s->expr,prog,t))return false;
            E(g,"    aut_print(_tmp%zu);\n",t);
            E(g,"    aut_free(_tmp%zu);\n",t);
            break;
        }
        case SK_ASSIGN:{
            size_t t=L(g);if(!cg_expr_into(g,s->expr,prog,t))return false;
            E(g,"    aut_free(%s); %s = _tmp%zu;\n",s->var,s->var,t);
            break;
        }
        case SK_IF:{
            size_t ct=L(g);if(!cg_expr_into(g,s->cond,prog,ct))return false;
            size_t le=L(g);
            E(g,"    if(aut_truthy(_tmp%zu)) {\n",ct);
            E(g,"    aut_free(_tmp%zu);\n",ct);
            if(!cg_stmts(g,s->then,s->nthen,prog,inlp))return false;
            if(s->nels>0){E(g,"    } else {\n");if(!cg_stmts(g,s->els,s->nels,prog,inlp))return false;}
            E(g,"    } /* end if %zu */\n",le);(void)le;
            break;
        }
        case SK_WHILE:{
            size_t lw=L(g);
            E(g,"    while(1) { /* while_%zu */\n",lw);
            size_t ct=L(g);if(!cg_expr_into(g,s->cond,prog,ct))return false;
            E(g,"        if(!aut_truthy(_tmp%zu)) { aut_free(_tmp%zu); break; }\n",ct,ct);
            E(g,"        aut_free(_tmp%zu);\n",ct);
            if(!cg_stmts(g,s->loop,s->nloop,prog,true))return false;
            E(g,"    } /* end while_%zu */\n",lw);
            break;
        }
        case SK_FOR:{
            size_t lf=L(g);
            size_t st=L(g),en=L(g),sp=L(g);
            if(!cg_expr_into(g,s->range_start,prog,st))return false;
            if(!cg_expr_into(g,s->range_end,prog,en))return false;
            if(!cg_expr_into(g,s->range_step,prog,sp))return false;
            E(g,"    { /* for_%zu */\n",lf);
            E(g,"        long long _for_start_%zu = aut_expect_int(\"range\", _tmp%zu);\n",lf,st);
            E(g,"        long long _for_end_%zu = aut_expect_int(\"range\", _tmp%zu);\n",lf,en);
            E(g,"        long long _for_step_%zu = aut_expect_int(\"range\", _tmp%zu);\n",lf,sp);
            E(g,"        aut_free(_tmp%zu); aut_free(_tmp%zu); aut_free(_tmp%zu);\n",st,en,sp);
            E(g,"        if(_for_step_%zu == 0) { fprintf(stderr, \"Error: range step cannot be 0\\n\"); exit(1); }\n",lf);
            E(g,"        for(long long _for_i_%zu = _for_start_%zu; ",lf,lf);
            E(g,"(_for_step_%zu > 0) ? (_for_i_%zu < _for_end_%zu) : (_for_i_%zu > _for_end_%zu); ",lf,lf,lf,lf,lf);
            E(g,"_for_i_%zu += _for_step_%zu) {\n",lf,lf);
            E(g,"            aut_free(%s); %s = aut_int(_for_i_%zu);\n",s->loop_var,s->loop_var,lf);
            if(!cg_stmts(g,s->loop,s->nloop,prog,true))return false;
            E(g,"        }\n");
            E(g,"    } /* end for_%zu */\n",lf);
            break;
        }
        case SK_RETURN:{
            if(s->expr){size_t t=L(g);if(!cg_expr_into(g,s->expr,prog,t))return false;E(g,"    _ret = _tmp%zu; goto _return;\n",t);}
            else E(g,"    _ret = aut_int(0); goto _return;\n");
            break;
        }
        case SK_BREAK:
            if(!inlp){ERR("break outside loop");return false;}
            E(g,"    break;\n");break;
        case SK_CONTINUE:
            if(!inlp){ERR("continue outside loop");return false;}
            E(g,"    continue;\n");break;
        case SK_CALL:{
            size_t t=L(g);if(!cg_expr_into(g,s->expr,prog,t))return false;
            E(g,"    aut_free(_tmp%zu);\n",t);break;
        }
        }
    }
    return true;
}

static bool cg_fn(CG*g,FnDef*fn,const Program*prog){
    E(g,"static aut_val _fn_%s(",fn->name);
    for(size_t i=0;i<fn->nparams;i++){if(i)E(g,", ");E(g,"aut_val _%s_arg",fn->params[i]);}
    E(g,") {\n");
    for(size_t i=0;i<fn->nparams;i++)E(g,"    aut_val %s = aut_copy(_%s_arg);\n",fn->params[i],fn->params[i]);

    SList vars;sl_init(&vars);
    struct{Stmt**s;size_t n;}stack[64];int sp=0;
    stack[sp].s=fn->body;stack[sp].n=fn->nbody;sp++;
    while(sp>0){sp--;Stmt**s=stack[sp].s;size_t n=stack[sp].n;
        for(size_t i=0;i<n;i++){
            if(s[i]->kind==SK_ASSIGN){
                bool found=false;for(size_t j=0;j<vars.len;j++)if(strcmp(vars.items[j],s[i]->var)==0){found=true;break;}
                if(!found)for(size_t j=0;j<fn->nparams;j++)if(strcmp(fn->params[j],s[i]->var)==0){found=true;break;}
                if(!found){sl_push(&vars,xdup(s[i]->var));}
            }
            if(s[i]->kind==SK_IF&&sp<63){stack[sp].s=s[i]->then;stack[sp].n=s[i]->nthen;sp++;if(sp<64&&s[i]->nels>0){stack[sp].s=s[i]->els;stack[sp].n=s[i]->nels;sp++;}}
            if(s[i]->kind==SK_WHILE&&sp<64){stack[sp].s=s[i]->loop;stack[sp].n=s[i]->nloop;sp++;}
            if(s[i]->kind==SK_FOR&&sp<64){stack[sp].s=s[i]->loop;stack[sp].n=s[i]->nloop;sp++;}
            if(s[i]->kind==SK_FOR){
                bool found=false;for(size_t j=0;j<vars.len;j++)if(strcmp(vars.items[j],s[i]->loop_var)==0){found=true;break;}
                if(!found)for(size_t j=0;j<fn->nparams;j++)if(strcmp(fn->params[j],s[i]->loop_var)==0){found=true;break;}
                if(!found){sl_push(&vars,xdup(s[i]->loop_var));}
            }
        }
    }
    for(size_t i=0;i<vars.len;i++)E(g,"    aut_val %s = aut_int(0);\n",vars.items[i]);
    E(g,"    aut_val _ret = aut_int(0);\n");
    if(!cg_stmts(g,fn->body,fn->nbody,prog,false)){sl_free(&vars);return false;}
    E(g,"    _ret = aut_int(0);\n");
    E(g,"  _return:\n");
    for(size_t i=0;i<fn->nparams;i++)E(g,"    aut_free(%s);\n",fn->params[i]);
    for(size_t i=0;i<vars.len;i++)E(g,"    aut_free(%s);\n",vars.items[i]);
    sl_free(&vars);
    E(g,"    return _ret;\n}\n\n");
    return true;
}

static const char*RUNTIME =
"#include <stdio.h>\n"
"#include <stdlib.h>\n"
"#include <string.h>\n"
"#include <ctype.h>\n\n"
"/* ---- AutismLang runtime v0.1.0 ---- */\n"
"typedef struct { int type; long long ival; char *sval; } aut_val;\n"
"/* type: 0=int, 1=heap-str, 2=static-str */\n\n"
"static aut_val aut_int(long long v) { aut_val r; r.type=0; r.ival=v; r.sval=NULL; return r; }\n"
"static aut_val aut_str_static(const char *s) { aut_val r; r.type=2; r.ival=0; r.sval=(char*)s; return r; }\n"
"static aut_val aut_str_heap(char *s) { aut_val r; r.type=1; r.ival=0; r.sval=s; return r; }\n"
"static void aut_free(aut_val v) { if(v.type==1) free(v.sval); }\n"
"static aut_val aut_copy(aut_val v) {\n"
"    if(v.type==0) return v;\n"
"    char *s = strdup(v.sval ? v.sval : \"\");\n"
"    return aut_str_heap(s);\n"
"}\n"
"static const char* aut_type_name(aut_val v) { return v.type==0 ? \"int\" : \"str\"; }\n"
"static long long aut_expect_int(const char *op, aut_val v) {\n"
"    if(v.type!=0) {\n"
"        fprintf(stderr, \"TypeError: operator %s expects int, got %s\\n\", op, aut_type_name(v));\n"
"        exit(1);\n"
"    }\n"
"    return v.ival;\n"
"}\n"
"static int aut_truthy(aut_val v) {\n"
"    if(v.type==0) return v.ival!=0;\n"
"    return v.sval && v.sval[0]!=0;\n"
"}\n"
"static int aut_eq(aut_val a, aut_val b) {\n"
"    if(a.type==0 && b.type==0) return a.ival==b.ival;\n"
"    if(a.type!=0 && b.type!=0) return strcmp(a.sval?a.sval:\"\", b.sval?b.sval:\"\")==0;\n"
"    return 0;\n"
"}\n"
"static aut_val aut_add(aut_val a, aut_val b) {\n"
"    if(a.type==0 && b.type==0) return aut_int(a.ival+b.ival);\n"
"    if(a.type!=0 && b.type!=0) {\n"
"        const char *sa = (a.type!=0 && a.sval) ? a.sval : \"\";\n"
"        const char *sb = (b.type!=0 && b.sval) ? b.sval : \"\";\n"
"        size_t la=strlen(sa), lb=strlen(sb);\n"
"        char *r=malloc(la+lb+1); memcpy(r,sa,la); memcpy(r+la,sb,lb); r[la+lb]=0;\n"
"        return aut_str_heap(r);\n"
"    }\n"
"    fprintf(stderr, \"TypeError: cannot add int and str\\n\");\n"
"    exit(1);\n"
"}\n"
"static void aut_print(aut_val v) {\n"
"    if(v.type==0) printf(\"%lld\\n\", v.ival);\n"
"    else printf(\"%s\\n\", v.sval ? v.sval : \"\");\n"
"    fflush(stdout);\n"
"}\n"
"static aut_val aut_input(void) {\n"
"    fflush(stdout);\n"
"    char buf[4096]; buf[0]=0;\n"
"    if(fgets(buf, sizeof(buf), stdin)) {\n"
"        size_t n=strlen(buf);\n"
"        while(n>0&&(buf[n-1]=='\\n'||buf[n-1]=='\\r'))buf[--n]=0;\n"
"    }\n"
"    return aut_str_heap(strdup(buf));\n"
"}\n"
"/* v0.1.0: type conversion functions */\n"
"static aut_val aut_to_int(aut_val v) {\n"
"    if(v.type==0) return v;\n"
"    long long result = 0;\n"
"    const char *s = v.sval ? v.sval : \"\";\n"
"    while(*s && isspace((unsigned char)*s)) s++;\n"
"    int neg = 0;\n"
"    if(*s == '-') { neg = 1; s++; }\n"
"    else if(*s == '+') s++;\n"
"    while(*s >= '0' && *s <= '9') {\n"
"        result = result * 10 + (*s - '0');\n"
"        s++;\n"
"    }\n"
"    return aut_int(neg ? -result : result);\n"
"}\n"
"static aut_val aut_to_str(aut_val v) {\n"
"    if(v.type!=0) return aut_copy(v);\n"
"    char buf[32];\n"
"    snprintf(buf, sizeof(buf), \"%lld\", v.ival);\n"
"    return aut_str_heap(strdup(buf));\n"
"}\n\n";

static bool codegen(const Program*prog,const char*out_path){
    CG g;cg_init(&g);
    sb_fmt(&g.out,"/* AutismLang v%s | backend=%s */\n",AUTISMLANG_VERSION,AUTISMLANG_BACKEND);
    sb_cat(&g.out,RUNTIME);
    /* forward declarations */
    for(size_t i=0;i<prog->nfns;i++){
        E(&g,"static aut_val _fn_%s(",prog->fns[i].name);
        for(size_t j=0;j<prog->fns[i].nparams;j++){if(j)E(&g,", ");E(&g,"aut_val");}
        E(&g,");\n");
    }
    sb_cat(&g.out,"\n");
    /* generate functions */
    for(size_t i=0;i<prog->nfns;i++){
        if(!cg_fn(&g,(FnDef*)&prog->fns[i],prog)){
            fprintf(stderr,"Codegen error in '%s': %s\n",prog->fns[i].name,g_err);
            cg_free(&g);return false;
        }
    }
    /* main */
    sb_cat(&g.out,"int main(void) { _fn_main(); return 0; }\n");

    /* write */
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
            printf("Backend: %s\n",AUTISMLANG_BACKEND);
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
    if(!codegen(&prog,out)){free_program(&prog);sl_free(&lines);return 1;}
    printf("Compiled %s -> %s\n",in,out);
    free_program(&prog);sl_free(&lines);return 0;
}