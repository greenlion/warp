// A Bison parser, made by GNU Bison 3.8.2.

// Skeleton implementation for Bison LALR(1) parsers in C++

// Copyright (C) 2002-2015, 2018-2021 Free Software Foundation, Inc.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

// As a special exception, you may create a larger work that contains
// part or all of the Bison parser skeleton and distribute that work
// under terms of your choice, so long as that work isn't itself a
// parser generator using the skeleton or a modified version thereof
// as a parser skeleton.  Alternatively, if you modify or redistribute
// the parser skeleton itself, you may (at your option) remove this
// special exception, which will cause the skeleton and the resulting
// Bison output files to be licensed under the GNU General Public
// License without this special exception.

// This special exception was added by the Free Software Foundation in
// version 2.2 of Bison.

// DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
// especially those whose name start with YY_ or yy_.  They are
// private implementation details that can be changed or removed.

// "%code top" blocks.
#line 6 "whereParser.yy"

/** \file Defines the parser for the where clause accepted by FastBit IBIS.
    The definitions are processed through bison.
*/

#include <iostream>

#line 47 "whereParser.cc"




#include "whereParser.hh"

// Second part of user prologue.
#line 108 "whereParser.yy"

#include "whereLexer.h"

#undef yylex
#define yylex driver.lexer->lex

/* Quotes a value of an IN list for ibis::qAnyString, which takes a back
   slash to escape the next character.  The value is as it was written in
   the quotes of the literal. */
static std::string listValue(const std::string &lit) {
    std::string res = "'";
    for (size_t i = 0; i < lit.size(); ++ i) {
        if (lit[i] == '\\' && i + 1 < lit.size())
            ++ i;
        if (lit[i] == '\'' || lit[i] == '\\')
            res += '\\';
        res += lit[i];
    }
    res += '\'';
    return res;
}

/* The number of a collation, collate_N, that the strings are compared in.
   The program has to give FastBit the functions that compare the strings
   of the collations (ibis::util::setCollationFunctions). */
static uint32_t collationNumber(int coll) {
    if (coll <= 0 || ! ibis::util::haveCollations()) {
        LOGGER(ibis::gVerbose >= 0)
            << "whereParser.yy: can not compare strings in the collation "
            << coll;
        throw "Unknown collation";
    }
    return static_cast<uint32_t>(coll);
}

/* Builds the comparison of a column with string literals, "name op 'lo'" or
   "name between 'lo' and 'hi'", in the collation coll (0 is the default
   one).  The expression on the left has to be the name of a column.  It and
   the strings are deleted. */
static ibis::qExpr* stringCompare(ibis::qExpr *left,
                                  ibis::qString::COMPARE op,
                                  std::string *lo, std::string *hi,
                                  bool dec=false, uint32_t coll=0) {
    ibis::math::variable *var = dynamic_cast<ibis::math::variable*>(left);
    if (var == 0) {
        LOGGER(ibis::gVerbose >= 0)
            << "whereParser.yy: a string can only be compared with a column "
            "name, not with " << *left;
        delete hi;
        delete lo;
        delete left;
        throw "A string can only be compared with a column name";
    }
    if (dec && (! ibis::util::isDecimalString(lo->c_str()) ||
                (hi != 0 && ! ibis::util::isDecimalString(hi->c_str())))) {
        LOGGER(ibis::gVerbose >= 0)
            << "whereParser.yy: decimal_cmp expects decimal numbers in "
            "quotes, not " << *lo << (hi != 0 ? " and " : "")
            << (hi != 0 ? *hi : std::string());
        delete hi;
        delete lo;
        delete var;
        throw "decimal_cmp expects decimal numbers";
    }
    ibis::qExpr *ret = (hi != 0 ?
        new ibis::qString(var->variableName(), lo->c_str(), hi->c_str(), dec,
                          coll) :
        new ibis::qString(var->variableName(), op, lo->c_str(), dec, coll));
    delete hi;
    delete lo;
    delete var;
    return ret;
}

#line 130 "whereParser.cc"



#ifndef YY_
# if defined YYENABLE_NLS && YYENABLE_NLS
#  if ENABLE_NLS
#   include <libintl.h> // FIXME: INFRINGES ON USER NAME SPACE.
#   define YY_(msgid) dgettext ("bison-runtime", msgid)
#  endif
# endif
# ifndef YY_
#  define YY_(msgid) msgid
# endif
#endif


// Whether we are compiled with exception support.
#ifndef YY_EXCEPTIONS
# if defined __GNUC__ && !defined __EXCEPTIONS
#  define YY_EXCEPTIONS 0
# else
#  define YY_EXCEPTIONS 1
# endif
#endif

#define YYRHSLOC(Rhs, K) ((Rhs)[K].location)
/* YYLLOC_DEFAULT -- Set CURRENT to span from RHS[1] to RHS[N].
   If N is 0, then set CURRENT to the empty location which ends
   the previous symbol: RHS[0] (always defined).  */

# ifndef YYLLOC_DEFAULT
#  define YYLLOC_DEFAULT(Current, Rhs, N)                               \
    do                                                                  \
      if (N)                                                            \
        {                                                               \
          (Current).begin  = YYRHSLOC (Rhs, 1).begin;                   \
          (Current).end    = YYRHSLOC (Rhs, N).end;                     \
        }                                                               \
      else                                                              \
        {                                                               \
          (Current).begin = (Current).end = YYRHSLOC (Rhs, 0).end;      \
        }                                                               \
    while (false)
# endif


// Enable debugging if requested.
#if YYDEBUG

// A pseudo ostream that takes yydebug_ into account.
# define YYCDEBUG if (yydebug_) (*yycdebug_)

# define YY_SYMBOL_PRINT(Title, Symbol)         \
  do {                                          \
    if (yydebug_)                               \
    {                                           \
      *yycdebug_ << Title << ' ';               \
      yy_print_ (*yycdebug_, Symbol);           \
      *yycdebug_ << '\n';                       \
    }                                           \
  } while (false)

# define YY_REDUCE_PRINT(Rule)          \
  do {                                  \
    if (yydebug_)                       \
      yy_reduce_print_ (Rule);          \
  } while (false)

# define YY_STACK_PRINT()               \
  do {                                  \
    if (yydebug_)                       \
      yy_stack_print_ ();                \
  } while (false)

#else // !YYDEBUG

# define YYCDEBUG if (false) std::cerr
# define YY_SYMBOL_PRINT(Title, Symbol)  YY_USE (Symbol)
# define YY_REDUCE_PRINT(Rule)           static_cast<void> (0)
# define YY_STACK_PRINT()                static_cast<void> (0)

#endif // !YYDEBUG

#define yyerrok         (yyerrstatus_ = 0)
#define yyclearin       (yyla.clear ())

#define YYACCEPT        goto yyacceptlab
#define YYABORT         goto yyabortlab
#define YYERROR         goto yyerrorlab
#define YYRECOVERING()  (!!yyerrstatus_)

#line 25 "whereParser.yy"
namespace ibis {
#line 224 "whereParser.cc"

  /// Build a parser object.
  whereParser::whereParser (class ibis::whereClause& driver_yyarg)
#if YYDEBUG
    : yydebug_ (false),
      yycdebug_ (&std::cerr),
#else
    :
#endif
      driver (driver_yyarg)
  {}

  whereParser::~whereParser ()
  {}

  whereParser::syntax_error::~syntax_error () YY_NOEXCEPT YY_NOTHROW
  {}

  /*---------.
  | symbol.  |
  `---------*/

  // basic_symbol.
  template <typename Base>
  whereParser::basic_symbol<Base>::basic_symbol (const basic_symbol& that)
    : Base (that)
    , value (that.value)
    , location (that.location)
  {}


  /// Constructor for valueless symbols.
  template <typename Base>
  whereParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, YY_MOVE_REF (location_type) l)
    : Base (t)
    , value ()
    , location (l)
  {}

  template <typename Base>
  whereParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, YY_RVREF (value_type) v, YY_RVREF (location_type) l)
    : Base (t)
    , value (YY_MOVE (v))
    , location (YY_MOVE (l))
  {}


  template <typename Base>
  whereParser::symbol_kind_type
  whereParser::basic_symbol<Base>::type_get () const YY_NOEXCEPT
  {
    return this->kind ();
  }


  template <typename Base>
  bool
  whereParser::basic_symbol<Base>::empty () const YY_NOEXCEPT
  {
    return this->kind () == symbol_kind::S_YYEMPTY;
  }

  template <typename Base>
  void
  whereParser::basic_symbol<Base>::move (basic_symbol& s)
  {
    super_type::move (s);
    value = YY_MOVE (s.value);
    location = YY_MOVE (s.location);
  }

  // by_kind.
  whereParser::by_kind::by_kind () YY_NOEXCEPT
    : kind_ (symbol_kind::S_YYEMPTY)
  {}

#if 201103L <= YY_CPLUSPLUS
  whereParser::by_kind::by_kind (by_kind&& that) YY_NOEXCEPT
    : kind_ (that.kind_)
  {
    that.clear ();
  }
#endif

  whereParser::by_kind::by_kind (const by_kind& that) YY_NOEXCEPT
    : kind_ (that.kind_)
  {}

  whereParser::by_kind::by_kind (token_kind_type t) YY_NOEXCEPT
    : kind_ (yytranslate_ (t))
  {}



  void
  whereParser::by_kind::clear () YY_NOEXCEPT
  {
    kind_ = symbol_kind::S_YYEMPTY;
  }

  void
  whereParser::by_kind::move (by_kind& that)
  {
    kind_ = that.kind_;
    that.clear ();
  }

  whereParser::symbol_kind_type
  whereParser::by_kind::kind () const YY_NOEXCEPT
  {
    return kind_;
  }


  whereParser::symbol_kind_type
  whereParser::by_kind::type_get () const YY_NOEXCEPT
  {
    return this->kind ();
  }



  // by_state.
  whereParser::by_state::by_state () YY_NOEXCEPT
    : state (empty_state)
  {}

  whereParser::by_state::by_state (const by_state& that) YY_NOEXCEPT
    : state (that.state)
  {}

  void
  whereParser::by_state::clear () YY_NOEXCEPT
  {
    state = empty_state;
  }

  void
  whereParser::by_state::move (by_state& that)
  {
    state = that.state;
    that.clear ();
  }

  whereParser::by_state::by_state (state_type s) YY_NOEXCEPT
    : state (s)
  {}

  whereParser::symbol_kind_type
  whereParser::by_state::kind () const YY_NOEXCEPT
  {
    if (state == empty_state)
      return symbol_kind::S_YYEMPTY;
    else
      return YY_CAST (symbol_kind_type, yystos_[+state]);
  }

  whereParser::stack_symbol_type::stack_symbol_type ()
  {}

  whereParser::stack_symbol_type::stack_symbol_type (YY_RVREF (stack_symbol_type) that)
    : super_type (YY_MOVE (that.state), YY_MOVE (that.value), YY_MOVE (that.location))
  {
#if 201103L <= YY_CPLUSPLUS
    // that is emptied.
    that.state = empty_state;
#endif
  }

  whereParser::stack_symbol_type::stack_symbol_type (state_type s, YY_MOVE_REF (symbol_type) that)
    : super_type (s, YY_MOVE (that.value), YY_MOVE (that.location))
  {
    // that is emptied.
    that.kind_ = symbol_kind::S_YYEMPTY;
  }

#if YY_CPLUSPLUS < 201103L
  whereParser::stack_symbol_type&
  whereParser::stack_symbol_type::operator= (const stack_symbol_type& that)
  {
    state = that.state;
    value = that.value;
    location = that.location;
    return *this;
  }

  whereParser::stack_symbol_type&
  whereParser::stack_symbol_type::operator= (stack_symbol_type& that)
  {
    state = that.state;
    value = that.value;
    location = that.location;
    // that is emptied.
    that.state = empty_state;
    return *this;
  }
#endif

  template <typename Base>
  void
  whereParser::yy_destroy_ (const char* yymsg, basic_symbol<Base>& yysym) const
  {
    if (yymsg)
      YY_SYMBOL_PRINT (yymsg, yysym);

    // User destructor.
    switch (yysym.kind ())
    {
      case symbol_kind::S_INTSEQ: // "signed integer sequence"
#line 105 "whereParser.yy"
                    { delete (yysym.value.stringVal); }
#line 436 "whereParser.cc"
        break;

      case symbol_kind::S_UINTSEQ: // "unsigned integer sequence"
#line 105 "whereParser.yy"
                    { delete (yysym.value.stringVal); }
#line 442 "whereParser.cc"
        break;

      case symbol_kind::S_NOUNSTR: // "name string"
#line 105 "whereParser.yy"
                    { delete (yysym.value.stringVal); }
#line 448 "whereParser.cc"
        break;

      case symbol_kind::S_NUMSEQ: // "number sequence"
#line 105 "whereParser.yy"
                    { delete (yysym.value.stringVal); }
#line 454 "whereParser.cc"
        break;

      case symbol_kind::S_STRSEQ: // "string sequence"
#line 105 "whereParser.yy"
                    { delete (yysym.value.stringVal); }
#line 460 "whereParser.cc"
        break;

      case symbol_kind::S_STRLIT: // "string literal"
#line 105 "whereParser.yy"
                    { delete (yysym.value.stringVal); }
#line 466 "whereParser.cc"
        break;

      case symbol_kind::S_qexpr: // qexpr
#line 106 "whereParser.yy"
                    { delete (yysym.value.whereNode); }
#line 472 "whereParser.cc"
        break;

      case symbol_kind::S_simpleRange: // simpleRange
#line 106 "whereParser.yy"
                    { delete (yysym.value.whereNode); }
#line 478 "whereParser.cc"
        break;

      case symbol_kind::S_compRange2: // compRange2
#line 106 "whereParser.yy"
                    { delete (yysym.value.whereNode); }
#line 484 "whereParser.cc"
        break;

      case symbol_kind::S_compRange3: // compRange3
#line 106 "whereParser.yy"
                    { delete (yysym.value.whereNode); }
#line 490 "whereParser.cc"
        break;

      case symbol_kind::S_mathExpr: // mathExpr
#line 106 "whereParser.yy"
                    { delete (yysym.value.whereNode); }
#line 496 "whereParser.cc"
        break;

      default:
        break;
    }
  }

#if YYDEBUG
  template <typename Base>
  void
  whereParser::yy_print_ (std::ostream& yyo, const basic_symbol<Base>& yysym) const
  {
    std::ostream& yyoutput = yyo;
    YY_USE (yyoutput);
    if (yysym.empty ())
      yyo << "empty symbol";
    else
      {
        symbol_kind_type yykind = yysym.kind ();
        yyo << (yykind < YYNTOKENS ? "token" : "nterm")
            << ' ' << yysym.name () << " ("
            << yysym.location << ": ";
        YY_USE (yykind);
        yyo << ')';
      }
  }
#endif

  void
  whereParser::yypush_ (const char* m, YY_MOVE_REF (stack_symbol_type) sym)
  {
    if (m)
      YY_SYMBOL_PRINT (m, sym);
    yystack_.push (YY_MOVE (sym));
  }

  void
  whereParser::yypush_ (const char* m, state_type s, YY_MOVE_REF (symbol_type) sym)
  {
#if 201103L <= YY_CPLUSPLUS
    yypush_ (m, stack_symbol_type (s, std::move (sym)));
#else
    stack_symbol_type ss (s, sym);
    yypush_ (m, ss);
#endif
  }

  void
  whereParser::yypop_ (int n) YY_NOEXCEPT
  {
    yystack_.pop (n);
  }

#if YYDEBUG
  std::ostream&
  whereParser::debug_stream () const
  {
    return *yycdebug_;
  }

  void
  whereParser::set_debug_stream (std::ostream& o)
  {
    yycdebug_ = &o;
  }


  whereParser::debug_level_type
  whereParser::debug_level () const
  {
    return yydebug_;
  }

  void
  whereParser::set_debug_level (debug_level_type l)
  {
    yydebug_ = l;
  }
