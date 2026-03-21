#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define MKDIR(path) mkdir((path), 0755)
#endif

#define INDENT "    "

typedef struct {
    char **items;
    size_t len;
    size_t cap;
} StringList;

typedef enum {
    VALUE_INT,
    VALUE_STR,
} ValueType;

typedef struct {
    ValueType type;
    long long int_value;
    char *str_value;
} Value;

typedef struct {
    char *name;
    Value value;
} Variable;

typedef struct {
    Variable *items;
    size_t len;
    size_t cap;
} VarTable;

typedef struct {
    char *name;
    StringList params;
    size_t body_start;
    size_t body_end;
} FunctionDef;

typedef struct {
    FunctionDef *items;
    size_t len;
    size_t cap;
} FunctionTable;

static char *x_strdup(const char *src);

static void list_init(StringList *list) {
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
}

static void list_free(StringList *list) {
    size_t i;
    for (i = 0; i < list->len; i++) {
        free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
}

static bool list_push(StringList *list, char *value) {
    if (list->len == list->cap) {
        size_t next_cap = list->cap == 0 ? 8 : list->cap * 2;
        char **next_items = (char **)realloc(list->items, next_cap * sizeof(char *));
        if (!next_items) {
            return false;
        }
        list->items = next_items;
        list->cap = next_cap;
    }
    list->items[list->len++] = value;
    return true;
}

static void value_init(Value *value) {
    value->type = VALUE_INT;
    value->int_value = 0;
    value->str_value = NULL;
}

static void value_free(Value *value) {
    if (value->type == VALUE_STR) {
        free(value->str_value);
        value->str_value = NULL;
    }
}

static bool value_set_int(Value *value, long long n) {
    value_free(value);
    value->type = VALUE_INT;
    value->int_value = n;
    return true;
}

static bool value_set_str(Value *value, const char *text) {
    char *copy = x_strdup(text);
    if (!copy) {
        return false;
    }
    value_free(value);
    value->type = VALUE_STR;
    value->str_value = copy;
    return true;
}

static bool value_clone(const Value *src, Value *dst) {
    if (src->type == VALUE_INT) {
        return value_set_int(dst, src->int_value);
    }
    return value_set_str(dst, src->str_value ? src->str_value : "");
}

static bool value_to_printable(const Value *value, char **out_text) {
    if (value->type == VALUE_STR) {
        *out_text = x_strdup(value->str_value ? value->str_value : "");
        return *out_text != NULL;
    }

    {
        char buf[64];
        int written = snprintf(buf, sizeof(buf), "%lld", value->int_value);
        if (written < 0 || (size_t)written >= sizeof(buf)) {
            return false;
        }
        *out_text = x_strdup(buf);
        return *out_text != NULL;
    }
}

static void vars_init(VarTable *vars) {
    vars->items = NULL;
    vars->len = 0;
    vars->cap = 0;
}

static void vars_free(VarTable *vars) {
    size_t i;
    for (i = 0; i < vars->len; i++) {
        free(vars->items[i].name);
        value_free(&vars->items[i].value);
    }
    free(vars->items);
    vars->items = NULL;
    vars->len = 0;
    vars->cap = 0;
}

static Variable *vars_find(VarTable *vars, const char *name) {
    size_t i;
    for (i = 0; i < vars->len; i++) {
        if (strcmp(vars->items[i].name, name) == 0) {
            return &vars->items[i];
        }
    }
    return NULL;
}

static bool vars_set(VarTable *vars, const char *name, const Value *value) {
    Variable *existing = vars_find(vars, name);
    if (existing) {
        return value_clone(value, &existing->value);
    }

    if (vars->len == vars->cap) {
        size_t next_cap = vars->cap == 0 ? 8 : vars->cap * 2;
        Variable *next_items = (Variable *)realloc(vars->items, next_cap * sizeof(Variable));
        if (!next_items) {
            return false;
        }
        vars->items = next_items;
        vars->cap = next_cap;
    }

    vars->items[vars->len].name = x_strdup(name);
    if (!vars->items[vars->len].name) {
        return false;
    }

    value_init(&vars->items[vars->len].value);
    if (!value_clone(value, &vars->items[vars->len].value)) {
        free(vars->items[vars->len].name);
        vars->items[vars->len].name = NULL;
        return false;
    }

    vars->len++;
    return true;
}

static const Value *vars_get(const VarTable *vars, const char *name) {
    size_t i;
    for (i = 0; i < vars->len; i++) {
        if (strcmp(vars->items[i].name, name) == 0) {
            return &vars->items[i].value;
        }
    }
    return NULL;
}

static void functions_init(FunctionTable *table) {
    table->items = NULL;
    table->len = 0;
    table->cap = 0;
}

static void function_def_free(FunctionDef *fn) {
    free(fn->name);
    fn->name = NULL;
    list_free(&fn->params);
    fn->body_start = 0;
    fn->body_end = 0;
}

static void functions_free(FunctionTable *table) {
    size_t i;
    for (i = 0; i < table->len; i++) {
        function_def_free(&table->items[i]);
    }
    free(table->items);
    table->items = NULL;
    table->len = 0;
    table->cap = 0;
}

static const FunctionDef *functions_find(const FunctionTable *table, const char *name) {
    size_t i;
    for (i = 0; i < table->len; i++) {
        if (strcmp(table->items[i].name, name) == 0) {
            return &table->items[i];
        }
    }
    return NULL;
}

static bool functions_push(FunctionTable *table, const FunctionDef *src) {
    FunctionDef *dst;
    size_t i;

    if (table->len == table->cap) {
        size_t next_cap = table->cap == 0 ? 8 : table->cap * 2;
        FunctionDef *next_items = (FunctionDef *)realloc(table->items, next_cap * sizeof(FunctionDef));
        if (!next_items) {
            return false;
        }
        table->items = next_items;
        table->cap = next_cap;
    }

    dst = &table->items[table->len];
    dst->name = x_strdup(src->name);
    if (!dst->name) {
        return false;
    }
    list_init(&dst->params);
    for (i = 0; i < src->params.len; i++) {
        char *param = x_strdup(src->params.items[i]);
        if (!param || !list_push(&dst->params, param)) {
            free(param);
            function_def_free(dst);
            return false;
        }
    }
    dst->body_start = src->body_start;
    dst->body_end = src->body_end;

    table->len++;
    return true;
}

static char *x_strdup(const char *src) {
    size_t n = strlen(src);
    char *copy = (char *)malloc(n + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, src, n + 1);
    return copy;
}

static void compile_error(size_t line_no, const char *message) {
    if (line_no > 0) {
        fprintf(stderr, "Compile error: line %zu: %s\n", line_no, message);
    } else {
        fprintf(stderr, "Compile error: %s\n", message);
    }
}

static char *read_text_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    char *buf;
    long size;

    if (!f) {
        fprintf(stderr, "Failed to open input '%s': %s\n", path, strerror(errno));
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        fprintf(stderr, "Failed to seek input '%s'\n", path);
        return NULL;
    }

    size = ftell(f);
    if (size < 0) {
        fclose(f);
        fprintf(stderr, "Failed to read size for '%s'\n", path);
        return NULL;
    }

    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        fprintf(stderr, "Failed to rewind input '%s'\n", path);
        return NULL;
    }

    buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        fprintf(stderr, "Out of memory while reading '%s'\n", path);
        return NULL;
    }

    if (size > 0) {
        size_t read_count = fread(buf, 1, (size_t)size, f);
        if (read_count != (size_t)size) {
            free(buf);
            fclose(f);
            fprintf(stderr, "Failed to read input '%s'\n", path);
            return NULL;
        }
    }

    buf[size] = '\0';
    fclose(f);
    *out_size = (size_t)size;
    return buf;
}

