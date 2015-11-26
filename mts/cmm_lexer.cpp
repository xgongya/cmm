// cmm_lex.cpp
// Initial version 2001.12.12 by doing
// Immigrated 2015.10.28 by doing

#include "std_port/std_port_cs.h"
#include "cmm.h"
#include "cmm_buffer_new.h"
#include "cmm_common_util.h"
#include "cmm_lang.h"
#include "cmm_lexer.h"
#include "cmm_program.h"

namespace cmm
{

/* Refer to external variable in cmm_grammar.y */
extern YYSTYPE yylval;
int yydebug = 1;

#define LEX_EOF ((char) EOF)

#define lex_errors(...) m_lang_context->syntax_errors(__VA_ARGS__)
#define lex_warn(str)   m_lang_context->syntax_warn(str)
#define lex_warns(...)  m_lang_context->syntax_warns(__VA_ARGS__)
#define lex_echo(str)   m_lang_context->syntax_echo(str)

// Key words
Keyword Lexer::m_define_keywords[] =
{
    { "array",       L_BASIC_TYPE,   ARRAY    },
    { "break",       L_BREAK,        0        },
    { "buffer",      L_BASIC_TYPE,   BUFFER   },
    { "build",       L_BUILD,        0        },
    { "call",        L_CALL,         0        },
    { "catch",       L_CATCH,        0        },
    { "case",        L_CASE,         0        },
    { "const",       L_CONST,        0        },
    { "continue",    L_CONTINUE,     0        },
    { "default",     L_DEFAULT,      0        },
    { "do",          L_DO,           0        },
    { "downto",      L_DOWNTO,       0        },
    { "each",        L_EACH,         0        },
    { "else",        L_ELSE,         0        },
    { "float",       L_BASIC_TYPE,   REAL     },
    { "function",    L_BASIC_TYPE,   FUNCTION },
    { "for",         L_FOR,          0        },
    { "goto",        L_GOTO,         0        },
    { "if",          L_IF,           0        },
    { "component",   L_ADD_COMPONENT,0,       },
    { "in",          L_IN,           0,       },
    { "int",         L_BASIC_TYPE,   INTEGER  },
    { "is_ref",      L_IS_REF,       0        },
    { "loop",        L_LOOP,         0        },
    { "mapping",     L_BASIC_TYPE,   MAPPING  },
    { "mixed",       L_BASIC_TYPE,   MIXED    },
    { "nomask",      L_NOMASK,       0        },
    { "nosave",      L_NOSAVE,       0        },
    { "object",      L_BASIC_TYPE,   OBJECT   },
    { "override",    L_OVERRIDE,     0        },
    { "private",     L_PRIVATE,      0        },
    { "public",      L_PUBLIC,       0        },
    { "real",        L_BASIC_TYPE,   REAL     },
    { "return",      L_RETURN,       0        },
    { "static",      L_STATIC,       0        },
    { "string",      L_BASIC_TYPE,   STRING   },
    { "switch",      L_SWITCH,       0        },
    { "trace",       L_TRACE,        0        },
    { "shout",       L_SHOUT,        0        },
    { "try",         L_TRY,          0        },
    { "nil",         L_NIL,          0        },
    { "upto",        L_UPTO,         0        },
    { "using",       L_USING,        0        },
    { "varargs",     L_VARARGS,      0        },
    { "void",        L_BASIC_TYPE,   TVOID    },
    { "while",       L_WHILE,        0        },
    { 0 },
};

// Mapping: name->keyword
Lexer::KeywordMap* Lexer::m_keywords = 0;

// All file names
Lexer::FileNameList* Lexer::m_file_name_list = 0;

// Critical section for access
std_critical_section_t* Lexer::m_cs = 0;

int Lexer::init()
{
    size_t i;
    for (i = 0; m_define_keywords[i].c_str_word; i++)
    {
        // Put the keyword name into program string pool 
        auto *str = Program::find_or_add_string(m_define_keywords[i].c_str_word);
        m_define_keywords[i].word = str;
    }

    // Create the keywords map
    m_keywords = XNEW(KeywordMap, i);
    for (i = 0; m_define_keywords[i].c_str_word; i++)
        m_keywords->put(m_define_keywords[i].word, &m_define_keywords[i]);

    // Create file name list
    m_file_name_list = XNEW(FileNameList);

    std_new_critical_section(&m_cs);
}

void Lexer::shutdown()
{
    std_delete_critical_section(m_cs);
    XDELETE(m_file_name_list);
    XDELETE(m_keywords);
}

Lexer::Lexer()
{
}

void Lexer::handle_elif(char *sp)
{
    if (this->m_if_top != NULL)
    {
        if (this->m_if_top->state == IfStatement::EXPECT_ELSE)
        {
            /* Last cond was false... */
            IntR cond;
            IfStatement *p = this->m_if_top;

            /* Pop previous condition */
            this->m_if_top = p->next;

            *--this->m_out = '\0';
            add_input(sp);
            cond = cond_get_exp(0);
            if (*this->m_out++)
            {
                lex_error("Condition too complex in #elif.");
                while (*this->m_out++);
            } else handle_cond(cond);
        } else
        {   /* EXPECT_ENDIF */
            /*
             * last cond was true...skip to end of
             * conditional
             */
            skip_to("endif", NULL);
        }
    } else
    {
        lex_errorp("Unexpected %celif.");
    }
}

void Lexer::handle_else()
{
    if (this->m_if_top != NULL)
    {
        if (this->m_if_top->state == IfStatement::EXPECT_ELSE)
        {
            this->m_if_top->state = IfStatement::EXPECT_ENDIF;
        } else
        {
            skip_to("endif", NULL);
        }
    } else
    {
        lex_errorp("Unexpected %cendif.");
    }
}

void Lexer::handle_endif()
{
    if (this->m_if_top != NULL &&
        (this->m_if_top->state == IfStatement::EXPECT_ENDIF ||
         this->m_if_top->state == IfStatement::EXPECT_ELSE))
    {
        IfStatement *p = this->m_if_top;
        
        this->m_if_top = p->next;
    } else
    {
        lex_errorp("Unexpected %cendif.");
    }
}

enum
{
    BNOT   = 1,
    LNOT   = 2,
    UMINUS = 3,
    UPLUS  = 4,
};

enum
{
    MULT   = 1,
    DIV    = 2,
    MOD    = 3,
    BPLUS  = 4,
    BMINUS = 5,
    LSHIFT = 6,
    RSHIFT = 7,
    LESS   = 8,
    LEQ    = 9,
    GREAT  = 10,
    GEQ    = 11,
    EQ     = 12,
    NEQ    = 13,
    BAND   = 14,
    XOR    = 15,
    BOR    = 16,
    LAND   = 17,
    LOR    = 18,
    QMARK  = 19,
};

char Lexer::m_optab[] =
{ 0, 0, 0, 0, 0,  0,  0, 0, 0, 0,  0,  0,  0,  0,  0,  0,
  0, 0, 0, 0, 0,  0,  0, 0, 0, 0,  0,  0,  0,  0,  0,  0,
  0, 4, 0, 0, 0, 26, 56, 0, 0, 0, 18, 14,  0, 10,  0, 22,
  0, 0, 0, 0, 0,  0,  0, 0, 0, 0,  0,  0, 30, 50, 40, 74,
  0, 0, 0, 0, 0,  0,  0, 0, 0, 0,  0,  0,  0,  0,  0,  0,
  0, 0, 0, 0, 0,  0,  0, 0, 0, 0,  0,  0,  0,  0, 70,  0,
  0, 0, 0, 0, 0,  0,  0, 0, 0, 0,  0,  0,  0,  0,  0,  0,
  0, 0, 0, 0, 0,  0,  0, 0, 0, 0,  0,  0, 63,  0,  1,  0,
  0, 0, 0, 0, 0,  0,  0, 0, 0, 0,  0,  0,  0,  0,  0,  0,
  0, 0, 0, 0, 0,  0,  0, 0, 0, 0,  0,  0,  0,  0,  0,  0,
  0, 0, 0, 0, 0,  0,  0, 0, 0, 0,  0,  0,  0,  0,  0,  0,
  0, 0, 0, 0, 0,  0,  0, 0, 0, 0,  0,  0,  0,  0,  0,  0,
  0, 0, 0, 0, 0,  0,  0, 0, 0, 0,  0,  0,  0,  0,  0,  0,
  0, 0, 0, 0, 0,  0,  0, 0, 0, 0,  0,  0,  0,  0,  0,  0,
  0, 0, 0, 0, 0,  0,  0, 0, 0, 0,  0,  0,  0,  0,  0,  0,
  0, 0, 0, 0, 0,  0,  0, 0, 0, 0,  0,  0,  0,  0,  0,  0, };

char Lexer::m_optab2[] =
{BNOT, 0, 0, LNOT, '=', NEQ, 7, 0, 0, UMINUS, 0, BMINUS, 10, UPLUS, 0, BPLUS, 10,
 0, 0, MULT, 11, 0, 0, DIV, 11, 0, 0, MOD, 11,
 0, '<', LSHIFT, 9, '=', LEQ, 8, 0, LESS, 8, 0, '>', RSHIFT, 9, '=', GEQ, 8, 0, GREAT, 8,
 0, '=', EQ, 7, 0, 0, 0, '&', LAND, 3, 0, BAND, 6, 0, '|', LOR, 2, 0, BOR, 4,
 0, 0, XOR, 5, 0, 0, QMARK, 1};

IntR Lexer::cond_get_exp(IntR priority)
{
    IntR c;
    IntR value, value2, x;

    do
    {
        c = get_char_in_cond_exp();
    } while (isspace(c));

    if (c == '(')
    {
        value = cond_get_exp(0);
        do {
            c = get_char_in_cond_exp();
        } while (isspace(c));
        if (c != ')')
        {
            lex_error("bracket not paired in #if.");
            if (!c) *--this->m_out = '\0';
        }
    } else
    if (ispunct(c))
    {
        if (!(x = m_optab[c]))
        {
            lex_errorp("illegal character in %cif.");
            return 0;
        }
        value = cond_get_exp(12);
        switch (m_optab2[x - 1])
        {
        case BNOT:
            value = ~value;
            break;
        case LNOT:
            value = !value;
            break;
        case UMINUS:
            value = -value;
            break;
        case UPLUS:
            value = value;
            break;
        default:
            lex_errorp("illegal unary operator in %cif.");
        }
    } else
    {
        IntR base;

        if (!isdigit(c))
        {
            if (!c)
            {
                lex_errorp("missing expression in %cif.");
            } else
                lex_errorp("illegal character in %cif.");
            return 0;
        }
        value = 0;
        if (c != '0')
            base = 10;
        else
        {
            c = *this->m_out++;
            if (c == 'x' || c == 'X')
            {
                base = 16;
                c = *this->m_out++;
            } else
                base = 8;
        }
        for (;;)
        {
            if (isdigit(c))
                x = - '0';
            else if (isupper(c))
                x = - 'A' + 10;
            else if (islower(c))
                x = - 'a' + 10;
            else
                break;
            x += c;
            if (x > base)
                break;
            value = value * base + x;
            c = *this->m_out++;
        }
        this->m_out--;
    }
    for (;;)
    {
        do
            c = get_char_in_cond_exp();
        while (isspace(c));
    
        if (!ispunct(c))
            break;

        if (!(x = m_optab[c]))
            break;

        value2 = *this->m_out++;
        for (;; x += 3)
        {
            if (!m_optab2[x])
            {
                this->m_out--;
                if (!m_optab2[x + 1])
                {
                    lex_errorp("illegal operator use in %cif.");
                    return 0;
                }
                break;
            }
            if (value2 == m_optab2[x])
                break;
        }
        if (priority >= m_optab2[x + 2])
        {
            if (m_optab2[x]) *--this->m_out = (char) value2;
            break;
        }
        value2 = cond_get_exp(m_optab2[x + 2]);
        switch (m_optab2[x + 1])
        {
        case MULT: 
            value *= value2; 
            break;
        case DIV:
            if (value2)
                value /= value2;
            else
                lex_errorp("division by 0 in %cif.");
            break;
        case MOD:
            if (value2)
                value %= value2;
            else
                lex_errorp("modulo by 0 in %cif.");
            break;
        case BPLUS:
            value += value2;
            break;
        case BMINUS:
            value -= value2;
            break;
        case LSHIFT:
            value <<= value2;
            break;
        case RSHIFT:
            value >>= value2;
            break;
        case LESS:
            value = value < value2;
            break;
        case LEQ:
            value = value <= value2;
            break;
        case GREAT:
            value = value > value2;
            break;
        case GEQ:
            value = value >= value2;
            break;
        case EQ:
            value = value == value2;
            break;
        case NEQ:
            value = value != value2;
            break;
        case BAND:
            value &= value2;
            break;
        case XOR:
            value ^= value2;
            break;
        case BOR:
            value |= value2;
            break;
        case LAND:
            value = value && value2;
            break;
        case LOR:
            value = value || value2;
            break;
        case QMARK:
            do
                c = get_char_in_cond_exp();
            while (isspace(c));
            if (c != ':')
            {
                lex_error("'?' without ':' in #if.");
                this->m_out--;
                return 0;
            }
            if (value)
            {
                cond_get_exp(1);
                value = value2;
            } else
                value = cond_get_exp(1);
            break;
        }
    }
    this->m_out--;
    return value;
}

void Lexer::handle_cond(IntR c)
{
    IfStatement *p;

    if (!c)
        skip_to("else", "endif");
    p = (IfStatement *) BUFFER_NEW(IfStatement);
    p->next = this->m_if_top;
    this->m_if_top = p;
    p->state = c ? IfStatement::EXPECT_ENDIF : IfStatement::EXPECT_ELSE;
}

void Lexer::lex_error(const char *msg)
{
    m_lang_context->syntax_error(msg);
}

void Lexer::lex_errorp(const char *msg)
{
    char buf[200];
    snprintf(buf, sizeof(buf), msg, '#');
    buf[sizeof(buf) - 1] = 0;
    lex_error(buf);
}

void Lexer::lex_stop(ErrorCode error_code)
{
    m_lang_context->syntax_stop(error_code);
}

// Skip token to expected one
bool Lexer::skip_to(const char *token, const char *atoken)
{
    char b[20], *p;
    char c;
    char *yyp = this->m_out, *startp;
    char *b_end = b + 19;
    IntR nest;

    for (nest = 0;;)
    {
        /* Skip space in the head of line */
        while (isspace(c = *yyp++));

        /* Meet a pre-compiler char '#'? */
        if (c == '#')
        {
            while (isspace(c = *yyp++));
            startp = yyp - 1;
            for (p = b; !isspace(c) && c != LEX_EOF; c = *yyp++)
            {
                if (p < b_end) *p++ = c;
                else break;
            }
            *p = 0;
            if (!strcmp(b, "if") || !strcmp(b, "ifdef") || !strcmp(b, "ifndef"))
            {
                nest++;
            } else if (nest > 0)
            {
                if (!strcmp(b, "endif"))
                    nest--;
            } else
            {
                if (!strcmp(b, token))
                {
                    this->m_out = startp;
                    *--this->m_out = '#';
                    return true;
                } else
                if (atoken != NULL && !strcmp(b, atoken))
                {
                    this->m_out = startp;
                    *--this->m_out = '#';
                    return false;
                } else
                if (!strcmp(b, "elif"))
                {
                    this->m_out = startp;
                    *--this->m_out = '#';
                    return (atoken == 0);
                }
            }
        }

        while (c != '\n' && c != LEX_EOF) c = *yyp++;
        if (c == LEX_EOF)
        {
            lex_error("Unexpected end of file while skipping");
            this->m_out = yyp - 1;
            return true;
        }

        m_current_line++;
        m_total_lines++;
        if (yyp == this->m_last_new_line + 1)
        {
            this->m_out = yyp;
            refill_buffer();
            yyp = this->m_out;
        }
    }
}

void Lexer::handle_pragma(char *name)
{
    UintR  flags;
    char  *p;

    for (p = name; *p && !isspace(*p); p++)
        ;
    *p = 0;

    if (strcmp(name, "???") == 0)
    {
        // m_default_attrib |= ???;
    }
    else
        lex_errors("Unknow pragma: %s.\n", name);
}

/* Update m_current_file_path.c_str() & m_current_line information of source file */
void Lexer::set_current_line_file(char *linefile)
{
    char *p, *p2;

    /* Skip white space */
    while (isspace((unsigned char) *linefile))
        linefile++;

    /* Find next separator */
    p = strchr(linefile, ' ');
    if (p == 0)
    {
        lex_error("Invalid #line, expected #line nnn file_name.");
        return;
    }

    /* Skip white space */
    while (*p && isspace((unsigned char) *p))
        p++;

    p2 = p;
    while (*p2 && !isspace((unsigned char) *p2))
        p2++;
    *p2 = 0;

    /* Update current file/line information */
    m_current_file_path = add_file_name(p);
    m_current_line = atoi(linefile);

    /* Skip #pragma line, so decrease m_current_line by 1 */
    m_current_line--;
}

/* Skip current line in buffer */
void Lexer::skip_line()
{
    IntR c;
    char *yyp = this->m_out;

    while (((c = *yyp) != '\n') && (c != LEX_EOF))
        yyp++;

    /* Next read of this '\n' will do vm_refillBuffer() if neccesary */
    this->m_out = yyp;
}

void Lexer::skip_comment()
{
    IntR c = '*';
    char *yyp = this->m_out;

    for (;;)
    {
        while ((c = *yyp++) != '*')
        {
            if (c == LEX_EOF)
            {
                this->m_out = --yyp;
                lex_error("End of file in a comment");
                return;
            }
            if (c == '\n')
            {
                m_current_line++;
                if (yyp == this->m_last_new_line + 1)
                {
                    this->m_out = yyp;
                    refill_buffer();
                    yyp = this->m_out;
                }
            }
        }
        if (*(yyp - 2) == '/')
            lex_warn("/* found in comment.");
        do
        {
            if ((c = *yyp++) == '/')
            {
                this->m_out = yyp;
                return;
            }
            if (c == '\n')
            {
                m_current_line++;
                if (yyp == this->m_last_new_line + 1)
                {
                    this->m_out = yyp;
                    refill_buffer();
                    yyp = this->m_out;
                }
            }
        } while (c == '*');
    }
}

/* Generate a dir & file name by m_current_file_path & path */
void Lexer::generate_file_dir_name()
{
    char buf[MAX_FILE_NAME_LEN];
    const char *tmp, *file;
    UintR len;

    if (!m_current_file_path.length())
    {
        m_current_file_string = "\"/Unknown File\"";
        m_current_pure_file_string = "\"Unknown File\"";
        m_current_dir_string = "\"/\"";
        return;
    }

    // Copy current file name
    len = m_current_file_path.length();
    const char *file_c_str = m_current_file_path.c_str();
    if (len > sizeof(buf) - 4)
        len = sizeof(buf) - 4;
    buf[0] = '"';
    memcpy(buf + 1, file_c_str, len);
    buf[len + 1] = '"';
    buf[len + 2] = 0;
    m_current_file_string = buf;

    if ((tmp = strrchr(file_c_str, PATH_SEPARATOR)) == NULL &&
        (tmp = strrchr(file_c_str, '\\')) == NULL)
        // No path separator in file name?
        file = tmp = file_c_str;
    else
        file = tmp + 1;

    // Fetch pure file name
    len = strlen(file);
    buf[0] = '"';
    if (len > sizeof(buf) - 4)
        len = sizeof(buf) - 4;
    memcpy(buf + 1, file, len);
    buf[len + 1] = '"';
    buf[len + 2] = 0;
    m_current_pure_file_string = buf;

    // Fetch directory
    if (tmp == m_current_file_path.c_str())
    {
        // No path name, set as root
        snprintf(buf, sizeof(buf),
                 "\"%c\"", PATH_SEPARATOR);
        buf[sizeof(buf) - 1] = 0;
    } else
    {
        len = tmp - m_current_file_path.c_str() + 1;
        if (len > sizeof(buf) - 4)
            len = sizeof(buf) - 4;
        buf[0] = '"';
        memcpy(buf + 1, m_current_file_path.c_str(), len);
        buf[len + 1] = '"';
        buf[len + 2] = 0;
    }
    m_current_dir_string = buf;
}

void Lexer::del_trail(char *sp)
{
    char *p;

    p = sp;
    if (!*p)
        lex_error("Illegal # command");
    else
    {
        while (*p && !isspace(*p))
            p++;
        *p = 0;
    }
}

#define SAVEC \
    if (yyp < this->m_text + MAXLINE - 5) \
       *yyp++ = c; \
    else \
    { \
       lex_error("Line too long"); \
       break; \
    }

// Return current line
LineNo Lexer::get_current_line()
{
    if (m_fixed_line)
        return m_fixed_line;

    return m_current_line;
}

/* Get default attrib of compiler */
LexerAttrib Lexer::get_default_attrib()
{
    return m_default_attrib;
}

/* Set default attrib of compiler */
bool Lexer::set_default_attrib(LexerAttrib attrib)
{
    m_default_attrib = attrib;
    return true;
}

/* Trim two side space of a string */
void Lexer::trim_to(char *str, size_t size, const char *from)
{
    const char *p;
    size_t n;

    /* Forward search */
    while (*from && isspace((unsigned char) *from))
        from++;

    if (!*from)
    {
        /* Empty string */
        str[0] = 0;
        return;
    }

    /* Back search */
    p = from + strlen(from);
    while (isspace((unsigned char) p[-1]))
        p--;
    /* p must large than from since str is not an empty string */
    STD_ASSERT(p > from);

    if ((size_t) (p - from) >= size)
        /* String is too long */
        n = size - 1;
    else
        /* Full copy */
        n = p - from;

    /* Do copy & return string */
    memcpy(str, from, n);
    str[n] = 0;
}

// Load data from file & fill the process buffer
void Lexer::refill_buffer()
{
    char*  p;
    char*  end;
    size_t i, size;
    bool   is_end_of_file;

    /* Here we are sure that we need more from the file */
    /* Assume this->m_out is one beyond a newline at this->m_last_new_line */
    /* or after an #include .... */

    /* First check if there's enough space at the end */
    end = this->m_current_buf->buf + DEFMAX;
    if (end - this->m_current_buf->buf_end > MAXLINE + 5)
    {
        p = this->m_current_buf->buf_end;
    } else
    {
        /* No more space at the end */
        size = (size_t) (this->m_current_buf->buf_end - this->m_out + 1);  /* Include newline */
        memcpy(this->m_current_buf->buf, this->m_out - 1, (UintR) size);
        this->m_out = this->m_current_buf->buf + 1;
        p = this->m_out + size - 1;
    }

    /* Read data from file */                
    STD_ASSERT(p + MAXLINE + 5 <= end);
    size = fread(p, MAXLINE, 1, (FILE *)this->m_in_file_fd);////----

    /* Is end of file? */
    is_end_of_file = (size < MAXLINE);

    /* Trim BOM head */
    /* I check here per read so I can make sure the BOM header must be
     * processed, how ever, the BOM shouldn't appeared in file, so the
     * follow codes won't make any error */
    if (size >= 3 && p[0] == '\xEF' && p[1] == '\xBB' && p[2] == '\xBF')
    {
        /* Meet BOOM, trim it */
        size -= 3;
        memmove(p, p + 3, size);
    }

    if (size <= 0)
        p[size = 0] = 0;
    else
        if (this->m_is_start)
        {
            /* Read first byte to check crypt-flag */

            if (p[0] == '!')
            {
                /* This is crypt file (1) */
                this->m_in_crypt_type = LEX_CRYPT_TYPE_XOR;
                this->m_in_crypt_code = 0xA5;

                /* Remove first byte */
                memmove(p, p + 1, --size);
            } else
            if (p[0] == '*')
            {
                /* This is crypt file (2) */
                this->m_in_crypt_type = LEX_CRYPT_TYPE_SHIFT;
                this->m_in_crypt_code = 0xB6;

                /* Remove first byte */
                memmove(p, p + 1, --size);
            }

            /* Clear flag */
            this->m_is_start = false;
        }

        switch (this->m_in_crypt_type)
        {
        case LEX_CRYPT_TYPE_XOR:
            /* Restore source file by type 1 (xor only) */
            for (i = 0; i < size; i++)
                p[i] ^= this->m_in_crypt_code;
            break;

        case LEX_CRYPT_TYPE_SHIFT:
            /* Restore source file by type 2 (xor & shift) */
            for (i = 0; i < size; i++)
            {
                p[i] ^= this->m_in_crypt_code;
                this->m_in_crypt_code = ((this->m_in_crypt_code * 3 + 5) ^ 0x27) & 0xFF;
            }
            break;

        default:
            // Do nothing
            break;
        }

    this->m_current_buf->buf_end = (p += size);
    if (is_end_of_file)
    {
        if (this->m_out > p) this->m_out = p;
        *(this->m_last_new_line = p) = LEX_EOF;
        this->m_current_buf->buf_end++;
        return;
    }
    while (*--p != '\n');
    if (p == this->m_out - 1)
    {
        lex_error("Line too long.");
        *(this->m_last_new_line = this->m_current_buf->buf_end - 1) = '\n';
        return;
    }
    this->m_last_new_line = p;
    return;
}

#define returnAssign(opcode)    { yylval.number = opcode; return L_ASSIGN; }
#define returnOrder(opcode)     { yylval.number = opcode; return L_ORDER;  }

/* Lex in */
IntR Lexer::lex_in()
{
    static char partial[MAXLINE + 5];   /* extra 5 for safety buffer */
    IntR   isReal;
    char  *partp;

    char *yyp;         /* Xeno */
    char c;            /* Xeno */

    this->m_text[0] = 0;

    partp = partial;            /* Xeno */
    partial[0] = 0;             /* Xeno */

    /* After a token, a new word in this line is increased. */
    this->m_line_words++;

    for (;;)
    {
        if (this->m_num_lex_fatal > 0)
            return -1;

        switch (c = *this->m_out++)
        {
        case LEX_EOF:
            if (this->m_if_top != 0)
            {
                IfStatement *p = this->m_if_top;
                                
                lex_error(p->state == IfStatement::EXPECT_ENDIF ? "Missing #endif." : "Missing #else/#elif.");
                this->m_if_top = 0;
            }
            this->m_out--;
            return -1;

        case '\n':
            {
                this->m_line_words = 1;
                m_current_line++;
                this->m_total_lines++;
                if (this->m_out == this->m_last_new_line + 1)
                    refill_buffer();
            }
        case ' ':
        case '\t':
        case '\f':
        case '\v':
        case '\r':
            break;

        case '+':
            switch(*this->m_out++)
            {
                case '+': return L_INC;
                case '=': returnAssign(F_ADD_EQ);
                default: this->m_out--; return '+';
            }

        case '-':
            switch(*this->m_out++)
            {
                case '>': return L_ARROW;
                case '-': return L_DEC;
                case '=': returnAssign(F_SUB_EQ);
                default: this->m_out--; return '-';
            }

        case '&':
            switch(*this->m_out++)
            {
                case '&': return L_LAND;
                case '=': returnAssign(F_AND_EQ);
                default: this->m_out--; return '&';
            }

        case '|':
            switch(*this->m_out++)
            {
                case '|': return L_LOR;
                case '=': returnAssign(F_OR_EQ);
                default: this->m_out--; return '|';
            }

        case '^':
            if (*this->m_out++ == '=') returnAssign(F_XOR_EQ);
            this->m_out--;
            return '^';

        case '<':
            switch(*this->m_out++)
            {
                case '<':
                {
                    if (*this->m_out++ == '=') returnAssign(F_LSH_EQ);
                    this->m_out--;
                    return L_LSH;
                }
                case '=': returnOrder(F_LE);
                default: this->m_out--; return '<';
            }

        case '>':
            switch(*this->m_out++)
            {
                case '>':
                {
                    if (*this->m_out++ == '=') returnAssign(F_RSH_EQ);
                    this->m_out--;
                    return L_RSH;
                }
                case '=': returnOrder(F_GE);
                default: this->m_out--; returnOrder(F_GT);
            }

        case '*':
            if (*this->m_out++ == '=') returnAssign(F_MULT_EQ);
            this->m_out--;
            return '*';

        case '%':
            if (*this->m_out++ == '=') returnAssign(F_MOD_EQ);
            this->m_out--;
            return '%';

        case '/':
            switch(*this->m_out++)
            {
                case '*': skip_comment(); break;
                case '/': skip_line(); break;
                case '=': returnAssign(F_DIV_EQ);
                default: this->m_out--; return '/';
            }
            break;

        case '=':
            switch(*this->m_out++)
            {
                case '>': return L_EXPAND_ARROW;
                case '=': return L_EQ;
            }

            this->m_out--;
            yylval.number = F_ASSIGN;
            return L_ASSIGN;

        case '(':
            yyp = this->m_out;
            while (isspace(c = *yyp++))
            {
                if (c == '\n')
                {
                    m_current_line++;
                    if (yyp == this->m_last_new_line + 1)
                    {
                        this->m_out = yyp;
                        refill_buffer();
                        yyp = this->m_out;
                    }
                } 
            }

            switch (c)
            {
                case '{' : { this->m_out = yyp; return L_ARRAY_OPEN; }
                case '[' : { this->m_out = yyp; return L_MAPPING_OPEN; }
                case ':' :
                {
                    if (*yyp == ':' && *(yyp + 1) != ':')
                    {
                        /* Following ':', must be ( ::, not (: :: */
                        this->m_out = yyp - 1;
                        return '(';
                    }

                    /* (: */
                    this->m_out = yyp; return L_FUNCTION_OPEN;
                }
                default:
                {
                    this->m_out = yyp - 1;
                    return '(';
                }
            }

        case ';':
            this->m_line_words = 0;
            return c;

        case ')':
        case '{':
        case '}':
        case '[':
        case ']':
        case ',':
        case '~':
        case '?':
            return c;

        case '!':
            if (*this->m_out++ == '=') return L_NE;
            this->m_out--;
            return L_NOT;

        case ':':
            if (*this->m_out++ == ':') return L_COLON_COLON;
            this->m_out--;
            return ':';

        case '.':
            if (*this->m_out++ == '.')
            {
                if (*this->m_out++ == '.')
                    return L_DOT_DOT_DOT;
                this->m_out--;
                return L_RANGE;
            }
            this->m_out--;
            return '.';

        case '#':
        {
            /* Must lead by space */
            char *check;
            bool is_lead_by_space_flag = true;

            check = this->m_out - 2;
            while (*check != '\n')
            {
                if (!isspace(*check))
                {
                    /* Not lead by white space */
                    is_lead_by_space_flag = 0;
                    break;
                }
                check--;
            }

            if (is_lead_by_space_flag)
            {
                char *sp = NULL;
                bool quote;

                while (isspace(c = *this->m_out++));
                yyp = this->m_text;

                for (quote = false;;)
                {
                    if (c == '"')
                        quote = !quote;
                    else
                    if (c == '/' && !quote)
                    {
                        if (*this->m_out == '*')
                        {
                            this->m_out++;
                            skip_comment();
                            c = *this->m_out++;
                        } else
                        if (*this->m_out == '/')
                        {
                            this->m_out++;
                            skip_line();
                            c = *this->m_out++;
                        }
                    }

                    if (sp == NULL && isspace(c))
                        sp = yyp;
                    if (c == '\n' || c == LEX_EOF) break;
                    SAVEC;
                    c = *this->m_out++;
                }

                if (sp != NULL)
                {
                    *sp++ = 0;
                    while (isspace(*sp))
                        sp++;
                } else
                    sp = yyp;

                *yyp = 0;
                if (this->m_out == this->m_last_new_line + 1) refill_buffer();

                if (strcmp("error", this->m_text) == 0)
                {
                    lex_error(sp);
                    lex_stop(COMPILE_ERROR);
                } else
                if (strcmp("warning", this->m_text) == 0)
                {
                    lex_warn(sp);
                } else
                if (strcmp("line", this->m_text) == 0)
                {
                    /* Set compiling source file/line information */
                    set_current_line_file(sp);
                } else
                if (strcmp("pragma", this->m_text) == 0)
                    handle_pragma(sp);
                else
                if (strcmp("if", this->m_text) == 0)
                {
                    IntR cond;

                    *--this->m_out = '\0';
                    add_input(sp);
                    cond = cond_get_exp(0);
                    if (*this->m_out++)
                    {
                        lex_error("Condition too complex in #if.");
                        while (*this->m_out++);
                    } else
                        handle_cond(cond);
                } else if (strcmp("elif", this->m_text) == 0)
                {
                    handle_elif(sp);
                } else if (strcmp("else", this->m_text) == 0)
                {
                    handle_else();
                } else if (strcmp("endif", this->m_text) == 0)
                {
                    handle_endif();
                } else if (strcmp("echo", this->m_text) == 0)
                {
                    lex_echo(sp);
                } else if (strcmp("fixedline", this->m_text) == 0)
                {
                    this->m_fixed_line = (LineNo)atoi(sp);
                } else if (strcmp("freeline", this->m_text) == 0)
                {
                    m_current_line = (LineNo)atoi(sp) - 1;
                    this->m_fixed_line = 0;
                } else
                {
                    lex_error("Unrecognised # directive.");
                }
                *--this->m_out = c;
                break;
            } else
                return '#';
        }

        case '$':
            if (*this->m_out++ == '$')
            {
                const char *str = "";

                if (isdigit(*this->m_out))
                {
                    /* Got a digit */
                    IntR stringId;

                    stringId = strtol(this->m_out, &this->m_out, 10, false);
////----                    str = vm_findResourceString(stringId);
                    str = NULL;
                    if (str == NULL)
                    {
                        str = "";
                        lex_errors("Bad resource string (id = %d).\n", (int) stringId);
                    }
                } else
                {
                    lex_error("Bad resource string, expected $$n, got $$ only.\n");
                }

                yylval.string = String(str);
                return L_STRING;
            }
            this->m_out--;
            return '$';

        case '\'':
        {
            char *save_out = this->m_out;
            size_t char_count = sizeof(Integer);
            Integer ch;
            /* Accept wide char len <= sizeof(IntR) */
            /* Ignore format such as: ''1' -> quick show */
            if (*this->m_out == '\'')
            {
                /* '', treat as quick show command */
                this->m_out = save_out;
                yylval.number = 0;
                return '\'';
            }

            yylval.number = 0;
            while (char_count > 0)
            {
                if (*this->m_out++ == '\\')
                {
                    switch(*this->m_out++)
                    {
                    case 'n': ch = '\n'; break;
                    case 't': ch = '\t'; break;
                    case 'r': ch = '\r'; break;
                    case 'b': ch = '\b'; break;
                    case 'a': ch = '\x07'; break;
                    case 'e': ch = '\x1b'; break;
                    case '\'': ch = '\''; break;
                    case '\"': ch = '\"'; break;
                    case '\\': ch = '\\'; break;
                    case '0': case '1': case '2': case '3': case '4': 
                    case '5': case '6': case '7': case '8': case '9':
                        this->m_out--;
                        ch = strtol(this->m_out, &this->m_out, 8, false);
                        if (ch > 255)
                        {
                            lex_warn("Illegal character constant; interpreting as 'x' instead.");
                            ch = 'x';
                        }
                        break;
                    case 'x':
                        if (!isxdigit(*this->m_out))
                        {
                            ch = 'x';
                            lex_warn("\\x must be followed by a valid hex value; interpreting as 'x' instead.");
                        } else
                        {
                            ch = strtol(this->m_out, &this->m_out, 16, false);
                            if (ch > 255)
                            {
                                lex_warn("Illegal character constant.");
                                ch = 'x';
                            }
                        }
                        break;
                    case '\n':
                        ch = '\n';
                        m_current_line++;
                        this->m_total_lines++;
                        if (this->m_out == this->m_last_new_line + 1) refill_buffer();
                        break;
                    default: 
                        lex_warn("Unknown \\ escape.");
                        ch = *(this->m_out - 1);
                        break;
                    }
                } else
                {
                    /* Get raw char */
                    ch = (Integer) (unsigned char) *(this->m_out - 1);
                }
                /* Append the char as low 8 bits */
                yylval.number = (yylval.number << 8) | (IntR) (unsigned char) ch;

                /* Peek next char */
                char_count--;
                if (*this->m_out == '\'')
                    break;
            }

            if (*this->m_out++ != '\'')
            {
                /* Single ' (can't find another ' in following chars),
                 * treat as quick show command */
                this->m_out = save_out;
                yylval.number = 0;
                return '\'';
            }

            return L_NUMBER;
        }

        case '\"':
        {
            IntR l;
            IntR is_section_string = 0;
            char sectionMark;
            char *to;

            if (this->m_out + 3 <= this->m_last_new_line + 1 && this->m_out[0] == '\"' && this->m_out[1] == '\"')
            {
                /* """, means the following section is a string */

                /* Set flag of section string */
                is_section_string = 1;

                /* Save section mark (should be alphalet) */
                sectionMark = this->m_out[2];
                if (!isalpha((unsigned char) sectionMark))
                {
                    lex_error("Bad mark of section string, expected alphalet (a-z, A-Z) following \"\"\"");
                    return LEX_EOF;
                }

                /* Skip """(mark), try find new line */
                this->m_out += 3;

                /* For section string, remove follow space */
                for (;;)
                {
                    if (this->m_out == this->m_last_new_line + 1)
                        refill_buffer();

                    c = *this->m_out++;
                    if (c == LEX_EOF)
                    {
                        /* End of file while string is not completed */
                        lex_error("End of file in section string");
                        return LEX_EOF;
                    }

                    if (c == '\n')
                    {
                        /* Got newline */
                        m_current_line++;
                        this->m_total_lines++;
                        if (this->m_out == this->m_last_new_line + 1) refill_buffer();
                        break;
                    }

                    if (!isspace((unsigned char) c))
                    {
                        /* Bad char after """ */
                        lex_error("Bad char after \"\"\", expected new line");
                        return LEX_EOF;
                    }
                }
            }

            // Put string to m_text
            l = MAXLINE;
            yyp = this->m_text;
            while (l--)
            {
                switch (c = *this->m_out++)
                {
                    case LEX_EOF:
                        lex_error("End of file in string");
                        return LEX_EOF;

                    case '"':
                    {
                        char *res, *start;
                        size_t len;

                        if (is_section_string)
                        {
                            /* Try to match whole """ */
                            if (this->m_out + 3 < this->m_last_new_line + 1 && this->m_out[0] == '"' && this->m_out[1] == '"' &&
                                this->m_out[2] == sectionMark)
                            {
                                /* Got """(mark), section string is completed */
                                this->m_out += 3;
                            } else
                            {
                                /* String is not complete, treat as normal char */
                                *yyp++ = c;
                                break;
                            }
                        }

                        yylval.string = String(m_text, yyp - this->m_text);
                        return L_STRING;
                    }
                        
                    case '\n':
                        m_current_line++;
                        this->m_total_lines++;
                        if (this->m_out == this->m_last_new_line + 1) refill_buffer();
                        *yyp++ = '\n';
                        break;

                    case '\\':
                        if (is_section_string)
                        {
                            /* Don't make escape char in section string */
                            *yyp++ = c;
                            break;
                        }

                        /* Don't copy the \ in yet */
                        switch(*this->m_out++)
                        {
                        case '\r':
                            while (*this->m_out == '\r') this->m_out++;
                            if (*this->m_out != '\n')
                            {
                                lex_error("Illegal character after '\\'");
                                break;
                            }

                            /* Fall through */
                        case '\n':
                            m_current_line++;
                            this->m_total_lines++;
                            if (this->m_out == this->m_last_new_line + 1) refill_buffer();
                            l++; /* Nothing is copied */
                            break;
                        case LEX_EOF:
                            lex_error("End of file in string");
                            return LEX_EOF;
                        case 'n': *yyp++ = '\n'; break;
                        case 't': *yyp++ = '\t'; break;
                        case 'r': *yyp++ = '\r'; break;
                        case 'b': *yyp++ = '\b'; break;
                        case 'a': *yyp++ = '\x07'; break;
                        case 'e': *yyp++ = '\x1b'; break;
                        case '"': *yyp++ = '"'; break;
                        case '\\': *yyp++ = '\\'; break;
                        case '0': case '1': case '2': case '3': case '4': 
                        case '5': case '6': case '7': case '8': case '9':
                        {
                            IntR tmp;
                            this->m_out--;
                            tmp = strtol(this->m_out, &this->m_out, 8, false);
                            if (tmp > 255)
                            {
                                lex_warn("Illegal character constant in string.");
                                tmp = 'x';
                            }
                            *yyp++ = (char) tmp;
                            break;
                        }

                        case 'x':
                        {
                            IntR tmp;
                            if (!isxdigit(*this->m_out))
                            {
                                *yyp++ = 'x';
                                lex_warn("\\x must be followed by a valid hex value; interpreting as 'x' instead.");
                            } else
                            {
                                tmp = strtol(this->m_out, &this->m_out, 16, false);
                                if (tmp > 255)
                                {
                                    lex_warn("Illegal character constant.");
                                    tmp = 'x';
                                }
                                *yyp++ = (char) tmp;
                            }
                            break;
                        }

                        default:
                            *yyp++ = *(this->m_out - 1);
                            lex_warn("Unknown \\ escape.");
                            break;
                        }
                        break;

                    default: *yyp++ = c; break;
                }
            }
    
            /* Not even enough length, declare too long string error */
            lex_errors("Size of long string > %d.\n", MAXLINE);
            lex_error("String too long.");
            *yyp++ = '\0';
            yylval.string = String(m_text, yyp - this->m_text);
            return L_STRING;
        }

        case '0':
            c = *this->m_out++;
            if (c == '.' || c == 'f')
            {
                /* Must be a float */
                /* Decrease this->m_out to fall down case '1'... */
                c = '0';
                this->m_out--;
            } else
            if (c == 'X' || c == 'x')
            {
                /* Heximal */
                yyp = this->m_text;
                for (;;)
                {
                    c = *this->m_out++;
                    SAVEC;
                    if (!isxdigit(c))
                        break;
                }
                this->m_out--;
                yylval.number = (Integer) strtol(this->m_text, (char **) NULL,
                                                 0x10, false);
                return L_NUMBER;
            } else
            if (c == 'b' || c == 'B')
            {
                /* Binary */
                yyp = this->m_text;
                for (;;)
                {
                    c = *this->m_out++;
                    SAVEC;
                    if (!isdigit(c))
                        break;
                }
                this->m_out--;
                yylval.number = (Integer) strtol(this->m_text, (char **) NULL,
                                                 0x2, false);
                return L_NUMBER;
            } else
            {
                this->m_out--;

                if (!isdigit(c))
                {
                    yylval.number = 0;
                    return L_NUMBER;
                }

                /* Octect */
                yyp = this->m_text;
                for (;;)
                {
                    c = *this->m_out++;
                    SAVEC;
                    if (!isdigit(c))
                        break;
                }
                this->m_out--;
                yylval.number = (Integer) strtol(this->m_text, (char **) NULL,
                                                 0x8, false);
                return L_NUMBER;
            }
            /* Fall through */

        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
            isReal = 0;
            yyp = this->m_text;
            *yyp++ = c;
            for (;;)
            {
                c = *this->m_out++;
                if (c == '.')
                {
                    if (!isReal)
                    {
                        isReal = 1;
                    } else
                    {
                        isReal = 0;
                        this->m_out--;
                        break;
                    }
                } else
                if (!isdigit(c))
                {
                    if (c == 'f')
                    {
                        /* Following f, must be a float */
                        this->m_out++;
                        isReal = 1;
                    }

                    break;
                }

                SAVEC;
            }
            this->m_out--;
            *yyp = 0;
            if (isReal)
            {
                yylval.real = (Real) atof(this->m_text);
                return L_REAL;
            } else
            {
                yylval.number = strtol(this->m_text, NULL, 10, false);
                return L_NUMBER;
            }

        default:
            if (isalpha(c) || c == '_')
            {
                IntR r;
                char oldC;
                char *oldOut;
                char c2;

#if 1
                /* Get Identifier, counting on '.' */
                yyp = this->m_text;
                oldOut = this->m_out;
                *yyp++ = oldC = c;
                for (;;)
                {
                    c = *this->m_out++;

                    if (isalnum(c) || c == '_')
                    {
                        SAVEC;
                        continue;
                    }

                    if (c == '.')
                    {
                        /* Check next character */
                        if (isalnum(*this->m_out) || (*this->m_out) == '_')
                        {
                            SAVEC;
                            continue;
                        }
                    }
                    
                    break;
                }
                *yyp = 0;

                /* Got an identifier may contain '.', treat the whole string as identifier */
                yylval.string = String(this->m_text);
                return L_IDENTIFIER;
#endif

                /* Other possibilities */
                this->m_out--;
                        
                /* look up identifier */
                auto word = String(this->m_text);

                /* 1.lookup key word */
                {
                    Keyword* keyword = get_keyword(word);
                    if (keyword != NULL)
                    {
                        yylval.number = keyword->sem_value; 
                        return keyword->token & TOKEN_MASK;
                    }
                }

                /* 2.lookup function argument */
                if (m_lang_context->m_in_function)
                {
                    auto& args = m_lang_context->m_in_function->arg_list.args;
                    for (auto& it : args)
                    {
                        if (it.name != word)
                            continue;

                        yylval.varDesc.name = it.name;
                        yylval.varDesc.isGlobal = 0;
                        yylval.varDesc.type = it.var_type;
                        return L_DEFINED_NAME;
                    }
                }

                /* lookup local variables */
                if (m_lang_context->m_in_function)
                {
                    auto& vars = m_lang_context->m_in_function->local_vars;
                    for (auto& it : vars)
                    {
                        if (it.name != word)
                            continue;

                        yylval.varDesc.name = it.name;
                        yylval.varDesc.isGlobal = 0;
                        yylval.varDesc.type = it.type;
                        it.is_used = true;
                        return L_DEFINED_NAME;
                    }
                }

                /* 3.lookup member variables */
                {
                    auto* var = m_lang_context->syntax_get_member_variable(word);
                    if (var)
                    {
                        yylval.varDesc.name = word;
                        yylval.varDesc.isGlobal = 0;
                        yylval.varDesc.type = var->type;
                        var->is_used = true;
                        return L_DEFINED_NAME;
                    }
                }

                yylval.string = word;
                return L_IDENTIFIER;
            }
            goto badlex;
        }
    }

badlex:
    lex_errors("Illegal character (0x%02X) '%c'.", (unsigned char)c, (char)c);
    return ' ';
}

/* Add a file name & return a value
 * ATTENTION: The string will be put into shared pool. And the string pointed
 * by the return pointer won't be free during all VM running period. So user can
 * use the return pointer directly. */
StringImpl* Lexer::add_file_name(const String& file_name)
{
    char regular_name[MAX_FILE_NAME_LEN];
    const char *c_str;

    do
    {
        if (file_name.length() == 0)
            c_str = UNKNOW_FILE_NAME;
        else
            c_str = file_name.c_str();

#if false
        /* Or regluar the file name first */
        if (!derive_file_name(file, regularName, sizeof(regularName)))
            /* Failed to derive the regular name */
            return NULL;
#else
        strncpy(regular_name, c_str, sizeof(regular_name));
        regular_name[sizeof(regular_name) - 1] = 0;
#endif
} while (0);

    String name = STRING_ALLOC(regular_name);
    StringImpl *string_in_pool = Program::find_or_add_string(name);

    // Put into list
    std_enter_critical_section(m_cs);
    m_file_name_list->push_back(string_in_pool);
    std_leave_critical_section(m_cs);
}

/* Lookup in added file name list, return the regular name */
StringImpl* Lexer::find_file_name(const String& file_name)
{
    char regular_name[MAX_FILE_NAME_LEN];
    const char *c_str;

    do
    {
        if (file_name.length() == 0)
            c_str = UNKNOW_FILE_NAME;
        else
            c_str = file_name.c_str();

#if false
        /* Or regluar the file name first */
        if (!derive_file_name(file, regularName, sizeof(regularName)))
            /* Failed to derive the regular name */
            return NULL;
#else
        strncpy(regular_name, c_str, sizeof(regular_name));
        regular_name[sizeof(regular_name) - 1] = 0;
#endif
    } while (0);

    String name = STRING_ALLOC(regular_name);
    return Program::find_string(name);
}

/* Lookup all file name name list, try to find nearest file name */
/* For file name a/b.c, can match x/a/b.c or y/x/a/b.c ... */
/* If matched multi file name, return NULL */
StringImpl* Lexer::find_file_name_not_exact_match(const String& file_name)
{
    // Find in shared string
    StringImpl *name;
    name = find_file_name(file_name);
    if (name != NULL)
        return name;

    /* Lookup all file names, try to match */
    StringImpl* matched_name = NULL;
    std_enter_critical_section(m_cs);
    for (auto& it : *m_file_name_list)
    {
        if (it->length() < file_name.length())
            /* Not matched!The length of file name is less then
             * "file" */
            continue;

        if (stricmp(it->c_str() + it->length() - file_name.length(),
                    file_name.c_str()) != 0)
            /* Not matched! */
            continue;

        if (file_name[0] != PATH_SEPARATOR &&
            it->length() > file_name.length() &&
            (*it)[it->length() - file_name.length() - 1] != PATH_SEPARATOR)
        {
            /* The name is not match near separator */
            /* For example:
             * full name: /daemons/logd.c
             * /logd.c & logd.c can matched, but ogd.c can't matched.
             * So, the first char of file should be separator '/' or the 
             * prior char in regular name should be '/'.
             */
            continue;
        }

        /* Matched!Check & save result */
        if (matched_name != NULL)
        {
            /* There is already a matched file name? Ambigous */
            STD_TRACE("Ambiguous file name to find: %s.\n", file_name.c_str());
            matched_name = NULL;
            break;
        }
        matched_name = it;
    }
    std_leave_critical_section(m_cs);

    /* Done!*/
    return matched_name;
}

/* Destruct all file names */
/* This function can't be called only if the system is shutdown &
 * the shared string's pool is to be cleared.
 * The function is used to detected string leak. */
void Lexer::destruct_file_names()
{
    std_enter_critical_section(m_cs);
    m_file_name_list->clear();
    std_leave_critical_section(m_cs);
}

/* End of compiled. Close the file */
/* The definesNeedFree is a flag indicate should I free all
 * the MACRO defines? It's set by the vm_startNewFile(). */
bool Lexer::end_new_file(bool succ)
{
    this->m_if_top = NULL;
    return succ;
}

/* Open a new file to compile */
/* pProgram: compile to which program... NULL means compile to
 * global program. */
bool Lexer::start_new_file(Program *program, IntR fd, const String& file_name)
{
    IntR len, slen;

    /* Generate full file name */
    generate_file_dir_name();

    // Default attribution of compiler
    m_default_attrib = (LexerAttrib)0;

    // For crypt type
    m_is_start = false;
    m_in_crypt_type = LEX_NO_CRYPT;

    /* Initialize YACC information & file information */
    this->m_in_file_fd = fd;
    this->m_num_lex_fatal = 0;
    this->m_current_buf = &m_main_buf;
    this->m_current_buf->buf_end = this->m_out = this->m_current_buf->buf + (DEFMAX >> 1);
    *(this->m_last_new_line = this->m_out - 1) = '\n';
    m_lang_context->set_current_attrib(m_default_attrib);
    this->m_current_line = 1;
    this->m_current_line_base = 0;
    this->m_current_line_saved = 0;
    this->m_line_words = 0;

    /* Add source current file name into file list */
    /* Save file name to "m_lang_context->currentInputFile" */
    m_current_file_path = add_file_name(file_name);

    /* Compile script file, fill line buffer first */
    refill_buffer();

    return true;
}

void Lexer::get_next_char(IntR *ptr_ch)
{
    IntR ch;
    if ((ch = (Uint8)*this->m_out++) == '\n' &&
        this->m_out == this->m_last_new_line + 1)
        refill_buffer();
}

/* Copy n bytes from p to [to, to_end] */
void Lexer::get_alpha_string_to(char* p, char* to, char* to_end)
{
    while (isalnum(*p))
    {
        *to = *p++;
        if (to < to_end)
        {
            to++;
        } else
        {
            lex_error("Name too long");
            return;
        }
    }
    *to++ = 0;
}

IntR Lexer::cmy_get_char()
{
    IntR c;

    for (;;)
    {
        get_next_char(&c);
        if (c == '/')
        {
            switch (*this->m_out++)
            {
                case '*': skip_comment(); break;
                case '/': skip_line(); break;
                default: this->m_out--; return c;
            }
        } else
        {
            return c;
        }
    }
}

void Lexer::refill()
{
    char *p;
    IntR c;

    p = this->m_text;
    do
    {
        c = *this->m_out++;
        if (p < this->m_text + MAXLINE - 5)
            *p++ = (char) c;
        else
        {
            lex_error("Line too long");
            break;
        }
    } while (c != '\n' && c != LEX_EOF);
    if ((c == '\n') && (this->m_out == this->m_last_new_line + 1))
        refill_buffer();
    p[-1] = ' ';
    *p = 0;
    m_current_line++;
}

/* IDEA: linked buffers, to allow "unlimited" buffer expansion */
void Lexer::add_input(char *p)
{
    size_t l = strlen(p);

    if (l + (this->m_current_buf->buf_end - this->m_out) >= DEFMAX - 10)
    {
        lex_error("Macro expansion buffer overflow");
        return;
    }

    if (this->m_out < l + 5 + this->m_current_buf->buf)
    {
        /* Move this->m_out->... to right */
        IntR shift = this->m_current_buf->buf + l + 5 - this->m_out;
        char *q = this->m_current_buf->buf_end - 1;
        char *to = q + shift;
        while (q >= this->m_out)
            *(to--) = *(q--);
        this->m_out += shift;
        this->m_last_new_line += shift;
        this->m_current_buf->buf_end += shift;
    }

    this->m_out -= l;
    strncpy(this->m_out, p, l);
}

#if 0
void Lexer::skip_white()
{
    do
    {
        c = cmy_get_char();
        if (c == '\n') {
            m_current_line ++;
            this->m_total_lines ++;
        }
    } while (isspace(c));
}
#endif

void Lexer::skip_white()
{
    IntR c;
    do
    {
        c = (Uint8)*this->m_out++;
    } while (isspace(c));
    this->m_out--;
}

IntR Lexer::get_char_in_cond_exp()
{
    char c, *yyp;

    c = *this->m_out++;
    while (isalpha(c) || c == '_')
    {
        yyp = this->m_text;
        do
        {
            SAVEC;
            c = *this->m_out++;
        } while (isalnum(c));
        this->m_out--;
        *yyp = '\0';
        c = *this->m_out++;
    }
    return c;
}

/* Get keyword token if input name is keyword, or return -1*/
Keyword *Lexer::get_keyword(const String& name)
{
    Keyword* keyword;
    if (m_keywords->try_get(name, &keyword))
        return keyword;

    // Not found in keyword
    return NULL;
}

}
