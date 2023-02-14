/* A Bison parser, made by GNU Bison 3.8.2.  */

/* Bison interface for Yacc-like parsers in C */

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

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

#ifndef YY_YY_ARPARSE_H_INCLUDED
# define YY_YY_ARPARSE_H_INCLUDED
/* Debug traces.  */
#ifndef YYDEBUG
# define YYDEBUG 0
#endif
#if YYDEBUG
extern int yydebug;
#endif

/* Token kinds.  */
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
  enum yytokentype
  {
    YYEMPTY = -2,
    YYEOF = 0,                     /* "end of file"  */
    YYerror = 256,                 /* error  */
    YYUNDEF = 257,                 /* "invalid token"  */
    NEWLINE = 258,                 /* NEWLINE  */
    VERBOSE = 259,                 /* VERBOSE  */
    FILENAME = 260,                /* FILENAME  */
    ADDLIB = 261,                  /* ADDLIB  */
    LIST = 262,                    /* LIST  */
    ADDMOD = 263,                  /* ADDMOD  */
    CLEAR = 264,                   /* CLEAR  */
    CREATE = 265,                  /* CREATE  */
    DELETE = 266,                  /* DELETE  */
    DIRECTORY = 267,               /* DIRECTORY  */
    END = 268,                     /* END  */
    EXTRACT = 269,                 /* EXTRACT  */
    FULLDIR = 270,                 /* FULLDIR  */
    HELP = 271,                    /* HELP  */
    QUIT = 272,                    /* QUIT  */
    REPLACE = 273,                 /* REPLACE  */
    SAVE = 274,                    /* SAVE  */
    OPEN = 275                     /* OPEN  */
  };
  typedef enum yytokentype yytoken_kind_t;
#endif
/* Token kinds.  */
#define YYEMPTY -2
#define YYEOF 0
#define YYerror 256
#define YYUNDEF 257
#define NEWLINE 258
#define VERBOSE 259
#define FILENAME 260
#define ADDLIB 261
#define LIST 262
#define ADDMOD 263
#define CLEAR 264
#define CREATE 265
#define DELETE 266
#define DIRECTORY 267
#define END 268
#define EXTRACT 269
#define FULLDIR 270
#define HELP 271
#define QUIT 272
#define REPLACE 273
#define SAVE 274
#define OPEN 275

/* Value type.  */
#if ! defined YYSTYPE && ! defined YYSTYPE_IS_DECLARED
union YYSTYPE
{
#line 37 "arparse.y"

  char *name;
struct list *list ;


#line 113 "arparse.h"

};
typedef union YYSTYPE YYSTYPE;
# define YYSTYPE_IS_TRIVIAL 1
# define YYSTYPE_IS_DECLARED 1
#endif


extern YYSTYPE yylval;


int yyparse (void);


#endif /* !YY_YY_ARPARSE_H_INCLUDED  */