// A Bison parser, made by GNU Bison 3.7.6.

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
#line 6 "selectParser.yy"

/** \file Defines the parser for the select clause accepted by FastBit
    IBIS.  The definitions are processed through bison.
*/
#include <iostream>

#line 46 "selectParser.cc"




#include "selectParser.hh"

// Second part of user prologue.
#line 70 "selectParser.yy"

#include "selectLexer.h"

#undef yylex
#define yylex driver.lexer->lex

#line 61 "selectParser.cc"



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

#line 23 "selectParser.yy"
namespace ibis {
#line 155 "selectParser.cc"

  /// Build a parser object.
  selectParser::selectParser (class ibis::selectClause& driver_yyarg)
#if YYDEBUG
    : yydebug_ (false),
      yycdebug_ (&std::cerr),
#else
    :
#endif
      driver (driver_yyarg)
  {}

  selectParser::~selectParser ()
  {}

  selectParser::syntax_error::~syntax_error () YY_NOEXCEPT YY_NOTHROW
  {}

  /*---------------.
  | symbol kinds.  |
  `---------------*/

  // basic_symbol.
  template <typename Base>
  selectParser::basic_symbol<Base>::basic_symbol (const basic_symbol& that)
    : Base (that)
    , value (that.value)
    , location (that.location)
  {}


  /// Constructor for valueless symbols.
  template <typename Base>
  selectParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, YY_MOVE_REF (location_type) l)
    : Base (t)
    , value ()
    , location (l)
  {}

  template <typename Base>
  selectParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, YY_RVREF (semantic_type) v, YY_RVREF (location_type) l)
    : Base (t)
    , value (YY_MOVE (v))
    , location (YY_MOVE (l))
  {}

  template <typename Base>
  selectParser::symbol_kind_type
  selectParser::basic_symbol<Base>::type_get () const YY_NOEXCEPT
  {
    return this->kind ();
  }

  template <typename Base>
  bool
  selectParser::basic_symbol<Base>::empty () const YY_NOEXCEPT
  {
    return this->kind () == symbol_kind::S_YYEMPTY;
  }

  template <typename Base>
  void
  selectParser::basic_symbol<Base>::move (basic_symbol& s)
  {
    super_type::move (s);
    value = YY_MOVE (s.value);
    location = YY_MOVE (s.location);
  }

  // by_kind.
  selectParser::by_kind::by_kind ()
    : kind_ (symbol_kind::S_YYEMPTY)
  {}

#if 201103L <= YY_CPLUSPLUS
  selectParser::by_kind::by_kind (by_kind&& that)
    : kind_ (that.kind_)
  {
    that.clear ();
  }
#endif

  selectParser::by_kind::by_kind (const by_kind& that)
    : kind_ (that.kind_)
  {}

  selectParser::by_kind::by_kind (token_kind_type t)
    : kind_ (yytranslate_ (t))
  {}

  void
  selectParser::by_kind::clear () YY_NOEXCEPT
  {
    kind_ = symbol_kind::S_YYEMPTY;
  }

  void
  selectParser::by_kind::move (by_kind& that)
  {
    kind_ = that.kind_;
    that.clear ();
  }

  selectParser::symbol_kind_type
  selectParser::by_kind::kind () const YY_NOEXCEPT
  {
    return kind_;
  }

  selectParser::symbol_kind_type
  selectParser::by_kind::type_get () const YY_NOEXCEPT
  {
    return this->kind ();
  }


  // by_state.
  selectParser::by_state::by_state () YY_NOEXCEPT
    : state (empty_state)
  {}

  selectParser::by_state::by_state (const by_state& that) YY_NOEXCEPT
    : state (that.state)
  {}

  void
  selectParser::by_state::clear () YY_NOEXCEPT
  {
    state = empty_state;
  }

  void
  selectParser::by_state::move (by_state& that)
  {
    state = that.state;
    that.clear ();
  }

  selectParser::by_state::by_state (state_type s) YY_NOEXCEPT
    : state (s)
  {}

  selectParser::symbol_kind_type
  selectParser::by_state::kind () const YY_NOEXCEPT
  {
    if (state == empty_state)
      return symbol_kind::S_YYEMPTY;
    else
      return YY_CAST (symbol_kind_type, yystos_[+state]);
  }

  selectParser::stack_symbol_type::stack_symbol_type ()
  {}

  selectParser::stack_symbol_type::stack_symbol_type (YY_RVREF (stack_symbol_type) that)
    : super_type (YY_MOVE (that.state), YY_MOVE (that.value), YY_MOVE (that.location))
  {
#if 201103L <= YY_CPLUSPLUS
    // that is emptied.
    that.state = empty_state;
#endif
  }

  selectParser::stack_symbol_type::stack_symbol_type (state_type s, YY_MOVE_REF (symbol_type) that)
    : super_type (s, YY_MOVE (that.value), YY_MOVE (that.location))
  {
    // that is emptied.
    that.kind_ = symbol_kind::S_YYEMPTY;
  }

