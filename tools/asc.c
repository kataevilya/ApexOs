/*
 * ASC — ApexOS Simple Compiler.
 *
 * ЧЕСТНО, прямо здесь: это НЕ TCC, НЕ GCC, и не полный компилятор C.
 * Это оригинальный, написанный с нуля для ApexOS маленький компилятор
 * небольшого C-подобного подмножества в реальный x86_64 машинный код
 * (через ассемблер) и рабочий ELF64-бинарник под syscall ABI ApexOS.
 *
 * Работает на ХОСТЕ (обычная Linux-программа, собирается системным
 * gcc — см. `make asc`), как и любой кросс-компилятор: ему не нужно
 * быть самим ApexOS, чтобы генерировать код ДЛЯ ApexOS.
 *
 * Поддерживаемое подмножество (и оно реально ограничено, без обмана):
 *   - только функция main(), без параметров, без других функций;
 *   - единственный тип — int (64-бит, знаковый);
 *   - int-переменные (объявление, присваивание);
 *   - арифметика: + - * / %
 *   - сравнения: == != < > <= >=
 *   - логические: && || ! -- БЕЗ короткого замыкания (честное отличие
 *     от настоящего C: обе стороны && / || всегда вычисляются);
 *   - if / else, while;
 *   - print("строка") и print(выражение);
 *   - return выражение; (=> SYS_EXIT с этим кодом)
 *   - // однострочные комментарии.
 * НЕ поддержано: массивы, указатели, структуры, другие функции,
 * #include/#define, строки как переменные, break/continue, for.
 *
 * Стратегия кодогенерации: простой стековый вычислитель выражений
 * (каждое выражение оставляет результат в %rax; бинарные операции —
 * push левого операнда, вычислить правый, pop, объединить). Локальные
 * переменные — фиксированные смещения от %rbp в кадре стека main().
 * Не оптимизирующий компилятор — корректность важнее эффективности
 * сгенерированного кода на этом этапе.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ================= Лексер ================= */

typedef enum {
    T_EOF, T_INT, T_IDENT, T_STRING, T_OP, T_KEYWORD
} TokType;

typedef struct {
    TokType type;
    char text[256];
    long ival;
    int line;
} Token;

#define MAX_TOKENS 65536
static Token toks[MAX_TOKENS];
static int ntoks = 0;

static const char *KEYWORDS[] = {
    "int", "if", "else", "while", "print", "return", "main", NULL
};

static int is_keyword(const char *s) {
    for (int i = 0; KEYWORDS[i]; i++) {
        if (strcmp(s, KEYWORDS[i]) == 0) return 1;
    }
    return 0;
}

static void lex_error(int line, const char *msg) {
    fprintf(stderr, "asc: lex error at line %d: %s\n", line, msg);
    exit(1);
}

