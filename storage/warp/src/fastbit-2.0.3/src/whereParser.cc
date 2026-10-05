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

#line 62 "whereParser.cc"



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
#line 156 "whereParser.cc"

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
#line 368 "whereParser.cc"
        break;

      case symbol_kind::S_UINTSEQ: // "unsigned integer sequence"
#line 103 "whereParser.yy"
                    { delete (yysym.value.stringVal); }
#line 374 "whereParser.cc"
        break;

      case symbol_kind::S_NOUNSTR: // "name string"
#line 103 "whereParser.yy"
                    { delete (yysym.value.stringVal); }
#line 380 "whereParser.cc"
        break;

      case symbol_kind::S_NUMSEQ: // "number sequence"
#line 103 "whereParser.yy"
                    { delete (yysym.value.stringVal); }
#line 386 "whereParser.cc"
        break;

      case symbol_kind::S_STRSEQ: // "string sequence"
#line 103 "whereParser.yy"
                    { delete (yysym.value.stringVal); }
#line 392 "whereParser.cc"
        break;

      case symbol_kind::S_STRLIT: // "string literal"
#line 103 "whereParser.yy"
                    { delete (yysym.value.stringVal); }
#line 398 "whereParser.cc"
        break;

      case symbol_kind::S_qexpr: // qexpr
#line 104 "whereParser.yy"
                    { delete (yysym.value.whereNode); }
#line 404 "whereParser.cc"
        break;

      case symbol_kind::S_simpleRange: // simpleRange
#line 104 "whereParser.yy"
                    { delete (yysym.value.whereNode); }
#line 410 "whereParser.cc"
        break;

      case symbol_kind::S_compRange2: // compRange2
#line 104 "whereParser.yy"
                    { delete (yysym.value.whereNode); }
#line 416 "whereParser.cc"
        break;

      case symbol_kind::S_compRange3: // compRange3
#line 104 "whereParser.yy"
                    { delete (yysym.value.whereNode); }
#line 422 "whereParser.cc"
        break;

      case symbol_kind::S_mathExpr: // mathExpr
#line 104 "whereParser.yy"
                    { delete (yysym.value.whereNode); }
#line 428 "whereParser.cc"
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

#line 570 "whereParser.cc"


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
#line 115 "whereParser.yy"
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
#line 717 "whereParser.cc"
    break;

  case 3: // qexpr: qexpr "xor" qexpr
#line 125 "whereParser.yy"
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
#line 732 "whereParser.cc"
    break;

  case 4: // qexpr: qexpr "and" qexpr
#line 135 "whereParser.yy"
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
#line 747 "whereParser.cc"
    break;

  case 5: // qexpr: qexpr "&!" qexpr
#line 145 "whereParser.yy"
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
#line 762 "whereParser.cc"
    break;

  case 6: // qexpr: "not" qexpr
#line 155 "whereParser.yy"
              {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- ! " << *(yystack_[0].value.whereNode);
#endif
    (yylhs.value.whereNode) = new ibis::qExpr(ibis::qExpr::LOGICAL_NOT);
    (yylhs.value.whereNode)->setLeft((yystack_[0].value.whereNode));
}
#line 775 "whereParser.cc"
    break;

  case 7: // qexpr: '(' qexpr ')'
#line 163 "whereParser.yy"
                            {
    (yylhs.value.whereNode) = (yystack_[1].value.whereNode);
}
#line 783 "whereParser.cc"
    break;

  case 8: // qexpr: simpleRange
#line 166 "whereParser.yy"
  { (yylhs.value.whereNode) = (yystack_[0].value.whereNode); }
#line 789 "whereParser.cc"
    break;

  case 9: // qexpr: compRange2
#line 167 "whereParser.yy"
  { (yylhs.value.whereNode) = (yystack_[0].value.whereNode); }
#line 795 "whereParser.cc"
    break;

  case 10: // qexpr: compRange3
#line 168 "whereParser.yy"
  { (yylhs.value.whereNode) = (yystack_[0].value.whereNode); }
#line 801 "whereParser.cc"
    break;

  case 11: // simpleRange: "exists" "name string"