#if YY_CPLUSPLUS < 201103L
  selectParser::stack_symbol_type&
  selectParser::stack_symbol_type::operator= (const stack_symbol_type& that)
  {
    state = that.state;
    value = that.value;
    location = that.location;
    return *this;
  }

  selectParser::stack_symbol_type&
  selectParser::stack_symbol_type::operator= (stack_symbol_type& that)
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
  selectParser::yy_destroy_ (const char* yymsg, basic_symbol<Base>& yysym) const
  {
    if (yymsg)
      YY_SYMBOL_PRINT (yymsg, yysym);

    // User destructor.
    switch (yysym.kind ())
    {
      case symbol_kind::S_NOUNSTR: // "name"
#line 67 "selectParser.yy"
                    { delete (yysym.value.stringVal); }
#line 361 "selectParser.cc"
        break;

      case symbol_kind::S_STRLIT: // "string literal"
#line 66 "selectParser.yy"
                    { delete (yysym.value.stringVal); }
#line 367 "selectParser.cc"
        break;

      case symbol_kind::S_mathExpr: // mathExpr
#line 68 "selectParser.yy"
                    { delete (yysym.value.selectNode); }
#line 373 "selectParser.cc"
        break;

      default:
        break;
    }
  }

#if YYDEBUG
  template <typename Base>
  void
  selectParser::yy_print_ (std::ostream& yyo, const basic_symbol<Base>& yysym) const
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
  selectParser::yypush_ (const char* m, YY_MOVE_REF (stack_symbol_type) sym)
  {
    if (m)
      YY_SYMBOL_PRINT (m, sym);
    yystack_.push (YY_MOVE (sym));
  }

  void
  selectParser::yypush_ (const char* m, state_type s, YY_MOVE_REF (symbol_type) sym)
  {
#if 201103L <= YY_CPLUSPLUS
    yypush_ (m, stack_symbol_type (s, std::move (sym)));
#else
    stack_symbol_type ss (s, sym);
    yypush_ (m, ss);
#endif
  }

  void
  selectParser::yypop_ (int n)
  {
    yystack_.pop (n);
  }

#if YYDEBUG
  std::ostream&
  selectParser::debug_stream () const
  {
    return *yycdebug_;
  }

  void
  selectParser::set_debug_stream (std::ostream& o)
  {
    yycdebug_ = &o;
  }


  selectParser::debug_level_type
  selectParser::debug_level () const
  {
    return yydebug_;
  }

  void
  selectParser::set_debug_level (debug_level_type l)
  {
    yydebug_ = l;
  }
#endif // YYDEBUG

  selectParser::state_type
  selectParser::yy_lr_goto_state_ (state_type yystate, int yysym)
  {
    int yyr = yypgoto_[yysym - YYNTOKENS] + yystate;
    if (0 <= yyr && yyr <= yylast_ && yycheck_[yyr] == yystate)
      return yytable_[yyr];
    else
      return yydefgoto_[yysym - YYNTOKENS];
  }

  bool
  selectParser::yy_pact_value_is_default_ (int yyvalue)
  {
    return yyvalue == yypact_ninf_;
  }

  bool
  selectParser::yy_table_value_is_error_ (int yyvalue)
  {
    return yyvalue == yytable_ninf_;
  }

  int
  selectParser::operator() ()
  {
    return parse ();
  }

  int
  selectParser::parse ()
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
#line 28 "selectParser.yy"
{ // initialize location object
    yyla.location.begin.filename = yyla.location.end.filename = &(driver.clause_);
}

#line 515 "selectParser.cc"


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
  case 4: // sterm: mathExpr ','
#line 79 "selectParser.yy"
                    {
    driver.addTerm((yystack_[1].value.selectNode), 0);
}
#line 655 "selectParser.cc"
    break;

  case 5: // sterm: mathExpr "end of input"
#line 82 "selectParser.yy"
               {
    driver.addTerm((yystack_[1].value.selectNode), 0);
}
#line 663 "selectParser.cc"
    break;

  case 6: // sterm: mathExpr "name" ','
#line 85 "selectParser.yy"
                       {
    driver.addTerm((yystack_[2].value.selectNode), (yystack_[1].value.stringVal));
    delete (yystack_[1].value.stringVal);
}
#line 672 "selectParser.cc"
    break;

  case 7: // sterm: mathExpr "name" "end of input"
#line 89 "selectParser.yy"
                       {
    driver.addTerm((yystack_[2].value.selectNode), (yystack_[1].value.stringVal));
    delete (yystack_[1].value.stringVal);
}
#line 681 "selectParser.cc"
    break;

  case 8: // sterm: mathExpr "as" "name" ','
#line 93 "selectParser.yy"
                            {
    driver.addTerm((yystack_[3].value.selectNode), (yystack_[1].value.stringVal));
    delete (yystack_[1].value.stringVal);
}
#line 690 "selectParser.cc"
    break;

  case 9: // sterm: mathExpr "as" "name" "end of input"
#line 97 "selectParser.yy"
                            {
    driver.addTerm((yystack_[3].value.selectNode), (yystack_[1].value.stringVal));
    delete (yystack_[1].value.stringVal);
}
#line 699 "selectParser.cc"
    break;

  case 10: // mathExpr: mathExpr "+" mathExpr
#line 104 "selectParser.yy"
                        {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.selectNode)
	<< " + " << *(yystack_[0].value.selectNode);
