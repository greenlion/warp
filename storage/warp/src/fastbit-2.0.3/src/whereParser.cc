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
#line 106 "whereParser.yy"

#include "whereLexer.h"

#undef yylex
#define yylex driver.lexer->lex

/* Builds the comparison of a column with string literals, "name op 'lo'" or
   "name between 'lo' and 'hi'".  The expression on the left has to be the
   name of a column.  It and the strings are deleted. */
static ibis::qExpr* stringCompare(ibis::qExpr *left,
                                  ibis::qString::COMPARE op,
                                  std::string *lo, std::string *hi) {
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
    ibis::qExpr *ret = (hi != 0 ?
        new ibis::qString(var->variableName(), lo->c_str(), hi->c_str()) :
        new ibis::qString(var->variableName(), op, lo->c_str()));
    delete hi;
    delete lo;
    delete var;
    return ret;
}

#line 87 "whereParser.cc"



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
#line 181 "whereParser.cc"

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
#line 103 "whereParser.yy"
                    { delete (yysym.value.stringVal); }
#line 393 "whereParser.cc"
        break;

      case symbol_kind::S_UINTSEQ: // "unsigned integer sequence"
#line 103 "whereParser.yy"
                    { delete (yysym.value.stringVal); }
#line 399 "whereParser.cc"
        break;

      case symbol_kind::S_NOUNSTR: // "name string"
#line 103 "whereParser.yy"
                    { delete (yysym.value.stringVal); }
#line 405 "whereParser.cc"
        break;

      case symbol_kind::S_NUMSEQ: // "number sequence"
#line 103 "whereParser.yy"
                    { delete (yysym.value.stringVal); }
#line 411 "whereParser.cc"
        break;

      case symbol_kind::S_STRSEQ: // "string sequence"
#line 103 "whereParser.yy"
                    { delete (yysym.value.stringVal); }
#line 417 "whereParser.cc"
        break;

      case symbol_kind::S_STRLIT: // "string literal"
#line 103 "whereParser.yy"
                    { delete (yysym.value.stringVal); }
#line 423 "whereParser.cc"
        break;

      case symbol_kind::S_qexpr: // qexpr
#line 104 "whereParser.yy"
                    { delete (yysym.value.whereNode); }
#line 429 "whereParser.cc"
        break;

      case symbol_kind::S_simpleRange: // simpleRange
#line 104 "whereParser.yy"
                    { delete (yysym.value.whereNode); }
#line 435 "whereParser.cc"
        break;

      case symbol_kind::S_compRange2: // compRange2
#line 104 "whereParser.yy"
                    { delete (yysym.value.whereNode); }
#line 441 "whereParser.cc"
        break;

      case symbol_kind::S_compRange3: // compRange3
#line 104 "whereParser.yy"
                    { delete (yysym.value.whereNode); }
#line 447 "whereParser.cc"
        break;

      case symbol_kind::S_mathExpr: // mathExpr
#line 104 "whereParser.yy"
                    { delete (yysym.value.whereNode); }
#line 453 "whereParser.cc"
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

#line 595 "whereParser.cc"


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
#line 140 "whereParser.yy"
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
#line 742 "whereParser.cc"
    break;

  case 3: // qexpr: qexpr "xor" qexpr
#line 150 "whereParser.yy"
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
#line 757 "whereParser.cc"
    break;

  case 4: // qexpr: qexpr "and" qexpr
#line 160 "whereParser.yy"
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
#line 772 "whereParser.cc"
    break;

  case 5: // qexpr: qexpr "&!" qexpr
#line 170 "whereParser.yy"
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
#line 787 "whereParser.cc"
    break;

  case 6: // qexpr: "not" qexpr
#line 180 "whereParser.yy"
              {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- ! " << *(yystack_[0].value.whereNode);
#endif
    (yylhs.value.whereNode) = new ibis::qExpr(ibis::qExpr::LOGICAL_NOT);
    (yylhs.value.whereNode)->setLeft((yystack_[0].value.whereNode));
}
#line 800 "whereParser.cc"
    break;

  case 7: // qexpr: '(' qexpr ')'
#line 188 "whereParser.yy"
                            {
    (yylhs.value.whereNode) = (yystack_[1].value.whereNode);
}
#line 808 "whereParser.cc"
    break;

  case 8: // qexpr: simpleRange
#line 191 "whereParser.yy"
  { (yylhs.value.whereNode) = (yystack_[0].value.whereNode); }
#line 814 "whereParser.cc"
    break;

  case 9: // qexpr: compRange2
#line 192 "whereParser.yy"
  { (yylhs.value.whereNode) = (yystack_[0].value.whereNode); }
#line 820 "whereParser.cc"
    break;

  case 10: // qexpr: compRange3
#line 193 "whereParser.yy"
  { (yylhs.value.whereNode) = (yystack_[0].value.whereNode); }
#line 826 "whereParser.cc"
    break;

  case 11: // simpleRange: "exists" "name string"
#line 197 "whereParser.yy"
                 {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- EXISTS(" << *(yystack_[0].value.stringVal) << ')';
#endif
    (yylhs.value.whereNode) = new ibis::qExists((yystack_[0].value.stringVal)->c_str());
    delete (yystack_[0].value.stringVal);
}
#line 839 "whereParser.cc"
    break;

  case 12: // simpleRange: "exists" "string literal"
#line 205 "whereParser.yy"
                  {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- EXISTS(" << *(yystack_[0].value.stringVal) << ')';
#endif
    (yylhs.value.whereNode) = new ibis::qExists((yystack_[0].value.stringVal)->c_str());
    delete (yystack_[0].value.stringVal);
}
#line 852 "whereParser.cc"
    break;

  case 13: // simpleRange: "exists" '(' "name string" ')'
#line 213 "whereParser.yy"
                           {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- EXISTS(" << *(yystack_[1].value.stringVal) << ')';
#endif
    (yylhs.value.whereNode) = new ibis::qExists((yystack_[1].value.stringVal)->c_str());
    delete (yystack_[1].value.stringVal);
}
#line 865 "whereParser.cc"
    break;

  case 14: // simpleRange: "exists" '(' "string literal" ')'
#line 221 "whereParser.yy"
                          {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- EXISTS(" << *(yystack_[1].value.stringVal) << ')';
#endif
    (yylhs.value.whereNode) = new ibis::qExists((yystack_[1].value.stringVal)->c_str());
    delete (yystack_[1].value.stringVal);
}
#line 878 "whereParser.cc"
    break;

  case 15: // simpleRange: "name string" "in" "number sequence"
#line 229 "whereParser.yy"
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
#line 893 "whereParser.cc"
    break;

  case 16: // simpleRange: "name string" "in" '(' "floating-point number" ',' "floating-point number" ')'
#line 239 "whereParser.yy"
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
#line 910 "whereParser.cc"
    break;

  case 17: // simpleRange: "name string" "in" '(' "floating-point number" ')'
#line 251 "whereParser.yy"
                              {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[4].value.stringVal) << " IN ("
	<< (yystack_[1].value.doubleVal) << ")";
#endif
    (yylhs.value.whereNode) = new ibis::qContinuousRange((yystack_[4].value.stringVal)->c_str(), ibis::qExpr::OP_EQ, (yystack_[1].value.doubleVal));
    delete (yystack_[4].value.stringVal);
}
#line 924 "whereParser.cc"
    break;

  case 18: // simpleRange: "name string" "not" "null"
#line 260 "whereParser.yy"
                       {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.stringVal) << " NOT NULL";
#endif
    (yylhs.value.whereNode) = new ibis::qContinuousRange((yystack_[2].value.stringVal)->c_str(), ibis::qExpr::OP_UNDEFINED, 0U);
}
#line 936 "whereParser.cc"
    break;

  case 19: // simpleRange: "name string" "not" "in" "number sequence"
#line 267 "whereParser.yy"
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
#line 952 "whereParser.cc"
    break;

  case 20: // simpleRange: "name string" "not" "in" '(' "floating-point number" ',' "floating-point number" ')'
#line 278 "whereParser.yy"
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
#line 970 "whereParser.cc"
    break;

  case 21: // simpleRange: "name string" "not" "in" '(' "floating-point number" ')'
#line 291 "whereParser.yy"
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
#line 985 "whereParser.cc"
    break;

  case 22: // simpleRange: "name string" "in" "string sequence"
#line 301 "whereParser.yy"
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
#line 1000 "whereParser.cc"
    break;

  case 23: // simpleRange: "name string" "in" '(' "name string" ',' "name string" ')'
#line 311 "whereParser.yy"
                                           {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[6].value.stringVal) << " IN ("
	<< *(yystack_[3].value.stringVal) << ", " << *(yystack_[1].value.stringVal) << ")";
#endif
    std::string val;
    val = '"'; /* add quote to keep strings intact */
    val += *(yystack_[3].value.stringVal);
    val += "\", \"";
    val += *(yystack_[1].value.stringVal);
    val += '"';
    (yylhs.value.whereNode) = new ibis::qAnyString((yystack_[6].value.stringVal)->c_str(), val.c_str());
    delete (yystack_[1].value.stringVal);
    delete (yystack_[3].value.stringVal);
    delete (yystack_[6].value.stringVal);
}
#line 1022 "whereParser.cc"
    break;

  case 24: // simpleRange: "name string" "in" '(' "string literal" ',' "name string" ')'
#line 328 "whereParser.yy"
                                          {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[6].value.stringVal) << " IN ("
	<< *(yystack_[3].value.stringVal) << ", " << *(yystack_[1].value.stringVal) << ")";
#endif
    std::string val;
    val = '"'; /* add quote to keep strings intact */
    val += *(yystack_[3].value.stringVal);
    val += "\", \"";
    val += *(yystack_[1].value.stringVal);
    val += '"';
    (yylhs.value.whereNode) = new ibis::qAnyString((yystack_[6].value.stringVal)->c_str(), val.c_str());
    delete (yystack_[1].value.stringVal);
    delete (yystack_[3].value.stringVal);
    delete (yystack_[6].value.stringVal);
}
#line 1044 "whereParser.cc"
    break;

  case 25: // simpleRange: "name string" "in" '(' "name string" ',' "string literal" ')'