#line 172 "whereParser.yy"
                 {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- EXISTS(" << *(yystack_[0].value.stringVal) << ')';
#endif
    (yylhs.value.whereNode) = new ibis::qExists((yystack_[0].value.stringVal)->c_str());
    delete (yystack_[0].value.stringVal);
}
#line 814 "whereParser.cc"
    break;

  case 12: // simpleRange: "exists" "string literal"
#line 180 "whereParser.yy"
                  {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- EXISTS(" << *(yystack_[0].value.stringVal) << ')';
#endif
    (yylhs.value.whereNode) = new ibis::qExists((yystack_[0].value.stringVal)->c_str());
    delete (yystack_[0].value.stringVal);
}
#line 827 "whereParser.cc"
    break;

  case 13: // simpleRange: "exists" '(' "name string" ')'
#line 188 "whereParser.yy"
                           {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- EXISTS(" << *(yystack_[1].value.stringVal) << ')';
#endif
    (yylhs.value.whereNode) = new ibis::qExists((yystack_[1].value.stringVal)->c_str());
    delete (yystack_[1].value.stringVal);
}
#line 840 "whereParser.cc"
    break;

  case 14: // simpleRange: "exists" '(' "string literal" ')'
#line 196 "whereParser.yy"
                          {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- EXISTS(" << *(yystack_[1].value.stringVal) << ')';
#endif
    (yylhs.value.whereNode) = new ibis::qExists((yystack_[1].value.stringVal)->c_str());
    delete (yystack_[1].value.stringVal);
}
#line 853 "whereParser.cc"
    break;

  case 15: // simpleRange: "name string" "in" "number sequence"
#line 204 "whereParser.yy"
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
#line 868 "whereParser.cc"
    break;

  case 16: // simpleRange: "name string" "in" '(' "floating-point number" ',' "floating-point number" ')'
#line 214 "whereParser.yy"
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
#line 885 "whereParser.cc"
    break;

  case 17: // simpleRange: "name string" "in" '(' "floating-point number" ')'
#line 226 "whereParser.yy"
                              {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[4].value.stringVal) << " IN ("
	<< (yystack_[1].value.doubleVal) << ")";
#endif
    (yylhs.value.whereNode) = new ibis::qContinuousRange((yystack_[4].value.stringVal)->c_str(), ibis::qExpr::OP_EQ, (yystack_[1].value.doubleVal));
    delete (yystack_[4].value.stringVal);
}
#line 899 "whereParser.cc"
    break;

  case 18: // simpleRange: "name string" "not" "null"
#line 235 "whereParser.yy"
                       {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.stringVal) << " NOT NULL";
#endif
    (yylhs.value.whereNode) = new ibis::qContinuousRange((yystack_[2].value.stringVal)->c_str(), ibis::qExpr::OP_UNDEFINED, 0U);
}
#line 911 "whereParser.cc"
    break;

  case 19: // simpleRange: "name string" "not" "in" "number sequence"
#line 242 "whereParser.yy"
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
#line 927 "whereParser.cc"
    break;

  case 20: // simpleRange: "name string" "not" "in" '(' "floating-point number" ',' "floating-point number" ')'
#line 253 "whereParser.yy"
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
#line 945 "whereParser.cc"
    break;

  case 21: // simpleRange: "name string" "not" "in" '(' "floating-point number" ')'
#line 266 "whereParser.yy"
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
#line 960 "whereParser.cc"
    break;

  case 22: // simpleRange: "name string" "in" "string sequence"
#line 276 "whereParser.yy"
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
#line 975 "whereParser.cc"
    break;

  case 23: // simpleRange: "name string" "in" '(' "name string" ',' "name string" ')'
#line 286 "whereParser.yy"
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
#line 997 "whereParser.cc"
    break;

  case 24: // simpleRange: "name string" "in" '(' "string literal" ',' "name string" ')'
#line 303 "whereParser.yy"
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
#line 1019 "whereParser.cc"
    break;

  case 25: // simpleRange: "name string" "in" '(' "name string" ',' "string literal" ')'
#line 320 "whereParser.yy"
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
#line 1041 "whereParser.cc"
    break;

  case 26: // simpleRange: "name string" "in" '(' "string literal" ',' "string literal" ')'
#line 337 "whereParser.yy"
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
#line 1063 "whereParser.cc"
    break;

  case 27: // simpleRange: "name string" "in" '(' "name string" ')'
#line 354 "whereParser.yy"
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
#line 1082 "whereParser.cc"
    break;

  case 28: // simpleRange: "name string" "in" '(' "string literal" ')'
#line 368 "whereParser.yy"
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
#line 1101 "whereParser.cc"
    break;

  case 29: // simpleRange: "name string" "like" "name string"
#line 382 "whereParser.yy"
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
#line 1116 "whereParser.cc"
    break;

  case 30: // simpleRange: "name string" "like" "string literal"
#line 392 "whereParser.yy"
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
#line 1131 "whereParser.cc"
    break;

  case 31: // simpleRange: "name string" "not" "in" "string sequence"
#line 402 "whereParser.yy"
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
#line 1147 "whereParser.cc"
    break;

  case 32: // simpleRange: "name string" "not" "in" '(' "name string" ',' "name string" ')'
#line 413 "whereParser.yy"
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
#line 1170 "whereParser.cc"
    break;

  case 33: // simpleRange: "name string" "not" "in" '(' "string literal" ',' "name string" ')'
#line 431 "whereParser.yy"
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
#line 1193 "whereParser.cc"
    break;

  case 34: // simpleRange: "name string" "not" "in" '(' "name string" ',' "string literal" ')'
#line 449 "whereParser.yy"
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
#line 1216 "whereParser.cc"
    break;

  case 35: // simpleRange: "name string" "not" "in" '(' "string literal" ',' "string literal" ')'
#line 467 "whereParser.yy"
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
#line 1239 "whereParser.cc"
    break;

  case 36: // simpleRange: "name string" "not" "in" '(' "name string" ')'
#line 485 "whereParser.yy"
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
#line 1259 "whereParser.cc"
    break;

  case 37: // simpleRange: "name string" "not" "in" '(' "string literal" ')'
#line 500 "whereParser.yy"
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
#line 1279 "whereParser.cc"
    break;

  case 38: // simpleRange: "name string" "in" "signed integer sequence"
#line 515 "whereParser.yy"
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
#line 1294 "whereParser.cc"
    break;

  case 39: // simpleRange: "name string" "not" "in" "signed integer sequence"
#line 525 "whereParser.yy"
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
#line 1310 "whereParser.cc"
    break;

  case 40: // simpleRange: "name string" "in" "unsigned integer sequence"
#line 536 "whereParser.yy"
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
#line 1325 "whereParser.cc"
    break;

  case 41: // simpleRange: "name string" "not" "in" "unsigned integer sequence"
#line 546 "whereParser.yy"
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
#line 1341 "whereParser.cc"
    break;

  case 42: // simpleRange: "name string" "contains" "name string"
#line 557 "whereParser.yy"
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
#line 1356 "whereParser.cc"
    break;

  case 43: // simpleRange: "name string" "contains" "string literal"
#line 567 "whereParser.yy"
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
#line 1371 "whereParser.cc"
    break;

  case 44: // simpleRange: "name string" "contains" '(' "name string" ')'
#line 577 "whereParser.yy"
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
#line 1386 "whereParser.cc"
    break;

  case 45: // simpleRange: "name string" "contains" '(' "string literal" ')'
#line 587 "whereParser.yy"
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
#line 1401 "whereParser.cc"
    break;

  case 46: // simpleRange: "name string" "contains" '(' "string literal" ',' "string literal" ')'
#line 597 "whereParser.yy"
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
#line 1417 "whereParser.cc"
    break;

  case 47: // simpleRange: "name string" "contains" '(' "string literal" ',' "name string" ')'
#line 608 "whereParser.yy"
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
#line 1433 "whereParser.cc"
    break;

  case 48: // simpleRange: "name string" "contains" '(' "name string" ',' "string literal" ')'
#line 619 "whereParser.yy"
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
#line 1449 "whereParser.cc"
    break;

  case 49: // simpleRange: "name string" "contains" '(' "name string" ',' "name string" ')'
#line 630 "whereParser.yy"
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
#line 1465 "whereParser.cc"
    break;

  case 50: // simpleRange: "name string" "contains" "string sequence"
#line 641 "whereParser.yy"
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
#line 1480 "whereParser.cc"
    break;

  case 51: // simpleRange: "any" '(' "name string" ')' "==" "floating-point number"
#line 651 "whereParser.yy"
                                    {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- ANY(" << *(yystack_[3].value.stringVal) << ") = "
	<< (yystack_[0].value.doubleVal) << ")";
#endif
    (yylhs.value.whereNode) = new ibis::qAnyAny((yystack_[3].value.stringVal)->c_str(), (yystack_[0].value.doubleVal));
    delete (yystack_[3].value.stringVal);
}
#line 1494 "whereParser.cc"
    break;

  case 52: // simpleRange: "any" '(' "name string" ')' "in" "number sequence"
#line 660 "whereParser.yy"
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
#line 1509 "whereParser.cc"
    break;

  case 53: // simpleRange: "name string" "==" "integer value"
#line 670 "whereParser.yy"
                     {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.stringVal) << " = " << *(yystack_[0].value.int64Val);
#endif
    (yylhs.value.whereNode) = new ibis::qIntHod((yystack_[2].value.stringVal)->c_str(), (yystack_[0].value.int64Val));
    delete (yystack_[2].value.stringVal);
}
#line 1522 "whereParser.cc"
    break;

  case 54: // simpleRange: "name string" "!=" "integer value"
#line 678 "whereParser.yy"
                      {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.stringVal) << " != " << *(yystack_[0].value.int64Val);
#endif
    (yylhs.value.whereNode) = new ibis::qExpr(ibis::qExpr::LOGICAL_NOT);
    (yylhs.value.whereNode)->setLeft(new ibis::qIntHod((yystack_[2].value.stringVal)->c_str(), (yystack_[0].value.int64Val)));
    delete (yystack_[2].value.stringVal);
}
#line 1536 "whereParser.cc"
    break;

  case 55: // simpleRange: "name string" "==" "unsigned integer value"
