/* A Bison parser, made by GNU Bison 3.8.2.  */

/* Bison implementation for Yacc-like parsers in C

   Copyright (C) 1984, 1989-1990, 2000-2015, 2018-2021 Free Software Foundation,
   Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

/* C LALR(1) parser skeleton written by Richard Stallman, by
   simplifying the original so-called "semantic" parser.  */

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

/* All symbols defined below should begin with yy or YY, to avoid
   infringing on user name space.  This should be done even for local
   variables, as they might otherwise be expanded by user macros.
   There are some unavoidable exceptions within include files to
   define necessary library symbols; they are noted "INFRINGES ON
   USER NAME SPACE" below.  */

/* Identify Bison output, and Bison version.  */
#define YYBISON 30802

/* Bison version string.  */
#define YYBISON_VERSION "3.8.2"

/* Skeleton name.  */
#define YYSKELETON_NAME "yacc.c"

/* Pure parsers.  */
#define YYPURE 0

/* Push parsers.  */
#define YYPUSH 0

/* Pull parsers.  */
#define YYPULL 1




/* First part of user prologue.  */
#line 2 "src/utils/expr_parser.y"

#define _GNU_SOURCE
#include <cstring>
#include <cstdio>

/* Need full definition for %union */
#include "expr_ast.hpp"

int yylex(void);
void yyerror(const char *s);
extern int yylineno;

extern ExprNode* g_parser_result;
extern const char* g_input_string;

#line 87 "src/utils/expr_parser.cpp"

# ifndef YY_CAST
#  ifdef __cplusplus
#   define YY_CAST(Type, Val) static_cast<Type> (Val)
#   define YY_REINTERPRET_CAST(Type, Val) reinterpret_cast<Type> (Val)
#  else
#   define YY_CAST(Type, Val) ((Type) (Val))
#   define YY_REINTERPRET_CAST(Type, Val) ((Type) (Val))
#  endif
# endif
# ifndef YY_NULLPTR
#  if defined __cplusplus
#   if 201103L <= __cplusplus
#    define YY_NULLPTR nullptr
#   else
#    define YY_NULLPTR 0
#   endif
#  else
#   define YY_NULLPTR ((void*)0)
#  endif
# endif

#include "expr_parser.hpp"
/* Symbol kind.  */
enum yysymbol_kind_t
{
  YYSYMBOL_YYEMPTY = -2,
  YYSYMBOL_YYEOF = 0,                      /* "end of file"  */
  YYSYMBOL_YYerror = 1,                    /* error  */
  YYSYMBOL_YYUNDEF = 2,                    /* "invalid token"  */
  YYSYMBOL_NUMBER = 3,                     /* NUMBER  */
  YYSYMBOL_CHAR_LITERAL = 4,               /* CHAR_LITERAL  */
  YYSYMBOL_IDENTIFIER = 5,                 /* IDENTIFIER  */
  YYSYMBOL_TRACEE_N = 6,                   /* TRACEE_N  */
  YYSYMBOL_KW_SIZEOF = 7,                  /* KW_SIZEOF  */
  YYSYMBOL_KW_OFFSETOF = 8,                /* KW_OFFSETOF  */
  YYSYMBOL_KW_TYPEOF = 9,                  /* KW_TYPEOF  */
  YYSYMBOL_KW_STRUCT = 10,                 /* KW_STRUCT  */
  YYSYMBOL_KW_UNION = 11,                  /* KW_UNION  */
  YYSYMBOL_KW_ENUM = 12,                   /* KW_ENUM  */
  YYSYMBOL_KW_INT = 13,                    /* KW_INT  */
  YYSYMBOL_KW_CHAR = 14,                   /* KW_CHAR  */
  YYSYMBOL_KW_SHORT = 15,                  /* KW_SHORT  */
  YYSYMBOL_KW_LONG = 16,                   /* KW_LONG  */
  YYSYMBOL_KW_FLOAT = 17,                  /* KW_FLOAT  */
  YYSYMBOL_KW_DOUBLE = 18,                 /* KW_DOUBLE  */
  YYSYMBOL_KW_VOID = 19,                   /* KW_VOID  */
  YYSYMBOL_KW_UNSIGNED = 20,               /* KW_UNSIGNED  */
  YYSYMBOL_KW_SIGNED = 21,                 /* KW_SIGNED  */
  YYSYMBOL_KW_CONST = 22,                  /* KW_CONST  */
  YYSYMBOL_KW_STATIC = 23,                 /* KW_STATIC  */
  YYSYMBOL_ARROW = 24,                     /* ARROW  */
  YYSYMBOL_EQ = 25,                        /* EQ  */
  YYSYMBOL_NE = 26,                        /* NE  */
  YYSYMBOL_LE = 27,                        /* LE  */
  YYSYMBOL_GE = 28,                        /* GE  */
  YYSYMBOL_AND = 29,                       /* AND  */
  YYSYMBOL_OR = 30,                        /* OR  */
  YYSYMBOL_SHL = 31,                       /* SHL  */
  YYSYMBOL_SHR = 32,                       /* SHR  */
  YYSYMBOL_INC = 33,                       /* INC  */
  YYSYMBOL_DEC = 34,                       /* DEC  */
  YYSYMBOL_INVALID = 35,                   /* INVALID  */
  YYSYMBOL_36_ = 36,                       /* '='  */
  YYSYMBOL_37_ = 37,                       /* '?'  */
  YYSYMBOL_38_ = 38,                       /* ':'  */
  YYSYMBOL_39_ = 39,                       /* '|'  */
  YYSYMBOL_40_ = 40,                       /* '^'  */
  YYSYMBOL_41_ = 41,                       /* '&'  */
  YYSYMBOL_42_ = 42,                       /* '<'  */
  YYSYMBOL_43_ = 43,                       /* '>'  */
  YYSYMBOL_44_ = 44,                       /* '+'  */
  YYSYMBOL_45_ = 45,                       /* '-'  */
  YYSYMBOL_46_ = 46,                       /* '*'  */
  YYSYMBOL_47_ = 47,                       /* '/'  */
  YYSYMBOL_48_ = 48,                       /* '%'  */
  YYSYMBOL_49_ = 49,                       /* '.'  */
  YYSYMBOL_50_ = 50,                       /* '['  */
  YYSYMBOL_51_ = 51,                       /* ']'  */
  YYSYMBOL_52_ = 52,                       /* '('  */
  YYSYMBOL_53_ = 53,                       /* ')'  */
  YYSYMBOL_54_ = 54,                       /* '!'  */
  YYSYMBOL_55_ = 55,                       /* '~'  */
  YYSYMBOL_56_ = 56,                       /* ','  */
  YYSYMBOL_YYACCEPT = 57,                  /* $accept  */
  YYSYMBOL_input = 58,                     /* input  */
  YYSYMBOL_assignment_expr = 59,           /* assignment_expr  */
  YYSYMBOL_conditional_expr = 60,          /* conditional_expr  */
  YYSYMBOL_logical_or_expr = 61,           /* logical_or_expr  */
  YYSYMBOL_logical_and_expr = 62,          /* logical_and_expr  */
  YYSYMBOL_or_expr = 63,                   /* or_expr  */
  YYSYMBOL_xor_expr = 64,                  /* xor_expr  */
  YYSYMBOL_and_expr = 65,                  /* and_expr  */
  YYSYMBOL_equality_expr = 66,             /* equality_expr  */
  YYSYMBOL_relational_expr = 67,           /* relational_expr  */
  YYSYMBOL_shift_expr = 68,                /* shift_expr  */
  YYSYMBOL_additive_expr = 69,             /* additive_expr  */
  YYSYMBOL_multiplicative_expr = 70,       /* multiplicative_expr  */
  YYSYMBOL_cast_expr = 71,                 /* cast_expr  */
  YYSYMBOL_type_name = 72,                 /* type_name  */
  YYSYMBOL_type_specifier = 73,            /* type_specifier  */
  YYSYMBOL_unary_expr = 74,                /* unary_expr  */
  YYSYMBOL_postfix_expr = 75,              /* postfix_expr  */
  YYSYMBOL_primary_expr = 76,              /* primary_expr  */
  YYSYMBOL_expression = 77                 /* expression  */
};
typedef enum yysymbol_kind_t yysymbol_kind_t;




#ifdef short
# undef short
#endif

/* On compilers that do not define __PTRDIFF_MAX__ etc., make sure
   <limits.h> and (if available) <stdint.h> are included
   so that the code can choose integer types of a good width.  */

#ifndef __PTRDIFF_MAX__
# include <limits.h> /* INFRINGES ON USER NAME SPACE */
# if defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#  include <stdint.h> /* INFRINGES ON USER NAME SPACE */
#  define YY_STDINT_H
# endif
#endif

/* Narrow types that promote to a signed type and that can represent a
   signed or unsigned integer of at least N bits.  In tables they can
   save space and decrease cache pressure.  Promoting to a signed type
   helps avoid bugs in integer arithmetic.  */

#ifdef __INT_LEAST8_MAX__
typedef __INT_LEAST8_TYPE__ yytype_int8;
#elif defined YY_STDINT_H
typedef int_least8_t yytype_int8;
#else
typedef signed char yytype_int8;
#endif

#ifdef __INT_LEAST16_MAX__
typedef __INT_LEAST16_TYPE__ yytype_int16;
#elif defined YY_STDINT_H
typedef int_least16_t yytype_int16;
#else
typedef short yytype_int16;
#endif

/* Work around bug in HP-UX 11.23, which defines these macros
   incorrectly for preprocessor constants.  This workaround can likely
   be removed in 2023, as HPE has promised support for HP-UX 11.23
   (aka HP-UX 11i v2) only through the end of 2022; see Table 2 of
   <https://h20195.www2.hpe.com/V2/getpdf.aspx/4AA4-7673ENW.pdf>.  */