#line 345 "whereParser.yy"
                                          {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[6].value.stringVal) << " IN ("
	<< *(yystack_[3].value.stringVal) << ", " << *(yystack_[1].value.stringVal) << ")";
#endif
    std::string val;
    val = '"'; /* add quote to keep strings intact */
    val += *(yystack_[3].value.stringVal);
    val += "\", \"";
    val += *(yystack_[1].value.stringVal);
    val += '"';
    (yylhs.value.whereNode) = new ibis::qAnyString((yystack_[6].value.stringVal)->c_str(), val.c_str());
    delete (yystack_[1].value.stringVal);
    delete (yystack_[3].value.stringVal);
    delete (yystack_[6].value.stringVal);
}
#line 1066 "whereParser.cc"
    break;

  case 26: // simpleRange: "name string" "in" '(' "string literal" ',' "string literal" ')'
#line 362 "whereParser.yy"
                                         {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[6].value.stringVal) << " IN ("
	<< *(yystack_[3].value.stringVal) << ", " << *(yystack_[1].value.stringVal) << ")";
#endif
    std::string val;
    val = '"'; /* add quote to keep strings intact */
    val += *(yystack_[3].value.stringVal);
    val += "\", \"";
    val += *(yystack_[1].value.stringVal);
    val += '"';
    (yylhs.value.whereNode) = new ibis::qAnyString((yystack_[6].value.stringVal)->c_str(), val.c_str());
    delete (yystack_[1].value.stringVal);
    delete (yystack_[3].value.stringVal);
    delete (yystack_[6].value.stringVal);
}
#line 1088 "whereParser.cc"
    break;

  case 27: // simpleRange: "name string" "in" '(' "name string" ')'
#line 379 "whereParser.yy"
                               {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[4].value.stringVal) << " IN ("
	<< *(yystack_[1].value.stringVal) << ")";
#endif
    std::string val;
    val = '"'; /* add quote to keep strings intact */
    val += *(yystack_[1].value.stringVal);
    val += '"';
    (yylhs.value.whereNode) = new ibis::qAnyString((yystack_[4].value.stringVal)->c_str(), val.c_str());
    delete (yystack_[1].value.stringVal);
    delete (yystack_[4].value.stringVal);
}
#line 1107 "whereParser.cc"
    break;

  case 28: // simpleRange: "name string" "in" '(' "string literal" ')'
#line 393 "whereParser.yy"
                              {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[4].value.stringVal) << " IN ("
	<< *(yystack_[1].value.stringVal) << ")";
#endif
    std::string val;
    val = '"'; /* add quote to keep strings intact */
    val += *(yystack_[1].value.stringVal);
    val += '"';
    (yylhs.value.whereNode) = new ibis::qAnyString((yystack_[4].value.stringVal)->c_str(), val.c_str());
    delete (yystack_[1].value.stringVal);
    delete (yystack_[4].value.stringVal);
}
#line 1126 "whereParser.cc"
    break;

  case 29: // simpleRange: "name string" "like" "name string"
#line 407 "whereParser.yy"
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
#line 1141 "whereParser.cc"
    break;

  case 30: // simpleRange: "name string" "like" "string literal"
#line 417 "whereParser.yy"
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
#line 1156 "whereParser.cc"
    break;

  case 31: // simpleRange: "name string" "not" "in" "string sequence"
#line 427 "whereParser.yy"
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
#line 1172 "whereParser.cc"
    break;

  case 32: // simpleRange: "name string" "not" "in" '(' "name string" ',' "name string" ')'
#line 438 "whereParser.yy"
                                                 {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[7].value.stringVal) << " NOT IN ("
	<< *(yystack_[3].value.stringVal) << ", " << *(yystack_[1].value.stringVal) << ")";
#endif
    std::string val;
    val = '"'; /* add quote to keep strings intact */
    val += *(yystack_[3].value.stringVal);
    val += "\", \"";
    val += *(yystack_[1].value.stringVal);
    val += '"';
    (yylhs.value.whereNode) = new ibis::qExpr(ibis::qExpr::LOGICAL_NOT);
    (yylhs.value.whereNode)->setLeft(new ibis::qAnyString((yystack_[7].value.stringVal)->c_str(), val.c_str()));
    delete (yystack_[1].value.stringVal);
    delete (yystack_[3].value.stringVal);
    delete (yystack_[7].value.stringVal);
}
#line 1195 "whereParser.cc"
    break;

  case 33: // simpleRange: "name string" "not" "in" '(' "string literal" ',' "name string" ')'
#line 456 "whereParser.yy"
                                                {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[7].value.stringVal) << " NOT IN ("
	<< *(yystack_[3].value.stringVal) << ", " << *(yystack_[1].value.stringVal) << ")";
#endif
    std::string val;
    val = '"'; /* add quote to keep strings intact */
    val += *(yystack_[3].value.stringVal);
    val += "\", \"";
    val += *(yystack_[1].value.stringVal);
    val += '"';
    (yylhs.value.whereNode) = new ibis::qExpr(ibis::qExpr::LOGICAL_NOT);
    (yylhs.value.whereNode)->setLeft(new ibis::qAnyString((yystack_[7].value.stringVal)->c_str(), val.c_str()));
    delete (yystack_[1].value.stringVal);
    delete (yystack_[3].value.stringVal);
    delete (yystack_[7].value.stringVal);
}
#line 1218 "whereParser.cc"
    break;

  case 34: // simpleRange: "name string" "not" "in" '(' "name string" ',' "string literal" ')'