#endif
    ibis::math::bediener *opr =
	new ibis::math::bediener(ibis::math::PLUS);
    opr->setRight((yystack_[0].value.selectNode));
    opr->setLeft((yystack_[2].value.selectNode));
    (yylhs.value.selectNode) = opr;
}
#line 716 "selectParser.cc"
    break;

  case 11: // mathExpr: mathExpr "-" mathExpr
#line 116 "selectParser.yy"
                            {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.selectNode)
	<< " - " << *(yystack_[0].value.selectNode);
#endif
    ibis::math::bediener *opr =
	new ibis::math::bediener(ibis::math::MINUS);
    opr->setRight((yystack_[0].value.selectNode));
    opr->setLeft((yystack_[2].value.selectNode));
    (yylhs.value.selectNode) = opr;
}
#line 733 "selectParser.cc"
    break;

  case 12: // mathExpr: mathExpr "*" mathExpr
#line 128 "selectParser.yy"
                           {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.selectNode)
	<< " * " << *(yystack_[0].value.selectNode);
#endif
    ibis::math::bediener *opr =
	new ibis::math::bediener(ibis::math::MULTIPLY);
    opr->setRight((yystack_[0].value.selectNode));
    opr->setLeft((yystack_[2].value.selectNode));
    (yylhs.value.selectNode) = opr;
}
#line 750 "selectParser.cc"
    break;

  case 13: // mathExpr: mathExpr "/" mathExpr
#line 140 "selectParser.yy"
                          {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.selectNode)
	<< " / " << *(yystack_[0].value.selectNode);
#endif
    ibis::math::bediener *opr =
	new ibis::math::bediener(ibis::math::DIVIDE);
    opr->setRight((yystack_[0].value.selectNode));
    opr->setLeft((yystack_[2].value.selectNode));
    (yylhs.value.selectNode) = opr;
}
#line 767 "selectParser.cc"
    break;

  case 14: // mathExpr: mathExpr "%" mathExpr
#line 152 "selectParser.yy"
                          {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.selectNode)
	<< " % " << *(yystack_[0].value.selectNode);
#endif
    ibis::math::bediener *opr =
	new ibis::math::bediener(ibis::math::REMAINDER);
    opr->setRight((yystack_[0].value.selectNode));
    opr->setLeft((yystack_[2].value.selectNode));
    (yylhs.value.selectNode) = opr;
}
#line 784 "selectParser.cc"
    break;

  case 15: // mathExpr: mathExpr "**" mathExpr
#line 164 "selectParser.yy"
                          {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.selectNode)
	<< " ^ " << *(yystack_[0].value.selectNode);
#endif
    ibis::math::bediener *opr =
	new ibis::math::bediener(ibis::math::POWER);
    opr->setRight((yystack_[0].value.selectNode));
    opr->setLeft((yystack_[2].value.selectNode));
    (yylhs.value.selectNode) = opr;
}
#line 801 "selectParser.cc"
    break;

  case 16: // mathExpr: mathExpr "&" mathExpr
#line 176 "selectParser.yy"
                             {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.selectNode)
	<< " & " << *(yystack_[0].value.selectNode);
#endif
    ibis::math::bediener *opr =
	new ibis::math::bediener(ibis::math::BITAND);
    opr->setRight((yystack_[0].value.selectNode));
    opr->setLeft((yystack_[2].value.selectNode));
    (yylhs.value.selectNode) = opr;
}
#line 818 "selectParser.cc"
    break;

  case 17: // mathExpr: mathExpr "|" mathExpr
#line 188 "selectParser.yy"
                            {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.selectNode)
	<< " | " << *(yystack_[0].value.selectNode);
#endif
    ibis::math::bediener *opr =
	new ibis::math::bediener(ibis::math::BITOR);
    opr->setRight((yystack_[0].value.selectNode));
    opr->setLeft((yystack_[2].value.selectNode));
    (yylhs.value.selectNode) = opr;
}
#line 835 "selectParser.cc"
    break;

  case 18: // mathExpr: "name" '(' "*" ')'
#line 200 "selectParser.yy"
                         {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[3].value.stringVal) << "(*)";
#endif
    ibis::math::term *fun = 0;
    if (stricmp((yystack_[3].value.stringVal)->c_str(), "count") == 0) { // aggregation count
	ibis::math::variable *var = new ibis::math::variable("*");
	fun = driver.addAgregado(ibis::selectClause::CNT, var);
    }
    else {
	LOGGER(ibis::gVerbose >= 0)
	    << "Warning -- only operator COUNT supports * as the argument, "
	    "but received " << *(yystack_[3].value.stringVal);
	throw "invalid use of (*)";
    }
    delete (yystack_[3].value.stringVal);
    (yylhs.value.selectNode) = fun;
}
#line 859 "selectParser.cc"
    break;

  case 19: // mathExpr: "name" '(' mathExpr ')'
#line 219 "selectParser.yy"
                           {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[3].value.stringVal) << "("
	<< *(yystack_[1].value.selectNode) << ")";