#ifdef __hpux
# undef UINT_LEAST8_MAX
# undef UINT_LEAST16_MAX
# define UINT_LEAST8_MAX 255
# define UINT_LEAST16_MAX 65535
#endif

#if defined __UINT_LEAST8_MAX__ && __UINT_LEAST8_MAX__ <= __INT_MAX__
typedef __UINT_LEAST8_TYPE__ yytype_uint8;
#elif (!defined __UINT_LEAST8_MAX__ && defined YY_STDINT_H \
       && UINT_LEAST8_MAX <= INT_MAX)
typedef uint_least8_t yytype_uint8;
#elif !defined __UINT_LEAST8_MAX__ && UCHAR_MAX <= INT_MAX
typedef unsigned char yytype_uint8;
#else
typedef short yytype_uint8;
#endif

#if defined __UINT_LEAST16_MAX__ && __UINT_LEAST16_MAX__ <= __INT_MAX__
typedef __UINT_LEAST16_TYPE__ yytype_uint16;
#elif (!defined __UINT_LEAST16_MAX__ && defined YY_STDINT_H \
       && UINT_LEAST16_MAX <= INT_MAX)
typedef uint_least16_t yytype_uint16;
#elif !defined __UINT_LEAST16_MAX__ && USHRT_MAX <= INT_MAX
typedef unsigned short yytype_uint16;
#else
typedef int yytype_uint16;
#endif

#ifndef YYPTRDIFF_T
# if defined __PTRDIFF_TYPE__ && defined __PTRDIFF_MAX__
#  define YYPTRDIFF_T __PTRDIFF_TYPE__
#  define YYPTRDIFF_MAXIMUM __PTRDIFF_MAX__
# elif defined PTRDIFF_MAX
#  ifndef ptrdiff_t
#   include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  endif
#  define YYPTRDIFF_T ptrdiff_t
#  define YYPTRDIFF_MAXIMUM PTRDIFF_MAX
# else
#  define YYPTRDIFF_T long
#  define YYPTRDIFF_MAXIMUM LONG_MAX
# endif
#endif

#ifndef YYSIZE_T
# ifdef __SIZE_TYPE__
#  define YYSIZE_T __SIZE_TYPE__
# elif defined size_t
#  define YYSIZE_T size_t
# elif defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#  include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  define YYSIZE_T size_t
# else
#  define YYSIZE_T unsigned
# endif
#endif

#define YYSIZE_MAXIMUM                                  \
  YY_CAST (YYPTRDIFF_T,                                 \
           (YYPTRDIFF_MAXIMUM < YY_CAST (YYSIZE_T, -1)  \
            ? YYPTRDIFF_MAXIMUM                         \
            : YY_CAST (YYSIZE_T, -1)))

#define YYSIZEOF(X) YY_CAST (YYPTRDIFF_T, sizeof (X))


/* Stored state numbers (used for stacks). */
typedef yytype_uint8 yy_state_t;

/* State numbers in computations.  */
typedef int yy_state_fast_t;

#ifndef YY_
# if defined YYENABLE_NLS && YYENABLE_NLS
#  if ENABLE_NLS
#   include <libintl.h> /* INFRINGES ON USER NAME SPACE */
#   define YY_(Msgid) dgettext ("bison-runtime", Msgid)
#  endif
# endif
# ifndef YY_
#  define YY_(Msgid) Msgid
# endif
#endif


#ifndef YY_ATTRIBUTE_PURE
# if defined __GNUC__ && 2 < __GNUC__ + (96 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_PURE __attribute__ ((__pure__))
# else
#  define YY_ATTRIBUTE_PURE
# endif
#endif

#ifndef YY_ATTRIBUTE_UNUSED
# if defined __GNUC__ && 2 < __GNUC__ + (7 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_UNUSED __attribute__ ((__unused__))
# else
#  define YY_ATTRIBUTE_UNUSED
# endif
#endif

/* Suppress unused-variable warnings by "using" E.  */
#if ! defined lint || defined __GNUC__
# define YY_USE(E) ((void) (E))
#else
# define YY_USE(E) /* empty */
#endif

/* Suppress an incorrect diagnostic about yylval being uninitialized.  */
#if defined __GNUC__ && ! defined __ICC && 406 <= __GNUC__ * 100 + __GNUC_MINOR__
# if __GNUC__ * 100 + __GNUC_MINOR__ < 407
#  define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                           \
    _Pragma ("GCC diagnostic push")                                     \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")
# else
#  define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                           \
    _Pragma ("GCC diagnostic push")                                     \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")              \
    _Pragma ("GCC diagnostic ignored \"-Wmaybe-uninitialized\"")
# endif
# define YY_IGNORE_MAYBE_UNINITIALIZED_END      \
    _Pragma ("GCC diagnostic pop")
#else
# define YY_INITIAL_VALUE(Value) Value
#endif
#ifndef YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_END
#endif
#ifndef YY_INITIAL_VALUE
# define YY_INITIAL_VALUE(Value) /* Nothing. */
#endif

#if defined __cplusplus && defined __GNUC__ && ! defined __ICC && 6 <= __GNUC__
# define YY_IGNORE_USELESS_CAST_BEGIN                          \
    _Pragma ("GCC diagnostic push")                            \
    _Pragma ("GCC diagnostic ignored \"-Wuseless-cast\"")
# define YY_IGNORE_USELESS_CAST_END            \
    _Pragma ("GCC diagnostic pop")
#endif
#ifndef YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_END
#endif


#define YY_ASSERT(E) ((void) (0 && (E)))

#if !defined yyoverflow

/* The parser invokes alloca or malloc; define the necessary symbols.  */

# ifdef YYSTACK_USE_ALLOCA
#  if YYSTACK_USE_ALLOCA
#   ifdef __GNUC__
#    define YYSTACK_ALLOC __builtin_alloca
#   elif defined __BUILTIN_VA_ARG_INCR
#    include <alloca.h> /* INFRINGES ON USER NAME SPACE */
#   elif defined _AIX
#    define YYSTACK_ALLOC __alloca
#   elif defined _MSC_VER
#    include <malloc.h> /* INFRINGES ON USER NAME SPACE */
#    define alloca _alloca
#   else
#    define YYSTACK_ALLOC alloca
#    if ! defined _ALLOCA_H && ! defined EXIT_SUCCESS
#     include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
      /* Use EXIT_SUCCESS as a witness for stdlib.h.  */
#     ifndef EXIT_SUCCESS
#      define EXIT_SUCCESS 0
#     endif
#    endif
#   endif
#  endif
# endif

# ifdef YYSTACK_ALLOC
   /* Pacify GCC's 'empty if-body' warning.  */
#  define YYSTACK_FREE(Ptr) do { /* empty */; } while (0)
#  ifndef YYSTACK_ALLOC_MAXIMUM
    /* The OS might guarantee only one guard page at the bottom of the stack,
       and a page size can be as small as 4096 bytes.  So we cannot safely
       invoke alloca (N) if N exceeds 4096.  Use a slightly smaller number
       to allow for a few compiler-allocated temporary stack slots.  */
#   define YYSTACK_ALLOC_MAXIMUM 4032 /* reasonable circa 2006 */
#  endif
# else
#  define YYSTACK_ALLOC YYMALLOC
#  define YYSTACK_FREE YYFREE
#  ifndef YYSTACK_ALLOC_MAXIMUM
#   define YYSTACK_ALLOC_MAXIMUM YYSIZE_MAXIMUM
#  endif
#  if (defined __cplusplus && ! defined EXIT_SUCCESS \
       && ! ((defined YYMALLOC || defined malloc) \
             && (defined YYFREE || defined free)))
#   include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
#   ifndef EXIT_SUCCESS
#    define EXIT_SUCCESS 0
#   endif
#  endif
#  ifndef YYMALLOC
#   define YYMALLOC malloc
#   if ! defined malloc && ! defined EXIT_SUCCESS
void *malloc (YYSIZE_T); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
#  ifndef YYFREE
#   define YYFREE free
#   if ! defined free && ! defined EXIT_SUCCESS
void free (void *); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
# endif
#endif /* !defined yyoverflow */

#if (! defined yyoverflow \
     && (! defined __cplusplus \
         || (defined YYSTYPE_IS_TRIVIAL && YYSTYPE_IS_TRIVIAL)))

/* A type that is properly aligned for any stack member.  */
union yyalloc
{
  yy_state_t yyss_alloc;
  YYSTYPE yyvs_alloc;
};

/* The size of the maximum gap between one aligned stack and the next.  */
# define YYSTACK_GAP_MAXIMUM (YYSIZEOF (union yyalloc) - 1)

/* The size of an array large to enough to hold all stacks, each with
   N elements.  */
# define YYSTACK_BYTES(N) \
     ((N) * (YYSIZEOF (yy_state_t) + YYSIZEOF (YYSTYPE)) \
      + YYSTACK_GAP_MAXIMUM)

# define YYCOPY_NEEDED 1

/* Relocate STACK from its old location to the new one.  The
   local variables YYSIZE and YYSTACKSIZE give the old and new number of
   elements in the stack, and YYPTR gives the new location of the
   stack.  Advance YYPTR to a properly aligned location for the next
   stack.  */
# define YYSTACK_RELOCATE(Stack_alloc, Stack)                           \
    do                                                                  \
      {                                                                 \
        YYPTRDIFF_T yynewbytes;                                         \
        YYCOPY (&yyptr->Stack_alloc, Stack, yysize);                    \
        Stack = &yyptr->Stack_alloc;                                    \
        yynewbytes = yystacksize * YYSIZEOF (*Stack) + YYSTACK_GAP_MAXIMUM; \
        yyptr += yynewbytes / YYSIZEOF (*yyptr);                        \
      }                                                                 \
    while (0)