#line 687 "whereParser.yy"
                      {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.stringVal) << " = " << *(yystack_[0].value.uint64Val);
#endif
    (yylhs.value.whereNode) = new ibis::qUIntHod((yystack_[2].value.stringVal)->c_str(), (yystack_[0].value.uint64Val));
    delete (yystack_[2].value.stringVal);
}
#line 1549 "whereParser.cc"
    break;

  case 56: // simpleRange: "name string" "!=" "unsigned integer value"
#line 695 "whereParser.yy"
                       {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.stringVal) << " != " << *(yystack_[0].value.uint64Val);
#endif
    (yylhs.value.whereNode) = new ibis::qExpr(ibis::qExpr::LOGICAL_NOT);
    (yylhs.value.whereNode)->setLeft(new ibis::qUIntHod((yystack_[2].value.stringVal)->c_str(), (yystack_[0].value.uint64Val)));
    delete (yystack_[2].value.stringVal);
}
#line 1563 "whereParser.cc"
    break;

  case 57: // simpleRange: "string literal" "==" "name string"
#line 704 "whereParser.yy"
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
#line 1578 "whereParser.cc"
    break;

  case 58: // simpleRange: "string literal" "!=" "name string"
#line 714 "whereParser.yy"
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
#line 1594 "whereParser.cc"
    break;

  case 59: // simpleRange: "name string" "==" "string literal"
#line 725 "whereParser.yy"
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
#line 1609 "whereParser.cc"
    break;

  case 60: // simpleRange: "name string" "!=" "string literal"
#line 735 "whereParser.yy"
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
#line 1625 "whereParser.cc"
    break;

  case 61: // simpleRange: "name string" "==" mathExpr
#line 746 "whereParser.yy"
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
#line 1647 "whereParser.cc"
    break;

  case 62: // simpleRange: "name string" "!=" mathExpr
#line 763 "whereParser.yy"
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
#line 1672 "whereParser.cc"
    break;

  case 63: // compRange2: mathExpr "<" "integer value"
#line 786 "whereParser.yy"
                    {
    /* exact comparisons with 64-bit integers, see qIntHod::compare */
    (yylhs.value.whereNode) = ibis::qIntHod::compare(static_cast<ibis::math::term*>((yystack_[2].value.whereNode)),
                                ibis::qExpr::OP_LT, (yystack_[0].value.int64Val));
}
#line 1682 "whereParser.cc"
    break;

  case 64: // compRange2: "integer value" "<" mathExpr