#endif // YYDEBUG

  whereParser::state_type
  whereParser::yy_lr_goto_state_ (state_type yystate, int yysym)
  {
    int yyr = yypgoto_[yysym - YYNTOKENS] + yystate;
    if (0 <= yyr && yyr <= yylast_ && yycheck_[yyr] == yystate)
      return yytable_[yyr];
    else
      return yydefgoto_[yysym - YYNTOKENS];
  }

  bool
  whereParser::yy_pact_value_is_default_ (int yyvalue) YY_NOEXCEPT
  {
    return yyvalue == yypact_ninf_;
  }

  bool
  whereParser::yy_table_value_is_error_ (int yyvalue) YY_NOEXCEPT
  {
    return yyvalue == yytable_ninf_;
  }

  int
  whereParser::operator() ()
  {
    return parse ();
  }

  int
  whereParser::parse ()
  {
    int yyn;
    /// Length of the RHS of the rule being reduced.
    int yylen = 0;

    // Error handling.
    int yynerrs_ = 0;
    int yyerrstatus_ = 0;

    /// The lookahead symbol.
    symbol_type yyla;

    /// The locations where the error started and ended.
    stack_symbol_type yyerror_range[3];

    /// The return value of parse ().
    int yyresult;

#if YY_EXCEPTIONS
    try
#endif // YY_EXCEPTIONS
      {
    YYCDEBUG << "Starting parse\n";


    // User initialization code.
#line 30 "whereParser.yy"
{ // initialize location object
    yyla.location.begin.filename = yyla.location.end.filename = &(driver.clause_);
}

#line 638 "whereParser.cc"


    /* Initialize the stack.  The initial state will be set in
       yynewstate, since the latter expects the semantical and the
       location values to have been already stored, initialize these
       stacks with a primary value.  */
    yystack_.clear ();
    yypush_ (YY_NULLPTR, 0, YY_MOVE (yyla));

  /*-----------------------------------------------.
  | yynewstate -- push a new symbol on the stack.  |
  `-----------------------------------------------*/
  yynewstate:
    YYCDEBUG << "Entering state " << int (yystack_[0].state) << '\n';
    YY_STACK_PRINT ();

    // Accept?
    if (yystack_[0].state == yyfinal_)
      YYACCEPT;

    goto yybackup;


  /*-----------.
  | yybackup.  |
  `-----------*/
  yybackup:
    // Try to take a decision without lookahead.
    yyn = yypact_[+yystack_[0].state];
    if (yy_pact_value_is_default_ (yyn))
      goto yydefault;

    // Read a lookahead token.
    if (yyla.empty ())
      {
        YYCDEBUG << "Reading a token\n";
#if YY_EXCEPTIONS
        try
#endif // YY_EXCEPTIONS
          {
            yyla.kind_ = yytranslate_ (yylex (&yyla.value, &yyla.location));
          }
#if YY_EXCEPTIONS
        catch (const syntax_error& yyexc)
          {
            YYCDEBUG << "Caught exception: " << yyexc.what() << '\n';
            error (yyexc);
            goto yyerrlab1;
          }
#endif // YY_EXCEPTIONS
      }
    YY_SYMBOL_PRINT ("Next token is", yyla);

    if (yyla.kind () == symbol_kind::S_YYerror)
    {
      // The scanner already issued an error message, process directly
      // to error recovery.  But do not keep the error token as
      // lookahead, it is too special and may lead us to an endless
      // loop in error recovery. */
      yyla.kind_ = symbol_kind::S_YYUNDEF;
      goto yyerrlab1;
    }

    /* If the proper action on seeing token YYLA.TYPE is to reduce or
       to detect an error, take that action.  */
    yyn += yyla.kind ();
    if (yyn < 0 || yylast_ < yyn || yycheck_[yyn] != yyla.kind ())
      {
        goto yydefault;
      }

    // Reduce or error.
    yyn = yytable_[yyn];
    if (yyn <= 0)
      {
        if (yy_table_value_is_error_ (yyn))
          goto yyerrlab;
        yyn = -yyn;
        goto yyreduce;
      }

    // Count tokens shifted since error; after three, turn off error status.
    if (yyerrstatus_)
      --yyerrstatus_;

    // Shift the lookahead token.
    yypush_ ("Shifting", state_type (yyn), YY_MOVE (yyla));
    goto yynewstate;


  /*-----------------------------------------------------------.
  | yydefault -- do the default action for the current state.  |
  `-----------------------------------------------------------*/
  yydefault:
    yyn = yydefact_[+yystack_[0].state];
    if (yyn == 0)
      goto yyerrlab;
    goto yyreduce;


  /*-----------------------------.
  | yyreduce -- do a reduction.  |
  `-----------------------------*/
  yyreduce:
    yylen = yyr2_[yyn];
    {
      stack_symbol_type yylhs;
      yylhs.state = yy_lr_goto_state_ (yystack_[yylen].state, yyr1_[yyn]);
      /* If YYLEN is nonzero, implement the default value of the
         action: '$$ = $1'.  Otherwise, use the top of the stack.

         Otherwise, the following line sets YYLHS.VALUE to garbage.
         This behavior is undocumented and Bison users should not rely
         upon it.  */
      if (yylen)
        yylhs.value = yystack_[yylen - 1].value;
      else
        yylhs.value = yystack_[0].value;

      // Default location.
      {
        stack_type::slice range (yystack_, yylen);
        YYLLOC_DEFAULT (yylhs.location, range, yylen);
        yyerror_range[1].location = yylhs.location;
      }

      // Perform the reduction.
      YY_REDUCE_PRINT (yyn);
#if YY_EXCEPTIONS
      try
#endif // YY_EXCEPTIONS
        {
          switch (yyn)
            {
  case 2: // qexpr: qexpr "or" qexpr
#line 185 "whereParser.yy"
                 {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.whereNode)
	<< " || " << *(yystack_[0].value.whereNode);
#endif
    (yylhs.value.whereNode) = new ibis::qExpr(ibis::qExpr::LOGICAL_OR);
    (yylhs.value.whereNode)->setRight((yystack_[0].value.whereNode));
    (yylhs.value.whereNode)->setLeft((yystack_[2].value.whereNode));
}
#line 785 "whereParser.cc"
    break;

  case 3: // qexpr: qexpr "xor" qexpr
#line 195 "whereParser.yy"
                    {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.whereNode)
	<< " ^ " << *(yystack_[0].value.whereNode);
#endif
    (yylhs.value.whereNode) = new ibis::qExpr(ibis::qExpr::LOGICAL_XOR);
    (yylhs.value.whereNode)->setRight((yystack_[0].value.whereNode));
    (yylhs.value.whereNode)->setLeft((yystack_[2].value.whereNode));
}
#line 800 "whereParser.cc"
    break;

  case 4: // qexpr: qexpr "and" qexpr
#line 205 "whereParser.yy"
                    {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.whereNode)
	<< " && " << *(yystack_[0].value.whereNode);
#endif
    (yylhs.value.whereNode) = new ibis::qExpr(ibis::qExpr::LOGICAL_AND);
    (yylhs.value.whereNode)->setRight((yystack_[0].value.whereNode));
    (yylhs.value.whereNode)->setLeft((yystack_[2].value.whereNode));
}
#line 815 "whereParser.cc"
    break;

  case 5: // qexpr: qexpr "&!" qexpr
#line 215 "whereParser.yy"
                       {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.whereNode)
	<< " &~ " << *(yystack_[0].value.whereNode);
#endif
    (yylhs.value.whereNode) = new ibis::qExpr(ibis::qExpr::LOGICAL_MINUS);
    (yylhs.value.whereNode)->setRight((yystack_[0].value.whereNode));
    (yylhs.value.whereNode)->setLeft((yystack_[2].value.whereNode));
}
#line 830 "whereParser.cc"
    break;

  case 6: // qexpr: "not" qexpr
#line 225 "whereParser.yy"
              {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- ! " << *(yystack_[0].value.whereNode);
#endif
    (yylhs.value.whereNode) = new ibis::qExpr(ibis::qExpr::LOGICAL_NOT);
    (yylhs.value.whereNode)->setLeft((yystack_[0].value.whereNode));
}
#line 843 "whereParser.cc"
    break;

  case 7: // qexpr: '(' qexpr ')'
#line 233 "whereParser.yy"
                            {
    (yylhs.value.whereNode) = (yystack_[1].value.whereNode);
}
#line 851 "whereParser.cc"
    break;

  case 8: // qexpr: simpleRange
#line 236 "whereParser.yy"
  { (yylhs.value.whereNode) = (yystack_[0].value.whereNode); }
#line 857 "whereParser.cc"
    break;

  case 9: // qexpr: compRange2
#line 237 "whereParser.yy"
  { (yylhs.value.whereNode) = (yystack_[0].value.whereNode); }
#line 863 "whereParser.cc"
    break;

  case 10: // qexpr: compRange3
#line 238 "whereParser.yy"
  { (yylhs.value.whereNode) = (yystack_[0].value.whereNode); }
#line 869 "whereParser.cc"
    break;

  case 11: // simpleRange: "exists" "name string"
#line 242 "whereParser.yy"
                 {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- EXISTS(" << *(yystack_[0].value.stringVal) << ')';
#endif
    (yylhs.value.whereNode) = new ibis::qExists((yystack_[0].value.stringVal)->c_str());
    delete (yystack_[0].value.stringVal);
}
#line 882 "whereParser.cc"
    break;

  case 12: // simpleRange: "exists" "string literal"
#line 250 "whereParser.yy"
                  {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- EXISTS(" << *(yystack_[0].value.stringVal) << ')';
#endif
    (yylhs.value.whereNode) = new ibis::qExists((yystack_[0].value.stringVal)->c_str());
    delete (yystack_[0].value.stringVal);
}
#line 895 "whereParser.cc"
    break;

  case 13: // simpleRange: "exists" '(' "name string" ')'
#line 258 "whereParser.yy"
                           {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- EXISTS(" << *(yystack_[1].value.stringVal) << ')';
#endif
    (yylhs.value.whereNode) = new ibis::qExists((yystack_[1].value.stringVal)->c_str());
    delete (yystack_[1].value.stringVal);
}
#line 908 "whereParser.cc"
    break;

  case 14: // simpleRange: "exists" '(' "string literal" ')'
#line 266 "whereParser.yy"
                          {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- EXISTS(" << *(yystack_[1].value.stringVal) << ')';
#endif
    (yylhs.value.whereNode) = new ibis::qExists((yystack_[1].value.stringVal)->c_str());
    delete (yystack_[1].value.stringVal);
}
#line 921 "whereParser.cc"
    break;

  case 15: // simpleRange: "name string" "in" "number sequence"
#line 274 "whereParser.yy"
                      {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.stringVal) << " IN ("
	<< *(yystack_[0].value.stringVal) << ")";
#endif
    (yylhs.value.whereNode) = new ibis::qDiscreteRange((yystack_[2].value.stringVal)->c_str(), (yystack_[0].value.stringVal)->c_str());
    delete (yystack_[0].value.stringVal);
    delete (yystack_[2].value.stringVal);
}
#line 936 "whereParser.cc"
    break;

  case 16: // simpleRange: "name string" "in" '(' "floating-point number" ',' "floating-point number" ')'
#line 284 "whereParser.yy"
                                         {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[6].value.stringVal) << " IN ("
	<< (yystack_[3].value.doubleVal) << ", " << (yystack_[1].value.doubleVal) << ")";
#endif
    std::vector<double> vals(2);
    vals[0] = (yystack_[3].value.doubleVal);
    vals[1] = (yystack_[1].value.doubleVal);
    (yylhs.value.whereNode) = new ibis::qDiscreteRange((yystack_[6].value.stringVal)->c_str(), vals);
    delete (yystack_[6].value.stringVal);
}
#line 953 "whereParser.cc"
    break;

  case 17: // simpleRange: "name string" "in" '(' "floating-point number" ')'
#line 296 "whereParser.yy"
                              {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[4].value.stringVal) << " IN ("
	<< (yystack_[1].value.doubleVal) << ")";
#endif
    (yylhs.value.whereNode) = new ibis::qContinuousRange((yystack_[4].value.stringVal)->c_str(), ibis::qExpr::OP_EQ, (yystack_[1].value.doubleVal));
    delete (yystack_[4].value.stringVal);
}
#line 967 "whereParser.cc"
    break;

  case 18: // simpleRange: "name string" "not" "null"
#line 305 "whereParser.yy"
                       {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.stringVal) << " NOT NULL";
#endif
    (yylhs.value.whereNode) = new ibis::qContinuousRange((yystack_[2].value.stringVal)->c_str(), ibis::qExpr::OP_UNDEFINED, 0U);
}
#line 979 "whereParser.cc"
    break;

  case 19: // simpleRange: "name string" "not" "in" "number sequence"
#line 312 "whereParser.yy"
                            {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[3].value.stringVal) << " NOT IN ("
	<< *(yystack_[0].value.stringVal) << ")";
#endif
    (yylhs.value.whereNode) = new ibis::qExpr(ibis::qExpr::LOGICAL_NOT);
    (yylhs.value.whereNode)->setLeft(new ibis::qDiscreteRange((yystack_[3].value.stringVal)->c_str(), (yystack_[0].value.stringVal)->c_str()));
    delete (yystack_[0].value.stringVal);
    delete (yystack_[3].value.stringVal);
}
#line 995 "whereParser.cc"
    break;

  case 20: // simpleRange: "name string" "not" "in" '(' "floating-point number" ',' "floating-point number" ')'
#line 323 "whereParser.yy"
                                               {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[7].value.stringVal) << " NOT IN ("
	<< (yystack_[3].value.doubleVal) << ", " << (yystack_[1].value.doubleVal) << ")";
#endif
    std::vector<double> vals(2);
    vals[0] = (yystack_[3].value.doubleVal);
    vals[1] = (yystack_[1].value.doubleVal);
    (yylhs.value.whereNode) = new ibis::qExpr(ibis::qExpr::LOGICAL_NOT);
    (yylhs.value.whereNode)->setLeft(new ibis::qDiscreteRange((yystack_[7].value.stringVal)->c_str(), vals));
    delete (yystack_[7].value.stringVal);
}
#line 1013 "whereParser.cc"
    break;

  case 21: // simpleRange: "name string" "not" "in" '(' "floating-point number" ')'
#line 336 "whereParser.yy"
                                    {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[5].value.stringVal) << " NOT IN ("
	<< (yystack_[1].value.doubleVal) << ")";
#endif
    (yylhs.value.whereNode) = new ibis::qExpr(ibis::qExpr::LOGICAL_NOT);
    (yylhs.value.whereNode)->setLeft(new ibis::qContinuousRange((yystack_[5].value.stringVal)->c_str(), ibis::qExpr::OP_EQ, (yystack_[1].value.doubleVal)));
    delete (yystack_[5].value.stringVal);
}
#line 1028 "whereParser.cc"
    break;

  case 22: // simpleRange: "name string" "in" "string sequence"
#line 346 "whereParser.yy"
                      {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.stringVal) << " IN ("
	<< *(yystack_[0].value.stringVal) << ")";
#endif
    (yylhs.value.whereNode) = new ibis::qAnyString((yystack_[2].value.stringVal)->c_str(), (yystack_[0].value.stringVal)->c_str());
    delete (yystack_[0].value.stringVal);
    delete (yystack_[2].value.stringVal);
}
#line 1043 "whereParser.cc"
    break;

  case 23: // simpleRange: "name string" "in" '(' "name string" ',' "name string" ')'
#line 356 "whereParser.yy"
                                           {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[6].value.stringVal) << " IN ("
	<< *(yystack_[3].value.stringVal) << ", " << *(yystack_[1].value.stringVal) << ")";
#endif
    std::string val;
    val = listValue(*(yystack_[3].value.stringVal)) + ", " + listValue(*(yystack_[1].value.stringVal));
    (yylhs.value.whereNode) = new ibis::qAnyString((yystack_[6].value.stringVal)->c_str(), val.c_str());
    delete (yystack_[1].value.stringVal);
    delete (yystack_[3].value.stringVal);
    delete (yystack_[6].value.stringVal);
}
#line 1061 "whereParser.cc"
    break;

  case 24: // simpleRange: "name string" "in" '(' "string literal" ',' "name string" ')'
#line 369 "whereParser.yy"
                                          {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[6].value.stringVal) << " IN ("
	<< *(yystack_[3].value.stringVal) << ", " << *(yystack_[1].value.stringVal) << ")";
#endif
    std::string val;
    val = listValue(*(yystack_[3].value.stringVal)) + ", " + listValue(*(yystack_[1].value.stringVal));
    (yylhs.value.whereNode) = new ibis::qAnyString((yystack_[6].value.stringVal)->c_str(), val.c_str());
    delete (yystack_[1].value.stringVal);
    delete (yystack_[3].value.stringVal);
    delete (yystack_[6].value.stringVal);
}
#line 1079 "whereParser.cc"
    break;

  case 25: // simpleRange: "name string" "in" '(' "name string" ',' "string literal" ')'
#line 382 "whereParser.yy"
                                          {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[6].value.stringVal) << " IN ("
	<< *(yystack_[3].value.stringVal) << ", " << *(yystack_[1].value.stringVal) << ")";