static void tokenize(const char *src) {
    const char *p = src;
    int line = 1;
    while (*p) {
        if (*p == '\n') { line++; p++; continue; }
        if (isspace((unsigned char)*p)) { p++; continue; }
        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n') p++;
            continue;
        }
        if (ntoks >= MAX_TOKENS) lex_error(line, "program too large (token limit)");

        if (isdigit((unsigned char)*p)) {
            long val = 0;
            while (isdigit((unsigned char)*p)) { val = val * 10 + (*p - '0'); p++; }
            toks[ntoks].type = T_INT;
            toks[ntoks].ival = val;
            toks[ntoks].line = line;
            ntoks++;
            continue;
        }
        if (isalpha((unsigned char)*p) || *p == '_') {
            int len = 0;
            char buf[256];
            while ((isalnum((unsigned char)*p) || *p == '_') && len < 255) {
                buf[len++] = *p++;
            }
            buf[len] = '\0';
            toks[ntoks].type = is_keyword(buf) ? T_KEYWORD : T_IDENT;
            memcpy(toks[ntoks].text, buf, (size_t)len + 1); /* +1: включая '\0', уже поставленный выше */
            toks[ntoks].line = line;
            ntoks++;
            continue;
        }
        if (*p == '"') {
            p++;
            int len = 0;
            char buf[256];
            while (*p && *p != '"' && len < 255) {
                if (*p == '\\' && p[1]) {
                    p++;
                    char c = *p;
                    if (c == 'n') c = '\n';
                    else if (c == 't') c = '\t';
                    else if (c == '\\') c = '\\';
                    else if (c == '"') c = '"';
                    buf[len++] = c;
                    p++;
                } else {
                    buf[len++] = *p++;
                }
            }
            buf[len] = '\0';
            if (*p != '"') lex_error(line, "unterminated string literal");
            p++;
            toks[ntoks].type = T_STRING;
            memcpy(toks[ntoks].text, buf, (size_t)len + 1);
            toks[ntoks].line = line;
            ntoks++;
            continue;
        }

        static const char *ops2[] = { "==", "!=", "<=", ">=", "&&", "||", NULL };
        int matched = 0;
        for (int i = 0; ops2[i]; i++) {
            if (p[0] == ops2[i][0] && p[1] == ops2[i][1]) {
                strcpy(toks[ntoks].text, ops2[i]);
                toks[ntoks].type = T_OP;
                toks[ntoks].line = line;
                ntoks++;
                p += 2;
                matched = 1;
                break;
            }
        }
        if (matched) continue;

        if (strchr("+-*/%=<>!(){};,", *p) != NULL) {
            toks[ntoks].text[0] = *p;
            toks[ntoks].text[1] = '\0';
            toks[ntoks].type = T_OP;
            toks[ntoks].line = line;
            ntoks++;
            p++;
            continue;
        }

        lex_error(line, "unexpected character");
    }
    toks[ntoks].type = T_EOF;
    toks[ntoks].text[0] = '\0';
    toks[ntoks].line = line;
    ntoks++;
}

/* ================= AST ================= */

typedef enum {
    N_NUM, N_VAR, N_BINOP, N_UNOP, N_ASSIGN, N_DECL,
    N_IF, N_WHILE, N_PRINT_STR, N_PRINT_EXPR, N_RETURN, N_BLOCK
} NodeType;

typedef struct Node {
    NodeType type;
    long ival;
    char sval[256];
    char op[3];
    struct Node *a, *b, *c;
    struct Node **stmts;
    int nstmts;
    int line;
} Node;

static Node *new_node(NodeType t) {
    Node *n = (Node *)calloc(1, sizeof(Node));
    if (!n) { fprintf(stderr, "asc: out of memory\n"); exit(1); }
    n->type = t;
    return n;
}

/* ================= Парсер (рекурсивный спуск) ================= */

static int pos = 0;

static Token *cur(void) { return &toks[pos]; }
static void advance(void) { if (toks[pos].type != T_EOF) pos++; }

static void parse_error(const char *msg) {
    fprintf(stderr, "asc: parse error at line %d: %s (got \"%s\")\n",
            cur()->line, msg, cur()->text);
    exit(1);
}

static int check_op(const char *s) { return cur()->type == T_OP && strcmp(cur()->text, s) == 0; }
static int check_kw(const char *s) { return cur()->type == T_KEYWORD && strcmp(cur()->text, s) == 0; }

static void expect_op(const char *s) {
    if (!check_op(s)) parse_error(s);
    advance();
}
static void expect_kw(const char *s) {
    if (!check_kw(s)) parse_error(s);
    advance();
}

static Node *parse_expr(void);

static Node *parse_primary(void) {
    if (cur()->type == T_INT) {
        Node *n = new_node(N_NUM);
        n->ival = cur()->ival;
        n->line = cur()->line;
        advance();
        return n;
    }
    if (cur()->type == T_IDENT) {
        Node *n = new_node(N_VAR);
        strncpy(n->sval, cur()->text, sizeof(n->sval) - 1);
        n->line = cur()->line;
        advance();
        return n;
    }
    if (check_op("(")) {
        advance();
        Node *n = parse_expr();
        expect_op(")");
        return n;
    }
    parse_error("expected expression");
    return NULL;
}