#line 791 "whereParser.yy"
                      {
    (yylhs.value.whereNode) = ibis::qIntHod::compare(static_cast<ibis::math::term*>((yystack_[0].value.whereNode)),
                                ibis::qExpr::OP_GT, (yystack_[2].value.int64Val));
}
#line 1691 "whereParser.cc"
    break;

  case 65: // compRange2: mathExpr "<=" "integer value"
#line 795 "whereParser.yy"
                      {
    (yylhs.value.whereNode) = ibis::qIntHod::compare(static_cast<ibis::math::term*>((yystack_[2].value.whereNode)),
                                ibis::qExpr::OP_LE, (yystack_[0].value.int64Val));
}
#line 1700 "whereParser.cc"
    break;

  case 66: // compRange2: "integer value" "<=" mathExpr
#line 799 "whereParser.yy"
                      {
    (yylhs.value.whereNode) = ibis::qIntHod::compare(static_cast<ibis::math::term*>((yystack_[0].value.whereNode)),
                                ibis::qExpr::OP_GE, (yystack_[2].value.int64Val));
}
#line 1709 "whereParser.cc"
    break;

  case 67: // compRange2: mathExpr ">" "integer value"
#line 803 "whereParser.yy"
                      {
    (yylhs.value.whereNode) = ibis::qIntHod::compare(static_cast<ibis::math::term*>((yystack_[2].value.whereNode)),
                                ibis::qExpr::OP_GT, (yystack_[0].value.int64Val));
}
#line 1718 "whereParser.cc"
    break;

  case 68: // compRange2: "integer value" ">" mathExpr
#line 807 "whereParser.yy"
                      {
    (yylhs.value.whereNode) = ibis::qIntHod::compare(static_cast<ibis::math::term*>((yystack_[0].value.whereNode)),
                                ibis::qExpr::OP_LT, (yystack_[2].value.int64Val));
}
#line 1727 "whereParser.cc"
    break;

  case 69: // compRange2: mathExpr ">=" "integer value"
#line 811 "whereParser.yy"
                      {
    (yylhs.value.whereNode) = ibis::qIntHod::compare(static_cast<ibis::math::term*>((yystack_[2].value.whereNode)),
                                ibis::qExpr::OP_GE, (yystack_[0].value.int64Val));
}
#line 1736 "whereParser.cc"
    break;

  case 70: // compRange2: "integer value" ">=" mathExpr
#line 815 "whereParser.yy"
                      {
    (yylhs.value.whereNode) = ibis::qIntHod::compare(static_cast<ibis::math::term*>((yystack_[0].value.whereNode)),
                                ibis::qExpr::OP_LE, (yystack_[2].value.int64Val));
}
#line 1745 "whereParser.cc"
    break;

  case 71: // compRange2: mathExpr "between" "integer value" "and" "integer value"
#line 819 "whereParser.yy"
                                       {
    (yylhs.value.whereNode) = ibis::qIntHod::between(static_cast<ibis::math::term*>((yystack_[4].value.whereNode)), (yystack_[2].value.int64Val), (yystack_[0].value.int64Val));
}
#line 1753 "whereParser.cc"
    break;

  case 72: // compRange2: mathExpr "<" "unsigned integer value"
#line 822 "whereParser.yy"
                       {
    (yylhs.value.whereNode) = ibis::qUIntHod::compare(static_cast<ibis::math::term*>((yystack_[2].value.whereNode)),
                                ibis::qExpr::OP_LT, (yystack_[0].value.uint64Val));
}
#line 1762 "whereParser.cc"
    break;

  case 73: // compRange2: "unsigned integer value" "<" mathExpr
#line 826 "whereParser.yy"
                       {
    (yylhs.value.whereNode) = ibis::qUIntHod::compare(static_cast<ibis::math::term*>((yystack_[0].value.whereNode)),
                                ibis::qExpr::OP_GT, (yystack_[2].value.uint64Val));
}
#line 1771 "whereParser.cc"
    break;

  case 74: // compRange2: mathExpr "<=" "unsigned integer value"
#line 830 "whereParser.yy"
                       {
    (yylhs.value.whereNode) = ibis::qUIntHod::compare(static_cast<ibis::math::term*>((yystack_[2].value.whereNode)),
                                ibis::qExpr::OP_LE, (yystack_[0].value.uint64Val));
}
#line 1780 "whereParser.cc"
    break;

  case 75: // compRange2: "unsigned integer value" "<=" mathExpr
#line 834 "whereParser.yy"
                       {
    (yylhs.value.whereNode) = ibis::qUIntHod::compare(static_cast<ibis::math::term*>((yystack_[0].value.whereNode)),
                                ibis::qExpr::OP_GE, (yystack_[2].value.uint64Val));
}
#line 1789 "whereParser.cc"
    break;

  case 76: // compRange2: mathExpr ">" "unsigned integer value"
#line 838 "whereParser.yy"
                       {
    (yylhs.value.whereNode) = ibis::qUIntHod::compare(static_cast<ibis::math::term*>((yystack_[2].value.whereNode)),
                                ibis::qExpr::OP_GT, (yystack_[0].value.uint64Val));
}
#line 1798 "whereParser.cc"
    break;

  case 77: // compRange2: "unsigned integer value" ">" mathExpr
#line 842 "whereParser.yy"
                       {
    (yylhs.value.whereNode) = ibis::qUIntHod::compare(static_cast<ibis::math::term*>((yystack_[0].value.whereNode)),
                                ibis::qExpr::OP_LT, (yystack_[2].value.uint64Val));
}
#line 1807 "whereParser.cc"
    break;

  case 78: // compRange2: mathExpr ">=" "unsigned integer value"