static bool split_lines(const char *text, StringList *lines) {
    const char *start = text;
    const char *p = text;

    while (*p) {
        if (*p == '\n') {
            size_t line_len = (size_t)(p - start);
            if (line_len > 0 && start[line_len - 1] == '\r') {
                line_len--;
            }
            {
                char *line = (char *)malloc(line_len + 1);
                if (!line) {
                    return false;
                }
                memcpy(line, start, line_len);
                line[line_len] = '\0';
                if (!list_push(lines, line)) {
                    free(line);
                    return false;
                }
            }
            start = p + 1;
        }
        p++;
    }

    if (start != p) {
        size_t line_len = (size_t)(p - start);
        if (line_len > 0 && start[line_len - 1] == '\r') {
            line_len--;
        }
        {
            char *line = (char *)malloc(line_len + 1);
            if (!line) {
                return false;
            }
            memcpy(line, start, line_len);
            line[line_len] = '\0';
            if (!list_push(lines, line)) {
                free(line);
                return false;
            }
        }
    }

    return true;
}

static void rtrim_inplace(char *s) {
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        s[n - 1] = '\0';
        n--;
    }
}

static const char *ltrim_ptr(const char *s) {
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    return s;
}

static bool is_blank_or_comment(const char *line) {
    const char *p = ltrim_ptr(line);
    return *p == '\0' || *p == '#';
}

static bool is_ident_start(char c) {
    return isalpha((unsigned char)c) || c == '_';
}

static bool is_ident_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

static void trim_view(const char **start, const char **end) {
    while (*start < *end && isspace((unsigned char)**start)) {
        (*start)++;
    }
    while (*end > *start && isspace((unsigned char)*((*end) - 1))) {
        (*end)--;
    }
}

static bool parse_identifier_view(const char *start, const char *end, char *out, size_t out_cap) {
    size_t i = 0;
    const char *p = start;

    if (p >= end || !is_ident_start(*p)) {
        return false;
    }

    while (p < end && is_ident_char(*p)) {
        if (i + 1 >= out_cap) {
            return false;
        }
        out[i++] = *p;
        p++;
    }

    if (p != end) {
        return false;
    }

    out[i] = '\0';
    return true;
}

static void strip_inline_comment_inplace(char *s) {
    bool in_string = false;
    bool escape = false;
    size_t i;

    for (i = 0; s[i] != '\0'; i++) {
        if (s[i] == '\\' && in_string && !escape) {
            escape = true;
            continue;
        }
        if (s[i] == '"' && !escape) {
            in_string = !in_string;
        }
        if (s[i] == '#' && !in_string) {
            s[i] = '\0';
            break;
        }
        if (escape) {
            escape = false;
        }
    }

    rtrim_inplace(s);
}

static bool parse_fn_header(const char *line, char *name_out, size_t name_cap, StringList *params, char *err_buf, size_t err_cap) {
    const char *p = line;
    size_t i = 0;

    list_init(params);

    if (strncmp(p, "fn", 2) != 0 || !isspace((unsigned char)p[2])) {
        snprintf(err_buf, err_cap, "expected function definition");
        return false;
    }
    p += 2;

    while (*p && isspace((unsigned char)*p)) {
        p++;
    }

    if (!is_ident_start(*p)) {
        snprintf(err_buf, err_cap, "invalid function name");
        return false;
    }

    while (*p && is_ident_char(*p)) {
        if (i + 1 >= name_cap) {
            snprintf(err_buf, err_cap, "function name too long");
            return false;
        }
        name_out[i++] = *p;
        p++;
    }
    name_out[i] = '\0';

    while (*p && isspace((unsigned char)*p)) {
        p++;
    }

    if (*p != '(') {
        snprintf(err_buf, err_cap, "expected '(' after function name");
        return false;
    }
    p++;

    while (*p && *p != ')') {
        char ident[128];
        size_t n = 0;

        while (*p && isspace((unsigned char)*p)) {
            p++;
        }

        if (!is_ident_start(*p)) {
            list_free(params);
            snprintf(err_buf, err_cap, "invalid parameter name");
            return false;
        }

        while (*p && is_ident_char(*p)) {
            if (n + 1 >= sizeof(ident)) {
                list_free(params);
                snprintf(err_buf, err_cap, "parameter name too long");
                return false;
            }
            ident[n++] = *p;
            p++;
        }
        ident[n] = '\0';

        {
            char *param_copy = x_strdup(ident);
            if (!param_copy || !list_push(params, param_copy)) {
                free(param_copy);
                list_free(params);
                snprintf(err_buf, err_cap, "out of memory");
                return false;
            }
        }

        while (*p && isspace((unsigned char)*p)) {
            p++;
        }

        if (*p == ',') {
            p++;
            continue;
        }
        if (*p != ')') {
            list_free(params);
            snprintf(err_buf, err_cap, "expected ',' or ')' in parameter list");
            return false;
        }
    }

    if (*p != ')') {
        list_free(params);
        snprintf(err_buf, err_cap, "missing ')' in function header");
        return false;
    }
    p++;

    while (*p && isspace((unsigned char)*p)) {
        p++;
    }

    if (*p != ':') {
        list_free(params);
        snprintf(err_buf, err_cap, "expected ':' at end of function header");
        return false;
    }
    p++;

    while (*p && isspace((unsigned char)*p)) {
        p++;
    }

    if (*p != '\0') {
        list_free(params);
        snprintf(err_buf, err_cap, "unexpected tokens after function header");
        return false;
    }

    return true;
}

static bool append_char(char **buf, size_t *len, size_t *cap, char c) {
    if (*len + 1 >= *cap) {
        size_t next_cap = *cap == 0 ? 16 : *cap * 2;
        char *next = (char *)realloc(*buf, next_cap);
        if (!next) {
            return false;
        }
        *buf = next;
        *cap = next_cap;
    }
    (*buf)[(*len)++] = c;
    return true;
}