static Node *parse_unary(void) {
    if (check_op("-") || check_op("!")) {
        Node *n = new_node(N_UNOP);
        strcpy(n->op, cur()->text);
        n->line = cur()->line;
        advance();
        n->a = parse_unary();
        return n;
    }
    return parse_primary();
}

static Node *parse_binop_level(Node *(*next)(void), const char **ops) {
    Node *left = next();
    for (;;) {
        int matched = 0;
        for (int i = 0; ops[i]; i++) {
            if (check_op(ops[i])) {
                Node *n = new_node(N_BINOP);
                strcpy(n->op, ops[i]);
                n->line = cur()->line;
                advance();
                n->a = left;
                n->b = next();
                left = n;
                matched = 1;
                break;
            }
        }
        if (!matched) break;
    }
    return left;
}

static Node *parse_mul(void) {
    static const char *ops[] = { "*", "/", "%", NULL };
    return parse_binop_level(parse_unary, ops);
}
static Node *parse_add(void) {
    static const char *ops[] = { "+", "-", NULL };
    return parse_binop_level(parse_mul, ops);
}
static Node *parse_rel(void) {
    static const char *ops[] = { "<=", ">=", "<", ">", NULL };
    return parse_binop_level(parse_add, ops);
}
static Node *parse_eq(void) {
    static const char *ops[] = { "==", "!=", NULL };
    return parse_binop_level(parse_rel, ops);
}
static Node *parse_and(void) {
    static const char *ops[] = { "&&", NULL };
    return parse_binop_level(parse_eq, ops);
}
static Node *parse_or(void) {
    static const char *ops[] = { "||", NULL };
    return parse_binop_level(parse_and, ops);
}
static Node *parse_expr(void) { return parse_or(); }

static Node *parse_block(void);

static Node *parse_stmt(void) {
    if (check_op("{")) return parse_block();

    if (check_kw("int")) {
        advance();
        if (cur()->type != T_IDENT) parse_error("expected identifier after int");
        Node *n = new_node(N_DECL);
        strncpy(n->sval, cur()->text, sizeof(n->sval) - 1);
        n->line = cur()->line;
        advance();
        if (check_op("=")) {
            advance();
            n->a = parse_expr();
        }
        expect_op(";");
        return n;
    }
    if (check_kw("if")) {
        advance();
        expect_op("(");
        Node *n = new_node(N_IF);
        n->a = parse_expr();
        expect_op(")");
        n->b = parse_stmt();
        if (check_kw("else")) {
            advance();
            n->c = parse_stmt();
        }
        return n;
    }
    if (check_kw("while")) {
        advance();
        expect_op("(");
        Node *n = new_node(N_WHILE);
        n->a = parse_expr();
        expect_op(")");
        n->b = parse_stmt();
        return n;
    }
    if (check_kw("print")) {
        advance();
        expect_op("(");
        Node *n;
        if (cur()->type == T_STRING) {
            n = new_node(N_PRINT_STR);
            memcpy(n->sval, cur()->text, sizeof(n->sval));
            advance();
        } else {
            n = new_node(N_PRINT_EXPR);
            n->a = parse_expr();
        }
        expect_op(")");
        expect_op(";");
        return n;
    }
    if (check_kw("return")) {
        advance();
        Node *n = new_node(N_RETURN);
        n->a = parse_expr();
        expect_op(";");
        return n;
    }
    if (cur()->type == T_IDENT) {
        Node *n = new_node(N_ASSIGN);
        strncpy(n->sval, cur()->text, sizeof(n->sval) - 1);
        n->line = cur()->line;
        advance();
        expect_op("=");
        n->a = parse_expr();
        expect_op(";");
        return n;
    }
    parse_error("unexpected token at start of statement");
    return NULL;
}

static Node *parse_block(void) {
    expect_op("{");
    Node *n = new_node(N_BLOCK);
    int cap = 64;
    n->stmts = (Node **)malloc(sizeof(Node *) * (size_t)cap);
    n->nstmts = 0;
    while (!check_op("}")) {
        if (cur()->type == T_EOF) parse_error("unexpected end of file, missing }");
        if (n->nstmts >= cap) {
            cap *= 2;
            n->stmts = (Node **)realloc(n->stmts, sizeof(Node *) * (size_t)cap);
        }
        n->stmts[n->nstmts++] = parse_stmt();
    }
    expect_op("}");
    return n;
}