#line 474 "whereParser.yy"
                                                {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[7].value.stringVal) << " NOT IN ("
	<< *(yystack_[3].value.stringVal) << ", " << *(yystack_[1].value.stringVal) << ")";
#endif
    std::string val;
    val = '"'; /* add quote to keep strings intact */
    val += *(yystack_[3].value.stringVal);
    val += "\", \"";
    val += *(yystack_[1].value.stringVal);
    val += '"';
    (yylhs.value.whereNode) = new ibis::qExpr(ibis::qExpr::LOGICAL_NOT);
    (yylhs.value.whereNode)->setLeft(new ibis::qAnyString((yystack_[7].value.stringVal)->c_str(), val.c_str()));
    delete (yystack_[1].value.stringVal);
    delete (yystack_[3].value.stringVal);
    delete (yystack_[7].value.stringVal);
}
#line 1241 "whereParser.cc"
    break;

  case 35: // simpleRange: "name string" "not" "in" '(' "string literal" ',' "string literal" ')'
#line 492 "whereParser.yy"
                                               {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[7].value.stringVal) << " NOT IN ("
	<< *(yystack_[3].value.stringVal) << ", " << *(yystack_[1].value.stringVal) << ")";
#endif
    std::string val;
    val = '"'; /* add quote to keep strings intact */
    val += *(yystack_[3].value.stringVal);
    val += "\", \"";
    val += *(yystack_[1].value.stringVal);
    val += '"';
    (yylhs.value.whereNode) = new ibis::qExpr(ibis::qExpr::LOGICAL_NOT);
    (yylhs.value.whereNode)->setLeft(new ibis::qAnyString((yystack_[7].value.stringVal)->c_str(), val.c_str()));
    delete (yystack_[1].value.stringVal);
    delete (yystack_[3].value.stringVal);
    delete (yystack_[7].value.stringVal);
}
#line 1264 "whereParser.cc"
    break;

  case 36: // simpleRange: "name string" "not" "in" '(' "name string" ')'
#line 510 "whereParser.yy"
                                     {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[5].value.stringVal) << " NOT IN ("
	<< *(yystack_[1].value.stringVal) << ")";
#endif
    std::string val;
    val = '"'; /* add quote to keep strings intact */
    val += *(yystack_[1].value.stringVal);
    val += '"';
    (yylhs.value.whereNode) = new ibis::qExpr(ibis::qExpr::LOGICAL_NOT);
    (yylhs.value.whereNode)->setLeft(new ibis::qAnyString((yystack_[5].value.stringVal)->c_str(), val.c_str()));
    delete (yystack_[1].value.stringVal);
    delete (yystack_[5].value.stringVal);
}
#line 1284 "whereParser.cc"
    break;

  case 37: // simpleRange: "name string" "not" "in" '(' "string literal" ')'
#line 525 "whereParser.yy"
                                    {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[5].value.stringVal) << " NOT IN ("
	<< *(yystack_[1].value.stringVal) << ")";
#endif
    std::string val;
    val = '"'; /* add quote to keep strings intact */
    val += *(yystack_[1].value.stringVal);
    val += '"';
    (yylhs.value.whereNode) = new ibis::qExpr(ibis::qExpr::LOGICAL_NOT);
    (yylhs.value.whereNode)->setLeft(new ibis::qAnyString((yystack_[5].value.stringVal)->c_str(), val.c_str()));
    delete (yystack_[1].value.stringVal);
    delete (yystack_[5].value.stringVal);
}
#line 1304 "whereParser.cc"
    break;

  case 38: // simpleRange: "name string" "in" "signed integer sequence"
#line 540 "whereParser.yy"
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
#line 1319 "whereParser.cc"
    break;

  case 39: // simpleRange: "name string" "not" "in" "signed integer sequence"
#line 550 "whereParser.yy"
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
#line 1335 "whereParser.cc"
    break;

  case 40: // simpleRange: "name string" "in" "unsigned integer sequence"
#line 561 "whereParser.yy"
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
#line 1350 "whereParser.cc"
    break;

  case 41: // simpleRange: "name string" "not" "in" "unsigned integer sequence"
#line 571 "whereParser.yy"
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
#line 1366 "whereParser.cc"
    break;

  case 42: // simpleRange: "name string" "contains" "name string"
#line 582 "whereParser.yy"
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
#line 1381 "whereParser.cc"
    break;

  case 43: // simpleRange: "name string" "contains" "string literal"
#line 592 "whereParser.yy"
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
#line 1396 "whereParser.cc"
    break;

  case 44: // simpleRange: "name string" "contains" '(' "name string" ')'
#line 602 "whereParser.yy"
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
#line 1411 "whereParser.cc"
    break;

  case 45: // simpleRange: "name string" "contains" '(' "string literal" ')'
#line 612 "whereParser.yy"
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
#line 1426 "whereParser.cc"
    break;

  case 46: // simpleRange: "name string" "contains" '(' "string literal" ',' "string literal" ')'
#line 622 "whereParser.yy"
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
#line 1442 "whereParser.cc"
    break;

  case 47: // simpleRange: "name string" "contains" '(' "string literal" ',' "name string" ')'
#line 633 "whereParser.yy"
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
#line 1458 "whereParser.cc"
    break;

  case 48: // simpleRange: "name string" "contains" '(' "name string" ',' "string literal" ')'
#line 644 "whereParser.yy"
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
#line 1474 "whereParser.cc"
    break;

  case 49: // simpleRange: "name string" "contains" '(' "name string" ',' "name string" ')'
#line 655 "whereParser.yy"
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
#line 1490 "whereParser.cc"
    break;

  case 50: // simpleRange: "name string" "contains" "string sequence"
#line 666 "whereParser.yy"
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
#line 1505 "whereParser.cc"
    break;

  case 51: // simpleRange: "any" '(' "name string" ')' "==" "floating-point number"
#line 676 "whereParser.yy"
                                    {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- ANY(" << *(yystack_[3].value.stringVal) << ") = "
	<< (yystack_[0].value.doubleVal) << ")";
#endif
    (yylhs.value.whereNode) = new ibis::qAnyAny((yystack_[3].value.stringVal)->c_str(), (yystack_[0].value.doubleVal));
    delete (yystack_[3].value.stringVal);
}
#line 1519 "whereParser.cc"
    break;

  case 52: // simpleRange: "any" '(' "name string" ')' "in" "number sequence"
#line 685 "whereParser.yy"
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
#line 1534 "whereParser.cc"
    break;

  case 53: // simpleRange: "name string" "==" "integer value"
#line 695 "whereParser.yy"
                     {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.stringVal) << " = " << *(yystack_[0].value.int64Val);
#endif
    (yylhs.value.whereNode) = new ibis::qIntHod((yystack_[2].value.stringVal)->c_str(), (yystack_[0].value.int64Val));
    delete (yystack_[2].value.stringVal);
}
#line 1547 "whereParser.cc"
    break;

  case 54: // simpleRange: "name string" "!=" "integer value"
#line 703 "whereParser.yy"
                      {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.stringVal) << " != " << *(yystack_[0].value.int64Val);
#endif
    (yylhs.value.whereNode) = new ibis::qExpr(ibis::qExpr::LOGICAL_NOT);
    (yylhs.value.whereNode)->setLeft(new ibis::qIntHod((yystack_[2].value.stringVal)->c_str(), (yystack_[0].value.int64Val)));
    delete (yystack_[2].value.stringVal);
}
#line 1561 "whereParser.cc"
    break;

  case 55: // simpleRange: "name string" "==" "unsigned integer value"