#endif

#if defined YYCOPY_NEEDED && YYCOPY_NEEDED
/* Copy COUNT objects from SRC to DST.  The source and destination do
   not overlap.  */
# ifndef YYCOPY
#  if defined __GNUC__ && 1 < __GNUC__
#   define YYCOPY(Dst, Src, Count) \
      __builtin_memcpy (Dst, Src, YY_CAST (YYSIZE_T, (Count)) * sizeof (*(Src)))
#  else
#   define YYCOPY(Dst, Src, Count)              \
      do                                        \
        {                                       \
          YYPTRDIFF_T yyi;                      \
          for (yyi = 0; yyi < (Count); yyi++)   \
            (Dst)[yyi] = (Src)[yyi];            \
        }                                       \
      while (0)
#  endif
# endif
#endif /* !YYCOPY_NEEDED */

/* YYFINAL -- State number of the termination state.  */
#define YYFINAL  63
/* YYLAST -- Last index in YYTABLE.  */
#define YYLAST   217

/* YYNTOKENS -- Number of terminals.  */
#define YYNTOKENS  57
/* YYNNTS -- Number of nonterminals.  */
#define YYNNTS  21
/* YYNRULES -- Number of rules.  */
#define YYNRULES  74
/* YYNSTATES -- Number of states.  */
#define YYNSTATES  132

/* YYMAXUTOK -- Last valid token kind.  */
#define YYMAXUTOK   290


/* YYTRANSLATE(TOKEN-NUM) -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex, with out-of-bounds checking.  */
#define YYTRANSLATE(YYX)                                \
  (0 <= (YYX) && (YYX) <= YYMAXUTOK                     \
   ? YY_CAST (yysymbol_kind_t, yytranslate[YYX])        \
   : YYSYMBOL_YYUNDEF)

/* YYTRANSLATE[TOKEN-NUM] -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex.  */
static const yytype_int8 yytranslate[] =
{
       0,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,    54,     2,     2,     2,    48,    41,     2,
      52,    53,    46,    44,    56,    45,    49,    47,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,    38,     2,
      42,    36,    43,    37,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,    50,     2,    51,    40,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,    39,     2,    55,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     1,     2,     3,     4,
       5,     6,     7,     8,     9,    10,    11,    12,    13,    14,
      15,    16,    17,    18,    19,    20,    21,    22,    23,    24,
      25,    26,    27,    28,    29,    30,    31,    32,    33,    34,
      35
};

#if YYDEBUG
/* YYRLINE[YYN] -- Source line where rule number YYN was defined.  */
static const yytype_uint8 yyrline[] =
{
       0,    61,    61,    65,    66,    70,    71,    77,    78,    82,
      83,    87,    88,    92,    93,    97,    98,   102,   103,   104,
     108,   109,   110,   111,   112,   116,   117,   118,   122,   123,
     124,   128,   129,   130,   131,   135,   136,   140,   141,   142,
     147,   155,   156,   157,   158,   159,   160,   161,   162,   163,
     167,   168,   169,   170,   171,   172,   173,   174,   175,   176,
     177,   181,   186,   192,   193,   194,   195,   196,   197,   198,
     206,   207,   208,   209,   213
};
#endif

/** Accessing symbol of state STATE.  */
#define YY_ACCESSING_SYMBOL(State) YY_CAST (yysymbol_kind_t, yystos[State])

#if YYDEBUG || 0
/* The user-facing name of the symbol whose (internal) number is
   YYSYMBOL.  No bounds checking.  */
static const char *yysymbol_name (yysymbol_kind_t yysymbol) YY_ATTRIBUTE_UNUSED;

/* YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
   First, the terminals, then, starting at YYNTOKENS, nonterminals.  */
static const char *const yytname[] =
{
  "\"end of file\"", "error", "\"invalid token\"", "NUMBER",
  "CHAR_LITERAL", "IDENTIFIER", "TRACEE_N", "KW_SIZEOF", "KW_OFFSETOF",
  "KW_TYPEOF", "KW_STRUCT", "KW_UNION", "KW_ENUM", "KW_INT", "KW_CHAR",
  "KW_SHORT", "KW_LONG", "KW_FLOAT", "KW_DOUBLE", "KW_VOID", "KW_UNSIGNED",
  "KW_SIGNED", "KW_CONST", "KW_STATIC", "ARROW", "EQ", "NE", "LE", "GE",
  "AND", "OR", "SHL", "SHR", "INC", "DEC", "INVALID", "'='", "'?'", "':'",
  "'|'", "'^'", "'&'", "'<'", "'>'", "'+'", "'-'", "'*'", "'/'", "'%'",
  "'.'", "'['", "']'", "'('", "')'", "'!'", "'~'", "','", "$accept",
  "input", "assignment_expr", "conditional_expr", "logical_or_expr",
  "logical_and_expr", "or_expr", "xor_expr", "and_expr", "equality_expr",
  "relational_expr", "shift_expr", "additive_expr", "multiplicative_expr",
  "cast_expr", "type_name", "type_specifier", "unary_expr", "postfix_expr",
  "primary_expr", "expression", YY_NULLPTR
};

static const char *
yysymbol_name (yysymbol_kind_t yysymbol)
{
  return yytname[yysymbol];
}
#endif

#define YYPACT_NINF (-53)

#define yypact_value_is_default(Yyn) \
  ((Yyn) == YYPACT_NINF)

#define YYTABLE_NINF (-39)

#define yytable_value_is_error(Yyn) \
  0

/* YYPACT[STATE-NUM] -- Index in YYTABLE of the portion describing
   STATE-NUM.  */
static const yytype_int16 yypact[] =
{
      14,   -53,   -53,   -53,   -45,   109,    -1,    59,   162,   162,
     162,   162,   162,   162,    86,   162,   162,    49,   -53,   -53,
      -6,    34,    70,    85,    92,   -22,    -2,    66,    77,    39,
     -53,    90,     3,   -53,    14,    86,   -53,    57,    14,    14,
     -53,   -53,   -53,   -53,   -53,   -53,    -3,   129,   -53,   -53,
     -53,   -53,   -53,   -53,   -53,   -53,   -53,   -53,     8,   -53,
      82,   -53,   -53,   -53,    14,    14,    14,    14,    14,    14,
      14,    14,    14,    14,    14,    14,    14,    14,    14,    14,
      14,    14,    14,    14,   131,   -53,   -53,   132,    14,    91,
      11,   -53,   -14,    93,   -53,   -53,    14,   -53,    34,   -53,
     101,    70,    85,    92,   -22,    -2,    -2,    66,    66,    66,
      66,    77,    77,    39,    39,   -53,   -53,   -53,   -53,   -53,
     -53,    94,   -53,   -53,   142,   -53,   -53,    14,   -53,    95,
     -53,   -53
};

/* YYDEFACT[STATE-NUM] -- Default reduction number in state STATE-NUM.
   Performed when YYTABLE does not specify something else to do.  Zero
   means the default is an error.  */
static const yytype_int8 yydefact[] =
{
       0,    70,    71,    72,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     2,     3,
       5,     7,     9,    11,    13,    15,    17,    20,    25,    28,
      31,    35,    50,    63,     0,     0,    59,     0,     0,     0,
      51,    52,    53,    55,    56,    54,    72,     0,    41,    42,
      43,    44,    45,    46,    47,    48,    49,    74,     0,    37,
       0,    57,    58,     1,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,    67,    68,     0,     0,     0,
       0,    38,     0,     0,    39,    40,     0,    73,     8,    35,
       0,    10,    12,    14,    16,    18,    19,    23,    24,    21,
      22,    26,    27,    29,    30,    32,    33,    34,     4,    66,
      65,     0,    69,    60,     0,    62,    36,     0,    64,     0,
       6,    61
};

/* YYPGOTO[NTERM-NUM].  */
static const yytype_int8 yypgoto[] =
{
     -53,   -53,     1,    22,   -53,    87,    96,    89,    84,    88,
     -25,     7,    47,    50,   -52,    73,   -53,     0,   -53,   -53,
     -32
};

/* YYDEFGOTO[NTERM-NUM].  */
static const yytype_int8 yydefgoto[] =
{
       0,    17,    57,    19,    20,    21,    22,    23,    24,    25,
      26,    27,    28,    29,    30,    58,    59,    99,    32,    33,
      60
};

/* YYTABLE[YYPACT[STATE-NUM]] -- What to do in state STATE-NUM.  If
   positive, shift that token.  If negative, reduce the rule whose
   number is the opposite.  If YYTABLE_NINF, syntax error.  */
static const yytype_int16 yytable[] =
{
      31,    18,    89,    70,    71,    36,    93,    34,    40,    41,
      42,    43,    44,    45,    31,    61,    62,     1,     2,     3,
       4,     5,     6,     7,    64,    72,    73,    84,   115,   116,
     117,    65,    95,   100,    31,    31,    85,    86,    31,    31,
      74,    75,   124,   -38,   126,   105,   106,     8,     9,    63,
     -38,    37,    87,    88,    95,    10,   121,    95,    11,    12,
      13,    96,    91,    66,   123,    31,    14,    47,    15,    16,
      48,    49,    50,    51,    52,    53,    54,    55,    56,   107,
     108,   109,   110,    31,   118,    80,    81,    82,    31,     1,
       2,    46,     4,     5,     6,     7,    47,    76,    77,    48,
      49,    50,    51,    52,    53,    54,    55,    56,    90,    67,
      92,    38,     1,     2,     3,     4,     5,     6,     7,     8,
       9,    78,    79,   111,   112,    68,    83,    10,   113,   114,
      11,    12,    13,    69,    94,    97,   119,   120,    14,   127,
      15,    16,     8,     9,   122,   128,   125,   129,   131,   130,
      10,    98,   103,    11,    12,    13,   102,   104,     0,     0,
       0,    35,   101,    15,    16,     1,     2,     3,     4,     5,
       6,     7,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     8,     9,     0,     0,     0,
       0,     0,     0,    10,     0,     0,    11,    12,    13,     0,
       0,     0,     0,     0,    39,     0,    15,    16
};