static Node *parse_program(void) {
    expect_kw("int");
    expect_kw("main");
    expect_op("(");
    expect_op(")");
    return parse_block();
}

/* ================= Кодогенерация ================= */

typedef struct { char name[64]; int offset; } Sym;
#define MAX_SYMS 512
static Sym syms[MAX_SYMS];
static int nsyms = 0;
static int next_offset = 0;
#define FRAME_SIZE 4096 /* фиксированный кадр стека main() -- с большим запасом для маленьких тестовых программ */

static int sym_offset(const char *name) {
    for (int i = 0; i < nsyms; i++) {
        if (strcmp(syms[i].name, name) == 0) return syms[i].offset;
    }
    next_offset -= 8;
    if (-next_offset > FRAME_SIZE - 64) {
        fprintf(stderr, "asc: too many local variables (compiler frame limit)\n");
        exit(1);
    }
    if (nsyms >= MAX_SYMS) {
        fprintf(stderr, "asc: too many local variables (symbol table limit)\n");
        exit(1);
    }
    strncpy(syms[nsyms].name, name, sizeof(syms[nsyms].name) - 1);
    syms[nsyms].offset = next_offset;
    nsyms++;
    return next_offset;
}

#define MAX_STRINGS 256
static char string_lits[MAX_STRINGS][256];
static int string_lens[MAX_STRINGS];
static int nstrings = 0;

static int add_string_literal(const char *s) {
    if (nstrings >= MAX_STRINGS) {
        fprintf(stderr, "asc: too many string literals\n");
        exit(1);
    }
    size_t len = strlen(s);
    memcpy(string_lits[nstrings], s, len + 1);
    string_lens[nstrings] = (int)len;
    return nstrings++;
}

static int label_counter = 0;

static void gen_expr(FILE *fp, Node *n) {
    switch (n->type) {
        case N_NUM:
            fprintf(fp, "    movq $%ld, %%rax\n", n->ival);
            break;
        case N_VAR:
            fprintf(fp, "    movq %d(%%rbp), %%rax\n", sym_offset(n->sval));
            break;
        case N_UNOP:
            gen_expr(fp, n->a);
            if (strcmp(n->op, "-") == 0) {
                fprintf(fp, "    negq %%rax\n");
            } else { /* "!" */
                fprintf(fp, "    testq %%rax, %%rax\n    sete %%al\n    movzbq %%al, %%rax\n");
            }
            break;
        case N_BINOP:
            gen_expr(fp, n->a);
            fprintf(fp, "    pushq %%rax\n");
            gen_expr(fp, n->b);
            fprintf(fp, "    movq %%rax, %%rcx\n    popq %%rax\n");
            if (strcmp(n->op, "+") == 0) fprintf(fp, "    addq %%rcx, %%rax\n");
            else if (strcmp(n->op, "-") == 0) fprintf(fp, "    subq %%rcx, %%rax\n");
            else if (strcmp(n->op, "*") == 0) fprintf(fp, "    imulq %%rcx, %%rax\n");
            else if (strcmp(n->op, "/") == 0) fprintf(fp, "    cqto\n    idivq %%rcx\n");
            else if (strcmp(n->op, "%") == 0) fprintf(fp, "    cqto\n    idivq %%rcx\n    movq %%rdx, %%rax\n");
            else if (strcmp(n->op, "==") == 0) fprintf(fp, "    cmpq %%rcx, %%rax\n    sete %%al\n    movzbq %%al, %%rax\n");
            else if (strcmp(n->op, "!=") == 0) fprintf(fp, "    cmpq %%rcx, %%rax\n    setne %%al\n    movzbq %%al, %%rax\n");
            else if (strcmp(n->op, "<") == 0) fprintf(fp, "    cmpq %%rcx, %%rax\n    setl %%al\n    movzbq %%al, %%rax\n");
            else if (strcmp(n->op, ">") == 0) fprintf(fp, "    cmpq %%rcx, %%rax\n    setg %%al\n    movzbq %%al, %%rax\n");
            else if (strcmp(n->op, "<=") == 0) fprintf(fp, "    cmpq %%rcx, %%rax\n    setle %%al\n    movzbq %%al, %%rax\n");
            else if (strcmp(n->op, ">=") == 0) fprintf(fp, "    cmpq %%rcx, %%rax\n    setge %%al\n    movzbq %%al, %%rax\n");
            else if (strcmp(n->op, "&&") == 0)
                fprintf(fp, "    testq %%rax,%%rax\n    setne %%al\n    movzbq %%al,%%rax\n"
                             "    testq %%rcx,%%rcx\n    setne %%cl\n    movzbq %%cl,%%rcx\n"
                             "    andq %%rcx,%%rax\n");
            else if (strcmp(n->op, "||") == 0)
                fprintf(fp, "    testq %%rax,%%rax\n    setne %%al\n    movzbq %%al,%%rax\n"
                             "    testq %%rcx,%%rcx\n    setne %%cl\n    movzbq %%cl,%%rcx\n"
                             "    orq %%rcx,%%rax\n");
            break;
        default:
            fprintf(stderr, "asc: internal error: gen_expr on non-expression node\n");
            exit(1);
    }
}