#line 712 "whereParser.yy"
                      {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.stringVal) << " = " << *(yystack_[0].value.uint64Val);
#endif
    (yylhs.value.whereNode) = new ibis::qUIntHod((yystack_[2].value.stringVal)->c_str(), (yystack_[0].value.uint64Val));
    delete (yystack_[2].value.stringVal);
}
#line 1574 "whereParser.cc"
    break;

  case 56: // simpleRange: "name string" "!=" "unsigned integer value"
#line 720 "whereParser.yy"
                       {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.stringVal) << " != " << *(yystack_[0].value.uint64Val);
#endif
    (yylhs.value.whereNode) = new ibis::qExpr(ibis::qExpr::LOGICAL_NOT);
    (yylhs.value.whereNode)->setLeft(new ibis::qUIntHod((yystack_[2].value.stringVal)->c_str(), (yystack_[0].value.uint64Val)));
    delete (yystack_[2].value.stringVal);
}
#line 1588 "whereParser.cc"
    break;

  case 57: // simpleRange: "string literal" "==" "name string"
#line 729 "whereParser.yy"
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
#line 1603 "whereParser.cc"
    break;

  case 58: // simpleRange: "string literal" "!=" "name string"
#line 739 "whereParser.yy"
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
#line 1619 "whereParser.cc"
    break;

  case 59: // simpleRange: "name string" "==" "string literal"
#line 750 "whereParser.yy"
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
#line 1634 "whereParser.cc"
    break;

  case 60: // simpleRange: "name string" "!=" "string literal"
#line 760 "whereParser.yy"
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
#line 1650 "whereParser.cc"
    break;

  case 61: // simpleRange: "name string" "==" mathExpr
#line 771 "whereParser.yy"
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
#line 1672 "whereParser.cc"
    break;

  case 62: // simpleRange: "name string" "!=" mathExpr
#line 788 "whereParser.yy"
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
#line 1697 "whereParser.cc"
    break;

  case 63: // compRange2: mathExpr "<" "integer value"
#line 811 "whereParser.yy"
                    {
    /* exact comparisons with 64-bit integers, see qIntHod::compare */
    (yylhs.value.whereNode) = ibis::qIntHod::compare(static_cast<ibis::math::term*>((yystack_[2].value.whereNode)),
                                ibis::qExpr::OP_LT, (yystack_[0].value.int64Val));
}
#line 1707 "whereParser.cc"
    break;

  case 64: // compRange2: "integer value" "<" mathExpr
#line 816 "whereParser.yy"
                      {
    (yylhs.value.whereNode) = ibis::qIntHod::compare(static_cast<ibis::math::term*>((yystack_[0].value.whereNode)),
                                ibis::qExpr::OP_GT, (yystack_[2].value.int64Val));
}
#line 1716 "whereParser.cc"
    break;

  case 65: // compRange2: mathExpr "<=" "integer value"
#line 820 "whereParser.yy"
                      {
    (yylhs.value.whereNode) = ibis::qIntHod::compare(static_cast<ibis::math::term*>((yystack_[2].value.whereNode)),
                                ibis::qExpr::OP_LE, (yystack_[0].value.int64Val));
}
#line 1725 "whereParser.cc"
    break;

  case 66: // compRange2: "integer value" "<=" mathExpr
#line 824 "whereParser.yy"
                      {
    (yylhs.value.whereNode) = ibis::qIntHod::compare(static_cast<ibis::math::term*>((yystack_[0].value.whereNode)),
                                ibis::qExpr::OP_GE, (yystack_[2].value.int64Val));
}
#line 1734 "whereParser.cc"
    break;

  case 67: // compRange2: mathExpr ">" "integer value"
#line 828 "whereParser.yy"
                      {
    (yylhs.value.whereNode) = ibis::qIntHod::compare(static_cast<ibis::math::term*>((yystack_[2].value.whereNode)),
                                ibis::qExpr::OP_GT, (yystack_[0].value.int64Val));
}
#line 1743 "whereParser.cc"
    break;

  case 68: // compRange2: "integer value" ">" mathExpr
#line 832 "whereParser.yy"
                      {
    (yylhs.value.whereNode) = ibis::qIntHod::compare(static_cast<ibis::math::term*>((yystack_[0].value.whereNode)),
                                ibis::qExpr::OP_LT, (yystack_[2].value.int64Val));
}
#line 1752 "whereParser.cc"
    break;

  case 69: // compRange2: mathExpr ">=" "integer value"
#line 836 "whereParser.yy"
                      {
    (yylhs.value.whereNode) = ibis::qIntHod::compare(static_cast<ibis::math::term*>((yystack_[2].value.whereNode)),
                                ibis::qExpr::OP_GE, (yystack_[0].value.int64Val));
}
#line 1761 "whereParser.cc"
    break;

  case 70: // compRange2: "integer value" ">=" mathExpr
#line 840 "whereParser.yy"
                      {
    (yylhs.value.whereNode) = ibis::qIntHod::compare(static_cast<ibis::math::term*>((yystack_[0].value.whereNode)),
                                ibis::qExpr::OP_LE, (yystack_[2].value.int64Val));
}
#line 1770 "whereParser.cc"
    break;

  case 71: // compRange2: mathExpr "between" "integer value" "and" "integer value"
#line 844 "whereParser.yy"
                                       {
    (yylhs.value.whereNode) = ibis::qIntHod::between(static_cast<ibis::math::term*>((yystack_[4].value.whereNode)), (yystack_[2].value.int64Val), (yystack_[0].value.int64Val));
}
#line 1778 "whereParser.cc"
    break;

  case 72: // compRange2: mathExpr "<" "unsigned integer value"
#line 847 "whereParser.yy"
                       {
    (yylhs.value.whereNode) = ibis::qUIntHod::compare(static_cast<ibis::math::term*>((yystack_[2].value.whereNode)),
                                ibis::qExpr::OP_LT, (yystack_[0].value.uint64Val));
}
#line 1787 "whereParser.cc"
    break;

  case 73: // compRange2: "unsigned integer value" "<" mathExpr
#line 851 "whereParser.yy"
                       {
    (yylhs.value.whereNode) = ibis::qUIntHod::compare(static_cast<ibis::math::term*>((yystack_[0].value.whereNode)),
                                ibis::qExpr::OP_GT, (yystack_[2].value.uint64Val));
}
#line 1796 "whereParser.cc"
    break;

  case 74: // compRange2: mathExpr "<=" "unsigned integer value"
#line 855 "whereParser.yy"
                       {
    (yylhs.value.whereNode) = ibis::qUIntHod::compare(static_cast<ibis::math::term*>((yystack_[2].value.whereNode)),
                                ibis::qExpr::OP_LE, (yystack_[0].value.uint64Val));
}
#line 1805 "whereParser.cc"
    break;

  case 75: // compRange2: "unsigned integer value" "<=" mathExpr
#line 859 "whereParser.yy"
                       {
    (yylhs.value.whereNode) = ibis::qUIntHod::compare(static_cast<ibis::math::term*>((yystack_[0].value.whereNode)),
                                ibis::qExpr::OP_GE, (yystack_[2].value.uint64Val));
}
#line 1814 "whereParser.cc"
    break;

  case 76: // compRange2: mathExpr ">" "unsigned integer value"
#line 863 "whereParser.yy"
                       {
    (yylhs.value.whereNode) = ibis::qUIntHod::compare(static_cast<ibis::math::term*>((yystack_[2].value.whereNode)),
                                ibis::qExpr::OP_GT, (yystack_[0].value.uint64Val));
}
#line 1823 "whereParser.cc"
    break;

  case 77: // compRange2: "unsigned integer value" ">" mathExpr