#endif
    ibis::math::term *fun = 0;
    if (stricmp((yystack_[3].value.stringVal)->c_str(), "count") == 0) { // aggregation count
	delete (yystack_[1].value.selectNode); // drop the expression, replace it with "*"
	ibis::math::variable *var = new ibis::math::variable("*");
	fun = driver.addAgregado(ibis::selectClause::CNT, var);
    }
    else if (stricmp((yystack_[3].value.stringVal)->c_str(), "max") == 0) { // aggregation max
	fun = driver.addAgregado(ibis::selectClause::MAX, (yystack_[1].value.selectNode));
    }
    else if (stricmp((yystack_[3].value.stringVal)->c_str(), "min") == 0) { // aggregation min
	fun = driver.addAgregado(ibis::selectClause::MIN, (yystack_[1].value.selectNode));
    }
    else if (stricmp((yystack_[3].value.stringVal)->c_str(), "sum") == 0) { // aggregation sum
	fun = driver.addAgregado(ibis::selectClause::SUM, (yystack_[1].value.selectNode));
    }
    else if (stricmp((yystack_[3].value.stringVal)->c_str(), "median") == 0) { // aggregation median
	fun = driver.addAgregado(ibis::selectClause::MEDIAN, (yystack_[1].value.selectNode));
    }
    else if (stricmp((yystack_[3].value.stringVal)->c_str(), "countd") == 0 ||
	     stricmp((yystack_[3].value.stringVal)->c_str(), "countdistinct") == 0) {
	// count distinct values
	fun = driver.addAgregado(ibis::selectClause::DISTINCT, (yystack_[1].value.selectNode));
    }
    else if (stricmp((yystack_[3].value.stringVal)->c_str(), "concat") == 0 ||
	     stricmp((yystack_[3].value.stringVal)->c_str(), "group_concat") == 0) {
	// concatenate all values as ASCII strings
	fun = driver.addAgregado(ibis::selectClause::CONCAT, (yystack_[1].value.selectNode));
    }
    else if (stricmp((yystack_[3].value.stringVal)->c_str(), "avg") == 0) { // aggregation avg
	ibis::math::term *numer =
	    driver.addAgregado(ibis::selectClause::SUM, (yystack_[1].value.selectNode));
	ibis::math::variable *var = new ibis::math::variable("*");
	ibis::math::term *denom =
	    driver.addAgregado(ibis::selectClause::CNT, var);
	ibis::math::bediener *opr =
	    new ibis::math::bediener(ibis::math::DIVIDE);
	opr->setRight(denom);
	opr->setLeft(numer);
	fun = opr;
    }
    else if (stricmp((yystack_[3].value.stringVal)->c_str(), "varp") == 0 ||
	     stricmp((yystack_[3].value.stringVal)->c_str(), "varpop") == 0) {
	// population variance is computed as
	// fabs(sum (x^2) / count(*) - (sum (x) / count(*))^2)
	ibis::math::term *x = (yystack_[1].value.selectNode);
	ibis::math::number *two = new ibis::math::number(2.0);
	ibis::math::variable *star = new ibis::math::variable("*");
	ibis::math::term *t11 = new ibis::math::bediener(ibis::math::POWER);
	t11->setLeft(x);
	t11->setRight(two);
	t11 = driver.addAgregado(ibis::selectClause::SUM, t11);
	ibis::math::term *t12 =
	    driver.addAgregado(ibis::selectClause::CNT, star);
	ibis::math::term *t13 = new ibis::math::bediener(ibis::math::DIVIDE);
	t13->setLeft(t11);
	t13->setRight(t12);
	ibis::math::term *t21 =
	    driver.addAgregado(ibis::selectClause::SUM, x->dup());
	ibis::math::term *t23 = new ibis::math::bediener(ibis::math::DIVIDE);
	t23->setLeft(t21);
	t23->setRight(t12->dup());
	ibis::math::term *t24 = new ibis::math::bediener(ibis::math::POWER);
	t24->setLeft(t23);
	t24->setRight(two->dup());
        ibis::math::term *t0 = new ibis::math::bediener(ibis::math::MINUS);
	t0->setLeft(t13);
	t0->setRight(t24);
        fun = new ibis::math::stdFunction1("fabs");
        fun->setLeft(t0);
	//fun = driver.addAgregado(ibis::selectClause::VARPOP, $3);
    }
    else if (stricmp((yystack_[3].value.stringVal)->c_str(), "var") == 0 ||
	     stricmp((yystack_[3].value.stringVal)->c_str(), "varsamp") == 0 ||
	     stricmp((yystack_[3].value.stringVal)->c_str(), "variance") == 0) {
	// sample variance is computed as
	// fabs((sum (x^2) / count(*) - (sum (x) / count(*))^2) * (count(*) / (count(*)-1)))
	ibis::math::term *x = (yystack_[1].value.selectNode);
	ibis::math::number *two = new ibis::math::number(2.0);
	ibis::math::variable *star = new ibis::math::variable("*");
	ibis::math::term *t11 = new ibis::math::bediener(ibis::math::POWER);
	t11->setLeft(x);
	t11->setRight(two);
	t11 = driver.addAgregado(ibis::selectClause::SUM, t11);
	ibis::math::term *t12 =
	    driver.addAgregado(ibis::selectClause::CNT, star);
	ibis::math::term *t13 = new ibis::math::bediener(ibis::math::DIVIDE);
	t13->setLeft(t11);
	t13->setRight(t12);
	ibis::math::term *t21 =
	    driver.addAgregado(ibis::selectClause::SUM, x->dup());
	ibis::math::term *t23 = new ibis::math::bediener(ibis::math::DIVIDE);
	t23->setLeft(t21);
	t23->setRight(t12->dup());
	ibis::math::term *t24 = new ibis::math::bediener(ibis::math::POWER);
	t24->setLeft(t23);
	t24->setRight(two->dup());
	ibis::math::term *t31 = new ibis::math::bediener(ibis::math::MINUS);
	t31->setLeft(t13);
	t31->setRight(t24);
	ibis::math::term *t32 = new ibis::math::bediener(ibis::math::MINUS);
	ibis::math::number *one = new ibis::math::number(1.0);
	t32->setLeft(t12->dup());
	t32->setRight(one);
	ibis::math::term *t33 = new ibis::math::bediener(ibis::math::DIVIDE);
	t33->setLeft(t12->dup());
	t33->setRight(t32);
        ibis::math::term *t0 = new ibis::math::bediener(ibis::math::MULTIPLY);
	t0->setLeft(t31);
	t0->setRight(t33);
        fun = new ibis::math::stdFunction1("fabs");
        fun->setLeft(t0);
	//fun = driver.addAgregado(ibis::selectClause::VARSAMP, $3);
    }
    else if (stricmp((yystack_[3].value.stringVal)->c_str(), "stdevp") == 0 ||
	     stricmp((yystack_[3].value.stringVal)->c_str(), "stdpop") == 0) {
	// population standard deviation is computed as
	// sqrt(fabs(sum (x^2) / count(*) - (sum (x) / count(*))^2))
	ibis::math::term *x = (yystack_[1].value.selectNode);
	ibis::math::number *two = new ibis::math::number(2.0);
	ibis::math::variable *star = new ibis::math::variable("*");
	ibis::math::term *t11 = new ibis::math::bediener(ibis::math::POWER);
	t11->setLeft(x);
	t11->setRight(two);
	t11 = driver.addAgregado(ibis::selectClause::SUM, t11);
	ibis::math::term *t12 =
	    driver.addAgregado(ibis::selectClause::CNT, star);
	ibis::math::term *t13 = new ibis::math::bediener(ibis::math::DIVIDE);
	t13->setLeft(t11);
	t13->setRight(t12);
	ibis::math::term *t21 =
	    driver.addAgregado(ibis::selectClause::SUM, x->dup());
	ibis::math::term *t23 = new ibis::math::bediener(ibis::math::DIVIDE);
	t23->setLeft(t21);
	t23->setRight(t12->dup());
	ibis::math::term *t24 = new ibis::math::bediener(ibis::math::POWER);
	t24->setLeft(t23);
	t24->setRight(two->dup());
	ibis::math::term *t31 = new ibis::math::bediener(ibis::math::MINUS);
	t31->setLeft(t13);
	t31->setRight(t24);
        ibis::math::term *t0 = new ibis::math::stdFunction1("fabs");
        t0->setLeft(t31);
	fun = new ibis::math::stdFunction1("sqrt");
	fun->setLeft(t0);
	//fun = driver.addAgregado(ibis::selectClause::STDPOP, $3);
    }
    else if (stricmp((yystack_[3].value.stringVal)->c_str(), "std") == 0 ||
	     stricmp((yystack_[3].value.stringVal)->c_str(), "stdev") == 0 ||
	     stricmp((yystack_[3].value.stringVal)->c_str(), "stddev") == 0 ||
	     stricmp((yystack_[3].value.stringVal)->c_str(), "stdsamp") == 0) {
	// sample standard deviation is computed as
	// sqrt(fabs(sum (x^2) / count(*) - (sum (x) / count(*))^2) * (count(*) / (count(*)-1))))
	ibis::math::term *x = (yystack_[1].value.selectNode);
	ibis::math::number *two = new ibis::math::number(2.0);
	ibis::math::variable *star = new ibis::math::variable("*");
	ibis::math::term *t11 = new ibis::math::bediener(ibis::math::POWER);
	t11->setLeft(x);
	t11->setRight(two);
	t11 = driver.addAgregado(ibis::selectClause::SUM, t11);
	ibis::math::term *t12 =
	    driver.addAgregado(ibis::selectClause::CNT, star);
	ibis::math::term *t13 = new ibis::math::bediener(ibis::math::DIVIDE);
	t13->setLeft(t11);
	t13->setRight(t12);
	ibis::math::term *t21 =
	    driver.addAgregado(ibis::selectClause::SUM, x->dup());
	ibis::math::term *t23 = new ibis::math::bediener(ibis::math::DIVIDE);
	t23->setLeft(t21);
	t23->setRight(t12->dup());
	ibis::math::term *t24 = new ibis::math::bediener(ibis::math::POWER);
	t24->setLeft(t23);
	t24->setRight(two->dup());
	ibis::math::term *t31 = new ibis::math::bediener(ibis::math::MINUS);
	t31->setLeft(t13);
	t31->setRight(t24);
	ibis::math::term *t32 = new ibis::math::bediener(ibis::math::MINUS);
	ibis::math::number *one = new ibis::math::number(1.0);
	t32->setLeft(t12->dup());
	t32->setRight(one);
	ibis::math::term *t33 = new ibis::math::bediener(ibis::math::DIVIDE);
	t33->setLeft(t12->dup());
	t33->setRight(t32);
	ibis::math::term *t34 = new ibis::math::bediener(ibis::math::MULTIPLY);
	t34->setLeft(t31);
	t34->setRight(t33);
        ibis::math::term *t0 = new ibis::math::stdFunction1("fabs");
        t0->setLeft(t34);
	fun = new ibis::math::stdFunction1("sqrt");
	fun->setLeft(t0);
	// fun = driver.addAgregado(ibis::selectClause::STDSAMP, $3);
    }
    else { // assume it is a standard math function
	fun = new ibis::math::stdFunction1((yystack_[3].value.stringVal)->c_str());
	fun->setLeft((yystack_[1].value.selectNode));
    }
    delete (yystack_[3].value.stringVal);
    (yylhs.value.selectNode) = fun;
}
#line 1068 "selectParser.cc"
    break;

  case 20: // mathExpr: "FORMAT_UNIXTIME_GMT" '(' mathExpr ',' "name" ')'