static void gen_stmt(FILE *fp, Node *n) {
    switch (n->type) {
        case N_BLOCK:
            for (int i = 0; i < n->nstmts; i++) gen_stmt(fp, n->stmts[i]);
            break;
        case N_DECL:
            if (n->a) {
                gen_expr(fp, n->a);
                fprintf(fp, "    movq %%rax, %d(%%rbp)\n", sym_offset(n->sval));
            } else {
                fprintf(fp, "    movq $0, %d(%%rbp)\n", sym_offset(n->sval));
            }
            break;
        case N_ASSIGN:
            gen_expr(fp, n->a);
            fprintf(fp, "    movq %%rax, %d(%%rbp)\n", sym_offset(n->sval));
            break;
        case N_IF: {
            int else_l = label_counter++;
            int end_l = label_counter++;
            gen_expr(fp, n->a);
            fprintf(fp, "    testq %%rax, %%rax\n    jz .Lelse%d\n", else_l);
            gen_stmt(fp, n->b);
            fprintf(fp, "    jmp .Lend%d\n.Lelse%d:\n", end_l, else_l);
            if (n->c) gen_stmt(fp, n->c);
            fprintf(fp, ".Lend%d:\n", end_l);
            break;
        }
        case N_WHILE: {
            int top_l = label_counter++;
            int end_l = label_counter++;
            fprintf(fp, ".Lwhile%d:\n", top_l);
            gen_expr(fp, n->a);
            fprintf(fp, "    testq %%rax, %%rax\n    jz .Lwhileend%d\n", end_l);
            gen_stmt(fp, n->b);
            fprintf(fp, "    jmp .Lwhile%d\n.Lwhileend%d:\n", top_l, end_l);
            break;
        }
        case N_PRINT_STR: {
            int id = add_string_literal(n->sval);
            fprintf(fp, "    movq $str%d, %%rdi\n    movq $%d, %%rsi\n    movq $1, %%rax\n    int $0x80\n",
                    id, string_lens[id]);
            break;
        }
        case N_PRINT_EXPR:
            gen_expr(fp, n->a);
            fprintf(fp, "    call __asc_itoa_print\n");
            break;
        case N_RETURN:
            gen_expr(fp, n->a);
            fprintf(fp, "    movq %%rax, %%rdi\n    movq $0, %%rax\n    int $0x80\n");
            break;
        default:
            fprintf(stderr, "asc: internal error: gen_stmt on non-statement node\n");
            exit(1);
    }
}

/* Встроенный помощник печати int -> десятичная строка, вызываемый через
   `call` -- фиксированный код, написанный вручную один раз, не
   генерируется заново для каждой программы. */