static const yytype_int8 yycheck[] =
{
       0,     0,    34,    25,    26,     5,    38,    52,     8,     9,
      10,    11,    12,    13,    14,    15,    16,     3,     4,     5,
       6,     7,     8,     9,    30,    27,    28,    24,    80,    81,
      82,    37,    46,    65,    34,    35,    33,    34,    38,    39,
      42,    43,    56,    46,    96,    70,    71,    33,    34,     0,
      53,    52,    49,    50,    46,    41,    88,    46,    44,    45,
      46,    53,     5,    29,    53,    65,    52,    10,    54,    55,
      13,    14,    15,    16,    17,    18,    19,    20,    21,    72,
      73,    74,    75,    83,    83,    46,    47,    48,    88,     3,
       4,     5,     6,     7,     8,     9,    10,    31,    32,    13,
      14,    15,    16,    17,    18,    19,    20,    21,    35,    39,
      37,    52,     3,     4,     5,     6,     7,     8,     9,    33,
      34,    44,    45,    76,    77,    40,    36,    41,    78,    79,
      44,    45,    46,    41,     5,    53,     5,     5,    52,    38,
      54,    55,    33,    34,    53,    51,    53,     5,    53,   127,
      41,    64,    68,    44,    45,    46,    67,    69,    -1,    -1,
      -1,    52,    66,    54,    55,     3,     4,     5,     6,     7,
       8,     9,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    33,    34,    -1,    -1,    -1,
      -1,    -1,    -1,    41,    -1,    -1,    44,    45,    46,    -1,
      -1,    -1,    -1,    -1,    52,    -1,    54,    55
};

/* YYSTOS[STATE-NUM] -- The symbol kind of the accessing symbol of
   state STATE-NUM.  */
static const yytype_int8 yystos[] =
{
       0,     3,     4,     5,     6,     7,     8,     9,    33,    34,
      41,    44,    45,    46,    52,    54,    55,    58,    59,    60,
      61,    62,    63,    64,    65,    66,    67,    68,    69,    70,
      71,    74,    75,    76,    52,    52,    74,    52,    52,    52,
      74,    74,    74,    74,    74,    74,     5,    10,    13,    14,
      15,    16,    17,    18,    19,    20,    21,    59,    72,    73,
      77,    74,    74,     0,    30,    37,    29,    39,    40,    41,
      25,    26,    27,    28,    42,    43,    31,    32,    44,    45,
      46,    47,    48,    36,    24,    33,    34,    49,    50,    77,
      72,     5,    72,    77,     5,    46,    53,    53,    62,    74,
      77,    63,    64,    65,    66,    67,    67,    68,    68,    68,
      68,    69,    69,    70,    70,    71,    71,    71,    59,     5,
       5,    77,    53,    53,    56,    53,    71,    38,    51,     5,
      60,    53
};

/* YYR1[RULE-NUM] -- Symbol kind of the left-hand side of rule RULE-NUM.  */
static const yytype_int8 yyr1[] =
{
       0,    57,    58,    59,    59,    60,    60,    61,    61,    62,
      62,    63,    63,    64,    64,    65,    65,    66,    66,    66,
      67,    67,    67,    67,    67,    68,    68,    68,    69,    69,
      69,    70,    70,    70,    70,    71,    71,    72,    72,    72,
      72,    73,    73,    73,    73,    73,    73,    73,    73,    73,
      74,    74,    74,    74,    74,    74,    74,    74,    74,    74,
      74,    74,    74,    75,    75,    75,    75,    75,    75,    75,
      76,    76,    76,    76,    77
};

/* YYR2[RULE-NUM] -- Number of symbols on the right-hand side of rule RULE-NUM.  */
static const yytype_int8 yyr2[] =
{
       0,     2,     1,     1,     3,     1,     5,     1,     3,     1,
       3,     1,     3,     1,     3,     1,     3,     1,     3,     3,
       1,     3,     3,     3,     3,     1,     3,     3,     1,     3,
       3,     1,     3,     3,     3,     1,     4,     1,     1,     2,
       2,     1,     1,     1,     1,     1,     1,     1,     1,     1,
       1,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       4,     6,     4,     1,     4,     3,     3,     2,     2,     4,
       1,     1,     1,     3,     1
};


enum { YYENOMEM = -2 };

#define yyerrok         (yyerrstatus = 0)
#define yyclearin       (yychar = YYEMPTY)

#define YYACCEPT        goto yyacceptlab
#define YYABORT         goto yyabortlab
#define YYERROR         goto yyerrorlab
#define YYNOMEM         goto yyexhaustedlab


#define YYRECOVERING()  (!!yyerrstatus)

#define YYBACKUP(Token, Value)                                    \
  do                                                              \
    if (yychar == YYEMPTY)                                        \
      {                                                           \
        yychar = (Token);                                         \
        yylval = (Value);                                         \
        YYPOPSTACK (yylen);                                       \
        yystate = *yyssp;                                         \
        goto yybackup;                                            \
      }                                                           \
    else                                                          \
      {                                                           \
        yyerror (YY_("syntax error: cannot back up")); \
        YYERROR;                                                  \
      }                                                           \
  while (0)

/* Backward compatibility with an undocumented macro.
   Use YYerror or YYUNDEF. */
#define YYERRCODE YYUNDEF


/* Enable debugging if requested.  */
#if YYDEBUG

# ifndef YYFPRINTF
#  include <stdio.h> /* INFRINGES ON USER NAME SPACE */
#  define YYFPRINTF fprintf
# endif

# define YYDPRINTF(Args)                        \
do {                                            \
  if (yydebug)                                  \
    YYFPRINTF Args;                             \
} while (0)




# define YY_SYMBOL_PRINT(Title, Kind, Value, Location)                    \
do {                                                                      \
  if (yydebug)                                                            \
    {                                                                     \
      YYFPRINTF (stderr, "%s ", Title);                                   \
      yy_symbol_print (stderr,                                            \
                  Kind, Value); \
      YYFPRINTF (stderr, "\n");                                           \
    }                                                                     \
} while (0)


/*-----------------------------------.
| Print this symbol's value on YYO.  |
`-----------------------------------*/

static void
yy_symbol_value_print (FILE *yyo,
                       yysymbol_kind_t yykind, YYSTYPE const * const yyvaluep)
{
  FILE *yyoutput = yyo;
  YY_USE (yyoutput);
  if (!yyvaluep)
    return;
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YY_USE (yykind);
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}


/*---------------------------.
| Print this symbol on YYO.  |
`---------------------------*/

static void
yy_symbol_print (FILE *yyo,
                 yysymbol_kind_t yykind, YYSTYPE const * const yyvaluep)
{
  YYFPRINTF (yyo, "%s %s (",
             yykind < YYNTOKENS ? "token" : "nterm", yysymbol_name (yykind));

  yy_symbol_value_print (yyo, yykind, yyvaluep);
  YYFPRINTF (yyo, ")");
}

/*------------------------------------------------------------------.
| yy_stack_print -- Print the state stack from its BOTTOM up to its |
| TOP (included).                                                   |
`------------------------------------------------------------------*/

static void
yy_stack_print (yy_state_t *yybottom, yy_state_t *yytop)
{
  YYFPRINTF (stderr, "Stack now");
  for (; yybottom <= yytop; yybottom++)
    {
      int yybot = *yybottom;
      YYFPRINTF (stderr, " %d", yybot);
    }
  YYFPRINTF (stderr, "\n");
}

# define YY_STACK_PRINT(Bottom, Top)                            \
do {                                                            \
  if (yydebug)                                                  \
    yy_stack_print ((Bottom), (Top));                           \
} while (0)


/*------------------------------------------------.
| Report that the YYRULE is going to be reduced.  |
`------------------------------------------------*/

static void
yy_reduce_print (yy_state_t *yyssp, YYSTYPE *yyvsp,
                 int yyrule)
{
  int yylno = yyrline[yyrule];
  int yynrhs = yyr2[yyrule];
  int yyi;
  YYFPRINTF (stderr, "Reducing stack by rule %d (line %d):\n",
             yyrule - 1, yylno);
  /* The symbols being reduced.  */
  for (yyi = 0; yyi < yynrhs; yyi++)
    {
      YYFPRINTF (stderr, "   $%d = ", yyi + 1);
      yy_symbol_print (stderr,
                       YY_ACCESSING_SYMBOL (+yyssp[yyi + 1 - yynrhs]),
                       &yyvsp[(yyi + 1) - (yynrhs)]);
      YYFPRINTF (stderr, "\n");
    }
}

# define YY_REDUCE_PRINT(Rule)          \
do {                                    \
  if (yydebug)                          \
    yy_reduce_print (yyssp, yyvsp, Rule); \
} while (0)

/* Nonzero means print parse trace.  It is left uninitialized so that
   multiple parsers can coexist.  */
int yydebug;
#else /* !YYDEBUG */
# define YYDPRINTF(Args) ((void) 0)
# define YY_SYMBOL_PRINT(Title, Kind, Value, Location)
# define YY_STACK_PRINT(Bottom, Top)
# define YY_REDUCE_PRINT(Rule)
#endif /* !YYDEBUG */