static bool parse_string_view(const char *start, const char *end, char **out_text, char *err_buf, size_t err_cap) {
    const char *p = start;
    char *decoded = NULL;
    size_t len = 0;
    size_t cap = 0;

    if (p >= end || *p != '"' || end - start < 2 || *(end - 1) != '"') {
        snprintf(err_buf, err_cap, "string literal must use double quotes");
        return false;
    }

    p++;
    while (p < end - 1) {
        if (*p == '\\') {
            char next;
            if (p + 1 >= end - 1) {
                free(decoded);
                snprintf(err_buf, err_cap, "invalid trailing escape in string");
                return false;
            }
            next = p[1];
            if (next == 'n') {
                if (!append_char(&decoded, &len, &cap, '\n')) {
                    free(decoded);
                    snprintf(err_buf, err_cap, "out of memory");
                    return false;
                }
            } else if (next == 't') {
                if (!append_char(&decoded, &len, &cap, '\t')) {
                    free(decoded);
                    snprintf(err_buf, err_cap, "out of memory");
                    return false;
                }
            } else if (next == 'r') {
                if (!append_char(&decoded, &len, &cap, '\r')) {
                    free(decoded);
                    snprintf(err_buf, err_cap, "out of memory");
                    return false;
                }
            } else if (next == '\\' || next == '"') {
                if (!append_char(&decoded, &len, &cap, next)) {
                    free(decoded);
                    snprintf(err_buf, err_cap, "out of memory");
                    return false;
                }
            } else {
                free(decoded);
                snprintf(err_buf, err_cap, "unsupported escape sequence: \\%c", next);
                return false;
            }
            p += 2;
            continue;
        }

        if (!append_char(&decoded, &len, &cap, *p)) {
            free(decoded);
            snprintf(err_buf, err_cap, "out of memory");
            return false;
        }
        p++;
    }

    if (!append_char(&decoded, &len, &cap, '\0')) {
        free(decoded);
        snprintf(err_buf, err_cap, "out of memory");
        return false;
    }

    *out_text = decoded;
    return true;
}

typedef struct {
    const char *p;
    const VarTable *vars;
    char *err_buf;
    size_t err_cap;
} ExprParser;

static void expr_skip_spaces(ExprParser *parser) {
    while (*parser->p && isspace((unsigned char)*parser->p)) {
        parser->p++;
    }
}

static bool expr_parse_sum(ExprParser *parser, Value *out);

static bool value_concat_inplace(Value *left, const Value *right, char *err_buf, size_t err_cap) {
    size_t left_len = strlen(left->str_value ? left->str_value : "");
    size_t right_len = strlen(right->str_value ? right->str_value : "");
    char *joined = (char *)malloc(left_len + right_len + 1);
    if (!joined) {
        snprintf(err_buf, err_cap, "out of memory");
        return false;
    }
    memcpy(joined, left->str_value ? left->str_value : "", left_len);
    memcpy(joined + left_len, right->str_value ? right->str_value : "", right_len);
    joined[left_len + right_len] = '\0';
    free(left->str_value);
    left->str_value = joined;
    return true;
}

static bool expr_parse_factor(ExprParser *parser, Value *out) {
    expr_skip_spaces(parser);
    value_init(out);

    if (*parser->p == '\0') {
        snprintf(parser->err_buf, parser->err_cap, "unexpected end of expression");
        return false;
    }

    if (*parser->p == '(') {
        parser->p++;
        if (!expr_parse_sum(parser, out)) {
            return false;
        }
        expr_skip_spaces(parser);
        if (*parser->p != ')') {
            value_free(out);
            snprintf(parser->err_buf, parser->err_cap, "missing ')' in expression");
            return false;
        }
        parser->p++;
        return true;
    }

    if (*parser->p == '"') {
        const char *start = parser->p;
        bool escape = false;
        char *decoded = NULL;

        parser->p++;
        while (*parser->p) {
            if (*parser->p == '\\' && !escape) {
                escape = true;
                parser->p++;
                continue;
            }
            if (*parser->p == '"' && !escape) {
                parser->p++;
                break;
            }
            if (escape) {
                escape = false;
            }
            parser->p++;
        }

        if (parser->p[-1] != '"') {
            snprintf(parser->err_buf, parser->err_cap, "unterminated string literal");
            return false;
        }

        if (!parse_string_view(start, parser->p, &decoded, parser->err_buf, parser->err_cap)) {
            return false;
        }
        if (!value_set_str(out, decoded)) {
            free(decoded);
            snprintf(parser->err_buf, parser->err_cap, "out of memory");
            return false;
        }
        free(decoded);
        return true;
    }

    if (*parser->p == '-' && isdigit((unsigned char)parser->p[1])) {
        char *end_num = NULL;
        long long v = strtoll(parser->p, &end_num, 10);
        if (end_num == parser->p) {
            snprintf(parser->err_buf, parser->err_cap, "invalid number");
            return false;
        }
        parser->p = end_num;
        return value_set_int(out, v);
    }

    if (isdigit((unsigned char)*parser->p)) {
        char *end_num = NULL;
        long long v = strtoll(parser->p, &end_num, 10);
        if (end_num == parser->p) {
            snprintf(parser->err_buf, parser->err_cap, "invalid number");
            return false;
        }
        parser->p = end_num;
        return value_set_int(out, v);
    }

    if (is_ident_start(*parser->p)) {
        char ident[128];
        size_t n = 0;
        const Value *stored;

        while (*parser->p && is_ident_char(*parser->p)) {
            if (n + 1 >= sizeof(ident)) {
                snprintf(parser->err_buf, parser->err_cap, "identifier too long");
                return false;
            }
            ident[n++] = *parser->p;
            parser->p++;
        }
        ident[n] = '\0';

        stored = vars_get(parser->vars, ident);
        if (!stored) {
            snprintf(parser->err_buf, parser->err_cap, "undefined variable: %.90s", ident);
            return false;
        }
        if (!value_clone(stored, out)) {
            snprintf(parser->err_buf, parser->err_cap, "out of memory");
            return false;
        }
        return true;
    }

    snprintf(parser->err_buf, parser->err_cap, "invalid expression term");
    return false;
}