#line 423 "selectParser.yy"
                                                   {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- FORMAT_UNIXTIME_GMT("
	<< *(yystack_[3].value.selectNode) << ", " << *(yystack_[1].value.stringVal) << ")";
#endif
    ibis::math::formatUnixTime fut((yystack_[1].value.stringVal)->c_str(), "GMT");
    ibis::math::stringFunction1 *fun = new ibis::math::stringFunction1(fut);
    fun->setLeft((yystack_[3].value.selectNode));
    (yylhs.value.selectNode) = fun;
    delete (yystack_[1].value.stringVal);
}
#line 1085 "selectParser.cc"
    break;

  case 21: // mathExpr: "FORMAT_UNIXTIME_GMT" '(' mathExpr ',' "string literal" ')'
#line 435 "selectParser.yy"
                                                  {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- FORMAT_UNIXTIME_GMT("
	<< *(yystack_[3].value.selectNode) << ", " << *(yystack_[1].value.stringVal) << ")";
#endif
    ibis::math::formatUnixTime fut((yystack_[1].value.stringVal)->c_str(), "GMT");
    ibis::math::stringFunction1 *fun = new ibis::math::stringFunction1(fut);
    fun->setLeft((yystack_[3].value.selectNode));
    (yylhs.value.selectNode) = fun;
    delete (yystack_[1].value.stringVal);
}
#line 1102 "selectParser.cc"
    break;

  case 22: // mathExpr: "FORMAT_UNIXTIME_LOCAL" '(' mathExpr ',' "name" ')'
#line 447 "selectParser.yy"
                                                     {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- FORMAT_UNIXTIME_LOCAL("
	<< *(yystack_[3].value.selectNode) << ", " << *(yystack_[1].value.stringVal) << ")";
#endif
    ibis::math::formatUnixTime fut((yystack_[1].value.stringVal)->c_str());
    ibis::math::stringFunction1 *fun = new ibis::math::stringFunction1(fut);
    fun->setLeft((yystack_[3].value.selectNode));
    (yylhs.value.selectNode) = fun;
    delete (yystack_[1].value.stringVal);
}
#line 1119 "selectParser.cc"
    break;

  case 23: // mathExpr: "FORMAT_UNIXTIME_LOCAL" '(' mathExpr ',' "string literal" ')'
#line 459 "selectParser.yy"
                                                    {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- FORMAT_UNIXTIME_LOCAL("
	<< *(yystack_[3].value.selectNode) << ", " << *(yystack_[1].value.stringVal) << ")";
#endif
    ibis::math::formatUnixTime fut((yystack_[1].value.stringVal)->c_str());
    ibis::math::stringFunction1 *fun = new ibis::math::stringFunction1(fut);
    fun->setLeft((yystack_[3].value.selectNode));
    (yylhs.value.selectNode) = fun;
    delete (yystack_[1].value.stringVal);
}
#line 1136 "selectParser.cc"
    break;

  case 24: // mathExpr: "name" '(' mathExpr ',' mathExpr ')'
#line 471 "selectParser.yy"
                                        {
    /* two-arugment math functions */
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[5].value.stringVal) << "("
	<< *(yystack_[3].value.selectNode) << ", " << *(yystack_[1].value.selectNode) << ")";
#endif
    ibis::math::stdFunction2 *fun =
	new ibis::math::stdFunction2((yystack_[5].value.stringVal)->c_str());
    fun->setRight((yystack_[1].value.selectNode));
    fun->setLeft((yystack_[3].value.selectNode));
    (yylhs.value.selectNode) = fun;
    delete (yystack_[5].value.stringVal);
}
#line 1155 "selectParser.cc"
    break;

  case 25: // mathExpr: "-" mathExpr
#line 485 "selectParser.yy"
                               {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- - " << *(yystack_[0].value.selectNode);
#endif
    ibis::math::bediener *opr =
	new ibis::math::bediener(ibis::math::NEGATE);
    opr->setRight((yystack_[0].value.selectNode));
    (yylhs.value.selectNode) = opr;
}
#line 1170 "selectParser.cc"
    break;

  case 26: // mathExpr: "+" mathExpr
#line 495 "selectParser.yy"
                             {
    (yylhs.value.selectNode) = (yystack_[0].value.selectNode);
}
#line 1178 "selectParser.cc"
    break;

  case 27: // mathExpr: '(' mathExpr ')'
#line 498 "selectParser.yy"
                   {
    (yylhs.value.selectNode) = (yystack_[1].value.selectNode);
}
#line 1186 "selectParser.cc"
    break;

  case 28: // mathExpr: "name"
#line 501 "selectParser.yy"
          {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " got a variable name " << *(yystack_[0].value.stringVal);
#endif
    (yylhs.value.selectNode) = new ibis::math::variable((yystack_[0].value.stringVal)->c_str());
    delete (yystack_[0].value.stringVal);
}
#line 1199 "selectParser.cc"
    break;

  case 29: // mathExpr: "string literal"
#line 509 "selectParser.yy"
         {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " got a string literal " << *(yystack_[0].value.stringVal);
#endif
    (yylhs.value.selectNode) = new ibis::math::literal((yystack_[0].value.stringVal)->c_str());
    delete (yystack_[0].value.stringVal);
}
#line 1212 "selectParser.cc"
    break;

  case 30: // mathExpr: "numerical value"
#line 517 "selectParser.yy"
         {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " got a number " << (yystack_[0].value.doubleVal);
#endif
    (yylhs.value.selectNode) = new ibis::math::number((yystack_[0].value.doubleVal));
}
#line 1224 "selectParser.cc"
    break;


#line 1228 "selectParser.cc"

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
  selectParser::error (const syntax_error& yyexc)
  {
    error (yyexc.location, yyexc.what ());
  }

  /* Return YYSTR after stripping away unnecessary quotes and
     backslashes, so that it's suitable for yyerror.  The heuristic is
     that double-quoting is unnecessary unless the string contains an
     apostrophe, a comma, or backslash (other than backslash-backslash).
     YYSTR is taken from yytname.  */
  std::string
  selectParser::yytnamerr_ (const char *yystr)
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
  selectParser::symbol_name (symbol_kind_type yysymbol)
  {
    return yytnamerr_ (yytname_[yysymbol]);
  }



  // selectParser::context.
  selectParser::context::context (const selectParser& yyparser, const symbol_type& yyla)
    : yyparser_ (yyparser)
    , yyla_ (yyla)
  {}

  int
  selectParser::context::expected_tokens (symbol_kind_type yyarg[], int yyargn) const
  {
    // Actual number of expected tokens
    int yycount = 0;

    int yyn = yypact_[+yyparser_.yystack_[0].state];
    if (!yy_pact_value_is_default_ (yyn))
      {
        /* Start YYX at -YYN if negative to avoid negative indexes in
           YYCHECK.  In other words, skip the first -YYN actions for
           this state because they are default actions.  */
        int yyxbegin = yyn < 0 ? -yyn : 0;
        // Stay within bounds of both yycheck and yytname.
        int yychecklim = yylast_ - yyn + 1;
        int yyxend = yychecklim < YYNTOKENS ? yychecklim : YYNTOKENS;
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
  selectParser::yy_syntax_error_arguments_ (const context& yyctx,
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
  selectParser::yysyntax_error_ (const context& yyctx) const
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


  const signed char selectParser::yypact_ninf_ = -13;

  const signed char selectParser::yytable_ninf_ = -1;

  const signed char
  selectParser::yypact_[] =
  {
     126,   126,   126,   -13,   -12,   -13,    -6,    -2,   126,    30,
     126,    29,    20,    20,   113,   126,   126,    61,   -13,   -13,
     -13,    28,   126,   126,   126,   126,   126,   126,   126,   126,
       2,   -13,    24,    45,    93,   107,   -13,     3,    68,    83,
       0,     0,    20,    20,    20,    20,   -13,   -13,   -13,   126,
     -13,    -9,     4,   -13,   -13,    77,    25,    26,    38,    39,
     -13,   -13,   -13,   -13,   -13
  };

  const signed char
  selectParser::yydefact_[] =
  {
       0,     0,     0,    30,    28,    29,     0,     0,     0,     0,
       2,     0,    26,    25,     0,     0,     0,     0,     1,     3,
       5,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     4,     0,     0,     0,     0,    27,     0,    17,    16,
      10,    11,    12,    13,    14,    15,     7,     6,    18,     0,
      19,     0,     0,     9,     8,     0,     0,     0,     0,     0,
      24,    20,    21,    22,    23
  };

  const signed char
  selectParser::yypgoto_[] =
  {
     -13,    37,   -13,    -1
  };

  const signed char
  selectParser::yydefgoto_[] =
  {
       0,     9,    10,    11
  };

  const signed char
  selectParser::yytable_[] =
  {
      12,    13,    46,    53,    56,    57,    14,    17,    26,    27,
      28,    29,    15,    33,    34,    35,    16,    58,    59,    47,
      54,    38,    39,    40,    41,    42,    43,    44,    45,    20,
      18,    29,    21,    22,    23,    24,    25,    26,    27,    28,
      29,    37,    30,    48,    61,    62,    31,    19,    55,    22,
      23,    24,    25,    26,    27,    28,    29,    63,    64,     0,
       0,     0,    49,     0,    50,    22,    23,    24,    25,    26,
      27,    28,    29,    23,    24,    25,    26,    27,    28,    29,
      36,    22,    23,    24,    25,    26,    27,    28,    29,    24,
      25,    26,    27,    28,    29,     0,    60,    22,    23,    24,
      25,    26,    27,    28,    29,     0,     0,     0,     0,     0,
      51,    22,    23,    24,    25,    26,    27,    28,    29,     1,
       2,    32,     0,     0,    52,     3,     4,     5,     6,     7,
       0,     8,     1,     2,     0,     0,     0,     0,     3,     4,
       5,     6,     7,     0,     8
  };

  const signed char
  selectParser::yycheck_[] =
  {
       1,     2,     0,     0,    13,    14,    18,     8,     8,     9,
      10,    11,    18,    14,    15,    16,    18,    13,    14,    17,
      17,    22,    23,    24,    25,    26,    27,    28,    29,     0,
       0,    11,     3,     4,     5,     6,     7,     8,     9,    10,
      11,    13,    13,    19,    19,    19,    17,    10,    49,     4,
       5,     6,     7,     8,     9,    10,    11,    19,    19,    -1,
      -1,    -1,    17,    -1,    19,     4,     5,     6,     7,     8,
       9,    10,    11,     5,     6,     7,     8,     9,    10,    11,
      19,     4,     5,     6,     7,     8,     9,    10,    11,     6,
       7,     8,     9,    10,    11,    -1,    19,     4,     5,     6,
       7,     8,     9,    10,    11,    -1,    -1,    -1,    -1,    -1,
      17,     4,     5,     6,     7,     8,     9,    10,    11,     6,
       7,     8,    -1,    -1,    17,    12,    13,    14,    15,    16,
      -1,    18,     6,     7,    -1,    -1,    -1,    -1,    12,    13,
      14,    15,    16,    -1,    18
  };

  const signed char
  selectParser::yystos_[] =
  {
       0,     6,     7,    12,    13,    14,    15,    16,    18,    21,
      22,    23,    23,    23,    18,    18,    18,    23,     0,    21,
       0,     3,     4,     5,     6,     7,     8,     9,    10,    11,
      13,    17,     8,    23,    23,    23,    19,    13,    23,    23,
      23,    23,    23,    23,    23,    23,     0,    17,    19,    17,
      19,    17,    17,     0,    17,    23,    13,    14,    13,    14,
      19,    19,    19,    19,    19
  };

  const signed char
  selectParser::yyr1_[] =
  {
       0,    20,    21,    21,    22,    22,    22,    22,    22,    22,
      23,    23,    23,    23,    23,    23,    23,    23,    23,    23,
      23,    23,    23,    23,    23,    23,    23,    23,    23,    23,
      23
  };

  const signed char
  selectParser::yyr2_[] =
  {
       0,     2,     1,     2,     2,     2,     3,     3,     4,     4,
       3,     3,     3,     3,     3,     3,     3,     3,     4,     4,
       6,     6,     6,     6,     6,     2,     2,     3,     1,     1,
       1
  };


#if YYDEBUG || 1
  // YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
  // First, the terminals, then, starting at \a YYNTOKENS, nonterminals.
  const char*
  const selectParser::yytname_[] =
  {
  "\"end of input\"", "error", "\"invalid token\"", "\"as\"", "\"|\"",
  "\"&\"", "\"+\"", "\"-\"", "\"*\"", "\"/\"", "\"%\"", "\"**\"",
  "\"numerical value\"", "\"name\"", "\"string literal\"",
  "\"FORMAT_UNIXTIME_GMT\"", "\"FORMAT_UNIXTIME_LOCAL\"", "','", "'('",
  "')'", "$accept", "slist", "sterm", "mathExpr", YY_NULLPTR
  };
#endif


#if YYDEBUG
  const short
  selectParser::yyrline_[] =
  {
       0,    78,    78,    78,    79,    82,    85,    89,    93,    97,
     104,   116,   128,   140,   152,   164,   176,   188,   200,   219,
     423,   435,   447,   459,   471,   485,   495,   498,   501,   509,
     517
  };

  void
  selectParser::yy_stack_print_ () const
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
  selectParser::yy_reduce_print_ (int yyrule) const
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

  selectParser::symbol_kind_type
  selectParser::yytranslate_ (int t)
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
      18,    19,     2,     2,    17,     2,     2,     2,     2,     2,
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
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     1,     2,     3,     4,
       5,     6,     7,     8,     9,    10,    11,    12,    13,    14,
      15,    16
    };
    // Last valid token kind.
    const int code_max = 271;

    if (t <= 0)
      return symbol_kind::S_YYEOF;
    else if (t <= code_max)
      return YY_CAST (symbol_kind_type, translate_table[t]);
    else
      return symbol_kind::S_YYUNDEF;
  }

#line 23 "selectParser.yy"
} // ibis
#line 1791 "selectParser.cc"

#line 526 "selectParser.yy"

void ibis::selectParser::error(const ibis::selectParser::location_type& l,
			       const std::string& m) {
    LOGGER(ibis::gVerbose >= 0)
	<< "Warning -- ibis::selectParser encountered " << m << " at location "
	<< l;
}