#endif
    std::string val;
    val = listValue(*(yystack_[3].value.stringVal)) + ", " + listValue(*(yystack_[1].value.stringVal));
    (yylhs.value.whereNode) = new ibis::qAnyString((yystack_[6].value.stringVal)->c_str(), val.c_str());
    delete (yystack_[1].value.stringVal);
    delete (yystack_[3].value.stringVal);
    delete (yystack_[6].value.stringVal);
}
#line 1097 "whereParser.cc"
    break;

  case 26: // simpleRange: "name string" "in" '(' "string literal" ',' "string literal" ')'
#line 395 "whereParser.yy"
                                         {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[6].value.stringVal) << " IN ("
	<< *(yystack_[3].value.stringVal) << ", " << *(yystack_[1].value.stringVal) << ")";
#endif
    std::string val;
    val = listValue(*(yystack_[3].value.stringVal)) + ", " + listValue(*(yystack_[1].value.stringVal));
    (yylhs.value.whereNode) = new ibis::qAnyString((yystack_[6].value.stringVal)->c_str(), val.c_str());
    delete (yystack_[1].value.stringVal);
    delete (yystack_[3].value.stringVal);
    delete (yystack_[6].value.stringVal);
}
#line 1115 "whereParser.cc"
    break;

  case 27: // simpleRange: "name string" "in" '(' "name string" ')'
#line 408 "whereParser.yy"
                               {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[4].value.stringVal) << " IN ("
	<< *(yystack_[1].value.stringVal) << ")";
#endif
    std::string val;
    val = listValue(*(yystack_[1].value.stringVal));
    (yylhs.value.whereNode) = new ibis::qAnyString((yystack_[4].value.stringVal)->c_str(), val.c_str());
    delete (yystack_[1].value.stringVal);
    delete (yystack_[4].value.stringVal);
}
#line 1132 "whereParser.cc"
    break;

  case 28: // simpleRange: "name string" "in" '(' "string literal" ')'
#line 420 "whereParser.yy"
                              {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[4].value.stringVal) << " IN ("
	<< *(yystack_[1].value.stringVal) << ")";
#endif
    std::string val;
    val = listValue(*(yystack_[1].value.stringVal));
    (yylhs.value.whereNode) = new ibis::qAnyString((yystack_[4].value.stringVal)->c_str(), val.c_str());
    delete (yystack_[1].value.stringVal);
    delete (yystack_[4].value.stringVal);
}
#line 1149 "whereParser.cc"
    break;

  case 29: // simpleRange: "collate_N" "name string" "in" "string sequence"
#line 432 "whereParser.yy"
                                {
    const uint32_t coll = collationNumber((yystack_[3].value.integerVal));
    (yylhs.value.whereNode) = new ibis::qAnyString((yystack_[2].value.stringVal)->c_str(), (yystack_[0].value.stringVal)->c_str(), coll);
    delete (yystack_[0].value.stringVal);
    delete (yystack_[2].value.stringVal);
}
#line 1160 "whereParser.cc"
    break;

  case 30: // simpleRange: "collate_N" "name string" "in" '(' "string literal" ',' "string literal" ')'
#line 438 "whereParser.yy"
                                                   {
    const uint32_t coll = collationNumber((yystack_[7].value.integerVal));
    std::string val;
    val = listValue(*(yystack_[3].value.stringVal)) + ", " + listValue(*(yystack_[1].value.stringVal));
    (yylhs.value.whereNode) = new ibis::qAnyString((yystack_[6].value.stringVal)->c_str(), val.c_str(), coll);
    delete (yystack_[1].value.stringVal);
    delete (yystack_[3].value.stringVal);
    delete (yystack_[6].value.stringVal);
}
#line 1174 "whereParser.cc"
    break;

  case 31: // simpleRange: "collate_N" "name string" "in" '(' "string literal" ')'
#line 447 "whereParser.yy"
                                        {
    const uint32_t coll = collationNumber((yystack_[5].value.integerVal));
    std::string val;
    val = listValue(*(yystack_[1].value.stringVal));
    (yylhs.value.whereNode) = new ibis::qAnyString((yystack_[4].value.stringVal)->c_str(), val.c_str(), coll);
    delete (yystack_[1].value.stringVal);
    delete (yystack_[4].value.stringVal);
}
#line 1187 "whereParser.cc"
    break;

  case 32: // simpleRange: "name string" "like" "name string"
#line 455 "whereParser.yy"
                         {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.stringVal) << " LIKE "
	<< *(yystack_[0].value.stringVal);
#endif
    (yylhs.value.whereNode) = new ibis::qLike((yystack_[2].value.stringVal)->c_str(), (yystack_[0].value.stringVal)->c_str());
    delete (yystack_[0].value.stringVal);
    delete (yystack_[2].value.stringVal);
}
#line 1202 "whereParser.cc"
    break;

  case 33: // simpleRange: "name string" "like" "string literal"
#line 465 "whereParser.yy"
                        {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.stringVal) << " LIKE "
	<< *(yystack_[0].value.stringVal);
#endif
    (yylhs.value.whereNode) = new ibis::qLike((yystack_[2].value.stringVal)->c_str(), (yystack_[0].value.stringVal)->c_str());
    delete (yystack_[0].value.stringVal);
    delete (yystack_[2].value.stringVal);
}
#line 1217 "whereParser.cc"
    break;

  case 34: // simpleRange: "name string" "not" "in" "string sequence"
#line 475 "whereParser.yy"
                            {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[3].value.stringVal) << " NOT IN ("
	<< *(yystack_[0].value.stringVal) << ")";
#endif
    (yylhs.value.whereNode) = new ibis::qExpr(ibis::qExpr::LOGICAL_NOT);
    (yylhs.value.whereNode)->setLeft(new ibis::qAnyString((yystack_[3].value.stringVal)->c_str(), (yystack_[0].value.stringVal)->c_str()));
    delete (yystack_[0].value.stringVal);
    delete (yystack_[3].value.stringVal);
}
#line 1233 "whereParser.cc"
    break;

  case 35: // simpleRange: "name string" "not" "in" '(' "name string" ',' "name string" ')'
#line 486 "whereParser.yy"
                                                 {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[7].value.stringVal) << " NOT IN ("
	<< *(yystack_[3].value.stringVal) << ", " << *(yystack_[1].value.stringVal) << ")";
#endif
    std::string val;
    val = listValue(*(yystack_[3].value.stringVal)) + ", " + listValue(*(yystack_[1].value.stringVal));
    (yylhs.value.whereNode) = new ibis::qExpr(ibis::qExpr::LOGICAL_NOT);
    (yylhs.value.whereNode)->setLeft(new ibis::qAnyString((yystack_[7].value.stringVal)->c_str(), val.c_str()));
    delete (yystack_[1].value.stringVal);
    delete (yystack_[3].value.stringVal);
    delete (yystack_[7].value.stringVal);
}
#line 1252 "whereParser.cc"
    break;

  case 36: // simpleRange: "name string" "not" "in" '(' "string literal" ',' "name string" ')'
#line 500 "whereParser.yy"
                                                {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[7].value.stringVal) << " NOT IN ("
	<< *(yystack_[3].value.stringVal) << ", " << *(yystack_[1].value.stringVal) << ")";
#endif
    std::string val;
    val = listValue(*(yystack_[3].value.stringVal)) + ", " + listValue(*(yystack_[1].value.stringVal));
    (yylhs.value.whereNode) = new ibis::qExpr(ibis::qExpr::LOGICAL_NOT);
    (yylhs.value.whereNode)->setLeft(new ibis::qAnyString((yystack_[7].value.stringVal)->c_str(), val.c_str()));
    delete (yystack_[1].value.stringVal);
    delete (yystack_[3].value.stringVal);
    delete (yystack_[7].value.stringVal);
}
#line 1271 "whereParser.cc"
    break;

  case 37: // simpleRange: "name string" "not" "in" '(' "name string" ',' "string literal" ')'
#line 514 "whereParser.yy"
                                                {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[7].value.stringVal) << " NOT IN ("
	<< *(yystack_[3].value.stringVal) << ", " << *(yystack_[1].value.stringVal) << ")";
#endif
    std::string val;
    val = listValue(*(yystack_[3].value.stringVal)) + ", " + listValue(*(yystack_[1].value.stringVal));
    (yylhs.value.whereNode) = new ibis::qExpr(ibis::qExpr::LOGICAL_NOT);
    (yylhs.value.whereNode)->setLeft(new ibis::qAnyString((yystack_[7].value.stringVal)->c_str(), val.c_str()));
    delete (yystack_[1].value.stringVal);
    delete (yystack_[3].value.stringVal);
    delete (yystack_[7].value.stringVal);
}
#line 1290 "whereParser.cc"
    break;

  case 38: // simpleRange: "name string" "not" "in" '(' "string literal" ',' "string literal" ')'
#line 528 "whereParser.yy"
                                               {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[7].value.stringVal) << " NOT IN ("
	<< *(yystack_[3].value.stringVal) << ", " << *(yystack_[1].value.stringVal) << ")";
#endif
    std::string val;
    val = listValue(*(yystack_[3].value.stringVal)) + ", " + listValue(*(yystack_[1].value.stringVal));
    (yylhs.value.whereNode) = new ibis::qExpr(ibis::qExpr::LOGICAL_NOT);
    (yylhs.value.whereNode)->setLeft(new ibis::qAnyString((yystack_[7].value.stringVal)->c_str(), val.c_str()));
    delete (yystack_[1].value.stringVal);
    delete (yystack_[3].value.stringVal);
    delete (yystack_[7].value.stringVal);
}
#line 1309 "whereParser.cc"
    break;

  case 39: // simpleRange: "name string" "not" "in" '(' "name string" ')'
#line 542 "whereParser.yy"
                                     {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[5].value.stringVal) << " NOT IN ("
	<< *(yystack_[1].value.stringVal) << ")";
#endif
    std::string val;
    val = listValue(*(yystack_[1].value.stringVal));
    (yylhs.value.whereNode) = new ibis::qExpr(ibis::qExpr::LOGICAL_NOT);
    (yylhs.value.whereNode)->setLeft(new ibis::qAnyString((yystack_[5].value.stringVal)->c_str(), val.c_str()));
    delete (yystack_[1].value.stringVal);
    delete (yystack_[5].value.stringVal);
}
#line 1327 "whereParser.cc"
    break;

  case 40: // simpleRange: "name string" "not" "in" '(' "string literal" ')'
#line 555 "whereParser.yy"
                                    {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[5].value.stringVal) << " NOT IN ("
	<< *(yystack_[1].value.stringVal) << ")";
#endif
    std::string val;
    val = listValue(*(yystack_[1].value.stringVal));
    (yylhs.value.whereNode) = new ibis::qExpr(ibis::qExpr::LOGICAL_NOT);
    (yylhs.value.whereNode)->setLeft(new ibis::qAnyString((yystack_[5].value.stringVal)->c_str(), val.c_str()));
    delete (yystack_[1].value.stringVal);
    delete (yystack_[5].value.stringVal);
}
#line 1345 "whereParser.cc"
    break;

  case 41: // simpleRange: "name string" "in" "signed integer sequence"
#line 568 "whereParser.yy"
                      {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.stringVal) << " in ("
	<< *(yystack_[0].value.stringVal) << ")";
#endif
    (yylhs.value.whereNode) = new ibis::qIntHod((yystack_[2].value.stringVal)->c_str(), (yystack_[0].value.stringVal)->c_str());
    delete (yystack_[0].value.stringVal);
    delete (yystack_[2].value.stringVal);
}
#line 1360 "whereParser.cc"
    break;

  case 42: // simpleRange: "name string" "not" "in" "signed integer sequence"
#line 578 "whereParser.yy"
                            {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[3].value.stringVal) << " not in ("
	<< *(yystack_[0].value.stringVal) << ")";
#endif
    (yylhs.value.whereNode) = new ibis::qExpr(ibis::qExpr::LOGICAL_NOT);
    (yylhs.value.whereNode)->setLeft(new ibis::qIntHod((yystack_[3].value.stringVal)->c_str(), (yystack_[0].value.stringVal)->c_str()));
    delete (yystack_[0].value.stringVal);
    delete (yystack_[3].value.stringVal);
}
#line 1376 "whereParser.cc"
    break;

  case 43: // simpleRange: "name string" "in" "unsigned integer sequence"
#line 589 "whereParser.yy"
                       {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.stringVal) << " in ("
	<< *(yystack_[0].value.stringVal) << ")";
#endif
    (yylhs.value.whereNode) = new ibis::qUIntHod((yystack_[2].value.stringVal)->c_str(), (yystack_[0].value.stringVal)->c_str());
    delete (yystack_[0].value.stringVal);
    delete (yystack_[2].value.stringVal);
}
#line 1391 "whereParser.cc"
    break;

  case 44: // simpleRange: "name string" "not" "in" "unsigned integer sequence"
#line 599 "whereParser.yy"
                             {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[3].value.stringVal) << " not in ("
	<< *(yystack_[0].value.stringVal) << ")";
#endif
    (yylhs.value.whereNode) = new ibis::qExpr(ibis::qExpr::LOGICAL_NOT);
    (yylhs.value.whereNode)->setLeft(new ibis::qUIntHod((yystack_[3].value.stringVal)->c_str(), (yystack_[0].value.stringVal)->c_str()));
    delete (yystack_[0].value.stringVal);
    delete (yystack_[3].value.stringVal);
}
#line 1407 "whereParser.cc"
    break;

  case 45: // simpleRange: "name string" "contains" "name string"
#line 610 "whereParser.yy"
                             {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.stringVal)
	<< " CONTAINS " << *(yystack_[0].value.stringVal);
#endif
    (yylhs.value.whereNode) = new ibis::qKeyword((yystack_[2].value.stringVal)->c_str(), (yystack_[0].value.stringVal)->c_str());
    delete (yystack_[2].value.stringVal);
    delete (yystack_[0].value.stringVal);
}
#line 1422 "whereParser.cc"
    break;

  case 46: // simpleRange: "name string" "contains" "string literal"
#line 620 "whereParser.yy"
                            {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << (yystack_[2].value.stringVal)
	<< " CONTAINS " << *(yystack_[0].value.stringVal);
#endif
    (yylhs.value.whereNode) = new ibis::qKeyword((yystack_[2].value.stringVal)->c_str(), (yystack_[0].value.stringVal)->c_str());
    delete (yystack_[0].value.stringVal);
    delete (yystack_[2].value.stringVal);
}
#line 1437 "whereParser.cc"
    break;

  case 47: // simpleRange: "name string" "contains" '(' "name string" ')'
#line 630 "whereParser.yy"
                                     {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[4].value.stringVal)
	<< " CONTAINS " << *(yystack_[1].value.stringVal);
#endif
    (yylhs.value.whereNode) = new ibis::qKeyword((yystack_[4].value.stringVal)->c_str(), (yystack_[1].value.stringVal)->c_str());
    delete (yystack_[4].value.stringVal);
    delete (yystack_[1].value.stringVal);
}
#line 1452 "whereParser.cc"
    break;

  case 48: // simpleRange: "name string" "contains" '(' "string literal" ')'
#line 640 "whereParser.yy"
                                    {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << (yystack_[4].value.stringVal)
	<< " CONTAINS " << *(yystack_[1].value.stringVal);
#endif
    (yylhs.value.whereNode) = new ibis::qKeyword((yystack_[4].value.stringVal)->c_str(), (yystack_[1].value.stringVal)->c_str());
    delete (yystack_[1].value.stringVal);
    delete (yystack_[4].value.stringVal);
}
#line 1467 "whereParser.cc"
    break;

  case 49: // simpleRange: "name string" "contains" '(' "string literal" ',' "string literal" ')'
#line 650 "whereParser.yy"
                                               {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << (yystack_[6].value.stringVal)
	<< " CONTAINS (" << *(yystack_[3].value.stringVal) << ", " << *(yystack_[1].value.stringVal) << ')';
#endif
    (yylhs.value.whereNode) = new ibis::qAllWords((yystack_[6].value.stringVal)->c_str(), (yystack_[3].value.stringVal)->c_str(), (yystack_[1].value.stringVal)->c_str());
    delete (yystack_[1].value.stringVal);
    delete (yystack_[3].value.stringVal);
    delete (yystack_[6].value.stringVal);
}
#line 1483 "whereParser.cc"
    break;

  case 50: // simpleRange: "name string" "contains" '(' "string literal" ',' "name string" ')'