static bool expr_parse_product(ExprParser *parser, Value *out) {
    Value right;

    if (!expr_parse_factor(parser, out)) {
        return false;
    }

    while (1) {
        char op;
        expr_skip_spaces(parser);
        op = *parser->p;
        if (op != '*' && op != '/') {
            break;
        }
        parser->p++;

        value_init(&right);
        if (!expr_parse_factor(parser, &right)) {
            value_free(out);
            return false;
        }

        if (out->type != VALUE_INT || right.type != VALUE_INT) {
            value_free(&right);
            value_free(out);
            snprintf(parser->err_buf, parser->err_cap, "'*' and '/' require integer operands");
            return false;
        }

        if (op == '*') {
            out->int_value *= right.int_value;
        } else {
            if (right.int_value == 0) {
                value_free(&right);
                value_free(out);
                snprintf(parser->err_buf, parser->err_cap, "division by zero");
                return false;
            }
            out->int_value /= right.int_value;
        }

        value_free(&right);
    }

    return true;
}

static bool expr_parse_sum(ExprParser *parser, Value *out) {
    Value right;

    if (!expr_parse_product(parser, out)) {
        return false;
    }

    while (1) {
        char op;
        expr_skip_spaces(parser);
        op = *parser->p;
        if (op != '+' && op != '-') {
            break;
        }
        parser->p++;

        value_init(&right);
        if (!expr_parse_product(parser, &right)) {
            value_free(out);
            return false;
        }

        if (op == '+') {
            if (out->type == VALUE_INT && right.type == VALUE_INT) {
                out->int_value += right.int_value;
            } else if (out->type == VALUE_STR && right.type == VALUE_STR) {
                if (!value_concat_inplace(out, &right, parser->err_buf, parser->err_cap)) {
                    value_free(&right);
                    value_free(out);
                    return false;
                }
            } else {
                value_free(&right);
                value_free(out);
                snprintf(parser->err_buf, parser->err_cap, "'+' requires int+int or str+str");
                return false;
            }
        } else {
            if (out->type != VALUE_INT || right.type != VALUE_INT) {
                value_free(&right);
                value_free(out);
                snprintf(parser->err_buf, parser->err_cap, "'-' requires integer operands");
                return false;
            }
            out->int_value -= right.int_value;
        }

        value_free(&right);
    }

    return true;
}

static bool eval_expression(const char *expr, const VarTable *vars, Value *out, char *err_buf, size_t err_cap) {
    ExprParser parser;

    parser.p = expr;
    parser.vars = vars;
    parser.err_buf = err_buf;
    parser.err_cap = err_cap;

    if (!expr_parse_sum(&parser, out)) {
        return false;
    }

    expr_skip_spaces(&parser);
    if (*parser.p != '\0') {
        value_free(out);
        snprintf(err_buf, err_cap, "unexpected token in expression");
        return false;
    }

    return true;
}

static bool value_is_truthy(const Value *value) {
    if (value->type == VALUE_INT) {
        return value->int_value != 0;
    }
    return value->str_value != NULL && value->str_value[0] != '\0';
}

static bool eval_condition(const char *cond_expr, const VarTable *vars, bool *out_result, char *err_buf, size_t err_cap) {
    static const char *ops[] = {"==", "!=", "<=", ">=", "<", ">"};
    static const size_t op_count = sizeof(ops) / sizeof(ops[0]);
    const char *op_pos = NULL;
    const char *op_text = NULL;
    size_t i;
    bool in_string = false;
    bool escape = false;
    const char *p = cond_expr;

    while (*p) {
        if (*p == '\\' && in_string && !escape) {
            escape = true;
            p++;
            continue;
        }
        if (*p == '"' && !escape) {
            in_string = !in_string;
        }
        if (!in_string) {
            for (i = 0; i < op_count; i++) {
                size_t n = strlen(ops[i]);
                if (strncmp(p, ops[i], n) == 0) {
                    op_pos = p;
                    op_text = ops[i];
                    break;
                }
            }
            if (op_pos) {
                break;
            }
        }
        if (escape) {
            escape = false;
        }
        p++;
    }

    if (!op_pos) {
        Value only;
        value_init(&only);
        if (!eval_expression(cond_expr, vars, &only, err_buf, err_cap)) {
            return false;
        }
        *out_result = value_is_truthy(&only);
        value_free(&only);
        return true;
    }

    {
        size_t op_len = strlen(op_text);
        size_t left_len = (size_t)(op_pos - cond_expr);
        const char *right_start = op_pos + op_len;
        size_t right_len = strlen(right_start);
        char *left_expr = (char *)malloc(left_len + 1);
        char *right_expr = (char *)malloc(right_len + 1);
        Value left;
        Value right;
        bool ok = false;

        value_init(&left);
        value_init(&right);

        if (!left_expr || !right_expr) {
            free(left_expr);
            free(right_expr);
            snprintf(err_buf, err_cap, "out of memory");
            return false;
        }

        memcpy(left_expr, cond_expr, left_len);
        left_expr[left_len] = '\0';
        memcpy(right_expr, right_start, right_len + 1);

        if (!eval_expression(left_expr, vars, &left, err_buf, err_cap)) {
            goto cleanup_cond;
        }
        if (!eval_expression(right_expr, vars, &right, err_buf, err_cap)) {
            goto cleanup_cond;
        }

        if (left.type != right.type) {
            snprintf(err_buf, err_cap, "condition compares different types");
            goto cleanup_cond;
        }

        if (left.type == VALUE_INT) {
            if (strcmp(op_text, "==") == 0) {
                *out_result = left.int_value == right.int_value;
            } else if (strcmp(op_text, "!=") == 0) {
                *out_result = left.int_value != right.int_value;
            } else if (strcmp(op_text, "<=") == 0) {
                *out_result = left.int_value <= right.int_value;
            } else if (strcmp(op_text, ">=") == 0) {
                *out_result = left.int_value >= right.int_value;
            } else if (strcmp(op_text, "<") == 0) {
                *out_result = left.int_value < right.int_value;
            } else {
                *out_result = left.int_value > right.int_value;
            }
        } else {
            int cmp = strcmp(left.str_value ? left.str_value : "", right.str_value ? right.str_value : "");
            if (strcmp(op_text, "==") == 0) {
                *out_result = cmp == 0;
            } else if (strcmp(op_text, "!=") == 0) {
                *out_result = cmp != 0;
            } else {
                snprintf(err_buf, err_cap, "string condition supports only == and !=");
                goto cleanup_cond;
            }
        }

        ok = true;

cleanup_cond:
        free(left_expr);
        free(right_expr);
        value_free(&left);
        value_free(&right);
        return ok;
    }
}