#line 867 "whereParser.yy"
                       {
    (yylhs.value.whereNode) = ibis::qUIntHod::compare(static_cast<ibis::math::term*>((yystack_[0].value.whereNode)),
                                ibis::qExpr::OP_LT, (yystack_[2].value.uint64Val));
}
#line 1832 "whereParser.cc"
    break;

  case 78: // compRange2: mathExpr ">=" "unsigned integer value"
#line 871 "whereParser.yy"
                       {
    (yylhs.value.whereNode) = ibis::qUIntHod::compare(static_cast<ibis::math::term*>((yystack_[2].value.whereNode)),
                                ibis::qExpr::OP_GE, (yystack_[0].value.uint64Val));
}
#line 1841 "whereParser.cc"
    break;

  case 79: // compRange2: "unsigned integer value" ">=" mathExpr
#line 875 "whereParser.yy"
                       {
    (yylhs.value.whereNode) = ibis::qUIntHod::compare(static_cast<ibis::math::term*>((yystack_[0].value.whereNode)),
                                ibis::qExpr::OP_LE, (yystack_[2].value.uint64Val));
}
#line 1850 "whereParser.cc"
    break;

  case 80: // compRange2: mathExpr "between" "unsigned integer value" "and" "unsigned integer value"
#line 879 "whereParser.yy"
                                         {
    (yylhs.value.whereNode) = ibis::qUIntHod::between(static_cast<ibis::math::term*>((yystack_[4].value.whereNode)), (yystack_[2].value.uint64Val), (yystack_[0].value.uint64Val));
}
#line 1858 "whereParser.cc"
    break;

  case 81: // compRange2: mathExpr "==" mathExpr
#line 882 "whereParser.yy"
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
#line 1873 "whereParser.cc"
    break;

  case 82: // compRange2: mathExpr "!=" mathExpr
#line 892 "whereParser.yy"
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
#line 1889 "whereParser.cc"
    break;

  case 83: // compRange2: mathExpr "<" mathExpr
#line 903 "whereParser.yy"
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
#line 1904 "whereParser.cc"
    break;

  case 84: // compRange2: mathExpr "<=" mathExpr
#line 913 "whereParser.yy"
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
#line 1919 "whereParser.cc"
    break;

  case 85: // compRange2: mathExpr ">" mathExpr
#line 923 "whereParser.yy"
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
#line 1934 "whereParser.cc"
    break;

  case 86: // compRange2: mathExpr ">=" mathExpr
#line 933 "whereParser.yy"
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
#line 1949 "whereParser.cc"
    break;

  case 87: // compRange2: mathExpr "<" "string literal"
#line 992 "whereParser.yy"
                       {
    (yylhs.value.whereNode) = stringCompare((yystack_[2].value.whereNode), ibis::qString::STR_LT, (yystack_[0].value.stringVal), 0);
}
#line 1957 "whereParser.cc"
    break;

  case 88: // compRange2: mathExpr "<=" "string literal"
#line 995 "whereParser.yy"
                       {
    (yylhs.value.whereNode) = stringCompare((yystack_[2].value.whereNode), ibis::qString::STR_LE, (yystack_[0].value.stringVal), 0);
}
#line 1965 "whereParser.cc"
    break;

  case 89: // compRange2: mathExpr ">" "string literal"
#line 998 "whereParser.yy"
                       {
    (yylhs.value.whereNode) = stringCompare((yystack_[2].value.whereNode), ibis::qString::STR_GT, (yystack_[0].value.stringVal), 0);
}
#line 1973 "whereParser.cc"
    break;

  case 90: // compRange2: mathExpr ">=" "string literal"
#line 1001 "whereParser.yy"
                       {
    (yylhs.value.whereNode) = stringCompare((yystack_[2].value.whereNode), ibis::qString::STR_GE, (yystack_[0].value.stringVal), 0);
}
#line 1981 "whereParser.cc"
    break;

  case 91: // compRange2: mathExpr "between" "string literal" "and" "string literal"
#line 1004 "whereParser.yy"
                                         {
    (yylhs.value.whereNode) = stringCompare((yystack_[4].value.whereNode), ibis::qString::STR_BETWEEN, (yystack_[2].value.stringVal), (yystack_[0].value.stringVal));
}
#line 1989 "whereParser.cc"
    break;

  case 92: // compRange3: mathExpr "<" mathExpr "<" mathExpr
#line 1010 "whereParser.yy"
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
#line 2006 "whereParser.cc"
    break;

  case 93: // compRange3: mathExpr "<" mathExpr "<=" mathExpr
#line 1022 "whereParser.yy"
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
#line 2023 "whereParser.cc"
    break;

  case 94: // compRange3: mathExpr "<=" mathExpr "<" mathExpr
#line 1034 "whereParser.yy"
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
#line 2040 "whereParser.cc"
    break;

  case 95: // compRange3: mathExpr "<=" mathExpr "<=" mathExpr
#line 1046 "whereParser.yy"
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
#line 2057 "whereParser.cc"
    break;

  case 96: // compRange3: mathExpr ">" mathExpr ">" mathExpr
#line 1058 "whereParser.yy"
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
#line 2074 "whereParser.cc"
    break;

  case 97: // compRange3: mathExpr ">" mathExpr ">=" mathExpr
#line 1070 "whereParser.yy"
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
#line 2091 "whereParser.cc"
    break;

  case 98: // compRange3: mathExpr ">=" mathExpr ">" mathExpr
#line 1082 "whereParser.yy"
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
#line 2108 "whereParser.cc"
    break;

  case 99: // compRange3: mathExpr ">=" mathExpr ">=" mathExpr
#line 1094 "whereParser.yy"
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
#line 2125 "whereParser.cc"
    break;

  case 100: // compRange3: mathExpr "between" mathExpr "and" mathExpr
#line 1106 "whereParser.yy"
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
#line 2142 "whereParser.cc"
    break;

  case 101: // mathExpr: mathExpr "+" mathExpr
#line 1121 "whereParser.yy"
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
#line 2159 "whereParser.cc"
    break;

  case 102: // mathExpr: mathExpr "-" mathExpr
#line 1133 "whereParser.yy"
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
#line 2176 "whereParser.cc"
    break;

  case 103: // mathExpr: mathExpr "*" mathExpr
#line 1145 "whereParser.yy"
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
#line 2193 "whereParser.cc"
    break;

  case 104: // mathExpr: mathExpr "/" mathExpr
#line 1157 "whereParser.yy"
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
#line 2210 "whereParser.cc"
    break;

  case 105: // mathExpr: mathExpr "%" mathExpr
#line 1169 "whereParser.yy"
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
#line 2227 "whereParser.cc"
    break;

  case 106: // mathExpr: mathExpr "**" mathExpr
#line 1181 "whereParser.yy"
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
#line 2244 "whereParser.cc"
    break;

  case 107: // mathExpr: mathExpr "&" mathExpr
#line 1193 "whereParser.yy"
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
#line 2261 "whereParser.cc"
    break;

  case 108: // mathExpr: mathExpr "|" mathExpr
#line 1205 "whereParser.yy"
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
#line 2278 "whereParser.cc"
    break;

  case 109: // mathExpr: "name string" '(' mathExpr ')'
#line 1217 "whereParser.yy"
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
#line 2295 "whereParser.cc"
    break;

  case 110: // mathExpr: "name string" '(' mathExpr ',' mathExpr ')'
#line 1229 "whereParser.yy"
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
#line 2313 "whereParser.cc"
    break;

  case 111: // mathExpr: "FROM_UNIXTIME_LOCAL" '(' mathExpr ',' "string literal" ')'