#line 661 "whereParser.yy"
                                                {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << (yystack_[6].value.stringVal)
	<< " CONTAINS (" << *(yystack_[3].value.stringVal) << ", " << *(yystack_[1].value.stringVal) << ')';
#endif
    (yylhs.value.whereNode) = new ibis::qAllWords((yystack_[6].value.stringVal)->c_str(), (yystack_[3].value.stringVal)->c_str(), (yystack_[1].value.stringVal)->c_str());
    delete (yystack_[1].value.stringVal);
    delete (yystack_[3].value.stringVal);
    delete (yystack_[6].value.stringVal);
}
#line 1499 "whereParser.cc"
    break;

  case 51: // simpleRange: "name string" "contains" '(' "name string" ',' "string literal" ')'
#line 672 "whereParser.yy"
                                                {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << (yystack_[6].value.stringVal)
	<< " CONTAINS (" << *(yystack_[3].value.stringVal) << ", " << *(yystack_[1].value.stringVal) << ')';
#endif
    (yylhs.value.whereNode) = new ibis::qAllWords((yystack_[6].value.stringVal)->c_str(), (yystack_[3].value.stringVal)->c_str(), (yystack_[1].value.stringVal)->c_str());
    delete (yystack_[1].value.stringVal);
    delete (yystack_[3].value.stringVal);
    delete (yystack_[6].value.stringVal);
}
#line 1515 "whereParser.cc"
    break;

  case 52: // simpleRange: "name string" "contains" '(' "name string" ',' "name string" ')'
#line 683 "whereParser.yy"
                                                 {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << (yystack_[6].value.stringVal)
	<< " CONTAINS (" << *(yystack_[3].value.stringVal) << ", " << *(yystack_[1].value.stringVal) << ')';
#endif
    (yylhs.value.whereNode) = new ibis::qAllWords((yystack_[6].value.stringVal)->c_str(), (yystack_[3].value.stringVal)->c_str(), (yystack_[1].value.stringVal)->c_str());
    delete (yystack_[1].value.stringVal);
    delete (yystack_[3].value.stringVal);
    delete (yystack_[6].value.stringVal);
}
#line 1531 "whereParser.cc"
    break;

  case 53: // simpleRange: "name string" "contains" "string sequence"
#line 694 "whereParser.yy"
                            {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << (yystack_[2].value.stringVal)
	<< " CONTAINS (" << *(yystack_[0].value.stringVal) << ')';
#endif
    (yylhs.value.whereNode) = new ibis::qAllWords((yystack_[2].value.stringVal)->c_str(), (yystack_[0].value.stringVal)->c_str());
    delete (yystack_[0].value.stringVal);
    delete (yystack_[2].value.stringVal);
}
#line 1546 "whereParser.cc"
    break;

  case 54: // simpleRange: "any" '(' "name string" ')' "==" "floating-point number"
#line 704 "whereParser.yy"
                                    {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- ANY(" << *(yystack_[3].value.stringVal) << ") = "
	<< (yystack_[0].value.doubleVal) << ")";
#endif
    (yylhs.value.whereNode) = new ibis::qAnyAny((yystack_[3].value.stringVal)->c_str(), (yystack_[0].value.doubleVal));
    delete (yystack_[3].value.stringVal);
}
#line 1560 "whereParser.cc"
    break;

  case 55: // simpleRange: "any" '(' "name string" ')' "in" "number sequence"
#line 713 "whereParser.yy"
                                    {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- ANY(" << *(yystack_[3].value.stringVal) << ") = "
	<< *(yystack_[0].value.stringVal) << ")";
#endif
    (yylhs.value.whereNode) = new ibis::qAnyAny((yystack_[3].value.stringVal)->c_str(), (yystack_[0].value.stringVal)->c_str());
    delete (yystack_[0].value.stringVal);
    delete (yystack_[3].value.stringVal);
}
#line 1575 "whereParser.cc"
    break;

  case 56: // simpleRange: "name string" "==" "integer value"
#line 723 "whereParser.yy"
                     {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.stringVal) << " = " << *(yystack_[0].value.int64Val);
#endif
    (yylhs.value.whereNode) = new ibis::qIntHod((yystack_[2].value.stringVal)->c_str(), (yystack_[0].value.int64Val));
    delete (yystack_[2].value.stringVal);
}
#line 1588 "whereParser.cc"
    break;

  case 57: // simpleRange: "name string" "!=" "integer value"
#line 731 "whereParser.yy"
                      {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.stringVal) << " != " << *(yystack_[0].value.int64Val);
#endif
    (yylhs.value.whereNode) = new ibis::qExpr(ibis::qExpr::LOGICAL_NOT);
    (yylhs.value.whereNode)->setLeft(new ibis::qIntHod((yystack_[2].value.stringVal)->c_str(), (yystack_[0].value.int64Val)));
    delete (yystack_[2].value.stringVal);
}
#line 1602 "whereParser.cc"
    break;

  case 58: // simpleRange: "name string" "==" "unsigned integer value"
#line 740 "whereParser.yy"
                      {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.stringVal) << " = " << *(yystack_[0].value.uint64Val);
#endif
    (yylhs.value.whereNode) = new ibis::qUIntHod((yystack_[2].value.stringVal)->c_str(), (yystack_[0].value.uint64Val));
    delete (yystack_[2].value.stringVal);
}
#line 1615 "whereParser.cc"
    break;

  case 59: // simpleRange: "name string" "!=" "unsigned integer value"
#line 748 "whereParser.yy"
                       {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.stringVal) << " != " << *(yystack_[0].value.uint64Val);
#endif
    (yylhs.value.whereNode) = new ibis::qExpr(ibis::qExpr::LOGICAL_NOT);
    (yylhs.value.whereNode)->setLeft(new ibis::qUIntHod((yystack_[2].value.stringVal)->c_str(), (yystack_[0].value.uint64Val)));
    delete (yystack_[2].value.stringVal);
}
#line 1629 "whereParser.cc"
    break;

  case 60: // simpleRange: "string literal" "==" "name string"
#line 757 "whereParser.yy"
                      {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[0].value.stringVal) << " = "
	<< *(yystack_[2].value.stringVal);
#endif
    (yylhs.value.whereNode) = new ibis::qString((yystack_[0].value.stringVal)->c_str(), (yystack_[2].value.stringVal)->c_str());
    delete (yystack_[0].value.stringVal);
    delete (yystack_[2].value.stringVal);
}
#line 1644 "whereParser.cc"
    break;

  case 61: // simpleRange: "string literal" "!=" "name string"
#line 767 "whereParser.yy"
                       {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[0].value.stringVal) << " = "
	<< *(yystack_[2].value.stringVal);
#endif
    (yylhs.value.whereNode) = new ibis::qExpr(ibis::qExpr::LOGICAL_NOT);
    (yylhs.value.whereNode)->setLeft(new ibis::qString((yystack_[0].value.stringVal)->c_str(), (yystack_[2].value.stringVal)->c_str()));
    delete (yystack_[0].value.stringVal);
    delete (yystack_[2].value.stringVal);
}
#line 1660 "whereParser.cc"
    break;

  case 62: // simpleRange: "name string" "==" "string literal"
#line 778 "whereParser.yy"
                      {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.stringVal) << " = "
	<< *(yystack_[0].value.stringVal);
#endif
    (yylhs.value.whereNode) = new ibis::qString((yystack_[2].value.stringVal)->c_str(), (yystack_[0].value.stringVal)->c_str());
    delete (yystack_[0].value.stringVal);
    delete (yystack_[2].value.stringVal);
}
#line 1675 "whereParser.cc"
    break;

  case 63: // simpleRange: "name string" "!=" "string literal"
#line 788 "whereParser.yy"
                       {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.stringVal) << " != "
	<< *(yystack_[0].value.stringVal);
#endif
    (yylhs.value.whereNode) = new ibis::qExpr(ibis::qExpr::LOGICAL_NOT);
    (yylhs.value.whereNode)->setLeft(new ibis::qString((yystack_[2].value.stringVal)->c_str(), (yystack_[0].value.stringVal)->c_str()));
    delete (yystack_[0].value.stringVal);
    delete (yystack_[2].value.stringVal);
}
#line 1691 "whereParser.cc"
    break;

  case 64: // simpleRange: "name string" "==" mathExpr
#line 799 "whereParser.yy"
                        {
    ibis::math::term *me2 = static_cast<ibis::math::term*>((yystack_[0].value.whereNode));
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.stringVal) << " = "
	<< *me2;
#endif
    if (me2->termType() == ibis::math::NUMBER) {
	(yylhs.value.whereNode) = new ibis::qContinuousRange((yystack_[2].value.stringVal)->c_str(), ibis::qExpr::OP_EQ, me2->eval());
	delete (yystack_[0].value.whereNode);
    }
    else {
	ibis::math::variable *me1 = new ibis::math::variable((yystack_[2].value.stringVal)->c_str());
	(yylhs.value.whereNode) = new ibis::compRange(me1, ibis::qExpr::OP_EQ, me2);
    }
    delete (yystack_[2].value.stringVal);
}
#line 1713 "whereParser.cc"
    break;

  case 65: // simpleRange: "name string" "!=" mathExpr
#line 816 "whereParser.yy"
                         {
    ibis::math::term *me2 = static_cast<ibis::math::term*>((yystack_[0].value.whereNode));
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.stringVal) << " = "
	<< *me2;
#endif
    ibis::qExpr*tmp = 0;
    if (me2->termType() == ibis::math::NUMBER) {
	tmp = new ibis::qContinuousRange((yystack_[2].value.stringVal)->c_str(), ibis::qExpr::OP_EQ, me2->eval());
	delete (yystack_[0].value.whereNode);
    }
    else {
	ibis::math::variable *me1 = new ibis::math::variable((yystack_[2].value.stringVal)->c_str());
	tmp = new ibis::compRange(me1, ibis::qExpr::OP_EQ, me2);
    }
    delete (yystack_[2].value.stringVal);
    (yylhs.value.whereNode) = new ibis::qExpr(ibis::qExpr::LOGICAL_NOT);
    (yylhs.value.whereNode)->setLeft(tmp);
}
#line 1738 "whereParser.cc"
    break;

  case 66: // compRange2: mathExpr "<" "integer value"
#line 839 "whereParser.yy"
                    {
    /* exact comparisons with 64-bit integers, see qIntHod::compare */
    (yylhs.value.whereNode) = ibis::qIntHod::compare(static_cast<ibis::math::term*>((yystack_[2].value.whereNode)),
                                ibis::qExpr::OP_LT, (yystack_[0].value.int64Val));
}
#line 1748 "whereParser.cc"
    break;

  case 67: // compRange2: "integer value" "<" mathExpr
#line 844 "whereParser.yy"
                      {
    (yylhs.value.whereNode) = ibis::qIntHod::compare(static_cast<ibis::math::term*>((yystack_[0].value.whereNode)),
                                ibis::qExpr::OP_GT, (yystack_[2].value.int64Val));
}
#line 1757 "whereParser.cc"
    break;

  case 68: // compRange2: mathExpr "<=" "integer value"
#line 848 "whereParser.yy"
                      {
    (yylhs.value.whereNode) = ibis::qIntHod::compare(static_cast<ibis::math::term*>((yystack_[2].value.whereNode)),
                                ibis::qExpr::OP_LE, (yystack_[0].value.int64Val));
}
#line 1766 "whereParser.cc"
    break;

  case 69: // compRange2: "integer value" "<=" mathExpr
#line 852 "whereParser.yy"
                      {
    (yylhs.value.whereNode) = ibis::qIntHod::compare(static_cast<ibis::math::term*>((yystack_[0].value.whereNode)),
                                ibis::qExpr::OP_GE, (yystack_[2].value.int64Val));
}
#line 1775 "whereParser.cc"
    break;

  case 70: // compRange2: mathExpr ">" "integer value"
#line 856 "whereParser.yy"
                      {
    (yylhs.value.whereNode) = ibis::qIntHod::compare(static_cast<ibis::math::term*>((yystack_[2].value.whereNode)),
                                ibis::qExpr::OP_GT, (yystack_[0].value.int64Val));
}
#line 1784 "whereParser.cc"
    break;

  case 71: // compRange2: "integer value" ">" mathExpr
#line 860 "whereParser.yy"
                      {
    (yylhs.value.whereNode) = ibis::qIntHod::compare(static_cast<ibis::math::term*>((yystack_[0].value.whereNode)),
                                ibis::qExpr::OP_LT, (yystack_[2].value.int64Val));
}
#line 1793 "whereParser.cc"
    break;

  case 72: // compRange2: mathExpr ">=" "integer value"
#line 864 "whereParser.yy"
                      {
    (yylhs.value.whereNode) = ibis::qIntHod::compare(static_cast<ibis::math::term*>((yystack_[2].value.whereNode)),
                                ibis::qExpr::OP_GE, (yystack_[0].value.int64Val));
}
#line 1802 "whereParser.cc"
    break;

  case 73: // compRange2: "integer value" ">=" mathExpr
#line 868 "whereParser.yy"
                      {
    (yylhs.value.whereNode) = ibis::qIntHod::compare(static_cast<ibis::math::term*>((yystack_[0].value.whereNode)),
                                ibis::qExpr::OP_LE, (yystack_[2].value.int64Val));
}
#line 1811 "whereParser.cc"
    break;

  case 74: // compRange2: mathExpr "between" "integer value" "and" "integer value"
#line 872 "whereParser.yy"
                                       {
    (yylhs.value.whereNode) = ibis::qIntHod::between(static_cast<ibis::math::term*>((yystack_[4].value.whereNode)), (yystack_[2].value.int64Val), (yystack_[0].value.int64Val));
}
#line 1819 "whereParser.cc"
    break;

  case 75: // compRange2: mathExpr "<" "unsigned integer value"
#line 875 "whereParser.yy"
                       {
    (yylhs.value.whereNode) = ibis::qUIntHod::compare(static_cast<ibis::math::term*>((yystack_[2].value.whereNode)),
                                ibis::qExpr::OP_LT, (yystack_[0].value.uint64Val));
}
#line 1828 "whereParser.cc"
    break;

  case 76: // compRange2: "unsigned integer value" "<" mathExpr
#line 879 "whereParser.yy"
                       {
    (yylhs.value.whereNode) = ibis::qUIntHod::compare(static_cast<ibis::math::term*>((yystack_[0].value.whereNode)),
                                ibis::qExpr::OP_GT, (yystack_[2].value.uint64Val));
}
#line 1837 "whereParser.cc"
    break;

  case 77: // compRange2: mathExpr "<=" "unsigned integer value"
#line 883 "whereParser.yy"
                       {
    (yylhs.value.whereNode) = ibis::qUIntHod::compare(static_cast<ibis::math::term*>((yystack_[2].value.whereNode)),
                                ibis::qExpr::OP_LE, (yystack_[0].value.uint64Val));
}
#line 1846 "whereParser.cc"
    break;

  case 78: // compRange2: "unsigned integer value" "<=" mathExpr
#line 887 "whereParser.yy"
                       {
    (yylhs.value.whereNode) = ibis::qUIntHod::compare(static_cast<ibis::math::term*>((yystack_[0].value.whereNode)),
                                ibis::qExpr::OP_GE, (yystack_[2].value.uint64Val));
}
#line 1855 "whereParser.cc"
    break;

  case 79: // compRange2: mathExpr ">" "unsigned integer value"
#line 891 "whereParser.yy"
                       {
    (yylhs.value.whereNode) = ibis::qUIntHod::compare(static_cast<ibis::math::term*>((yystack_[2].value.whereNode)),
                                ibis::qExpr::OP_GT, (yystack_[0].value.uint64Val));
}
#line 1864 "whereParser.cc"
    break;

  case 80: // compRange2: "unsigned integer value" ">" mathExpr
#line 895 "whereParser.yy"
                       {
    (yylhs.value.whereNode) = ibis::qUIntHod::compare(static_cast<ibis::math::term*>((yystack_[0].value.whereNode)),
                                ibis::qExpr::OP_LT, (yystack_[2].value.uint64Val));
}
#line 1873 "whereParser.cc"
    break;

  case 81: // compRange2: mathExpr ">=" "unsigned integer value"
#line 899 "whereParser.yy"
                       {
    (yylhs.value.whereNode) = ibis::qUIntHod::compare(static_cast<ibis::math::term*>((yystack_[2].value.whereNode)),
                                ibis::qExpr::OP_GE, (yystack_[0].value.uint64Val));
}
#line 1882 "whereParser.cc"
    break;

  case 82: // compRange2: "unsigned integer value" ">=" mathExpr