static bool parse_block_header(const char *stmt, const char *keyword, char **out_expr, char *err_buf, size_t err_cap) {
    const char *p = stmt;
    const char *expr_start;
    const char *expr_end;
    size_t key_len = strlen(keyword);
    size_t len;
    char *copy;

    *out_expr = NULL;

    if (strncmp(p, keyword, key_len) != 0 || !isspace((unsigned char)p[key_len])) {
        return false;
    }
    p += key_len;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }

    expr_start = p;
    expr_end = stmt + strlen(stmt);
    while (expr_end > expr_start && isspace((unsigned char)*(expr_end - 1))) {
        expr_end--;
    }

    if (expr_end <= expr_start || *(expr_end - 1) != ':') {
        snprintf(err_buf, err_cap, "%s statement must end with ':'", keyword);
        return false;
    }

    expr_end--;
    while (expr_end > expr_start && isspace((unsigned char)*(expr_end - 1))) {
        expr_end--;
    }

    if (expr_end <= expr_start) {
        snprintf(err_buf, err_cap, "%s condition cannot be empty", keyword);
        return false;
    }

    len = (size_t)(expr_end - expr_start);
    copy = (char *)malloc(len + 1);
    if (!copy) {
        snprintf(err_buf, err_cap, "out of memory");
        return false;
    }
    memcpy(copy, expr_start, len);
    copy[len] = '\0';
    *out_expr = copy;
    return true;
}

static bool line_has_indent(const char *raw, size_t indent) {
    size_t k;
    for (k = 0; k < indent; k++) {
        if (raw[k] != ' ') {
            return false;
        }
    }
    return true;
}

static size_t find_block_end(const StringList *lines, size_t start, size_t limit, size_t indent) {
    size_t i = start;
    while (i < limit) {
        char *copy = x_strdup(lines->items[i]);
        if (!copy) {
            return i;
        }
        rtrim_inplace(copy);
        if (is_blank_or_comment(copy)) {
            free(copy);
            i++;
            continue;
        }
        free(copy);
        if (strlen(lines->items[i]) < indent || !line_has_indent(lines->items[i], indent)) {
            break;
        }
        i++;
    }
    return i;
}

static bool parse_call_stmt(const char *stmt, char *name_out, size_t name_cap, StringList *arg_exprs, char *err_buf, size_t err_cap) {
    const char *p = stmt;
    const char *args_start;
    const char *args_end;
    bool in_string = false;
    bool escape = false;
    int depth = 0;
    size_t n = 0;

    list_init(arg_exprs);

    if (!is_ident_start(*p)) {
        return false;
    }
    while (*p && is_ident_char(*p)) {
        if (n + 1 >= name_cap) {
            snprintf(err_buf, err_cap, "function name too long");
            return false;
        }
        name_out[n++] = *p;
        p++;
    }
    name_out[n] = '\0';

    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (*p != '(') {
        return false;
    }
    p++;
    args_start = p;

    while (*p) {
        if (*p == '\\' && in_string && !escape) {
            escape = true;
            p++;
            continue;
        }
        if (*p == '"' && !escape) {
            in_string = !in_string;
        }
        if (!in_string) {
            if (*p == '(') {
                depth++;
            } else if (*p == ')') {
                if (depth == 0) {
                    break;
                }
                depth--;
            }
        }
        if (escape) {
            escape = false;
        }
        p++;
    }

    if (*p != ')') {
        list_free(arg_exprs);
        snprintf(err_buf, err_cap, "expected ')' in function call");
        return false;
    }
    args_end = p;
    p++;

    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (*p != '\0') {
        list_free(arg_exprs);
        snprintf(err_buf, err_cap, "unexpected tokens after function call");
        return false;
    }

    {
        const char *seg = args_start;
        const char *q = args_start;
        in_string = false;
        escape = false;
        depth = 0;

        while (q <= args_end) {
            bool split = false;
            if (q == args_end) {
                split = true;
            } else {
                if (*q == '\\' && in_string && !escape) {
                    escape = true;
                    q++;
                    continue;
                }
                if (*q == '"' && !escape) {
                    in_string = !in_string;
                }
                if (!in_string) {
                    if (*q == '(') {
                        depth++;
                    } else if (*q == ')') {
                        if (depth > 0) {
                            depth--;
                        }
                    } else if (*q == ',' && depth == 0) {
                        split = true;
                    }
                }
                if (escape) {
                    escape = false;
                }
            }

            if (split) {
                const char *a = seg;
                const char *b = q;
                trim_view(&a, &b);
                if (a < b) {
                    size_t len = (size_t)(b - a);
                    char *arg = (char *)malloc(len + 1);
                    if (!arg) {
                        list_free(arg_exprs);
                        snprintf(err_buf, err_cap, "out of memory");
                        return false;
                    }
                    memcpy(arg, a, len);
                    arg[len] = '\0';
                    if (!list_push(arg_exprs, arg)) {
                        free(arg);
                        list_free(arg_exprs);
                        snprintf(err_buf, err_cap, "out of memory");
                        return false;
                    }
                }
                seg = q + 1;
            }
            q++;
        }
    }

    return true;
}

static bool parse_print_stmt(const char *stmt, const VarTable *vars, char **out_text, char *err_buf, size_t err_cap) {
    const char *p = stmt;
    const char *arg_start;
    const char *arg_end;
    bool in_string = false;
    bool escape = false;
    char *expr_copy = NULL;
    Value value;

    if (strncmp(p, "print", 5) != 0) {
        snprintf(err_buf, err_cap, "unknown statement");
        return false;
    }
    p += 5;

    while (*p && isspace((unsigned char)*p)) {
        p++;
    }

    if (*p != '(') {
        snprintf(err_buf, err_cap, "expected '(' after print");
        return false;
    }
    p++;
    arg_start = p;

    while (*p) {
        if (*p == '\\' && in_string && !escape) {
            escape = true;
            p++;
            continue;
        }
        if (*p == '"' && !escape) {
            in_string = !in_string;
        }
        if (*p == ')' && !in_string) {
            break;
        }
        if (escape) {
            escape = false;
        }
        p++;
    }
    if (*p != ')') {
        snprintf(err_buf, err_cap, "expected ')' after print argument");
        return false;
    }
    arg_end = p;
    p++;

    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (*p != '\0') {
        snprintf(err_buf, err_cap, "unexpected tokens after print statement");
        return false;
    }

    value_init(&value);
    {
        size_t expr_len = (size_t)(arg_end - arg_start);
        expr_copy = (char *)malloc(expr_len + 1);
        if (!expr_copy) {
            snprintf(err_buf, err_cap, "out of memory");
            return false;
        }
        memcpy(expr_copy, arg_start, expr_len);
        expr_copy[expr_len] = '\0';
    }

    if (!eval_expression(expr_copy, vars, &value, err_buf, err_cap)) {
        free(expr_copy);
        return false;
    }
    free(expr_copy);

    if (!value_to_printable(&value, out_text)) {
        value_free(&value);
        snprintf(err_buf, err_cap, "out of memory");
        return false;
    }

    value_free(&value);
    return true;
}