#line 846 "whereParser.yy"
                       {
    (yylhs.value.whereNode) = ibis::qUIntHod::compare(static_cast<ibis::math::term*>((yystack_[2].value.whereNode)),
                                ibis::qExpr::OP_GE, (yystack_[0].value.uint64Val));
}
#line 1816 "whereParser.cc"
    break;

  case 79: // compRange2: "unsigned integer value" ">=" mathExpr
#line 850 "whereParser.yy"
                       {
    (yylhs.value.whereNode) = ibis::qUIntHod::compare(static_cast<ibis::math::term*>((yystack_[0].value.whereNode)),
                                ibis::qExpr::OP_LE, (yystack_[2].value.uint64Val));
}
#line 1825 "whereParser.cc"
    break;

  case 80: // compRange2: mathExpr "between" "unsigned integer value" "and" "unsigned integer value"
#line 854 "whereParser.yy"
                                         {
    (yylhs.value.whereNode) = ibis::qUIntHod::between(static_cast<ibis::math::term*>((yystack_[4].value.whereNode)), (yystack_[2].value.uint64Val), (yystack_[0].value.uint64Val));
}
#line 1833 "whereParser.cc"
    break;

  case 81: // compRange2: mathExpr "==" mathExpr
#line 857 "whereParser.yy"
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
#line 1848 "whereParser.cc"
    break;

  case 82: // compRange2: mathExpr "!=" mathExpr
#line 867 "whereParser.yy"
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
#line 1864 "whereParser.cc"
    break;

  case 83: // compRange2: mathExpr "<" mathExpr
#line 878 "whereParser.yy"
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
#line 1879 "whereParser.cc"
    break;

  case 84: // compRange2: mathExpr "<=" mathExpr
#line 888 "whereParser.yy"
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
#line 1894 "whereParser.cc"
    break;

  case 85: // compRange2: mathExpr ">" mathExpr
#line 898 "whereParser.yy"
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
#line 1909 "whereParser.cc"
    break;

  case 86: // compRange2: mathExpr ">=" mathExpr
#line 908 "whereParser.yy"
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
#line 1924 "whereParser.cc"
    break;

  case 87: // compRange3: mathExpr "<" mathExpr "<" mathExpr
#line 970 "whereParser.yy"
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
#line 1941 "whereParser.cc"
    break;

  case 88: // compRange3: mathExpr "<" mathExpr "<=" mathExpr
#line 982 "whereParser.yy"
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
#line 1958 "whereParser.cc"
    break;

  case 89: // compRange3: mathExpr "<=" mathExpr "<" mathExpr
#line 994 "whereParser.yy"
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
#line 1975 "whereParser.cc"
    break;

  case 90: // compRange3: mathExpr "<=" mathExpr "<=" mathExpr
#line 1006 "whereParser.yy"
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
#line 1992 "whereParser.cc"
    break;

  case 91: // compRange3: mathExpr ">" mathExpr ">" mathExpr
#line 1018 "whereParser.yy"
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
#line 2009 "whereParser.cc"
    break;

  case 92: // compRange3: mathExpr ">" mathExpr ">=" mathExpr
#line 1030 "whereParser.yy"
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
#line 2026 "whereParser.cc"
    break;

  case 93: // compRange3: mathExpr ">=" mathExpr ">" mathExpr
#line 1042 "whereParser.yy"
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
#line 2043 "whereParser.cc"
    break;

  case 94: // compRange3: mathExpr ">=" mathExpr ">=" mathExpr
#line 1054 "whereParser.yy"
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
#line 2060 "whereParser.cc"
    break;

  case 95: // compRange3: mathExpr "between" mathExpr "and" mathExpr
#line 1066 "whereParser.yy"
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
#line 2077 "whereParser.cc"
    break;

  case 96: // mathExpr: mathExpr "+" mathExpr
#line 1081 "whereParser.yy"
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
#line 2094 "whereParser.cc"
    break;

  case 97: // mathExpr: mathExpr "-" mathExpr
#line 1093 "whereParser.yy"
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
#line 2111 "whereParser.cc"
    break;

  case 98: // mathExpr: mathExpr "*" mathExpr
#line 1105 "whereParser.yy"
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
#line 2128 "whereParser.cc"
    break;

  case 99: // mathExpr: mathExpr "/" mathExpr
#line 1117 "whereParser.yy"
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
#line 2145 "whereParser.cc"
    break;

  case 100: // mathExpr: mathExpr "%" mathExpr
#line 1129 "whereParser.yy"
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
#line 2162 "whereParser.cc"
    break;

  case 101: // mathExpr: mathExpr "**" mathExpr
#line 1141 "whereParser.yy"
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
#line 2179 "whereParser.cc"
    break;

  case 102: // mathExpr: mathExpr "&" mathExpr
#line 1153 "whereParser.yy"
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
#line 2196 "whereParser.cc"
    break;

  case 103: // mathExpr: mathExpr "|" mathExpr
#line 1165 "whereParser.yy"
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
#line 2213 "whereParser.cc"
    break;

  case 104: // mathExpr: "name string" '(' mathExpr ')'
#line 1177 "whereParser.yy"
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
#line 2230 "whereParser.cc"
    break;

  case 105: // mathExpr: "name string" '(' mathExpr ',' mathExpr ')'
#line 1189 "whereParser.yy"
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
#line 2248 "whereParser.cc"
    break;

  case 106: // mathExpr: "FROM_UNIXTIME_LOCAL" '(' mathExpr ',' "string literal" ')'