#line 903 "whereParser.yy"
                       {
    (yylhs.value.whereNode) = ibis::qUIntHod::compare(static_cast<ibis::math::term*>((yystack_[0].value.whereNode)),
                                ibis::qExpr::OP_LE, (yystack_[2].value.uint64Val));
}
#line 1891 "whereParser.cc"
    break;

  case 83: // compRange2: mathExpr "between" "unsigned integer value" "and" "unsigned integer value"
#line 907 "whereParser.yy"
                                         {
    (yylhs.value.whereNode) = ibis::qUIntHod::between(static_cast<ibis::math::term*>((yystack_[4].value.whereNode)), (yystack_[2].value.uint64Val), (yystack_[0].value.uint64Val));
}
#line 1899 "whereParser.cc"
    break;

  case 84: // compRange2: mathExpr "==" mathExpr
#line 910 "whereParser.yy"
                         {
    ibis::math::term *me2 = static_cast<ibis::math::term*>((yystack_[0].value.whereNode));
    ibis::math::term *me1 = static_cast<ibis::math::term*>((yystack_[2].value.whereNode));
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *me1 << " = "
	<< *me2;
#endif
    (yylhs.value.whereNode) = new ibis::compRange(me1, ibis::qExpr::OP_EQ, me2);
}
#line 1914 "whereParser.cc"
    break;

  case 85: // compRange2: mathExpr "!=" mathExpr
#line 920 "whereParser.yy"
                          {
    ibis::math::term *me2 = static_cast<ibis::math::term*>((yystack_[0].value.whereNode));
    ibis::math::term *me1 = static_cast<ibis::math::term*>((yystack_[2].value.whereNode));
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *me1 << " != "
	<< *me2;
#endif
    (yylhs.value.whereNode) = new ibis::qExpr(ibis::qExpr::LOGICAL_NOT);
    (yylhs.value.whereNode)->setLeft(new ibis::compRange(me1, ibis::qExpr::OP_EQ, me2));
}
#line 1930 "whereParser.cc"
    break;

  case 86: // compRange2: mathExpr "<" mathExpr
#line 931 "whereParser.yy"
                         {
    ibis::math::term *me2 = static_cast<ibis::math::term*>((yystack_[0].value.whereNode));
    ibis::math::term *me1 = static_cast<ibis::math::term*>((yystack_[2].value.whereNode));
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *me1 << " < "
	<< *me2;
#endif
    (yylhs.value.whereNode) = new ibis::compRange(me1, ibis::qExpr::OP_LT, me2);
}
#line 1945 "whereParser.cc"
    break;

  case 87: // compRange2: mathExpr "<=" mathExpr
#line 941 "whereParser.yy"
                         {
    ibis::math::term *me2 = static_cast<ibis::math::term*>((yystack_[0].value.whereNode));
    ibis::math::term *me1 = static_cast<ibis::math::term*>((yystack_[2].value.whereNode));
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *me1 << " <= "
	<< *me2;
#endif
    (yylhs.value.whereNode) = new ibis::compRange(me1, ibis::qExpr::OP_LE, me2);
}
#line 1960 "whereParser.cc"
    break;

  case 88: // compRange2: mathExpr ">" mathExpr
#line 951 "whereParser.yy"
                         {
    ibis::math::term *me2 = static_cast<ibis::math::term*>((yystack_[0].value.whereNode));
    ibis::math::term *me1 = static_cast<ibis::math::term*>((yystack_[2].value.whereNode));
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *me1 << " > "
	<< *me2;
#endif
    (yylhs.value.whereNode) = new ibis::compRange(me1, ibis::qExpr::OP_GT, me2);
}
#line 1975 "whereParser.cc"
    break;

  case 89: // compRange2: mathExpr ">=" mathExpr
#line 961 "whereParser.yy"
                         {
    ibis::math::term *me2 = static_cast<ibis::math::term*>((yystack_[0].value.whereNode));
    ibis::math::term *me1 = static_cast<ibis::math::term*>((yystack_[2].value.whereNode));
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *me1 << " >= "
	<< *me2;
#endif
    (yylhs.value.whereNode) = new ibis::compRange(me1, ibis::qExpr::OP_GE, me2);
}
#line 1990 "whereParser.cc"
    break;

  case 90: // compRange2: mathExpr "<" "string literal"
#line 1020 "whereParser.yy"
                       {
    (yylhs.value.whereNode) = stringCompare((yystack_[2].value.whereNode), ibis::qString::STR_LT, (yystack_[0].value.stringVal), 0);
}
#line 1998 "whereParser.cc"
    break;

  case 91: // compRange2: mathExpr "<=" "string literal"
#line 1023 "whereParser.yy"
                       {
    (yylhs.value.whereNode) = stringCompare((yystack_[2].value.whereNode), ibis::qString::STR_LE, (yystack_[0].value.stringVal), 0);
}
#line 2006 "whereParser.cc"
    break;

  case 92: // compRange2: mathExpr ">" "string literal"
#line 1026 "whereParser.yy"
                       {
    (yylhs.value.whereNode) = stringCompare((yystack_[2].value.whereNode), ibis::qString::STR_GT, (yystack_[0].value.stringVal), 0);
}
#line 2014 "whereParser.cc"
    break;

  case 93: // compRange2: mathExpr ">=" "string literal"
#line 1029 "whereParser.yy"
                       {
    (yylhs.value.whereNode) = stringCompare((yystack_[2].value.whereNode), ibis::qString::STR_GE, (yystack_[0].value.stringVal), 0);
}
#line 2022 "whereParser.cc"
    break;

  case 94: // compRange2: mathExpr "between" "string literal" "and" "string literal"
#line 1032 "whereParser.yy"
                                         {
    (yylhs.value.whereNode) = stringCompare((yystack_[4].value.whereNode), ibis::qString::STR_BETWEEN, (yystack_[2].value.stringVal), (yystack_[0].value.stringVal));
}
#line 2030 "whereParser.cc"
    break;

  case 95: // compRange2: "decimal_cmp" mathExpr "==" "string literal"
#line 1035 "whereParser.yy"
                                 {
    (yylhs.value.whereNode) = stringCompare((yystack_[2].value.whereNode), ibis::qString::STR_EQ, (yystack_[0].value.stringVal), 0, true);
}
#line 2038 "whereParser.cc"
    break;

  case 96: // compRange2: "decimal_cmp" mathExpr "<" "string literal"
#line 1038 "whereParser.yy"
                                 {
    (yylhs.value.whereNode) = stringCompare((yystack_[2].value.whereNode), ibis::qString::STR_LT, (yystack_[0].value.stringVal), 0, true);
}
#line 2046 "whereParser.cc"
    break;

  case 97: // compRange2: "decimal_cmp" mathExpr "<=" "string literal"
#line 1041 "whereParser.yy"
                                 {
    (yylhs.value.whereNode) = stringCompare((yystack_[2].value.whereNode), ibis::qString::STR_LE, (yystack_[0].value.stringVal), 0, true);
}
#line 2054 "whereParser.cc"
    break;

  case 98: // compRange2: "decimal_cmp" mathExpr ">" "string literal"
#line 1044 "whereParser.yy"
                                 {
    (yylhs.value.whereNode) = stringCompare((yystack_[2].value.whereNode), ibis::qString::STR_GT, (yystack_[0].value.stringVal), 0, true);
}
#line 2062 "whereParser.cc"
    break;

  case 99: // compRange2: "decimal_cmp" mathExpr ">=" "string literal"
#line 1047 "whereParser.yy"
                                 {
    (yylhs.value.whereNode) = stringCompare((yystack_[2].value.whereNode), ibis::qString::STR_GE, (yystack_[0].value.stringVal), 0, true);
}
#line 2070 "whereParser.cc"
    break;

  case 100: // compRange2: "decimal_cmp" mathExpr "between" "string literal" "and" "string literal"
#line 1050 "whereParser.yy"
                                                   {
    (yylhs.value.whereNode) = stringCompare((yystack_[4].value.whereNode), ibis::qString::STR_BETWEEN, (yystack_[2].value.stringVal), (yystack_[0].value.stringVal), true);
}
#line 2078 "whereParser.cc"
    break;

  case 101: // compRange2: "collate_N" mathExpr "==" "string literal"
#line 1053 "whereParser.yy"
                                 {
    (yylhs.value.whereNode) = stringCompare((yystack_[2].value.whereNode), ibis::qString::STR_EQ, (yystack_[0].value.stringVal), 0, false,
                       collationNumber((yystack_[3].value.integerVal)));
}
#line 2087 "whereParser.cc"
    break;

  case 102: // compRange2: "collate_N" mathExpr "<" "string literal"
#line 1057 "whereParser.yy"
                                 {
    (yylhs.value.whereNode) = stringCompare((yystack_[2].value.whereNode), ibis::qString::STR_LT, (yystack_[0].value.stringVal), 0, false,
                       collationNumber((yystack_[3].value.integerVal)));
}
#line 2096 "whereParser.cc"
    break;

  case 103: // compRange2: "collate_N" mathExpr "<=" "string literal"
#line 1061 "whereParser.yy"
                                 {
    (yylhs.value.whereNode) = stringCompare((yystack_[2].value.whereNode), ibis::qString::STR_LE, (yystack_[0].value.stringVal), 0, false,
                       collationNumber((yystack_[3].value.integerVal)));
}
#line 2105 "whereParser.cc"
    break;

  case 104: // compRange2: "collate_N" mathExpr ">" "string literal"
#line 1065 "whereParser.yy"
                                 {
    (yylhs.value.whereNode) = stringCompare((yystack_[2].value.whereNode), ibis::qString::STR_GT, (yystack_[0].value.stringVal), 0, false,
                       collationNumber((yystack_[3].value.integerVal)));
}
#line 2114 "whereParser.cc"
    break;

  case 105: // compRange2: "collate_N" mathExpr ">=" "string literal"
#line 1069 "whereParser.yy"
                                 {
    (yylhs.value.whereNode) = stringCompare((yystack_[2].value.whereNode), ibis::qString::STR_GE, (yystack_[0].value.stringVal), 0, false,
                       collationNumber((yystack_[3].value.integerVal)));
}
#line 2123 "whereParser.cc"
    break;

  case 106: // compRange2: "collate_N" mathExpr "between" "string literal" "and" "string literal"
#line 1073 "whereParser.yy"
                                                   {
    (yylhs.value.whereNode) = stringCompare((yystack_[4].value.whereNode), ibis::qString::STR_BETWEEN, (yystack_[2].value.stringVal), (yystack_[0].value.stringVal), false,
                       collationNumber((yystack_[5].value.integerVal)));
}
#line 2132 "whereParser.cc"
    break;

  case 107: // compRange3: mathExpr "<" mathExpr "<" mathExpr
#line 1080 "whereParser.yy"
                                     {
    ibis::math::term *me3 = static_cast<ibis::math::term*>((yystack_[0].value.whereNode));
    ibis::math::term *me2 = static_cast<ibis::math::term*>((yystack_[2].value.whereNode));
    ibis::math::term *me1 = static_cast<ibis::math::term*>((yystack_[4].value.whereNode));
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *me1 << " < "
	<< *me2 << " < " << *me3;
#endif
    (yylhs.value.whereNode) = new ibis::compRange(me1, ibis::qExpr::OP_LT, me2,
			     ibis::qExpr::OP_LT, me3);
}
#line 2149 "whereParser.cc"
    break;

  case 108: // compRange3: mathExpr "<" mathExpr "<=" mathExpr
#line 1092 "whereParser.yy"
                                       {
    ibis::math::term *me3 = static_cast<ibis::math::term*>((yystack_[0].value.whereNode));
    ibis::math::term *me2 = static_cast<ibis::math::term*>((yystack_[2].value.whereNode));
    ibis::math::term *me1 = static_cast<ibis::math::term*>((yystack_[4].value.whereNode));
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *me1 << " < "
	<< *me2 << " <= " << *me3;
#endif
    (yylhs.value.whereNode) = new ibis::compRange(me1, ibis::qExpr::OP_LT, me2,
			     ibis::qExpr::OP_LE, me3);
}
#line 2166 "whereParser.cc"
    break;

  case 109: // compRange3: mathExpr "<=" mathExpr "<" mathExpr
#line 1104 "whereParser.yy"
                                       {
    ibis::math::term *me3 = static_cast<ibis::math::term*>((yystack_[0].value.whereNode));
    ibis::math::term *me2 = static_cast<ibis::math::term*>((yystack_[2].value.whereNode));
    ibis::math::term *me1 = static_cast<ibis::math::term*>((yystack_[4].value.whereNode));
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *me1 << " <= "
	<< *me2 << " < " << *me3;
#endif
    (yylhs.value.whereNode) = new ibis::compRange(me1, ibis::qExpr::OP_LE, me2,
			     ibis::qExpr::OP_LT, me3);
}
#line 2183 "whereParser.cc"
    break;

  case 110: // compRange3: mathExpr "<=" mathExpr "<=" mathExpr
#line 1116 "whereParser.yy"
                                       {
    ibis::math::term *me3 = static_cast<ibis::math::term*>((yystack_[0].value.whereNode));
    ibis::math::term *me2 = static_cast<ibis::math::term*>((yystack_[2].value.whereNode));
    ibis::math::term *me1 = static_cast<ibis::math::term*>((yystack_[4].value.whereNode));
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *me1 << " <= "
	<< *me2 << " <= " << *me3;
#endif
    (yylhs.value.whereNode) = new ibis::compRange(me1, ibis::qExpr::OP_LE, me2,
			     ibis::qExpr::OP_LE, me3);
}
#line 2200 "whereParser.cc"
    break;

  case 111: // compRange3: mathExpr ">" mathExpr ">" mathExpr
#line 1128 "whereParser.yy"
                                       {
    ibis::math::term *me3 = static_cast<ibis::math::term*>((yystack_[0].value.whereNode));
    ibis::math::term *me2 = static_cast<ibis::math::term*>((yystack_[2].value.whereNode));
    ibis::math::term *me1 = static_cast<ibis::math::term*>((yystack_[4].value.whereNode));
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *me1 << " > "
	<< *me2 << " > " << *me3;
#endif
    (yylhs.value.whereNode) = new ibis::compRange(me3, ibis::qExpr::OP_LT, me2,
			     ibis::qExpr::OP_LT, me1);
}
#line 2217 "whereParser.cc"
    break;

  case 112: // compRange3: mathExpr ">" mathExpr ">=" mathExpr
#line 1140 "whereParser.yy"
                                       {
    ibis::math::term *me3 = static_cast<ibis::math::term*>((yystack_[0].value.whereNode));
    ibis::math::term *me2 = static_cast<ibis::math::term*>((yystack_[2].value.whereNode));
    ibis::math::term *me1 = static_cast<ibis::math::term*>((yystack_[4].value.whereNode));
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *me1 << " > "
	<< *me2 << " >= " << *me3;
#endif
    (yylhs.value.whereNode) = new ibis::compRange(me3, ibis::qExpr::OP_LE, me2,
			     ibis::qExpr::OP_LT, me1);
}
#line 2234 "whereParser.cc"
    break;

  case 113: // compRange3: mathExpr ">=" mathExpr ">" mathExpr
#line 1152 "whereParser.yy"
                                       {
    ibis::math::term *me3 = static_cast<ibis::math::term*>((yystack_[0].value.whereNode));
    ibis::math::term *me2 = static_cast<ibis::math::term*>((yystack_[2].value.whereNode));
    ibis::math::term *me1 = static_cast<ibis::math::term*>((yystack_[4].value.whereNode));
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *me1 << " >= "
	<< *me2 << " > " << *me3;
#endif
    (yylhs.value.whereNode) = new ibis::compRange(me3, ibis::qExpr::OP_LT, me2,
			     ibis::qExpr::OP_LE, me1);
}
#line 2251 "whereParser.cc"
    break;

  case 114: // compRange3: mathExpr ">=" mathExpr ">=" mathExpr
#line 1164 "whereParser.yy"
                                       {
    ibis::math::term *me3 = static_cast<ibis::math::term*>((yystack_[0].value.whereNode));
    ibis::math::term *me2 = static_cast<ibis::math::term*>((yystack_[2].value.whereNode));
    ibis::math::term *me1 = static_cast<ibis::math::term*>((yystack_[4].value.whereNode));
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *me1 << " >= "
	<< *me2 << " >= " << *me3;
#endif
    (yylhs.value.whereNode) = new ibis::compRange(me3, ibis::qExpr::OP_LE, me2,
			     ibis::qExpr::OP_LE, me1);
}
#line 2268 "whereParser.cc"
    break;

  case 115: // compRange3: mathExpr "between" mathExpr "and" mathExpr