static bool parse_assignment_stmt(const char *stmt, VarTable *vars, char *err_buf, size_t err_cap) {
    const char *p = stmt;
    const char *eq = NULL;
    bool in_string = false;
    bool escape = false;
    const char *lhs_start;
    const char *lhs_end;
    const char *rhs_start;
    const char *rhs_end;
    char ident[128];
    Value value;

    while (*p) {
        if (*p == '\\' && in_string && !escape) {
            escape = true;
            p++;
            continue;
        }
        if (*p == '"' && !escape) {
            in_string = !in_string;
        }
        if (*p == '=' && !in_string) {
            eq = p;
            break;
        }
        if (escape) {
            escape = false;
        }
        p++;
    }

    if (!eq) {
        snprintf(err_buf, err_cap, "unknown statement (use print(...) or name = expression)");
        return false;
    }

    lhs_start = stmt;
    lhs_end = eq;
    trim_view(&lhs_start, &lhs_end);

    if (!parse_identifier_view(lhs_start, lhs_end, ident, sizeof(ident))) {
        snprintf(err_buf, err_cap, "invalid assignment target");
        return false;
    }

    rhs_start = eq + 1;
    rhs_end = stmt + strlen(stmt);
    trim_view(&rhs_start, &rhs_end);

    value_init(&value);
    if (!eval_expression(rhs_start, vars, &value, err_buf, err_cap)) {
        return false;
    }

    if (!vars_set(vars, ident, &value)) {
        value_free(&value);
        snprintf(err_buf, err_cap, "out of memory");
        return false;
    }

    value_free(&value);
    return true;
}

static bool execute_block(
    const StringList *lines,
    size_t *index,
    size_t limit,
    size_t indent,
    const FunctionTable *functions,
    VarTable *vars,
    StringList *main_prints,
    bool emit_output,
    int call_depth,
    char *err_buf,
    size_t err_cap
);

static bool execute_function(
    const StringList *lines,
    const FunctionTable *functions,
    const FunctionDef *fn,
    Value *args,
    size_t arg_count,
    StringList *main_prints,
    bool emit_output,
    int call_depth,
    char *err_buf,
    size_t err_cap
) {
    VarTable locals;
    size_t i;
    size_t idx;

    if (call_depth > 128) {
        snprintf(err_buf, err_cap, "call stack limit exceeded");
        return false;
    }

    if (arg_count != fn->params.len) {
        snprintf(err_buf, err_cap, "function '%s' expects %zu args but got %zu", fn->name, fn->params.len, arg_count);
        return false;
    }

    vars_init(&locals);
    for (i = 0; i < arg_count; i++) {
        if (!vars_set(&locals, fn->params.items[i], &args[i])) {
            vars_free(&locals);
            snprintf(err_buf, err_cap, "out of memory");
            return false;
        }
    }

    idx = fn->body_start;
    if (!execute_block(lines, &idx, fn->body_end, 4, functions, &locals, main_prints, emit_output, call_depth + 1, err_buf, err_cap)) {
        vars_free(&locals);
        return false;
    }

    vars_free(&locals);
    return true;
}

static bool execute_call_stmt(
    const char *stmt,
    const StringList *lines,
    const FunctionTable *functions,
    VarTable *vars,
    StringList *main_prints,
    bool emit_output,
    int call_depth,
    char *err_buf,
    size_t err_cap
) {
    char fn_name[128];
    StringList arg_exprs;
    Value *args = NULL;
    size_t i;
    const FunctionDef *fn;
    bool ok = false;

    if (!parse_call_stmt(stmt, fn_name, sizeof(fn_name), &arg_exprs, err_buf, err_cap)) {
        return false;
    }

    fn = functions_find(functions, fn_name);
    if (!fn) {
        list_free(&arg_exprs);
        snprintf(err_buf, err_cap, "unknown function: %.90s", fn_name);
        return false;
    }

    args = (Value *)calloc(arg_exprs.len, sizeof(Value));
    if (arg_exprs.len > 0 && !args) {
        list_free(&arg_exprs);
        snprintf(err_buf, err_cap, "out of memory");
        return false;
    }

    for (i = 0; i < arg_exprs.len; i++) {
        value_init(&args[i]);
        if (!eval_expression(arg_exprs.items[i], vars, &args[i], err_buf, err_cap)) {
            goto cleanup;
        }
    }

    ok = execute_function(lines, functions, fn, args, arg_exprs.len, main_prints, emit_output, call_depth, err_buf, err_cap);

cleanup:
    for (i = 0; i < arg_exprs.len; i++) {
        value_free(&args[i]);
    }
    free(args);
    list_free(&arg_exprs);
    return ok;
}