/* YYINITDEPTH -- initial size of the parser's stacks.  */
#ifndef YYINITDEPTH
# define YYINITDEPTH 200
#endif

/* YYMAXDEPTH -- maximum size the stacks can grow to (effective only
   if the built-in stack extension method is used).

   Do not make this value too large; the results are undefined if
   YYSTACK_ALLOC_MAXIMUM < YYSTACK_BYTES (YYMAXDEPTH)
   evaluated with infinite-precision integer arithmetic.  */

#ifndef YYMAXDEPTH
# define YYMAXDEPTH 10000
#endif






/*-----------------------------------------------.
| Release the memory associated to this symbol.  |
`-----------------------------------------------*/

static void
yydestruct (const char *yymsg,
            yysymbol_kind_t yykind, YYSTYPE *yyvaluep)
{
  YY_USE (yyvaluep);
  if (!yymsg)
    yymsg = "Deleting";
  YY_SYMBOL_PRINT (yymsg, yykind, yyvaluep, yylocationp);

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  switch (yykind)
    {
    case YYSYMBOL_IDENTIFIER: /* IDENTIFIER  */
#line 26 "src/utils/expr_parser.y"
            { free(((*yyvaluep).str_val)); }
#line 998 "src/utils/expr_parser.cpp"
        break;

    case YYSYMBOL_TRACEE_N: /* TRACEE_N  */
#line 26 "src/utils/expr_parser.y"
            { free(((*yyvaluep).str_val)); }
#line 1004 "src/utils/expr_parser.cpp"
        break;

    case YYSYMBOL_assignment_expr: /* assignment_expr  */
#line 25 "src/utils/expr_parser.y"
            { delete ((*yyvaluep).node_val); }
#line 1010 "src/utils/expr_parser.cpp"
        break;

    case YYSYMBOL_conditional_expr: /* conditional_expr  */
#line 25 "src/utils/expr_parser.y"
            { delete ((*yyvaluep).node_val); }
#line 1016 "src/utils/expr_parser.cpp"
        break;

    case YYSYMBOL_logical_or_expr: /* logical_or_expr  */
#line 25 "src/utils/expr_parser.y"
            { delete ((*yyvaluep).node_val); }
#line 1022 "src/utils/expr_parser.cpp"
        break;

    case YYSYMBOL_logical_and_expr: /* logical_and_expr  */
#line 25 "src/utils/expr_parser.y"
            { delete ((*yyvaluep).node_val); }
#line 1028 "src/utils/expr_parser.cpp"
        break;

    case YYSYMBOL_or_expr: /* or_expr  */
#line 25 "src/utils/expr_parser.y"
            { delete ((*yyvaluep).node_val); }
#line 1034 "src/utils/expr_parser.cpp"
        break;

    case YYSYMBOL_xor_expr: /* xor_expr  */
#line 25 "src/utils/expr_parser.y"
            { delete ((*yyvaluep).node_val); }
#line 1040 "src/utils/expr_parser.cpp"
        break;

    case YYSYMBOL_and_expr: /* and_expr  */
#line 25 "src/utils/expr_parser.y"
            { delete ((*yyvaluep).node_val); }
#line 1046 "src/utils/expr_parser.cpp"
        break;

    case YYSYMBOL_equality_expr: /* equality_expr  */
#line 25 "src/utils/expr_parser.y"
            { delete ((*yyvaluep).node_val); }
#line 1052 "src/utils/expr_parser.cpp"
        break;

    case YYSYMBOL_relational_expr: /* relational_expr  */
#line 25 "src/utils/expr_parser.y"
            { delete ((*yyvaluep).node_val); }
#line 1058 "src/utils/expr_parser.cpp"
        break;

    case YYSYMBOL_shift_expr: /* shift_expr  */
#line 25 "src/utils/expr_parser.y"
            { delete ((*yyvaluep).node_val); }
#line 1064 "src/utils/expr_parser.cpp"
        break;

    case YYSYMBOL_additive_expr: /* additive_expr  */
#line 25 "src/utils/expr_parser.y"
            { delete ((*yyvaluep).node_val); }
#line 1070 "src/utils/expr_parser.cpp"
        break;

    case YYSYMBOL_multiplicative_expr: /* multiplicative_expr  */
#line 25 "src/utils/expr_parser.y"
            { delete ((*yyvaluep).node_val); }
#line 1076 "src/utils/expr_parser.cpp"
        break;

    case YYSYMBOL_cast_expr: /* cast_expr  */
#line 25 "src/utils/expr_parser.y"
            { delete ((*yyvaluep).node_val); }
#line 1082 "src/utils/expr_parser.cpp"
        break;

    case YYSYMBOL_type_name: /* type_name  */
#line 25 "src/utils/expr_parser.y"
            { delete ((*yyvaluep).node_val); }
#line 1088 "src/utils/expr_parser.cpp"
        break;

    case YYSYMBOL_type_specifier: /* type_specifier  */
#line 26 "src/utils/expr_parser.y"
            { free(((*yyvaluep).str_val)); }
#line 1094 "src/utils/expr_parser.cpp"
        break;

    case YYSYMBOL_unary_expr: /* unary_expr  */
#line 25 "src/utils/expr_parser.y"
            { delete ((*yyvaluep).node_val); }
#line 1100 "src/utils/expr_parser.cpp"
        break;

    case YYSYMBOL_postfix_expr: /* postfix_expr  */
#line 25 "src/utils/expr_parser.y"
            { delete ((*yyvaluep).node_val); }
#line 1106 "src/utils/expr_parser.cpp"
        break;

    case YYSYMBOL_primary_expr: /* primary_expr  */
#line 25 "src/utils/expr_parser.y"
            { delete ((*yyvaluep).node_val); }
#line 1112 "src/utils/expr_parser.cpp"
        break;

    case YYSYMBOL_expression: /* expression  */
#line 25 "src/utils/expr_parser.y"
            { delete ((*yyvaluep).node_val); }
#line 1118 "src/utils/expr_parser.cpp"
        break;

      default:
        break;
    }
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}


/* Lookahead token kind.  */
int yychar;

/* The semantic value of the lookahead symbol.  */
YYSTYPE yylval;
/* Number of syntax errors so far.  */
int yynerrs;




/*----------.
| yyparse.  |
`----------*/