#line 1202 "whereParser.yy"
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
#line 2266 "whereParser.cc"
    break;

  case 107: // mathExpr: "FROM_UNIXTIME_GMT" '(' mathExpr ',' "string literal" ')'
#line 1215 "whereParser.yy"
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
#line 2285 "whereParser.cc"
    break;

  case 108: // mathExpr: "ISO_TO_UNIXTIME_LOCAL" '(' mathExpr ')'
#line 1229 "whereParser.yy"
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
#line 2303 "whereParser.cc"
    break;

  case 109: // mathExpr: "ISO_TO_UNIXTIME_GMT" '(' mathExpr ')'
#line 1242 "whereParser.yy"
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
#line 2321 "whereParser.cc"
    break;

  case 110: // mathExpr: "TO_UNIXTIME_LOCAL" '(' "string literal" ',' "string literal" ')'
#line 1255 "whereParser.yy"
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
#line 2367 "whereParser.cc"
    break;

  case 111: // mathExpr: "TO_UNIXTIME_GMT" '(' "string literal" ',' "string literal" ')'
#line 1296 "whereParser.yy"
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
#line 2409 "whereParser.cc"
    break;

  case 112: // mathExpr: "-" mathExpr
#line 1333 "whereParser.yy"
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
#line 2424 "whereParser.cc"
    break;

  case 113: // mathExpr: "+" mathExpr
#line 1343 "whereParser.yy"
                             {
    (yylhs.value.whereNode) = (yystack_[0].value.whereNode);
}
#line 2432 "whereParser.cc"
    break;

  case 114: // mathExpr: '(' mathExpr ')'
#line 1346 "whereParser.yy"
                   {
    (yylhs.value.whereNode) = (yystack_[1].value.whereNode);
}
#line 2440 "whereParser.cc"
    break;

  case 115: // mathExpr: "name string"
#line 1349 "whereParser.yy"
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
#line 2455 "whereParser.cc"
    break;

  case 116: // mathExpr: "floating-point number"
#line 1359 "whereParser.yy"
         {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " got a number " << (yystack_[0].value.doubleVal);
#endif
    ibis::math::number *num = new ibis::math::number((yystack_[0].value.doubleVal));
    (yylhs.value.whereNode) = static_cast<ibis::qExpr*>(num);
}
#line 2468 "whereParser.cc"
    break;

  case 117: // START: qexpr "end of input"
#line 1369 "whereParser.yy"
                  { /* pass qexpr to the driver */
    driver.expr_ = (yystack_[1].value.whereNode);
}
#line 2476 "whereParser.cc"
    break;

  case 118: // START: qexpr ';'
#line 1372 "whereParser.yy"
            { /* pass qexpr to the driver */
    driver.expr_ = (yystack_[1].value.whereNode);
}
#line 2484 "whereParser.cc"
    break;