static bool execute_block(
    const StringList *lines,
    size_t *index,
    size_t limit,
    size_t indent,
    const FunctionTable *functions,
    VarTable *vars,
    StringList *main_prints,
    bool emit_output,
    int call_depth,
    char *err_buf,
    size_t err_cap
) {
    bool has_stmt = false;

    while (*index < limit) {
        const char *raw = lines->items[*index];
        char *line = x_strdup(raw);
        char *stmt_copy;
        char *printed = NULL;

        if (!line) {
            snprintf(err_buf, err_cap, "out of memory");
            return false;
        }

        rtrim_inplace(line);
        if (is_blank_or_comment(line)) {
            free(line);
            (*index)++;
            continue;
        }

        if (strlen(raw) < indent || !line_has_indent(raw, indent)) {
            free(line);
            break;
        }

        if (raw[indent] == ' ') {
            free(line);
            snprintf(err_buf, err_cap, "unexpected indentation");
            return false;
        }

        stmt_copy = x_strdup(raw + indent);
        free(line);
        if (!stmt_copy) {
            snprintf(err_buf, err_cap, "out of memory");
            return false;
        }

        rtrim_inplace(stmt_copy);
        strip_inline_comment_inplace(stmt_copy);
        if (stmt_copy[0] == '\0') {
            free(stmt_copy);
            (*index)++;
            continue;
        }

        has_stmt = true;

        if (strncmp(stmt_copy, "if", 2) == 0 && (stmt_copy[2] == ' ' || stmt_copy[2] == '\t')) {
            char *cond_expr = NULL;
            bool cond_true;
            size_t body_start;
            size_t body_end;

            if (!parse_block_header(stmt_copy, "if", &cond_expr, err_buf, err_cap)) {
                free(stmt_copy);
                return false;
            }

            if (!eval_condition(cond_expr, vars, &cond_true, err_buf, err_cap)) {
                free(cond_expr);
                free(stmt_copy);
                return false;
            }
            free(cond_expr);

            body_start = *index + 1;
            body_end = find_block_end(lines, body_start, limit, indent + 4);
            if (body_start == body_end) {
                free(stmt_copy);
                snprintf(err_buf, err_cap, "if block requires an indented body");
                return false;
            }

            if (cond_true) {
                size_t run_idx = body_start;
                if (!execute_block(lines, &run_idx, body_end, indent + 4, functions, vars, main_prints, emit_output, call_depth, err_buf, err_cap)) {
                    free(stmt_copy);
                    return false;
                }
            }

            *index = body_end;

            while (*index < limit) {
                char *probe = x_strdup(lines->items[*index]);
                if (!probe) {
                    free(stmt_copy);
                    snprintf(err_buf, err_cap, "out of memory");
                    return false;
                }
                rtrim_inplace(probe);
                if (is_blank_or_comment(probe)) {
                    free(probe);
                    (*index)++;
                    continue;
                }
                free(probe);
                break;
            }

            if (*index < limit) {
                const char *else_raw = lines->items[*index];
                char *else_stmt;
                if (strlen(else_raw) >= indent && line_has_indent(else_raw, indent) && else_raw[indent] != ' ') {
                    else_stmt = x_strdup(else_raw + indent);
                    if (!else_stmt) {
                        free(stmt_copy);
                        snprintf(err_buf, err_cap, "out of memory");
                        return false;
                    }
                    rtrim_inplace(else_stmt);
                    strip_inline_comment_inplace(else_stmt);
                    if (strcmp(else_stmt, "else:") == 0) {
                        size_t else_start = *index + 1;
                        size_t else_end = find_block_end(lines, else_start, limit, indent + 4);
                        if (else_start == else_end) {
                            free(else_stmt);
                            free(stmt_copy);
                            snprintf(err_buf, err_cap, "else block requires an indented body");
                            return false;
                        }
                        if (!cond_true) {
                            size_t run_idx = else_start;
                            if (!execute_block(lines, &run_idx, else_end, indent + 4, functions, vars, main_prints, emit_output, call_depth, err_buf, err_cap)) {
                                free(else_stmt);
                                free(stmt_copy);
                                return false;
                            }
                        }
                        *index = else_end;
                    }
                    free(else_stmt);
                }
            }

            free(stmt_copy);
            continue;
        }

        if (strncmp(stmt_copy, "while", 5) == 0 && (stmt_copy[5] == ' ' || stmt_copy[5] == '\t')) {
            char *cond_expr = NULL;
            size_t body_start = *index + 1;
            size_t body_end = find_block_end(lines, body_start, limit, indent + 4);
            size_t guard = 0;

            if (!parse_block_header(stmt_copy, "while", &cond_expr, err_buf, err_cap)) {
                free(stmt_copy);
                return false;
            }
            if (body_start == body_end) {
                free(cond_expr);
                free(stmt_copy);
                snprintf(err_buf, err_cap, "while block requires an indented body");
                return false;
            }

            while (1) {
                bool cond_true;
                size_t run_idx;
                if (!eval_condition(cond_expr, vars, &cond_true, err_buf, err_cap)) {
                    free(cond_expr);
                    free(stmt_copy);
                    return false;
                }
                if (!cond_true) {
                    break;
                }
                run_idx = body_start;
                if (!execute_block(lines, &run_idx, body_end, indent + 4, functions, vars, main_prints, emit_output, call_depth, err_buf, err_cap)) {
                    free(cond_expr);
                    free(stmt_copy);
                    return false;
                }
                guard++;
                if (guard > 1000000) {
                    free(cond_expr);
                    free(stmt_copy);
                    snprintf(err_buf, err_cap, "while loop iteration limit exceeded");
                    return false;
                }
            }

            free(cond_expr);
            *index = body_end;
            free(stmt_copy);
            continue;
        }

        if (strcmp(stmt_copy, "else:") == 0) {
            free(stmt_copy);
            break;
        }

        if (strncmp(stmt_copy, "print", 5) == 0) {
            if (!parse_print_stmt(stmt_copy, vars, &printed, err_buf, err_cap)) {
                free(stmt_copy);
                return false;
            }
            if (emit_output) {
                if (!list_push(main_prints, printed)) {
                    free(printed);
                    free(stmt_copy);
                    snprintf(err_buf, err_cap, "out of memory");
                    return false;
                }
            } else {
                free(printed);
            }
            (*index)++;
            free(stmt_copy);
            continue;
        }

        {
            StringList dummy_args;
            char call_name[128];
            if (parse_call_stmt(stmt_copy, call_name, sizeof(call_name), &dummy_args, err_buf, err_cap)) {
                list_free(&dummy_args);
                if (!execute_call_stmt(stmt_copy, lines, functions, vars, main_prints, emit_output, call_depth, err_buf, err_cap)) {
                    free(stmt_copy);
                    return false;
                }
                (*index)++;
                free(stmt_copy);
                continue;
            }
        }

        if (!parse_assignment_stmt(stmt_copy, vars, err_buf, err_cap)) {
            free(stmt_copy);
            return false;
        }

        (*index)++;
        free(stmt_copy);
    }

    if (!has_stmt) {
        snprintf(err_buf, err_cap, "block requires at least one statement");
        return false;
    }

    return true;
}

static bool collect_functions(const StringList *lines, FunctionTable *functions, char *err_buf, size_t err_cap) {
    size_t i = 0;

    while (i < lines->len) {
        const char *raw = lines->items[i];
        char *line = x_strdup(raw);
        FunctionDef def;

        if (!line) {
            snprintf(err_buf, err_cap, "out of memory");
            return false;
        }

        rtrim_inplace(line);
        if (is_blank_or_comment(line)) {
            free(line);
            i++;
            continue;
        }

        if (raw[0] == ' ' || raw[0] == '\t') {
            free(line);
            snprintf(err_buf, err_cap, "line %zu: unexpected indentation at top-level", i + 1);
            return false;
        }

        {
            char fn_name[128];
            StringList tmp_params;
            char parse_err[256];

            memset(&def, 0, sizeof(def));
            list_init(&def.params);

            if (!parse_fn_header(line, fn_name, sizeof(fn_name), &tmp_params, parse_err, sizeof(parse_err))) {
                list_free(&def.params);
                free(line);
                snprintf(err_buf, err_cap, "line %zu: %.220s", i + 1, parse_err);
                return false;
            }

            def.name = x_strdup(fn_name);
            def.params = tmp_params;
            if (!def.name) {
                function_def_free(&def);
                free(line);
                snprintf(err_buf, err_cap, "out of memory");
                return false;
            }
        }

        if (functions_find(functions, def.name)) {
            function_def_free(&def);
            free(line);
            snprintf(err_buf, err_cap, "line %zu: duplicate function '%s'", i + 1, def.name);
            return false;
        }

        def.body_start = i + 1;
        def.body_end = find_block_end(lines, def.body_start, lines->len, 4);
        if (def.body_start == def.body_end) {
            function_def_free(&def);
            free(line);
            snprintf(err_buf, err_cap, "line %zu: function '%s' requires an indented body", i + 1, def.name);
            return false;
        }

        if (!functions_push(functions, &def)) {
            function_def_free(&def);
            free(line);
            snprintf(err_buf, err_cap, "out of memory");
            return false;
        }

        function_def_free(&def);
        free(line);
        i = functions->items[functions->len - 1].body_end;
    }

    return true;
}