int
yyparse (void)
{
    yy_state_fast_t yystate = 0;
    /* Number of tokens to shift before error messages enabled.  */
    int yyerrstatus = 0;

    /* Refer to the stacks through separate pointers, to allow yyoverflow
       to reallocate them elsewhere.  */

    /* Their size.  */
    YYPTRDIFF_T yystacksize = YYINITDEPTH;

    /* The state stack: array, bottom, top.  */
    yy_state_t yyssa[YYINITDEPTH];
    yy_state_t *yyss = yyssa;
    yy_state_t *yyssp = yyss;

    /* The semantic value stack: array, bottom, top.  */
    YYSTYPE yyvsa[YYINITDEPTH];
    YYSTYPE *yyvs = yyvsa;
    YYSTYPE *yyvsp = yyvs;

  int yyn;
  /* The return value of yyparse.  */
  int yyresult;
  /* Lookahead symbol kind.  */
  yysymbol_kind_t yytoken = YYSYMBOL_YYEMPTY;
  /* The variables used to return semantic value and location from the
     action routines.  */
  YYSTYPE yyval;



#define YYPOPSTACK(N)   (yyvsp -= (N), yyssp -= (N))

  /* The number of symbols on the RHS of the reduced rule.
     Keep to zero when no symbol should be popped.  */
  int yylen = 0;

  YYDPRINTF ((stderr, "Starting parse\n"));

  yychar = YYEMPTY; /* Cause a token to be read.  */

  goto yysetstate;


/*------------------------------------------------------------.
| yynewstate -- push a new state, which is found in yystate.  |
`------------------------------------------------------------*/
yynewstate:
  /* In all cases, when you get here, the value and location stacks
     have just been pushed.  So pushing a state here evens the stacks.  */
  yyssp++;


/*--------------------------------------------------------------------.
| yysetstate -- set current state (the top of the stack) to yystate.  |
`--------------------------------------------------------------------*/
yysetstate:
  YYDPRINTF ((stderr, "Entering state %d\n", yystate));
  YY_ASSERT (0 <= yystate && yystate < YYNSTATES);
  YY_IGNORE_USELESS_CAST_BEGIN
  *yyssp = YY_CAST (yy_state_t, yystate);
  YY_IGNORE_USELESS_CAST_END
  YY_STACK_PRINT (yyss, yyssp);

  if (yyss + yystacksize - 1 <= yyssp)
#if !defined yyoverflow && !defined YYSTACK_RELOCATE
    YYNOMEM;
#else
    {
      /* Get the current used size of the three stacks, in elements.  */
      YYPTRDIFF_T yysize = yyssp - yyss + 1;

# if defined yyoverflow
      {
        /* Give user a chance to reallocate the stack.  Use copies of
           these so that the &'s don't force the real ones into
           memory.  */
        yy_state_t *yyss1 = yyss;
        YYSTYPE *yyvs1 = yyvs;

        /* Each stack pointer address is followed by the size of the
           data in use in that stack, in bytes.  This used to be a
           conditional around just the two extra args, but that might
           be undefined if yyoverflow is a macro.  */
        yyoverflow (YY_("memory exhausted"),
                    &yyss1, yysize * YYSIZEOF (*yyssp),
                    &yyvs1, yysize * YYSIZEOF (*yyvsp),
                    &yystacksize);
        yyss = yyss1;
        yyvs = yyvs1;
      }
# else /* defined YYSTACK_RELOCATE */
      /* Extend the stack our own way.  */
      if (YYMAXDEPTH <= yystacksize)
        YYNOMEM;
      yystacksize *= 2;
      if (YYMAXDEPTH < yystacksize)
        yystacksize = YYMAXDEPTH;

      {
        yy_state_t *yyss1 = yyss;
        union yyalloc *yyptr =
          YY_CAST (union yyalloc *,
                   YYSTACK_ALLOC (YY_CAST (YYSIZE_T, YYSTACK_BYTES (yystacksize))));
        if (! yyptr)
          YYNOMEM;
        YYSTACK_RELOCATE (yyss_alloc, yyss);
        YYSTACK_RELOCATE (yyvs_alloc, yyvs);
#  undef YYSTACK_RELOCATE
        if (yyss1 != yyssa)
          YYSTACK_FREE (yyss1);
      }
# endif

      yyssp = yyss + yysize - 1;
      yyvsp = yyvs + yysize - 1;

      YY_IGNORE_USELESS_CAST_BEGIN
      YYDPRINTF ((stderr, "Stack size increased to %ld\n",
                  YY_CAST (long, yystacksize)));
      YY_IGNORE_USELESS_CAST_END

      if (yyss + yystacksize - 1 <= yyssp)
        YYABORT;
    }
#endif /* !defined yyoverflow && !defined YYSTACK_RELOCATE */


  if (yystate == YYFINAL)
    YYACCEPT;

  goto yybackup;


/*-----------.
| yybackup.  |
`-----------*/
yybackup:
  /* Do appropriate processing given the current state.  Read a
     lookahead token if we need one and don't already have one.  */

  /* First try to decide what to do without reference to lookahead token.  */
  yyn = yypact[yystate];
  if (yypact_value_is_default (yyn))
    goto yydefault;

  /* Not known => get a lookahead token if don't already have one.  */

  /* YYCHAR is either empty, or end-of-input, or a valid lookahead.  */
  if (yychar == YYEMPTY)
    {
      YYDPRINTF ((stderr, "Reading a token\n"));
      yychar = yylex ();
    }

  if (yychar <= YYEOF)
    {
      yychar = YYEOF;
      yytoken = YYSYMBOL_YYEOF;
      YYDPRINTF ((stderr, "Now at end of input.\n"));
    }
  else if (yychar == YYerror)
    {
      /* The scanner already issued an error message, process directly
         to error recovery.  But do not keep the error token as
         lookahead, it is too special and may lead us to an endless
         loop in error recovery. */
      yychar = YYUNDEF;
      yytoken = YYSYMBOL_YYerror;
      goto yyerrlab1;
    }
  else
    {
      yytoken = YYTRANSLATE (yychar);
      YY_SYMBOL_PRINT ("Next token is", yytoken, &yylval, &yylloc);
    }

  /* If the proper action on seeing token YYTOKEN is to reduce or to
     detect an error, take that action.  */
  yyn += yytoken;
  if (yyn < 0 || YYLAST < yyn || yycheck[yyn] != yytoken)
    goto yydefault;
  yyn = yytable[yyn];
  if (yyn <= 0)
    {
      if (yytable_value_is_error (yyn))
        goto yyerrlab;
      yyn = -yyn;
      goto yyreduce;
    }

  /* Count tokens shifted since error; after three, turn off error
     status.  */
  if (yyerrstatus)
    yyerrstatus--;

  /* Shift the lookahead token.  */
  YY_SYMBOL_PRINT ("Shifting", yytoken, &yylval, &yylloc);
  yystate = yyn;
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END

  /* Discard the shifted token.  */
  yychar = YYEMPTY;
  goto yynewstate;


/*-----------------------------------------------------------.
| yydefault -- do the default action for the current state.  |
`-----------------------------------------------------------*/
yydefault:
  yyn = yydefact[yystate];
  if (yyn == 0)
    goto yyerrlab;
  goto yyreduce;


/*-----------------------------.
| yyreduce -- do a reduction.  |
`-----------------------------*/
yyreduce:
  /* yyn is the number of a rule to reduce with.  */
  yylen = yyr2[yyn];

  /* If YYLEN is nonzero, implement the default value of the action:
     '$$ = $1'.

     Otherwise, the following line sets YYVAL to garbage.
     This behavior is undocumented and Bison
     users should not rely upon it.  Assigning to YYVAL
     unconditionally makes the parser a bit smaller, and it avoids a
     GCC warning that YYVAL may be used uninitialized.  */
  yyval = yyvsp[1-yylen];


  YY_REDUCE_PRINT (yyn);
  switch (yyn)
    {
  case 2: /* input: assignment_expr  */
#line 61 "src/utils/expr_parser.y"
                    { g_parser_result = (yyvsp[0].node_val); }
#line 1388 "src/utils/expr_parser.cpp"
    break;

  case 4: /* assignment_expr: unary_expr '=' assignment_expr  */
#line 66 "src/utils/expr_parser.y"
                                     { (yyval.node_val) = new AssignNode((yyvsp[-2].node_val), (yyvsp[0].node_val)); }
#line 1394 "src/utils/expr_parser.cpp"
    break;

  case 6: /* conditional_expr: logical_or_expr '?' expression ':' conditional_expr  */
#line 71 "src/utils/expr_parser.y"
                                                          { 
        (yyval.node_val) = new ConditionalNode((yyvsp[-4].node_val), (yyvsp[-2].node_val), (yyvsp[0].node_val)); 
    }
#line 1402 "src/utils/expr_parser.cpp"
    break;

  case 8: /* logical_or_expr: logical_or_expr OR logical_and_expr  */
#line 78 "src/utils/expr_parser.y"
                                          { (yyval.node_val) = new BinaryOpNode(BinaryOp::OR, (yyvsp[-2].node_val), (yyvsp[0].node_val)); }
#line 1408 "src/utils/expr_parser.cpp"
    break;

  case 10: /* logical_and_expr: logical_and_expr AND or_expr  */
#line 83 "src/utils/expr_parser.y"
                                   { (yyval.node_val) = new BinaryOpNode(BinaryOp::AND, (yyvsp[-2].node_val), (yyvsp[0].node_val)); }
#line 1414 "src/utils/expr_parser.cpp"
    break;

  case 12: /* or_expr: or_expr '|' xor_expr  */
#line 88 "src/utils/expr_parser.y"
                           { (yyval.node_val) = new BinaryOpNode(BinaryOp::BITOR, (yyvsp[-2].node_val), (yyvsp[0].node_val)); }
#line 1420 "src/utils/expr_parser.cpp"
    break;

  case 14: /* xor_expr: xor_expr '^' and_expr  */
#line 93 "src/utils/expr_parser.y"
                            { (yyval.node_val) = new BinaryOpNode(BinaryOp::XOR, (yyvsp[-2].node_val), (yyvsp[0].node_val)); }
#line 1426 "src/utils/expr_parser.cpp"
    break;

  case 16: /* and_expr: and_expr '&' equality_expr  */
#line 98 "src/utils/expr_parser.y"
                                 { (yyval.node_val) = new BinaryOpNode(BinaryOp::BITAND, (yyvsp[-2].node_val), (yyvsp[0].node_val)); }
#line 1432 "src/utils/expr_parser.cpp"
    break;

  case 18: /* equality_expr: equality_expr EQ relational_expr  */
#line 103 "src/utils/expr_parser.y"
                                       { (yyval.node_val) = new BinaryOpNode(BinaryOp::EQ, (yyvsp[-2].node_val), (yyvsp[0].node_val)); }
#line 1438 "src/utils/expr_parser.cpp"
    break;

  case 19: /* equality_expr: equality_expr NE relational_expr  */
#line 104 "src/utils/expr_parser.y"
                                       { (yyval.node_val) = new BinaryOpNode(BinaryOp::NE, (yyvsp[-2].node_val), (yyvsp[0].node_val)); }
#line 1444 "src/utils/expr_parser.cpp"
    break;

  case 21: /* relational_expr: relational_expr '<' shift_expr  */
#line 109 "src/utils/expr_parser.y"
                                     { (yyval.node_val) = new BinaryOpNode(BinaryOp::LT, (yyvsp[-2].node_val), (yyvsp[0].node_val)); }
#line 1450 "src/utils/expr_parser.cpp"
    break;

  case 22: /* relational_expr: relational_expr '>' shift_expr  */
#line 110 "src/utils/expr_parser.y"
                                     { (yyval.node_val) = new BinaryOpNode(BinaryOp::GT, (yyvsp[-2].node_val), (yyvsp[0].node_val)); }
#line 1456 "src/utils/expr_parser.cpp"
    break;

  case 23: /* relational_expr: relational_expr LE shift_expr  */
#line 111 "src/utils/expr_parser.y"
                                    { (yyval.node_val) = new BinaryOpNode(BinaryOp::LE, (yyvsp[-2].node_val), (yyvsp[0].node_val)); }
#line 1462 "src/utils/expr_parser.cpp"
    break;

  case 24: /* relational_expr: relational_expr GE shift_expr  */
#line 112 "src/utils/expr_parser.y"
                                    { (yyval.node_val) = new BinaryOpNode(BinaryOp::GE, (yyvsp[-2].node_val), (yyvsp[0].node_val)); }
#line 1468 "src/utils/expr_parser.cpp"
    break;

  case 26: /* shift_expr: shift_expr SHL additive_expr  */
#line 117 "src/utils/expr_parser.y"
                                   { (yyval.node_val) = new BinaryOpNode(BinaryOp::SHL, (yyvsp[-2].node_val), (yyvsp[0].node_val)); }
#line 1474 "src/utils/expr_parser.cpp"
    break;

  case 27: /* shift_expr: shift_expr SHR additive_expr  */
#line 118 "src/utils/expr_parser.y"
                                   { (yyval.node_val) = new BinaryOpNode(BinaryOp::SHR, (yyvsp[-2].node_val), (yyvsp[0].node_val)); }
#line 1480 "src/utils/expr_parser.cpp"
    break;

  case 29: /* additive_expr: additive_expr '+' multiplicative_expr  */
#line 123 "src/utils/expr_parser.y"
                                            { (yyval.node_val) = new BinaryOpNode(BinaryOp::ADD, (yyvsp[-2].node_val), (yyvsp[0].node_val)); }
#line 1486 "src/utils/expr_parser.cpp"
    break;

  case 30: /* additive_expr: additive_expr '-' multiplicative_expr  */
#line 124 "src/utils/expr_parser.y"
                                            { (yyval.node_val) = new BinaryOpNode(BinaryOp::SUB, (yyvsp[-2].node_val), (yyvsp[0].node_val)); }
#line 1492 "src/utils/expr_parser.cpp"
    break;

  case 32: /* multiplicative_expr: multiplicative_expr '*' cast_expr  */
#line 129 "src/utils/expr_parser.y"
                                        { (yyval.node_val) = new BinaryOpNode(BinaryOp::MUL, (yyvsp[-2].node_val), (yyvsp[0].node_val)); }
#line 1498 "src/utils/expr_parser.cpp"
    break;

  case 33: /* multiplicative_expr: multiplicative_expr '/' cast_expr  */
#line 130 "src/utils/expr_parser.y"
                                        { (yyval.node_val) = new BinaryOpNode(BinaryOp::DIV, (yyvsp[-2].node_val), (yyvsp[0].node_val)); }
#line 1504 "src/utils/expr_parser.cpp"
    break;

  case 34: /* multiplicative_expr: multiplicative_expr '%' cast_expr  */
#line 131 "src/utils/expr_parser.y"
                                        { (yyval.node_val) = new BinaryOpNode(BinaryOp::MOD, (yyvsp[-2].node_val), (yyvsp[0].node_val)); }
#line 1510 "src/utils/expr_parser.cpp"
    break;

  case 36: /* cast_expr: '(' type_name ')' cast_expr  */
#line 136 "src/utils/expr_parser.y"
                                  { (yyval.node_val) = new CastNode((yyvsp[-2].node_val), (yyvsp[0].node_val)); }
#line 1516 "src/utils/expr_parser.cpp"
    break;

  case 37: /* type_name: type_specifier  */
#line 140 "src/utils/expr_parser.y"
                   { (yyval.node_val) = new TypeNode((yyvsp[0].str_val)); free((yyvsp[0].str_val)); }
#line 1522 "src/utils/expr_parser.cpp"
    break;

  case 38: /* type_name: IDENTIFIER  */
#line 141 "src/utils/expr_parser.y"
                 { (yyval.node_val) = new TypeNode((yyvsp[0].str_val)); free((yyvsp[0].str_val)); }
#line 1528 "src/utils/expr_parser.cpp"
    break;

  case 39: /* type_name: KW_STRUCT IDENTIFIER  */
#line 142 "src/utils/expr_parser.y"
                           {
        std::string type_str = std::string("struct ") + (yyvsp[0].str_val);
        free((yyvsp[0].str_val));
        (yyval.node_val) = new TypeNode(type_str);
    }
#line 1538 "src/utils/expr_parser.cpp"
    break;

  case 40: /* type_name: type_name '*'  */
#line 147 "src/utils/expr_parser.y"
                    {
        std::string type_str = static_cast<TypeNode*>((yyvsp[-1].node_val))->get_type() + " *";
        delete (yyvsp[-1].node_val);
        (yyval.node_val) = new TypeNode(type_str);
    }
#line 1548 "src/utils/expr_parser.cpp"
    break;

  case 41: /* type_specifier: KW_INT  */
#line 155 "src/utils/expr_parser.y"
                { (yyval.str_val) = strdup("int"); }
#line 1554 "src/utils/expr_parser.cpp"
    break;

  case 42: /* type_specifier: KW_CHAR  */
#line 156 "src/utils/expr_parser.y"
                { (yyval.str_val) = strdup("char"); }
#line 1560 "src/utils/expr_parser.cpp"
    break;

  case 43: /* type_specifier: KW_SHORT  */
#line 157 "src/utils/expr_parser.y"
                { (yyval.str_val) = strdup("short"); }
#line 1566 "src/utils/expr_parser.cpp"
    break;

  case 44: /* type_specifier: KW_LONG  */
#line 158 "src/utils/expr_parser.y"
                { (yyval.str_val) = strdup("long"); }
#line 1572 "src/utils/expr_parser.cpp"
    break;

  case 45: /* type_specifier: KW_FLOAT  */
#line 159 "src/utils/expr_parser.y"
                { (yyval.str_val) = strdup("float"); }
#line 1578 "src/utils/expr_parser.cpp"
    break;

  case 46: /* type_specifier: KW_DOUBLE  */
#line 160 "src/utils/expr_parser.y"
                { (yyval.str_val) = strdup("double"); }
#line 1584 "src/utils/expr_parser.cpp"
    break;

  case 47: /* type_specifier: KW_VOID  */
#line 161 "src/utils/expr_parser.y"
                { (yyval.str_val) = strdup("void"); }
#line 1590 "src/utils/expr_parser.cpp"
    break;

  case 48: /* type_specifier: KW_UNSIGNED  */
#line 162 "src/utils/expr_parser.y"
                  { (yyval.str_val) = strdup("unsigned"); }
#line 1596 "src/utils/expr_parser.cpp"
    break;

  case 49: /* type_specifier: KW_SIGNED  */
#line 163 "src/utils/expr_parser.y"
                  { (yyval.str_val) = strdup("signed"); }
#line 1602 "src/utils/expr_parser.cpp"
    break;

  case 51: /* unary_expr: INC unary_expr  */
#line 168 "src/utils/expr_parser.y"
                     { (yyval.node_val) = new PrefixOpNode(PrefixOp::PRE_INC, (yyvsp[0].node_val)); }
#line 1608 "src/utils/expr_parser.cpp"
    break;

  case 52: /* unary_expr: DEC unary_expr  */
#line 169 "src/utils/expr_parser.y"
                     { (yyval.node_val) = new PrefixOpNode(PrefixOp::PRE_DEC, (yyvsp[0].node_val)); }
#line 1614 "src/utils/expr_parser.cpp"
    break;

  case 53: /* unary_expr: '&' unary_expr  */
#line 170 "src/utils/expr_parser.y"
                     { (yyval.node_val) = new AddressOfNode((yyvsp[0].node_val)); }
#line 1620 "src/utils/expr_parser.cpp"
    break;

  case 54: /* unary_expr: '*' unary_expr  */
#line 171 "src/utils/expr_parser.y"
                     { (yyval.node_val) = new DereferenceNode((yyvsp[0].node_val)); }
#line 1626 "src/utils/expr_parser.cpp"
    break;

  case 55: /* unary_expr: '+' unary_expr  */
#line 172 "src/utils/expr_parser.y"
                     { (yyval.node_val) = (yyvsp[0].node_val); }
#line 1632 "src/utils/expr_parser.cpp"
    break;

  case 56: /* unary_expr: '-' unary_expr  */
#line 173 "src/utils/expr_parser.y"
                     { (yyval.node_val) = new UnaryOpNode(UnaryOp::NEG, (yyvsp[0].node_val)); }
#line 1638 "src/utils/expr_parser.cpp"
    break;

  case 57: /* unary_expr: '!' unary_expr  */
#line 174 "src/utils/expr_parser.y"
                     { (yyval.node_val) = new UnaryOpNode(UnaryOp::NOT, (yyvsp[0].node_val)); }
#line 1644 "src/utils/expr_parser.cpp"
    break;

  case 58: /* unary_expr: '~' unary_expr  */
#line 175 "src/utils/expr_parser.y"
                     { (yyval.node_val) = new UnaryOpNode(UnaryOp::BITNOT, (yyvsp[0].node_val)); }
#line 1650 "src/utils/expr_parser.cpp"
    break;

  case 59: /* unary_expr: KW_SIZEOF unary_expr  */
#line 176 "src/utils/expr_parser.y"
                           { (yyval.node_val) = new SizeofExprNode((yyvsp[0].node_val)); }
#line 1656 "src/utils/expr_parser.cpp"
    break;

  case 60: /* unary_expr: KW_SIZEOF '(' type_name ')'  */
#line 177 "src/utils/expr_parser.y"
                                  { 
        (yyval.node_val) = new SizeofTypeNode(static_cast<TypeNode*>((yyvsp[-1].node_val))->get_type()); 
        delete (yyvsp[-1].node_val);
    }
#line 1665 "src/utils/expr_parser.cpp"
    break;

  case 61: /* unary_expr: KW_OFFSETOF '(' type_name ',' IDENTIFIER ')'  */
#line 181 "src/utils/expr_parser.y"
                                                   {
        (yyval.node_val) = new OffsetofNode(static_cast<TypeNode*>((yyvsp[-3].node_val))->get_type(), (yyvsp[-1].str_val));
        delete (yyvsp[-3].node_val);
        free((yyvsp[-1].str_val));
    }
#line 1675 "src/utils/expr_parser.cpp"
    break;

  case 62: /* unary_expr: KW_TYPEOF '(' expression ')'  */
#line 186 "src/utils/expr_parser.y"
                                   {
        (yyval.node_val) = new TypeofNode((yyvsp[-1].node_val));
    }
#line 1683 "src/utils/expr_parser.cpp"
    break;

  case 64: /* postfix_expr: postfix_expr '[' expression ']'  */
#line 193 "src/utils/expr_parser.y"
                                      { (yyval.node_val) = new ArrayIndexNode((yyvsp[-3].node_val), (yyvsp[-1].node_val)); }
#line 1689 "src/utils/expr_parser.cpp"
    break;

  case 65: /* postfix_expr: postfix_expr '.' IDENTIFIER  */
#line 194 "src/utils/expr_parser.y"
                                  { (yyval.node_val) = new MemberAccessNode(MemberOp::DOT, (yyvsp[-2].node_val), (yyvsp[0].str_val)); free((yyvsp[0].str_val)); }
#line 1695 "src/utils/expr_parser.cpp"
    break;

  case 66: /* postfix_expr: postfix_expr ARROW IDENTIFIER  */
#line 195 "src/utils/expr_parser.y"
                                    { (yyval.node_val) = new MemberAccessNode(MemberOp::ARROW, (yyvsp[-2].node_val), (yyvsp[0].str_val)); free((yyvsp[0].str_val)); }
#line 1701 "src/utils/expr_parser.cpp"
    break;

  case 67: /* postfix_expr: postfix_expr INC  */
#line 196 "src/utils/expr_parser.y"
                       { (yyval.node_val) = new PostfixOpNode(PostfixOp::POST_INC, (yyvsp[-1].node_val)); }
#line 1707 "src/utils/expr_parser.cpp"
    break;

  case 68: /* postfix_expr: postfix_expr DEC  */
#line 197 "src/utils/expr_parser.y"
                       { (yyval.node_val) = new PostfixOpNode(PostfixOp::POST_DEC, (yyvsp[-1].node_val)); }
#line 1713 "src/utils/expr_parser.cpp"
    break;

  case 69: /* postfix_expr: TRACEE_N '(' expression ')'  */
#line 198 "src/utils/expr_parser.y"
                                  {
        int proc_id = atoi((yyvsp[-3].str_val) + 6);
        free((yyvsp[-3].str_val));
        (yyval.node_val) = new ProcessQualifiedNode(proc_id, (yyvsp[-1].node_val));
    }
#line 1723 "src/utils/expr_parser.cpp"
    break;

  case 70: /* primary_expr: NUMBER  */
#line 206 "src/utils/expr_parser.y"
           { (yyval.node_val) = new NumberNode((yyvsp[0].int_val)); }
#line 1729 "src/utils/expr_parser.cpp"
    break;

  case 71: /* primary_expr: CHAR_LITERAL  */
#line 207 "src/utils/expr_parser.y"
                   { (yyval.node_val) = new NumberNode((yyvsp[0].int_val)); }
#line 1735 "src/utils/expr_parser.cpp"
    break;

  case 72: /* primary_expr: IDENTIFIER  */
#line 208 "src/utils/expr_parser.y"
                 { (yyval.node_val) = new VariableNode((yyvsp[0].str_val)); free((yyvsp[0].str_val)); }
#line 1741 "src/utils/expr_parser.cpp"
    break;

  case 73: /* primary_expr: '(' expression ')'  */
#line 209 "src/utils/expr_parser.y"
                         { (yyval.node_val) = (yyvsp[-1].node_val); }
#line 1747 "src/utils/expr_parser.cpp"
    break;

  case 74: /* expression: assignment_expr  */
#line 213 "src/utils/expr_parser.y"
                    { (yyval.node_val) = (yyvsp[0].node_val); }
#line 1753 "src/utils/expr_parser.cpp"
    break;


#line 1757 "src/utils/expr_parser.cpp"

      default: break;
    }
  /* User semantic actions sometimes alter yychar, and that requires
     that yytoken be updated with the new translation.  We take the
     approach of translating immediately before every use of yytoken.
     One alternative is translating here after every semantic action,
     but that translation would be missed if the semantic action invokes
     YYABORT, YYACCEPT, or YYERROR immediately after altering yychar or
     if it invokes YYBACKUP.  In the case of YYABORT or YYACCEPT, an
     incorrect destructor might then be invoked immediately.  In the
     case of YYERROR or YYBACKUP, subsequent parser actions might lead
     to an incorrect destructor call or verbose syntax error message
     before the lookahead is translated.  */
  YY_SYMBOL_PRINT ("-> $$ =", YY_CAST (yysymbol_kind_t, yyr1[yyn]), &yyval, &yyloc);

  YYPOPSTACK (yylen);
  yylen = 0;

  *++yyvsp = yyval;

  /* Now 'shift' the result of the reduction.  Determine what state
     that goes to, based on the state we popped back to and the rule
     number reduced by.  */
  {
    const int yylhs = yyr1[yyn] - YYNTOKENS;
    const int yyi = yypgoto[yylhs] + *yyssp;
    yystate = (0 <= yyi && yyi <= YYLAST && yycheck[yyi] == *yyssp
               ? yytable[yyi]
               : yydefgoto[yylhs]);
  }

  goto yynewstate;


/*--------------------------------------.
| yyerrlab -- here on detecting error.  |
`--------------------------------------*/
yyerrlab:
  /* Make sure we have latest lookahead translation.  See comments at
     user semantic actions for why this is necessary.  */
  yytoken = yychar == YYEMPTY ? YYSYMBOL_YYEMPTY : YYTRANSLATE (yychar);
  /* If not already recovering from an error, report this error.  */
  if (!yyerrstatus)
    {
      ++yynerrs;
      yyerror (YY_("syntax error"));
    }

  if (yyerrstatus == 3)
    {
      /* If just tried and failed to reuse lookahead token after an
         error, discard it.  */

      if (yychar <= YYEOF)
        {
          /* Return failure if at end of input.  */
          if (yychar == YYEOF)
            YYABORT;
        }
      else
        {
          yydestruct ("Error: discarding",
                      yytoken, &yylval);
          yychar = YYEMPTY;
        }
    }

  /* Else will try to reuse lookahead token after shifting the error
     token.  */
  goto yyerrlab1;


/*---------------------------------------------------.
| yyerrorlab -- error raised explicitly by YYERROR.  |
`---------------------------------------------------*/
yyerrorlab:
  /* Pacify compilers when the user code never invokes YYERROR and the
     label yyerrorlab therefore never appears in user code.  */
  if (0)
    YYERROR;
  ++yynerrs;

  /* Do not reclaim the symbols of the rule whose action triggered
     this YYERROR.  */
  YYPOPSTACK (yylen);
  yylen = 0;
  YY_STACK_PRINT (yyss, yyssp);
  yystate = *yyssp;
  goto yyerrlab1;


/*-------------------------------------------------------------.
| yyerrlab1 -- common code for both syntax error and YYERROR.  |
`-------------------------------------------------------------*/
yyerrlab1:
  yyerrstatus = 3;      /* Each real token shifted decrements this.  */

  /* Pop stack until we find a state that shifts the error token.  */
  for (;;)
    {
      yyn = yypact[yystate];
      if (!yypact_value_is_default (yyn))
        {
          yyn += YYSYMBOL_YYerror;
          if (0 <= yyn && yyn <= YYLAST && yycheck[yyn] == YYSYMBOL_YYerror)
            {
              yyn = yytable[yyn];
              if (0 < yyn)
                break;
            }
        }

      /* Pop the current state because it cannot handle the error token.  */
      if (yyssp == yyss)
        YYABORT;


      yydestruct ("Error: popping",
                  YY_ACCESSING_SYMBOL (yystate), yyvsp);
      YYPOPSTACK (1);
      yystate = *yyssp;
      YY_STACK_PRINT (yyss, yyssp);
    }

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END


  /* Shift the error token.  */
  YY_SYMBOL_PRINT ("Shifting", YY_ACCESSING_SYMBOL (yyn), yyvsp, yylsp);

  yystate = yyn;
  goto yynewstate;


/*-------------------------------------.
| yyacceptlab -- YYACCEPT comes here.  |
`-------------------------------------*/
yyacceptlab:
  yyresult = 0;
  goto yyreturnlab;


/*-----------------------------------.
| yyabortlab -- YYABORT comes here.  |
`-----------------------------------*/
yyabortlab:
  yyresult = 1;
  goto yyreturnlab;


/*-----------------------------------------------------------.
| yyexhaustedlab -- YYNOMEM (memory exhaustion) comes here.  |
`-----------------------------------------------------------*/
yyexhaustedlab:
  yyerror (YY_("memory exhausted"));
  yyresult = 2;
  goto yyreturnlab;


/*----------------------------------------------------------.
| yyreturnlab -- parsing is finished, clean up and return.  |
`----------------------------------------------------------*/
yyreturnlab:
  if (yychar != YYEMPTY)
    {
      /* Make sure we have latest lookahead translation.  See comments at
         user semantic actions for why this is necessary.  */
      yytoken = YYTRANSLATE (yychar);
      yydestruct ("Cleanup: discarding lookahead",
                  yytoken, &yylval);
    }
  /* Do not reclaim the symbols of the rule whose action triggered
     this YYABORT or YYACCEPT.  */
  YYPOPSTACK (yylen);
  YY_STACK_PRINT (yyss, yyssp);
  while (yyssp != yyss)
    {
      yydestruct ("Cleanup: popping",
                  YY_ACCESSING_SYMBOL (+*yyssp), yyvsp);
      YYPOPSTACK (1);
    }
#ifndef yyoverflow
  if (yyss != yyssa)
    YYSTACK_FREE (yyss);
#endif

  return yyresult;
}

#line 216 "src/utils/expr_parser.y"


void yyerror(const char *s) {
    fprintf(stderr, "Parse error: %s at line %d\n", s, yylineno);
}

int yywrap() {
    return 1;
}

/* Include AST definitions after union declaration */
#include "expr_ast.hpp"