#line 1242 "whereParser.yy"
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
#line 2331 "whereParser.cc"
    break;

  case 112: // mathExpr: "FROM_UNIXTIME_GMT" '(' mathExpr ',' "string literal" ')'
#line 1255 "whereParser.yy"
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
#line 2350 "whereParser.cc"
    break;

  case 113: // mathExpr: "ISO_TO_UNIXTIME_LOCAL" '(' mathExpr ')'
#line 1269 "whereParser.yy"
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
#line 2368 "whereParser.cc"
    break;

  case 114: // mathExpr: "ISO_TO_UNIXTIME_GMT" '(' mathExpr ')'
#line 1282 "whereParser.yy"
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
#line 2386 "whereParser.cc"
    break;

  case 115: // mathExpr: "TO_UNIXTIME_LOCAL" '(' "string literal" ',' "string literal" ')'
#line 1295 "whereParser.yy"
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
#line 2432 "whereParser.cc"
    break;

  case 116: // mathExpr: "TO_UNIXTIME_GMT" '(' "string literal" ',' "string literal" ')'
#line 1336 "whereParser.yy"
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
#line 2474 "whereParser.cc"
    break;

  case 117: // mathExpr: "-" mathExpr
#line 1373 "whereParser.yy"
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
#line 2489 "whereParser.cc"
    break;

  case 118: // mathExpr: "+" mathExpr
#line 1383 "whereParser.yy"
                             {
    (yylhs.value.whereNode) = (yystack_[0].value.whereNode);
}
#line 2497 "whereParser.cc"
    break;

  case 119: // mathExpr: '(' mathExpr ')'
#line 1386 "whereParser.yy"
                   {
    (yylhs.value.whereNode) = (yystack_[1].value.whereNode);
}
#line 2505 "whereParser.cc"
    break;

  case 120: // mathExpr: "name string"
#line 1389 "whereParser.yy"
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
#line 2520 "whereParser.cc"
    break;

  case 121: // mathExpr: "floating-point number"
#line 1399 "whereParser.yy"
         {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " got a number " << (yystack_[0].value.doubleVal);
#endif
    ibis::math::number *num = new ibis::math::number((yystack_[0].value.doubleVal));
    (yylhs.value.whereNode) = static_cast<ibis::qExpr*>(num);
}
#line 2533 "whereParser.cc"
    break;

  case 122: // START: qexpr "end of input"
#line 1409 "whereParser.yy"
                  { /* pass qexpr to the driver */
    driver.expr_ = (yystack_[1].value.whereNode);
}
#line 2541 "whereParser.cc"
    break;

  case 123: // START: qexpr ';'
#line 1412 "whereParser.yy"
            { /* pass qexpr to the driver */
    driver.expr_ = (yystack_[1].value.whereNode);
}
#line 2549 "whereParser.cc"
    break;