static bool parse_program(const StringList *lines, StringList *main_prints) {
    FunctionTable functions;
    const FunctionDef *main_fn;
    char err_buf[256];

    functions_init(&functions);

    if (!collect_functions(lines, &functions, err_buf, sizeof(err_buf))) {
        compile_error(0, err_buf);
        functions_free(&functions);
        return false;
    }

    main_fn = functions_find(&functions, "main");
    if (!main_fn) {
        compile_error(0, "missing entry function: fn main():");
        functions_free(&functions);
        return false;
    }

    {
        bool ok = execute_function(lines, &functions, main_fn, NULL, 0, main_prints, true, 0, err_buf, sizeof(err_buf));
        functions_free(&functions);
        if (!ok) {
            compile_error(0, err_buf);
            return false;
        }
    }

    return true;
}

static bool ensure_parent_dirs(const char *file_path) {
    char *path = x_strdup(file_path);
    size_t i;

    if (!path) {
        return false;
    }

    for (i = 0; path[i] != '\0'; i++) {
        if (path[i] == '/' || path[i] == '\\') {
            char saved = path[i];
            path[i] = '\0';
            if (path[0] != '\0') {
                if (MKDIR(path) != 0 && errno != EEXIST) {
                    free(path);
                    return false;
                }
            }
            path[i] = saved;
        }
    }

    free(path);
    return true;
}

static void emit_db_encoded(FILE *out, const char *text) {
    size_t i;
    for (i = 0; text[i] != '\0'; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (i > 0) {
            fprintf(out, ", ");
        }

        if (ch == '\'') {
            fprintf(out, "39");
        } else if (ch >= 32 && ch <= 126) {
            fprintf(out, "'%c'", ch);
        } else {
            fprintf(out, "%u", (unsigned)ch);
        }
    }
}

static bool generate_nasm(const char *output_path, const StringList *main_prints) {
    FILE *out;
    size_t i;

    if (!ensure_parent_dirs(output_path)) {
        fprintf(stderr, "Failed to create parent directories for '%s': %s\n", output_path, strerror(errno));
        return false;
    }

    out = fopen(output_path, "wb");

    if (!out) {
        fprintf(stderr, "Failed to open output '%s': %s\n", output_path, strerror(errno));
        return false;
    }

    fprintf(out, "section .data\n");
#ifdef _WIN32
    for (i = 0; i < main_prints->len; i++) {
        const char *msg = main_prints->items[i];
        fprintf(out, "    msg_%zu: db ", i);
        emit_db_encoded(out, msg);
        if (msg[0] != '\0') {
            fprintf(out, ", 10\n");
        } else {
            fprintf(out, "10\n");
        }
        fprintf(out, "    msg_%zu_len: equ $ - msg_%zu\n", i, i);
    }

    fprintf(out, "\n");
    fprintf(out, "section .bss\n");
    fprintf(out, "    bytes_written: resd 1\n");
    fprintf(out, "\n");
    fprintf(out, "extern GetStdHandle\n");
    fprintf(out, "extern WriteFile\n");
    fprintf(out, "extern ExitProcess\n");
    fprintf(out, "global _start\n");
    fprintf(out, "section .text\n");
    fprintf(out, "_start:\n");
    fprintf(out, "    sub rsp, 40\n");
    fprintf(out, "    mov ecx, -11\n");
    fprintf(out, "    call GetStdHandle\n");
    fprintf(out, "    mov rbx, rax\n");

    for (i = 0; i < main_prints->len; i++) {
        fprintf(out, "    mov rcx, rbx\n");
        fprintf(out, "    lea rdx, [rel msg_%zu]\n", i);
        fprintf(out, "    mov r8d, msg_%zu_len\n", i);
        fprintf(out, "    lea r9, [rel bytes_written]\n");
        fprintf(out, "    mov qword [rsp + 32], 0\n");
        fprintf(out, "    call WriteFile\n");
    }

    fprintf(out, "    xor ecx, ecx\n");
    fprintf(out, "    call ExitProcess\n");
#else
    for (i = 0; i < main_prints->len; i++) {
        const char *msg = main_prints->items[i];
        fprintf(out, "    msg_%zu: db ", i);
        emit_db_encoded(out, msg);
        if (msg[0] != '\0') {
            fprintf(out, ", ");
        }
        fprintf(out, "10\n");
        fprintf(out, "    msg_%zu_len: equ $ - msg_%zu\n", i, i);
    }

    fprintf(out, "\n");
    fprintf(out, "global _start\n");
    fprintf(out, "section .text\n");
    fprintf(out, "_start:\n");

    for (i = 0; i < main_prints->len; i++) {
        fprintf(out, "    ; write(1, msg, len)\n");
        fprintf(out, "    mov rax, 1\n");
        fprintf(out, "    mov rdi, 1\n");
        fprintf(out, "    mov rsi, msg_%zu\n", i);
        fprintf(out, "    mov rdx, msg_%zu_len\n", i);
        fprintf(out, "    syscall\n");
    }

    fprintf(out, "    ; exit(0)\n");
    fprintf(out, "    mov rax, 60\n");
    fprintf(out, "    xor rdi, rdi\n");
    fprintf(out, "    syscall\n");
#endif

    fclose(out);
    return true;
}

static void print_usage(const char *argv0) {
    fprintf(stderr, "Usage: %s <input.aut> [-o output.asm]\n", argv0);
}

int main(int argc, char **argv) {
    const char *input_path;
    const char *output_path = "build/out.asm";
    size_t source_size = 0;
    char *source = NULL;
    StringList lines;
    StringList main_prints;
    bool ok = false;
    int exit_code = 1;

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    input_path = argv[1];
    if (argc >= 4 && strcmp(argv[2], "-o") == 0) {
        output_path = argv[3];
    } else if (argc > 2) {
        print_usage(argv[0]);
        return 1;
    }

    list_init(&lines);
    list_init(&main_prints);

    source = read_text_file(input_path, &source_size);
    if (!source) {
        goto cleanup;
    }

    (void)source_size;

    if (!split_lines(source, &lines)) {
        compile_error(0, "out of memory while splitting source lines");
        goto cleanup;
    }

    if (!parse_program(&lines, &main_prints)) {
        goto cleanup;
    }

    if (!generate_nasm(output_path, &main_prints)) {
        goto cleanup;
    }

    printf("Compiled %s -> %s\n", input_path, output_path);
    ok = true;

cleanup:
    free(source);
    list_free(&lines);
    list_free(&main_prints);

    if (ok) {
        exit_code = 0;
    }
    return exit_code;
}