#line 1176 "whereParser.yy"
                                             {
    ibis::math::term *me3 = static_cast<ibis::math::term*>((yystack_[0].value.whereNode));
    ibis::math::term *me2 = static_cast<ibis::math::term*>((yystack_[2].value.whereNode));
    ibis::math::term *me1 = static_cast<ibis::math::term*>((yystack_[4].value.whereNode));
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *me1 << " BETWEEN "
	<< *me2 << " AND " << *me3;
#endif
    (yylhs.value.whereNode) = new ibis::compRange(me2, ibis::qExpr::OP_LE, me1,
			     ibis::qExpr::OP_LE, me3);
}
#line 2285 "whereParser.cc"
    break;

  case 116: // mathExpr: mathExpr "+" mathExpr
#line 1191 "whereParser.yy"
                        {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.whereNode)
	<< " + " << *(yystack_[0].value.whereNode);
#endif
    ibis::math::bediener *opr =
	new ibis::math::bediener(ibis::math::PLUS);
    opr->setRight((yystack_[0].value.whereNode));
    opr->setLeft((yystack_[2].value.whereNode));
    (yylhs.value.whereNode) = static_cast<ibis::qExpr*>(opr);
}
#line 2302 "whereParser.cc"
    break;

  case 117: // mathExpr: mathExpr "-" mathExpr
#line 1203 "whereParser.yy"
                            {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.whereNode)
	<< " - " << *(yystack_[0].value.whereNode);
#endif
    ibis::math::bediener *opr =
	new ibis::math::bediener(ibis::math::MINUS);
    opr->setRight((yystack_[0].value.whereNode));
    opr->setLeft((yystack_[2].value.whereNode));
    (yylhs.value.whereNode) = static_cast<ibis::qExpr*>(opr);
}
#line 2319 "whereParser.cc"
    break;

  case 118: // mathExpr: mathExpr "*" mathExpr
#line 1215 "whereParser.yy"
                           {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.whereNode)
	<< " * " << *(yystack_[0].value.whereNode);
#endif
    ibis::math::bediener *opr =
	new ibis::math::bediener(ibis::math::MULTIPLY);
    opr->setRight((yystack_[0].value.whereNode));
    opr->setLeft((yystack_[2].value.whereNode));
    (yylhs.value.whereNode) = static_cast<ibis::qExpr*>(opr);
}
#line 2336 "whereParser.cc"
    break;

  case 119: // mathExpr: mathExpr "/" mathExpr
#line 1227 "whereParser.yy"
                          {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.whereNode)
	<< " / " << *(yystack_[0].value.whereNode);
#endif
    ibis::math::bediener *opr =
	new ibis::math::bediener(ibis::math::DIVIDE);
    opr->setRight((yystack_[0].value.whereNode));
    opr->setLeft((yystack_[2].value.whereNode));
    (yylhs.value.whereNode) = static_cast<ibis::qExpr*>(opr);
}
#line 2353 "whereParser.cc"
    break;

  case 120: // mathExpr: mathExpr "%" mathExpr
#line 1239 "whereParser.yy"
                          {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.whereNode)
	<< " % " << *(yystack_[0].value.whereNode);
#endif
    ibis::math::bediener *opr =
	new ibis::math::bediener(ibis::math::REMAINDER);
    opr->setRight((yystack_[0].value.whereNode));
    opr->setLeft((yystack_[2].value.whereNode));
    (yylhs.value.whereNode) = static_cast<ibis::qExpr*>(opr);
}
#line 2370 "whereParser.cc"
    break;

  case 121: // mathExpr: mathExpr "**" mathExpr
#line 1251 "whereParser.yy"
                          {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.whereNode)
	<< " ^ " << *(yystack_[0].value.whereNode);
#endif
    ibis::math::bediener *opr =
	new ibis::math::bediener(ibis::math::POWER);
    opr->setRight((yystack_[0].value.whereNode));
    opr->setLeft((yystack_[2].value.whereNode));
    (yylhs.value.whereNode) = static_cast<ibis::qExpr*>(opr);
}
#line 2387 "whereParser.cc"
    break;

  case 122: // mathExpr: mathExpr "&" mathExpr
#line 1263 "whereParser.yy"
                             {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.whereNode)
	<< " & " << *(yystack_[0].value.whereNode);
#endif
    ibis::math::bediener *opr =
	new ibis::math::bediener(ibis::math::BITAND);
    opr->setRight((yystack_[0].value.whereNode));
    opr->setLeft((yystack_[2].value.whereNode));
    (yylhs.value.whereNode) = static_cast<ibis::qExpr*>(opr);
}
#line 2404 "whereParser.cc"
    break;

  case 123: // mathExpr: mathExpr "|" mathExpr
#line 1275 "whereParser.yy"
                            {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.whereNode)
	<< " | " << *(yystack_[0].value.whereNode);
#endif
    ibis::math::bediener *opr =
	new ibis::math::bediener(ibis::math::BITOR);
    opr->setRight((yystack_[0].value.whereNode));
    opr->setLeft((yystack_[2].value.whereNode));
    (yylhs.value.whereNode) = static_cast<ibis::qExpr*>(opr);
}
#line 2421 "whereParser.cc"
    break;

  case 124: // mathExpr: "name string" '(' mathExpr ')'
#line 1287 "whereParser.yy"
                                      {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[3].value.stringVal) << "("
	<< *(yystack_[1].value.whereNode) << ")";
#endif
    ibis::math::stdFunction1 *fun =
	new ibis::math::stdFunction1((yystack_[3].value.stringVal)->c_str());
    delete (yystack_[3].value.stringVal);
    fun->setLeft((yystack_[1].value.whereNode));
    (yylhs.value.whereNode) = static_cast<ibis::qExpr*>(fun);
}
#line 2438 "whereParser.cc"
    break;

  case 125: // mathExpr: "name string" '(' mathExpr ',' mathExpr ')'
#line 1299 "whereParser.yy"
                                                   {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[5].value.stringVal) << "("
	<< *(yystack_[3].value.whereNode) << ", " << *(yystack_[1].value.whereNode) << ")";
#endif
    ibis::math::stdFunction2 *fun =
	new ibis::math::stdFunction2((yystack_[5].value.stringVal)->c_str());
    fun->setRight((yystack_[1].value.whereNode));
    fun->setLeft((yystack_[3].value.whereNode));
    (yylhs.value.whereNode) = static_cast<ibis::qExpr*>(fun);
    delete (yystack_[5].value.stringVal);
}
#line 2456 "whereParser.cc"
    break;

  case 126: // mathExpr: "FROM_UNIXTIME_LOCAL" '(' mathExpr ',' "string literal" ')'