#line 2553 "whereParser.cc"

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


  const signed char whereParser::yypact_ninf_ = -42;

  const signed char whereParser::yytable_ninf_ = -1;

  const short
  whereParser::yypact_[] =
  {
      69,    69,   -35,   -41,   -36,   -21,    31,    51,    62,    79,
     427,   427,   158,   188,   -42,   117,    32,    69,     2,   -42,
     -42,   -42,   176,   130,   -42,   -42,   -42,   -23,   427,   427,
      89,    91,   427,   427,   127,   108,   427,   -42,   -42,   427,
     427,   427,   427,   427,   427,   427,   427,     3,   245,   271,
      68,    36,   -15,   427,   139,   149,    33,   110,   -42,    69,
      69,    69,    69,   -42,   297,   323,   349,   375,   427,   427,
     401,   427,   427,   427,   427,   427,   427,   427,   427,   -42,
     151,   155,   454,   475,   164,   182,   483,   503,   189,   511,
     212,   212,   212,   212,   212,   212,   212,   212,   -42,   116,
     -42,   -42,   -42,   212,   -42,   -42,   -42,   212,   -42,   -42,
     -42,    -3,   -42,   -42,   -42,   -42,    45,   -42,   -42,   446,
     -42,   -42,   -42,   -42,   -42,   -42,    37,    41,   -42,   -42,
     -42,   185,   -42,   -42,   -42,   222,   -42,   -42,   -42,   193,
     -42,   -42,   -42,   230,   212,   212,   226,   268,   273,   118,
     466,   429,   200,   200,   253,   253,   253,   253,   -42,   -42,
     246,   254,   255,   256,   -42,   -42,    -6,   -42,   -42,   -42,
     -42,    60,    55,    76,   113,   131,   141,   -42,   427,   427,
     427,   427,   427,   427,   427,   427,   427,   267,   269,   260,
     427,   258,   263,   264,   266,   276,   274,   201,   225,   231,
     -42,    40,   -42,    44,   -42,   286,   -42,    88,   -42,   159,
     531,   212,   212,   212,   212,   212,   212,   212,   212,   -42,
     -42,   -42,   212,   -42,   -42,   -42,   -42,   -42,   -42,   -42,
     287,   -42,   233,   -42,   243,   279,   282,   283,   284,   285,
     289,   290,   292,   293,   -42,   295,   303,   304,   305,   308,
     -42,   -42,   -42,   -42,   -42,   -42,   -42,   -42,   -42,   -42,
     -42,   -42,   -42,   -42
  };

  const signed char
  whereParser::yydefact_[] =
  {
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,   121,   120,     0,     0,     0,     8,
       9,    10,     0,     0,     6,    11,    12,     0,     0,     0,
       0,     0,     0,     0,     0,   120,     0,   118,   117,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,   122,     0,
       0,     0,     0,   123,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     1,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
      66,    70,    64,    68,    75,    79,    73,    77,    18,     0,
      53,    55,    59,    61,    54,    56,    60,    62,    42,    50,
      43,     0,    38,    40,    15,    22,     0,    29,    30,     0,
      57,    58,     7,   119,     4,     5,     2,     3,    65,    74,
      88,    84,    69,    78,    90,    86,    63,    72,    87,    83,
      67,    76,    89,    85,    81,    82,     0,     0,     0,     0,
     108,   107,   101,   102,   103,   104,   105,   106,    13,    14,
       0,     0,     0,     0,   114,   113,     0,    39,    41,    19,
      31,     0,     0,     0,     0,     0,     0,   109,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
      44,     0,    45,     0,    17,     0,    27,     0,    28,     0,
       0,    95,    94,    99,    98,    93,    92,    97,    96,    71,
      80,    91,   100,   112,   111,   116,   115,    51,    52,    21,
       0,    36,     0,    37,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,   110,     0,     0,     0,     0,     0,
      49,    48,    47,    46,    16,    23,    25,    24,    26,    20,
      32,    34,    33,    35
  };

  const signed char
  whereParser::yypgoto_[] =
  {
     -42,    10,   -42,   -42,   -42,   -10,   -42
  };

  const signed char
  whereParser::yydefgoto_[] =
  {
       0,    18,    19,    20,    21,    22,    23
  };

  const short
  whereParser::yytable_[] =
  {
      37,    38,    58,   195,    28,    25,    98,    57,    26,    29,
      27,    24,   196,    59,    60,    61,    62,    80,    82,    83,
      81,    99,    86,    87,    30,   117,    89,    56,   118,    90,
      91,    92,    93,    94,    95,    96,    97,   172,   103,   107,
     173,    54,    55,   119,    59,    60,    61,    62,    59,    60,
      63,    62,    59,    60,   131,   135,   139,   143,   144,   145,
     149,   150,   151,   152,   153,   154,   155,   156,   157,   124,
     125,   126,   127,     1,   112,   113,    31,   114,   115,   122,
     235,   116,   174,   236,   237,   175,     2,   238,   176,     3,
       4,     5,     6,     7,     8,     9,    32,   197,    10,    11,
     198,   200,   201,   199,    12,    13,    14,    33,   108,    15,
     109,   110,    16,   111,    17,    64,    65,    66,    67,    68,
      69,    47,   202,   203,    34,    70,    48,    49,   240,   190,
      79,   241,    84,    50,    85,    51,    52,    71,    72,    73,
      74,    75,    76,    77,    78,    71,    72,    73,    74,    75,
      76,    77,    78,    53,   167,   168,   123,   169,   170,   204,
     205,   171,    53,    39,    40,    41,    42,    88,   210,   211,
     212,   213,   214,   215,   216,   217,   218,   206,   207,   120,
     222,    64,    65,    66,    67,    68,    69,   208,   209,   121,
     179,    70,   180,    43,    44,    45,    46,   158,   183,   242,
     184,   159,   243,    71,    72,    73,    74,    75,    76,    77,
      78,   162,    71,    72,    73,    74,    75,    76,    77,    78,
      71,    72,    73,    74,    75,    76,    77,    78,   181,   163,
     182,    75,    76,    77,    78,   166,   185,   187,   186,    71,
      72,    73,    74,    75,    76,    77,    78,   229,   230,    71,
      72,    73,    74,    75,    76,    77,    78,    71,    72,    73,
      74,    75,    76,    77,    78,     3,     4,     5,     6,     7,
       8,   231,   232,   246,    10,    11,   247,   233,   234,   188,
     100,   101,    14,   248,   189,    35,   249,    78,   102,   191,
      36,     3,     4,     5,     6,     7,     8,   192,   193,   194,
      10,    11,   219,   221,   223,   220,   104,   105,    14,   224,
     225,    35,   226,   227,   106,   228,    36,     3,     4,     5,
       6,     7,     8,   239,   245,   250,    10,    11,   251,   252,
     253,   254,   128,   129,    14,   255,   256,    35,   257,   258,
     130,   259,    36,     3,     4,     5,     6,     7,     8,   260,
     261,   262,    10,    11,   263,     0,     0,     0,   132,   133,
      14,     0,     0,    35,     0,     0,   134,     0,    36,     3,
       4,     5,     6,     7,     8,     0,     0,     0,    10,    11,
       0,     0,     0,     0,   136,   137,    14,     0,     0,    35,
       0,     0,   138,     0,    36,     3,     4,     5,     6,     7,
       8,     0,     0,     0,    10,    11,     0,     0,     0,     0,
     140,   141,    14,     0,     0,    35,     0,     0,   142,     0,
      36,     3,     4,     5,     6,     7,     8,     0,     0,     0,
      10,    11,     0,     0,     0,     0,   146,   147,    14,     0,
       0,    35,     0,     0,   148,     0,    36,     3,     4,     5,
       6,     7,     8,     0,     0,     0,    10,    11,    73,    74,
      75,    76,    77,    78,    14,     0,     0,    35,     0,     0,
       0,     0,    36,    71,    72,    73,    74,    75,    76,    77,
      78,    71,    72,    73,    74,    75,    76,    77,    78,     0,
       0,     0,   177,   178,    72,    73,    74,    75,    76,    77,
      78,   160,    71,    72,    73,    74,    75,    76,    77,    78,
      71,    72,    73,    74,    75,    76,    77,    78,     0,     0,
       0,     0,   161,     0,     0,     0,     0,     0,     0,   164,
      71,    72,    73,    74,    75,    76,    77,    78,    71,    72,
      73,    74,    75,    76,    77,    78,     0,     0,     0,   165,
       0,     0,     0,     0,     0,     0,     0,   123,    71,    72,
      73,    74,    75,    76,    77,    78,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,   244
  };

  const short
  whereParser::yycheck_[] =
  {
      10,    11,     0,     9,    45,    40,     3,    17,    43,    45,
      45,     1,    18,    11,    12,    13,    14,    40,    28,    29,
      43,    18,    32,    33,    45,    40,    36,    17,    43,    39,
      40,    41,    42,    43,    44,    45,    46,    40,    48,    49,
      43,     9,    10,    53,    11,    12,    13,    14,    11,    12,
      48,    14,    11,    12,    64,    65,    66,    67,    68,    69,
      70,    71,    72,    73,    74,    75,    76,    77,    78,    59,
      60,    61,    62,     4,    38,    39,    45,    41,    42,    46,
      40,    45,    37,    43,    40,    40,    17,    43,    43,    20,
      21,    22,    23,    24,    25,    26,    45,    37,    29,    30,
      40,    46,    47,    43,    35,    36,    37,    45,    40,    40,
      42,    43,    43,    45,    45,     5,     6,     7,     8,     9,
      10,     4,    46,    47,    45,    15,     9,    10,    40,    11,
       0,    43,    43,    16,    43,    18,    19,    27,    28,    29,
      30,    31,    32,    33,    34,    27,    28,    29,    30,    31,
      32,    33,    34,    45,    38,    39,    46,    41,    42,    46,
      47,    45,    45,     5,     6,     7,     8,    40,   178,   179,
     180,   181,   182,   183,   184,   185,   186,    46,    47,    40,
     190,     5,     6,     7,     8,     9,    10,    46,    47,    40,
       5,    15,     7,     5,     6,     7,     8,    46,     5,    40,
       7,    46,    43,    27,    28,    29,    30,    31,    32,    33,
      34,    47,    27,    28,    29,    30,    31,    32,    33,    34,
      27,    28,    29,    30,    31,    32,    33,    34,     6,    47,
       8,    31,    32,    33,    34,    46,     6,    11,     8,    27,
      28,    29,    30,    31,    32,    33,    34,    46,    47,    27,
      28,    29,    30,    31,    32,    33,    34,    27,    28,    29,
      30,    31,    32,    33,    34,    20,    21,    22,    23,    24,
      25,    46,    47,    40,    29,    30,    43,    46,    47,    11,
      35,    36,    37,    40,    11,    40,    43,    34,    43,    43,
      45,    20,    21,    22,    23,    24,    25,    43,    43,    43,
      29,    30,    35,    43,    46,    36,    35,    36,    37,    46,
      46,    40,    46,    37,    43,    41,    45,    20,    21,    22,
      23,    24,    25,    37,    37,    46,    29,    30,    46,    46,
      46,    46,    35,    36,    37,    46,    46,    40,    46,    46,
      43,    46,    45,    20,    21,    22,    23,    24,    25,    46,
      46,    46,    29,    30,    46,    -1,    -1,    -1,    35,    36,
      37,    -1,    -1,    40,    -1,    -1,    43,    -1,    45,    20,
      21,    22,    23,    24,    25,    -1,    -1,    -1,    29,    30,
      -1,    -1,    -1,    -1,    35,    36,    37,    -1,    -1,    40,
      -1,    -1,    43,    -1,    45,    20,    21,    22,    23,    24,
      25,    -1,    -1,    -1,    29,    30,    -1,    -1,    -1,    -1,
      35,    36,    37,    -1,    -1,    40,    -1,    -1,    43,    -1,
      45,    20,    21,    22,    23,    24,    25,    -1,    -1,    -1,
      29,    30,    -1,    -1,    -1,    -1,    35,    36,    37,    -1,
      -1,    40,    -1,    -1,    43,    -1,    45,    20,    21,    22,
      23,    24,    25,    -1,    -1,    -1,    29,    30,    29,    30,
      31,    32,    33,    34,    37,    -1,    -1,    40,    -1,    -1,
      -1,    -1,    45,    27,    28,    29,    30,    31,    32,    33,
      34,    27,    28,    29,    30,    31,    32,    33,    34,    -1,
      -1,    -1,    46,    47,    28,    29,    30,    31,    32,    33,
      34,    47,    27,    28,    29,    30,    31,    32,    33,    34,
      27,    28,    29,    30,    31,    32,    33,    34,    -1,    -1,
      -1,    -1,    47,    -1,    -1,    -1,    -1,    -1,    -1,    46,
      27,    28,    29,    30,    31,    32,    33,    34,    27,    28,
      29,    30,    31,    32,    33,    34,    -1,    -1,    -1,    46,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    46,    27,    28,
      29,    30,    31,    32,    33,    34,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    46
  };

  const signed char
  whereParser::yystos_[] =
  {
       0,     4,    17,    20,    21,    22,    23,    24,    25,    26,
      29,    30,    35,    36,    37,    40,    43,    45,    50,    51,
      52,    53,    54,    55,    50,    40,    43,    45,    45,    45,
      45,    45,    45,    45,    45,    40,    45,    54,    54,     5,
       6,     7,     8,     5,     6,     7,     8,     4,     9,    10,
      16,    18,    19,    45,     9,    10,    50,    54,     0,    11,
      12,    13,    14,    48,     5,     6,     7,     8,     9,    10,
      15,    27,    28,    29,    30,    31,    32,    33,    34,     0,
      40,    43,    54,    54,    43,    43,    54,    54,    40,    54,
      54,    54,    54,    54,    54,    54,    54,    54,     3,    18,
      35,    36,    43,    54,    35,    36,    43,    54,    40,    42,
      43,    45,    38,    39,    41,    42,    45,    40,    43,    54,
      40,    40,    46,    46,    50,    50,    50,    50,    35,    36,
      43,    54,    35,    36,    43,    54,    35,    36,    43,    54,
      35,    36,    43,    54,    54,    54,    35,    36,    43,    54,
      54,    54,    54,    54,    54,    54,    54,    54,    46,    46,
      47,    47,    47,    47,    46,    46,    46,    38,    39,    41,
      42,    45,    40,    43,    37,    40,    43,    46,    47,     5,
       7,     6,     8,     5,     7,     6,     8,    11,    11,    11,
      11,    43,    43,    43,    43,     9,    18,    37,    40,    43,
      46,    47,    46,    47,    46,    47,    46,    47,    46,    47,
      54,    54,    54,    54,    54,    54,    54,    54,    54,    35,
      36,    43,    54,    46,    46,    46,    46,    37,    41,    46,
      47,    46,    47,    46,    47,    40,    43,    40,    43,    37,
      40,    43,    40,    43,    46,    37,    40,    43,    40,    43,
      46,    46,    46,    46,    46,    46,    46,    46,    46,    46,
      46,    46,    46,    46
  };

  const signed char
  whereParser::yyr1_[] =
  {
       0,    49,    50,    50,    50,    50,    50,    50,    50,    50,
      50,    51,    51,    51,    51,    51,    51,    51,    51,    51,
      51,    51,    51,    51,    51,    51,    51,    51,    51,    51,
      51,    51,    51,    51,    51,    51,    51,    51,    51,    51,
      51,    51,    51,    51,    51,    51,    51,    51,    51,    51,
      51,    51,    51,    51,    51,    51,    51,    51,    51,    51,
      51,    51,    51,    52,    52,    52,    52,    52,    52,    52,
      52,    52,    52,    52,    52,    52,    52,    52,    52,    52,
      52,    52,    52,    52,    52,    52,    52,    52,    52,    52,
      52,    52,    53,    53,    53,    53,    53,    53,    53,    53,
      53,    54,    54,    54,    54,    54,    54,    54,    54,    54,
      54,    54,    54,    54,    54,    54,    54,    54,    54,    54,
      54,    54,    55,    55
  };

  const signed char
  whereParser::yyr2_[] =
  {
       0,     2,     3,     3,     3,     3,     2,     3,     1,     1,
       1,     2,     2,     4,     4,     3,     7,     5,     3,     4,
       8,     6,     3,     7,     7,     7,     7,     5,     5,     3,
       3,     4,     8,     8,     8,     8,     6,     6,     3,     4,
       3,     4,     3,     3,     5,     5,     7,     7,     7,     7,
       3,     6,     6,     3,     3,     3,     3,     3,     3,     3,
       3,     3,     3,     3,     3,     3,     3,     3,     3,     3,
       3,     5,     3,     3,     3,     3,     3,     3,     3,     3,
       5,     3,     3,     3,     3,     3,     3,     3,     3,     3,
       3,     5,     5,     5,     5,     5,     5,     5,     5,     5,
       5,     3,     3,     3,     3,     3,     3,     3,     3,     4,
       6,     6,     6,     4,     4,     6,     6,     2,     2,     3,
       1,     1,     2,     2
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
  "\"exists\"", "\"in\"", "\"like\"", "\"FROM_UNIXTIME_GMT\"",
  "\"FROM_UNIXTIME_LOCAL\"", "\"TO_UNIXTIME_GMT\"",
  "\"TO_UNIXTIME_LOCAL\"", "\"ISO_TO_UNIXTIME_GMT\"",
  "\"ISO_TO_UNIXTIME_LOCAL\"", "\"any\"", "\"|\"", "\"&\"", "\"+\"",
  "\"-\"", "\"*\"", "\"/\"", "\"%\"", "\"**\"", "\"integer value\"",
  "\"unsigned integer value\"", "\"floating-point number\"",
  "\"signed integer sequence\"", "\"unsigned integer sequence\"",
  "\"name string\"", "\"number sequence\"", "\"string sequence\"",
  "\"string literal\"", "CONSTAINSOP", "'('", "')'", "','", "';'",
  "$accept", "qexpr", "simpleRange", "compRange2", "compRange3",
  "mathExpr", "START", YY_NULLPTR
  };