static void emit_itoa_helper(FILE *fp) {
    fprintf(fp,
        "\n__asc_itoa_print:\n"
        "    pushq %%rbp\n"
        "    movq %%rsp, %%rbp\n"
        "    subq $32, %%rsp\n"
        "    movq %%rax, %%rbx\n"
        "    xorq %%r9, %%r9\n"
        "    testq %%rbx, %%rbx\n"
        "    jns .Litoa_positive\n"
        "    movq $1, %%r9\n"
        "    negq %%rbx\n"
        ".Litoa_positive:\n"
        "    leaq 31(%%rsp), %%rdi\n"
        "    xorq %%r8, %%r8\n"
        "    movq %%rbx, %%rax\n"
        ".Litoa_loop:\n"
        "    xorq %%rdx, %%rdx\n"
        "    movq $10, %%rcx\n"
        "    divq %%rcx\n"
        "    addb $'0', %%dl\n"
        "    decq %%rdi\n"
        "    movb %%dl, (%%rdi)\n"
        "    incq %%r8\n"
        "    testq %%rax, %%rax\n"
        "    jnz .Litoa_loop\n"
        "    testq %%r9, %%r9\n"
        "    jz .Litoa_write\n"
        "    decq %%rdi\n"
        "    movb $'-', (%%rdi)\n"
        "    incq %%r8\n"
        ".Litoa_write:\n"
        "    movq %%r8, %%rsi\n"
        "    movq $1, %%rax\n"
        "    int $0x80\n"
        "    movq %%rbp, %%rsp\n"
        "    popq %%rbp\n"
        "    ret\n"
    );
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "ASC -- ApexOS Simple Compiler (NOT TCC/GCC, small original subset -- see tools/asc.c)\n");
        fprintf(stderr, "usage: %s <input.c> <output.S>\n", argv[0]);
        return 1;
    }

    FILE *fin = fopen(argv[1], "rb");
    if (!fin) { fprintf(stderr, "asc: cannot open %s\n", argv[1]); return 1; }
    fseek(fin, 0, SEEK_END);
    long sz = ftell(fin);
    fseek(fin, 0, SEEK_SET);
    char *src = (char *)malloc((size_t)sz + 1);
    if (fread(src, 1, (size_t)sz, fin) != (size_t)sz) {
        fprintf(stderr, "asc: failed reading %s\n", argv[1]);
        return 1;
    }
    src[sz] = '\0';
    fclose(fin);

    tokenize(src);
    Node *body = parse_program();

    FILE *fout = fopen(argv[2], "w");
    if (!fout) { fprintf(stderr, "asc: cannot open %s for writing\n", argv[2]); return 1; }

    fprintf(fout, "/* Auto-generated by ASC (ApexOS Simple Compiler) from %s -- not hand-written */\n", argv[1]);
    fprintf(fout, ".section .text\n.global _start\n_start:\n");
    fprintf(fout, "    pushq %%rbp\n    movq %%rsp, %%rbp\n    subq $%d, %%rsp\n", FRAME_SIZE);

    gen_stmt(fout, body);

    /* Если main() "провалилась" до конца без явного return -- считаем
       это return 0 (как в настоящем C для int main). */
    fprintf(fout, "    movq $0, %%rdi\n    movq $0, %%rax\n    int $0x80\n");

    emit_itoa_helper(fout);

    fprintf(fout, "\n.section .data\n");
    for (int i = 0; i < nstrings; i++) {
        fprintf(fout, "str%d:\n", i);
        for (int j = 0; j < string_lens[i]; j++) {
            fprintf(fout, "    .byte %d\n", (unsigned char)string_lits[i][j]);
        }
        if (string_lens[i] == 0) {
            fprintf(fout, "    .byte 0\n"); /* пустая строка -- нужен хотя бы 1 байт под метку */
        }
    }

    fprintf(fout, "\n.section .note.GNU-stack, \"\", @progbits\n");
    fclose(fout);

    fprintf(stderr, "asc: compiled %s -> %s (%d local var(s), %d string literal(s))\n",
            argv[1], argv[2], nsyms, nstrings);
    return 0;
}