#line 2488 "whereParser.cc"

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
     421,   421,   182,   220,   -42,   117,    32,    69,     2,   -42,
     -42,   -42,   170,   130,   -42,   -42,   -42,   -23,   421,   421,
      89,    91,   421,   421,   143,   108,   421,   -42,   -42,   421,
     421,   421,   421,   421,   421,   421,   421,     3,   239,   265,
      68,    36,   -15,   421,   151,   155,    33,   110,   -42,    69,
      69,    69,    69,   -42,   291,   317,   343,   369,   421,   421,
     395,   421,   421,   421,   421,   421,   421,   421,   421,   -42,
     159,   177,   448,   469,   184,   226,   477,   497,   183,   505,
     206,   206,   206,   206,   206,   206,   206,   206,   -42,   116,
     -42,   -42,   -42,   206,   -42,   -42,   -42,   206,   -42,   -42,
     -42,    -3,   -42,   -42,   -42,   -42,    45,   -42,   -42,   440,
     -42,   -42,   -42,   -42,   -42,   -42,    37,    41,   -42,   -42,
     179,   -42,   -42,   216,   -42,   -42,   187,   -42,   -42,   224,
     206,   206,   267,   270,   118,   460,   423,   301,   301,   249,
     249,   249,   249,   -42,   -42,   248,   250,   253,   254,   -42,
     -42,    -6,   -42,   -42,   -42,   -42,    60,    55,    76,   113,
     126,   135,   -42,   421,   421,   421,   421,   421,   421,   421,
     421,   421,   257,   262,   421,   258,   260,   261,   263,   266,
     276,   195,   219,   225,   -42,    40,   -42,    44,   -42,   281,
     -42,    88,   -42,   153,   525,   206,   206,   206,   206,   206,
     206,   206,   206,   -42,   -42,   206,   -42,   -42,   -42,   -42,
     -42,   -42,   -42,   282,   -42,   227,   -42,   237,   277,   278,
     279,   283,   284,   297,   298,   299,   302,   -42,   303,   304,
     305,   309,   310,   -42,   -42,   -42,   -42,   -42,   -42,   -42,
     -42,   -42,   -42,   -42,   -42,   -42,   -42
  };

  const signed char
  whereParser::yydefact_[] =
  {
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,   116,   115,     0,     0,     0,     8,
       9,    10,     0,     0,     6,    11,    12,     0,     0,     0,
       0,     0,     0,     0,     0,   115,     0,   113,   112,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,   117,     0,
       0,     0,     0,   118,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     1,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
      66,    70,    64,    68,    75,    79,    73,    77,    18,     0,
      53,    55,    59,    61,    54,    56,    60,    62,    42,    50,
      43,     0,    38,    40,    15,    22,     0,    29,    30,     0,
      57,    58,     7,   114,     4,     5,     2,     3,    65,    74,
      84,    69,    78,    86,    63,    72,    83,    67,    76,    85,
      81,    82,     0,     0,     0,   103,   102,    96,    97,    98,
      99,   100,   101,    13,    14,     0,     0,     0,     0,   109,
     108,     0,    39,    41,    19,    31,     0,     0,     0,     0,
       0,     0,   104,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,    44,     0,    45,     0,    17,     0,
      27,     0,    28,     0,     0,    90,    89,    94,    93,    88,
      87,    92,    91,    71,    80,    95,   107,   106,   111,   110,
      51,    52,    21,     0,    36,     0,    37,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,   105,     0,     0,
       0,     0,     0,    49,    48,    47,    46,    16,    23,    25,
      24,    26,    20,    32,    34,    33,    35
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
      37,    38,    58,   189,    28,    25,    98,    57,    26,    29,
      27,    24,   190,    59,    60,    61,    62,    80,    82,    83,
      81,    99,    86,    87,    30,   117,    89,    56,   118,    90,
      91,    92,    93,    94,    95,    96,    97,   167,   103,   107,
     168,    54,    55,   119,    59,    60,    61,    62,    59,    60,
      63,    62,    59,    60,   130,   133,   136,   139,   140,   141,
     144,   145,   146,   147,   148,   149,   150,   151,   152,   124,
     125,   126,   127,     1,   112,   113,    31,   114,   115,   122,
     228,   116,   169,   229,   230,   170,     2,   231,   171,     3,
       4,     5,     6,     7,     8,     9,    32,   191,    10,    11,
     192,   194,   195,   193,    12,    13,    14,    33,   108,    15,
     109,   110,    16,   111,    17,    64,    65,    66,    67,    68,
      69,    47,   196,   197,    34,    70,    48,    49,   233,   184,
      79,   234,    84,    50,    85,    51,    52,    71,    72,    73,
      74,    75,    76,    77,    78,    71,    72,    73,    74,    75,
      76,    77,    78,    53,   162,   163,   123,   164,   165,   198,
     199,   166,    53,   204,   205,   206,   207,   208,   209,   210,
     211,   212,   200,   201,   215,    64,    65,    66,    67,    68,
      69,   202,   203,    88,   174,    70,   175,    39,    40,    41,
      42,   120,   178,   235,   179,   121,   236,    71,    72,    73,
      74,    75,    76,    77,    78,   153,    71,    72,    73,    74,
      75,    76,    77,    78,    71,    72,    73,    74,    75,    76,
      77,    78,   176,   154,   177,    43,    44,    45,    46,   161,
     180,   157,   181,    71,    72,    73,    74,    75,    76,    77,
      78,   222,   223,    71,    72,    73,    74,    75,    76,    77,
      78,    71,    72,    73,    74,    75,    76,    77,    78,     3,
       4,     5,     6,     7,     8,   224,   225,   239,    10,    11,
     240,   226,   227,   158,   100,   101,    14,   241,   182,    35,
     242,   183,   102,    78,    36,     3,     4,     5,     6,     7,
       8,   185,   213,   186,    10,    11,   187,   188,   214,     0,
     104,   105,    14,   220,   216,    35,   217,   218,   106,   219,
      36,     3,     4,     5,     6,     7,     8,   221,   232,   238,
      10,    11,     0,   243,   244,   245,   128,   129,    14,   246,
     247,    35,    75,    76,    77,    78,    36,     3,     4,     5,
       6,     7,     8,   248,   249,   250,    10,    11,   251,   252,
     253,   254,   131,   132,    14,   255,   256,    35,     0,     0,
       0,     0,    36,     3,     4,     5,     6,     7,     8,     0,
       0,     0,    10,    11,     0,     0,     0,     0,   134,   135,
      14,     0,     0,    35,     0,     0,     0,     0,    36,     3,
       4,     5,     6,     7,     8,     0,     0,     0,    10,    11,
       0,     0,     0,     0,   137,   138,    14,     0,     0,    35,
       0,     0,     0,     0,    36,     3,     4,     5,     6,     7,
       8,     0,     0,     0,    10,    11,     0,     0,     0,     0,
     142,   143,    14,     0,     0,    35,     0,     0,     0,     0,
      36,     3,     4,     5,     6,     7,     8,     0,     0,     0,
      10,    11,    73,    74,    75,    76,    77,    78,    14,     0,
       0,    35,     0,     0,     0,     0,    36,    71,    72,    73,
      74,    75,    76,    77,    78,    71,    72,    73,    74,    75,
      76,    77,    78,     0,     0,     0,   172,   173,    72,    73,
      74,    75,    76,    77,    78,   155,    71,    72,    73,    74,
      75,    76,    77,    78,    71,    72,    73,    74,    75,    76,
      77,    78,     0,     0,     0,     0,   156,     0,     0,     0,
       0,     0,     0,   159,    71,    72,    73,    74,    75,    76,
      77,    78,    71,    72,    73,    74,    75,    76,    77,    78,
       0,     0,     0,   160,     0,     0,     0,     0,     0,     0,
       0,   123,    71,    72,    73,    74,    75,    76,    77,    78,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,   237
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
      47,    45,    45,   173,   174,   175,   176,   177,   178,   179,
     180,   181,    46,    47,   184,     5,     6,     7,     8,     9,
      10,    46,    47,    40,     5,    15,     7,     5,     6,     7,
       8,    40,     5,    40,     7,    40,    43,    27,    28,    29,
      30,    31,    32,    33,    34,    46,    27,    28,    29,    30,
      31,    32,    33,    34,    27,    28,    29,    30,    31,    32,
      33,    34,     6,    46,     8,     5,     6,     7,     8,    46,
       6,    47,     8,    27,    28,    29,    30,    31,    32,    33,
      34,    46,    47,    27,    28,    29,    30,    31,    32,    33,
      34,    27,    28,    29,    30,    31,    32,    33,    34,    20,
      21,    22,    23,    24,    25,    46,    47,    40,    29,    30,
      43,    46,    47,    47,    35,    36,    37,    40,    11,    40,
      43,    11,    43,    34,    45,    20,    21,    22,    23,    24,
      25,    43,    35,    43,    29,    30,    43,    43,    36,    -1,
      35,    36,    37,    37,    46,    40,    46,    46,    43,    46,
      45,    20,    21,    22,    23,    24,    25,    41,    37,    37,
      29,    30,    -1,    46,    46,    46,    35,    36,    37,    46,
      46,    40,    31,    32,    33,    34,    45,    20,    21,    22,
      23,    24,    25,    46,    46,    46,    29,    30,    46,    46,
      46,    46,    35,    36,    37,    46,    46,    40,    -1,    -1,
      -1,    -1,    45,    20,    21,    22,    23,    24,    25,    -1,
      -1,    -1,    29,    30,    -1,    -1,    -1,    -1,    35,    36,
      37,    -1,    -1,    40,    -1,    -1,    -1,    -1,    45,    20,
      21,    22,    23,    24,    25,    -1,    -1,    -1,    29,    30,
      -1,    -1,    -1,    -1,    35,    36,    37,    -1,    -1,    40,
      -1,    -1,    -1,    -1,    45,    20,    21,    22,    23,    24,
      25,    -1,    -1,    -1,    29,    30,    -1,    -1,    -1,    -1,
      35,    36,    37,    -1,    -1,    40,    -1,    -1,    -1,    -1,
      45,    20,    21,    22,    23,    24,    25,    -1,    -1,    -1,
      29,    30,    29,    30,    31,    32,    33,    34,    37,    -1,
      -1,    40,    -1,    -1,    -1,    -1,    45,    27,    28,    29,
      30,    31,    32,    33,    34,    27,    28,    29,    30,    31,
      32,    33,    34,    -1,    -1,    -1,    46,    47,    28,    29,
      30,    31,    32,    33,    34,    47,    27,    28,    29,    30,
      31,    32,    33,    34,    27,    28,    29,    30,    31,    32,
      33,    34,    -1,    -1,    -1,    -1,    47,    -1,    -1,    -1,
      -1,    -1,    -1,    46,    27,    28,    29,    30,    31,    32,
      33,    34,    27,    28,    29,    30,    31,    32,    33,    34,
      -1,    -1,    -1,    46,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    46,    27,    28,    29,    30,    31,    32,    33,    34,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    46
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
      54,    35,    36,    54,    35,    36,    54,    35,    36,    54,
      54,    54,    35,    36,    54,    54,    54,    54,    54,    54,
      54,    54,    54,    46,    46,    47,    47,    47,    47,    46,
      46,    46,    38,    39,    41,    42,    45,    40,    43,    37,
      40,    43,    46,    47,     5,     7,     6,     8,     5,     7,
       6,     8,    11,    11,    11,    43,    43,    43,    43,     9,
      18,    37,    40,    43,    46,    47,    46,    47,    46,    47,
      46,    47,    46,    47,    54,    54,    54,    54,    54,    54,
      54,    54,    54,    35,    36,    54,    46,    46,    46,    46,
      37,    41,    46,    47,    46,    47,    46,    47,    40,    43,
      40,    43,    37,    40,    43,    40,    43,    46,    37,    40,
      43,    40,    43,    46,    46,    46,    46,    46,    46,    46,
      46,    46,    46,    46,    46,    46,    46
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
      52,    52,    52,    52,    52,    52,    52,    53,    53,    53,
      53,    53,    53,    53,    53,    53,    54,    54,    54,    54,
      54,    54,    54,    54,    54,    54,    54,    54,    54,    54,
      54,    54,    54,    54,    54,    54,    54,    55,    55
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
       5,     3,     3,     3,     3,     3,     3,     5,     5,     5,
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
       0,   115,   115,   125,   135,   145,   155,   163,   166,   167,
     168,   172,   180,   188,   196,   204,   214,   226,   235,   242,
     253,   266,   276,   286,   303,   320,   337,   354,   368,   382,
     392,   402,   413,   431,   449,   467,   485,   500,   515,   525,
     536,   546,   557,   567,   577,   587,   597,   608,   619,   630,
     641,   651,   660,   670,   678,   687,   695,   704,   714,   725,
     735,   746,   763,   786,   791,   795,   799,   803,   807,   811,
     815,   819,   822,   826,   830,   834,   838,   842,   846,   850,
     854,   857,   867,   878,   888,   898,   908,   970,   982,   994,
    1006,  1018,  1030,  1042,  1054,  1066,  1081,  1093,  1105,  1117,
    1129,  1141,  1153,  1165,  1177,  1189,  1202,  1215,  1229,  1242,
    1255,  1296,  1333,  1343,  1346,  1349,  1359,  1369,  1372
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
#line 3232 "whereParser.cc"

#line 1377 "whereParser.yy"

void ibis::whereParser::error(const ibis::whereParser::location_type& l,
			      const std::string& m) {
    LOGGER(ibis::gVerbose >= 0)
	<< "Warning -- ibis::whereParser encountered " << m
	<< " at location " << l;
}