#endif


#if YYDEBUG
  const short
  whereParser::yyrline_[] =
  {
       0,   140,   140,   150,   160,   170,   180,   188,   191,   192,
     193,   197,   205,   213,   221,   229,   239,   251,   260,   267,
     278,   291,   301,   311,   328,   345,   362,   379,   393,   407,
     417,   427,   438,   456,   474,   492,   510,   525,   540,   550,
     561,   571,   582,   592,   602,   612,   622,   633,   644,   655,
     666,   676,   685,   695,   703,   712,   720,   729,   739,   750,
     760,   771,   788,   811,   816,   820,   824,   828,   832,   836,
     840,   844,   847,   851,   855,   859,   863,   867,   871,   875,
     879,   882,   892,   903,   913,   923,   933,   992,   995,   998,
    1001,  1004,  1010,  1022,  1034,  1046,  1058,  1070,  1082,  1094,
    1106,  1121,  1133,  1145,  1157,  1169,  1181,  1193,  1205,  1217,
    1229,  1242,  1255,  1269,  1282,  1295,  1336,  1373,  1383,  1386,
    1389,  1399,  1409,  1412
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
      45,    46,     2,     2,    47,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,    48,
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
      35,    36,    37,    38,    39,    40,    41,    42,    43,    44
    };
    // Last valid token kind.
    const int code_max = 299;

    if (t <= 0)
      return symbol_kind::S_YYEOF;
    else if (t <= code_max)
      return static_cast <symbol_kind_type> (translate_table[t]);
    else
      return symbol_kind::S_YYUNDEF;
  }

#line 25 "whereParser.yy"
} // ibis
#line 3303 "whereParser.cc"

#line 1417 "whereParser.yy"

void ibis::whereParser::error(const ibis::whereParser::location_type& l,
			      const std::string& m) {
    LOGGER(ibis::gVerbose >= 0)
	<< "Warning -- ibis::whereParser encountered " << m
	<< " at location " << l;
}