#line 1312 "whereParser.yy"
                                                  {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- FROM_UNIXTIME_LOCAL("
	<< *(yystack_[3].value.whereNode) << ", " << *(yystack_[1].value.stringVal) << ")";
#endif
    ibis::math::fromUnixTime fut((yystack_[1].value.stringVal)->c_str());
    ibis::math::customFunction1 *fun =
	new ibis::math::customFunction1(fut);
    fun->setLeft((yystack_[3].value.whereNode));
    (yylhs.value.whereNode) = static_cast<ibis::qExpr*>(fun);
    delete (yystack_[1].value.stringVal);
}
#line 2474 "whereParser.cc"
    break;

  case 127: // mathExpr: "FROM_UNIXTIME_GMT" '(' mathExpr ',' "string literal" ')'
#line 1325 "whereParser.yy"
                                                {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- FROM_UNIXTIME_GMT("
	<< *(yystack_[3].value.whereNode) << ", " << *(yystack_[1].value.stringVal) << ")";
#endif

    ibis::math::fromUnixTime fut((yystack_[1].value.stringVal)->c_str(), "GMT");
    ibis::math::customFunction1 *fun =
	new ibis::math::customFunction1(fut);
    fun->setLeft((yystack_[3].value.whereNode));
    (yylhs.value.whereNode) = static_cast<ibis::qExpr*>(fun);
    delete (yystack_[1].value.stringVal);
}
#line 2493 "whereParser.cc"
    break;

  case 128: // mathExpr: "ISO_TO_UNIXTIME_LOCAL" '(' mathExpr ')'
#line 1339 "whereParser.yy"
                                         {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- ISO_TO_UNIXTIME_LOCAL("
	<< *(yystack_[1].value.whereNode) << ")";
#endif

    ibis::math::toUnixTime fut;
    ibis::math::customFunction1 *fun =
	new ibis::math::customFunction1(fut);
    fun->setLeft((yystack_[1].value.whereNode));
    (yylhs.value.whereNode) = static_cast<ibis::qExpr*>(fun);
}
#line 2511 "whereParser.cc"
    break;

  case 129: // mathExpr: "ISO_TO_UNIXTIME_GMT" '(' mathExpr ')'
#line 1352 "whereParser.yy"
                                       {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- ISO_TO_UNIXTIME_GMT("
	<< *(yystack_[1].value.whereNode) << ")";
#endif

    ibis::math::toUnixTime fut("GMT0");
    ibis::math::customFunction1 *fun =
	new ibis::math::customFunction1(fut);
    fun->setLeft((yystack_[1].value.whereNode));
    (yylhs.value.whereNode) = static_cast<ibis::qExpr*>(fun);
}
#line 2529 "whereParser.cc"
    break;

  case 130: // mathExpr: "TO_UNIXTIME_LOCAL" '(' "string literal" ',' "string literal" ')'
#line 1365 "whereParser.yy"
                                              {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- TO_UNIXTIME_LOCAL("
	<< *(yystack_[3].value.stringVal) << ", " << *(yystack_[1].value.stringVal)  << ")";
#endif
#if defined(HAVE_STRPTIME)
    struct tm mytm;
    memset(&mytm, 0, sizeof(mytm));
    const char *ret = strptime((yystack_[3].value.stringVal)->c_str(), (yystack_[1].value.stringVal)->c_str(), &mytm);
    if (ret != 0) {
        // A negative value for tm_isdst causes mktime() to attempt to
        // determine whether Daylight Saving Time is in effect for the
        // specified time.
        mytm.tm_isdst = -1;
        if (mytm.tm_mday == 0) {
            // This can happen if we are using a format without '%d'
            // e.g. "%Y%m" as tm_mday is day of the month (in the range 1
            // through 31).
            mytm.tm_mday = 1;
        }
        (yylhs.value.whereNode) = new ibis::math::number(mktime(&mytm));
    }
    delete (yystack_[3].value.stringVal);
    delete (yystack_[1].value.stringVal);

    if (ret == 0) {
        LOGGER(ibis::gVerbose >= 0)
            << "Warning -- " << __FILE__ << ':' << __LINE__
            << " failed to parse \"" << *(yystack_[3].value.stringVal) << "\" using format string \""
            << *(yystack_[1].value.stringVal) << "\", errno = " << errno;
        throw "Failed to parse string value in TO_UNIXTIME_LOCAL";
    }
#else
    LOGGER(ibis::gVerbose >= 0)
        << "Warning -- " << __FILE__ << ':' << __LINE__
        << " failed to parse \"" << *(yystack_[3].value.stringVal) << "\" using format string \""
        << *(yystack_[1].value.stringVal) << "\" because there is no strptime";
    throw "No strptime to parse string value in TO_UNIXTIME_LOCAL";
#endif
}
#line 2575 "whereParser.cc"
    break;

  case 131: // mathExpr: "TO_UNIXTIME_GMT" '(' "string literal" ',' "string literal" ')'
#line 1406 "whereParser.yy"
                                            {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- TO_UNIXTIME_GMT("
	<< *(yystack_[3].value.stringVal) << ", " << *(yystack_[1].value.stringVal)  << ")";
#endif
#if defined(HAVE_STRPTIME)
    struct tm mytm;
    memset(&mytm, 0, sizeof(mytm));
    const char *ret = strptime((yystack_[3].value.stringVal)->c_str(), (yystack_[1].value.stringVal)->c_str(), &mytm);
    if (ret != 0) {
        if (mytm.tm_mday == 0) {
            // This can happen if we are using a format without '%d'
            // e.g. "%Y%m" as tm_mday is day of the month (in the range 1
            // through 31).
            mytm.tm_mday = 1;
        }
        (yylhs.value.whereNode) = new ibis::math::number(timegm(&mytm));
    }
    delete (yystack_[3].value.stringVal);
    delete (yystack_[1].value.stringVal);

    if (ret == 0) {
        LOGGER(ibis::gVerbose >= 0)
            << "Warning -- " << __FILE__ << ':' << __LINE__
            << " failed to parse \"" << *(yystack_[3].value.stringVal) << "\" using format string \""
            << *(yystack_[1].value.stringVal) << "\", errno = " << errno;
        throw "Failed to parse string value in TO_UNIXTIME_GMT";
    }
#else
    LOGGER(ibis::gVerbose >= 0)
        << "Warning -- " << __FILE__ << ':' << __LINE__
        << " failed to parse \"" << *(yystack_[3].value.stringVal) << "\" using format string \""
        << *(yystack_[1].value.stringVal) << "\" because there is no strptime";
    throw "No strptime to parse string value in TO_UNIXTIME_GMT";
#endif
}
#line 2617 "whereParser.cc"
    break;

  case 132: // mathExpr: "-" mathExpr
#line 1443 "whereParser.yy"
                               {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- - " << *(yystack_[0].value.whereNode);
#endif
    ibis::math::bediener *opr =
	new ibis::math::bediener(ibis::math::NEGATE);
    opr->setRight((yystack_[0].value.whereNode));
    (yylhs.value.whereNode) = static_cast<ibis::qExpr*>(opr);
}
#line 2632 "whereParser.cc"
    break;

  case 133: // mathExpr: "+" mathExpr
#line 1453 "whereParser.yy"
                             {
    (yylhs.value.whereNode) = (yystack_[0].value.whereNode);
}
#line 2640 "whereParser.cc"
    break;

  case 134: // mathExpr: '(' mathExpr ')'
#line 1456 "whereParser.yy"
                   {
    (yylhs.value.whereNode) = (yystack_[1].value.whereNode);
}
#line 2648 "whereParser.cc"
    break;

  case 135: // mathExpr: "name string"
#line 1459 "whereParser.yy"
          {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " got a variable name " << *(yystack_[0].value.stringVal);
#endif
    ibis::math::variable *var =
	new ibis::math::variable((yystack_[0].value.stringVal)->c_str());
    (yylhs.value.whereNode) = static_cast<ibis::qExpr*>(var);
    delete (yystack_[0].value.stringVal);
}
#line 2663 "whereParser.cc"
    break;

  case 136: // mathExpr: "floating-point number"
#line 1469 "whereParser.yy"
         {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " got a number " << (yystack_[0].value.doubleVal);
#endif
    ibis::math::number *num = new ibis::math::number((yystack_[0].value.doubleVal));
    (yylhs.value.whereNode) = static_cast<ibis::qExpr*>(num);
}
#line 2676 "whereParser.cc"
    break;

  case 137: // START: qexpr "end of input"
#line 1479 "whereParser.yy"
                  { /* pass qexpr to the driver */
    driver.expr_ = (yystack_[1].value.whereNode);
}
#line 2684 "whereParser.cc"
    break;

  case 138: // START: qexpr ';'
#line 1482 "whereParser.yy"
            { /* pass qexpr to the driver */
    driver.expr_ = (yystack_[1].value.whereNode);
}
#line 2692 "whereParser.cc"
    break;


#line 2696 "whereParser.cc"

            default:
              break;
            }
        }
#if YY_EXCEPTIONS
      catch (const syntax_error& yyexc)
        {
          YYCDEBUG << "Caught exception: " << yyexc.what() << '\n';
          error (yyexc);
          YYERROR;
        }
#endif // YY_EXCEPTIONS
      YY_SYMBOL_PRINT ("-> $$ =", yylhs);
      yypop_ (yylen);
      yylen = 0;

      // Shift the result of the reduction.
      yypush_ (YY_NULLPTR, YY_MOVE (yylhs));
    }
    goto yynewstate;


  /*--------------------------------------.
  | yyerrlab -- here on detecting error.  |
  `--------------------------------------*/
  yyerrlab:
    // If not already recovering from an error, report this error.
    if (!yyerrstatus_)
      {
        ++yynerrs_;
        context yyctx (*this, yyla);
        std::string msg = yysyntax_error_ (yyctx);
        error (yyla.location, YY_MOVE (msg));
      }


    yyerror_range[1].location = yyla.location;
    if (yyerrstatus_ == 3)
      {
        /* If just tried and failed to reuse lookahead token after an
           error, discard it.  */

        // Return failure if at end of input.
        if (yyla.kind () == symbol_kind::S_YYEOF)
          YYABORT;
        else if (!yyla.empty ())
          {
            yy_destroy_ ("Error: discarding", yyla);
            yyla.clear ();
          }
      }

    // Else will try to reuse lookahead token after shifting the error token.
    goto yyerrlab1;


  /*---------------------------------------------------.
  | yyerrorlab -- error raised explicitly by YYERROR.  |
  `---------------------------------------------------*/
  yyerrorlab:
    /* Pacify compilers when the user code never invokes YYERROR and
       the label yyerrorlab therefore never appears in user code.  */
    if (false)
      YYERROR;

    /* Do not reclaim the symbols of the rule whose action triggered
       this YYERROR.  */
    yypop_ (yylen);
    yylen = 0;
    YY_STACK_PRINT ();
    goto yyerrlab1;


  /*-------------------------------------------------------------.
  | yyerrlab1 -- common code for both syntax error and YYERROR.  |
  `-------------------------------------------------------------*/
  yyerrlab1:
    yyerrstatus_ = 3;   // Each real token shifted decrements this.
    // Pop stack until we find a state that shifts the error token.
    for (;;)
      {
        yyn = yypact_[+yystack_[0].state];
        if (!yy_pact_value_is_default_ (yyn))
          {
            yyn += symbol_kind::S_YYerror;
            if (0 <= yyn && yyn <= yylast_
                && yycheck_[yyn] == symbol_kind::S_YYerror)
              {
                yyn = yytable_[yyn];
                if (0 < yyn)
                  break;
              }
          }

        // Pop the current state because it cannot handle the error token.
        if (yystack_.size () == 1)
          YYABORT;

        yyerror_range[1].location = yystack_[0].location;
        yy_destroy_ ("Error: popping", yystack_[0]);
        yypop_ ();
        YY_STACK_PRINT ();
      }
    {
      stack_symbol_type error_token;

      yyerror_range[2].location = yyla.location;
      YYLLOC_DEFAULT (error_token.location, yyerror_range, 2);

      // Shift the error token.
      error_token.state = state_type (yyn);
      yypush_ ("Shifting", YY_MOVE (error_token));
    }
    goto yynewstate;


  /*-------------------------------------.
  | yyacceptlab -- YYACCEPT comes here.  |
  `-------------------------------------*/
  yyacceptlab:
    yyresult = 0;
    goto yyreturn;


  /*-----------------------------------.
  | yyabortlab -- YYABORT comes here.  |
  `-----------------------------------*/
  yyabortlab:
    yyresult = 1;
    goto yyreturn;


  /*-----------------------------------------------------.
  | yyreturn -- parsing is finished, return the result.  |
  `-----------------------------------------------------*/
  yyreturn:
    if (!yyla.empty ())
      yy_destroy_ ("Cleanup: discarding lookahead", yyla);

    /* Do not reclaim the symbols of the rule whose action triggered
       this YYABORT or YYACCEPT.  */
    yypop_ (yylen);
    YY_STACK_PRINT ();
    while (1 < yystack_.size ())
      {
        yy_destroy_ ("Cleanup: popping", yystack_[0]);
        yypop_ ();
      }

    return yyresult;
  }
#if YY_EXCEPTIONS
    catch (...)
      {
        YYCDEBUG << "Exception caught: cleaning lookahead and stack\n";
        // Do not try to display the values of the reclaimed symbols,
        // as their printers might throw an exception.
        if (!yyla.empty ())
          yy_destroy_ (YY_NULLPTR, yyla);

        while (1 < yystack_.size ())
          {
            yy_destroy_ (YY_NULLPTR, yystack_[0]);
            yypop_ ();
          }
        throw;
      }
#endif // YY_EXCEPTIONS
  }

  void
  whereParser::error (const syntax_error& yyexc)
  {
    error (yyexc.location, yyexc.what ());
  }

  /* Return YYSTR after stripping away unnecessary quotes and
     backslashes, so that it's suitable for yyerror.  The heuristic is
     that double-quoting is unnecessary unless the string contains an
     apostrophe, a comma, or backslash (other than backslash-backslash).
     YYSTR is taken from yytname.  */
  std::string
  whereParser::yytnamerr_ (const char *yystr)
  {
    if (*yystr == '"')
      {
        std::string yyr;
        char const *yyp = yystr;

        for (;;)
          switch (*++yyp)
            {
            case '\'':
            case ',':
              goto do_not_strip_quotes;

            case '\\':
              if (*++yyp != '\\')
                goto do_not_strip_quotes;
              else
                goto append;

            append:
            default:
              yyr += *yyp;
              break;

            case '"':
              return yyr;
            }
      do_not_strip_quotes: ;
      }

    return yystr;
  }

  std::string
  whereParser::symbol_name (symbol_kind_type yysymbol)
  {
    return yytnamerr_ (yytname_[yysymbol]);
  }



  // whereParser::context.
  whereParser::context::context (const whereParser& yyparser, const symbol_type& yyla)
    : yyparser_ (yyparser)
    , yyla_ (yyla)
  {}

  int
  whereParser::context::expected_tokens (symbol_kind_type yyarg[], int yyargn) const
  {
    // Actual number of expected tokens
    int yycount = 0;

    const int yyn = yypact_[+yyparser_.yystack_[0].state];
    if (!yy_pact_value_is_default_ (yyn))
      {
        /* Start YYX at -YYN if negative to avoid negative indexes in
           YYCHECK.  In other words, skip the first -YYN actions for
           this state because they are default actions.  */
        const int yyxbegin = yyn < 0 ? -yyn : 0;
        // Stay within bounds of both yycheck and yytname.
        const int yychecklim = yylast_ - yyn + 1;
        const int yyxend = yychecklim < YYNTOKENS ? yychecklim : YYNTOKENS;
        for (int yyx = yyxbegin; yyx < yyxend; ++yyx)
          if (yycheck_[yyx + yyn] == yyx && yyx != symbol_kind::S_YYerror
              && !yy_table_value_is_error_ (yytable_[yyx + yyn]))
            {
              if (!yyarg)
                ++yycount;
              else if (yycount == yyargn)
                return 0;
              else
                yyarg[yycount++] = YY_CAST (symbol_kind_type, yyx);
            }
      }

    if (yyarg && yycount == 0 && 0 < yyargn)
      yyarg[0] = symbol_kind::S_YYEMPTY;
    return yycount;
  }






  int
  whereParser::yy_syntax_error_arguments_ (const context& yyctx,
                                                 symbol_kind_type yyarg[], int yyargn) const
  {
    /* There are many possibilities here to consider:
       - If this state is a consistent state with a default action, then
         the only way this function was invoked is if the default action
         is an error action.  In that case, don't check for expected
         tokens because there are none.
       - The only way there can be no lookahead present (in yyla) is
         if this state is a consistent state with a default action.
         Thus, detecting the absence of a lookahead is sufficient to
         determine that there is no unexpected or expected token to
         report.  In that case, just report a simple "syntax error".
       - Don't assume there isn't a lookahead just because this state is
         a consistent state with a default action.  There might have
         been a previous inconsistent state, consistent state with a
         non-default action, or user semantic action that manipulated
         yyla.  (However, yyla is currently not documented for users.)
       - Of course, the expected token list depends on states to have
         correct lookahead information, and it depends on the parser not
         to perform extra reductions after fetching a lookahead from the
         scanner and before detecting a syntax error.  Thus, state merging
         (from LALR or IELR) and default reductions corrupt the expected
         token list.  However, the list is correct for canonical LR with
         one exception: it will still contain any token that will not be
         accepted due to an error action in a later state.
    */

    if (!yyctx.lookahead ().empty ())
      {
        if (yyarg)
          yyarg[0] = yyctx.token ();
        int yyn = yyctx.expected_tokens (yyarg ? yyarg + 1 : yyarg, yyargn - 1);
        return yyn + 1;
      }
    return 0;
  }

  // Generate an error message.
  std::string
  whereParser::yysyntax_error_ (const context& yyctx) const
  {
    // Its maximum.
    enum { YYARGS_MAX = 5 };
    // Arguments of yyformat.
    symbol_kind_type yyarg[YYARGS_MAX];
    int yycount = yy_syntax_error_arguments_ (yyctx, yyarg, YYARGS_MAX);

    char const* yyformat = YY_NULLPTR;
    switch (yycount)
      {
#define YYCASE_(N, S)                         \
        case N:                               \
          yyformat = S;                       \
        break
      default: // Avoid compiler warnings.
        YYCASE_ (0, YY_("syntax error"));
        YYCASE_ (1, YY_("syntax error, unexpected %s"));
        YYCASE_ (2, YY_("syntax error, unexpected %s, expecting %s"));
        YYCASE_ (3, YY_("syntax error, unexpected %s, expecting %s or %s"));
        YYCASE_ (4, YY_("syntax error, unexpected %s, expecting %s or %s or %s"));
        YYCASE_ (5, YY_("syntax error, unexpected %s, expecting %s or %s or %s or %s"));
#undef YYCASE_
      }

    std::string yyres;
    // Argument number.
    std::ptrdiff_t yyi = 0;
    for (char const* yyp = yyformat; *yyp; ++yyp)
      if (yyp[0] == '%' && yyp[1] == 's' && yyi < yycount)
        {
          yyres += symbol_name (yyarg[yyi++]);
          ++yyp;
        }
      else
        yyres += *yyp;
    return yyres;
  }


  const signed char whereParser::yypact_ninf_ = -46;

  const signed char whereParser::yytable_ninf_ = -1;

  const short
  whereParser::yypact_[] =
  {
      78,    78,   461,   487,   134,   -45,   -15,   -11,    -6,     8,
      18,    36,   461,   461,   127,   185,   -46,     3,    44,    78,
      16,   -46,   -46,   -46,   132,   107,   -46,    97,   461,   165,
     -12,   216,   -46,   -46,   -27,   461,   461,   104,   114,   461,
     461,   135,   -46,   -46,   461,   461,   461,   461,   461,   461,
     461,   461,    11,   279,   305,   101,   193,   -20,   461,   177,
     186,    -8,   121,   -46,    78,    78,    78,    78,   -46,   331,
     357,   383,   409,   461,   461,   435,   461,   461,   461,   461,
     461,   461,   461,   461,   -46,   244,   190,   199,   208,   226,
     238,   246,    75,   270,   280,   296,   301,   304,   306,   313,
     318,   522,   543,   189,   237,   551,   571,   319,    28,    28,
      28,    28,    28,    28,    28,    28,   -46,   241,   -46,   -46,
     -46,    28,   -46,   -46,   -46,    28,   -46,   -46,   -46,   133,
     -46,   -46,   -46,   -46,    69,   -46,   -46,   514,   -46,   -46,
     -46,   -46,   -46,   -46,   215,    87,   -46,   -46,   -46,   153,
     -46,   -46,   -46,   233,   -46,   -46,   -46,   225,   -46,   -46,
     -46,   264,    28,    28,   360,   361,   363,   506,    58,   171,
     462,   462,   253,   253,   253,   253,   -46,   -46,   -46,   -46,
     -46,   364,   -46,   332,   -46,   -46,   -46,   -46,   -46,   374,
     -46,   -46,   341,   342,   345,   346,   -46,   -46,     0,   -46,
     -46,   -46,   -46,    79,    64,   169,   194,   259,   265,   -46,
     461,   461,   461,   461,   461,   461,   461,   461,   461,   355,
     359,   348,   461,   353,   285,   356,   352,   365,   368,   369,
     372,   375,   291,   311,   316,   -46,   245,   -46,   267,   -46,
     373,   -46,   277,   -46,   278,   579,    28,    28,    28,    28,
      28,    28,    28,    28,   -46,   -46,   -46,    28,   -46,   -46,
     358,   -46,   -46,   -46,   -46,   -46,   -46,   -46,   -46,   380,
     -46,   293,   -46,   303,   376,   378,   379,   381,   389,   390,
     391,   394,   395,   -46,   396,   397,   401,   402,   404,   405,
     -46,   -46,   -46,   -46,   -46,   -46,   -46,   -46,   -46,   -46,
     -46,   -46,   -46,   -46,   -46
  };

  const unsigned char
  whereParser::yydefact_[] =
  {
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,   136,   135,     0,     0,
       0,     8,     9,    10,     0,     0,     6,   135,     0,     0,
     135,     0,    11,    12,     0,     0,     0,     0,     0,     0,
       0,     0,   133,   132,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,   137,     0,     0,     0,     0,   138,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     1,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,    69,    73,
      67,    71,    78,    82,    76,    80,    18,     0,    56,    58,
      62,    64,    57,    59,    63,    65,    45,    53,    46,     0,
      41,    43,    15,    22,     0,    32,    33,     0,    60,    61,
       7,   134,     4,     5,     2,     3,    68,    77,    91,    87,
      72,    81,    93,    89,    66,    75,    90,    86,    70,    79,
      92,    88,    84,    85,     0,     0,     0,     0,   123,   122,
     116,   117,   118,   119,   120,   121,    97,    99,    96,    98,
      95,     0,    29,     0,   103,   105,   102,   104,   101,     0,
      13,    14,     0,     0,     0,     0,   129,   128,     0,    42,
      44,    19,    34,     0,     0,     0,     0,     0,     0,   124,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,    47,     0,    48,     0,    17,
       0,    27,     0,    28,     0,     0,   110,   109,   114,   113,
     108,   107,   112,   111,    74,    83,    94,   115,   100,    31,
       0,   106,   127,   126,   131,   130,    54,    55,    21,     0,
      39,     0,    40,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,   125,     0,     0,     0,     0,     0,     0,
      52,    51,    50,    49,    16,    23,    25,    24,    26,    30,
      20,    35,    37,    36,    38
  };

  const signed char
  whereParser::yypgoto_[] =
  {
     -46,    20,   -46,   -46,   -46,    -2,   -46
  };

  const signed char
  whereParser::yydefgoto_[] =
  {
       0,    20,    21,    22,    23,    24,    25
  };

  const short
  whereParser::yytable_[] =
  {
      29,    31,    35,    64,    65,    66,    67,    52,    92,   230,
      42,    43,    53,    54,   116,    99,    63,    62,   100,    55,
     231,    26,   135,    56,    57,   136,    85,    64,    65,    66,
      67,   117,    36,   101,   102,    58,    37,   105,   106,    61,
     140,    38,   108,   109,   110,   111,   112,   113,   114,   115,
      58,   121,   125,    59,    60,    39,   137,    76,    77,    78,
      79,    80,    81,    82,    83,    40,    68,   149,   153,   157,
     161,   162,   163,   167,   168,   169,   170,   171,   172,   173,
     174,   175,     1,    41,   142,   143,   144,   145,    77,    78,
      79,    80,    81,    82,    83,     2,     3,     4,    64,    65,
       5,     6,     7,     8,     9,    10,    11,    84,   206,    12,
      13,   207,   235,   236,   208,    14,    15,    16,   232,   182,
      17,   233,   183,    18,   234,    19,    69,    70,    71,    72,
      73,    74,    44,    45,    46,    47,    75,    69,    70,    71,
      72,    73,    74,   126,    58,   127,   128,    75,   129,   103,
      76,    77,    78,    79,    80,    81,    82,    83,   211,   104,
     212,    76,    77,    78,    79,    80,    81,    82,    83,   141,
      86,    87,    88,    89,    90,   204,    32,   107,   205,    33,
      91,    34,    76,    77,    78,    79,    80,    81,    82,    83,
      48,    49,    50,    51,    76,    77,    78,    79,    80,    81,
      82,    83,    78,    79,    80,    81,    82,    83,   245,   246,
     247,   248,   249,   250,   251,   252,   253,   237,   238,   138,
     257,    93,    94,    95,    96,    97,    64,    65,   139,    67,
     215,    98,   216,   130,   131,   176,   132,   133,   194,   213,
     134,   214,   239,   240,   177,    76,    77,    78,    79,    80,
      81,    82,    83,   178,    76,    77,    78,    79,    80,    81,
      82,    83,    76,    77,    78,    79,    80,    81,    82,    83,
     217,   179,   218,    76,    77,    78,    79,    80,    81,    82,
      83,   199,   200,   180,   201,   202,   195,   274,   203,    83,
     275,   181,   141,    76,    77,    78,    79,    80,    81,    82,
      83,     5,     6,     7,     8,     9,    10,   241,   242,   276,
      12,    13,   277,   243,   244,   184,   118,   119,    16,   279,
     281,    27,   280,   282,   120,   185,    28,     5,     6,     7,
       8,     9,    10,   259,   260,   286,    12,    13,   287,   268,
     269,   186,   122,   123,    16,   288,   187,    27,   289,   188,
     124,   189,    28,     5,     6,     7,     8,     9,    10,   270,
     271,   190,    12,    13,   272,   273,   191,   198,   146,   147,
      16,   219,   220,    27,   221,   223,   148,   224,    28,     5,
       6,     7,     8,     9,    10,   225,   226,   227,    12,    13,
     228,   229,   254,   256,   150,   151,    16,   255,   258,    27,
     262,   261,   152,   284,    28,     5,     6,     7,     8,     9,
      10,   266,   278,   263,    12,    13,   264,   265,   267,   285,
     154,   155,    16,     0,   290,    27,   291,   292,   156,   293,
      28,     5,     6,     7,     8,     9,    10,   294,   295,   296,
      12,    13,   297,   298,   299,   300,   158,   159,    16,   301,
     302,    27,   303,   304,   160,     0,    28,     5,     6,     7,
       8,     9,    10,     0,     0,     0,    12,    13,     0,     0,
       0,     0,   164,   165,    16,     0,     0,    27,     0,     0,
     166,     0,    28,     5,     6,     7,     8,     9,    10,     0,
       0,     0,    12,    13,     0,    80,    81,    82,    83,     0,
      16,     0,     0,    27,     0,     0,     0,     0,    28,     5,
       6,     7,     8,     9,    10,     0,     0,   222,    12,    13,
       0,     0,     0,     0,     0,     0,    16,     0,     0,    30,
       0,     0,     0,     0,    28,    76,    77,    78,    79,    80,
      81,    82,    83,    76,    77,    78,    79,    80,    81,    82,
      83,    76,    77,    78,    79,    80,    81,    82,    83,     0,
       0,     0,   209,   210,     0,     0,     0,     0,     0,     0,
       0,   192,    76,    77,    78,    79,    80,    81,    82,    83,
      76,    77,    78,    79,    80,    81,    82,    83,     0,     0,
       0,     0,   193,     0,     0,     0,     0,     0,     0,   196,
      76,    77,    78,    79,    80,    81,    82,    83,    76,    77,
      78,    79,    80,    81,    82,    83,     0,     0,     0,   197,
       0,     0,     0,     0,     0,     0,     0,   283
  };

  const short
  whereParser::yycheck_[] =
  {
       2,     3,    47,    11,    12,    13,    14,     4,    20,     9,
      12,    13,     9,    10,     3,    42,     0,    19,    45,    16,
      20,     1,    42,    20,    21,    45,    28,    11,    12,    13,
      14,    20,    47,    35,    36,    47,    47,    39,    40,    19,
      48,    47,    44,    45,    46,    47,    48,    49,    50,    51,
      47,    53,    54,     9,    10,    47,    58,    29,    30,    31,
      32,    33,    34,    35,    36,    47,    50,    69,    70,    71,
      72,    73,    74,    75,    76,    77,    78,    79,    80,    81,
      82,    83,     4,    47,    64,    65,    66,    67,    30,    31,
      32,    33,    34,    35,    36,    17,    18,    19,    11,    12,
      22,    23,    24,    25,    26,    27,    28,     0,    39,    31,
      32,    42,    48,    49,    45,    37,    38,    39,    39,    44,
      42,    42,    47,    45,    45,    47,     5,     6,     7,     8,
       9,    10,     5,     6,     7,     8,    15,     5,     6,     7,
       8,     9,    10,    42,    47,    44,    45,    15,    47,    45,
      29,    30,    31,    32,    33,    34,    35,    36,     5,    45,
       7,    29,    30,    31,    32,    33,    34,    35,    36,    48,
       5,     6,     7,     8,     9,    42,    42,    42,    45,    45,
      15,    47,    29,    30,    31,    32,    33,    34,    35,    36,
       5,     6,     7,     8,    29,    30,    31,    32,    33,    34,
      35,    36,    31,    32,    33,    34,    35,    36,   210,   211,
     212,   213,   214,   215,   216,   217,   218,    48,    49,    42,
     222,     5,     6,     7,     8,     9,    11,    12,    42,    14,
       5,    15,     7,    40,    41,    45,    43,    44,    49,     6,
      47,     8,    48,    49,    45,    29,    30,    31,    32,    33,
      34,    35,    36,    45,    29,    30,    31,    32,    33,    34,
      35,    36,    29,    30,    31,    32,    33,    34,    35,    36,
       6,    45,     8,    29,    30,    31,    32,    33,    34,    35,
      36,    40,    41,    45,    43,    44,    49,    42,    47,    36,
      45,    45,    48,    29,    30,    31,    32,    33,    34,    35,
      36,    22,    23,    24,    25,    26,    27,    48,    49,    42,
      31,    32,    45,    48,    49,    45,    37,    38,    39,    42,
      42,    42,    45,    45,    45,    45,    47,    22,    23,    24,
      25,    26,    27,    48,    49,    42,    31,    32,    45,    48,
      49,    45,    37,    38,    39,    42,    45,    42,    45,    45,
      45,    45,    47,    22,    23,    24,    25,    26,    27,    48,
      49,    48,    31,    32,    48,    49,    48,    48,    37,    38,
      39,    11,    11,    42,    11,    11,    45,    45,    47,    22,
      23,    24,    25,    26,    27,    11,    45,    45,    31,    32,
      45,    45,    37,    45,    37,    38,    39,    38,    45,    42,
      48,    45,    45,    45,    47,    22,    23,    24,    25,    26,
      27,    39,    39,    48,    31,    32,    48,    48,    43,    39,
      37,    38,    39,    -1,    48,    42,    48,    48,    45,    48,
      47,    22,    23,    24,    25,    26,    27,    48,    48,    48,
      31,    32,    48,    48,    48,    48,    37,    38,    39,    48,
      48,    42,    48,    48,    45,    -1,    47,    22,    23,    24,
      25,    26,    27,    -1,    -1,    -1,    31,    32,    -1,    -1,
      -1,    -1,    37,    38,    39,    -1,    -1,    42,    -1,    -1,
      45,    -1,    47,    22,    23,    24,    25,    26,    27,    -1,
      -1,    -1,    31,    32,    -1,    33,    34,    35,    36,    -1,
      39,    -1,    -1,    42,    -1,    -1,    -1,    -1,    47,    22,
      23,    24,    25,    26,    27,    -1,    -1,    11,    31,    32,
      -1,    -1,    -1,    -1,    -1,    -1,    39,    -1,    -1,    42,
      -1,    -1,    -1,    -1,    47,    29,    30,    31,    32,    33,
      34,    35,    36,    29,    30,    31,    32,    33,    34,    35,
      36,    29,    30,    31,    32,    33,    34,    35,    36,    -1,
      -1,    -1,    48,    49,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    49,    29,    30,    31,    32,    33,    34,    35,    36,
      29,    30,    31,    32,    33,    34,    35,    36,    -1,    -1,
      -1,    -1,    49,    -1,    -1,    -1,    -1,    -1,    -1,    48,
      29,    30,    31,    32,    33,    34,    35,    36,    29,    30,
      31,    32,    33,    34,    35,    36,    -1,    -1,    -1,    48,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    48
  };

  const signed char
  whereParser::yystos_[] =
  {
       0,     4,    17,    18,    19,    22,    23,    24,    25,    26,
      27,    28,    31,    32,    37,    38,    39,    42,    45,    47,
      52,    53,    54,    55,    56,    57,    52,    42,    47,    56,
      42,    56,    42,    45,    47,    47,    47,    47,    47,    47,
      47,    47,    56,    56,     5,     6,     7,     8,     5,     6,
       7,     8,     4,     9,    10,    16,    20,    21,    47,     9,
      10,    52,    56,     0,    11,    12,    13,    14,    50,     5,
       6,     7,     8,     9,    10,    15,    29,    30,    31,    32,
      33,    34,    35,    36,     0,    56,     5,     6,     7,     8,
       9,    15,    20,     5,     6,     7,     8,     9,    15,    42,
      45,    56,    56,    45,    45,    56,    56,    42,    56,    56,
      56,    56,    56,    56,    56,    56,     3,    20,    37,    38,
      45,    56,    37,    38,    45,    56,    42,    44,    45,    47,
      40,    41,    43,    44,    47,    42,    45,    56,    42,    42,
      48,    48,    52,    52,    52,    52,    37,    38,    45,    56,
      37,    38,    45,    56,    37,    38,    45,    56,    37,    38,
      45,    56,    56,    56,    37,    38,    45,    56,    56,    56,
      56,    56,    56,    56,    56,    56,    45,    45,    45,    45,
      45,    45,    44,    47,    45,    45,    45,    45,    45,    45,
      48,    48,    49,    49,    49,    49,    48,    48,    48,    40,
      41,    43,    44,    47,    42,    45,    39,    42,    45,    48,
      49,     5,     7,     6,     8,     5,     7,     6,     8,    11,
      11,    11,    11,    11,    45,    11,    45,    45,    45,    45,
       9,    20,    39,    42,    45,    48,    49,    48,    49,    48,
      49,    48,    49,    48,    49,    56,    56,    56,    56,    56,
      56,    56,    56,    56,    37,    38,    45,    56,    45,    48,
      49,    45,    48,    48,    48,    48,    39,    43,    48,    49,
      48,    49,    48,    49,    42,    45,    42,    45,    39,    42,
      45,    42,    45,    48,    45,    39,    42,    45,    42,    45,
      48,    48,    48,    48,    48,    48,    48,    48,    48,    48,
      48,    48,    48,    48,    48
  };

  const signed char
  whereParser::yyr1_[] =
  {
       0,    51,    52,    52,    52,    52,    52,    52,    52,    52,
      52,    53,    53,    53,    53,    53,    53,    53,    53,    53,
      53,    53,    53,    53,    53,    53,    53,    53,    53,    53,
      53,    53,    53,    53,    53,    53,    53,    53,    53,    53,
      53,    53,    53,    53,    53,    53,    53,    53,    53,    53,
      53,    53,    53,    53,    53,    53,    53,    53,    53,    53,
      53,    53,    53,    53,    53,    53,    54,    54,    54,    54,
      54,    54,    54,    54,    54,    54,    54,    54,    54,    54,
      54,    54,    54,    54,    54,    54,    54,    54,    54,    54,
      54,    54,    54,    54,    54,    54,    54,    54,    54,    54,
      54,    54,    54,    54,    54,    54,    54,    55,    55,    55,
      55,    55,    55,    55,    55,    55,    56,    56,    56,    56,
      56,    56,    56,    56,    56,    56,    56,    56,    56,    56,
      56,    56,    56,    56,    56,    56,    56,    57,    57
  };

  const signed char
  whereParser::yyr2_[] =
  {
       0,     2,     3,     3,     3,     3,     2,     3,     1,     1,
       1,     2,     2,     4,     4,     3,     7,     5,     3,     4,
       8,     6,     3,     7,     7,     7,     7,     5,     5,     4,
       8,     6,     3,     3,     4,     8,     8,     8,     8,     6,
       6,     3,     4,     3,     4,     3,     3,     5,     5,     7,
       7,     7,     7,     3,     6,     6,     3,     3,     3,     3,
       3,     3,     3,     3,     3,     3,     3,     3,     3,     3,
       3,     3,     3,     3,     5,     3,     3,     3,     3,     3,
       3,     3,     3,     5,     3,     3,     3,     3,     3,     3,
       3,     3,     3,     3,     5,     4,     4,     4,     4,     4,
       6,     4,     4,     4,     4,     4,     6,     5,     5,     5,
       5,     5,     5,     5,     5,     5,     3,     3,     3,     3,
       3,     3,     3,     3,     4,     6,     6,     6,     4,     4,
       6,     6,     2,     2,     3,     1,     1,     2,     2
  };


#if YYDEBUG || 1
  // YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
  // First, the terminals, then, starting at \a YYNTOKENS, nonterminals.
  const char*
  const whereParser::yytname_[] =
  {
  "\"end of input\"", "error", "\"invalid token\"", "\"null\"", "\"not\"",
  "\"<=\"", "\">=\"", "\"<\"", "\">\"", "\"==\"", "\"!=\"", "\"and\"",
  "\"&!\"", "\"or\"", "\"xor\"", "\"between\"", "\"contains\"",
  "\"decimal_cmp\"", "\"collate_N\"", "\"exists\"", "\"in\"", "\"like\"",
  "\"FROM_UNIXTIME_GMT\"", "\"FROM_UNIXTIME_LOCAL\"",
  "\"TO_UNIXTIME_GMT\"", "\"TO_UNIXTIME_LOCAL\"",
  "\"ISO_TO_UNIXTIME_GMT\"", "\"ISO_TO_UNIXTIME_LOCAL\"", "\"any\"",
  "\"|\"", "\"&\"", "\"+\"", "\"-\"", "\"*\"", "\"/\"", "\"%\"", "\"**\"",
  "\"integer value\"", "\"unsigned integer value\"",
  "\"floating-point number\"", "\"signed integer sequence\"",
  "\"unsigned integer sequence\"", "\"name string\"",
  "\"number sequence\"", "\"string sequence\"", "\"string literal\"",
  "CONSTAINSOP", "'('", "')'", "','", "';'", "$accept", "qexpr",
  "simpleRange", "compRange2", "compRange3", "mathExpr", "START", YY_NULLPTR
  };
#endif


#if YYDEBUG
  const short
  whereParser::yyrline_[] =
  {
       0,   185,   185,   195,   205,   215,   225,   233,   236,   237,
     238,   242,   250,   258,   266,   274,   284,   296,   305,   312,
     323,   336,   346,   356,   369,   382,   395,   408,   420,   432,
     438,   447,   455,   465,   475,   486,   500,   514,   528,   542,
     555,   568,   578,   589,   599,   610,   620,   630,   640,   650,
     661,   672,   683,   694,   704,   713,   723,   731,   740,   748,
     757,   767,   778,   788,   799,   816,   839,   844,   848,   852,
     856,   860,   864,   868,   872,   875,   879,   883,   887,   891,
     895,   899,   903,   907,   910,   920,   931,   941,   951,   961,
    1020,  1023,  1026,  1029,  1032,  1035,  1038,  1041,  1044,  1047,
    1050,  1053,  1057,  1061,  1065,  1069,  1073,  1080,  1092,  1104,
    1116,  1128,  1140,  1152,  1164,  1176,  1191,  1203,  1215,  1227,
    1239,  1251,  1263,  1275,  1287,  1299,  1312,  1325,  1339,  1352,
    1365,  1406,  1443,  1453,  1456,  1459,  1469,  1479,  1482
  };

  void
  whereParser::yy_stack_print_ () const
  {
    *yycdebug_ << "Stack now";
    for (stack_type::const_iterator
           i = yystack_.begin (),
           i_end = yystack_.end ();
         i != i_end; ++i)
      *yycdebug_ << ' ' << int (i->state);
    *yycdebug_ << '\n';
  }

  void
  whereParser::yy_reduce_print_ (int yyrule) const
  {
    int yylno = yyrline_[yyrule];
    int yynrhs = yyr2_[yyrule];
    // Print the symbols being reduced, and their result.
    *yycdebug_ << "Reducing stack by rule " << yyrule - 1
               << " (line " << yylno << "):\n";
    // The symbols being reduced.
    for (int yyi = 0; yyi < yynrhs; yyi++)
      YY_SYMBOL_PRINT ("   $" << yyi + 1 << " =",
                       yystack_[(yynrhs) - (yyi + 1)]);
  }
#endif // YYDEBUG

  whereParser::symbol_kind_type
  whereParser::yytranslate_ (int t) YY_NOEXCEPT
  {
    // YYTRANSLATE[TOKEN-NUM] -- Symbol number corresponding to
    // TOKEN-NUM as returned by yylex.
    static
    const signed char
    translate_table[] =
    {
       0,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
      47,    48,     2,     2,    49,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,    50,
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
      35,    36,    37,    38,    39,    40,    41,    42,    43,    44,
      45,    46
    };
    // Last valid token kind.
    const int code_max = 301;

    if (t <= 0)
      return symbol_kind::S_YYEOF;
    else if (t <= code_max)
      return static_cast <symbol_kind_type> (translate_table[t]);
    else
      return symbol_kind::S_YYUNDEF;
  }

#line 25 "whereParser.yy"
} // ibis
#line 3472 "whereParser.cc"

#line 1487 "whereParser.yy"

void ibis::whereParser::error(const ibis::whereParser::location_type& l,
			      const std::string& m) {
    LOGGER(ibis::gVerbose >= 0)
	<< "Warning -- ibis::whereParser encountered " << m
	<< " at location " << l;
}
