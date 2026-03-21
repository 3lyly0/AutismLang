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

static bool parse_fn_header(const char *line, char *name_out, size_t name_cap) {
    const char *p = line;
    size_t i = 0;

    if (strncmp(p, "fn", 2) != 0 || !isspace((unsigned char)p[2])) {
        return false;
    }
    p += 2;

    while (*p && isspace((unsigned char)*p)) {
        p++;
    }

    if (!is_ident_start(*p)) {
        return false;
    }

    while (*p && is_ident_char(*p)) {
        if (i + 1 >= name_cap) {
            return false;
        }
        name_out[i++] = *p;
        p++;
    }
    name_out[i] = '\0';

    while (*p && isspace((unsigned char)*p)) {
        p++;
    }

    if (p[0] != '(' || p[1] != ')') {
        return false;
    }
    p += 2;

    while (*p && isspace((unsigned char)*p)) {
        p++;
    }

    if (*p != ':') {
        return false;
    }
    p++;

    while (*p && isspace((unsigned char)*p)) {
        p++;
    }

    return *p == '\0';
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

static bool parse_int_view(const char *start, const char *end, long long *out_value) {
    char buf[64];
    size_t n = (size_t)(end - start);
    char *parse_end;
    long long value;

    if (n == 0 || n >= sizeof(buf)) {
        return false;
    }

    memcpy(buf, start, n);
    buf[n] = '\0';
    errno = 0;
    value = strtoll(buf, &parse_end, 10);
    if (errno != 0 || *parse_end != '\0') {
        return false;
    }

    *out_value = value;
    return true;
}

static bool parse_atom_view(const char *start, const char *end, const VarTable *vars, Value *out, char *err_buf, size_t err_cap) {
    char ident[128];
    long long int_value;
    char *decoded = NULL;

    trim_view(&start, &end);
    if (start >= end) {
        snprintf(err_buf, err_cap, "empty expression term");
        return false;
    }

    if (*start == '"' && *(end - 1) == '"') {
        if (!parse_string_view(start, end, &decoded, err_buf, err_cap)) {
            return false;
        }
        if (!value_set_str(out, decoded)) {
            free(decoded);
            snprintf(err_buf, err_cap, "out of memory");
            return false;
        }
        free(decoded);
        return true;
    }

    if (parse_int_view(start, end, &int_value)) {
        return value_set_int(out, int_value);
    }

    if (parse_identifier_view(start, end, ident, sizeof(ident))) {
        const Value *stored = vars_get(vars, ident);
        if (!stored) {
            snprintf(err_buf, err_cap, "undefined variable: %.90s", ident);
            return false;
        }
        if (!value_clone(stored, out)) {
            snprintf(err_buf, err_cap, "out of memory");
            return false;
        }
        return true;
    }

    snprintf(err_buf, err_cap, "invalid expression term");
    return false;
}

static bool value_add(Value *acc, const Value *term, char *err_buf, size_t err_cap) {
    if (acc->type == VALUE_INT && term->type == VALUE_INT) {
        acc->int_value += term->int_value;
        return true;
    }

    if (acc->type == VALUE_STR && term->type == VALUE_STR) {
        size_t left_len = strlen(acc->str_value ? acc->str_value : "");
        size_t right_len = strlen(term->str_value ? term->str_value : "");
        char *joined = (char *)malloc(left_len + right_len + 1);
        if (!joined) {
            snprintf(err_buf, err_cap, "out of memory");
            return false;
        }
        memcpy(joined, acc->str_value ? acc->str_value : "", left_len);
        memcpy(joined + left_len, term->str_value ? term->str_value : "", right_len);
        joined[left_len + right_len] = '\0';
        free(acc->str_value);
        acc->str_value = joined;
        return true;
    }

    snprintf(err_buf, err_cap, "'+' requires both sides to be same type (int+int or str+str)");
    return false;
}

static bool eval_expression(const char *expr, const VarTable *vars, Value *out, char *err_buf, size_t err_cap) {
    const char *segment_start = expr;
    const char *p = expr;
    bool in_string = false;
    bool escape = false;
    bool has_term = false;

    value_init(out);

    while (1) {
        bool at_end = *p == '\0';
        bool is_plus = *p == '+';
        if (at_end || (is_plus && !in_string)) {
            const char *term_end = p;
            Value term;
            value_init(&term);

            if (!parse_atom_view(segment_start, term_end, vars, &term, err_buf, err_cap)) {
                value_free(&term);
                value_free(out);
                return false;
            }

            if (!has_term) {
                if (!value_clone(&term, out)) {
                    value_free(&term);
                    value_free(out);
                    snprintf(err_buf, err_cap, "out of memory");
                    return false;
                }
                has_term = true;
            } else {
                if (!value_add(out, &term, err_buf, err_cap)) {
                    value_free(&term);
                    value_free(out);
                    return false;
                }
            }
            value_free(&term);

            if (at_end) {
                break;
            }
            segment_start = p + 1;
        }

        if (at_end) {
            break;
        }

        if (*p == '\\' && in_string && !escape) {
            escape = true;
            p++;
            continue;
        }

        if (*p == '"' && !escape) {
            in_string = !in_string;
        }

        if (escape) {
            escape = false;
        }

        p++;
    }

    if (in_string) {
        value_free(out);
        snprintf(err_buf, err_cap, "unterminated string literal");
        return false;
    }

    if (!has_term) {
        value_free(out);
        snprintf(err_buf, err_cap, "empty expression");
        return false;
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

static bool parse_program(const StringList *lines, StringList *main_prints) {
    size_t i = 0;
    bool seen_main = false;

    while (i < lines->len) {
        const char *raw = lines->items[i];
        char *line = x_strdup(raw);
        char fn_name[128];
        VarTable vars;
        bool in_main;
        bool has_stmt = false;

        if (!line) {
            compile_error(0, "out of memory");
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
            compile_error(i + 1, "unexpected indentation at top-level");
            return false;
        }

        if (!parse_fn_header(line, fn_name, sizeof(fn_name))) {
            free(line);
            compile_error(i + 1, "expected function definition like 'fn name():'");
            return false;
        }
        free(line);

        in_main = strcmp(fn_name, "main") == 0;
        if (in_main) {
            seen_main = true;
        }

        vars_init(&vars);

        i++;
        while (i < lines->len) {
            const char *body_raw = lines->items[i];
            char *body_line = x_strdup(body_raw);

            if (!body_line) {
                compile_error(0, "out of memory");
                return false;
            }

            rtrim_inplace(body_line);
            if (is_blank_or_comment(body_line)) {
                free(body_line);
                i++;
                continue;
            }

            if (strncmp(body_raw, INDENT, 4) != 0) {
                free(body_line);
                break;
            }

            {
                const char *stmt = body_raw + 4;
                char *stmt_copy = x_strdup(stmt);
                char *printed = NULL;
                char err_buf[128];

                if (!stmt_copy) {
                    free(body_line);
                    vars_free(&vars);
                    compile_error(0, "out of memory");
                    return false;
                }

                rtrim_inplace(stmt_copy);
                if (strncmp(stmt_copy, "print", 5) == 0) {
                    if (!parse_print_stmt(stmt_copy, &vars, &printed, err_buf, sizeof(err_buf))) {
                        free(stmt_copy);
                        free(body_line);
                        vars_free(&vars);
                        compile_error(i + 1, err_buf);
                        return false;
                    }

                    if (in_main) {
                        if (!list_push(main_prints, printed)) {
                            free(printed);
                            free(stmt_copy);
                            free(body_line);
                            vars_free(&vars);
                            compile_error(0, "out of memory");
                            return false;
                        }
                    } else {
                        free(printed);
                    }
                } else {
                    if (!parse_assignment_stmt(stmt_copy, &vars, err_buf, sizeof(err_buf))) {
                        free(stmt_copy);
                        free(body_line);
                        vars_free(&vars);
                        compile_error(i + 1, err_buf);
                        return false;
                    }
                }

                has_stmt = true;
                free(stmt_copy);
            }

            free(body_line);
            i++;
        }

        if (!has_stmt) {
            char msg[192];
            vars_free(&vars);
            snprintf(msg, sizeof(msg), "function '%s' must contain at least one statement", fn_name);
            compile_error(i, msg);
            return false;
        }

        vars_free(&vars);
    }

    if (!seen_main) {
        compile_error(0, "missing entry function: fn main():");
        return false;
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
